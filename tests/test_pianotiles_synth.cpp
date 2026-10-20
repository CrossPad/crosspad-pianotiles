// SPDX-License-Identifier: GPL-3.0-or-later
// Host test of the synth node (needs only crosspad-core's IAudioNode header):
//   c++ -std=c++17 -Wall -Wextra -I src -I <crosspad-core>/include tests/test_pianotiles_synth.cpp -o /tmp/pt_synth && /tmp/pt_synth
#include "PianoTilesSynth.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int s_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++s_failures;
}

constexpr uint32_t kRate = 48000, kBlock = 256;

/// Peak of the left channel over @p ms, rendered in blocks as the audio module does.
float render(pianotiles::Synth& s, uint32_t ms) {
    std::vector<float> buf(kBlock * 2);
    float peak = 0.0f;
    for (uint32_t done = 0; done < kRate * ms / 1000; done += kBlock) {
        std::fill(buf.begin(), buf.end(), 0.0f);
        s.mix(buf.data(), kBlock);
        for (uint32_t i = 0; i < kBlock; ++i) peak = std::max(peak, std::fabs(buf[i * 2]));
    }
    return peak;
}

} // namespace

int main() {
    pianotiles::Synth s;
    s.onPrepare(kBlock, kRate);
    check(render(s, 50) == 0.0f, "silent with no note");
    s.noteOn(69, 127, 400);
    const float attack = render(s, 100);
    const float later = render(s, 250);
    check(attack > 0.1f && attack < 1.0f, "a note sounds, below full scale");
    check(later < attack, "it decays");
    render(s, 450);   // 400 ms of note, then a release of ~30 ms a 1/e
    check(s.sounding() == 0 && render(s, 50) == 0.0f, "released at its length, then silent");

    s.noteOn(60, 100, 5000);
    render(s, 100);
    s.noteOff(60);
    render(s, 300);
    check(s.sounding() == 0, "a note off ends a long note early");

    s.noteOn(72, 100, 300, 200);
    check(render(s, 150) == 0.0f && render(s, 150) > 0.05f, "a delayed note waits, then sounds");
    s.allOff();
    render(s, 300);
    check(s.sounding() == 0, "all off silences everything");

    for (int i = 0; i < 12; ++i) s.noteOn(60 + i, 100, 1000);
    const float chord = render(s, 50);
    check(s.sounding() <= 8 && chord > 0.0f && std::isfinite(chord), "12 notes on 8 voices: the quietest are taken");

    pianotiles::Synth unprepared;   // no audio thread: the ring fills, nothing breaks
    for (int i = 0; i < 100; ++i) unprepared.noteOn(60, 100, 100);
    check(unprepared.sounding() == 0, "an undrained ring drops notes");

    std::printf("%s\n", s_failures ? "FAILED" : "all passed");
    return s_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
