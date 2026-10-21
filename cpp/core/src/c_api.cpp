#include "codenet/codenet_c.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>

#include "codenet/device_output.hpp"
#include "codenet/rx_service.hpp"

using namespace codenet;

struct cn_receiver {
    std::unique_ptr<RxService> service;
    std::mutex outputLock;
    std::unique_ptr<DeviceOutput> output;
};

namespace {
void copyError(const std::string& message, char* error, int size) {
    if (error == nullptr || size <= 0) return;
    std::strncpy(error, message.c_str(), size_t(size - 1));
    error[size - 1] = 0;
}

std::string str(const char* s) { return s ? s : ""; }

std::optional<SourceId> sourceOrNone(const char* name) {
    if (name == nullptr || *name == 0) return std::nullopt;
    return makeSourceId(name);
}
}  // namespace

extern "C" {

cn_receiver* cn_receiver_create(const cn_receiver_config* c, char* error, int error_size) {
    RxServiceConfig sc;
    sc.name = str(c->name);
    sc.group = c->group && *c->group ? c->group : kMulticastGroup;
    sc.port = uint16_t(c->port > 0 ? c->port : kFramePort);
    sc.wifi = c->wifi != 0;
    if (sc.name.empty() || sc.name.size() > 16) {
        copyError("the receiver name must be 1 to 16 bytes", error, error_size);
        return nullptr;
    }
    ReceiverConfig rc;
    rc.main = sourceOrNone(c->main);
    rc.backup = sourceOrNone(c->backup);
    rc.lossTimeoutFrames = std::max(1, c->loss_timeout_frames);
    rc.maxPredictFrames = std::max(1, c->max_predict_frames);
    if (c->wifi) {
        rc.lossTimeoutFrames = std::max(rc.lossTimeoutFrames, 6);
        rc.maxPredictFrames = std::max(rc.maxPredictFrames, 6);
    }
    rc.autoReturn = c->auto_return != 0;
    rc.outputAdvanceNs = int64_t(c->advance_ms * 1e6);
    auto r = std::make_unique<cn_receiver>();
    r->service = std::make_unique<RxService>(sc, rc);
    if (auto err = r->service->start()) {
        copyError(*err, error, error_size);
        return nullptr;
    }
    return r.release();
}

void cn_receiver_destroy(cn_receiver* r) {
    if (r == nullptr) return;
    cn_receiver_stop_output(r);
    r->service->stop();
    delete r;
}

void cn_receiver_set_sources(cn_receiver* r, const char* main, const char* backup) {
    r->service->setSources(str(main), str(backup));
}

void cn_receiver_return_to_main(cn_receiver* r) { r->service->returnToMain(); }

void cn_receiver_set_assign_callback(cn_receiver* r, cn_assign_callback callback, void* context) {
    if (callback == nullptr) {
        r->service->setAssignCallback(nullptr);
        return;
    }
    r->service->setAssignCallback(
        [callback, context](const std::string& main, const std::string& backup) {
            callback(context, main.c_str(), backup.c_str());
        });
}

int cn_receiver_status(cn_receiver* r, char* buffer, int size) {
    const std::string s = r->service->statusJson();
    if (buffer != nullptr && size > 0) {
        const size_t n = std::min(s.size(), size_t(size - 1));
        std::memcpy(buffer, s.data(), n);
        buffer[n] = 0;
    }
    return int(s.size());
}

int cn_list_devices(cn_device* out, int max) {
    const auto devices = listOutputDevices();
    for (int i = 0; i < max && i < int(devices.size()); ++i) {
        cn_device& d = out[i];
        std::memset(&d, 0, sizeof d);
        std::strncpy(d.id, devices[size_t(i)].id.c_str(), sizeof d.id - 1);
        std::strncpy(d.name, devices[size_t(i)].name.c_str(), sizeof d.name - 1);
        d.kind = devices[size_t(i)].kind;
        d.channels = devices[size_t(i)].channels;
    }
    return int(devices.size());
}

int cn_receiver_start_output(cn_receiver* r, int kind, const char* device_id, int channel, float level, char* error,
                             int error_size) {
    std::lock_guard<std::mutex> g(r->outputLock);
    r->output.reset();
    r->service->setDevice("none");
    std::string err;
    if (kind == CN_DEVICE_MIDI)
        r->output = openMidiOutput(*r->service, str(device_id), err);
    else
        r->output = openAudioOutput(*r->service, str(device_id), channel, level, err);
    if (!r->output) {
        copyError(err, error, error_size);
        return -1;
    }
    r->service->setDevice(r->output->description());
    return 0;
}

void cn_receiver_stop_output(cn_receiver* r) {
    std::lock_guard<std::mutex> g(r->outputLock);
    r->output.reset();
    r->service->setDevice("none");
}

void cn_receiver_set_level(cn_receiver* r, float level) {
    std::lock_guard<std::mutex> g(r->outputLock);
    if (r->output) r->output->setLevel(level);
}

int64_t cn_now_ns(void) { return monotonicNs(); }

}  // extern "C"
