// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file PianoTilesApp.cpp
 * @brief Piano Tiles as a CrossPad app: tiles fall down four lanes on the
 *        panel, the 4x4 pads show the bottom of the field (top row = tiles
 *        about to arrive, bottom row = play now), a pad of the right column
 *        plays the melody's next note on the app's own synth, and the pause
 *        menu's Quit goes back to the other firmware slot.
 *
 * Meant as the one app of a firmware of its own (the "pianotiles" kit). The
 * game steps on an lv_timer on the LVGL thread, where the encoder, the power
 * click and the menus live too. Pads arrive on another thread with the time
 * they were pressed; the timer replays them in order, stepping the song clock
 * to each one, so a press is judged by when it happened, not by the tick.
 */

#if USE_LVGL

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#if defined(ESP_PLATFORM)
  #include <freertos/FreeRTOS.h>
  #include <freertos/semphr.h>
  #include <freertos/task.h>
#else
  #include "FreeRTOS.h"
  #include "semphr.h"
  #include "task.h"
#endif

#include <crosspad/app/AppRegistrar.hpp>
#include <crosspad/audio/IAudioModule.hpp>
#include <crosspad/pad/IPadLogicHandler.hpp>
#include <crosspad/pad/PadManager.hpp>
#include <crosspad/platform/IClock.hpp>
#include <crosspad/platform/IFileSystem.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include "crosspad-gui/components/app_icon.h"
#include "crosspad-gui/components/power_gesture.h"
#include "crosspad-gui/fw/IFwViewService.h"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "crosspad-gui/widgets/gui_helpers.h"

#include "lvgl.h"

#include "PianoTilesGame.hpp"
#include "PianoTilesSynth.hpp"

class App;

namespace {

using pianotiles::Fail;
using pianotiles::Game;
using pianotiles::PadRole;
using pianotiles::Phase;

constexpr const char* kPadLogicName = "PianoTiles";
constexpr const char* kDataDir = "/crosspad/pianotiles";
constexpr const char* kProgressFile = "progress.txt";
constexpr uint32_t kTickMs = 16;
constexpr int32_t kPxPerBeat = 64;       // the field's scale: a beat of the song is this tall
constexpr uint32_t kResultMs = 2200;     // the result stays up this long before its menu
constexpr uint32_t kCountdownMs = 1500;  // resume: 3, 2, 1
constexpr uint32_t kBlinkMs = 250;
constexpr int kPool = 24;
constexpr int32_t kHudSideW = 104;       // the title's room left of the score                // tile objects: more than ever fit on the panel
constexpr int kLanes = Game::kLanes;
constexpr int kMaxSongRows = pianotiles::kSongCount + 1;   // the song menu: the songs and QUIT

/* ── Pads (index 0 = bottom left = PAD 1, 15 = top right = PAD 16) ───────
 * Playing: a column is a lane; any of its four pads plays it.
 * Menus:   left column = turn (top two up, bottom two down), the middle two
 *          columns = click, right column = power (back).                   */
constexpr uint16_t kMenuUp = (1u << 8) | (1u << 12);
constexpr uint16_t kMenuDown = (1u << 0) | (1u << 4);
constexpr uint16_t kMenuChoose = 0x6666;
constexpr uint16_t kMenuBack = (1u << 3) | (1u << 7) | (1u << 11) | (1u << 15);

/// Pad colours (0xRRGGBB). No white: the simulator draws it as an unlit pad.
constexpr uint32_t kLedHead = 0x0030FF;
constexpr uint32_t kLedHitNow = 0x00FF40;
constexpr uint32_t kLedBody = 0x7000D0;
constexpr uint32_t kLedHeld = 0xFFB000;
constexpr uint32_t kLedFail = 0xFF0000;
constexpr uint32_t kLedMenuMove = 0x001070;
constexpr uint32_t kLedMenuChoose = 0x006020;
constexpr uint32_t kLedMenuBack = 0x601000;

/// Panel colours: a cream keyboard, black tiles.
constexpr uint32_t kColField = 0xF5F2EA;
constexpr uint32_t kColLaneLine = 0xC9C3B6;
constexpr uint32_t kColBandLine = 0xE2DCCD;
constexpr uint32_t kColHitZone = 0xCFE3F7;
constexpr uint32_t kColHitLine = 0x3A7BD5;
constexpr uint32_t kColTile = 0x141414;
constexpr uint32_t kColHoldTile = 0x1C2541;
constexpr uint32_t kColHoldStripe = 0x5A6B9A;
constexpr uint32_t kColHeldTile = 0x2A4A8C;
constexpr uint32_t kColHeldFill = 0x2EC4F0;
constexpr uint32_t kColPlayed = 0xD3CEC3;
constexpr uint32_t kColFail = 0xE53935;
constexpr uint32_t kColExpected = 0x18B04A;
constexpr uint32_t kColAccent = 0x3A7BD5;

enum class Screen : uint8_t { Songs, Game, Paused, Result };

enum class Action : uint8_t { Resume, Restart, SongMenu, Quit, NextLevel, NextSong, Retry };

struct MenuItem {
    const char* text;
    Action action;
};
constexpr int kMaxMenuItems = 4;

/* ── Pads -> LVGL thread: the last press and release of each pad, with its time ── */

std::atomic<uint64_t> s_downUs[16];
std::atomic<uint64_t> s_upUs[16];
std::atomic<uint8_t> s_downVel[16];
std::atomic<bool> s_ledsStale{true};

uint64_t nowUs() { return crosspad::getClock().getTimeUs(); }
uint32_t nowMs() { return static_cast<uint32_t>(nowUs() / 1000); }

class PianoPadLogic : public crosspad::IPadLogicHandler {
public:
    void onActivate(crosspad::PadManager&) override { s_ledsStale.store(true); }
    void onDeactivate(crosspad::PadManager& pm) override {
        for (uint8_t i = 0; i < 16; ++i) pm.setPadColor(i, crosspad::RgbColor(0, 0, 0));
        s_ledsStale.store(true);
    }
    void onPadPress(crosspad::PadManager&, uint8_t pad, uint8_t velocity) override {
        if (pad >= 16) return;
        s_downVel[pad].store(velocity ? velocity : 1);
        s_downUs[pad].store(nowUs() | 1);   // never 0, which means "none"
    }
    void onPadRelease(crosspad::PadManager&, uint8_t pad) override {
        if (pad < 16) s_upUs[pad].store(nowUs() | 1);
    }
    void onPadPressure(crosspad::PadManager&, uint8_t, uint8_t) override {}
};

std::shared_ptr<PianoPadLogic> s_padLogic;
pianotiles::Synth s_synth;
bool s_synthAttached = false;

/* ── State (LVGL thread) ─────────────────────────────────────────────── */

Game s_game;
pianotiles::Progress s_progress;
std::string s_progressPath;
Screen s_screen = Screen::Songs;
int s_song = 0;              // the song in play (and the menu's highlight)
int s_songSel = 0;           // song menu highlight; songCount() = QUIT
bool s_levelPick = false;
int s_levelSel = 1;
const MenuItem* s_menuItems = nullptr;
int s_menuCount = 0;
int s_menuSel = 0;
MenuItem s_resultItems[kMaxMenuItems];
uint64_t s_lastUs = 0;       // the song clock has been stepped up to here
uint32_t s_resultAt = 0;     // Result: when its menu opens (0 = open)
uint32_t s_resumeAt = 0;     // Game: the countdown after a pause ends here (0 = none)
uint16_t s_padsDown = 0;     // pads down, as replayed
uint8_t s_lastVel = 100;
int s_holdMidi = -1;
uint32_t s_leds[16];
bool s_newBest = false;
int s_unlockedLevel = 0;     // what a clear opened, for the result text
int s_unlockedSong = -1;

int32_t s_w = 320, s_h = 240, s_yHit = 224;

lv_obj_t* s_root = nullptr;
lv_obj_t* s_input = nullptr;
lv_obj_t* s_field = nullptr;
lv_obj_t* s_tile[kPool] = {};
lv_obj_t* s_tileStripe[kPool] = {};
lv_obj_t* s_tileFill[kPool] = {};
lv_obj_t* s_startLabel = nullptr;
lv_obj_t* s_failMark = nullptr;
lv_obj_t* s_score = nullptr;
lv_obj_t* s_title = nullptr;
lv_obj_t* s_level = nullptr;
lv_obj_t* s_combo = nullptr;
lv_obj_t* s_banner = nullptr;
lv_obj_t* s_bannerTitle = nullptr;
lv_obj_t* s_bannerText = nullptr;
lv_obj_t* s_menuBox = nullptr;
lv_obj_t* s_menuTitle = nullptr;
lv_obj_t* s_menuNote = nullptr;
lv_obj_t* s_menuItem[kMaxMenuItems] = {};
lv_obj_t* s_songs = nullptr;
lv_obj_t* s_songName[kMaxSongRows] = {};
lv_obj_t* s_songInfo[kMaxSongRows] = {};
lv_obj_t* s_songsNote = nullptr;
lv_group_t* s_group = nullptr;
lv_group_t* s_prevGroup = nullptr;
lv_timer_t* s_timer = nullptr;
uint32_t s_backToken = 0;

constexpr MenuItem kPauseMenu[] = {
    {"RESUME", Action::Resume}, {"RESTART", Action::Restart}, {"SONG MENU", Action::SongMenu}, {"QUIT", Action::Quit}};

lv_indev_t* navEncoder() { return crosspad_gui::getGuiPlatform().getNavigationEncoder(); }

lv_obj_t* makeRect(lv_obj_t* parent, int32_t w, int32_t h, uint32_t color) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return o;
}

lv_obj_t* makeLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    return l;
}

void setText(lv_obj_t* l, const char* text) {
    if (std::strcmp(lv_label_get_text(l), text) != 0) lv_label_set_text(l, text);
}

void show(lv_obj_t* o, bool on) {
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

const pianotiles::Song& song(int i) { return pianotiles::songs()[i]; }

/* ── Progress on the card ────────────────────────────────────────────── */

bool makeDirs(const std::string& dir) {
    for (size_t at = 1; at <= dir.size(); ++at) {
        if (at != dir.size() && dir[at] != '/') continue;
        if (!crosspad::getFileSystem().makeDir(dir.substr(0, at).c_str())) return false;
    }
    return true;
}

void loadProgress() {
    s_progress = pianotiles::Progress();
    std::FILE* f = std::fopen(s_progressPath.c_str(), "rb");
    if (!f) {
        std::printf("[PianoTiles] no progress yet (%s)\n", s_progressPath.c_str());
        return;
    }
    std::string text;
    char buf[256];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    s_progress.parse(text);
    std::printf("[PianoTiles] progress loaded from %s\n", s_progressPath.c_str());
    for (int s = 0; s < pianotiles::songCount(); ++s)
        std::printf("[PianoTiles]   %s: levels open %d, best %d\n", song(s).id, s_progress.of(s).unlocked,
                    s_progress.bestOf(s));
}

/* The card is written on a task of its own, never on the LVGL thread: a
 * write can wait on the card for as long as the card likes. The GUI side
 * hands over the newest text and wakes it; a second save before the first is
 * written replaces the text. */
constexpr uint32_t kSaveStackBytes = 4096;
SemaphoreHandle_t s_saveLock = nullptr;
TaskHandle_t s_saveTask = nullptr;
std::string s_saveText;

void writeProgress(const std::string& text) {
    const size_t slash = s_progressPath.rfind('/');
    if (slash != std::string::npos) makeDirs(s_progressPath.substr(0, slash));
    std::FILE* f = std::fopen(s_progressPath.c_str(), "wb");
    if (!f) {
        std::printf("[PianoTiles] cannot write %s\n", s_progressPath.c_str());
        return;
    }
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    std::printf("[PianoTiles] progress saved to %s\n", s_progressPath.c_str());
}

void saveTask(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        std::string text;
        xSemaphoreTake(s_saveLock, portMAX_DELAY);
        text.swap(s_saveText);
        xSemaphoreGive(s_saveLock);
        if (!text.empty()) writeProgress(text);
    }
}

void saveProgress() {
    if (!s_saveLock) s_saveLock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_saveLock, portMAX_DELAY);
    s_saveText = s_progress.serialize();
    xSemaphoreGive(s_saveLock);
#if defined(ESP_PLATFORM)
    const uint32_t depth = kSaveStackBytes;   // bytes on ESP-IDF
#else
    const uint32_t depth = kSaveStackBytes / sizeof(StackType_t);
#endif
    if (!s_saveTask && xTaskCreate(saveTask, "pt_save", depth, nullptr, 1, &s_saveTask) != pdPASS) {
        s_saveTask = nullptr;
        std::printf("[PianoTiles] no task to save the progress\n");
        return;
    }
    xTaskNotifyGive(s_saveTask);
}

/* ── Song menu ───────────────────────────────────────────────────────── */

void paintSongs() {
    const int count = pianotiles::songCount();
    for (int i = 0; i <= count; ++i) {
        const bool sel = i == s_songSel;
        lv_obj_t* row = s_songName[i];
        lv_obj_set_style_bg_opa(lv_obj_get_parent(row), sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        const bool open = i == count || s_progress.songUnlocked(i);
        const uint32_t textCol = sel ? 0xFFFFFF : open ? 0xD8D8D8 : 0x707070;
        lv_obj_set_style_text_color(row, lv_color_hex(textCol), 0);
        lv_obj_set_style_text_color(s_songInfo[i], lv_color_hex(sel && s_levelPick ? 0xFFE040 : textCol), 0);
        if (i == count) {
            setText(row, "QUIT");
            setText(s_songInfo[i], "");
            continue;
        }
        setText(row, song(i).title);
        char info[48];
        const pianotiles::SongProgress& p = s_progress.of(i);
        if (!open) std::snprintf(info, sizeof info, "locked");
        else if (sel && s_levelPick)
            std::snprintf(info, sizeof info, "< LEVEL %d >  best %d", s_levelSel, p.best[s_levelSel - 1]);
        else std::snprintf(info, sizeof info, "L%d/%d  best %d", p.unlocked, pianotiles::kMaxLevel, s_progress.bestOf(i));
        setText(s_songInfo[i], info);
    }
    if (s_levelPick)
        setText(s_songsNote, "Turn: level   Click: play   Power: back");
    else if (s_songSel == count)
        setText(s_songsNote, "Click: back to the other firmware");
    else if (!s_progress.songUnlocked(s_songSel))
        setText(s_songsNote, "Clear the song above to open this one");
    else
        setText(s_songsNote, "Turn: song   Click: level   Power: quit");
}

void showSongs(int sel) {
    s_screen = Screen::Songs;
    s_songSel = sel;
    s_levelPick = false;
    s_synth.allOff();
    show(s_menuBox, false);
    show(s_banner, false);
    show(s_songs, true);
    paintSongs();
    s_ledsStale = true;
}

/* ── Playing ─────────────────────────────────────────────────────────── */

void startSong(int songIndex, int level) {
    s_song = songIndex;
    s_game.load(song(songIndex), level);
    s_screen = Screen::Game;
    s_padsDown = 0;
    s_holdMidi = -1;
    s_resumeAt = 0;
    s_lastUs = nowUs();
    show(s_songs, false);
    show(s_menuBox, false);
    show(s_banner, false);
    show(s_failMark, false);
    s_ledsStale = true;
    std::printf("[PianoTiles] start %s level %d (%.0f bpm, %zu tiles)\n", song(songIndex).id, s_game.level(),
                static_cast<double>(s_game.bpm()), s_game.tiles().size());
}

/* ── Menus (pause, result) ───────────────────────────────────────────── */

void paintMenu() {
    for (int i = 0; i < kMaxMenuItems; ++i) {
        show(s_menuItem[i], i < s_menuCount);
        if (i >= s_menuCount) continue;
        lv_label_set_text(s_menuItem[i], s_menuItems[i].text);
        const bool sel = i == s_menuSel;
        lv_obj_set_style_bg_opa(s_menuItem[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(s_menuItem[i], lv_color_hex(sel ? 0xFFFFFF : 0xC0C0C0), 0);
        lv_obj_align(s_menuItem[i], LV_ALIGN_BOTTOM_MID, 0, -8 - (s_menuCount - 1 - i) * 28);
    }
}

void openMenu(const MenuItem* items, int count, const char* title, const char* note) {
    s_menuItems = items;
    s_menuCount = count;
    s_menuSel = 0;
    lv_label_set_text(s_menuTitle, title);
    lv_label_set_text(s_menuNote, note);
    paintMenu();
    show(s_menuBox, true);
    s_ledsStale = true;
}

void pause() {
    s_screen = Screen::Paused;
    if (s_game.holdingTile() >= 0) {   // a hold cannot survive a pause
        s_game.release(s_game.tiles()[static_cast<size_t>(s_game.holdingTile())].lane);
    }
    s_synth.allOff();
    openMenu(kPauseMenu, 4, "PAUSED", song(s_song).title);
    std::printf("[PianoTiles] paused\n");
}

void resume() {
    s_screen = Screen::Game;
    show(s_menuBox, false);
    s_padsDown = 0;
    // A song that has not started needs no countdown: it waits for its first tile.
    s_resumeAt = s_game.phase() == Phase::Playing ? nowMs() + kCountdownMs : 0;
    s_lastUs = nowUs();
    s_ledsStale = true;
    std::printf("[PianoTiles] resumed\n");
}

/// Back to the other firmware slot; false (and a note in the menu) when the platform refused.
bool leaveFirmware() {
    std::printf("[PianoTiles] Quit: back to the other firmware\n");
    if (crosspad_gui::fwViewService().flowSwitchAndRestart(false)) return true;
    const char* why = "There is no other firmware to go back to.";
    if (s_screen == Screen::Songs) setText(s_songsNote, why);
    else lv_label_set_text(s_menuNote, why);
    return false;
}

void runAction(Action a) {
    std::printf("[PianoTiles] menu: %s\n", s_menuItems ? s_menuItems[s_menuSel].text : "?");
    switch (a) {
        case Action::Resume: resume(); break;
        case Action::Restart:
        case Action::Retry: startSong(s_song, s_game.level()); break;
        case Action::SongMenu: showSongs(s_song); break;
        case Action::Quit: leaveFirmware(); break;
        case Action::NextLevel: startSong(s_song, s_game.level() + 1); break;
        case Action::NextSong: startSong(s_song + 1, 1); break;
    }
}

void menuMove(int d) {
    if (s_screen == Screen::Songs) {
        if (s_levelPick) {
            const int open = s_progress.of(s_songSel).unlocked;
            s_levelSel = std::clamp(s_levelSel + d, 1, open);
        } else {
            const int n = pianotiles::songCount() + 1;
            s_songSel = (s_songSel + d + n) % n;
        }
        paintSongs();
    } else if (s_menuCount) {
        s_menuSel = (s_menuSel + d + s_menuCount) % s_menuCount;
        paintMenu();
    }
    s_synth.noteOn(84, 40, 40);
}

void menuSelect() {
    if (s_screen == Screen::Songs) {
        const int count = pianotiles::songCount();
        if (s_levelPick) {
            std::printf("[PianoTiles] menu: play %s level %d\n", song(s_songSel).id, s_levelSel);
            startSong(s_songSel, s_levelSel);
        } else if (s_songSel == count) {
            leaveFirmware();
        } else if (s_progress.songUnlocked(s_songSel)) {
            s_levelPick = true;
            s_levelSel = s_progress.of(s_songSel).unlocked;
            s_synth.noteOn(91, 60, 60);
            paintSongs();
        } else {
            s_synth.noteOn(40, 80, 150);
            paintSongs();
        }
        return;
    }
    if (s_menuCount) {
        s_synth.noteOn(91, 60, 60);
        runAction(s_menuItems[s_menuSel].action);
    }
}

/* ── The end of a run ────────────────────────────────────────────────── */

void endRun(bool cleared) {
    const int songIndex = s_song, level = s_game.level();
    const pianotiles::SongProgress before = s_progress.of(songIndex);
    const bool nextSongWasOpen = songIndex + 1 < pianotiles::songCount() && s_progress.songUnlocked(songIndex + 1);
    s_newBest = s_game.score() > before.best[level - 1];
    if (s_progress.record(songIndex, level, s_game.score(), cleared)) saveProgress();
    s_unlockedLevel = s_progress.of(songIndex).unlocked > before.unlocked ? s_progress.of(songIndex).unlocked : 0;
    s_unlockedSong = !nextSongWasOpen && songIndex + 1 < pianotiles::songCount() &&
                             s_progress.songUnlocked(songIndex + 1)
                         ? songIndex + 1
                         : -1;
    s_screen = Screen::Result;
    s_resultAt = nowMs() + kResultMs;

    char text[96];
    if (cleared) {
        lv_obj_set_style_text_color(s_bannerTitle, lv_color_hex(kColExpected), 0);
        lv_label_set_text(s_bannerTitle, "SONG CLEARED!");
        if (s_unlockedSong >= 0)
            std::snprintf(text, sizeof text, "Score %d%s\nLevel %d and %s unlocked", s_game.score(),
                          s_newBest ? "  NEW BEST" : "", s_unlockedLevel, song(s_unlockedSong).title);
        else if (s_unlockedLevel)
            std::snprintf(text, sizeof text, "Score %d%s\nLevel %d unlocked", s_game.score(),
                          s_newBest ? "  NEW BEST" : "", s_unlockedLevel);
        else
            std::snprintf(text, sizeof text, "Score %d%s\nBest %d", s_game.score(), s_newBest ? "  NEW BEST" : "",
                          s_progress.of(songIndex).best[level - 1]);
        static constexpr int kFanfare[] = {72, 76, 79, 84};
        for (int i = 0; i < 4; ++i) s_synth.noteOn(kFanfare[i], 100, i == 3 ? 700 : 160, 250 + 130 * i);
        std::printf("[PianoTiles] cleared %s level %d score %d best %d perfect %d/%zu combo %d unlocked level %d song %s\n",
                    song(songIndex).id, level, s_game.score(), s_progress.of(songIndex).best[level - 1],
                    s_game.perfects(), s_game.tiles().size(), s_game.bestCombo(), s_progress.of(songIndex).unlocked,
                    s_unlockedSong >= 0 ? song(s_unlockedSong).id : "-");
    } else {
        lv_obj_set_style_text_color(s_bannerTitle, lv_color_hex(kColFail), 0);
        const int expected = s_game.tiles()[static_cast<size_t>(s_game.failTile())].lane;
        switch (s_game.failReason()) {
            case Fail::Missed:
                lv_label_set_text(s_bannerTitle, "MISSED!");
                std::snprintf(text, sizeof text, "The tile in column %d went by\nScore %d%s", expected + 1,
                              s_game.score(), s_newBest ? "  NEW BEST" : "");
                break;
            case Fail::WrongPad:
                lv_label_set_text(s_bannerTitle, "WRONG PAD!");
                std::snprintf(text, sizeof text, "Column %d pressed, the tile was in %d\nScore %d%s",
                              s_game.failLane() + 1, expected + 1, s_game.score(), s_newBest ? "  NEW BEST" : "");
                break;
            default:
                lv_label_set_text(s_bannerTitle, "TOO EARLY!");
                std::snprintf(text, sizeof text, "Wait for the tile to reach the line\nScore %d%s", s_game.score(),
                              s_newBest ? "  NEW BEST" : "");
                break;
        }
        s_synth.allOff();
        s_synth.noteOn(40, 110, 500);
        s_synth.noteOn(41, 110, 500);
        s_synth.noteOn(46, 110, 500);
        static const char* const kReason[] = {"none", "missed", "wrong_pad", "too_early"};
        std::printf("[PianoTiles] fail %s song %s level %d tile %d/%zu lane %d expected %d score %d\n",
                    kReason[static_cast<int>(s_game.failReason())], song(songIndex).id, level, s_game.failTile() + 1,
                    s_game.tiles().size(), s_game.failLane() + 1, expected + 1, s_game.score());
    }
    lv_label_set_text(s_bannerText, text);
    show(s_banner, true);
    s_ledsStale = true;
}

void openResultMenu() {
    s_resultAt = 0;
    int n = 0;
    const bool cleared = s_game.phase() == Phase::Cleared;
    if (cleared && s_game.level() < pianotiles::kMaxLevel) s_resultItems[n++] = {"NEXT LEVEL", Action::NextLevel};
    else if (cleared && s_song + 1 < pianotiles::songCount()) s_resultItems[n++] = {"NEXT SONG", Action::NextSong};
    s_resultItems[n++] = {cleared ? "PLAY AGAIN" : "RETRY", Action::Retry};
    s_resultItems[n++] = {"SONG MENU", Action::SongMenu};
    s_resultItems[n++] = {"QUIT", Action::Quit};
    show(s_banner, false);
    static char note[64];
    std::snprintf(note, sizeof note, "%s  L%d  %d", song(s_song).title, s_game.level(), s_game.score());
    openMenu(s_resultItems, n, cleared ? "CLEARED" : "GAME OVER", note);
}

void onGameEvents(uint32_t ev) {
    if (ev & pianotiles::kEvStart) std::printf("[PianoTiles] song started\n");
    if (ev & pianotiles::kEvHit) {
        const int i = s_game.lastHit();
        const pianotiles::Tile& t = s_game.tiles()[static_cast<size_t>(i)];
        const float beats = t.hold ? t.beats : t.beats * 0.9f;
        s_synth.noteOn(t.midi, s_lastVel, static_cast<uint32_t>(beats * s_game.secondsPerBeat() * 1000.0f));
        if (t.hold) s_holdMidi = t.midi;
        std::printf("[PianoTiles] hit %d/%zu lane %d %s score %d combo %d\n", i + 1, s_game.tiles().size(), t.lane + 1,
                    s_game.lastGrade() == pianotiles::Grade::Perfect ? "PERFECT" : "GOOD", s_game.score(),
                    s_game.combo());
    }
    if (ev & pianotiles::kEvHoldBroken) {
        if (s_holdMidi >= 0) s_synth.noteOff(s_holdMidi);
        s_holdMidi = -1;
        std::printf("[PianoTiles] hold let go early\n");
    }
    if (ev & pianotiles::kEvHoldDone) {
        s_holdMidi = -1;
        std::printf("[PianoTiles] hold held through, score %d\n", s_game.score());
    }
    if (ev & pianotiles::kEvFail) endRun(false);
    else if (ev & pianotiles::kEvCleared) endRun(true);
}

/* ── Input ───────────────────────────────────────────────────────────── */

void backOnce() {
    switch (s_screen) {
        case Screen::Game: pause(); break;
        case Screen::Paused: resume(); break;
        case Screen::Result:
            if (s_resultAt) openResultMenu();
            else showSongs(s_song);
            break;
        case Screen::Songs:
            if (s_levelPick) s_levelPick = false;
            else s_songSel = pianotiles::songCount();
            paintSongs();
            break;
    }
}

/* Power: pause and resume in play; back one step in the menus. Pushed before
 * the encoder's edit mode, which is always on here, would take the click. */
bool onPowerBack(void*) {
    backOnce();
    return true;
}

void onEncoderKey(lv_event_t* e) {
    const uint32_t k = lv_event_get_key(e);
    if (k != LV_KEY_LEFT && k != LV_KEY_RIGHT && k != LV_KEY_ENTER) return;
    lv_event_stop_processing(e);
    const int dir = k == LV_KEY_LEFT ? -1 : k == LV_KEY_RIGHT ? 1 : 0;   // wheel up = LEFT = up the list
    switch (s_screen) {
        case Screen::Game: break;   // the pads play; the encoder rests
        case Screen::Result:
            if (s_resultAt) {
                if (!dir) openResultMenu();   // the click skips the result
                break;
            }
            [[fallthrough]];
        default:
            if (dir) menuMove(dir);
            else menuSelect();
            break;
    }
}

struct PadEvent {
    uint64_t us;
    uint8_t pad;
    bool down;
};

/// A pad event on a menu screen: the pads as the encoder and power.
void menuPad(uint8_t pad) {
    const uint16_t bit = static_cast<uint16_t>(1u << pad);
    if (s_screen == Screen::Result && s_resultAt) return;   // the press that ended the run lands here
    if (bit & kMenuUp) menuMove(-1);
    else if (bit & kMenuDown) menuMove(1);
    else if (bit & kMenuChoose) menuSelect();
    else if (bit & kMenuBack) backOnce();
}

void stepTo(uint64_t us) {
    if (s_screen != Screen::Game || s_resumeAt || s_game.phase() != Phase::Playing) {
        s_lastUs = std::max(s_lastUs, us);
        return;
    }
    if (us > s_lastUs) {
        float dt = static_cast<float>(us - s_lastUs) / 1e6f;
        s_lastUs = us;
        if (dt > 0.25f) dt = 0.25f;   // a stalled frame does not skip half the song
        onGameEvents(s_game.step(dt));
    }
}

/// Replays the pads' presses and releases in the order they happened.
void handlePads() {
    PadEvent ev[32];
    int n = 0;
    for (uint8_t p = 0; p < 16; ++p) {
        if (const uint64_t d = s_downUs[p].exchange(0)) ev[n++] = {d, p, true};
        if (const uint64_t u = s_upUs[p].exchange(0)) ev[n++] = {u, p, false};
    }
    std::sort(ev, ev + n, [](const PadEvent& a, const PadEvent& b) { return a.us < b.us; });
    for (int i = 0; i < n; ++i) {
        const PadEvent& e = ev[i];
        const uint16_t bit = static_cast<uint16_t>(1u << e.pad);
        const int lane = e.pad % kLanes;
        const uint16_t colMask = static_cast<uint16_t>(0x1111u << lane);
        const bool colWasDown = s_padsDown & colMask;
        if (e.down) s_padsDown |= bit;
        else s_padsDown &= static_cast<uint16_t>(~bit);
        const bool colDown = s_padsDown & colMask;
        if (s_screen != Screen::Game) {
            if (e.down) menuPad(e.pad);
            continue;
        }
        if (s_resumeAt) continue;
        stepTo(e.us);
        if (s_screen != Screen::Game) continue;   // that step ended the run
        if (e.down && !colWasDown) {
            s_lastVel = s_downVel[e.pad].load();
            onGameEvents(s_game.press(lane));
        } else if (!e.down && colWasDown && !colDown) {
            onGameEvents(s_game.release(lane));
        }
    }
}

/* ── Drawing ─────────────────────────────────────────────────────────── */

int32_t yOfBeat(float beat) {
    return s_yHit - static_cast<int32_t>((beat - s_game.songBeat()) * static_cast<float>(kPxPerBeat));
}

void drawTiles() {
    const auto& tiles = s_game.tiles();
    const int32_t laneW = s_w / kLanes;
    int used = 0;
    const int first = std::max(0, std::min(s_game.nextTile(), s_game.holdingTile() >= 0 ? s_game.holdingTile()
                                                                                      : s_game.nextTile()) - 12);
    const bool failed = s_game.phase() == Phase::Failed;
    bool startShown = false;
    for (int i = first; i < static_cast<int>(tiles.size()) && used < kPool; ++i) {
        const pianotiles::Tile& t = tiles[static_cast<size_t>(i)];
        const int32_t head = yOfBeat(t.beat);
        const int32_t top = yOfBeat(t.beat + t.beats) + 2;
        if (top >= s_h) continue;   // gone below the panel
        if (head < 0) break;        // the rest are above it
        lv_obj_t* o = s_tile[used];
        lv_obj_t* stripe = s_tileStripe[used];
        lv_obj_t* fill = s_tileFill[used];
        ++used;
        const int32_t height = std::max<int32_t>(head - top, 8);
        lv_obj_set_pos(o, t.lane * laneW + 2, top);
        lv_obj_set_size(o, laneW - 4, height);
        const bool played = s_game.tileDone(i) && i != s_game.holdingTile();
        const bool isFail = failed && i == s_game.failTile();
        const bool held = i == s_game.holdingTile();
        uint32_t col = played ? kColPlayed : held ? kColHeldTile : t.hold ? kColHoldTile : kColTile;
        if (isFail && s_game.failReason() == Fail::Missed) col = kColFail;
        lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
        lv_obj_set_style_border_width(o, isFail && s_game.failReason() != Fail::Missed ? 4 : 0, 0);
        show(stripe, t.hold && !played);
        if (t.hold && !played) {
            lv_obj_set_size(stripe, 4, std::max<int32_t>(height - 24, 1));
            lv_obj_align(stripe, LV_ALIGN_TOP_MID, 0, 6);
        }
        show(fill, held);
        if (held) {
            const int32_t fh = std::clamp<int32_t>(head - s_yHit, 0, height);   // what has passed the line
            lv_obj_set_size(fill, laneW - 4, fh);
            lv_obj_align(fill, LV_ALIGN_BOTTOM_MID, 0, 0);
        }
        show(o, true);
        if (i == 0 && s_game.phase() == Phase::Ready) {
            lv_obj_align_to(s_startLabel, o, LV_ALIGN_CENTER, 0, 0);
            startShown = true;
        }
    }
    for (int i = used; i < kPool; ++i) show(s_tile[i], false);
    show(s_startLabel, startShown);
    if (failed && s_game.failReason() != Fail::Missed) {
        lv_obj_set_pos(s_failMark, s_game.failLane() * laneW + 2, s_yHit - kPxPerBeat / 2);
        show(s_failMark, true);
    } else {
        show(s_failMark, false);
    }
}

void drawHud() {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%d", s_game.score());
    setText(s_score, buf);
    setText(s_title, song(s_song).title);
    std::snprintf(buf, sizeof buf, "LEVEL %d\nBEST %d", s_game.level(), s_progress.of(s_song).best[s_game.level() - 1]);
    setText(s_level, buf);
    if (s_screen == Screen::Game && s_resumeAt) {
        const uint32_t left = s_resumeAt - nowMs();
        std::snprintf(buf, sizeof buf, "%u", static_cast<unsigned>(left / (kCountdownMs / 3) + 1));
    } else if (s_game.phase() == Phase::Ready) {
        std::snprintf(buf, sizeof buf, "Play the START tile");
    } else if (s_game.phase() == Phase::Playing && s_game.hits() > 1) {
        if (s_game.lastGrade() == pianotiles::Grade::Perfect)
            std::snprintf(buf, sizeof buf, "PERFECT  x%d", s_game.combo());
        else
            std::snprintf(buf, sizeof buf, "GOOD");
    } else {
        buf[0] = 0;
    }
    setText(s_combo, buf);
    show(s_combo, buf[0] != 0);
}

uint32_t roleColor(PadRole r) {
    switch (r) {
        case PadRole::Head: return kLedHead;
        case PadRole::HitNow: return kLedHitNow;
        case PadRole::Body: return kLedBody;
        case PadRole::Held: return kLedHeld;
        default: return 0;
    }
}

/// The pads: in play, the bottom of the field (Game::padRoles); after a
/// fail, the wrong column red and the right one green, blinking; a clear
/// sweeps green up the pads; the menus show what each column does.
void drawLeds() {
    uint32_t want[16] = {};
    const uint32_t now = nowMs();
    const bool blinkOn = (now / kBlinkMs) & 1;
    if (s_screen == Screen::Game && !s_resumeAt) {
        PadRole roles[16];
        s_game.padRoles(roles);
        for (int i = 0; i < 16; ++i) want[i] = roleColor(roles[i]);
    } else if (s_screen == Screen::Result && s_resultAt) {
        if (s_game.phase() == Phase::Cleared) {
            const int row = static_cast<int>((now / 120) % 4);
            for (int c = 0; c < kLanes; ++c) want[row * kLanes + c] = kLedHitNow;
        } else if (blinkOn) {
            const int expected = s_game.tiles()[static_cast<size_t>(s_game.failTile())].lane;
            for (int r = 0; r < 4; ++r) {
                if (s_game.failReason() != Fail::Missed) want[r * kLanes + expected] = kLedHitNow;
                want[r * kLanes + s_game.failLane()] = kLedFail;
            }
        }
    } else if (s_screen != Screen::Game) {
        for (int i = 0; i < 16; ++i) {
            const uint16_t bit = static_cast<uint16_t>(1u << i);
            want[i] = (bit & (kMenuUp | kMenuDown)) ? kLedMenuMove : (bit & kMenuChoose) ? kLedMenuChoose : kLedMenuBack;
        }
    }
    const bool stale = s_ledsStale.exchange(false);
    crosspad::PadManager& pm = crosspad::getPadManager();
    for (uint8_t i = 0; i < 16; ++i) {
        if (!stale && want[i] == s_leds[i]) continue;
        s_leds[i] = want[i];
        pm.setPadColor(i, crosspad::RgbColor((want[i] >> 16) & 0xFF, (want[i] >> 8) & 0xFF, want[i] & 0xFF));
    }
}

void tick(lv_timer_t*) {
    // A long press of the encoder toggles edit mode off; the game wants the turns.
    if (s_group && !lv_group_get_editing(s_group)) lv_group_set_editing(s_group, true);
    handlePads();
    if (s_screen == Screen::Game && s_resumeAt && static_cast<int32_t>(nowMs() - s_resumeAt) >= 0) {
        s_resumeAt = 0;
        s_lastUs = nowUs();
    }
    stepTo(nowUs());
    if (s_screen == Screen::Result && s_resultAt && static_cast<int32_t>(nowMs() - s_resultAt) >= 0)
        openResultMenu();
    if (s_screen != Screen::Songs) {
        drawTiles();
        drawHud();
    }
    drawLeds();
}

/* ── Build and teardown ──────────────────────────────────────────────── */

/// A label over the field, on a pale pill so it reads over a black tile too.
lv_obj_t* hudLabel(const lv_font_t* font, uint32_t color, lv_text_align_t align) {
    lv_obj_t* l = makeLabel(s_field, font, color);
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_set_style_bg_color(l, lv_color_hex(kColField), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(l, 4, 0);
    lv_obj_set_style_radius(l, 4, 0);
    return l;
}

void buildField() {
    s_field = makeRect(s_root, s_w, s_h, kColField);
    const int32_t laneW = s_w / kLanes;
    const int32_t early = static_cast<int32_t>(pianotiles::kEarlyBeats * kPxPerBeat);
    const int32_t band = static_cast<int32_t>(pianotiles::kBandBeats * kPxPerBeat);
    lv_obj_t* zone = makeRect(s_field, s_w, s_h - (s_yHit - early), kColHitZone);
    lv_obj_set_pos(zone, 0, s_yHit - early);
    for (int r = 1; r < Game::kRows; ++r) {   // where each pad row starts, above the hit zone
        lv_obj_t* line = makeRect(s_field, s_w, 1, kColBandLine);
        lv_obj_set_pos(line, 0, s_yHit - early - r * band);
    }
    for (int l = 1; l < kLanes; ++l) {
        lv_obj_t* line = makeRect(s_field, 1, s_h, kColLaneLine);
        lv_obj_set_pos(line, l * laneW, 0);
    }
    lv_obj_t* hitLine = makeRect(s_field, s_w, 2, kColHitLine);
    lv_obj_set_pos(hitLine, 0, s_yHit - 1);

    for (int i = 0; i < kPool; ++i) {
        s_tile[i] = makeRect(s_field, laneW - 4, 32, kColTile);
        lv_obj_set_style_radius(s_tile[i], 3, 0);
        lv_obj_set_style_border_color(s_tile[i], lv_color_hex(kColExpected), 0);
        s_tileFill[i] = makeRect(s_tile[i], laneW - 4, 0, kColHeldFill);
        s_tileStripe[i] = makeRect(s_tile[i], 4, 8, kColHoldStripe);
        lv_obj_add_flag(s_tile[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_startLabel = makeLabel(s_field, &lv_font_montserrat_16, 0xFFFFFF);
    lv_label_set_text(s_startLabel, "START");
    s_failMark = makeRect(s_field, laneW - 4, kPxPerBeat / 2 + 16, kColFail);
    lv_obj_set_style_bg_opa(s_failMark, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_failMark, 3, 0);
    lv_obj_add_flag(s_failMark, LV_OBJ_FLAG_HIDDEN);

    s_score = hudLabel(&lv_font_montserrat_24, kColFail, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(s_score, LV_ALIGN_TOP_MID, 0, 2);
    s_title = hudLabel(&lv_font_montserrat_14, 0x303030, LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);   // one line, clear of the score in the middle
    lv_obj_set_size(s_title, kHudSideW, lv_font_get_line_height(&lv_font_montserrat_14));
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 2, 2);
    s_level = hudLabel(&lv_font_montserrat_14, 0x303030, LV_TEXT_ALIGN_RIGHT);
    lv_obj_align(s_level, LV_ALIGN_TOP_RIGHT, -2, 2);
    s_combo = hudLabel(&lv_font_montserrat_16, kColAccent, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(s_combo, LV_ALIGN_TOP_MID, 0, 34);

    s_banner = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_banner);
    lv_obj_set_size(s_banner, 280, 96);
    lv_obj_align(s_banner, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_banner, 6, 0);
    lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    s_bannerTitle = makeLabel(s_banner, &lv_font_montserrat_24, kColFail);
    lv_obj_align(s_bannerTitle, LV_ALIGN_TOP_MID, 0, 10);
    s_bannerText = makeLabel(s_banner, &lv_font_montserrat_14, 0xFFFFFF);
    lv_obj_align(s_bannerText, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
}

void buildMenu() {
    s_menuBox = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_menuBox);
    lv_obj_set_size(s_menuBox, 230, 196);
    lv_obj_center(s_menuBox);
    lv_obj_set_style_bg_color(s_menuBox, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(s_menuBox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_menuBox, lv_color_hex(kColAccent), 0);
    lv_obj_set_style_border_width(s_menuBox, 2, 0);
    lv_obj_set_style_radius(s_menuBox, 6, 0);
    lv_obj_remove_flag(s_menuBox, LV_OBJ_FLAG_SCROLLABLE);
    s_menuTitle = makeLabel(s_menuBox, &lv_font_montserrat_24, 0xFFFFFF);
    lv_obj_align(s_menuTitle, LV_ALIGN_TOP_MID, 0, 6);
    s_menuNote = makeLabel(s_menuBox, &lv_font_montserrat_14, 0xFFC060);
    lv_obj_set_width(s_menuNote, 214);
    lv_label_set_long_mode(s_menuNote, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_menuNote, LV_ALIGN_TOP_MID, 0, 36);
    for (int i = 0; i < kMaxMenuItems; ++i) {
        s_menuItem[i] = makeLabel(s_menuBox, &lv_font_montserrat_16, 0xC0C0C0);
        lv_obj_set_width(s_menuItem[i], 160);
        lv_obj_set_style_bg_color(s_menuItem[i], lv_color_hex(kColAccent), 0);
        lv_obj_set_style_radius(s_menuItem[i], 4, 0);
        lv_obj_set_style_pad_ver(s_menuItem[i], 3, 0);
    }
    lv_obj_add_flag(s_menuBox, LV_OBJ_FLAG_HIDDEN);
}

void buildSongs() {
    s_songs = makeRect(s_root, s_w, s_h, 0x0C1018);
    lv_obj_t* title = makeLabel(s_songs, &lv_font_montserrat_24, 0xFFFFFF);
    lv_label_set_text(title, "PIANO TILES");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);
    const int count = pianotiles::songCount();
    const int32_t rowH = std::min<int32_t>(24, (s_h - 34 - 22) / (count + 1));
    for (int i = 0; i <= count; ++i) {
        lv_obj_t* row = makeRect(s_songs, s_w - 16, rowH - 2, kColAccent);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_pos(row, 8, 34 + i * rowH);
        s_songName[i] = makeLabel(row, &lv_font_montserrat_16, 0xD8D8D8);
        lv_obj_align(s_songName[i], LV_ALIGN_LEFT_MID, 6, 0);
        s_songInfo[i] = makeLabel(row, &lv_font_montserrat_14, 0xD8D8D8);
        lv_obj_align(s_songInfo[i], LV_ALIGN_RIGHT_MID, -6, 0);
    }
    s_songsNote = makeLabel(s_songs, &lv_font_montserrat_14, 0xFFC060);
    lv_obj_align(s_songsNote, LV_ALIGN_BOTTOM_MID, 0, -3);
}

lv_obj_t* lv_CreatePianoTiles(lv_obj_t* parent, App* app) {
    (void)app;
    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(parent);
    s_w = lv_obj_get_content_width(parent);
    s_h = lv_obj_get_content_height(parent);
    s_yHit = s_h - static_cast<int32_t>(pianotiles::kLateBeats * kPxPerBeat);

    s_progressPath = crosspad_gui::getGuiPlatform().storagePath(kDataDir) + "/" + kProgressFile;
    loadProgress();
    s_game.load(song(0), 1);
    buildField();
    buildMenu();
    buildSongs();

    /* The encoder's group: one object always in edit mode, so a turn is a
     * key here rather than a move of focus. Scrollable, so LVGL counts it
     * editable; it has nothing to scroll. */
    s_input = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_input);
    lv_obj_set_size(s_input, 1, 1);
    s_group = lv_group_create();
    lv_group_add_obj(s_group, s_input);
    lv_obj_add_event_cb(s_input, onEncoderKey, static_cast<lv_event_code_t>(LV_EVENT_KEY | LV_EVENT_PREPROCESS),
                        nullptr);
    if (lv_indev_t* nav = navEncoder()) {
        s_prevGroup = lv_indev_get_group(nav);
        lv_indev_set_group(nav, s_group);
    }
    lv_group_focus_obj(s_input);
    lv_group_set_editing(s_group, true);

    s_backToken = crosspad_gui::ui_back_push(onPowerBack, nullptr);

    if (!s_padLogic) {
        s_padLogic = std::make_shared<PianoPadLogic>();
        crosspad::getPadManager().registerPadLogic(kPadLogicName, s_padLogic);
    }
    if (!s_synthAttached) {
        crosspad::getAudioModule().addNode(&s_synth);
        s_synthAttached = true;
    }
    for (int p = 0; p < 16; ++p) {
        s_downUs[p].store(0);
        s_upUs[p].store(0);
    }
    s_lastUs = nowUs();
    showSongs(0);
    s_timer = lv_timer_create(tick, kTickMs, nullptr);
    std::printf("[PianoTiles] ready: %d songs, field %dx%d, hit line at y %d\n", pianotiles::songCount(),
                static_cast<int>(s_w), static_cast<int>(s_h), static_cast<int>(s_yHit));
    return s_root;
}

void lv_DestroyPianoTiles(lv_obj_t* root) {
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    s_synth.allOff();
    crosspad_gui::ui_back_remove(s_backToken);
    s_backToken = 0;
    if (lv_indev_t* nav = navEncoder()) lv_indev_restore_group(nav, s_prevGroup);
    if (s_group) {
        lv_group_delete(s_group);
        s_group = nullptr;
    }
    s_root = s_input = s_field = s_startLabel = s_failMark = s_score = s_title = s_level = s_combo = nullptr;
    s_banner = s_bannerTitle = s_bannerText = s_menuBox = s_menuTitle = s_menuNote = s_songs = s_songsNote = nullptr;
    for (auto& o : s_tile) o = nullptr;
    for (auto& o : s_tileStripe) o = nullptr;
    for (auto& o : s_tileFill) o = nullptr;
    for (auto& o : s_menuItem) o = nullptr;
    for (auto& o : s_songName) o = nullptr;
    for (auto& o : s_songInfo) o = nullptr;
    if (root) lv_obj_delete_async(root);
}

} // namespace

REGISTER_APP_PL(PianoTiles, nullptr, crosspad_gui::resolveAppIcon("pianotiles.png"), lv_CreatePianoTiles,
                lv_DestroyPianoTiles, nullptr, nullptr, nullptr, 0, kPadLogicName)

#endif // USE_LVGL
