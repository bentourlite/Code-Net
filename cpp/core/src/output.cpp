#include "codenet/output.hpp"

#include <algorithm>
#include <cmath>

#include "codenet/ltc.hpp"

namespace codenet {

void LtcRenderer::render(int64_t blockStartNs, float* out, int count) {
    // Each sample takes the frame covering the middle of its sample period, which rounds
    // frame starts to the nearest sample without a sample being skipped or repeated.
    const int64_t halfSampleNs = kNsPerSecond / (2 * int64_t(sampleRate_));
    int i = 0;
    while (i < count) {
        const int64_t t = sampleTime(blockStartNs, i) + halfSampleNs;
        const auto frame = frames_(t);
        if (!frame) {
            // While silent, look again every half millisecond so a new frame starts promptly.
            const int step = std::max(1, std::min(count - i, sampleRate_ / 2000));
            std::fill(out + i, out + i + step, 0.0f);
            i += step;
            continue;
        }
        if (!cachedFrame_ || !(cachedFrame_->timecode == frame->timecode && cachedFrame_->userBits == frame->userBits)) {
            waveLength_ = std::min(ltcEncodeFrameInto(frame->timecode, sampleRate_, frame->userBits, wave_.data(),
                                                      kMaxFrameSamples),
                                   kMaxFrameSamples);
            cachedFrame_ = frame;
        }
        const int64_t offset = (t - frame->startNs) * sampleRate_ / kNsPerSecond;
        if (offset >= waveLength_) {
            out[i++] = wave_[size_t(waveLength_ - 1)] * level_;
            continue;
        }
        const int n = std::min(waveLength_ - int(offset), count - i);
        for (int k = 0; k < n; ++k) out[i + k] = wave_[size_t(offset + k)] * level_;
        i += n;
    }
}

namespace mtc {
namespace {
uint8_t rateCode(int rate) { return rate == 25 ? 1 : 3; }
}  // namespace

std::array<uint8_t, 2> quarterFrame(const Timecode& tc, int piece) {
    int h, m, s, f;
    tc.toHMSF(h, m, s, f);
    const int values[4] = {f, s, m, h};
    const int value = values[piece / 2];
    int nibble = piece % 2 == 0 ? value & 0x0F : value >> 4;
    if (piece == 7) nibble = (rateCode(tc.rate) << 1) | (h >> 4 & 1);
    return {0xF1, uint8_t((piece << 4) | nibble)};
}

std::array<uint8_t, 10> fullFrame(const Timecode& tc) {
    int h, m, s, f;
    tc.toHMSF(h, m, s, f);
    return {0xF0, 0x7F, 0x7F, 0x01, 0x01, uint8_t((rateCode(tc.rate) << 5) | h), uint8_t(m), uint8_t(s), uint8_t(f),
            0xF7};
}

std::vector<Message> messagesForFrame(const Timecode& tc, const std::optional<Timecode>& previous) {
    std::vector<Message> out;
    const bool continuous = previous && tc.follows(*previous);
    if (!continuous) {
        auto ff = fullFrame(tc);
        out.push_back({0.0, std::vector<uint8_t>(ff.begin(), ff.end())});
    }
    if (tc.frames % 2 == 0) {
        for (int q = 0; q < 4; ++q) {
            auto m = quarterFrame(tc, q);
            out.push_back({q / 4.0, {m[0], m[1]}});
        }
    } else if (continuous) {
        for (int q = 0; q < 4; ++q) {
            auto m = quarterFrame(*previous, q + 4);
            out.push_back({q / 4.0, {m[0], m[1]}});
        }
    }
    return out;
}
}  // namespace mtc

}  // namespace codenet
