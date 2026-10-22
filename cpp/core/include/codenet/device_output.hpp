// Device outputs for the desktop receivers: LTC through Core Audio or WASAPI, MTC through
// Core MIDI or WinMM. Implemented in src/mac_io.cpp and src/win_io.cpp; elsewhere opening
// one reports that it is unsupported.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "rx_service.hpp"

namespace codenet {

struct DeviceInfo {
    std::string id, name;
    int kind = 0;  // 0 audio, 1 MIDI
    int channels = 0;
};

std::vector<DeviceInfo> listOutputDevices();

class DeviceOutput {
public:
    virtual ~DeviceOutput() = default;
    virtual void setLevel(float) {}
    virtual std::string description() const = 0;  // for the status report, e.g. "mif4: MIF4 Port 1"
};

// LTC on an audio device. `channel` is 0-based, or -1 for every output channel.
std::unique_ptr<DeviceOutput> openAudioOutput(RxService& service, const std::string& deviceUid, int channel,
                                              float level, std::string& error);
// MTC to a MIDI destination (a Rosendahl MIF4 turns it into LTC).
std::unique_ptr<DeviceOutput> openMidiOutput(RxService& service, const std::string& endpointId, std::string& error);

}  // namespace codenet
