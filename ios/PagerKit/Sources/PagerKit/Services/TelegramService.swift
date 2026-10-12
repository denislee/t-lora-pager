import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

/// Answers the pager's `tg.chats` / `tg.messages` / `tg.read` / `tg.send`
/// (PROTOCOL.md §6, HUB_REMOVAL_PLAN.md H2.6) by calling the user's tg-bridge
/// over the phone's internet.
///
/// Validation matches the mock phone (`tools/pagerlink/cmd/pagerlink/
/// tgbridge.go`): `bridge` and `token` are required on every call; `chat_id`,
/// `limit`, `up_to` and `xfer` must be JSON integers; `limit` is 1..100; a
/// non-zero `xfer` must be odd. The phone builds only the fixed bridge paths —
/// it never fetches a path the pager supplies — and uses the bearer token for
/// that one request: it is never logged, stored, or echoed in an error.
///
/// Replies are trimmed to the fields the pager parses (chats: id, title,
/// unread; messages: id, text, from, out, and media {type, w, h} only when the
/// bridge sent one). A body that fits a RES goes inline as `{"body":…}`;
/// otherwise it streams as DATA under the pager's `xfer` and the RES is
/// `{xfer, size, crc32}`. A bridge 401/403 is `unauthorized`; any other
/// non-2xx is `http_error` whose msg is the decimal status.
public struct TelegramService: Sendable {
    /// Largest RES payload (PROTOCOL.md §3).
    public static let inlineLimit = PagerLink.maxPayload
    /// Idle bound for a bulk reply's ACKs (the pager's tg timeout).
    public static let bulkTimeout: Duration = .seconds(15)
    /// Per-request bridge timeout, under the pager's 15 s.
    public static let requestTimeout: TimeInterval = 12

    private let session: LinkSession
    private let http: URLSession

    public init(session: LinkSession, urlSession: URLSession = .shared) {
        self.session = session
        self.http = urlSession
    }

    /// Install the `tg.*` handlers on the session.
    public func register() async {
        let service = self
        await session.handle("tg.chats") { params in try await service.chats(params) }
        await session.handle("tg.messages") { params in try await service.messages(params) }
        await session.handle("tg.read") { params in try await service.read(params) }
        await session.handle("tg.send") { params in try await service.send(params) }
    }

    // MARK: - Handlers

    func chats(_ params: JSONValue) async throws -> JSONValue {
        let call = try Self.common(params)
        let limit = try Self.limit(params)
        let xfer = try Self.xfer(params)
        let body = try await request(call, "GET", "/v1/chats?limit=\(limit)")
        return try await reply(Self.trimChats(body), xfer: xfer)
    }

    func messages(_ params: JSONValue) async throws -> JSONValue {
        let call = try Self.common(params)
        let limit = try Self.limit(params)
        let chatID = try Self.integer(params, "chat_id")
        let xfer = try Self.xfer(params)
        let body = try await request(call, "GET", "/v1/chats/\(chatID)/messages?limit=\(limit)")
        return try await reply(Self.trimMessages(body), xfer: xfer)
    }

    func read(_ params: JSONValue) async throws -> JSONValue {
        let call = try Self.common(params)
        let chatID = try Self.integer(params, "chat_id")
        let upTo = try Self.integer(params, "up_to")
        _ = try Self.xfer(params)
        _ = try await request(call, "POST", "/v1/chats/\(chatID)/read",
                              json: ["up_to": .number(Double(upTo))])
        return .object([:])
    }

    func send(_ params: JSONValue) async throws -> JSONValue {
        let call = try Self.common(params)
        let chatID = try Self.integer(params, "chat_id")
        guard let text = params["text"]?.stringValue, !text.isEmpty else {
            throw RPCError(code: "invalid_argument", msg: "text must be a non-empty string")
        }
        _ = try Self.xfer(params)
        _ = try await request(call, "POST", "/v1/chats/\(chatID)/messages", json: ["text": .string(text)])
        return .object([:])
    }

    // MARK: - Reply

    /// mock `tgReply`: inline when `{"body":…}` fits a RES, else DATA under
    /// the pager's xfer (which may overtake the RES; the pager buffers it).
    func reply(_ trimmed: JSONValue, xfer: UInt16) async throws -> JSONValue {
        let body = try trimmed.encoded()
        if body.count + #"{"body":}"#.utf8.count <= Self.inlineLimit {
            return ["body": trimmed]
        }
        guard xfer != 0 else {
            throw RPCError(code: "invalid_argument", msg: "reply over 4096 B needs xfer")
        }
        let session = self.session
        Task {
            try? await session.sendBytes(xfer: xfer, data: body, timeout: TelegramService.bulkTimeout)
        }
        return [
            "xfer": .number(Double(xfer)),
            "size": .number(Double(body.count)),
            "crc32": .number(Double(CRC32.checksum(body))),
        ]
    }

    // MARK: - Bridge HTTP

    struct Call {
        let base: String
        let token: String
    }

    private func request(_ call: Call, _ method: String, _ path: String,
                         json: JSONValue? = nil) async throws -> JSONValue? {
        guard let url = URL(string: call.base + path) else {
            throw RPCError(code: "invalid_argument", msg: "bad bridge url")
        }
        var request = URLRequest(url: url)
        request.httpMethod = method
        request.timeoutInterval = Self.requestTimeout
        request.setValue("Bearer \(call.token)", forHTTPHeaderField: "Authorization")
        if let json {
            request.httpBody = Data(try json.encoded())
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        }

        let reply: HTTPReply
        do {
            reply = try await HTTP.send(request, using: http)
        } catch {
            throw RPCError(code: "http_error", msg: Self.transportReason(error))
        }
        switch reply.status {
        case 200..<300:
            if method == "GET" {
                guard let value = try? JSONValue.decode(bytes: Array(reply.body)) else {
                    throw RPCError(code: "internal", msg: "bad bridge reply")
                }
                return value
            }
            return nil
        case 401, 403:
            throw RPCError(code: "unauthorized", msg: "\(reply.status)")
        default:
            throw RPCError(code: "http_error", msg: "\(reply.status)")
        }
    }

    /// Short, token-free reason for a request that got no HTTP reply; the
    /// pager shows it as "phone: <reason>" (non-numeric `http_error` msg).
    static func transportReason(_ error: Error) -> String {
        guard let code = (error as? URLError)?.code else { return "network error" }
        switch code {
        case .timedOut:
            return "bridge timeout"
        case .cannotFindHost, .cannotConnectToHost, .dnsLookupFailed,
             .networkConnectionLost, .notConnectedToInternet:
            return "bridge unreachable"
        case .appTransportSecurityRequiresSecureConnection:
            return "bridge needs https"
        case .secureConnectionFailed, .serverCertificateUntrusted,
             .serverCertificateHasBadDate, .serverCertificateNotYetValid,
             .serverCertificateHasUnknownRoot:
            return "bridge TLS error"
        default:
            return "network error"
        }
    }

    // MARK: - Params (mock tgParams / tgInt / tgXfer)

    /// `bridge` (an http(s) base URL, trailing slash dropped) and `token`.
    static func common(_ params: JSONValue) throws -> Call {
        guard let bridge = params["bridge"]?.stringValue, !bridge.isEmpty,
              let token = params["token"]?.stringValue, !token.isEmpty else {
            throw RPCError(code: "invalid_argument", msg: "bridge and token are required")
        }
        var base = bridge
        if base.hasSuffix("/") { base.removeLast() }
        guard let components = URLComponents(string: base),
              let scheme = components.scheme?.lowercased(), scheme == "http" || scheme == "https",
              components.host?.isEmpty == false,
              components.query == nil, components.fragment == nil else {
            throw RPCError(code: "invalid_argument", msg: "bridge must be an http(s) URL")
        }
        return Call(base: base, token: token)
    }

    /// A JSON integer (`7`; not `"7"`, `7.5` or `true`), named in the error.
    static func integer(_ params: JSONValue, _ key: String) throws -> Int64 {
        guard let value = Params.int64(params[key]) else {
            throw RPCError(code: "invalid_argument", msg: "\(key) must be an integer")
        }
        return value
    }

    static func limit(_ params: JSONValue) throws -> Int64 {
        let limit = try integer(params, "limit")
        guard (1...100).contains(limit) else {
            throw RPCError(code: "invalid_argument", msg: "limit must be 1..100")
        }
        return limit
    }

    /// Absent or 0: no bulk receiver offered; otherwise an odd 1..65535.
    static func xfer(_ params: JSONValue) throws -> UInt16 {
        guard params["xfer"] != nil else { return 0 }
        let value = try integer(params, "xfer")
        if value == 0 { return 0 }
        guard value >= 1, value <= 65535, value % 2 != 0 else {
            throw RPCError(code: "invalid_argument", msg: "xfer must be an odd 1..65535")
        }
        return UInt16(value)
    }

    // MARK: - Trimming

    private static func items(_ body: JSONValue?) throws -> [JSONValue] {
        guard case .array(let items)? = body else {
            throw RPCError(code: "internal", msg: "bad bridge reply")
        }
        return items
    }

    private static func pick(_ value: JSONValue, _ keys: [String]) -> [String: JSONValue] {
        var out: [String: JSONValue] = [:]
        for key in keys {
            if let field = value[key] { out[key] = field }
        }
        return out
    }

    /// Each chat → `{id, title, unread}` (fields the bridge sent).
    static func trimChats(_ body: JSONValue?) throws -> JSONValue {
        .array(try items(body).map { .object(pick($0, ["id", "title", "unread"])) })
    }

    /// Each message → `{id, text, from, out}` plus `media {type, w, h}` only
    /// when the bridge sent a media object.
    static func trimMessages(_ body: JSONValue?) throws -> JSONValue {
        .array(try items(body).map { message in
            var out = pick(message, ["id", "text", "from", "out"])
            if case .object? = message["media"], let media = message["media"] {
                out["media"] = .object(pick(media, ["type", "w", "h"]))
            }
            return .object(out)
        })
    }
}
