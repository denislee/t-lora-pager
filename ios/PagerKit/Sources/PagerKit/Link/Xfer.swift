/// PagerLink v1 bulk-transfer engine (PROTOCOL.md §4) + CRC-32.
///
/// A faithful line-for-line Swift port of the Go engine in
/// `tools/pagerlink/internal/pagerlink/xfer.go`, itself a deliberate
/// line-for-line port of the pager firmware engine in
/// `src/hal/link/link_xfer.{h,cpp}`, so all three implementations (firmware,
/// iOS companion, CLI) share identical flow-control semantics. Where the two
/// disagree, `link_xfer.cpp` is the oracle.
///
/// Flow control:
///   - Credits count DATA messages. The sender keeps at most `credits` DATA
///     messages past the last acknowledged offset; the implicit initial grant
///     is `Xfer.initialCredits`.
///   - `ACK.next` acknowledges every byte below it and its credits replace the
///     sender's window, counting from that offset.
///   - The receiver only ACKs after DATA arrives: every `ackEvery` messages, on
///     completion, and immediately (a re-ACK of its current offset) when a DATA
///     offset is not the one it expects. A sender that gets an ACK with no
///     progress while it has bytes in flight therefore rewinds to that offset
///     (go-back-N) and resends from there.
///
/// The DATA/ACK payload codec is not re-implemented here: it lives in `Bulk`
/// (Frame.swift). Neither `XferSender` nor `XferReceiver` is safe for
/// concurrent use: drive each from a single task, exactly as the firmware
/// drives it from the link task.

/// CRC-32/ISO-HDLC (zlib `crc32`, Go `crc32.ChecksumIEEE`), incremental:
/// `update(update(0, a), b)` is the CRC of `a` followed by `b`. Start a new
/// checksum with 0. The reflected polynomial is 0xEDB88320, the initial value
/// is 0xFFFFFFFF and the final value is XORed with 0xFFFFFFFF.
public enum CRC32 {
    /// Lookup table for the reflected polynomial, built once on first use.
    private static let table: [UInt32] = {
        (0..<256).map { index -> UInt32 in
            var crc = UInt32(index)
            for _ in 0..<8 {
                crc = (crc & 1 != 0) ? (0xEDB8_8320 ^ (crc >> 1)) : (crc >> 1)
            }
            return crc
        }
    }()

    /// The CRC-32/ISO-HDLC of `bytes`; 0 for an empty collection.
    public static func checksum<C: Collection>(_ bytes: C) -> UInt32
    where C.Element == UInt8 {
        update(0, bytes)
    }

    /// Continue a running checksum: `update(checksum(a), b) ==
    /// checksum(a + b)`.
    public static func update<C: Collection>(_ crc: UInt32, _ bytes: C) -> UInt32
    where C.Element == UInt8 {
        var running = crc ^ 0xFFFF_FFFF
        for byte in bytes {
            running = (running >> 8) ^ table[Int((running ^ UInt32(byte)) & 0xFF)]
        }
        return running ^ 0xFFFF_FFFF
    }
}

/// The state of a bulk transfer, mirroring `link_xfer.h` `XferState`.
public enum XferState: Sendable, Equatable {
    /// Still moving bytes.
    case active
    /// Every byte acknowledged / received.
    case done
    /// A callback failed, or the peer overran the transfer.
    case failed
}

/// Bulk-transfer constants (`link_xfer.h`).
public enum Xfer {
    /// Largest DATA chunk: `PagerLink.maxPayload` minus the u32 offset.
    public static let maxDataChunk = 4092
    /// The implicit credit grant at the start of a transfer.
    public static let initialCredits: UInt16 = PagerLink.initialCredits
}

/// Sends `size` bytes as DATA messages under the receiver's credit window. It
/// mirrors `link_xfer.h` `XferSender`.
public final class XferSender {
    private let size: UInt32
    private let chunk: Int
    private let read: (UInt32, Int) throws -> [UInt8]
    private let send: ([UInt8]) throws -> Void
    private var ackedValue: UInt32
    private var next: UInt32
    private var credits: UInt16
    public private(set) var state: XferState

    /// Start a transfer of `size` bytes, `chunk` bytes per DATA message
    /// (clamped to 1...`Xfer.maxDataChunk`), with `start` (clamped to `size`)
    /// the first offset to send — a resumed read. A zero-length remainder is
    /// Done immediately, and `read` / `send` are not called then. A read or
    /// send error fails the transfer.
    public init(
        size: UInt32,
        chunk: Int,
        start: UInt32,
        read: @escaping (UInt32, Int) throws -> [UInt8],
        send: @escaping ([UInt8]) throws -> Void
    ) {
        var clamped = chunk
        if clamped < 1 {
            clamped = 1
        } else if clamped > Xfer.maxDataChunk {
            clamped = Xfer.maxDataChunk
        }
        var acked = start
        if acked > size {
            acked = size
        }
        self.size = size
        self.chunk = clamped
        self.read = read
        self.send = send
        self.ackedValue = acked
        self.next = acked
        self.credits = Xfer.initialCredits
        self.state = acked >= size ? .done : .active
    }

    /// The highest acknowledged offset (bytes below it are safe).
    public var acked: UInt32 { ackedValue }

    /// The next offset to send — also the number of bytes handed to `send` so
    /// far, since DATA is always sent in order from the start offset.
    public var sent: UInt32 { next }

    /// Send DATA while the window allows and return the state afterwards.
    @discardableResult
    public func pump() -> XferState {
        guard state == .active else { return state }

        let chunk32 = UInt32(chunk)
        while next < size {
            // In-flight DATA messages past the acknowledged offset.
            let inflight = (next &- ackedValue &+ chunk32 &- 1) / chunk32
            if inflight >= UInt32(credits) {
                break
            }

            var n = chunk
            let remaining = Int(size &- next)
            if n > remaining {
                n = remaining
            }
            let buf: [UInt8]
            do {
                buf = try read(next, n)
            } catch {
                state = .failed
                return state
            }
            let trimmed = buf.count > n ? Array(buf[0..<n]) : buf
            do {
                let payload = try Bulk.encodeData(offset: next, bytes: trimmed)
                try send(payload)
            } catch {
                state = .failed
                return state
            }
            next = next &+ UInt32(n)
        }
        return state
    }

    /// Feed one ACK payload. A malformed payload, an ACK below the current
    /// acknowledged offset, or one beyond what was sent is ignored. It does not
    /// send; call `pump` next.
    @discardableResult
    public func onAck(_ payload: [UInt8]) -> XferState {
        guard state == .active else { return state }

        guard let decoded = Bulk.decodeAck(payload) else {
            return state
        }
        // Below what we already know, or beyond what we have sent: ignore.
        if decoded.next < ackedValue || decoded.next > next {
            return state
        }
        // No progress while bytes are in flight: rewind and resend (go-back-N).
        if decoded.next == ackedValue && next > ackedValue {
            next = ackedValue
        }
        ackedValue = decoded.next
        credits = decoded.credits
        if ackedValue == size {
            state = .done
        }
        return state
    }
}

/// Receives `size` bytes of DATA in order and acknowledges them. It mirrors
/// `link_xfer.h` `XferReceiver`.
public final class XferReceiver {
    private let size: UInt32
    private let window: UInt16
    private let ackEvery: Int
    private let write: (UInt32, [UInt8]) throws -> Void
    private let ack: ([UInt8]) throws -> Void
    private var sinceAck: Int
    private var next: UInt32
    public private(set) var state: XferState

    /// Receive `size` bytes, granting `window` credits in every ACK and ACKing
    /// every `ackEvery` (clamped to >= 1) in-order DATA messages, with `start`
    /// the first expected offset. A zero-length transfer is Done immediately.
    public init(
        size: UInt32,
        window: UInt16,
        ackEvery: Int,
        start: UInt32,
        write: @escaping (UInt32, [UInt8]) throws -> Void,
        ack: @escaping ([UInt8]) throws -> Void
    ) {
        var every = ackEvery
        if every < 1 {
            every = 1
        }
        self.size = size
        self.window = window
        self.ackEvery = every
        self.write = write
        self.ack = ack
        self.sinceAck = 0
        self.next = start
        self.state = start >= size ? .done : .active
    }

    /// The next expected offset — the number of bytes written so far.
    public var received: UInt32 { next }

    private func sendAck() throws {
        try ack(Bulk.encodeAck(next: next, credits: window))
    }

    /// Feed one DATA payload. A malformed payload is ignored. A wrong offset
    /// makes it re-ACK the current offset and drop the bytes. Bytes past `size`
    /// or a failed write / ACK fail the transfer. It returns the state
    /// afterwards.
    @discardableResult
    public func onData(_ payload: [UInt8]) -> XferState {
        guard state == .active else { return state }

        guard let decoded = Bulk.decodeData(payload) else {
            return state // malformed: ignore
        }

        // Not the offset we are waiting for: re-ACK it and drop the bytes.
        if decoded.offset != next {
            do {
                try sendAck()
            } catch {
                state = .failed
            }
            return state
        }
        let n = decoded.bytes.count
        if UInt32(n) > size &- next {
            state = .failed
            return state
        }
        if n > 0 {
            do {
                try write(decoded.offset, decoded.bytes)
            } catch {
                state = .failed
                return state
            }
        }
        next = next &+ UInt32(n)
        sinceAck += 1

        if next == size {
            state = .done
            do {
                try sendAck()
            } catch {
                state = .failed
            }
            return state
        }
        if sinceAck >= ackEvery {
            sinceAck = 0
            do {
                try sendAck()
            } catch {
                state = .failed
            }
        }
        return state
    }
}
