// Code Net wire protocol v1; see docs/protocol.md.
import Foundation

enum CodeNet {
    static let framePort: UInt16 = 5260
    static let syncPort: UInt16 = 5261
    static let pluginPort: UInt16 = 5262
    static let multicastGroup = "239.255.60.60"
    static let version: UInt8 = 1

    enum MessageType: UInt8 { case frame = 1, syncRequest = 2, syncResponse = 3, status = 4, assign = 5 }

    static let flagPlaying: UInt8 = 1
    static let flagSourceLost: UInt8 = 2

    /// A 16-byte source id from a short name (UTF-8, zero padded, cut at 16 bytes).
    static func sourceId(_ name: String) -> [UInt8] {
        var bytes = Array(name.utf8.prefix(16))
        bytes += [UInt8](repeating: 0, count: 16 - bytes.count)
        return bytes
    }

    static func sourceName(_ id: [UInt8]) -> String {
        String(decoding: id.prefix { $0 != 0 }, as: UTF8.self)
    }

    /// The Mac host clock in nanoseconds; the same clock the Ableton plugin stamps frames with.
    static func nowNs() -> Int64 { Int64(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) }
}

struct FramePacket: Equatable {
    static let size = 46
    var source = [UInt8](repeating: 0, count: 16)
    var stream: UInt16 = 0
    var sequence: UInt32 = 0
    var frames: UInt32 = 0
    var rate: UInt8 = 25
    var userBits: UInt32 = 0
    var frameStartNs: Int64 = 0
    var flags: UInt8 = 0

    var playing: Bool { flags & CodeNet.flagPlaying != 0 }

    var timecode: String {
        let r = UInt32(max(rate, 1))
        let f = frames % r, s = frames / r
        return String(format: "%02u:%02u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60, f)
    }

    func encode() -> Data {
        var d = Data(capacity: Self.size)
        d.append(contentsOf: Array("CNET".utf8))
        d.append(CodeNet.version)
        d.append(CodeNet.MessageType.frame.rawValue)
        d.append(flags)
        d.append(rate)
        d.append(contentsOf: source)
        d.appendBE(stream)
        d.appendBE(sequence)
        d.appendBE(frames)
        d.appendBE(userBits)
        d.appendBE(frameStartNs)
        return d
    }

    static func decode(_ data: Data) -> FramePacket? {
        let b = [UInt8](data)
        guard b.count == size, header(b) == .frame, b[7] == 25 || b[7] == 30 else { return nil }
        var p = FramePacket()
        p.flags = b[6]
        p.rate = b[7]
        p.source = Array(b[8..<24])
        p.stream = b.readBE(24)
        p.sequence = b.readBE(26)
        p.frames = b.readBE(30)
        p.userBits = b.readBE(34)
        p.frameStartNs = b.readBE(38)
        return p
    }

    /// The message type of a Code Net packet, or nil if it isn't one.
    static func header(_ b: [UInt8]) -> CodeNet.MessageType? {
        guard b.count >= 6, b[0] == 0x43, b[1] == 0x4E, b[2] == 0x45, b[3] == 0x54, b[4] == CodeNet.version
        else { return nil }
        return CodeNet.MessageType(rawValue: b[5])
    }
}

struct SyncRequest {
    var receiver: [UInt8]
    var sequence: UInt32
    var t1: Int64

    static func decode(_ data: Data) -> SyncRequest? {
        let b = [UInt8](data)
        guard b.count == 34, FramePacket.header(b) == .syncRequest else { return nil }
        return SyncRequest(receiver: Array(b[6..<22]), sequence: b.readBE(22), t1: b.readBE(26))
    }

    func response(host: [UInt8], t2: Int64, t3: Int64) -> Data {
        var d = Data(capacity: 50)
        d.append(contentsOf: Array("CNET".utf8))
        d.append(CodeNet.version)
        d.append(CodeNet.MessageType.syncResponse.rawValue)
        d.append(contentsOf: host)
        d.appendBE(sequence)
        d.appendBE(t1)
        d.appendBE(t2)
        d.appendBE(t3)
        return d
    }
}

/// A receiver's once-a-second report (JSON after a 22-byte header).
struct ReceiverStatus: Decodable, Equatable {
    struct Readback: Decodable, Equatable {
        var state: String
        var offset_ms: Double?
    }

    struct Source: Decodable, Equatable {
        var name: String
        var role: String
        var valid: Bool
        var playing: Bool
        var gap_ms: Double?
        var offset_ms: Double?
        // Only in this Mac's own receiver status.
        var rtt_ms: Double?
        var address: String?
        var last_timecode: String?
    }

    var name: String?
    var main: String?
    var backup: String?
    var heard: [String]?
    var following: String?
    var output: String?
    var rate: Int?
    var idle: String?
    var device: String?
    var readback: Readback?
    var wifi: Bool?
    var uptime_s: Int?
    var sources: [Source]?

    static func decode(_ data: Data) -> (id: String, status: ReceiverStatus)? {
        let b = [UInt8](data)
        guard b.count > 22, FramePacket.header(b) == .status else { return nil }
        let id = CodeNet.sourceName(Array(b[6..<22]))
        guard let status = try? JSONDecoder().decode(ReceiverStatus.self, from: Data(b[22...])) else { return nil }
        return (id, status)
    }
}

/// Sets a receiver's main and backup hosts; sent to the receiver's frame port.
struct Assign: Equatable {
    var receiver: String
    var main: String
    var backup: String?
    var sequence: UInt32

    func encode() -> Data {
        var d = Data(capacity: 58)
        d.append(contentsOf: Array("CNET".utf8))
        d.append(CodeNet.version)
        d.append(CodeNet.MessageType.assign.rawValue)
        d.append(contentsOf: CodeNet.sourceId(receiver))
        d.append(contentsOf: CodeNet.sourceId(main))
        d.append(contentsOf: CodeNet.sourceId(backup ?? ""))
        d.appendBE(sequence)
        return d
    }
}

extension Data {
    mutating func appendBE<T: FixedWidthInteger>(_ value: T) {
        var be = value.bigEndian
        Swift.withUnsafeBytes(of: &be) { append(contentsOf: $0) }
    }
}

extension Array where Element == UInt8 {
    func readBE<T: FixedWidthInteger>(_ offset: Int) -> T {
        var v: T = 0
        for i in 0..<MemoryLayout<T>.size { v = v << 8 | T(truncatingIfNeeded: self[offset + i]) }
        return v
    }
}
