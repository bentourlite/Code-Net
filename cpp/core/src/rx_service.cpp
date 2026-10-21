#include "codenet/rx_service.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

namespace codenet {

int64_t monotonicNs() {
#if defined(__APPLE__)
    return int64_t(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
#elif defined(_WIN32)
    // QueryPerformanceCounter: the clock WASAPI reports device positions against.
    static const int64_t frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return int64_t(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const int64_t ticks = int64_t(c.QuadPart);
    return ticks / frequency * kNsPerSecond + ticks % frequency * kNsPerSecond / frequency;
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * kNsPerSecond + ts.tv_nsec;
#endif
}

namespace {

constexpr int64_t kSyncIntervalNs = 250'000'000;
constexpr int64_t kStatusIntervalNs = 1'000'000'000;
constexpr int64_t kOtherHostExpiryNs = 10'000'000'000;

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            out += char(c);
        }
    }
    return out + "\"";
}

std::string jsonNumber(double v, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

SourceId idFromName(const std::string& name) { return makeSourceId(name.c_str()); }

#if defined(_WIN32)
using socklen_t = int;
void closeSocket(int fd) { closesocket(SOCKET(fd)); }
std::string socketError() {
    char buf[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, DWORD(WSAGetLastError()), 0, buf,
                   sizeof buf, nullptr);
    std::string s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.')) s.pop_back();
    return s;
}
bool waitReadable(int fd, int timeoutMs) {
    WSAPOLLFD p{SOCKET(fd), POLLRDNORM, 0};
    return WSAPoll(&p, 1, timeoutMs) > 0 && (p.revents & POLLRDNORM);
}
#else
void closeSocket(int fd) { close(fd); }
std::string socketError() { return std::strerror(errno); }
bool waitReadable(int fd, int timeoutMs) {
    pollfd p{fd, POLLIN, 0};
    return poll(&p, 1, timeoutMs) > 0 && (p.revents & POLLIN);
}
#endif

}  // namespace

RxService::RxService(RxServiceConfig config, ReceiverConfig receiverConfig)
    : config_(std::move(config)), id_(idFromName(config_.name)), receiver_(receiverConfig) {
    startedNs_ = monotonicNs();
    publishSchedule();
}

RxService::~RxService() { stop(); }

std::optional<std::string> RxService::start() {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return std::string("can't start Windows networking");
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    fd_ = s == INVALID_SOCKET ? -1 : int(s);
#else
    fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#endif
    if (fd_ < 0) return std::string("socket: ") + socketError();
    int yes = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
#ifdef SO_REUSEPORT
    setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof yes);
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        std::string err = std::string("can't listen on port ") + std::to_string(config_.port) + ": " +
                          socketError();
        closeSocket(fd_);
        fd_ = -1;
        return err;
    }
    if (config_.group != "none" && !config_.group.empty()) {
        ip_mreq mreq{};
        inet_pton(AF_INET, config_.group.c_str(), &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof mreq) != 0) {
            std::string err = std::string("can't join multicast group ") + config_.group + ": " + socketError();
            closeSocket(fd_);
            fd_ = -1;
            return err;
        }
    }
    int tos = 0xB8;  // DSCP EF: low delay
    setsockopt(fd_, IPPROTO_IP, IP_TOS, reinterpret_cast<const char*>(&tos), sizeof tos);
    stop_ = false;
    thread_ = std::thread([this] { run(); });
    return std::nullopt;
}

void RxService::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) {
        closeSocket(fd_);
#if defined(_WIN32)
        WSACleanup();
#endif
    }
    fd_ = -1;
}

std::optional<ScheduledFrame> RxService::frameAt(int64_t localNs) const {
    Schedule copy;
    for (;;) {
        const uint32_t before = scheduleSeq_.load(std::memory_order_acquire);
        if (before & 1) continue;
        copy = schedule_;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (scheduleSeq_.load(std::memory_order_relaxed) == before) break;
    }
    return copy.frameAt(localNs);
}

void RxService::publishSchedule() {
    // Called with lock_ held (or before the thread starts); the only writer.
    const Schedule s = receiver_.schedule();
    scheduleSeq_.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    schedule_ = s;
    scheduleSeq_.fetch_add(1, std::memory_order_release);
}

void RxService::setSources(const std::string& main, const std::string& backup) {
    std::lock_guard<std::mutex> g(lock_);
    receiver_.setSources(main.empty() ? std::nullopt : std::optional<SourceId>(idFromName(main)),
                         backup.empty() ? std::nullopt : std::optional<SourceId>(idFromName(backup)), monotonicNs());
    publishSchedule();
}

void RxService::returnToMain() {
    std::lock_guard<std::mutex> g(lock_);
    receiver_.requestReturnToMain();
}

void RxService::setDevice(const std::string& device) {
    std::lock_guard<std::mutex> g(lock_);
    device_ = device;
}

void RxService::setAdvance(double ms) {
    std::lock_guard<std::mutex> g(lock_);
    receiver_.setOutputAdvance(int64_t(ms * 1e6));
    publishSchedule();
}

double RxService::advanceMs() const {
    std::lock_guard<std::mutex> g(lock_);
    return double(receiver_.config().outputAdvanceNs) / 1e6;
}

void RxService::setAdvanceCallback(AdvanceCallback cb) {
    std::lock_guard<std::mutex> g(lock_);
    onAdvance_ = std::move(cb);
}

void RxService::setAssignCallback(AssignCallback cb) {
    std::lock_guard<std::mutex> g(lock_);
    onAssign_ = std::move(cb);
}

std::string RxService::statusJson() const {
    std::lock_guard<std::mutex> g(lock_);
    return status_;
}

void RxService::sendTo(const uint8_t* data, size_t size, const std::string& host, uint16_t port) {
    if (fd_ < 0) return;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) return;
    sendto(fd_, reinterpret_cast<const char*>(data), int(size), 0, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
}

void RxService::run() {
    std::vector<uint8_t> buf(2048);
    int64_t nextSync = 0, nextStatus = monotonicNs() + kStatusIntervalNs;
    while (!stop_) {
        if (waitReadable(fd_, 1)) {
            sockaddr_in from{};
            socklen_t len = sizeof from;
            const auto n = recvfrom(fd_, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                                    reinterpret_cast<sockaddr*>(&from), &len);
            if (n > 0) {
                char host[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &from.sin_addr, host, sizeof host);
                handle(buf.data(), size_t(n), host, monotonicNs());
            }
        }
        const int64_t now = monotonicNs();
        if (now >= nextSync) {
            sendSync(now);
            nextSync = now + kSyncIntervalNs;
        }
        {
            std::lock_guard<std::mutex> g(lock_);
            receiver_.update(now);
            publishSchedule();
        }
        if (now >= nextStatus) {
            const std::string payload = buildStatus(now);
            std::vector<uint8_t> packet(kStatusHeaderSize);
            putHeader(packet.data(), MessageType::Status);
            std::memcpy(packet.data() + 6, id_.data(), 16);
            packet.insert(packet.end(), payload.begin(), payload.end());
            std::set<std::string> hosts;
            {
                std::lock_guard<std::mutex> g(lock_);
                for (auto& [_, a] : hostAddresses_) hosts.insert(a);
                for (auto& [_, h] : otherHosts_) hosts.insert(h.first);
            }
            if (payload.size() <= kMaxStatusBytes)
                for (auto& h : hosts) sendTo(packet.data(), packet.size(), h, kSyncPort);
            nextStatus = now + kStatusIntervalNs;
        }
    }
}

void RxService::sendSync(int64_t nowNs) {
    std::vector<std::string> hosts;
    {
        std::lock_guard<std::mutex> g(lock_);
        for (auto& [_, a] : hostAddresses_) hosts.push_back(a);
    }
    for (auto& h : hosts) {
        SyncRequest r;
        r.receiver = id_;
        r.sequence = ++syncSequence_;
        r.t1 = nowNs;
        uint8_t out[SyncRequest::kSize];
        encode(r, out);
        sendTo(out, sizeof out, h, kSyncPort);
    }
}

void RxService::handle(const uint8_t* data, size_t size, const std::string& host, int64_t t4) {
    const uint8_t type = messageType(data, size);
    if (type == uint8_t(MessageType::Frame)) {
        FramePacket f;
        if (!decode(data, size, f)) return;
        std::lock_guard<std::mutex> g(lock_);
        ++packetsReceived_;
        if (receiver_.find(f.source) == nullptr) {
            otherHosts_[sourceName(f.source)] = {host, t4};
        } else {
            hostAddresses_[f.source] = host;
            auto it = lastFrameRx_.find(f.source);
            if (it != lastFrameRx_.end()) maxGapNs_[f.source] = std::max(maxGapNs_[f.source], t4 - it->second);
            lastFrameRx_[f.source] = t4;
        }
        receiver_.onFrame(f, t4);
        publishSchedule();
    } else if (type == uint8_t(MessageType::SyncResponse)) {
        SyncResponse r;
        if (!decode(data, size, r)) return;
        std::lock_guard<std::mutex> g(lock_);
        ++packetsReceived_;
        receiver_.onSync(r.host, r.t1, r.t2, r.t3, t4);
    } else if (type == uint8_t(MessageType::Assign)) {
        Assign a;
        if (!decode(data, size, a) || a.receiver != id_) return;
        AssignCallback cb;
        std::string main = sourceName(a.main), backup = isEmpty(a.backup) ? "" : sourceName(a.backup);
        {
            std::lock_guard<std::mutex> g(lock_);
            // The host app repeats each assignment until it sees it in our status.
            if (lastAssign_ && lastAssign_->first == host && lastAssign_->second == a.sequence) return;
            lastAssign_ = std::make_pair(host, a.sequence);
            if (a.backup == a.main) return;
            const auto& cfg = receiver_.config();
            const std::optional<SourceId> newBackup =
                isEmpty(a.backup) ? std::nullopt : std::optional<SourceId>(a.backup);
            if (cfg.main == std::optional<SourceId>(a.main) && cfg.backup == newBackup) return;
            receiver_.setSources(a.main, newBackup, t4);
            for (auto it = hostAddresses_.begin(); it != hostAddresses_.end();) {
                if (receiver_.find(it->first) == nullptr) {
                    otherHosts_[sourceName(it->first)] = {it->second, t4};
                    it = hostAddresses_.erase(it);
                } else {
                    ++it;
                }
            }
            for (const auto& s : receiver_.sources()) {
                auto o = otherHosts_.find(sourceName(s.source));
                if (hostAddresses_.count(s.source) == 0 && o != otherHosts_.end()) {
                    hostAddresses_[s.source] = o->second.first;
                    otherHosts_.erase(o);
                }
            }
            publishSchedule();
            cb = onAssign_;
        }
        if (cb) cb(main, backup);
    } else if (type == uint8_t(MessageType::Calibrate)) {
        Calibrate c;
        if (!decode(data, size, c) || c.receiver != id_) return;
        AdvanceCallback cb;
        double ms = 0;
        {
            std::lock_guard<std::mutex> g(lock_);
            if (lastCalibrate_ && lastCalibrate_->first == host && lastCalibrate_->second == c.sequence) return;
            lastCalibrate_ = std::make_pair(host, c.sequence);
            calibrateSeq_ = c.sequence;
            if (c.mode == CalibrateMode::Measure) {
                // Measuring needs the output looped back into an input, which this receiver
                // does not read yet; Core Audio reports most of the device delay itself.
                calibrateState_ = "failed";
                calibrateMessage_ = "this receiver can't measure its output; set its offset by hand";
                return;
            }
            receiver_.setOutputAdvance(int64_t(c.valueUs) * 1000);
            publishSchedule();
            calibrateState_ = "done";
            calibrateMessage_ = "set by hand";
            ms = c.valueUs / 1000.0;
            cb = onAdvance_;
        }
        if (cb) cb(ms);
    }
}

std::string RxService::describeIdle(int64_t nowNs) const {
    if (receiver_.sources().empty()) return "no main host assigned yet; pick one in the Code Net app";
    if (packetsReceived_ == 0) return "no Code Net packets received yet; check the network and the sender";
    std::string reasons;
    for (const auto& s : receiver_.sources()) {
        const std::string name = sourceName(s.source);
        std::string r;
        if (!s.hasPacket) r = "nothing from " + name;
        else if (!s.clock.ready()) r = name + " heard but its clock is not synced; check its firewall";
        else if (!(s.lastPacket.flags & kFlagPlaying)) r = name + " is stopped";
        else if (!receiver_.isValid(s, nowNs)) r = name + " has gone quiet";
        if (!r.empty()) reasons += (reasons.empty() ? "" : "; ") + r;
    }
    return reasons.empty() ? "waiting" : reasons;
}

std::string RxService::buildStatus(int64_t nowNs) {
    std::lock_guard<std::mutex> g(lock_);
    for (auto it = otherHosts_.begin(); it != otherHosts_.end();)
        it = nowNs - it->second.second > kOtherHostExpiryNs ? otherHosts_.erase(it) : std::next(it);

    const auto frame = receiver_.frameAt(nowNs);
    const auto& cfg = receiver_.config();
    const SourceState* active = receiver_.active();
    std::string j = "{";
    j += "\"name\":" + jsonString(config_.name);
    j += ",\"main\":" + (cfg.main ? jsonString(sourceName(*cfg.main)) : std::string("null"));
    j += ",\"backup\":" + (cfg.backup ? jsonString(sourceName(*cfg.backup)) : std::string("null"));
    j += ",\"heard\":[";
    bool first = true;
    for (auto& [name, _] : otherHosts_) {
        j += (first ? "" : ",") + jsonString(name);
        first = false;
    }
    j += "]";
    j += ",\"following\":" + (active ? jsonString(roleName(active->role)) : std::string("null"));
    j += ",\"output\":" + (frame ? jsonString(frame->timecode.toString()) : std::string("null"));
    j += ",\"rate\":" + (frame ? std::to_string(int(frame->timecode.rate)) : std::string("null"));
    j += ",\"idle\":" + (frame ? std::string("null") : jsonString(describeIdle(nowNs)));
    j += ",\"device\":" + jsonString(device_);
    j += ",\"readback\":null";
    j += std::string(",\"wifi\":") + (config_.wifi ? "true" : "false");
    j += ",\"advance_ms\":" + jsonNumber(double(cfg.outputAdvanceNs) / 1e6, 3);
    if (calibrateSeq_) {
        j += ",\"calibration\":{\"seq\":" + std::to_string(*calibrateSeq_) + ",\"state\":" +
             jsonString(calibrateState_) + ",\"message\":" + jsonString(calibrateMessage_) + ",\"measured_ms\":null}";
    } else {
        j += ",\"calibration\":null";
    }
    j += ",\"uptime_s\":" + std::to_string((nowNs - startedNs_) / kNsPerSecond);
    j += ",\"sources\":[";
    first = true;
    for (const auto& s : receiver_.sources()) {
        j += first ? "{" : ",{";
        first = false;
        j += "\"name\":" + jsonString(sourceName(s.source));
        j += ",\"role\":" + jsonString(roleName(s.role));
        j += std::string(",\"valid\":") + (receiver_.isValid(s, nowNs) ? "true" : "false");
        j += std::string(",\"playing\":") + (s.hasPacket && (s.lastPacket.flags & kFlagPlaying) ? "true" : "false");
        auto gap = maxGapNs_.find(s.source);
        j += ",\"gap_ms\":" + (gap != maxGapNs_.end() ? jsonNumber(gap->second / 1e6, 1) : std::string("null"));
        j += ",\"offset_ms\":" + (s.clock.ready() ? jsonNumber(s.clock.best().offsetNs / 1e6, 3) : std::string("null"));
        j += ",\"rtt_ms\":" + (s.clock.ready() ? jsonNumber(s.clock.best().rttNs / 1e6, 3) : std::string("null"));
        auto addr = hostAddresses_.find(s.source);
        j += ",\"address\":" + (addr != hostAddresses_.end() ? jsonString(addr->second) : std::string("null"));
        j += ",\"last_timecode\":" + (s.hasPacket ? jsonString(s.lastPacket.timecode.toString()) : std::string("null"));
        j += "}";
    }
    j += "]}";
    maxGapNs_.clear();
    status_ = j;
    return j;
}

}  // namespace codenet
