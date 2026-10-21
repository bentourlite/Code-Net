// Source selection, failover and frame scheduling for one receiver. A port of
// python/codenet/receiver/core.py; pure logic, every call takes the local time.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "clock.hpp"
#include "protocol.hpp"
#include "timecode.hpp"

namespace codenet {

constexpr int64_t kNsPerSecond = 1'000'000'000;

// Start of the frame `count` frames after a reference, without rounding drift.
inline int64_t frameOffsetNs(int64_t count, int rate) { return count * kNsPerSecond / rate; }

enum class Role { Main, Backup };
const char* roleName(Role role);

struct SourceState {
    SourceId source{};
    Role role = Role::Main;
    ClockOffsetEstimator clock;
    bool hasPacket = false;
    FramePacket lastPacket;
    int64_t lastRxNs = 0;
    std::optional<int64_t> validSinceNs;
    // Latest playing frame, converted to the local clock.
    bool hasAnchor = false;
    Timecode anchor;
    int64_t anchorStartNs = 0;
};

struct ScheduledFrame {
    Timecode timecode;
    uint32_t userBits = 0;
    int64_t startNs = 0;  // local time at which the frame's first sample must be output
    bool operator==(const ScheduledFrame& o) const {
        return timecode == o.timecode && userBits == o.userBits && startNs == o.startNs;
    }
};

// What the output needs to generate frames, copied out of the receiver so audio
// threads never touch its state.
struct Schedule {
    bool active = false;
    Timecode anchor;
    int64_t anchorStartNs = 0;
    uint32_t userBits = 0;
    int maxPredictFrames = 2;
    int64_t outputAdvanceNs = 0;

    std::optional<ScheduledFrame> frameAt(int64_t localNs) const;
};

struct Switch {
    int64_t atNs = 0;
    std::optional<Role> from, to;
    std::string reason;
};

struct ReceiverConfig {
    std::optional<SourceId> main;  // none until a host app assigns one
    std::optional<SourceId> backup;
    // A source is lost after this many frame periods without a packet; keep it no
    // larger than maxPredictFrames so failover lands before the prediction runs out.
    int lossTimeoutFrames = 2;
    // Frames past the newest received frame that may be generated (no freewheel).
    int maxPredictFrames = 2;
    int64_t returnToMainAfterNs = 10 * kNsPerSecond;
    bool autoReturn = true;
    int64_t outputAdvanceNs = 0;  // positive = earlier
};

class Receiver {
public:
    explicit Receiver(ReceiverConfig config);

    void onFrame(const FramePacket& packet, int64_t nowNs);
    void onSync(const SourceId& source, int64_t t1, int64_t t2, int64_t t3, int64_t t4);
    void setSources(std::optional<SourceId> main, std::optional<SourceId> backup, int64_t nowNs);
    void requestReturnToMain() { returnRequested_ = true; }

    bool isValid(const SourceState& state, int64_t nowNs) const;
    void update(int64_t nowNs);

    Schedule schedule() const;
    std::optional<ScheduledFrame> frameAt(int64_t localNs) const { return schedule().frameAt(localNs); }

    const ReceiverConfig& config() const { return config_; }
    const std::vector<SourceState>& sources() const { return sources_; }
    SourceState* find(const SourceId& source);
    const SourceState* active() const;
    const std::vector<Switch>& switches() const { return switches_; }

private:
    ReceiverConfig config_;
    std::vector<SourceState> sources_;  // main first, then backup
    std::optional<SourceId> active_;
    std::vector<Switch> switches_;
    bool returnRequested_ = false;
};

}  // namespace codenet
