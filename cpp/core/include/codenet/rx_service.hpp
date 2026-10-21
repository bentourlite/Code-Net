// The receiver's network side: frame packets in, clock sync out, status reports to the
// hosts it hears, and main/backup assignments from host apps. A port of
// python/codenet/receiver/service.py plus the status loop in main.py. POSIX sockets.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "receiver.hpp"

namespace codenet {

// The local clock all timing uses. On macOS it is the audio and MIDI host clock.
int64_t monotonicNs();

struct RxServiceConfig {
    std::string name = "mac";
    std::string group = kMulticastGroup;  // multicast group, or "none" for unicast only
    uint16_t port = kFramePort;
    bool wifi = false;  // reported in status only; the caller widens the windows
};

class RxService {
public:
    using AssignCallback = std::function<void(const std::string& main, const std::string& backup)>;
    using AdvanceCallback = std::function<void(double advanceMs)>;

    RxService(RxServiceConfig config, ReceiverConfig receiverConfig);
    ~RxService();

    // Opens the socket and starts the network thread. Returns an error message on failure.
    std::optional<std::string> start();
    void stop();

    // Real-time safe: the frame to output at `localNs`, from a lock-free copy of the schedule.
    std::optional<ScheduledFrame> frameAt(int64_t localNs) const;

    void setSources(const std::string& main, const std::string& backup);
    void returnToMain();
    void setDevice(const std::string& device);
    void setAssignCallback(AssignCallback cb);
    // Output advance in ms (positive = earlier), set here or by a host app's Calibrate.
    void setAdvance(double ms);
    double advanceMs() const;
    // Called on the network thread when a host app sets the advance.
    void setAdvanceCallback(AdvanceCallback cb);

    // The latest status as JSON (the same object sent to hosts, plus a few local fields).
    std::string statusJson() const;

    // For tests: handle one datagram as if it arrived from `host`.
    void handle(const uint8_t* data, size_t size, const std::string& host, int64_t nowNs);
    std::string buildStatus(int64_t nowNs);

private:
    void run();
    void sendTo(const uint8_t* data, size_t size, const std::string& host, uint16_t port);
    void sendSync(int64_t nowNs);
    void publishSchedule();
    std::string describeIdle(int64_t nowNs) const;

    RxServiceConfig config_;
    SourceId id_;
    int fd_ = -1;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    int64_t startedNs_ = 0;

    mutable std::mutex lock_;
    Receiver receiver_;
    std::map<SourceId, std::string> hostAddresses_;
    std::map<std::string, std::pair<std::string, int64_t>> otherHosts_;  // name -> (address, heard)
    std::map<SourceId, int64_t> lastFrameRx_, maxGapNs_;
    uint64_t packetsReceived_ = 0;
    uint32_t syncSequence_ = 0;
    std::optional<std::pair<std::string, uint32_t>> lastAssign_;
    std::string device_ = "none";
    std::string status_ = "{}";
    AssignCallback onAssign_;
    AdvanceCallback onAdvance_;
    std::optional<std::pair<std::string, uint32_t>> lastCalibrate_;
    std::optional<uint32_t> calibrateSeq_;
    std::string calibrateState_, calibrateMessage_;

    // Seqlock around the schedule copy read by the audio and MIDI threads.
    mutable std::atomic<uint32_t> scheduleSeq_{0};
    Schedule schedule_;
};

}  // namespace codenet
