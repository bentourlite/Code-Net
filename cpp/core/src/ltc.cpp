#include "codenet/ltc.hpp"

#include <cmath>

namespace codenet {
namespace {

constexpr uint8_t kSync[16] = {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1};
constexpr int kUserBitStarts[8] = {4, 12, 20, 28, 36, 44, 52, 60};

void put(std::array<uint8_t, kBitsPerFrame>& bits, int start, int width, uint32_t value) {
    for (int i = 0; i < width; ++i) bits[size_t(start + i)] = uint8_t((value >> i) & 1u);
}

int polarityBit(int rate) { return rate == 25 ? 59 : 27; }

}  // namespace

std::array<uint8_t, kBitsPerFrame> ltcFrameBits(const Timecode& tc, uint32_t userBits) {
    int h, m, s, f;
    tc.toHMSF(h, m, s, f);
    std::array<uint8_t, kBitsPerFrame> bits{};
    put(bits, 0, 4, uint32_t(f % 10));
    put(bits, 8, 2, uint32_t(f / 10));
    put(bits, 16, 4, uint32_t(s % 10));
    put(bits, 24, 3, uint32_t(s / 10));
    put(bits, 32, 4, uint32_t(m % 10));
    put(bits, 40, 3, uint32_t(m / 10));
    put(bits, 48, 4, uint32_t(h % 10));
    put(bits, 56, 2, uint32_t(h / 10));
    for (int g = 0; g < 8; ++g) put(bits, kUserBitStarts[g], 4, (userBits >> (4 * g)) & 0xFu);
    for (int i = 0; i < 16; ++i) bits[size_t(64 + i)] = kSync[i];
    int ones = 0;
    for (auto b : bits) ones += b;
    if (ones % 2) bits[size_t(polarityBit(tc.rate))] = 1;
    return bits;
}

int ltcEncodeFrameInto(const Timecode& tc, int sampleRate, uint32_t userBits, float* out, int capacity) {
    const auto bits = ltcFrameBits(tc, userBits);
    const double spf = double(sampleRate) / tc.rate;
    const int length = int(std::lround(spf));
    auto edge = [&](int half) { return int(std::lround(half * spf / (2 * kBitsPerFrame))); };
    float level = -1.0f;
    for (int i = 0; i < kBitsPerFrame; ++i) {
        level = -level;
        for (int k = edge(2 * i); k < edge(2 * i + 1) && k < capacity; ++k) out[k] = level;
        if (bits[size_t(i)]) level = -level;
        for (int k = edge(2 * i + 1); k < edge(2 * i + 2) && k < capacity; ++k) out[k] = level;
    }
    return length;
}

std::vector<float> ltcEncodeFrame(const Timecode& tc, int sampleRate, uint32_t userBits) {
    std::vector<float> out(size_t(std::lround(double(sampleRate) / tc.rate)));
    ltcEncodeFrameInto(tc, sampleRate, userBits, out.data(), int(out.size()));
    return out;
}

LtcDecoder::LtcDecoder(double sampleRate, float minThreshold)
    : sampleRate_(sampleRate),
      minThreshold_(minThreshold),
      decay_(float(std::pow(0.5, 1.0 / (0.02 * sampleRate)))),  // 20 ms half-life
      cell_(sampleRate / (kBitsPerFrame * 27.5)) {}

void LtcDecoder::reset() {
    state_ = 0;
    envelope_ = 0.0f;
    position_ = 0;
    lastTransition_ = -1;
    cell_ = sampleRate_ / (kBitsPerFrame * 27.5);
    resetWord();
}

bool LtcDecoder::onTransition(int64_t pos, DecodedFrame& out) {
    const int64_t last = lastTransition_;
    lastTransition_ = pos;
    if (last < 0) return false;
    const double interval = double(pos - last);
    if (interval > 1.5 * cell_ || interval < 0.25 * cell_) {
        // Silence, noise or a rate change: start over.
        resetWord();
        const double minCell = sampleRate_ / (kBitsPerFrame * 40.0), maxCell = sampleRate_ / (kBitsPerFrame * 20.0);
        if (interval > 0.75 * minCell && interval < 1.5 * maxCell) cell_ = interval;
        return false;
    }
    if (interval > 0.75 * cell_) {
        if (halfStart_ >= 0) {
            resetWord();
            return false;
        }
        cell_ = 0.9 * cell_ + 0.1 * interval;
        return push(0, last, pos, out);
    }
    cell_ = 0.9 * cell_ + 0.1 * 2 * interval;
    if (halfStart_ < 0) {
        halfStart_ = last;
        return false;
    }
    const int64_t start = halfStart_;
    halfStart_ = -1;
    return push(1, start, pos, out);
}

bool LtcDecoder::push(int bit, int64_t cellStart, int64_t cellEnd, DecodedFrame& out) {
    // Ring buffer of the last 80 bits.
    int index;
    if (count_ < kBitsPerFrame) {
        index = (head_ + count_) % kBitsPerFrame;
        ++count_;
    } else {
        index = head_;
        head_ = (head_ + 1) % kBitsPerFrame;
    }
    bits_[size_t(index)] = uint8_t(bit);
    starts_[size_t(index)] = cellStart;
    if (count_ < kBitsPerFrame) return false;
    for (int i = 0; i < 16; ++i)
        if (bits_[size_t((head_ + 64 + i) % kBitsPerFrame)] != kSync[i]) return false;
    const bool ok = decodeWord(starts_[size_t(head_)], cellEnd, out);
    count_ = 0;
    head_ = 0;
    return ok;
}

bool LtcDecoder::decodeWord(int64_t start, int64_t end, DecodedFrame& out) const {
    auto get = [&](int s, int width) {
        uint32_t v = 0;
        for (int i = 0; i < width; ++i) v |= uint32_t(bits_[size_t((head_ + s + i) % kBitsPerFrame)]) << i;
        return v;
    };
    const double length = double(end - start);
    const int rate = std::fabs(sampleRate_ / 25 - length) <= std::fabs(sampleRate_ / 30 - length) ? 25 : 30;
    const int f = int(get(0, 4) + 10 * get(8, 2));
    const int s = int(get(16, 4) + 10 * get(24, 3));
    const int m = int(get(32, 4) + 10 * get(40, 3));
    const int h = int(get(48, 4) + 10 * get(56, 2));
    uint32_t ub = 0;
    for (int g = 0; g < 8; ++g) ub |= get(kUserBitStarts[g], 4) << (4 * g);
    if (!Timecode::fromHMSF(h, m, s, f, rate, out.timecode)) return false;
    out.userBits = ub;
    out.startSample = start;
    out.endSample = end;
    return true;
}

}  // namespace codenet
