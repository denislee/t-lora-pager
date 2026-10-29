/// A Telegram account the phone holds itself, answering the pager's `tg.*`
/// with no tg-bridge (PROTOCOL.md §6). `TelegramService` prefers it whenever
/// `isReady()`; results are already in the pager's trimmed shape.
public protocol TelegramNative: Sendable {
    /// Logged in and able to answer.
    func isReady() async -> Bool
    /// `[{id, title, unread}…]` in chat-list order.
    func chats(limit: Int) async throws -> JSONValue
    /// `[{id, text, from, out, media?}…]`, newest first.
    func messages(chatID: Int64, limit: Int) async throws -> JSONValue
    /// Mark `chatID` read up to pager message id `upTo`.
    func read(chatID: Int64, upTo: Int64) async throws
    func send(chatID: Int64, text: String) async throws
}

/// One TDLib JSON request → its reply. A TDLib `error` reply is thrown as
/// `TDError`. The app implements this over `td_json_client`; tests use a fake.
public protocol TDQuerying: Sendable {
    func query(_ request: JSONValue) async throws -> JSONValue
}

/// TDLib's `error code:int32 message:string`.
public struct TDError: Error, Equatable, Sendable {
    public let code: Int
    public let message: String

    public init(code: Int, message: String) {
        self.code = code
        self.message = message
    }
}

/// `TelegramNative` over TDLib's JSON API (td_api.tl).
///
/// Message ids: TDLib's are `server_id << 20`, which overflow the pager's
/// 32-bit ids, so the pager sees `id >> 20` (the server id, what tg-bridge
/// sends) and `read` shifts `up_to` back. Errors come out as the `RPCError`s
/// the pager already maps: TDLib 401 → `unauthorized`, any other TDLib error →
/// `http_error` with the decimal code, and a request TDLib doesn't answer in
/// time (it waits out network loss rather than failing) → `http_error`
/// "telegram timeout".
///
/// A tg.* call runs many TDLib requests (history pages one after another;
/// getChat per chat and getUser per sender concurrently, P7.61), so each one
/// is also capped by the call's `budget`: once it runs out, chats / messages
/// answer with what they already have rather than letting the pager's 15 s
/// RPC timeout fire.
public actor TDTelegram: TelegramNative {
    /// Per TDLib request. Inside chats / messages the call budget (no longer)
    /// is what binds, so this mostly matters for `read` and `send`, which run
    /// one request each.
    public static let requestTimeout: Duration = .seconds(10)
    /// Whole tg.* call, under the pager's 15 s (TG_LINK_TIMEOUT_MS) with room
    /// for the reply's trip over the link.
    public static let callBudget: Duration = .seconds(10)
    static let idShift: Int64 = 20

    private let td: TDQuerying
    private let ready: @Sendable () -> Bool
    private let timeout: Duration
    private let budget: Duration
    private var names: [String: String] = [:]
    private var chatsLoaded = false

    public init(td: TDQuerying, timeout: Duration = TDTelegram.requestTimeout,
                budget: Duration = TDTelegram.callBudget,
                isReady: @escaping @Sendable () -> Bool) {
        self.td = td
        self.timeout = timeout
        self.budget = budget
        self.ready = isReady
    }

    public nonisolated func isReady() async -> Bool { ready() }

    // MARK: - tg.*

    public func chats(limit: Int) async throws -> JSONValue {
        let deadline = ContinuousClock.now + budget
        let list: JSONValue = ["@type": "chatListMain"]
        if !chatsLoaded {
            // Pulls the list from the server once; TDLib answers 404 when
            // it already has everything. Either way getChats reads its cache.
            do {
                _ = try await call(["@type": "loadChats", "chat_list": list, "limit": .number(Double(limit))],
                                   timeout: .seconds(4), by: deadline)
                chatsLoaded = true
            } catch let error as TDError where error.code == 404 {
                chatsLoaded = true
            } catch {}
        }
        let reply = try await call(["@type": "getChats", "chat_list": list, "limit": .number(Double(limit))],
                                   by: deadline)
        guard case .array(let ids)? = reply["chat_ids"] else { throw Self.badReply }
        let chatIDs = ids.compactMap { Params.int64($0) }
        // getChat for every id at once (P7.61), answers kept in list order.
        let answers = await withTaskGroup(of: (Int, Result<JSONValue, Error>).self) { group in
            for (index, chatID) in chatIDs.enumerated() {
                group.addTask {
                    do {
                        return (index, .success(try await self.call(
                            ["@type": "getChat", "chat_id": .number(Double(chatID))], by: deadline)))
                    } catch {
                        return (index, .failure(error))
                    }
                }
            }
            // Every index gets an answer before the group finishes.
            var answers = [Result<JSONValue, Error>](repeating: .failure(Self.badReply), count: chatIDs.count)
            for await (index, answer) in group { answers[index] = answer }
            return answers
        }
        var out: [JSONValue] = []
        for (chatID, answer) in zip(chatIDs, answers) {
            let chat: JSONValue
            do {
                chat = try answer.get()
            } catch where !out.isEmpty {
                break   // failed or out of time: the chats so far beat a pager timeout
            }
            out.append([
                "id": .number(Double(chatID)),
                "title": .string(chat["title"]?.stringValue ?? ""),
                "unread": .number(Double(Params.int64(chat["unread_count"]) ?? 0)),
            ])
        }
        return .array(out)
    }

    public func messages(chatID: Int64, limit: Int) async throws -> JSONValue {
        let deadline = ContinuousClock.now + budget
        var out: [JSONValue] = []
        var seen = Set<Int64>()
        var from: Int64 = 0
        // getChatHistory may answer with fewer than asked (often one message
        // on a cold cache); page back from the oldest seen until full. A
        // later page that fails or runs out of time keeps what is already in.
        for _ in 0..<5 where out.count < limit {
            let reply: JSONValue
            do {
                reply = try await call([
                    "@type": "getChatHistory",
                    "chat_id": .number(Double(chatID)),
                    "from_message_id": .number(Double(from)),
                    "offset": 0,
                    "limit": .number(Double(limit - out.count)),
                    "only_local": false,
                ], by: deadline)
            } catch where !out.isEmpty {
                break
            }
            guard case .array(let batch)? = reply["messages"] else { throw Self.badReply }
            await prefetchSenders(batch, by: deadline)
            var added = false
            for message in batch {
                guard let id = Params.int64(message["id"]), seen.insert(id).inserted else { continue }
                out.append(try await pagerMessage(message, by: deadline))
                from = id
                added = true
                if out.count == limit { break }
            }
            if !added { break }
        }
        return .array(out)
    }

    public func read(chatID: Int64, upTo: Int64) async throws {
        _ = try await call([
            "@type": "viewMessages",
            "chat_id": .number(Double(chatID)),
            "message_ids": [.number(Double(upTo << Self.idShift))],
            "source": nil,
            "force_read": true,
        ])
    }

    public func send(chatID: Int64, text: String) async throws {
        _ = try await call([
            "@type": "sendMessage",
            "chat_id": .number(Double(chatID)),
            "input_message_content": [
                "@type": "inputMessageText",
                "text": ["@type": "formattedText", "text": .string(text), "entities": []],
            ],
        ])
    }

    // MARK: - Mapping

    /// One TDLib `message` → `{id, text, from, out, media?}`.
    func pagerMessage(_ message: JSONValue, by deadline: ContinuousClock.Instant) async throws -> JSONValue {
        let id = Params.int64(message["id"]) ?? 0
        var out: [String: JSONValue] = [
            "id": .number(Double(id >> Self.idShift)),
            "out": .bool(message["is_outgoing"]?.boolValue ?? false),
            "from": .string(await senderName(message["sender_id"], by: deadline)),
        ]
        let (text, media) = Self.content(message["content"])
        out["text"] = .string(text)
        if let media { out["media"] = media }
        return .object(out)
    }

    /// Text (or caption) plus the pager's media descriptor
    /// (photo / video / voice / sticker / document, as the bridge sends).
    static func content(_ content: JSONValue?) -> (String, JSONValue?) {
        let type = content?["@type"]?.stringValue ?? ""
        let caption = content?["caption"]?["text"]?.stringValue ?? ""
        func media(_ kind: String, _ sized: JSONValue?) -> JSONValue {
            [
                "type": .string(kind),
                "w": .number(Double(Params.int64(sized?["width"]) ?? 0)),
                "h": .number(Double(Params.int64(sized?["height"]) ?? 0)),
            ]
        }
        switch type {
        case "messageText":
            return (content?["text"]?["text"]?.stringValue ?? "", nil)
        case "messagePhoto":
            var largest: JSONValue?
            if case .array(let sizes)? = content?["photo"]?["sizes"] { largest = sizes.last }
            return (caption, media("photo", largest))
        case "messageVideo":
            return (caption, media("video", content?["video"]))
        case "messageAnimation":
            return (caption, media("video", content?["animation"]))
        case "messageVideoNote":
            return ("", media("video", nil))
        case "messageVoiceNote":
            return (caption, media("voice", nil))
        case "messageSticker":
            return (content?["sticker"]?["emoji"]?.stringValue ?? "", media("sticker", content?["sticker"]))
        case "messageDocument", "messageAudio":
            return (caption, media("document", nil))
        default:
            // Service messages, polls, locations…: name the kind.
            let kind = type.hasPrefix("message") ? String(type.dropFirst("message".count)) : type
            return ("[\(kind.isEmpty ? "Message" : kind)]", nil)
        }
    }

    /// Resolve a page's uncached senders concurrently (P7.61), so mapping its
    /// messages finds every name cached instead of one getUser at a time.
    private func prefetchSenders(_ batch: [JSONValue], by deadline: ContinuousClock.Instant) async {
        var pending: [String: JSONValue] = [:]
        for message in batch {
            guard let sender = Sender(message["sender_id"]), names[sender.key] == nil else { continue }
            pending[sender.key] = message["sender_id"]
        }
        guard pending.count > 1 else { return }
        await withTaskGroup(of: Void.self) { group in
            for sender in pending.values {
                group.addTask { _ = await self.senderName(sender, by: deadline) }
            }
        }
    }

    /// A TDLib `messageSender*` this can name; `key` indexes `names`.
    private enum Sender {
        case user(Int64)
        case chat(Int64)

        init?(_ sender: JSONValue?) {
            switch sender?["@type"]?.stringValue {
            case "messageSenderUser":
                guard let id = Params.int64(sender?["user_id"]) else { return nil }
                self = .user(id)
            case "messageSenderChat":
                guard let id = Params.int64(sender?["chat_id"]) else { return nil }
                self = .chat(id)
            default:
                return nil
            }
        }

        var key: String {
            switch self {
            case .user(let id): return "u\(id)"
            case .chat(let id): return "c\(id)"
            }
        }
    }

    /// A sender's display name, cached per session ("" when not had in time).
    func senderName(_ sender: JSONValue?, by deadline: ContinuousClock.Instant) async -> String {
        guard let who = Sender(sender) else { return "" }
        if let name = names[who.key] { return name }
        let name: String
        switch who {
        case .user(let userID):
            guard let user = try? await call(["@type": "getUser", "user_id": .number(Double(userID))],
                                             by: deadline) else {
                return ""
            }
            name = Self.displayName(user)
        case .chat(let chatID):
            guard let chat = try? await call(["@type": "getChat", "chat_id": .number(Double(chatID))],
                                             by: deadline) else {
                return ""
            }
            name = chat["title"]?.stringValue ?? ""
        }
        names[who.key] = name
        return name
    }

    /// A TDLib `user`'s first and last name, whichever are set.
    public static func displayName(_ user: JSONValue) -> String {
        [user["first_name"]?.stringValue, user["last_name"]?.stringValue]
            .compactMap { $0 }.filter { !$0.isEmpty }.joined(separator: " ")
    }

    // MARK: - Transport

    static let badReply = RPCError(code: "internal", msg: "bad telegram reply")
    static let timedOut = RPCError(code: "http_error", msg: "telegram timeout")

    /// One TDLib request under `timeout` (and `deadline`, when given), TDLib
    /// errors already mapped for the pager (loadChats' 404 is let through as
    /// `TDError` for the caller).
    private func call(_ request: JSONValue, timeout: Duration? = nil,
                      by deadline: ContinuousClock.Instant? = nil) async throws -> JSONValue {
        let td = self.td
        var limit = timeout ?? self.timeout
        if let deadline { limit = min(limit, deadline - .now) }
        guard limit > .zero else { throw Self.timedOut }
        let timeout = limit
        do {
            return try await withThrowingTaskGroup(of: JSONValue.self) { group in
                group.addTask { try await td.query(request) }
                group.addTask {
                    try await Task.sleep(for: timeout)
                    throw TDTelegram.timedOut
                }
                defer { group.cancelAll() }
                guard let first = try await group.next() else { throw Self.badReply }
                return first
            }
        } catch let error as TDError {
            if request["@type"]?.stringValue == "loadChats" { throw error }
            if error.code == 401 { throw RPCError(code: "unauthorized", msg: "401") }
            throw RPCError(code: "http_error", msg: "\(error.code)")
        }
    }
}
