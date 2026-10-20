// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file PianoTilesSynth.hpp
 * @brief Piano Tiles' instrument: eight voices of a small piano-like tone on
 *        the audio module -- four harmonics from one sine table, a 3 ms
 *        attack, a decay that is longer for low notes and faster for the
 *        upper harmonics, and a short release at the note's end or on a note
 *        off.
 *
 * noteOn()/noteOff()/allOff() are called from one thread (the LVGL thread,
 * where the game steps); they only write commands into a lock-free ring that
 * mix() drains on the audio thread. mix() allocates nothing, locks nothing,
 * logs nothing.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include <crosspad/audio/IAudioNode.hpp>

namespace pianotiles {

class Synth : public crosspad::IAudioNode {
public:
    Synth() {
        for (uint32_t i = 0; i <= kTable; ++i)
            table_[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979 * i / kTable));
    }

    const char* name() const override { return "Piano Tiles"; }
    bool isGenerator() const override { return true; }
    void onPrepare(uint32_t, uint32_t sampleRate) override { rate_.store(sampleRate, std::memory_order_relaxed); }

    /// A note of @p lenMs (then released), starting @p delayMs from now; velocity 1..127.
    void noteOn(int midi, int velocity, uint32_t lenMs, uint32_t delayMs = 0) {
        push({kOn, static_cast<uint8_t>(midi), static_cast<uint8_t>(velocity), lenMs, delayMs});
    }
    void noteOff(int midi) { push({kOff, static_cast<uint8_t>(midi), 0, 0, 0}); }
    void allOff() { push({kAllOff, 0, 0, 0, 0}); }

    /// Voices sounding at the end of the last mix() (for logs and tests).
    uint32_t sounding() const { return sounding_.load(std::memory_order_relaxed); }

    void mix(float* out, uint32_t frames) override {
        const uint32_t rate = rate_.load(std::memory_order_relaxed);
        if (rate == 0) return;
        drain(rate);
        uint32_t live = 0;
        for (Voice& v : voices_) {
            if (!v.active) continue;
            for (uint32_t i = 0; i < frames; ++i) {
                if (v.delay) {
                    --v.delay;
                    continue;
                }
                if (v.left) {
                    if (--v.left == 0) v.releasing = true;
                }
                if (v.releasing) v.rel *= relMul_;
                const float att = v.age < attack_ ? static_cast<float>(v.age) / static_cast<float>(attack_) : 1.0f;
                ++v.age;
                const float p = v.phase;
                const float s = v.e1 * sine(p) + 0.42f * v.e2 * sine(2.0f * p) + 0.18f * v.e3 * sine(3.0f * p) +
                                0.07f * v.e3 * sine(4.0f * p);
                const float x = s * v.amp * att * v.rel;
                out[i * 2 + 0] += x;
                out[i * 2 + 1] += x;
                v.phase += v.inc;
                if (v.phase >= 1.0f) v.phase -= 1.0f;
                v.e1 *= v.d1;
                v.e2 *= v.d2;
                v.e3 *= v.d3;
                if (v.e1 * v.rel < 1e-4f) {
                    v.active = false;
                    break;
                }
            }
            if (v.active) ++live;
        }
        sounding_.store(live, std::memory_order_relaxed);
    }

private:
    enum : uint8_t { kOn, kOff, kAllOff };
    struct Cmd {
        uint8_t type;
        uint8_t midi;
        uint8_t vel;
        uint32_t lenMs;
        uint32_t delayMs;
    };
    struct Voice {
        bool active = false;
        bool releasing = false;
        uint8_t midi = 0;
        uint32_t delay = 0;   ///< samples before it starts
        uint32_t left = 0;    ///< samples until its release; 0 = held until a note off
        uint32_t age = 0;
        float phase = 0.0f, inc = 0.0f, amp = 0.0f;
        float e1 = 0.0f, e2 = 0.0f, e3 = 0.0f;   ///< harmonic envelopes
        float d1 = 1.0f, d2 = 1.0f, d3 = 1.0f;   ///< their per-sample decay
        float rel = 1.0f;
    };

    static constexpr uint32_t kRing = 32;
    static constexpr unsigned kVoices = 8;
    static constexpr uint32_t kTable = 1024;
    static constexpr float kLevel = 0.22f;

    float sine(float cycles) const {
        cycles -= static_cast<float>(static_cast<int>(cycles));
        const float x = cycles * kTable;
        const uint32_t i = static_cast<uint32_t>(x);
        const float f = x - static_cast<float>(i);
        return table_[i] + (table_[i + 1] - table_[i]) * f;
    }

    void push(const Cmd& c) {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= kRing) return;   // no audio thread draining
        ring_[head % kRing] = c;
        head_.store(head + 1, std::memory_order_release);
    }

    void drain(uint32_t rate) {
        if (rate != preparedRate_) {
            preparedRate_ = rate;
            attack_ = std::max<uint32_t>(1, rate * 3 / 1000);
            relMul_ = std::exp(-1.0f / (0.03f * static_cast<float>(rate)));
        }
        uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        for (; tail != head; ++tail) {
            const Cmd& c = ring_[tail % kRing];
            if (c.type == kAllOff) {
                for (Voice& v : voices_) v.releasing = true;
            } else if (c.type == kOff) {
                for (Voice& v : voices_)
                    if (v.active && v.midi == c.midi && !v.delay) v.releasing = true;
            } else {
                start(c, rate);
            }
        }
        tail_.store(tail, std::memory_order_release);
    }

    void start(const Cmd& c, uint32_t rate) {
        for (Voice& v : voices_)   // a repeated note lets the old one go
            if (v.active && v.midi == c.midi && !v.delay) v.releasing = true;
        Voice* pick = nullptr;
        for (Voice& v : voices_)
            if (!v.active) {
                pick = &v;
                break;
            }
        if (!pick) {   // all busy: the quietest goes
            pick = &voices_[0];
            for (Voice& v : voices_)
                if (v.e1 * v.rel < pick->e1 * pick->rel) pick = &v;
        }
        Voice& v = *pick;
        const float hz = 440.0f * std::pow(2.0f, (static_cast<float>(c.midi) - 69.0f) / 12.0f);
        const float tau = std::clamp(2.2f * std::pow(261.6f / hz, 0.6f), 0.35f, 3.0f);   // seconds to 1/e
        v.active = true;
        v.releasing = false;
        v.midi = c.midi;
        v.delay = static_cast<uint32_t>(static_cast<uint64_t>(c.delayMs) * rate / 1000u);
        v.left = static_cast<uint32_t>(static_cast<uint64_t>(c.lenMs) * rate / 1000u) + 1u;
        v.age = 0;
        v.phase = 0.0f;
        v.inc = hz / static_cast<float>(rate);
        v.amp = kLevel * (0.45f + 0.55f * static_cast<float>(c.vel) / 127.0f);
        v.e1 = v.e2 = v.e3 = 1.0f;
        v.d1 = std::exp(-1.0f / (tau * static_cast<float>(rate)));
        v.d2 = v.d1 * v.d1;
        v.d3 = v.d2 * v.d1;
        v.rel = 1.0f;
    }

    Cmd ring_[kRing]{};
    std::atomic<uint32_t> head_{0}, tail_{0};
    std::atomic<uint32_t> rate_{0};
    std::atomic<uint32_t> sounding_{0};
    uint32_t preparedRate_ = 0;
    uint32_t attack_ = 1;
    float relMul_ = 0.999f;
    float table_[kTable + 1];
    Voice voices_[kVoices];
};

} // namespace pianotiles
