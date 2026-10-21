#include "ReceiverEngine.h"

#include <algorithm>

using namespace codenet;

bool ReceiverSettings::operator==(const ReceiverSettings& o) const {
    return name == o.name && main == o.main && backup == o.backup && outputKind == o.outputKind &&
           deviceId == o.deviceId && channel == o.channel && level == o.level && multicast == o.multicast &&
           wifi == o.wifi && advanceMs == o.advanceMs;
}

ReceiverEngine::~ReceiverEngine() { stop(); }

std::optional<std::string> ReceiverEngine::apply(const ReceiverSettings& in) {
    ReceiverSettings s = in;
    if (s.backup == s.main) s.backup.clear();
    const std::string networkKey = s.name + "|" + (s.multicast ? "m" : "u") + "|" + (s.wifi ? "w" : "");
    if (!service_ || networkKey != networkKey_) {
        stop();
        if (s.name.empty() || s.name.size() > 16) return std::string("the receiver name must be 1 to 16 bytes");
        RxServiceConfig sc;
        sc.name = s.name;
        sc.group = s.multicast ? kMulticastGroup : "none";
        sc.wifi = s.wifi;
        ReceiverConfig rc;
        if (!s.main.empty()) rc.main = makeSourceId(s.main.c_str());
        if (!s.backup.empty()) rc.backup = makeSourceId(s.backup.c_str());
        if (s.wifi) rc.lossTimeoutFrames = rc.maxPredictFrames = 6;
        rc.outputAdvanceNs = int64_t(s.advanceMs * 1e6);
        auto service = std::make_unique<RxService>(sc, rc);
        if (auto err = service->start()) return err;
        service->setAssignCallback([this](const std::string& m, const std::string& b) {
            if (onAssigned) onAssigned(m, b);
        });
        service->setAdvanceCallback([this](double ms) {
            if (onAdvanceSet) onAdvanceSet(ms);
        });
        service_ = std::move(service);
        networkKey_ = networkKey;
        outputKey_.clear();
        sources_ = {s.main, s.backup};
    }
    if (sources_ != std::make_pair(s.main, s.backup)) {
        service_->setSources(s.main, s.backup);
        sources_ = {s.main, s.backup};
    }
    service_->setAdvance(s.advanceMs);
    const std::string outputKey = std::to_string(s.outputKind) + "|" + s.deviceId + "|" + std::to_string(s.channel);
    if (outputKey != outputKey_) {
        outputKey_ = outputKey;
        output_.reset();
        outputError_.reset();
        if (!s.deviceId.empty()) {
            std::string error;
            output_ = s.outputKind == 1 ? openMidiOutput(*service_, s.deviceId, error)
                                        : openAudioOutput(*service_, s.deviceId, s.channel, s.level, error);
            if (!output_) outputError_ = error;
        }
        service_->setDevice(output_ ? output_->description() : "none");
    }
    if (output_) output_->setLevel(s.level);
    return outputError_;
}

void ReceiverEngine::stop() {
    output_.reset();
    if (service_) service_->stop();
    service_.reset();
    networkKey_.clear();
    outputKey_.clear();
}

void ReceiverEngine::returnToMain() {
    if (service_) service_->returnToMain();
}

std::optional<ScheduledFrame> ReceiverEngine::frameNow() const {
    if (!service_) return std::nullopt;
    return service_->frameAt(monotonicNs());
}

std::string ReceiverEngine::statusJson() const { return service_ ? service_->statusJson() : std::string("{}"); }
