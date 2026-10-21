// A small IPv4 UDP socket on top of BSD sockets, read through a dispatch source.
import Darwin
import Foundation

final class UDPSocket {
    enum SocketError: Error, CustomStringConvertible {
        case create(Int32), bind(String, UInt16, Int32)
        var description: String {
            switch self {
            case .create(let e): return "socket(): \(String(cString: strerror(e)))"
            case .bind(let h, let p, let e): return "bind \(h):\(p): \(String(cString: strerror(e)))"
            }
        }
    }

    let fd: Int32
    private var source: DispatchSourceRead?

    init() throws {
        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { throw SocketError.create(errno) }
        var yes: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))
        var ttl: UInt8 = 1
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, socklen_t(1))
        var tos: Int32 = 0xB8  // DSCP EF: low delay
        setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, socklen_t(MemoryLayout<Int32>.size))
    }

    deinit {
        source?.cancel()
        close(fd)
    }

    private static func address(_ host: String, _ port: UInt16) -> sockaddr_in {
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        addr.sin_addr.s_addr = inet_addr(host)
        return addr
    }

    func bind(host: String, port: UInt16) throws {
        var addr = Self.address(host, port)
        let result = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        if result != 0 { throw SocketError.bind(host, port, errno) }
    }

    /// Sends one datagram; returns nil on success or the error text.
    @discardableResult
    func send(_ data: Data, to host: String, port: UInt16) -> String? {
        var addr = Self.address(host, port)
        let sent = data.withUnsafeBytes { raw in
            withUnsafePointer(to: &addr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    sendto(fd, raw.baseAddress, data.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
        return sent == data.count ? nil : String(cString: strerror(errno))
    }

    func startReceiving(on queue: DispatchQueue, handler: @escaping (Data, String, UInt16) -> Void) {
        let src = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
        src.setEventHandler { [weak self] in self?.drain(handler) }
        src.resume()
        source = src
    }

    private func drain(_ handler: (Data, String, UInt16) -> Void) {
        var buffer = [UInt8](repeating: 0, count: 2048)
        while true {
            var addr = sockaddr_in()
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            let n = buffer.withUnsafeMutableBytes { raw in
                withUnsafeMutablePointer(to: &addr) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                        recvfrom(fd, raw.baseAddress, raw.count, MSG_DONTWAIT, $0, &len)
                    }
                }
            }
            if n <= 0 { return }
            let host = String(cString: inet_ntoa(addr.sin_addr))
            handler(Data(buffer[0..<n]), host, UInt16(bigEndian: addr.sin_port))
        }
    }
}
