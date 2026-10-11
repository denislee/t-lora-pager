#if canImport(Network)
import Foundation
import Network

/// TCP transport for the emulator (PROTOCOL.md §5). Each packet (flags byte
/// included) is sent as a `u16` little-endian length-prefixed record; the packet
/// size is fixed at 244. Both sides reset `seq` to 0 on each new connection.
///
/// Only built where `Network` is available (Apple platforms).
public actor TCPTransport: PacketTransport {
    public static let defaultHost = "127.0.0.1"
    public static let defaultPort: UInt16 = 7600

    public let packetSize = PagerLink.tcpPacket
    public nonisolated let packets: AsyncStream<[UInt8]>
    private nonisolated let continuation: AsyncStream<[UInt8]>.Continuation

    private let host: String
    private let port: UInt16
    private var connection: NWConnection?
    private var ready = false

    public init(host: String = TCPTransport.defaultHost, port: UInt16 = TCPTransport.defaultPort) {
        self.host = host
        self.port = port
        var continuation: AsyncStream<[UInt8]>.Continuation!
        self.packets = AsyncStream { continuation = $0 }
        self.continuation = continuation
    }

    /// Open the connection and begin reading length-prefixed records.
    public func start() {
        guard connection == nil, let nwPort = NWEndpoint.Port(rawValue: port) else { return }
        let connection = NWConnection(host: NWEndpoint.Host(host), port: nwPort, using: .tcp)
        self.connection = connection
        connection.stateUpdateHandler = { [weak self] state in
            Task { await self?.handleState(state) }
        }
        connection.start(queue: .global(qos: .userInitiated))
    }

    /// Close the connection and finish the packet stream.
    public func stop() {
        connection?.cancel()
        connection = nil
        ready = false
        continuation.finish()
    }

    public func send(_ packet: [UInt8]) async throws {
        guard ready, let connection else {
            throw RPCError.disconnected("TCP connection is not ready")
        }
        guard let length = UInt16(exactly: packet.count), length >= 1 else {
            throw RPCError(code: "invalid_argument", msg: "packet length out of range")
        }
        var record: [UInt8] = []
        record.reserveCapacity(2 + packet.count)
        record.append(UInt8(length & 0xFF))
        record.append(UInt8((length >> 8) & 0xFF))
        record.append(contentsOf: packet)
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            connection.send(content: Data(record), completion: .contentProcessed { error in
                if let error {
                    continuation.resume(throwing: error)
                } else {
                    continuation.resume()
                }
            })
        }
    }

    private func handleState(_ state: NWConnection.State) {
        switch state {
        case .ready:
            ready = true
            receiveLength()
        case .failed, .cancelled:
            ready = false
            continuation.finish()
        default:
            break
        }
    }

    private func receiveLength() {
        guard let connection else { return }
        connection.receive(minimumIncompleteLength: 2, maximumLength: 2) { [weak self] data, _, _, error in
            Task { await self?.handleLength(data, error: error) }
        }
    }

    private func handleLength(_ data: Data?, error: NWError?) {
        guard error == nil else {
            continuation.finish()
            return
        }
        guard let data, data.count == 2 else {
            receiveLength()
            return
        }
        let start = data.startIndex
        let length = Int(data[start]) | (Int(data[start + 1]) << 8)
        receiveBody(remaining: length, buffer: [])
    }

    private func receiveBody(remaining: Int, buffer: [UInt8]) {
        guard let connection else { return }
        if remaining == 0 {
            continuation.yield(buffer)
            receiveLength()
            return
        }
        connection.receive(minimumIncompleteLength: 1, maximumLength: remaining) { [weak self] data, _, _, error in
            Task { await self?.handleBody(data, error: error, remaining: remaining, buffer: buffer) }
        }
    }

    private func handleBody(_ data: Data?, error: NWError?, remaining: Int, buffer: [UInt8]) {
        guard error == nil, let data, !data.isEmpty else {
            continuation.finish()
            return
        }
        var buffer = buffer
        buffer.append(contentsOf: data)
        receiveBody(remaining: remaining - data.count, buffer: buffer)
    }
}
#endif
