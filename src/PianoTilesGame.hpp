// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file PianoTilesGame.hpp
 * @brief Piano Tiles' rules in plain C++: melodies parsed into tiles in four
 *        lanes, the song clock, the hit window, hold tiles, score and combo,
 *        levels (tempo), what the 4x4 pads show, and the saved progress.
 *        No LVGL, no pads, no sound -- the app feeds it presses and time and
 *        turns what it reports into all of those, and a host test drives it
 *        the same way (tests/test_pianotiles_game.cpp).
 *
 * Time is counted in beats of the song. A tile's head reaches the hit line at
 * its onset beat; the pads mirror the bottom of the field in four bands of
 * time-to-hit: the bottom row is the hit window, each row above it half a
 * beat further away.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pianotiles {

/// One note of a melody as written: a MIDI note (or a rest) and its length in beats.
struct Note {
    int midi;      ///< 0..127; kRest for a rest
    float beats;
};
constexpr int kRest = -1;

/// One tile: a note of the melody where it falls.
struct Tile {
    int midi;
    float beat;    ///< onset, in beats from the first tile
    float beats;   ///< length
    uint8_t lane;  ///< 0..3, left to right (= pad column)
    bool hold;     ///< a long note: press at the head, hold to the tail
};

struct Song {
    const char* id;        ///< key in the progress file (a-z, no spaces)
    const char* title;
    const char* composer;
    float bpm;             ///< level 1's tempo, in the melody's beats
    const char* melody;    ///< see parseMelody()
};

/// The built-in songs, easiest first.
constexpr int kSongCount = 6;
const Song* songs();
int songCount();

/**
 * Parse a melody: notes separated by spaces, '|' (bar lines) ignored.
 * A note is a letter A-G, an optional '#' or 'b', an octave 0-8 (C4 = MIDI
 * 60), and an optional length "*<beats>": "*2", "*0.5", "*3/2" (default 1).
 * "R" is a rest: "R*0.5". Returns false and says where in @p err on a bad token.
 */
bool parseMelody(const char* text, std::vector<Note>& out, std::string* err = nullptr);

/// Tiles from notes: onsets from the lengths (rests only add time), lanes from
/// the melody's contour (up = right, down = left, a repeat moves aside), never
/// the same lane twice in a row; notes of kHoldBeats or longer are hold tiles.
std::vector<Tile> buildTiles(const std::vector<Note>& notes);

constexpr float kHoldBeats = 2.0f;
constexpr int kMaxLevel = 5;
/// Tempo of a level as a multiple of the song's own.
float levelTempoScale(int level);

/// Timing, in beats relative to a tile's onset (negative = before it).
constexpr float kEarlyBeats = 0.5f;    ///< the hit window opens this early ...
constexpr float kLateBeats = 0.25f;    ///< ... and closes this late
constexpr float kPerfectBeats = 0.2f;  ///< within this of the onset is PERFECT
constexpr float kBandBeats = 0.5f;     ///< each pad row above the bottom one is this much further away
constexpr float kHoldGraceBeats = 0.5f;  ///< a hold counts as held through once its tail is this near

enum class Phase : uint8_t {
    Ready,    ///< the first tile waits on the hit line; hitting it starts the song
    Playing,
    Failed,   ///< a tile was missed or a wrong pad pressed
    Cleared,  ///< every tile played
};

enum class Fail : uint8_t { None, Missed, WrongPad, TooEarly };

enum class Grade : uint8_t { Perfect, Good };

/// What step() / press() / release() did, as bits.
enum Event : uint32_t {
    kEvHit = 1u << 0,         ///< a tile was hit (lastHit())
    kEvHoldDone = 1u << 1,    ///< a hold tile was held to its tail
    kEvHoldBroken = 1u << 2,  ///< a hold tile was let go of early (no fail, no bonus)
    kEvFail = 1u << 3,        ///< the run ended (failReason())
    kEvCleared = 1u << 4,     ///< the song is done
    kEvStart = 1u << 5,       ///< the first tile was hit: the clock runs
};

/// What a pad shows; the app picks the colours.
enum class PadRole : uint8_t {
    Off,
    Head,      ///< a tile's head, approaching
    HitNow,    ///< the next tile's head, in the hit window: press this column
    Body,      ///< a hold tile's body
    Held,      ///< a hold tile being held
};

class Game {
public:
    static constexpr int kLanes = 4;
    static constexpr int kRows = 4;

    /// Parses the song's melody; false when it does not parse (the game is then empty).
    bool load(const Song& song, int level);
    /// Back to Ready on the same song and level.
    void restart();

    /// Advance the song clock by dt seconds (only while Playing). Returns Event bits.
    uint32_t step(float dt);
    /// A pad of column @p lane went down (the column was up before). Returns Event bits.
    uint32_t press(int lane);
    /// The last held pad of column @p lane went up. Returns Event bits.
    uint32_t release(int lane);

    Phase phase() const { return phase_; }
    Fail failReason() const { return fail_; }
    /// Wrong pad / too early: the pressed column. Missed: the missed tile's column.
    int failLane() const { return failLane_; }
    /// The tile that should have been played when the run failed.
    int failTile() const { return failTile_; }

    const Song* song() const { return song_; }
    int level() const { return level_; }
    float bpm() const { return bpm_; }
    float secondsPerBeat() const { return 60.0f / bpm_; }
    /// Where the hit line is, in beats of the song.
    float songBeat() const { return beat_; }

    const std::vector<Tile>& tiles() const { return tiles_; }
    bool tileDone(int i) const { return done_[static_cast<size_t>(i)] != 0; }
    /// The tile to play next; tiles().size() when all are played.
    int nextTile() const { return next_; }
    /// The hold tile being held, or -1.
    int holdingTile() const { return holding_; }
    /// The last tile hit and how well.
    int lastHit() const { return lastHit_; }
    Grade lastGrade() const { return lastGrade_; }

    /// PERFECT 100, GOOD 50, +5 per PERFECT in a row (up to 40), a hold held
    /// through 50 a beat; all times the level.
    int score() const { return score_; }
    int combo() const { return combo_; }
    int bestCombo() const { return bestCombo_; }
    int hits() const { return hits_; }
    int perfects() const { return perfects_; }

    /// Pad row of a beat distance from the hit line (0 = bottom = hit window), -1 off the pads.
    static int rowOf(float beatsAhead);
    /// The 4x4 pads, index = row * 4 + lane (0 = bottom left).
    void padRoles(PadRole out[16]) const;

private:
    uint32_t hitNext(Grade g);
    uint32_t endHold(bool through);
    uint32_t failWith(Fail f, int lane);
    uint32_t checkCleared();

    const Song* song_ = nullptr;
    int level_ = 1;
    float bpm_ = 120.0f;
    std::vector<Tile> tiles_;
    std::vector<uint8_t> done_;
    Phase phase_ = Phase::Ready;
    Fail fail_ = Fail::None;
    int failLane_ = -1;
    int failTile_ = -1;
    float beat_ = 0.0f;
    int next_ = 0;
    int holding_ = -1;
    int lastHit_ = -1;
    Grade lastGrade_ = Grade::Perfect;
    int score_ = 0;
    int combo_ = 0;
    int bestCombo_ = 0;
    int hits_ = 0;
    int perfects_ = 0;
};

/// Saved progress per song: the highest level open and the best score of each level.
struct SongProgress {
    int unlocked = 1;               ///< levels 1..unlocked can be played
    int best[kMaxLevel] = {};       ///< best score per level
    bool cleared[kMaxLevel] = {};   ///< the level was played to the end
};

class Progress {
public:
    Progress();
    SongProgress& of(int song) { return songs_[static_cast<size_t>(song)]; }
    const SongProgress& of(int song) const { return songs_[static_cast<size_t>(song)]; }
    /// A song is open when it is the first or the one before it was cleared once.
    bool songUnlocked(int song) const;
    /// Best score of a song over its levels.
    int bestOf(int song) const;
    /// Record a run. Returns true when anything changed (a new best, a level opened).
    bool record(int song, int level, int score, bool cleared);

    /// One line per song: "<id> <unlocked> <best L1>..<best L5> <cleared mask>".
    std::string serialize() const;
    /// Lines of unknown songs and broken lines are skipped.
    void parse(const std::string& text);

private:
    std::vector<SongProgress> songs_;
};

} // namespace pianotiles
