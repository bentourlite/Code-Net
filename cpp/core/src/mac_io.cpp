#include "codenet/device_output.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>

#include "codenet/output.hpp"

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#endif

namespace codenet {

#if defined(_WIN32)
// See win_io.cpp.
#elif !defined(__APPLE__)

std::vector<DeviceInfo> listOutputDevices() { return {}; }

std::unique_ptr<DeviceOutput> openAudioOutput(RxService&, const std::string&, int, float, std::string& error) {
    error = "audio output is only available on macOS";
    return nullptr;
}

std::unique_ptr<DeviceOutput> openMidiOutput(RxService&, const std::string&, std::string& error) {
    error = "MIDI output is only available on macOS";
    return nullptr;
}

#else  // __APPLE__

namespace {

constexpr AudioObjectPropertyElement kElementMain = 0;  // kAudioObjectPropertyElementMain, macOS 12+

std::string cfString(CFStringRef s) {
    if (s == nullptr) return {};
    char buf[512];
    if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return buf;
    return {};
}

template <typename T>
bool getProperty(AudioObjectID object, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope, T& out) {
    AudioObjectPropertyAddress address{selector, scope, kElementMain};
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &out) == noErr;
}

std::vector<AudioObjectID> audioDevices() {
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                       kElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr) return {};
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, ids.data()) != noErr)
        return {};
    return ids;
}

int outputChannels(AudioObjectID device) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput,
                                       kElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size == 0) return 0;
    std::vector<uint8_t> storage(size);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, list) != noErr) return 0;
    int channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) channels += int(list->mBuffers[i].mNumberChannels);
    return channels;
}

std::string deviceString(AudioObjectID device, AudioObjectPropertySelector selector) {
    CFStringRef s = nullptr;
    if (!getProperty(device, selector, kAudioObjectPropertyScopeGlobal, s) || s == nullptr) return {};
    std::string out = cfString(s);
    CFRelease(s);
    return out;
}

mach_timebase_info_data_t timebase() {
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return tb;
}

int64_t hostTicksToNs(uint64_t ticks) {
    const auto tb = timebase();
    return int64_t(__uint128_t(ticks) * tb.numer / tb.denom);
}

uint64_t nsToHostTicks(int64_t ns) {
    const auto tb = timebase();
    return ns <= 0 ? 0 : uint64_t(__uint128_t(ns) * tb.denom / tb.numer);
}

// ---- LTC on a Core Audio device (AUHAL) -------------------------------------------

class AudioOutput final : public DeviceOutput {
public:
    AudioOutput(RxService& service, AudioObjectID device, std::string name, int channel, int sampleRate,
                int64_t latencyNs, float level)
        : renderer_([&service](int64_t t) { return service.frameAt(t); }, sampleRate, level),
          device_(device), name_(std::move(name)), channel_(channel), latencyNs_(latencyNs) {}

    ~AudioOutput() override {
        if (unit_ != nullptr) {
            AudioOutputUnitStop(unit_);
            AudioUnitUninitialize(unit_);
            AudioComponentInstanceDispose(unit_);
        }
    }

    bool open(std::string& error) {
        AudioComponentDescription desc{kAudioUnitType_Output, kAudioUnitSubType_HALOutput,
                                       kAudioUnitManufacturer_Apple, 0, 0};
        AudioComponent component = AudioComponentFindNext(nullptr, &desc);
        if (component == nullptr || AudioComponentInstanceNew(component, &unit_) != noErr) {
            error = "can't create an audio output unit";
            return false;
        }
        UInt32 enable = 1, disable = 0;
        AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &enable,
                             sizeof enable);
        AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &disable,
                             sizeof disable);
        if (AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device_,
                                 sizeof device_) != noErr) {
            error = "can't use audio device " + name_;
            return false;
        }
        channels_ = std::max(1, outputChannels(device_));
        AudioStreamBasicDescription format{};
        format.mSampleRate = renderer_.sampleRate();
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
        format.mBytesPerPacket = 4;
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = 4;
        format.mChannelsPerFrame = UInt32(channels_);
        format.mBitsPerChannel = 32;
        if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format,
                                 sizeof format) != noErr) {
            error = "audio device " + name_ + " doesn't accept the output format";
            return false;
        }
        UInt32 maxFrames = 4096;
        AudioUnitSetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames,
                             sizeof maxFrames);
        scratch_.assign(8192, 0.0f);
        AURenderCallbackStruct cb{&AudioOutput::render, this};
        if (AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb,
                                 sizeof cb) != noErr ||
            AudioUnitInitialize(unit_) != noErr || AudioOutputUnitStart(unit_) != noErr) {
            error = "can't start audio on " + name_;
            return false;
        }
        return true;
    }

    void setLevel(float level) override { pendingLevel_.store(level); }

    std::string description() const override {
        return "audio: " + name_ + (channel_ < 0 ? " (all outputs)" : " (output " + std::to_string(channel_ + 1) + ")");
    }

private:
    static OSStatus render(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp* ts, UInt32, UInt32 frames,
                           AudioBufferList* io) {
        auto* self = static_cast<AudioOutput*>(ref);
        self->renderer_.setLevel(self->pendingLevel_.load(std::memory_order_relaxed));
        // The timestamp is when the first sample leaves the unit; add the device's own
        // latency to get the time it reaches the output socket.
        int64_t t = (ts->mFlags & kAudioTimeStampHostTimeValid) ? hostTicksToNs(ts->mHostTime) : monotonicNs();
        t += self->latencyNs_;
        UInt32 done = 0;
        while (done < frames) {
            const UInt32 n = std::min<UInt32>(frames - done, UInt32(self->scratch_.size()));
            const int64_t blockNs = t + int64_t(done) * kNsPerSecond / self->renderer_.sampleRate();
            self->renderer_.render(blockNs, self->scratch_.data(), int(n));
            for (UInt32 b = 0; b < io->mNumberBuffers; ++b) {
                auto* out = static_cast<float*>(io->mBuffers[b].mData);
                if (out == nullptr) continue;
                const bool wanted = self->channel_ < 0 || int(b) == self->channel_;
                for (UInt32 i = 0; i < n; ++i) out[done + i] = wanted ? self->scratch_[i] : 0.0f;
            }
            done += n;
        }
        return noErr;
    }

    LtcRenderer renderer_;
    AudioObjectID device_;
    std::string name_;
    int channel_;
    int channels_ = 2;
    int64_t latencyNs_;
    AudioComponentInstance unit_ = nullptr;
    std::vector<float> scratch_;
    std::atomic<float> pendingLevel_{0.5f};

public:
    void initLevel(float level) { pendingLevel_.store(level); }
};

// ---- MTC on a Core MIDI destination -------------------------------------------------

class MidiOutput final : public DeviceOutput {
public:
    MidiOutput(RxService& service, MIDIEndpointRef endpoint, std::string name)
        : service_(service), endpoint_(endpoint), name_(std::move(name)) {}

    ~MidiOutput() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (port_) MIDIPortDispose(port_);
        if (client_) MIDIClientDispose(client_);
    }

    bool open(std::string& error) {
        if (MIDIClientCreate(CFSTR("Code Net"), nullptr, nullptr, &client_) != noErr ||
            MIDIOutputPortCreate(client_, CFSTR("Code Net MTC"), &port_) != noErr) {
            error = "can't open Core MIDI";
            return false;
        }
        thread_ = std::thread([this] { run(); });
        return true;
    }

    std::string description() const override { return "mif4: " + name_; }

private:
    void send(const std::vector<uint8_t>& bytes, int64_t dueNs) {
        Byte buffer[256];
        auto* list = reinterpret_cast<MIDIPacketList*>(buffer);
        MIDIPacket* packet = MIDIPacketListInit(list);
        const MIDITimeStamp when = dueNs <= monotonicNs() ? 0 : nsToHostTicks(dueNs);
        packet = MIDIPacketListAdd(list, sizeof buffer, packet, when, bytes.size(), bytes.data());
        if (packet != nullptr) MIDISend(port_, endpoint_, list);
    }

    void run() {
        // Look a little ahead so piece 0 of each frame is scheduled on time, then let
        // Core MIDI deliver every message at its timestamp.
        constexpr int64_t kLookaheadNs = 2'000'000;
        std::optional<ScheduledFrame> previous;
        while (!stop_) {
            const auto frame = service_.frameAt(monotonicNs() + kLookaheadNs);
            if (!frame) {
                previous.reset();
            } else if (!previous || !(*frame == *previous)) {
                const int64_t duration = kNsPerSecond / frame->timecode.rate;
                std::optional<Timecode> prevTc;
                if (previous) prevTc = previous->timecode;
                for (const auto& m : mtc::messagesForFrame(frame->timecode, prevTc))
                    send(m.bytes, frame->startNs + int64_t(m.fraction * double(duration)));
                previous = frame;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }

    RxService& service_;
    MIDIEndpointRef endpoint_;
    std::string name_;
    MIDIClientRef client_ = 0;
    MIDIPortRef port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

std::string midiName(MIDIObjectRef object) {
    CFStringRef s = nullptr;
    if (MIDIObjectGetStringProperty(object, kMIDIPropertyDisplayName, &s) != noErr || s == nullptr) return "MIDI";
    std::string out = cfString(s);
    CFRelease(s);
    return out;
}

}  // namespace

std::vector<DeviceInfo> listOutputDevices() {
    std::vector<DeviceInfo> out;
    for (AudioObjectID id : audioDevices()) {
        const int channels = outputChannels(id);
        if (channels == 0) continue;
        out.push_back({deviceString(id, kAudioDevicePropertyDeviceUID), deviceString(id, kAudioObjectPropertyName), 0,
                       channels});
    }
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i) {
        MIDIEndpointRef e = MIDIGetDestination(i);
        SInt32 uid = 0;
        MIDIObjectGetIntegerProperty(e, kMIDIPropertyUniqueID, &uid);
        out.push_back({std::to_string(uid), midiName(e), 1, 0});
    }
    return out;
}

std::unique_ptr<DeviceOutput> openAudioOutput(RxService& service, const std::string& deviceUid, int channel,
                                              float level, std::string& error) {
    for (AudioObjectID id : audioDevices()) {
        if (deviceString(id, kAudioDevicePropertyDeviceUID) != deviceUid) continue;
        Float64 rate = 48000;
        getProperty(id, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, rate);
        UInt32 deviceLatency = 0;
        getProperty(id, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, deviceLatency);
        UInt32 streamLatency = 0;
        AudioObjectPropertyAddress streams{kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput,
                                           kElementMain};
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(id, &streams, 0, nullptr, &size) == noErr && size >= sizeof(AudioStreamID)) {
            std::vector<AudioStreamID> ids(size / sizeof(AudioStreamID));
            if (AudioObjectGetPropertyData(id, &streams, 0, nullptr, &size, ids.data()) == noErr)
                getProperty(ids[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, streamLatency);
        }
        const int sampleRate = int(rate + 0.5);
        const int64_t latencyNs = int64_t(deviceLatency + streamLatency) * kNsPerSecond / std::max(1, sampleRate);
        auto out = std::make_unique<AudioOutput>(service, id, deviceString(id, kAudioObjectPropertyName), channel,
                                                 sampleRate, latencyNs, level);
        out->initLevel(level);
        if (!out->open(error)) return nullptr;
        return out;
    }
    error = "audio device not found; is it connected?";
    return nullptr;
}

std::unique_ptr<DeviceOutput> openMidiOutput(RxService& service, const std::string& endpointId, std::string& error) {
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i) {
        MIDIEndpointRef e = MIDIGetDestination(i);
        SInt32 uid = 0;
        MIDIObjectGetIntegerProperty(e, kMIDIPropertyUniqueID, &uid);
        if (std::to_string(uid) != endpointId) continue;
        auto out = std::make_unique<MidiOutput>(service, e, midiName(e));
        if (!out->open(error)) return nullptr;
        return out;
    }
    error = "MIDI port not found; is the MIF4 connected?";
    return nullptr;
}

#endif

}  // namespace codenet
