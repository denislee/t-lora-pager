import Foundation

/// The phone-side statement of the PROTOCOL.md §6.5 messaging rules, the same
/// checks and messages as the pager (`src/hal/companion_msg.cpp`) and the Go
/// client (`tools/pagerlink/internal/pagerlink/messaging.go` `CheckMessaging`).
/// `vectors/messaging.json` pins every verdict, so a phone that checks here
/// first never sends what the pager would refuse, and refuses a bad
/// pager → phone `clip.push` with the pager's own words.
public enum MessagingRules {
    public static let titleMax = 64
    public static let bodyMax = 512
    public static let idMax = 32
    public static let srcMax = 32
    public static let clipMax = 4000
    public static let textSendMax = 2000

    /// The pager's `invalid_argument` msg for `params` of `notify.post`,
    /// `clip.push` or `text.send`, or nil when it accepts them (and for any
    /// other method). Fields are checked in the pager's order.
    public static func check(_ method: String, _ params: JSONValue) -> String? {
        guard case .object(let p) = params else { return "params: expected object" }
        switch method {
        case "notify.post":
            if let m = text(p, "title", required: true, max: titleMax, allowed: []) { return m }
            if let m = text(p, "body", required: false, max: bodyMax, allowed: ["\n"]) { return m }
            if let raw = p["id"] {
                guard let id = string(raw) else { return "id: expected string" }
                if id.isEmpty { return "id: empty" }
                if id.utf8.count > idMax { return "id: too long" }
                if !id.utf8.allSatisfy(isIDByte) { return "id: bad character" }
            }
            if let m = text(p, "src", required: false, max: srcMax, allowed: []) { return m }
            if let raw = p["ts"] {
                guard case .number(let v) = raw, v.isFinite, v == v.rounded() else { return "ts: expected integer" }
                if v < 0 || v > 4_294_967_295 { return "ts: out of range" }
            }
            if let raw = p["prio"] {
                guard let s = string(raw), NotifyPriority(rawValue: s) != nil else {
                    return "prio: not one of low, normal, high"
                }
            }
            return nil
        case "clip.push":
            return text(p, "text", required: true, max: clipMax, allowed: ["\n", "\t"])
        case "text.send":
            if let m = text(p, "text", required: true, max: textSendMax, allowed: ["\n", "\t"]) { return m }
            guard let raw = p["target"] else { return "target: required" }
            guard let s = string(raw), TextTarget(rawValue: s) != nil else { return "target: not one of editor, kb" }
            return nil
        default:
            return nil
        }
    }

    /// Go's `json.Unmarshal` into a string: a string, or `null` (left empty).
    private static func string(_ value: JSONValue) -> String? {
        switch value {
        case .string(let s): return s
        case .null: return ""
        default: return nil
        }
    }

    private static func text(_ p: [String: JSONValue], _ key: String, required: Bool, max: Int,
                             allowed: Set<Unicode.Scalar>) -> String? {
        guard let raw = p[key] else { return required ? "\(key): required" : nil }
        guard let s = string(raw) else { return "\(key): expected string" }
        if required && s.isEmpty { return "\(key): empty" }
        if s.utf8.count > max { return "\(key): too long" }
        for c in s.unicodeScalars where (c.value < 0x20 || c.value == 0x7F) && !allowed.contains(c) {
            return "\(key): control character"
        }
        return nil
    }

    private static func isIDByte(_ c: UInt8) -> Bool {
        (c >= 0x41 && c <= 0x5A) || (c >= 0x61 && c <= 0x7A) || (c >= 0x30 && c <= 0x39)
            || c == UInt8(ascii: ".") || c == UInt8(ascii: "_") || c == UInt8(ascii: ":") || c == UInt8(ascii: "-")
    }

    /// `s` cut to at most `maxBytes` UTF-8 bytes on a character boundary.
    public static func truncate(_ s: String, maxBytes: Int) -> String {
        guard s.utf8.count > maxBytes else { return s }
        var out = ""
        var used = 0
        for ch in s {
            let n = String(ch).utf8.count
            if used + n > maxBytes { break }
            out.append(ch)
            used += n
        }
        return out
    }

    /// `s` with every control character except those in `keep` replaced by
    /// a space (`\r\n` and lone `\r` become `\n` when `\n` is kept).
    public static func scrub(_ s: String, keep: Set<Unicode.Scalar> = []) -> String {
        var normalized = s
        if keep.contains("\n") {
            normalized = normalized.replacingOccurrences(of: "\r\n", with: "\n").replacingOccurrences(of: "\r", with: "\n")
        }
        var scalars = String.UnicodeScalarView()
        for c in normalized.unicodeScalars {
            scalars.append((c.value < 0x20 || c.value == 0x7F) && !keep.contains(c) ? " " : c)
        }
        return String(scalars)
    }
}

/// `notify.post` priority (PROTOCOL.md §6.5).
public enum NotifyPriority: String, Sendable, CaseIterable, Codable {
    case low, normal, high
}

/// `text.send` target.
public enum TextTarget: String, Sendable, CaseIterable {
    case editor, kb
}

/// One message for the pager's inbox (`notify.post`).
public struct NotifyMessage: Sendable, Equatable {
    public var title: String
    public var body: String?
    /// Stable id: a retry with the same id is not shown twice (`dup`).
    public var id: String?
    public var src: String?
    public var ts: UInt32?
    public var prio: NotifyPriority?

    public init(title: String, body: String? = nil, id: String? = nil, src: String? = nil,
                ts: UInt32? = nil, prio: NotifyPriority? = nil) {
        self.title = title
        self.body = body
        self.id = id
        self.src = src
        self.ts = ts
        self.prio = prio
    }

    /// The `notify.post` params (absent fields left out).
    public var params: [String: JSONValue] {
        var p: [String: JSONValue] = ["title": .string(title)]
        if let body { p["body"] = .string(body) }
        if let id { p["id"] = .string(id) }
        if let src { p["src"] = .string(src) }
        if let ts { p["ts"] = .number(Double(ts)) }
        if let prio { p["prio"] = .string(prio.rawValue) }
        return p
    }

    /// A fresh id the pager accepts: `<prefix>-<12 hex>`.
    public static func newID(prefix: String = "ios") -> String {
        let hex = UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased().prefix(12)
        return "\(prefix)-\(hex)"
    }

    /// A message from shared content (the share extension, Shortcuts): the
    /// first non-empty line of `text` (or the URL's host) as the title, the
    /// rest plus the URL as the body, control characters scrubbed and both cut
    /// to the pager's byte limits on character boundaries. Always passes
    /// `MessagingRules.check`.
    public static func shared(text: String?, url: URL?, src: String?, id: String? = nil,
                              ts: UInt32? = nil) -> NotifyMessage {
        let cleaned = MessagingRules.scrub(text ?? "", keep: ["\n"]).trimmingCharacters(in: .whitespacesAndNewlines)
        var lines = cleaned.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        while let first = lines.first, first.trimmingCharacters(in: .whitespaces).isEmpty { lines.removeFirst() }
        var title = lines.first.map { $0.trimmingCharacters(in: .whitespaces) } ?? ""
        var rest = lines.dropFirst().joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
        if title.isEmpty { title = url?.host ?? "Shared" }
        if title.utf8.count > MessagingRules.titleMax {
            // Keep the whole first line readable in the body.
            rest = rest.isEmpty ? title : title + "\n" + rest
            title = MessagingRules.truncate(title, maxBytes: MessagingRules.titleMax - 3) + "..."
        }
        if let url {
            let link = MessagingRules.scrub(url.absoluteString)
            if !rest.contains(link) && title != link { rest = rest.isEmpty ? link : rest + "\n" + link }
        }
        let body = MessagingRules.truncate(rest, maxBytes: MessagingRules.bodyMax)
        let source = src.map { MessagingRules.truncate(MessagingRules.scrub($0), maxBytes: MessagingRules.srcMax) }
        return NotifyMessage(title: title, body: body.isEmpty ? nil : body, id: id,
                             src: (source?.isEmpty ?? true) ? nil : source, ts: ts)
    }
}

/// What `notify.post` did.
public struct NotifyResult: Sendable, Equatable {
    /// The message is in the inbox now.
    public let stored: Bool
    /// Its id was already there (a retry): not shown again.
    public let dup: Bool

    public init(stored: Bool, dup: Bool) {
        self.stored = stored
        self.dup = dup
    }
}

/// PagerLink messaging (PROTOCOL.md §6.5) on one `LinkSession`: the phone's
/// `notify.post`, `clip.push`, `text.send` and `pager.find` calls, each
/// checked with `MessagingRules` before it is sent (a refusal is the pager's
/// `invalid_argument` with the same msg), and handlers for the pager's
/// `clip.push` and `phone.find`, delivered to the app.
public struct MessagingService: Sendable {
    public let session: LinkSession
    public let timeout: Duration

    public init(session: LinkSession, timeout: Duration = .seconds(10)) {
        self.session = session
        self.timeout = timeout
    }

    /// Install `clip.push` (text checked like the pager checks its own) and
    /// `phone.find`. Each answers `{}` once its callback returns.
    public func register(onClip: @escaping @Sendable (String) async -> Void,
                         onFind: @escaping @Sendable () async -> Void) async {
        await session.handle("clip.push") { params in
            if let msg = MessagingRules.check("clip.push", params) {
                throw RPCError(code: "invalid_argument", msg: msg)
            }
            await onClip(params["text"]?.stringValue ?? "")
            return .object([:])
        }
        await session.handle("phone.find") { _ in
            await onFind()
            return .object([:])
        }
    }

    private func call(_ method: String, _ params: [String: JSONValue]) async throws -> JSONValue {
        if let msg = MessagingRules.check(method, .object(params)) {
            throw RPCError(code: "invalid_argument", msg: msg)
        }
        return try await session.call(method, params: params, timeout: timeout)
    }

    /// `notify.post`.
    public func notifyPost(_ message: NotifyMessage) async throws -> NotifyResult {
        let reply = try await call("notify.post", message.params)
        guard let stored = reply["stored"]?.boolValue else {
            throw RPCError(code: "internal", msg: "notify.post: bad response")
        }
        return NotifyResult(stored: stored, dup: reply["dup"]?.boolValue ?? false)
    }

    /// `clip.push` to the pager; true when it was pasted into an open editor,
    /// false when the pager kept it in its inbox.
    public func clipPush(_ text: String) async throws -> Bool {
        let reply = try await call("clip.push", ["text": .string(text)])
        guard let pasted = reply["pasted"]?.boolValue else {
            throw RPCError(code: "internal", msg: "clip.push: bad response")
        }
        return pasted
    }

    /// `text.send`; returns the bytes typed. `invalid_argument` "editor not
    /// open" / "no text input" when there is nothing to type into.
    public func textSend(_ text: String, target: TextTarget) async throws -> Int {
        let reply = try await call("text.send", ["text": .string(text), "target": .string(target.rawValue)])
        guard let typed = reply["typed"]?.intValue else {
            throw RPCError(code: "internal", msg: "text.send: bad response")
        }
        return typed
    }

    /// `pager.find`: buzz the pager (30 s), or stop it early.
    public func findPager(stop: Bool = false) async throws {
        _ = try await session.call("pager.find", params: stop ? ["stop": true] : [:], timeout: timeout)
    }
}
