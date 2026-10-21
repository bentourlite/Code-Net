// Receiving on a Windows PC: the same job as a Raspberry Pi receiver (follow a main and a
// backup host, generate fresh LTC or MTC), run by the C++ engine in cpp/core.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "codenet/device_output.hpp"
#include "codenet/rx_service.hpp"

struct ReceiverSettings {
    std::string name = "win-rx";
    std::string main, backup;
    int outputKind = 0;  // 0 audio (LTC), 1 MIDI (MTC)
    std::string deviceId;
    int channel = 0;  // 0-based audio output, -1 for all
    float level = 0.5f;
    bool multicast = true;
    bool wifi = false;
    double advanceMs = 0;

    bool operator==(const ReceiverSettings& o) const;
    bool operator!=(const ReceiverSettings& o) const { return !(*this == o); }
};

class ReceiverEngine {
public:
    // Called on the network thread when a host app reassigns this receiver or sets its offset.
    std::function<void(const std::string& main, const std::string& backup)> onAssigned;
    std::function<void(double advanceMs)> onAdvanceSet;

    ~ReceiverEngine();

    // Starts, restarts or adjusts the receiver to match. Returns an error to show, if any.
    std::optional<std::string> apply(const ReceiverSettings& settings);
    void stop();

    // On the message thread, before applying settings that carry a host's assignment: the
    // receiver already follows it, so it must not be set again (that would force a return
    // to main).
    void noteAssigned(const std::string& main, const std::string& backup) { sources_ = {main, backup}; }

    void returnToMain();
    std::optional<codenet::ScheduledFrame> frameNow() const;
    std::string statusJson() const;
    bool running() const { return service_ != nullptr; }

    static std::vector<codenet::DeviceInfo> devices() { return codenet::listOutputDevices(); }

private:
    std::unique_ptr<codenet::RxService> service_;
    std::unique_ptr<codenet::DeviceOutput> output_;
    std::string networkKey_, outputKey_;
    std::pair<std::string, std::string> sources_;
    std::optional<std::string> outputError_;
};
