// Turns a receiver's frame schedule into LTC audio or MIDI timecode (MTC).
// Ports of python/codenet/receiver/render.py and mtc.py.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "receiver.hpp"

namespace codenet {

// Fills audio blocks with LTC so each frame starts on its scheduled sample.
// Real-time safe: no allocation or locking in render().
class LtcRenderer {
public:
    using FrameSource = std::function<std::optional<ScheduledFrame>(int64_t)>;
    static constexpr int kMaxFrameSamples = 192000 / 25;

    LtcRenderer(FrameSource frames, int sampleRate, float level = 0.5f)
        : frames_(std::move(frames)), sampleRate_(sampleRate), level_(level) {}

    // `blockStartNs` is the local clock time at which the block's first sample reaches
    // the output, so the device's own buffering is compensated for.
    void render(int64_t blockStartNs, float* out, int count);

    void setLevel(float level) { level_ = level; }
    int sampleRate() const { return sampleRate_; }

private:
    int64_t sampleTime(int64_t blockStartNs, int index) const {
        return blockStartNs + int64_t(index) * kNsPerSecond / sampleRate_;
    }

    FrameSource frames_;
    int sampleRate_;
    float level_;
    std::optional<ScheduledFrame> cachedFrame_;
    std::array<float, kMaxFrameSamples> wave_{};
    int waveLength_ = 0;
};

namespace mtc {
// Quarter-frame message `piece` (0-7) describing `tc`.
std::array<uint8_t, 2> quarterFrame(const Timecode& tc, int piece);
// Full-frame SysEx, sent when the position jumps or output starts.
std::array<uint8_t, 10> fullFrame(const Timecode& tc);

struct Message {
    double fraction = 0;  // of the frame, when to send it
    std::vector<uint8_t> bytes;
};

// MTC messages for one output frame. Eight quarter frames span two frames and describe
// the frame at which piece 0 was sent, so even frames send pieces 0-3 of their own time
// and odd frames send pieces 4-7 of the previous frame's time. A jump sends a full frame.
std::vector<Message> messagesForFrame(const Timecode& tc, const std::optional<Timecode>& previous);
}  // namespace mtc

}  // namespace codenet
