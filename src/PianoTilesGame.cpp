// SPDX-License-Identifier: GPL-3.0-or-later
#include "PianoTilesGame.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace pianotiles {

/* ── Songs ───────────────────────────────────────────────────────────────
 * All public domain (composers dead for well over a century, or traditional).
 * The beat is the note the tempo counts: a quarter, except Für Elise (an
 * eighth, so its sixteenths are half beats). */

namespace {

const Song kSongs[] = {
    {"twinkle", "Twinkle Twinkle", "Traditional", 100.0f,
     "C5 C5 G5 G5 | A5 A5 G5*2 | F5 F5 E5 E5 | D5 D5 C5*2 | "
     "G5 G5 F5 F5 | E5 E5 D5*2 | G5 G5 F5 F5 | E5 E5 D5*2 | "
     "C5 C5 G5 G5 | A5 A5 G5*2 | F5 F5 E5 E5 | D5 D5 C5*2"},
    {"jacques", "Frere Jacques", "Traditional", 100.0f,
     "C5 D5 E5 C5 | C5 D5 E5 C5 | E5 F5 G5*2 | E5 F5 G5*2 | "
     "G5*0.5 A5*0.5 G5*0.5 F5*0.5 E5 C5 | G5*0.5 A5*0.5 G5*0.5 F5*0.5 E5 C5 | "
     "C5 G4 C5*2 | C5 G4 C5*2"},
    {"odetojoy", "Ode to Joy", "L. van Beethoven", 108.0f,
     "E5 E5 F5 G5 | G5 F5 E5 D5 | C5 C5 D5 E5 | E5*1.5 D5*0.5 D5*2 | "
     "E5 E5 F5 G5 | G5 F5 E5 D5 | C5 C5 D5 E5 | D5*1.5 C5*0.5 C5*2 | "
     "D5 D5 E5 C5 | D5 E5*0.5 F5*0.5 E5 C5 | D5 E5*0.5 F5*0.5 E5 D5 | C5 D5 G4*2 | "
     "E5 E5 F5 G5 | G5 F5 E5 D5 | C5 C5 D5 E5 | D5*1.5 C5*0.5 C5*2"},
    {"jinglebells", "Jingle Bells", "J. L. Pierpont", 120.0f,
     "E5 E5 E5*2 | E5 E5 E5*2 | E5 G5 C5*1.5 D5*0.5 | E5*4 | "
     "F5 F5 F5*1.5 F5*0.5 | F5 E5 E5 E5*0.5 E5*0.5 | E5 D5 D5 E5 | D5*2 G5*2 | "
     "E5 E5 E5*2 | E5 E5 E5*2 | E5 G5 C5*1.5 D5*0.5 | E5*4 | "
     "F5 F5 F5*1.5 F5*0.5 | F5 E5 E5 E5*0.5 E5*0.5 | G5 G5 F5 D5 | C5*4"},
    {"canon", "Canon in D", "J. Pachelbel", 84.0f,
     "F#5*2 E5*2 | D5*2 C#5*2 | B4*2 A4*2 | B4*2 C#5*2 | "
     "D5 C#5 B4 A4 | G4 F#4 G4 E4 | D4 F#4 A4 G4 | F#4 D4 F#4 E4 | "
     "D4 B3 D4 A4 | G4 B4 A4 G4 | F#4*2 E4*2 | D4*4"},
    {"furelise", "Fur Elise", "L. van Beethoven", 120.0f,
     "E5*0.5 D#5*0.5 | E5*0.5 D#5*0.5 E5*0.5 B4*0.5 D5*0.5 C5*0.5 | "
     "A4 R*0.5 C4*0.5 E4*0.5 A4*0.5 | B4 R*0.5 E4*0.5 G#4*0.5 B4*0.5 | "
     "C5 R*0.5 E4*0.5 E5*0.5 D#5*0.5 | E5*0.5 D#5*0.5 E5*0.5 B4*0.5 D5*0.5 C5*0.5 | "
     "A4 R*0.5 C4*0.5 E4*0.5 A4*0.5 | B4 R*0.5 E4*0.5 C5*0.5 B4*0.5 | A4 R E5*0.5 D#5*0.5 | "
     "E5*0.5 D#5*0.5 E5*0.5 B4*0.5 D5*0.5 C5*0.5 | "
     "A4 R*0.5 C4*0.5 E4*0.5 A4*0.5 | B4 R*0.5 E4*0.5 G#4*0.5 B4*0.5 | "
     "C5 R*0.5 E4*0.5 E5*0.5 D#5*0.5 | E5*0.5 D#5*0.5 E5*0.5 B4*0.5 D5*0.5 C5*0.5 | "
     "A4 R*0.5 C4*0.5 E4*0.5 A4*0.5 | B4 R*0.5 E4*0.5 C5*0.5 B4*0.5 | A4*3"},
};

static_assert(sizeof(kSongs) / sizeof(kSongs[0]) == kSongCount, "kSongCount is the songs' count");

bool parseBeats(const std::string& s, float& out) {
    if (s.empty()) return false;
    const size_t slash = s.find('/');
    char* end = nullptr;
    if (slash != std::string::npos) {
        const std::string a = s.substr(0, slash), b = s.substr(slash + 1);
        const long n = std::strtol(a.c_str(), &end, 10);
        if (a.empty() || *end) return false;
        const long d = std::strtol(b.c_str(), &end, 10);
        if (b.empty() || *end || d <= 0) return false;
        out = static_cast<float>(n) / static_cast<float>(d);
    } else {
        out = std::strtof(s.c_str(), &end);
        if (*end) return false;
    }
    return out > 0.0f && out <= 16.0f;
}

bool parseToken(const std::string& tok, Note& n) {
    size_t at = 0;
    const char letter = tok[at++];
    std::string len;
    const size_t star = tok.find('*');
    const std::string head = tok.substr(0, star);
    if (star != std::string::npos) len = tok.substr(star + 1);
    n.beats = 1.0f;
    if (star != std::string::npos && !parseBeats(len, n.beats)) return false;
    if (letter == 'R') {
        n.midi = kRest;
        return head.size() == 1;
    }
    static const int kSemitone[7] = {9, 11, 0, 2, 4, 5, 7};   // A B C D E F G
    if (letter < 'A' || letter > 'G') return false;
    int semi = kSemitone[letter - 'A'];
    if (at < head.size() && (head[at] == '#' || head[at] == 'b')) semi += head[at++] == '#' ? 1 : -1;
    if (at + 1 != head.size() || head[at] < '0' || head[at] > '8') return false;
    n.midi = (head[at] - '0' + 1) * 12 + semi;
    return n.midi >= 0 && n.midi <= 127;
}

} // namespace

const Song* songs() { return kSongs; }
int songCount() { return kSongCount; }

bool parseMelody(const char* text, std::vector<Note>& out, std::string* err) {
    out.clear();
    std::istringstream in(text ? text : "");
    std::string tok;
    int index = 0;
    while (in >> tok) {
        ++index;
        if (tok == "|") continue;
        Note n{};
        if (!parseToken(tok, n)) {
            if (err) *err = "token " + std::to_string(index) + " \"" + tok + "\"";
            out.clear();
            return false;
        }
        out.push_back(n);
    }
    const bool anyNote = std::any_of(out.begin(), out.end(), [](const Note& n) { return n.midi != kRest; });
    if (!anyNote && err) *err = "no notes";
    return anyNote;
}

std::vector<Tile> buildTiles(const std::vector<Note>& notes) {
    std::vector<Tile> tiles;
    int lo = 127, hi = 0;
    for (const Note& n : notes) {
        if (n.midi == kRest) continue;
        lo = std::min(lo, n.midi);
        hi = std::max(hi, n.midi);
    }
    float beat = 0.0f;
    int lane = 0, prevMidi = -1;
    for (const Note& n : notes) {
        if (n.midi == kRest) {
            beat += n.beats;
            continue;
        }
        if (prevMidi < 0) {
            lane = (n.midi - lo) * Game::kLanes / (hi - lo + 1);   // the first by its height in the song
        } else {
            const int diff = n.midi - prevMidi;
            int step;
            if (diff > 0) step = diff >= 5 ? 2 : 1;
            else if (diff < 0) step = diff <= -5 ? -2 : -1;
            else step = lane <= 1 ? 1 : -1;   // a repeat steps aside, towards the middle
            int to = lane + step;
            if (to < 0 || to >= Game::kLanes) to = lane - step;   // off the edge: the other way
            lane = std::clamp(to, 0, Game::kLanes - 1);
        }
        prevMidi = n.midi;
        tiles.push_back({n.midi, beat, n.beats, static_cast<uint8_t>(lane), n.beats >= kHoldBeats});
        beat += n.beats;
    }
    return tiles;
}

float levelTempoScale(int level) {
    level = std::clamp(level, 1, kMaxLevel);
    return 1.0f + 0.2f * static_cast<float>(level - 1);
}

/* ── Game ────────────────────────────────────────────────────────────── */

bool Game::load(const Song& song, int level) {
    song_ = &song;
    level_ = std::clamp(level, 1, kMaxLevel);
    bpm_ = song.bpm * levelTempoScale(level_);
    std::vector<Note> notes;
    const bool ok = parseMelody(song.melody, notes);
    tiles_ = ok ? buildTiles(notes) : std::vector<Tile>{};
    restart();
    return ok;
}

void Game::restart() {
    done_.assign(tiles_.size(), 0);
    phase_ = tiles_.empty() ? Phase::Failed : Phase::Ready;
    fail_ = Fail::None;
    failLane_ = failTile_ = -1;
    beat_ = tiles_.empty() ? 0.0f : tiles_.front().beat;
    next_ = 0;
    holding_ = -1;
    lastHit_ = -1;
    lastGrade_ = Grade::Perfect;
    score_ = combo_ = bestCombo_ = hits_ = perfects_ = 0;
}

uint32_t Game::step(float dt) {
    if (phase_ != Phase::Playing || dt <= 0.0f) return 0;
    beat_ += dt * bpm_ / 60.0f;
    uint32_t ev = 0;
    if (holding_ >= 0) {
        const Tile& h = tiles_[static_cast<size_t>(holding_)];
        if (beat_ >= h.beat + h.beats - kHoldGraceBeats) ev |= endHold(true);
    }
    if (phase_ == Phase::Playing && next_ < static_cast<int>(tiles_.size())) {
        const Tile& t = tiles_[static_cast<size_t>(next_)];
        if (beat_ - t.beat > kLateBeats) ev |= failWith(Fail::Missed, t.lane);
    }
    return ev | checkCleared();
}

uint32_t Game::press(int lane) {
    if (lane < 0 || lane >= kLanes || next_ >= static_cast<int>(tiles_.size())) return 0;
    const Tile& t = tiles_[static_cast<size_t>(next_)];
    if (phase_ == Phase::Ready) {
        if (lane != t.lane) return 0;   // only the START tile starts the song
        phase_ = Phase::Playing;
        return kEvStart | hitNext(Grade::Perfect);
    }
    if (phase_ != Phase::Playing) return 0;
    if (holding_ >= 0 && tiles_[static_cast<size_t>(holding_)].lane == lane) return 0;   // another pad of the held column
    const float d = beat_ - t.beat;   // > 0: late
    if (lane != t.lane) return failWith(Fail::WrongPad, lane);
    if (d < -kEarlyBeats) return failWith(Fail::TooEarly, lane);
    return hitNext(std::fabs(d) <= kPerfectBeats ? Grade::Perfect : Grade::Good);
}

uint32_t Game::release(int lane) {
    if (phase_ != Phase::Playing || holding_ < 0) return 0;
    if (tiles_[static_cast<size_t>(holding_)].lane != lane) return 0;
    return endHold(false);
}

uint32_t Game::hitNext(Grade g) {
    const int i = next_++;
    const Tile& t = tiles_[static_cast<size_t>(i)];
    done_[static_cast<size_t>(i)] = 1;
    lastHit_ = i;
    lastGrade_ = g;
    ++hits_;
    if (g == Grade::Perfect) {
        ++perfects_;
        ++combo_;
        bestCombo_ = std::max(bestCombo_, combo_);
    } else {
        combo_ = 0;
    }
    score_ += ((g == Grade::Perfect ? 100 : 50) + 5 * std::min(combo_, 40)) * level_;
    if (t.hold) holding_ = i;
    return kEvHit | checkCleared();
}

uint32_t Game::endHold(bool through) {
    const Tile& h = tiles_[static_cast<size_t>(holding_)];
    holding_ = -1;
    if (!through) return kEvHoldBroken | checkCleared();
    score_ += 50 * static_cast<int>(std::lround(h.beats)) * level_;
    return kEvHoldDone | checkCleared();
}

uint32_t Game::failWith(Fail f, int lane) {
    phase_ = Phase::Failed;
    fail_ = f;
    failLane_ = lane;
    failTile_ = next_;
    holding_ = -1;
    return kEvFail;
}

uint32_t Game::checkCleared() {
    if (phase_ != Phase::Playing || next_ < static_cast<int>(tiles_.size()) || holding_ >= 0) return 0;
    phase_ = Phase::Cleared;
    return kEvCleared;
}

int Game::rowOf(float ahead) {
    if (ahead < -kLateBeats) return -1;
    if (ahead < kEarlyBeats) return 0;
    const int r = 1 + static_cast<int>((ahead - kEarlyBeats) / kBandBeats);
    return r < kRows ? r : -1;
}

void Game::padRoles(PadRole out[16]) const {
    for (int i = 0; i < 16; ++i) out[i] = PadRole::Off;
    if (phase_ != Phase::Ready && phase_ != Phase::Playing) return;
    constexpr float kTop = kEarlyBeats + (kRows - 1) * kBandBeats;   // where the top row ends
    constexpr float kBodyGap = 0.25f;                                 // a body stops short of the next head
    auto paint = [&](int row, int lane, PadRole role) {
        PadRole& p = out[row * kLanes + lane];
        if (p == PadRole::Off || p == PadRole::Body) p = role;   // heads win over bodies
    };
    const int first = holding_ >= 0 ? holding_ : next_;
    for (int i = first; i < static_cast<int>(tiles_.size()); ++i) {
        const Tile& t = tiles_[static_cast<size_t>(i)];
        const float head = t.beat - beat_;
        if (head >= kTop) break;
        const bool held = i == holding_;
        if (done_[static_cast<size_t>(i)] && !held) continue;
        if (t.hold) {
            const float lo = std::max(head, -kLateBeats);
            const float tail = std::min(head + t.beats - kBodyGap, kTop - 0.001f);
            const int rlo = rowOf(lo), rhi = rowOf(tail);
            for (int r = rlo; r >= 0 && r <= rhi; ++r) paint(r, t.lane, held ? PadRole::Held : PadRole::Body);
        }
        if (held) continue;
        const int r = rowOf(head);
        if (r < 0) continue;
        PadRole& p = out[r * kLanes + t.lane];
        p = (i == next_ && r == 0) ? PadRole::HitNow : PadRole::Head;
    }
}

/* ── Progress ────────────────────────────────────────────────────────── */

Progress::Progress() : songs_(static_cast<size_t>(songCount())) {}

bool Progress::songUnlocked(int song) const {
    if (song <= 0) return true;
    const SongProgress& prev = of(song - 1);
    return std::any_of(prev.cleared, prev.cleared + kMaxLevel, [](bool c) { return c; });
}

int Progress::bestOf(int song) const {
    const SongProgress& p = of(song);
    return *std::max_element(p.best, p.best + kMaxLevel);
}

bool Progress::record(int song, int level, int score, bool cleared) {
    if (song < 0 || song >= songCount() || level < 1 || level > kMaxLevel) return false;
    SongProgress& p = of(song);
    bool changed = false;
    if (score > p.best[level - 1]) {
        p.best[level - 1] = score;
        changed = true;
    }
    if (cleared && !p.cleared[level - 1]) {
        p.cleared[level - 1] = true;
        changed = true;
    }
    if (cleared && level == p.unlocked && level < kMaxLevel) {
        p.unlocked = level + 1;
        changed = true;
    }
    return changed;
}

std::string Progress::serialize() const {
    std::string out = "# Piano Tiles: song, levels open, best score of levels 1-5, levels cleared (bits)\n";
    char line[160];
    for (int s = 0; s < songCount(); ++s) {
        const SongProgress& p = of(s);
        int mask = 0;
        for (int l = 0; l < kMaxLevel; ++l)
            if (p.cleared[l]) mask |= 1 << l;
        std::snprintf(line, sizeof line, "%s %d %d %d %d %d %d %d\n", songs()[s].id, p.unlocked, p.best[0],
                      p.best[1], p.best[2], p.best[3], p.best[4], mask);
        out += line;
    }
    return out;
}

void Progress::parse(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string id;
        SongProgress p;
        int mask = 0;
        if (!(ls >> id >> p.unlocked)) continue;
        bool ok = true;
        for (int& b : p.best) ok = ok && static_cast<bool>(ls >> b) && b >= 0;
        if (!ok || !(ls >> mask) || p.unlocked < 1 || p.unlocked > kMaxLevel) continue;
        for (int l = 0; l < kMaxLevel; ++l) p.cleared[l] = (mask >> l) & 1;
        for (int s = 0; s < songCount(); ++s)
            if (id == songs()[s].id) songs_[static_cast<size_t>(s)] = p;
    }
}

} // namespace pianotiles
