// SPDX-License-Identifier: GPL-3.0-or-later
// Host test of Piano Tiles' rules (no LVGL, no platform):
//   c++ -std=c++17 -Wall -Wextra -I src tests/test_pianotiles_game.cpp src/PianoTilesGame.cpp -o /tmp/pt_test && /tmp/pt_test
#include "PianoTilesGame.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace pianotiles;

namespace {

int s_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++s_failures;
}

constexpr float kDt = 1.0f / 60.0f;

uint32_t s_rng = 12345;
float randUnit() {   // 0..1
    s_rng = s_rng * 1664525u + 1013904223u;
    return static_cast<float>(s_rng >> 8) / static_cast<float>(1u << 24);
}

/// Step until the clock is @p beats past @p onset (as a press would land then).
uint32_t stepTo(Game& g, float beat) {
    uint32_t ev = 0;
    while (g.phase() == Phase::Playing && g.songBeat() < beat) ev |= g.step(kDt);
    return ev;
}

struct Run {
    Phase end;
    float seconds;
    int score;
    int perfects;
    int holdsDone;
};

/// A player pressing each tile @p offset(i) beats from its onset (0 = on it), holding
/// holds to the end, ticking at 60 Hz. Gives up after 10 minutes of song.
template <typename Offset>
Run autoplay(Game& g, Offset offset) {
    Run r{};
    uint32_t ev = g.press(g.tiles()[0].lane);
    int held = g.holdingTile() >= 0 ? g.tiles()[0].lane : -1;
    float seconds = 0.0f;
    while (g.phase() == Phase::Playing && seconds < 600.0f) {
        const int n = g.nextTile();
        if (n < static_cast<int>(g.tiles().size())) {
            const Tile& t = g.tiles()[static_cast<size_t>(n)];
            if (g.songBeat() >= t.beat + offset(n)) {
                if (held >= 0 && g.holdingTile() < 0) held = -1;
                ev |= g.press(t.lane);
                if (g.holdingTile() == n) held = t.lane;
            }
        }
        if (held >= 0 && g.holdingTile() < 0) {
            ev |= g.release(held);
            held = -1;
        }
        const uint32_t e = g.step(kDt);
        if (e & kEvHoldDone) ++r.holdsDone;
        ev |= e;
        seconds += kDt;
    }
    r.end = g.phase();
    r.seconds = seconds;
    r.score = g.score();
    r.perfects = g.perfects();
    return r;
}

const Song kTestSong{"test", "Test", "nobody", 120.0f, "C4 D4 E4*2 F4 | R G4*0.5 A4*0.5 C4"};

} // namespace

int main() {
    {   // Melody parsing: notes, accidentals, octaves, lengths, rests; bad tokens say where.
        std::vector<Note> n;
        std::string err;
        check(parseMelody("C4 D#4*0.5 | Bb3*3/2 R*0.5 A4", n, &err) && n.size() == 5, "melody parses");
        check(n.size() == 5 && n[0].midi == 60 && n[1].midi == 63 && n[2].midi == 58 && n[3].midi == kRest &&
                  n[4].midi == 69,
              "pitches: C4=60 D#4=63 Bb3=58 rest A4=69");
        check(n.size() == 5 && n[0].beats == 1.0f && n[1].beats == 0.5f && std::fabs(n[2].beats - 1.5f) < 1e-6f &&
                  n[3].beats == 0.5f,
              "lengths: default 1, *0.5, *3/2");
        check(!parseMelody("C4 H4 E4", n, &err) && err.find("H4") != std::string::npos, "bad letter named");
        check(!parseMelody("C4 C9", n, &err), "octave 9 refused");
        check(!parseMelody("C4*0", n, &err) && !parseMelody("C4*x", n, &err) && !parseMelody("C4*1/0", n, &err),
              "bad lengths refused");
        check(!parseMelody("R R*2 |", n, &err), "only rests: no melody");
    }
    {   // Tiles: onsets from lengths (rests add time), lanes 0..3 never twice in a row, holds from 2 beats.
        std::vector<Note> n;
        parseMelody(kTestSong.melody, n);
        const auto t = buildTiles(n);
        check(t.size() == 7, "7 tiles (a rest is no tile)");
        check(t.size() == 7 && t[3].beat == 4.0f && t[4].beat == 6.0f, "onsets: after a hold, after a rest");
        check(t.size() == 7 && t[2].hold && !t[1].hold && !t[3].hold, "the 2-beat note is a hold tile");
        bool allSongsOk = true;
        for (int s = 0; s < songCount(); ++s) {
            std::vector<Note> notes;
            std::string err;
            if (!parseMelody(songs()[s].melody, notes, &err)) {
                std::printf("     %s: %s\n", songs()[s].id, err.c_str());
                allSongsOk = false;
                continue;
            }
            const auto tiles = buildTiles(notes);
            int holds = 0;
            for (size_t i = 0; i < tiles.size(); ++i) {
                if (tiles[i].lane > 3) allSongsOk = false;
                if (i && (tiles[i].lane == tiles[i - 1].lane || tiles[i].beat <= tiles[i - 1].beat)) allSongsOk = false;
                holds += tiles[i].hold;
            }
            std::printf("     %-12s %3zu tiles, %2d holds, %.0f bpm\n", songs()[s].id, tiles.size(), holds,
                        songs()[s].bpm);
            if (tiles.size() < 20) allSongsOk = false;
        }
        check(allSongsOk, "every song parses; lanes in 0..3, never the same twice, onsets rising, 20+ tiles");
    }
    {   // Levels: faster each level; level 5 the fastest still leaves a human window.
        bool rising = true;
        for (int l = 2; l <= kMaxLevel; ++l) rising = rising && levelTempoScale(l) > levelTempoScale(l - 1);
        check(rising && levelTempoScale(1) == 1.0f, "tempo rises with the level from the song's own");
        Game g;
        g.load(kTestSong, 3);
        check(std::fabs(g.bpm() - 120.0f * levelTempoScale(3)) < 1e-3f, "a level's bpm");
        float fastest = 1e9f, tightest = 1e9f;
        for (int s = 0; s < songCount(); ++s) {
            g.load(songs()[s], kMaxLevel);
            for (size_t i = 1; i < g.tiles().size(); ++i)
                fastest = std::min(fastest, (g.tiles()[i].beat - g.tiles()[i - 1].beat) * g.secondsPerBeat());
            tightest = std::min(tightest, kLateBeats * g.secondsPerBeat());
        }
        std::printf("     level %d: closest tiles %.0f ms apart, late window %.0f ms\n", kMaxLevel, fastest * 1000,
                    tightest * 1000);
        check(fastest >= 0.12f && tightest >= 0.06f, "level 5: tiles >= 120 ms apart, late window >= 60 ms");
    }
    {   // The hit window: Ready waits for the START tile; early, in time, late.
        Game g;
        g.load(kTestSong, 1);
        const auto& t = g.tiles();
        check(g.phase() == Phase::Ready && g.press((t[0].lane + 1) % 4) == 0 && g.phase() == Phase::Ready,
              "Ready: another column does nothing");
        check(g.step(1.0f) == 0 && g.songBeat() == 0.0f, "Ready: the clock waits");
        uint32_t ev = g.press(t[0].lane);
        check((ev & kEvStart) && (ev & kEvHit) && g.phase() == Phase::Playing && g.nextTile() == 1,
              "the START tile starts the song and plays its note");
        stepTo(g, t[1].beat - kEarlyBeats - 0.1f);
        ev = g.press(t[1].lane);
        check((ev & kEvFail) && g.failReason() == Fail::TooEarly && g.failLane() == t[1].lane,
              "the right column before the window: TOO EARLY");

        g.restart();
        g.press(t[0].lane);
        stepTo(g, t[1].beat - kEarlyBeats + 0.05f);
        ev = g.press(t[1].lane);
        check((ev & kEvHit) && g.lastGrade() == Grade::Good, "just inside the window: GOOD");
        stepTo(g, t[2].beat - 0.05f);
        ev = g.press(t[2].lane);
        check((ev & kEvHit) && g.lastGrade() == Grade::Perfect && g.holdingTile() == 2, "on the beat: PERFECT, holding");
        check(g.press(t[2].lane) == 0, "another pad of the held column is no wrong pad");
        const int wrong = (t[3].lane + 1) % 4 == t[2].lane ? (t[3].lane + 2) % 4 : (t[3].lane + 1) % 4;
        stepTo(g, t[3].beat - 0.1f);
        ev = g.press(wrong);
        check((ev & kEvFail) && g.failReason() == Fail::WrongPad && g.failLane() == wrong && g.failTile() == 3,
              "another column: WRONG PAD, the pressed column and the tile it should have been");

        g.restart();
        g.press(t[0].lane);
        ev = stepTo(g, t[1].beat + kLateBeats + 0.05f);
        check((ev & kEvFail) && g.failReason() == Fail::Missed && g.failLane() == t[1].lane && g.failTile() == 1,
              "nothing pressed: MISSED past the late edge, in the tile's column");
        check(g.step(kDt) == 0 && g.press(t[1].lane) == 0, "a failed run takes no more input");
    }
    {   // Hold tiles: held through = bonus; let go early = no bonus, no fail.
        Game a, b;
        a.load(kTestSong, 1);
        b.load(kTestSong, 1);
        const auto& t = a.tiles();
        for (Game* g : {&a, &b}) {
            g->press(t[0].lane);
            stepTo(*g, t[1].beat);
            g->press(t[1].lane);
            stepTo(*g, t[2].beat);
            g->press(t[2].lane);
        }
        stepTo(a, t[2].beat + 0.5f);
        const uint32_t evB = b.release(t[2].lane);
        const uint32_t evA = stepTo(a, t[2].beat + t[2].beats);
        check((evB & kEvHoldBroken) && b.phase() == Phase::Playing && b.holdingTile() < 0, "let go early: broken, playing on");
        check((evA & kEvHoldDone) && a.score() > b.score(), "held through: done, with a bonus");
    }
    {   // Every built-in song ends on a hold: letting go of it early (a pause does that) still clears the song.
        bool all = true;
        for (int s = 0; s < songCount(); ++s) {
            Game g;
            g.load(songs()[s], 1);
            const auto& t = g.tiles();
            uint32_t ev = 0;
            for (size_t i = 0; i < t.size(); ++i) {
                stepTo(g, t[i].beat);
                ev = g.press(t[i].lane);
            }
            const bool held = !(ev & kEvCleared) && g.phase() == Phase::Playing && g.holdingTile() == static_cast<int>(t.size()) - 1;
            stepTo(g, t.back().beat + 0.5f);
            ev = g.release(t.back().lane);
            all = all && held && (ev & kEvHoldBroken) && (ev & kEvCleared) && g.phase() == Phase::Cleared;
        }
        check(all, "the last hold let go early: broken and CLEARED in the same events, in every song");
    }
    {   // The pads: bottom row = hit window, a row per half beat above, heads over bodies.
        Game g;
        g.load(kTestSong, 1);
        PadRole p[16];
        g.padRoles(p);
        const auto& t = g.tiles();
        check(p[t[0].lane] == PadRole::HitNow, "Ready: the START tile is HIT NOW on the bottom row");
        check(Game::rowOf(-0.3f) == -1 && Game::rowOf(-0.2f) == 0 && Game::rowOf(0.49f) == 0 && Game::rowOf(0.5f) == 1 &&
                  Game::rowOf(1.99f) == 3 && Game::rowOf(2.0f) == -1,
              "rows: [-0.25,0.5) bottom, then half a beat each, nothing past 2 beats");
        check(p[1 * 4 + t[1].lane] == PadRole::Off && p[2 * 4 + t[1].lane] == PadRole::Head,
              "the next tile, 1 beat away, is a head on row 2");
        g.press(t[0].lane);
        stepTo(g, t[1].beat - 0.1f);
        g.padRoles(p);
        check(p[t[0].lane] == PadRole::Off && p[t[1].lane] == PadRole::HitNow,
              "a played tile leaves the pads; the next one in the window is HIT NOW");
        g.press(t[1].lane);
        stepTo(g, t[2].beat - 0.1f);
        g.padRoles(p);
        check(p[t[2].lane] == PadRole::HitNow && p[4 + t[2].lane] == PadRole::Body && p[12 + t[2].lane] == PadRole::Body,
              "a hold tile in the window: HIT NOW at the bottom, its body above");
        g.press(t[2].lane);
        stepTo(g, t[2].beat + 0.6f);
        g.padRoles(p);
        check(p[t[2].lane] == PadRole::Held && p[4 + t[2].lane] == PadRole::Held, "held: its column shows HELD");
    }
    {   // Balance: a perfect player clears every song at every level; an idle one loses.
        bool allClear = true, allIdleLose = true;
        for (int s = 0; s < songCount(); ++s) {
            for (int l = 1; l <= kMaxLevel; ++l) {
                Game g;
                g.load(songs()[s], l);
                const Run r = autoplay(g, [](int) { return 0.0f; });
                if (r.end != Phase::Cleared || r.perfects != static_cast<int>(g.tiles().size())) {
                    std::printf("     %s L%d: phase %d perfects %d/%zu\n", songs()[s].id, l, static_cast<int>(r.end),
                                r.perfects, g.tiles().size());
                    allClear = false;
                }
                if (l == 1 || l == kMaxLevel)
                    std::printf("     %-12s L%d cleared in %5.1f s, score %d\n", songs()[s].id, l, r.seconds, r.score);
                Game idle;
                idle.load(songs()[s], l);
                idle.press(idle.tiles()[0].lane);
                for (int i = 0; i < 60 * 60 && idle.phase() == Phase::Playing; ++i) idle.step(kDt);
                if (idle.phase() != Phase::Failed || idle.failReason() != Fail::Missed || idle.failTile() != 1)
                    allIdleLose = false;
            }
        }
        check(allClear, "perfect autoplay clears every song at every level, every tile PERFECT");
        check(allIdleLose, "an idle player misses tile 2 of every song at every level");
    }
    {   // A sloppy but in-window player still clears; a later level takes less time.
        Game g;
        g.load(songs()[2], 1);
        const Run r = autoplay(g, [](int) { return -0.4f + 0.6f * randUnit(); });
        check(r.end == Phase::Cleared && r.perfects < static_cast<int>(g.tiles().size()),
              "jittery player (-0.4..+0.2 beat) clears with some GOODs");
        Game a, b;
        a.load(songs()[0], 1);
        b.load(songs()[0], kMaxLevel);
        const Run ra = autoplay(a, [](int) { return 0.0f; });
        const Run rb = autoplay(b, [](int) { return 0.0f; });
        check(rb.seconds < ra.seconds * 0.6f, "level 5 plays the song in < 60% of level 1's time");
        Game c;
        c.load(songs()[4], 1);   // Canon: holds
        const Run rc = autoplay(c, [](int) { return 0.0f; });
        check(rc.holdsDone >= 8, "Canon's hold tiles are held through");
    }
    {   // Progress: levels and songs open as they are cleared; the file round-trips.
        Progress p;
        check(p.songUnlocked(0) && !p.songUnlocked(1) && p.of(0).unlocked == 1, "fresh: first song, level 1");
        check(p.record(0, 1, 1200, false) && p.of(0).best[0] == 1200 && !p.songUnlocked(1) && p.of(0).unlocked == 1,
              "a failed run keeps its best, opens nothing");
        check(!p.record(0, 1, 900, false), "a worse run changes nothing");
        check(p.record(0, 1, 3000, true) && p.of(0).unlocked == 2 && p.songUnlocked(1) && !p.songUnlocked(2),
              "clearing level 1 opens level 2 and the next song");
        p.record(1, 1, 500, true);
        p.record(1, 2, 700, true);
        Progress q;
        q.parse(p.serialize() + "nosuchsong 3 1 2 3 4 5 0\ntwinkle broken line\n");
        check(q.of(0).best[0] == 3000 && q.of(0).unlocked == 2 && q.of(1).unlocked == 3 && q.of(1).cleared[1] &&
                  q.bestOf(1) == 700 && q.songUnlocked(2),
              "serialize -> parse round-trips; unknown and broken lines skipped");
    }
    std::printf("%s\n", s_failures ? "FAILED" : "all passed");
    return s_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
