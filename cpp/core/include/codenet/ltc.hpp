// LTC (SMPTE 12M) encoder and real-time-safe streaming decoder, 25 and 30 fps.
// Mirrors python/codenet/ltc.py; both are tested against the same rules.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "timecode.hpp"

namespace codenet {

constexpr int kBitsPerFrame = 80;

// The 80 bits of one frame in transmit order.
std::array<uint8_t, kBitsPerFrame> ltcFrameBits(const Timecode& tc, uint32_t userBits = 0);

// One frame of LTC audio at full scale; the level before the frame is taken as -1.
std::vector<float> ltcEncodeFrame(const Timecode& tc, int sampleRate, uint32_t userBits = 0);

struct DecodedFrame {
    Timecode timecode;
    uint32_t userBits = 0;
    int64_t startSample = 0;  // absolute sample index of the frame's first transition
    int64_t endSample = 0;    // absolute sample index where the next frame starts
};

// Feed blocks of samples; each complete frame is reported through the callback with
// the absolute sample position where it started. No allocation after construction.
// The switching threshold follows the signal level (a fraction of the recent peak),
// so LTC is read from full scale down to about -50 dBFS.
class LtcDecoder {
public:
    explicit LtcDecoder(double sampleRate, float minThreshold = 0.002f);

    template <typename Callback>
    void process(const float* samples, int count, Callback&& onFrame) {
        for (int i = 0; i < count; ++i) {
            const float x = samples[i];
            const float level = x < 0 ? -x : x;
            envelope_ = level > envelope_ ? level : envelope_ * decay_;
            const float threshold = envelope_ * 0.3f > minThreshold_ ? envelope_ * 0.3f : minThreshold_;
            int next = state_;
            if (x > threshold) next = 1;
            else if (x < -threshold) next = -1;
            if (next != state_) {
                if (state_ != 0) {
                    DecodedFrame frame;
                    if (onTransition(position_ + i, frame)) onFrame(frame);
                }
                state_ = next;
            }
        }
        position_ += count;
    }

    int64_t position() const { return position_; }
    float envelope() const { return envelope_; }  // recent peak level, 0..1
    void reset();

private:
    bool onTransition(int64_t pos, DecodedFrame& out);
    bool push(int bit, int64_t cellStart, int64_t cellEnd, DecodedFrame& out);
    bool decodeWord(int64_t start, int64_t end, DecodedFrame& out) const;
    void resetWord() { count_ = 0; halfStart_ = -1; }

    double sampleRate_;
    float minThreshold_;
    float decay_;
    float envelope_ = 0.0f;
    double cell_;
    int state_ = 0;
    int64_t position_ = 0;
    int64_t lastTransition_ = -1;
    int64_t halfStart_ = -1;
    std::array<uint8_t, kBitsPerFrame> bits_{};
    std::array<int64_t, kBitsPerFrame> starts_{};
    int count_ = 0;  // bits held, up to 80, oldest first in a ring starting at head_
    int head_ = 0;
};

}  // namespace codenet
