// Clock offset between a receiver and a host from NTP-style exchanges. Mirrors
// python/codenet/clock.py: the sample with the smallest round trip in a recent
// window is the most trustworthy, since network delay only ever adds to it.
#pragma once

#include <array>
#include <cstdint>

namespace codenet {

struct ClockSample {
    int64_t offsetNs = 0;  // host clock minus local clock
    int64_t rttNs = 0;
};

class ClockOffsetEstimator {
public:
    static constexpr int kWindow = 16;

    ClockSample add(int64_t t1, int64_t t2, int64_t t3, int64_t t4) {
        const int64_t a = t2 - t1, b = t3 - t4;
        // Floor division by 2, matching Python's //.
        const int64_t sum = a + b;
        ClockSample s{sum >= 0 ? sum / 2 : -((-sum + 1) / 2), (t4 - t1) - (t3 - t2)};
        if (s.rttNs >= 0) {
            samples_[size_t(next_)] = s;
            next_ = (next_ + 1) % kWindow;
            if (count_ < kWindow) ++count_;
        }
        return s;
    }

    bool ready() const { return count_ > 0; }

    ClockSample best() const {
        ClockSample b = samples_[0];
        for (int i = 1; i < count_; ++i)
            if (samples_[size_t(i)].rttNs < b.rttNs) b = samples_[size_t(i)];
        return b;
    }

    int64_t hostToLocal(int64_t hostNs) const { return hostNs - best().offsetNs; }

private:
    std::array<ClockSample, kWindow> samples_{};
    int count_ = 0;
    int next_ = 0;
};

}  // namespace codenet
