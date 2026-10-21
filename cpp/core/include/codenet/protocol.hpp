// Code Net wire protocol v1 (see docs/protocol.md). Header-only, no allocation.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "timecode.hpp"

namespace codenet {

constexpr uint16_t kFramePort = 5260;
constexpr uint16_t kSyncPort = 5261;
constexpr uint16_t kPluginPort = 5262;
constexpr const char* kMulticastGroup = "239.255.60.60";
constexpr uint8_t kVersion = 1;

enum class MessageType : uint8_t { Frame = 1, SyncRequest = 2, SyncResponse = 3, Status = 4, Assign = 5 };

enum FrameFlags : uint8_t { kFlagNone = 0, kFlagPlaying = 1, kFlagSourceLost = 2 };

using SourceId = std::array<uint8_t, 16>;

inline bool isEmpty(const SourceId& id) { return id[0] == 0; }

inline std::string sourceName(const SourceId& id) {
    size_t n = 0;
    while (n < id.size() && id[n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(id.data()), n);
}

inline SourceId makeSourceId(const char* name) {
    SourceId id{};
    const size_t n = std::strlen(name);
    std::memcpy(id.data(), name, n < id.size() ? n : id.size());
    return id;
}

struct FramePacket {
    static constexpr size_t kSize = 46;
    SourceId source{};
    uint16_t stream = 0;
    uint32_t sequence = 0;
    Timecode timecode;
    uint32_t userBits = 0;
    int64_t frameStartNs = 0;
    uint8_t flags = kFlagNone;
};

namespace wire {
inline void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i)); }
inline void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (56 - 8 * i)); }
inline uint16_t get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t get32(const uint8_t* p) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v = v << 8 | p[i]; return v; }
inline uint64_t get64(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v = v << 8 | p[i]; return v; }
}  // namespace wire

inline void encode(const FramePacket& f, uint8_t (&out)[FramePacket::kSize]) {
    std::memcpy(out, "CNET", 4);
    out[4] = kVersion;
    out[5] = uint8_t(MessageType::Frame);
    out[6] = f.flags;
    out[7] = f.timecode.rate;
    std::memcpy(out + 8, f.source.data(), 16);
    wire::put16(out + 24, f.stream);
    wire::put32(out + 26, f.sequence);
    wire::put32(out + 30, f.timecode.frames);
    wire::put32(out + 34, f.userBits);
    wire::put64(out + 38, uint64_t(f.frameStartNs));
}

inline bool decode(const uint8_t* data, size_t size, FramePacket& f) {
    if (size != FramePacket::kSize || std::memcmp(data, "CNET", 4) != 0 || data[4] != kVersion ||
        data[5] != uint8_t(MessageType::Frame) || !Timecode::supportedRate(data[7]))
        return false;
    f.flags = data[6];
    f.timecode.rate = data[7];
    std::memcpy(f.source.data(), data + 8, 16);
    f.stream = wire::get16(data + 24);
    f.sequence = wire::get32(data + 26);
    f.timecode.frames = wire::get32(data + 30);
    f.userBits = wire::get32(data + 34);
    f.frameStartNs = int64_t(wire::get64(data + 38));
    return f.timecode.frames < Timecode::framesPerDay(f.timecode.rate);
}

// The message type of a Code Net packet, or 0 if it isn't one.
inline uint8_t messageType(const uint8_t* data, size_t size) {
    if (size < 6 || std::memcmp(data, "CNET", 4) != 0 || data[4] != kVersion) return 0;
    return data[5];
}

struct SyncRequest {
    static constexpr size_t kSize = 34;
    SourceId receiver{};
    uint32_t sequence = 0;
    int64_t t1 = 0;
};

struct SyncResponse {
    static constexpr size_t kSize = 50;
    SourceId host{};
    uint32_t sequence = 0;
    int64_t t1 = 0, t2 = 0, t3 = 0;
};

struct Assign {
    static constexpr size_t kSize = 58;
    SourceId receiver{};
    SourceId main{};
    SourceId backup{};  // all zero for none
    uint32_t sequence = 0;
};

inline void putHeader(uint8_t* out, MessageType type) {
    std::memcpy(out, "CNET", 4);
    out[4] = kVersion;
    out[5] = uint8_t(type);
}

inline void encode(const SyncRequest& r, uint8_t (&out)[SyncRequest::kSize]) {
    putHeader(out, MessageType::SyncRequest);
    std::memcpy(out + 6, r.receiver.data(), 16);
    wire::put32(out + 22, r.sequence);
    wire::put64(out + 26, uint64_t(r.t1));
}

inline bool decode(const uint8_t* data, size_t size, SyncResponse& r) {
    if (size != SyncResponse::kSize || messageType(data, size) != uint8_t(MessageType::SyncResponse)) return false;
    std::memcpy(r.host.data(), data + 6, 16);
    r.sequence = wire::get32(data + 22);
    r.t1 = int64_t(wire::get64(data + 26));
    r.t2 = int64_t(wire::get64(data + 34));
    r.t3 = int64_t(wire::get64(data + 42));
    return true;
}

inline void encode(const Assign& a, uint8_t (&out)[Assign::kSize]) {
    putHeader(out, MessageType::Assign);
    std::memcpy(out + 6, a.receiver.data(), 16);
    std::memcpy(out + 22, a.main.data(), 16);
    std::memcpy(out + 38, a.backup.data(), 16);
    wire::put32(out + 54, a.sequence);
}

inline bool decode(const uint8_t* data, size_t size, Assign& a) {
    if (size != Assign::kSize || messageType(data, size) != uint8_t(MessageType::Assign)) return false;
    std::memcpy(a.receiver.data(), data + 6, 16);
    std::memcpy(a.main.data(), data + 22, 16);
    std::memcpy(a.backup.data(), data + 38, 16);
    a.sequence = wire::get32(data + 54);
    return !isEmpty(a.main);
}

// Status report header; the JSON payload follows.
constexpr size_t kStatusHeaderSize = 22;
constexpr size_t kMaxStatusBytes = 1200;

}  // namespace codenet
