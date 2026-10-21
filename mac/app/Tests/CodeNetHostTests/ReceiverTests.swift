// The C++ receiver engine through its C interface, as the app uses it.
import CodeNetCore
import XCTest
@testable import CodeNetHost

final class ReceiverTests: XCTestCase {
    func testLocalReceiverStartsAndReportsStatus() throws {
        var message = [CChar](repeating: 0, count: 256)
        // The strings only need to live for the create call, which copies them.
        let created = "test-rx".withCString { name in
            "mac-a".withCString { main in
                "".withCString { backup in
                    "none".withCString { group -> OpaquePointer? in
                        var config = cn_receiver_config(
                            name: name, main: main, backup: backup, group: group, port: 5299,
                            loss_timeout_frames: 2, max_predict_frames: 2, wifi: 0, auto_return: 1, advance_ms: 0)
                        return cn_receiver_create(&config, &message, Int32(message.count))
                    }
                }
            }
        }
        let handle = try XCTUnwrap(created, String(cString: message))
        defer { cn_receiver_destroy(handle) }
        Thread.sleep(forTimeInterval: 1.2)
        var buffer = [CChar](repeating: 0, count: 4096)
        cn_receiver_status(handle, &buffer, Int32(buffer.count))
        let status = try JSONDecoder().decode(ReceiverStatus.self, from: Data(String(cString: buffer).utf8))
        XCTAssertEqual(status.name, "test-rx")
        XCTAssertEqual(status.main, "mac-a")
        XCTAssertNil(status.output)
        XCTAssertEqual(status.idle?.hasPrefix("no Code Net packets"), true)
    }

    func testDeviceListIsReadable() {
        var list = [cn_device](repeating: cn_device(), count: 64)
        let count = cn_list_devices(&list, Int32(list.count))
        XCTAssertGreaterThanOrEqual(count, 0)
    }
}
