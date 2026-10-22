// Device outputs for the Windows receiver: LTC through WASAPI (shared mode), MTC through
// WinMM. The Windows counterpart of mac_io.cpp.
#if defined(_WIN32)

#include "codenet/device_output.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <optional>
#include <thread>
#include <vector>

#include "codenet/output.hpp"

namespace codenet {

namespace {

// PKEY_Device_FriendlyName, defined here so no extra GUID library is needed.
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::string utf8(const wchar_t* w) {
    if (w == nullptr) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(size_t(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T** put() {
        reset();
        return &p;
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// COM for the calling thread, released on scope exit if this call initialised it.
struct ComScope {
    bool owns;
    ComScope() : owns(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope() {
        if (owns) CoUninitialize();
    }
};

bool enumerator(ComPtr<IMMDeviceEnumerator>& out) {
    return SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(out.put())));
}

std::string deviceName(IMMDevice* device) {
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, props.put()))) return "audio device";
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string name = "audio device";
    if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) name = utf8(v.pwszVal);
    PropVariantClear(&v);
    return name;
}

int mixChannels(IMMDevice* device) {
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.put()))))
        return 0;
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || mix == nullptr) return 0;
    const int channels = mix->nChannels;
    CoTaskMemFree(mix);
    return channels;
}

// ---- LTC on a WASAPI endpoint ----------------------------------------------------------

class AudioOutput final : public DeviceOutput {
public:
    AudioOutput(RxService& service, std::string deviceId, int channel, float level)
        : service_(service), deviceId_(std::move(deviceId)), channel_(channel) {
        level_.store(level);
    }

    ~AudioOutput() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

    // Opens the device on the audio thread and waits for the result.
    bool open(std::string& error) {
        std::atomic<int> state{0};  // 0 opening, 1 running, 2 failed
        thread_ = std::thread([this, &state, &error] { run(state, error); });
        while (state.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (state.load() == 2) {
            thread_.join();
            return false;
        }
        return true;
    }

    void setLevel(float level) override { level_.store(level); }

    std::string description() const override {
        return "audio: " + name_ + (channel_ < 0 ? " (all outputs)" : " (output " + std::to_string(channel_ + 1) + ")");
    }

private:
    void run(std::atomic<int>& state, std::string& error) {
        ComScope com;
        auto fail = [&](const std::string& message) {
            error = message;
            state.store(2);
        };
        ComPtr<IMMDeviceEnumerator> devices;
        ComPtr<IMMDevice> device;
        if (!enumerator(devices) || FAILED(devices->GetDevice(wide(deviceId_).c_str(), device.put())))
            return fail("audio device not found; is it connected?");
        name_ = deviceName(device.p);
        ComPtr<IAudioClient> client;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(client.put()))))
            return fail("can't open " + name_);
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client->GetMixFormat(&mix)) || mix == nullptr) return fail("can't read the format of " + name_);
        const int sampleRate = int(mix->nSamplesPerSec);
        const int channels = mix->nChannels;
        CoTaskMemFree(mix);

        // 32-bit float at the device's own rate and channel count: the shared-mode engine's
        // native format, so nothing is resampled.
        WAVEFORMATEXTENSIBLE format{};
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels = WORD(channels);
        format.Format.nSamplesPerSec = DWORD(sampleRate);
        format.Format.wBitsPerSample = 32;
        format.Format.nBlockAlign = WORD(4 * channels);
        format.Format.nAvgBytesPerSec = DWORD(sampleRate) * format.Format.nBlockAlign;
        format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        format.Samples.wValidBitsPerSample = 32;
        format.dwChannelMask = 0;
        format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        const REFERENCE_TIME bufferDuration = 200'000;  // 20 ms, in 100 ns units
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferDuration, 0,
                                      reinterpret_cast<WAVEFORMATEX*>(&format), nullptr)))
            return fail(name_ + " doesn't accept 32-bit float output");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        UINT32 bufferFrames = 0;
        ComPtr<IAudioRenderClient> render;
        ComPtr<IAudioClock> clock;
        UINT64 clockFrequency = 0;
        if (event == nullptr || FAILED(client->SetEventHandle(event)) || FAILED(client->GetBufferSize(&bufferFrames)) ||
            FAILED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(render.put()))) ||
            FAILED(client->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(clock.put()))) ||
            FAILED(clock->GetFrequency(&clockFrequency)) || clockFrequency == 0) {
            if (event) CloseHandle(event);
            return fail("can't start audio on " + name_);
        }

        LtcRenderer renderer([this](int64_t t) { return service_.frameAt(t); }, sampleRate, level_.load());
        DacClock dac(sampleRate);
        std::vector<float> mono(bufferFrames);
        uint64_t written = 0;  // frames handed to the device since Start

        // Prime the buffer with silence so the first event has a position to work from.
        BYTE* data = nullptr;
        if (SUCCEEDED(render->GetBuffer(bufferFrames, &data))) {
            render->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
            written = bufferFrames;
        }
        DWORD taskIndex = 0;
        HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (FAILED(client->Start())) {
            CloseHandle(event);
            return fail("can't start audio on " + name_);
        }
        state.store(1);

        while (!stop_) {
            if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) continue;
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;  // device removed
            const UINT32 frames = bufferFrames - padding;
            if (frames == 0) continue;
            // The device position is the frame playing at qpc (100 ns units); the first frame
            // we write now follows everything already queued.
            UINT64 position = 0, qpc = 0;
            int64_t firstNs = monotonicNs() + int64_t(padding) * kNsPerSecond / sampleRate;
            if (SUCCEEDED(clock->GetPosition(&position, &qpc))) {
                const double playing = double(position) * sampleRate / double(clockFrequency);
                firstNs = int64_t(qpc) * 100 + int64_t((double(written) - playing) * 1e9 / sampleRate);
            }
            const int64_t startNs = dac.blockStart(firstNs, int(frames));
            if (FAILED(render->GetBuffer(frames, &data))) break;
            renderer.setLevel(level_.load(std::memory_order_relaxed));
            renderer.render(startNs, mono.data(), int(frames));
            auto* out = reinterpret_cast<float*>(data);
            for (UINT32 i = 0; i < frames; ++i)
                for (int c = 0; c < channels; ++c)
                    out[size_t(i) * size_t(channels) + size_t(c)] = (channel_ < 0 || c == channel_) ? mono[i] : 0.0f;
            render->ReleaseBuffer(frames, 0);
            written += frames;
        }
        client->Stop();
        if (task) AvRevertMmThreadCharacteristics(task);
        CloseHandle(event);
    }

    RxService& service_;
    std::string deviceId_;
    std::string name_ = "audio device";
    int channel_;
    std::atomic<float> level_{0.5f};
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

// ---- MTC through WinMM ----------------------------------------------------------------

class MidiOutput final : public DeviceOutput {
public:
    MidiOutput(RxService& service, UINT deviceIndex, std::string name)
        : service_(service), deviceIndex_(deviceIndex), name_(std::move(name)) {}

    ~MidiOutput() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (handle_) {
            midiOutReset(handle_);
            midiOutClose(handle_);
        }
    }

    bool open(std::string& error) {
        if (midiOutOpen(&handle_, deviceIndex_, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            handle_ = nullptr;
            error = "can't open MIDI port " + name_ + "; is another program using it?";
            return false;
        }
        thread_ = std::thread([this] { run(); });
        return true;
    }

    std::string description() const override { return "mif4: " + name_; }

private:
    struct Pending {
        int64_t dueNs;
        std::vector<uint8_t> bytes;
    };

    void send(const std::vector<uint8_t>& bytes) {
        if (bytes.size() <= 3) {
            DWORD msg = 0;
            for (size_t i = 0; i < bytes.size(); ++i) msg |= DWORD(bytes[i]) << (8 * i);
            midiOutShortMsg(handle_, msg);
            return;
        }
        // SysEx (full frame): hand over a buffer and wait for the driver to finish with it.
        std::vector<char> buffer(bytes.begin(), bytes.end());
        MIDIHDR header{};
        header.lpData = buffer.data();
        header.dwBufferLength = DWORD(buffer.size());
        if (midiOutPrepareHeader(handle_, &header, sizeof header) != MMSYSERR_NOERROR) return;
        if (midiOutLongMsg(handle_, &header, sizeof header) == MMSYSERR_NOERROR) {
            for (int i = 0; i < 100 && !(header.dwFlags & MHDR_DONE); ++i) Sleep(1);
        }
        midiOutUnprepareHeader(handle_, &header, sizeof header);
    }

    void run() {
        // WinMM has no timestamped sending, so this thread sends each message itself: it
        // sleeps in 1 ms steps and spins for the last half millisecond.
        timeBeginPeriod(1);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        constexpr int64_t kLookaheadNs = 2'000'000;
        std::optional<ScheduledFrame> previous;
        std::deque<Pending> queue;
        while (!stop_) {
            const auto frame = service_.frameAt(monotonicNs() + kLookaheadNs);
            if (!frame) {
                previous.reset();
            } else if (!previous || !(*frame == *previous)) {
                const int64_t duration = kNsPerSecond / frame->timecode.rate;
                std::optional<Timecode> prevTc;
                if (previous) prevTc = previous->timecode;
                for (const auto& m : mtc::messagesForFrame(frame->timecode, prevTc))
                    queue.push_back({frame->startNs + int64_t(m.fraction * double(duration)), m.bytes});
                previous = frame;
            }
            const int64_t now = monotonicNs();
            while (!queue.empty() && queue.front().dueNs <= now) {
                send(queue.front().bytes);
                queue.pop_front();
            }
            const int64_t wait = queue.empty() ? 1'000'000 : std::min<int64_t>(queue.front().dueNs - now, 1'000'000);
            if (wait > 600'000) Sleep(1);
            else std::this_thread::yield();
        }
        timeEndPeriod(1);
    }

    RxService& service_;
    UINT deviceIndex_;
    std::string name_;
    HMIDIOUT handle_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

std::vector<std::string> midiNames() {
    std::vector<std::string> names;
    const UINT count = midiOutGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        MIDIOUTCAPSW caps{};
        names.push_back(midiOutGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR ? utf8(caps.szPname)
                                                                                      : "MIDI " + std::to_string(i + 1));
    }
    return names;
}

}  // namespace

std::vector<DeviceInfo> listOutputDevices() {
    ComScope com;
    std::vector<DeviceInfo> out;
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMMDeviceCollection> collection;
    if (enumerator(devices) && SUCCEEDED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put()))) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, device.put()))) continue;
            LPWSTR id = nullptr;
            if (FAILED(device->GetId(&id))) continue;
            out.push_back({utf8(id), deviceName(device.p), 0, mixChannels(device.p)});
            CoTaskMemFree(id);
        }
    }
    // MIDI ports are known by name; indices change as devices come and go.
    for (const auto& name : midiNames()) out.push_back({name, name, 1, 0});
    return out;
}

std::unique_ptr<DeviceOutput> openAudioOutput(RxService& service, const std::string& deviceId, int channel,
                                              float level, std::string& error) {
    auto out = std::make_unique<AudioOutput>(service, deviceId, channel, level);
    if (!out->open(error)) return nullptr;
    return out;
}

std::unique_ptr<DeviceOutput> openMidiOutput(RxService& service, const std::string& name, std::string& error) {
    const auto names = midiNames();
    for (UINT i = 0; i < UINT(names.size()); ++i) {
        if (names[i] != name) continue;
        auto out = std::make_unique<MidiOutput>(service, i, name);
        if (!out->open(error)) return nullptr;
        return out;
    }
    error = "MIDI port not found; is the MIF4 connected?";
    return nullptr;
}

}  // namespace codenet

#endif  // _WIN32
