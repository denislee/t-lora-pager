import Foundation
import PagerKit
#if canImport(Glibc)
import Glibc
private let sockStream = Int32(SOCK_STREAM.rawValue)
private let sysSend = Glibc.send
private let sysClose = Glibc.close
#else
import Darwin
private let sockStream = SOCK_STREAM
private let sysSend = Darwin.send
private let sysClose = Darwin.close
#endif

/// PagerLink over the emulator's TCP port (PROTOCOL.md §5: u16 LE length +
/// packet) on a blocking POSIX socket; Linux (and plain macOS) stand-in for
/// TCPTransport.
final class POSIXTransport: PacketTransport, @unchecked Sendable {
    let packetSize = PagerLink.tcpPacket
    let packets: AsyncStream<[UInt8]>
    private let continuation: AsyncStream<[UInt8]>.Continuation
    private let fd: Int32
    private let lock = NSLock()

    init(host: String, port: UInt16) throws {
        var c: AsyncStream<[UInt8]>.Continuation!
        packets = AsyncStream { c = $0 }
        continuation = c
        fd = socket(AF_INET, sockStream, 0)
        #if canImport(Darwin)
        // Darwin has no MSG_NOSIGNAL: a send after the emulator hangs up
        // must fail, not raise SIGPIPE.
        var on: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
        #endif
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        inet_pton(AF_INET, host, &addr.sin_addr)
        let rc = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard rc == 0 else { throw RPCError(code: "disconnected", msg: "connect failed") }
        let thread = Thread { [fd, continuation] in
            func readN(_ n: Int) -> [UInt8]? {
                var buf = [UInt8](repeating: 0, count: n)
                var got = 0
                while got < n {
                    let r = buf.withUnsafeMutableBytes { recv(fd, $0.baseAddress! + got, n - got, 0) }
                    if r <= 0 { return nil }
                    got += r
                }
                return buf
            }
            while let head = readN(2) {
                let len = Int(head[0]) | Int(head[1]) << 8
                guard let packet = readN(len) else { break }
                continuation.yield(packet)
            }
            continuation.finish()
        }
        thread.start()
    }

    func send(_ packet: [UInt8]) async throws {
        var record = [UInt8(packet.count & 0xFF), UInt8(packet.count >> 8)]
        record += packet
        guard writeAll(record) else { throw RPCError(code: "disconnected", msg: "send failed") }
    }

    private func writeAll(_ record: [UInt8]) -> Bool {
        lock.withLock {
            var sent = 0
            while sent < record.count {
                let r = record.withUnsafeBytes { sysSend(fd, $0.baseAddress! + sent, record.count - sent, 0) }
                if r <= 0 { return false }
                sent += r
            }
            return true
        }
    }

    func close() { shutdown(fd, Int32(SHUT_RDWR)); _ = sysClose(fd) }
}
