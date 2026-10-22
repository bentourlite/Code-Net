// Notices a host processing this track ahead of playback, as REAPER does with anticipative
// FX on tracks that aren't record-armed. Frames are stamped from the block's timestamp, so
// unless the host's timestamp allows for the lead, every frame would go out early.
#pragma once

#include <algorithm>
#include <cstdint>

class AheadDetector {
public:
    // Call once per block. `nowNs` is the clock now; `blockNs` the time the block's first sample
    // is said to play (the host's timestamp, or now if it gives none, which allows for nothing).
    void update(int64_t nowNs, int64_t blockNs, int samples, double sampleRate) {
        if (lastCallNs_ == 0 || nowNs - lastCallNs_ > kGapNs) {
            // A new run of playback: measure afresh over its first seconds, before the audio
            // and system clocks have drifted apart.
            runStartNs_ = nowNs;
            processedNs_ = 0;
            maxLeadNs_ = 0;
            hostLead0Ns_ = blockNs - nowNs;
            measuring_ = true;
        }
        lastCallNs_ = nowNs;
        if (measuring_) {
            // Processed audio minus elapsed time: about zero when called in real time, the
            // render-ahead when the host works in advance. Whatever the host's timestamps
            // already move forward (beyond their usual output latency) is allowed for.
            const int64_t lead = processedNs_ - (nowNs - runStartNs_);
            const int64_t hostAllowance = (blockNs - nowNs) - hostLead0Ns_;
            maxLeadNs_ = std::max(maxLeadNs_, lead - hostAllowance);
            if (nowNs - runStartNs_ > kMeasureNs) {
                measuring_ = false;
                ahead_ = maxLeadNs_ > kThresholdNs;
            }
        }
        processedNs_ += int64_t(double(samples) * 1e9 / sampleRate);
    }

    // True when the last run was processed ahead of playback and the host's timestamps
    // didn't allow for it; leadNs() is by how much.
    bool ahead() const { return ahead_; }
    int64_t leadNs() const { return maxLeadNs_; }

private:
    static constexpr int64_t kGapNs = 300'000'000, kMeasureNs = 3'000'000'000, kThresholdNs = 30'000'000;
    int64_t lastCallNs_ = 0, runStartNs_ = 0, processedNs_ = 0, maxLeadNs_ = 0, hostLead0Ns_ = 0;
    bool measuring_ = false, ahead_ = false;
};
