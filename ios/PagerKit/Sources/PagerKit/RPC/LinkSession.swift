import Foundation

/// Metadata `fs.read` reports for one file (PROTOCOL.md §6.1). `protected`
/// marks `Salted__` ciphertext; the bytes are always the on-disk bytes.
public struct FileInfo: Sendable, Equatable {
    public var size: UInt32
    public var crc32: UInt32
    public var protected: Bool

    public init(size: UInt32, crc32: UInt32, protected: Bool) {
        self.size = size
        self.crc32 = crc32
        self.protected = protected
    }
}

/// A bulk transfer this side receives under an id it allocated
/// (`LinkSession.acceptBytes`). The receiver is live from the moment the id
/// exists, so DATA that overtakes the RES carrying `xfer` is not lost.
public struct IncomingTransfer: Sendable {
    public let xfer: UInt16
    public let size: UInt32
    public let crc32: UInt32
    let session: LinkSession

    /// Wait for every byte, then check the CRC-32. `timeout` bounds each idle
    /// gap between DATA messages. A zero `size` returns empty bytes at once.
    /// Throws `RPCError`: `timeout` ("accept: timeout"), `internal` ("accept:
    /// transfer failed"), `invalid_argument` ("crc mismatch") or
    /// `disconnected`. The transfer is closed on every exit.
    public func wait(timeout: Duration) async throws -> [UInt8] {
        try await session.finishAccept(self, timeout: timeout)
    }
}

/// Phone-side RPC session over one PagerLink transport (PROTOCOL.md §3).
///
/// Allocates even request ids (`2, 4, …, 65534`, wrapping to `2` and skipping
/// any id still outstanding), correlates RES/ERR by id, dispatches incoming REQ
/// to registered handlers, and fans incoming EVT and the local `link.reset` out
/// through `events`. With `role: .pager` it plays the other end (odd ids), which
/// the tests use to drive a phone session end to end.
///
/// Bulk transfer (§4): incoming DATA is routed by transfer id to a receiver this
/// side opened (`acceptBytes`, `openIncoming`, `readFile`), incoming ACK to a
/// sender this side runs (`sendBytes`); anything else is counted in
/// `stats.unroutedBulk` and dropped. Every outgoing message goes through one
/// serial writer, so the packets of two messages never interleave on the wire.
public actor LinkSession {
    /// Which end of the link this session plays; decides id parity (§3.2).
    public enum Role: Sendable {
        case phone
        case pager
    }

    /// Cumulative counters for the failure modes the link layer observes.
    public struct Stats: Sendable, Equatable {
        public var malformed = 0
        public var lateResponses = 0
        public var linkResets = 0
        /// DATA/ACK for a transfer id with no receiver/sender here.
        public var unroutedBulk = 0

        public init() {}
    }

    /// Events delivered to the owner: incoming EVT plus the local `link.reset`
    /// (whose params carry a `reason`). The stream finishes when the transport's
    /// packet stream ends, so iterating it also observes the link dropping.
    public nonisolated let events: AsyncStream<(name: String, params: JSONValue)>
    private nonisolated let eventContinuation: AsyncStream<(name: String, params: JSONValue)>.Continuation

    /// The end of the link this session plays.
    public nonisolated let role: Role

    private struct Pending {
        let continuation: CheckedContinuation<JSONValue, Error>
        let timeoutTask: Task<Void, Never>
    }

    /// One encoded message for the writer. `onFailure` runs when the
    /// transport refuses one of its packets.
    private struct Outgoing: Sendable {
        let bytes: [UInt8]
        let onFailure: (@Sendable () -> Void)?
    }

    /// A transfer this side receives. DATA that arrives before the size is
    /// known (`openIncoming` → RES → `receiveBytes`) is held in `early`.
    private final class Inbound {
        var receiver: XferReceiver?
        var early: [[UInt8]] = []
        var data: [UInt8] = []
        var acks: [[UInt8]] = []
        var lastActivity = ContinuousClock.now
        var waiter: CheckedContinuation<Void, Error>?
        var watchdog: Task<Void, Never>?
    }

    /// A transfer this side sends under the peer's receiver id.
    private final class Outbound {
        var sender: XferSender?
        var out: [[UInt8]] = []
        var lastActivity = ContinuousClock.now
        var lastRetransmit = ContinuousClock.now
        var waiter: CheckedContinuation<Void, Error>?
        var watchdog: Task<Void, Never>?
    }

    /// Early DATA held per transfer before its size is known (the Go client's
    /// sink buffer is the same size).
    private static let earlyLimit = 64
    /// Receivers ACK every this many DATA messages (files.go / responder.go).
    private static let ackEvery = 4

    private let transport: PacketTransport
    private let defaultTimeout: Duration
    private nonisolated let outgoing: AsyncStream<Outgoing>.Continuation
    private let writerTask: Task<Void, Never>

    private var pending: [UInt16: Pending] = [:]
    /// Handlers answer with the RES payload bytes (see `handleEncoded`).
    private var handlers: [String: @Sendable (JSONValue) async throws -> [UInt8]] = [:]
    private var nextId: UInt16
    private var nextXfer: UInt16
    private var inbound: [UInt16: Inbound] = [:]
    private var outbound: [UInt16: Outbound] = [:]
    private var reassembler = Reassembler()
    private var readTask: Task<Void, Never>?
    private var closed = false

    public private(set) var stats = Stats()

    public init(transport: PacketTransport, defaultTimeout: Duration = .seconds(10), role: Role = .phone) {
        self.transport = transport
        self.defaultTimeout = defaultTimeout
        self.role = role
        let first: UInt16 = role == .phone ? 2 : 1
        self.nextId = first
        self.nextXfer = first
        var continuation: AsyncStream<(name: String, params: JSONValue)>.Continuation!
        self.events = AsyncStream { continuation = $0 }
        self.eventContinuation = continuation

        let (stream, writer) = AsyncStream<Outgoing>.makeStream()
        self.outgoing = writer
        self.writerTask = Task {
            var segmenter = Segmenter()
            for await item in stream {
                let packetSize = await transport.packetSize
                for packet in segmenter.segment(item.bytes, packetSize: packetSize) {
                    do {
                        try await transport.send(packet)
                    } catch {
                        item.onFailure?()
                        break
                    }
                }
            }
        }
    }

    deinit {
        outgoing.finish()
        writerTask.cancel()
    }

    /// Number of outstanding requests.
    public var pendingCount: Int { pending.count }

    /// Number of open bulk transfers (both directions).
    public var transferCount: Int { inbound.count + outbound.count }

    /// Start reading the transport. Idempotent.
    public func start() {
        guard readTask == nil else { return }
        let transport = self.transport
        readTask = Task { [weak self] in
            for await packet in transport.packets {
                guard let self else { return }
                await self.receive(packet)
            }
            await self?.transportEnded()
        }
    }

    /// Stop reading and fail every outstanding request and transfer with
    /// `disconnected`.
    public func stop() {
        readTask?.cancel()
        readTask = nil
        failAllPending()
    }

    /// The transport's packet stream finished: the link is gone for good.
    private func transportEnded() {
        closed = true
        failAllPending()
        eventContinuation.finish()
    }

    private func failAllPending() {
        let doomed = pending
        pending.removeAll()
        for (_, entry) in doomed {
            entry.timeoutTask.cancel()
            entry.continuation.resume(throwing: RPCError.disconnected())
        }
        let doomedIn = inbound
        inbound.removeAll()
        for (_, transfer) in doomedIn {
            transfer.watchdog?.cancel()
            transfer.waiter?.resume(throwing: RPCError.disconnected())
            transfer.waiter = nil
        }
        let doomedOut = outbound
        outbound.removeAll()
        for (_, transfer) in doomedOut {
            transfer.watchdog?.cancel()
            transfer.waiter?.resume(throwing: RPCError.disconnected())
            transfer.waiter = nil
        }
    }

    /// Register or replace the handler for `method`.
    public func handle(_ method: String, _ handler: @escaping @Sendable (JSONValue) async throws -> JSONValue) {
        handlers[method] = { params in try await handler(params).encoded() }
    }

    /// Register a handler that returns its RES payload already encoded as
    /// JSON, so a reply it measured is not encoded a second time (P7.56).
    public func handleEncoded(_ method: String, _ handler: @escaping @Sendable (JSONValue) async throws -> [UInt8]) {
        handlers[method] = handler
    }

    /// Send a REQ and await its RES. Throws an `RPCError` when the peer answers
    /// ERR, when the request times out, or when the link drops while it is
    /// outstanding.
    public func call(
        _ method: String,
        params: [String: JSONValue] = [:],
        timeout: Duration? = nil
    ) async throws -> JSONValue {
        if closed { throw RPCError.disconnected() }
        var request: [String: JSONValue] = ["m": .string(method)]
        if !params.isEmpty { request["p"] = .object(params) }
        guard let payload = try? JSONValue.object(request).encoded() else {
            throw RPCError(code: "internal", msg: "encode failed")
        }

        let id = allocId()
        guard let encoded = try? Message(type: .req, id: id, payload: payload).encoded() else {
            throw RPCError(code: "internal", msg: "encode failed")
        }

        let effectiveTimeout = timeout ?? defaultTimeout

        return try await withCheckedThrowingContinuation { continuation in
            let timeoutTask = Task { [weak self] in
                do { try await Task.sleep(for: effectiveTimeout) } catch { return }
                await self?.expire(id)
            }
            pending[id] = Pending(continuation: continuation, timeoutTask: timeoutTask)
            outgoing.yield(Outgoing(bytes: encoded, onFailure: { [weak self] in
                Task { await self?.fail(id, with: .disconnected()) }
            }))
        }
    }

    /// Send an EVT (id 0) with payload `{"e":event,"p":params}`.
    public func emit(_ event: String, params: JSONValue = .object([:])) async {
        let object: [String: JSONValue] = ["e": .string(event), "p": params]
        guard let payload = try? JSONValue.object(object).encoded() else { return }
        sendMessage(type: .evt, id: 0, payload: payload)
    }

    // MARK: - Bulk transfer (PROTOCOL.md §4)

    /// Prepare to receive `size` bytes the peer will stream as DATA, checked
    /// against `crc32` (CRC-32/ISO-HDLC) at the end — the receive side of
    /// `chat.audio.begin`. Allocates a transfer id with this side's parity and
    /// installs the receiver immediately. Mirrors the Go `Client.AcceptBytes`.
    public func acceptBytes(size: UInt32, crc32: UInt32) -> IncomingTransfer {
        let id = openIncoming()
        arm(id, size: size)
        return IncomingTransfer(xfer: id, size: size, crc32: crc32, session: self)
    }

    /// Allocate a receive transfer id (this side's parity, skipping ids in
    /// use) whose size is not known yet: DATA for it is held until
    /// `receiveBytes` names the size. Used for `fs.read` and for a request that
    /// offers the peer an `xfer` for a bulk reply (`tg.*`).
    public func openIncoming() -> UInt16 {
        var id = nextXfer
        for _ in 0..<32768 {
            if inbound[id] == nil { break }
            id = advanceId(id)
        }
        nextXfer = advanceId(id)
        inbound[id] = Inbound()
        return id
    }

    /// Receive `size` bytes on a transfer from `openIncoming`, check the CRC,
    /// and close the transfer. `timeout` bounds each idle gap. Throws
    /// `RPCError` as `IncomingTransfer.wait` does (prefix "xfer").
    public func receiveBytes(xfer: UInt16, size: UInt32, crc32: UInt32, timeout: Duration) async throws -> [UInt8] {
        arm(xfer, size: size)
        return try await finishIncoming(xfer, crc32: crc32, timeout: timeout, label: "xfer")
    }

    /// Drop a receive transfer; buffered bytes are discarded.
    public func closeIncoming(_ xfer: UInt16) {
        guard let transfer = inbound.removeValue(forKey: xfer) else { return }
        transfer.watchdog?.cancel()
        transfer.waiter?.resume(throwing: RPCError.disconnected("transfer closed"))
        transfer.waiter = nil
    }

    /// Stream `data` as DATA under `xfer`, a receiver id the PEER allocated
    /// (e.g. the pager's `xfer` in `tg.chats`, or the phone's in `fs.read`),
    /// honouring its credit window, and return once every byte is
    /// acknowledged. `timeout` bounds each idle wait for an ACK. Throws
    /// `RPCError`: `internal` ("transfer failed"), `timeout` ("timeout") or
    /// `disconnected`. Mirrors the Go `Client.SendBytes`.
    public func sendBytes(xfer: UInt16, data: [UInt8], timeout: Duration) async throws {
        if closed { throw RPCError.disconnected() }
        let transfer = Outbound()
        let sender = XferSender(
            size: UInt32(data.count), chunk: Xfer.maxDataChunk, start: 0,
            read: { offset, count in Array(data[Int(offset)..<Int(offset) + count]) },
            send: { [unowned transfer] payload in transfer.out.append(payload) }
        )
        transfer.sender = sender
        if let previous = outbound[xfer] {
            previous.watchdog?.cancel()
            previous.waiter?.resume(throwing: RPCError(code: "internal", msg: "transfer replaced"))
            previous.waiter = nil
        }
        outbound[xfer] = transfer
        defer {
            if outbound[xfer] === transfer { outbound[xfer] = nil }
            transfer.watchdog?.cancel()
        }

        sender.pump()
        flushData(transfer, xfer: xfer)
        if sender.state == .active {
            transfer.lastActivity = .now
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                installOutboundWaiter(xfer, continuation: continuation, timeout: timeout)
            }
        }
        if sender.state != .done {
            throw RPCError(code: "internal", msg: "transfer failed")
        }
    }

    /// `fs.read` client (PROTOCOL.md §6.1): download `path` from `vol` and
    /// return its bytes and metadata. The transfer id is allocated before the
    /// request so DATA that overtakes the RES is kept; every byte is checked
    /// against the reported CRC-32. `timeout` bounds the RPC and each idle wait
    /// for DATA. Mirrors the Go `Client.ReadFile`; RPC errors pass through, and
    /// local failures are `RPCError`s: `internal` ("fs.read: bad response",
    /// "fs.read: transfer failed"), `timeout` ("fs.read: timeout"),
    /// `invalid_argument` ("crc mismatch").
    public func readFile(vol: String, path: String, timeout: Duration) async throws -> ([UInt8], FileInfo) {
        let id = openIncoming()
        defer { closeIncoming(id) }
        let result = try await call("fs.read", params: [
            "vol": .string(vol),
            "path": .string(path),
            "xfer": .number(Double(id)),
        ], timeout: timeout)
        guard let size = Self.uint32(result["size"]), let crc = Self.uint32(result["crc32"]) else {
            throw RPCError(code: "internal", msg: "fs.read: bad response")
        }
        let info = FileInfo(size: size, crc32: crc, protected: result["protected"]?.boolValue ?? false)
        arm(id, size: size)
        let data = try await finishIncoming(id, crc32: crc, timeout: timeout, label: "fs.read")
        return (data, info)
    }

    // MARK: - Bulk internals

    func finishAccept(_ transfer: IncomingTransfer, timeout: Duration) async throws -> [UInt8] {
        if transfer.size == 0 {
            closeIncoming(transfer.xfer)
            return []
        }
        return try await finishIncoming(transfer.xfer, crc32: transfer.crc32, timeout: timeout, label: "accept")
    }

    private static func uint32(_ value: JSONValue?) -> UInt32? {
        guard let number = value?.doubleValue, number >= 0, number <= Double(UInt32.max),
              number == number.rounded() else { return nil }
        return UInt32(number)
    }

    /// Largest receive buffer reserved before the bytes arrive.
    static let maxReserve = 4 << 20

    /// Give a receive transfer its size and replay any early DATA.
    private func arm(_ xfer: UInt16, size: UInt32) {
        guard let transfer = inbound[xfer], transfer.receiver == nil else { return }
        // One allocation for the whole body (P7.63); the size is the peer's
        // claim, so cap what is reserved up front.
        transfer.data.reserveCapacity(min(Int(size), Self.maxReserve))
        let receiver = XferReceiver(
            size: size, window: Xfer.initialCredits, ackEvery: Self.ackEvery, start: 0,
            write: { [unowned transfer] _, bytes in transfer.data.append(contentsOf: bytes) },
            ack: { [unowned transfer] payload in transfer.acks.append(payload) }
        )
        transfer.receiver = receiver
        let early = transfer.early
        transfer.early = []
        for payload in early { receiver.onData(payload) }
        flushAcks(transfer, xfer: xfer)
    }

    private func finishIncoming(_ xfer: UInt16, crc32: UInt32, timeout: Duration, label: String) async throws -> [UInt8] {
        defer { closeIncoming(xfer) }
        if closed { throw RPCError.disconnected() }
        guard let transfer = inbound[xfer], let receiver = transfer.receiver else {
            throw RPCError.disconnected()
        }
        if receiver.state == .active {
            transfer.lastActivity = .now
            do {
                try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                    installInboundWaiter(xfer, continuation: continuation, timeout: timeout)
                }
            } catch let error as RPCError where error.code == "timeout" {
                throw RPCError.timeout("\(label): timeout")
            }
        }
        transfer.watchdog?.cancel()
        guard receiver.state == .done else {
            throw RPCError(code: "internal", msg: "\(label): transfer failed")
        }
        guard CRC32.checksum(transfer.data) == crc32 else {
            throw RPCError(code: "invalid_argument", msg: "crc mismatch")
        }
        return transfer.data
    }

    private func installInboundWaiter(_ xfer: UInt16, continuation: CheckedContinuation<Void, Error>, timeout: Duration) {
        guard let transfer = inbound[xfer] else {
            continuation.resume(throwing: RPCError.disconnected())
            return
        }
        if let receiver = transfer.receiver, receiver.state != .active {
            continuation.resume()
            return
        }
        transfer.waiter = continuation
        let token = ObjectIdentifier(transfer)
        transfer.watchdog = watchdog(timeout: timeout, initial: timeout) { session in
            await session.inboundIdle(xfer, token: token, timeout: timeout)
        }
    }

    private func installOutboundWaiter(_ xfer: UInt16, continuation: CheckedContinuation<Void, Error>, timeout: Duration) {
        guard let transfer = outbound[xfer] else {
            continuation.resume(throwing: RPCError.disconnected())
            return
        }
        if let sender = transfer.sender, sender.state != .active {
            continuation.resume()
            return
        }
        transfer.waiter = continuation
        let token = ObjectIdentifier(transfer)
        // The first wake-up doubles as the retransmit check; later ones follow
        // whatever the check asks for.
        transfer.watchdog = watchdog(
            timeout: timeout,
            initial: min(timeout, Xfer.retransmitTimeout)
        ) { session in
            await session.outboundIdle(xfer, token: token, timeout: timeout)
        }
    }

    /// Re-arming idle timer: the first sleep is `initial`; afterwards `check`
    /// returns how much longer to wait, or nil when the transfer has finished
    /// or the timer has fired.
    private func watchdog(
        timeout: Duration,
        initial: Duration,
        check: @escaping @Sendable (LinkSession) async -> Duration?
    ) -> Task<Void, Never> {
        Task { [weak self] in
            var wait = initial
            while true {
                do { try await Task.sleep(for: wait) } catch { return }
                guard let self, let next = await check(self) else { return }
                wait = next
            }
        }
    }

    private nonisolated func idleRemaining(since last: ContinuousClock.Instant, timeout: Duration) -> Duration? {
        let idle = ContinuousClock.now - last
        return idle >= timeout ? nil : timeout - idle
    }

    private func inboundIdle(_ xfer: UInt16, token: ObjectIdentifier, timeout: Duration) -> Duration? {
        guard let transfer = inbound[xfer], ObjectIdentifier(transfer) == token,
              let waiter = transfer.waiter else { return nil }
        if let remaining = idleRemaining(since: transfer.lastActivity, timeout: timeout) {
            return remaining
        }
        transfer.waiter = nil
        waiter.resume(throwing: RPCError.timeout())
        return nil
    }

    private func outboundIdle(_ xfer: UInt16, token: ObjectIdentifier, timeout: Duration) -> Duration? {
        guard let transfer = outbound[xfer], ObjectIdentifier(transfer) == token,
              let waiter = transfer.waiter else { return nil }

        // Retransmit timeout: no ACK for `Xfer.retransmitTimeout` while bytes are
        // unacknowledged -> rewind and resend (go-back-N).
        let now = ContinuousClock.now
        if now - max(transfer.lastActivity, transfer.lastRetransmit) >= Xfer.retransmitTimeout {
            transfer.lastRetransmit = .now
            if let sender = transfer.sender, sender.retransmit() {
                sender.pump()
                flushData(transfer, xfer: xfer)
                if sender.state != .active {
                    transfer.waiter = nil
                    waiter.resume()
                    return nil
                }
            }
        }

        if let remaining = idleRemaining(since: transfer.lastActivity, timeout: timeout) {
            // Wake again for the next retransmit check, but never busy-loop.
            let sinceRetransmit = ContinuousClock.now - max(transfer.lastActivity, transfer.lastRetransmit)
            let untilRetransmit = Xfer.retransmitTimeout - sinceRetransmit
            return min(remaining, max(untilRetransmit, Duration.milliseconds(50)))
        }
        transfer.waiter = nil
        waiter.resume(throwing: RPCError.timeout("timeout"))
        return nil
    }

    private func flushAcks(_ transfer: Inbound, xfer: UInt16) {
        let acks = transfer.acks
        transfer.acks.removeAll()
        for payload in acks { sendMessage(type: .ack, id: xfer, payload: payload) }
    }

    private func flushData(_ transfer: Outbound, xfer: UInt16) {
        let out = transfer.out
        transfer.out.removeAll()
        for payload in out { sendMessage(type: .data, id: xfer, payload: payload) }
    }

    private func handleData(_ message: Message) {
        guard let transfer = inbound[message.id] else {
            stats.unroutedBulk += 1
            return
        }
        transfer.lastActivity = .now
        guard let receiver = transfer.receiver else {
            if transfer.early.count < Self.earlyLimit { transfer.early.append(message.payload) }
            return
        }
        receiver.onData(message.payload)
        flushAcks(transfer, xfer: message.id)
        if receiver.state != .active, let waiter = transfer.waiter {
            transfer.waiter = nil
            waiter.resume()
        }
    }

    private func handleAck(_ message: Message) {
        guard let transfer = outbound[message.id], let sender = transfer.sender else {
            stats.unroutedBulk += 1
            return
        }
        transfer.lastActivity = .now
        sender.onAck(message.payload)
        sender.pump()
        flushData(transfer, xfer: message.id)
        if sender.state != .active, let waiter = transfer.waiter {
            transfer.waiter = nil
            waiter.resume()
        }
    }

    // MARK: - Outgoing

    private func sendMessage(type: MsgType, id: UInt16, payload: [UInt8]) {
        guard let encoded = try? Message(type: type, id: id, payload: payload).encoded() else { return }
        outgoing.yield(Outgoing(bytes: encoded, onFailure: nil))
    }

    private func allocId() -> UInt16 {
        var id = nextId
        for _ in 0..<32768 {
            if pending[id] == nil {
                nextId = advanceId(id)
                return id
            }
            id = advanceId(id)
        }
        return 0
    }

    /// Next id of this side's parity: phone `…, 65534, 2`, pager `…, 65535, 1`.
    private func advanceId(_ id: UInt16) -> UInt16 {
        let next = id &+ 2
        return next == 0 ? 2 : next
    }

    private func fail(_ id: UInt16, with error: RPCError) {
        guard let entry = pending.removeValue(forKey: id) else { return }
        entry.timeoutTask.cancel()
        entry.continuation.resume(throwing: error)
    }

    private func expire(_ id: UInt16) {
        guard let entry = pending.removeValue(forKey: id) else { return }
        entry.continuation.resume(throwing: RPCError.timeout())
    }

    // MARK: - Incoming

    private func receive(_ packet: [UInt8]) {
        for event in reassembler.feed(packet) {
            switch event {
            case .message(let bytes):
                guard let message = try? Message.decode(bytes) else {
                    stats.malformed += 1
                    continue
                }
                dispatch(message)
            case .reset(let reason):
                stats.linkResets += 1
                eventContinuation.yield((
                    name: "link.reset",
                    params: .object(["reason": .string(reason.rawValue)])
                ))
            }
        }
    }

    private func dispatch(_ message: Message) {
        switch message.type {
        case .req:
            Task { await self.handleRequest(message) }
        case .res, .err:
            handleResponse(message)
        case .evt:
            handleEvent(message)
        case .data:
            handleData(message)
        case .ack:
            handleAck(message)
        }
    }

    private func handleResponse(_ message: Message) {
        guard let entry = pending.removeValue(forKey: message.id) else {
            stats.lateResponses += 1
            return
        }
        entry.timeoutTask.cancel()
        if message.type == .res {
            if let value = try? JSONValue.decode(bytes: message.payload) {
                entry.continuation.resume(returning: value)
            } else {
                entry.continuation.resume(throwing: RPCError(code: "internal", msg: "bad response"))
            }
        } else {
            entry.continuation.resume(throwing: parseError(message.payload))
        }
    }

    private func parseError(_ payload: [UInt8]) -> RPCError {
        guard let value = try? JSONValue.decode(bytes: payload),
              case .object(let object) = value,
              let code = object["code"]?.stringValue else {
            return RPCError(code: "internal")
        }
        return RPCError(code: code, msg: object["msg"]?.stringValue ?? "")
    }

    private func handleEvent(_ message: Message) {
        guard let value = try? JSONValue.decode(bytes: message.payload),
              case .object(let object) = value,
              let name = object["e"]?.stringValue else {
            stats.malformed += 1
            return
        }
        eventContinuation.yield((name: name, params: object["p"] ?? .object([:])))
    }

    private func handleRequest(_ message: Message) async {
        // The pager allocates odd ids and the phone even ones; a REQ with the
        // peer's wrong parity is rejected.
        let peerParity: UInt16 = role == .phone ? 1 : 0
        guard message.id != 0, (message.id & 1) == peerParity else {
            sendError(id: message.id, code: "invalid_argument", msg: "bad request id parity")
            return
        }
        guard let value = try? JSONValue.decode(bytes: message.payload),
              case .object(let object) = value,
              let method = object["m"]?.stringValue else {
            sendError(id: message.id, code: "bad_request", msg: "bad request")
            return
        }

        let params: JSONValue
        if let candidate = object["p"] {
            guard case .object = candidate else {
                sendError(id: message.id, code: "bad_request", msg: "bad request")
                return
            }
            params = candidate
        } else {
            params = .object([:])
        }

        guard let handler = handlers[method] else {
            sendError(id: message.id, code: "unsupported", msg: "unknown method \(method)")
            return
        }

        do {
            let payload = try await handler(params)
            // A RES the frame can't carry would be dropped and leave the pager
            // waiting out its timeout; answer ERR now instead (P7.63).
            guard payload.count <= PagerLink.maxPayload else {
                sendError(id: message.id, code: "internal", msg: "reply too large")
                return
            }
            sendMessage(type: .res, id: message.id, payload: payload)
        } catch let error as RPCError {
            sendError(id: message.id, code: error.code, msg: error.msg)
        } catch {
            sendError(id: message.id, code: "internal", msg: "internal error")
        }
    }

    private func sendError(id: UInt16, code: String, msg: String) {
        let object: [String: JSONValue] = ["code": .string(code), "msg": .string(msg)]
        guard let payload = try? JSONValue.object(object).encoded() else { return }
        sendMessage(type: .err, id: id, payload: payload)
    }
}
