import Foundation

/// Phone-side RPC session over one PagerLink transport (PROTOCOL.md §3).
///
/// Allocates even request ids (`2, 4, …, 65534`, wrapping to `2` and skipping
/// any id still outstanding), correlates RES/ERR by id, dispatches incoming REQ
/// to registered handlers, and fans incoming EVT and the local `link.reset` out
/// through `events`.
public actor LinkSession {
    /// Cumulative counters for the failure modes the link layer observes.
    public struct Stats: Sendable, Equatable {
        public var malformed = 0
        public var lateResponses = 0
        public var linkResets = 0

        public init() {}
    }

    /// Events delivered to the owner: incoming EVT plus the local `link.reset`
    /// (whose params carry a `reason`).
    public nonisolated let events: AsyncStream<(name: String, params: JSONValue)>
    private nonisolated let eventContinuation: AsyncStream<(name: String, params: JSONValue)>.Continuation

    private struct Pending {
        let continuation: CheckedContinuation<JSONValue, Error>
        let timeoutTask: Task<Void, Never>
    }

    private let transport: PacketTransport
    private let defaultTimeout: Duration

    private var pending: [UInt16: Pending] = [:]
    private var handlers: [String: @Sendable (JSONValue) async throws -> JSONValue] = [:]
    private var nextId: UInt16 = 2
    private var segmenter = Segmenter()
    private var reassembler = Reassembler()
    private var readTask: Task<Void, Never>?

    public private(set) var stats = Stats()

    public init(transport: PacketTransport, defaultTimeout: Duration = .seconds(10)) {
        self.transport = transport
        self.defaultTimeout = defaultTimeout
        var continuation: AsyncStream<(name: String, params: JSONValue)>.Continuation!
        self.events = AsyncStream { continuation = $0 }
        self.eventContinuation = continuation
    }

    /// Number of outstanding requests.
    public var pendingCount: Int { pending.count }

    /// Start reading the transport. Idempotent.
    public func start() {
        guard readTask == nil else { return }
        let transport = self.transport
        readTask = Task { [weak self] in
            for await packet in transport.packets {
                guard let self else { return }
                await self.receive(packet)
            }
        }
    }

    /// Stop reading and fail every outstanding request with `disconnected`.
    public func stop() {
        readTask?.cancel()
        readTask = nil
        let doomed = pending
        pending.removeAll()
        for (_, entry) in doomed {
            entry.timeoutTask.cancel()
            entry.continuation.resume(throwing: RPCError.disconnected())
        }
    }

    /// Register or replace the handler for `method`.
    public func handle(_ method: String, _ handler: @escaping @Sendable (JSONValue) async throws -> JSONValue) {
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
        let packetSize = await transport.packetSize
        let packets = segmenter.segment(encoded, packetSize: packetSize)

        return try await withCheckedThrowingContinuation { continuation in
            let timeoutTask = Task { [weak self] in
                do { try await Task.sleep(for: effectiveTimeout) } catch { return }
                await self?.expire(id)
            }
            pending[id] = Pending(continuation: continuation, timeoutTask: timeoutTask)
            Task { [weak self] in
                await self?.send(packets, id: id)
            }
        }
    }

    /// Send an EVT (id 0) with payload `{"e":event,"p":params}`.
    public func emit(_ event: String, params: JSONValue = .object([:])) async {
        let object: [String: JSONValue] = ["e": .string(event), "p": params]
        guard let payload = try? JSONValue.object(object).encoded() else { return }
        await sendMessage(type: .evt, id: 0, payload: payload)
    }

    // MARK: - Outgoing

    private func send(_ packets: [[UInt8]], id: UInt16) async {
        for packet in packets {
            do {
                try await transport.send(packet)
            } catch {
                fail(id, with: .disconnected())
                return
            }
        }
    }

    private func sendMessage(type: MsgType, id: UInt16, payload: [UInt8]) async {
        guard let encoded = try? Message(type: type, id: id, payload: payload).encoded() else { return }
        let packetSize = await transport.packetSize
        for packet in segmenter.segment(encoded, packetSize: packetSize) {
            try? await transport.send(packet)
        }
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
        case .data, .ack:
            break
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
        // The pager allocates odd ids; a REQ with anything else is rejected.
        guard message.id != 0, (message.id & 1) == 1 else {
            await sendError(id: message.id, code: "invalid_argument", msg: "bad request id parity")
            return
        }
        guard let value = try? JSONValue.decode(bytes: message.payload),
              case .object(let object) = value,
              let method = object["m"]?.stringValue else {
            await sendError(id: message.id, code: "bad_request", msg: "bad request")
            return
        }

        let params: JSONValue
        if let candidate = object["p"] {
            guard case .object = candidate else {
                await sendError(id: message.id, code: "bad_request", msg: "bad request")
                return
            }
            params = candidate
        } else {
            params = .object([:])
        }

        guard let handler = handlers[method] else {
            await sendError(id: message.id, code: "unsupported", msg: "unknown method \(method)")
            return
        }

        do {
            let result = try await handler(params)
            if let payload = try? result.encoded() {
                await sendMessage(type: .res, id: message.id, payload: payload)
            }
        } catch let error as RPCError {
            await sendError(id: message.id, code: error.code, msg: error.msg)
        } catch {
            await sendError(id: message.id, code: "internal", msg: "internal error")
        }
    }

    private func sendError(id: UInt16, code: String, msg: String) async {
        let object: [String: JSONValue] = ["code": .string(code), "msg": .string(msg)]
        guard let payload = try? JSONValue.object(object).encoded() else { return }
        await sendMessage(type: .err, id: id, payload: payload)
    }
}
