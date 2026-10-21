// Code Net wire protocol v1 (see docs/protocol.md). Header-only, no allocation.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>

#include "timecode.hpp"

namespace codenet {

constexpr uint16_t kFramePort = 5260;
constexpr uint16_t kSyncPort = 5261;
constexpr uint16_t kPluginPort = 5262;
constexpr const char* kMulticastGroup = "239.255.60.60";
constexpr uint8_t kVersion = 1;

enum class MessageType : uint8_t { Frame = 1, SyncRequest = 2, SyncResponse = 3, Status = 4 };

enum FrameFlags : uint8_t { kFlagNone = 0, kFlagPlaying = 1, kFlagSourceLost = 2 };

using SourceId = std::array<uint8_t, 16>;

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

}  // namespace codenet
