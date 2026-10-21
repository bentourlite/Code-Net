// Wire compatibility with the Python reference (python/codenet/protocol.py).
import XCTest
@testable import CodeNetHost

final class ProtocolTests: XCTestCase {
    // Bytes produced by the Python encoder for the same values.
    let pythonFrame = "434e45540101011961626c65746f6e2d61000000000000000002000004d200015f9012345678fffffffed5fa0d85"
    let pythonSyncRequest = "434e4554010270692d310000000000000000000000000000000700000000075bcd15"
    let pythonSyncResponse = "434e455401036d6163000000000000000000000000000000000700000000075bcd1500000000000000c8000000000000012c"

    func testFrameEncodeMatchesPython() {
        var p = FramePacket()
        p.source = CodeNet.sourceId("ableton-a")
        p.stream = 2
        p.sequence = 1234
        p.frames = 90000
        p.rate = 25
        p.userBits = 0x1234_5678
        p.frameStartNs = -5_000_000_123
        p.flags = CodeNet.flagPlaying
        XCTAssertEqual(hex(p.encode()), pythonFrame)
        XCTAssertEqual(FramePacket.decode(p.encode()), p)
        XCTAssertEqual(p.timecode, "01:00:00:00")
        XCTAssertTrue(p.playing)
    }

    func testAnchorCountsFramesBetweenPackets() {
        var a = TimecodeAnchor(frames: 90000, rate: 25, frameStartNs: 1_000_000_000, playing: true)
        XCTAssertEqual(a.timecode(at: 1_000_000_000), "01:00:00:00")
        XCTAssertEqual(a.timecode(at: 1_039_999_999), "01:00:00:00")
        XCTAssertEqual(a.timecode(at: 1_040_000_000), "01:00:00:01")
        XCTAssertEqual(a.timecode(at: 1_120_000_000), "01:00:00:03")
        XCTAssertEqual(a.timecode(at: 0), "01:00:00:00")
        XCTAssertEqual(a.timecode(at: 9_000_000_000), "01:00:00:06")  // runs on for at most 1/4 s
        a.playing = false
        XCTAssertEqual(a.timecode(at: 1_120_000_000), "01:00:00:00")
        a = TimecodeAnchor(frames: 25 * 86_400 - 1, rate: 25, frameStartNs: 0, playing: true)
        XCTAssertEqual(a.timecode(at: 40_000_000), "00:00:00:00")
        XCTAssertEqual(TimecodeAnchor().timecode(at: 0), "--:--:--:--")
    }

    func testCalibrateMatchesPython() {
        let c = Calibrate(receiver: "mac-rx", mode: .set, valueUs: -1500, sequence: 3)
        XCTAssertEqual(hex(c.encode()), "434e455401066d61632d72780000000000000000000001fffffa2400000003")
    }

    func testReceiverAnchorRunsOnFromItsReport() throws {
        XCTAssertEqual(TimecodeAnchor.parse("01:00:00:05", rate: 25), 90005)
        var e = ReceiverEntry(id: "pi-1", address: "10.0.0.2", status: try JSONDecoder().decode(
            ReceiverStatus.self, from: Data(#"{"output":"01:00:00:05","rate":25,"advance_ms":1.5}"#.utf8)),
                              lastSeen: Date(), heardNs: 1_000_000_000)
        let a = try XCTUnwrap(e.outputAnchor)
        XCTAssertEqual(a.timecode(at: 1_400_000_000), "01:00:00:15")
        XCTAssertEqual(a.timecode(at: 9_000_000_000), "01:00:01:17")  // stops 1.5 s on
        e.status.output = nil
        XCTAssertNil(e.outputAnchor)
    }

    func testFrameDecodeRejectsBadPackets() {
        var bytes = unhex(pythonFrame)
        XCTAssertNil(FramePacket.decode(Data(bytes.dropLast())))
        bytes[7] = 24
        XCTAssertNil(FramePacket.decode(Data(bytes)))
        bytes = unhex(pythonFrame)
        bytes[0] = 0
        XCTAssertNil(FramePacket.decode(Data(bytes)))
    }

    func testSyncResponseMatchesPython() throws {
        let req = try XCTUnwrap(SyncRequest.decode(Data(unhex(pythonSyncRequest))))
        XCTAssertEqual(CodeNet.sourceName(req.receiver), "pi-1")
        XCTAssertEqual(req.sequence, 7)
        XCTAssertEqual(req.t1, 123_456_789)
        XCTAssertEqual(hex(req.response(host: CodeNet.sourceId("mac"), t2: 200, t3: 300)), pythonSyncResponse)
    }

    func testStatusDecode() throws {
        let json = #"{"name":"pi-1","following":"mac-a","output":"running","rate":25,"device":"MIF4","wifi":true,"sources":[{"name":"mac-a","role":"main","valid":true,"playing":true,"gap_ms":40.1}],"extra":1}"#
        var data = Data(unhex("434e45540104") + CodeNet.sourceId("pi-1"))
        data.append(Data(json.utf8))
        let (id, status) = try XCTUnwrap(ReceiverStatus.decode(data))
        XCTAssertEqual(id, "pi-1")
        XCTAssertEqual(status.device, "MIF4")
        XCTAssertEqual(status.sources?.first?.role, "main")
        XCTAssertNil(status.readback)
    }

    func testAssignMatchesPython() {
        XCTAssertEqual(hex(Assign(receiver: "pi-1", main: "mac-a", backup: nil, sequence: 9).encode()),
                       "434e4554010570692d310000000000000000000000006d61632d6100000000000000000000000000000000000000000000000000000000000009")
        XCTAssertEqual(hex(Assign(receiver: "pi-1", main: "mac-a", backup: "mac-b", sequence: 10).encode()),
                       "434e4554010570692d310000000000000000000000006d61632d6100000000000000000000006d61632d6200000000000000000000000000000a")
    }

    func testStatusWithAssignmentDecodes() throws {
        let json = #"{"name":"pi-1","main":"mac-a","backup":null,"heard":["mac-b"],"sources":[]}"#
        var data = Data(unhex("434e45540104") + CodeNet.sourceId("pi-1"))
        data.append(Data(json.utf8))
        let (_, status) = try XCTUnwrap(ReceiverStatus.decode(data))
        XCTAssertEqual(status.main, "mac-a")
        XCTAssertNil(status.backup)
        XCTAssertEqual(status.heard, ["mac-b"])
    }

    private func hex(_ d: Data) -> String { d.map { String(format: "%02x", $0) }.joined() }

    private func unhex(_ s: String) -> [UInt8] {
        var out: [UInt8] = []
        var i = s.startIndex
        while i < s.endIndex {
            let j = s.index(i, offsetBy: 2)
            out.append(UInt8(s[i..<j], radix: 16)!)
            i = j
        }
        return out
    }
}
