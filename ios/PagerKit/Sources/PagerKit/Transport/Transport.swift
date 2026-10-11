import Foundation

/// A packet-oriented link transport: one ATT write / notification (or one TCP
/// record, §5) carries one packet (PROTOCOL.md §2).
public protocol PacketTransport: Sendable {
    /// Bytes per packet, flags byte included: negotiated ATT_MTU − 3, or 244
    /// over TCP.
    var packetSize: Int { get async }

    /// Send one packet. Throws when the link is gone.
    func send(_ packet: [UInt8]) async throws

    /// Received packets, in arrival order.
    var packets: AsyncStream<[UInt8]> { get }
}

/// In-memory transport pair for tests: a packet written to one end appears on
/// the other end's `packets`. Thread-safe via an `NSLock` and `AsyncStream`
/// continuations; marked `@unchecked Sendable` accordingly.
public final class LoopbackTransport: PacketTransport, @unchecked Sendable {
    public let packetSize: Int
    public let packets: AsyncStream<[UInt8]>

    private let continuation: AsyncStream<[UInt8]>.Continuation
    private let lock = NSLock()
    private var peer: LoopbackTransport?

    private init(
        packetSize: Int,
        packets: AsyncStream<[UInt8]>,
        continuation: AsyncStream<[UInt8]>.Continuation
    ) {
        self.packetSize = packetSize
        self.packets = packets
        self.continuation = continuation
    }

    /// Create a cross-wired pair with the given packet size.
    public static func pair(packetSize: Int) -> (LoopbackTransport, LoopbackTransport) {
        let (streamA, continuationA) = makeStream()
        let (streamB, continuationB) = makeStream()
        let a = LoopbackTransport(packetSize: packetSize, packets: streamA, continuation: continuationA)
        let b = LoopbackTransport(packetSize: packetSize, packets: streamB, continuation: continuationB)
        a.lock.lock()
        a.peer = b
        a.lock.unlock()
        b.lock.lock()
        b.peer = a
        b.lock.unlock()
        return (a, b)
    }

    private static func makeStream() -> (AsyncStream<[UInt8]>, AsyncStream<[UInt8]>.Continuation) {
        var continuation: AsyncStream<[UInt8]>.Continuation!
        let stream = AsyncStream<[UInt8]> { continuation = $0 }
        return (stream, continuation)
    }

    public func send(_ packet: [UInt8]) async throws {
        let peer = lock.withLock { self.peer }
        peer?.continuation.yield(packet)
    }
}
