import Foundation

// The pager's settings over PagerLink (PROTOCOL.md §6.4, IOS_SETTINGS_PLAN.md
// §4.4). The pager owns the catalog: the schema lists only what the connected
// board has, with its ranges, so the app renders it generically.

/// One setting value as the app holds it.
public enum SettingValue: Sendable, Equatable, Hashable {
    case bool(Bool)
    case int(Int)
    case string(String)
    /// A write-only secret: whether one is set. Written as `.string`.
    case secret(set: Bool)
    /// `list` / `object` settings, kept as JSON.
    case json(JSONValue)

    /// Decode a `settings.get` value. With the item's `type` the decoding is
    /// exact; without it (`type` nil) it is inferred from the JSON shape.
    public init?(wire: JSONValue, type: SettingItem.Kind?) {
        switch (type, wire) {
        case (.secret?, _):
            guard let set = wire["set"]?.boolValue else { return nil }
            self = .secret(set: set)
        case (.bool?, .bool(let b)), (nil, .bool(let b)):
            self = .bool(b)
        case (.int?, .number(let n)):
            guard let i = Int(exactly: n) else { return nil }
            self = .int(i)
        case (.string?, .string(let s)):
            self = .string(s)
        case (.enumeration?, .number(let n)):
            guard let i = Int(exactly: n) else { return nil }
            self = .int(i)
        case (.enumeration?, .string(let s)):
            self = .string(s)
        case (.list?, .array), (.object?, .object):
            self = .json(wire)
        case (nil, .number(let n)):
            guard let i = Int(exactly: n) else { return nil }
            self = .int(i)
        case (nil, .string(let s)):
            self = .string(s)
        case (nil, .object(let o)) where o.count == 1 && o["set"]?.boolValue != nil:
            self = .secret(set: o["set"]!.boolValue!)
        case (nil, .array), (nil, .object):
            self = .json(wire)
        default:
            return nil
        }
    }

    /// The value as sent in `settings.set` (a secret's state is never sent).
    public var wire: JSONValue? {
        switch self {
        case .bool(let b): return .bool(b)
        case .int(let i): return .number(Double(i))
        case .string(let s): return .string(s)
        case .secret: return nil
        case .json(let j): return j
        }
    }

    public var boolValue: Bool? { if case .bool(let b) = self { return b }; return nil }
    public var intValue: Int? { if case .int(let i) = self { return i }; return nil }
    public var stringValue: String? { if case .string(let s) = self { return s }; return nil }
}

public struct SettingOption: Sendable, Equatable, Hashable {
    /// The wire value (a string or an integer).
    public var value: SettingValue
    public var label: String

    public init(value: SettingValue, label: String) {
        self.value = value
        self.label = label
    }
}

/// One schema item (`settings.schema` `items[]`).
public struct SettingItem: Sendable, Equatable, Identifiable {
    public enum Kind: String, Sendable {
        case bool, int, string, secret, list, object
        case enumeration = "enum"
    }

    public enum Apply: String, Sendable {
        case live, tick, theme, sleep, reboot
    }

    public var id: String
    public var group: String
    public var title: String
    public var kind: Kind
    public var min: Int?
    public var max: Int?
    public var step: Int?
    public var options: [SettingOption]
    public var maxLen: Int?
    public var apply: Apply
    public var secret: Bool
    public var danger: Bool
    public var readOnly: Bool
    public var hint: String?

    /// Build an item locally (tests, or a catalog the app ships itself).
    public init(id: String, group: String, title: String, kind: Kind,
                min: Int? = nil, max: Int? = nil, step: Int? = nil,
                options: [SettingOption] = [], maxLen: Int? = nil,
                apply: Apply = .live, secret: Bool = false, danger: Bool = false,
                readOnly: Bool = false, hint: String? = nil) {
        self.id = id
        self.group = group
        self.title = title
        self.kind = kind
        self.min = min
        self.max = max
        self.step = step
        self.options = options
        self.maxLen = maxLen
        self.apply = apply
        self.secret = secret
        self.danger = danger
        self.readOnly = readOnly
        self.hint = hint
    }

    init?(_ json: JSONValue) {
        guard let id = json["id"]?.stringValue,
              let group = json["group"]?.stringValue,
              let title = json["title"]?.stringValue,
              let kind = json["type"]?.stringValue.flatMap(Kind.init(rawValue:))
        else { return nil }
        self.id = id
        self.group = group
        self.title = title
        self.kind = kind
        min = json["min"]?.intValue
        max = json["max"]?.intValue
        step = json["step"]?.intValue
        if case .array(let opts)? = json["enum"] {
            options = opts.compactMap { o in
                guard let v = o["v"], let value = SettingValue(wire: v, type: .enumeration) else { return nil }
                let label = o["label"]?.stringValue ?? value.stringValue ?? value.intValue.map(String.init) ?? ""
                return SettingOption(value: value, label: label)
            }
        } else {
            options = []
        }
        maxLen = json["max_len"]?.intValue
        apply = json["apply"]?.stringValue.flatMap(Apply.init(rawValue:)) ?? .live
        secret = json["secret"]?.boolValue ?? false
        danger = json["danger"]?.boolValue ?? false
        readOnly = json["ro"]?.boolValue ?? false
        hint = json["hint"]?.stringValue
    }

    /// The same checks the pager makes (settings_registry.cpp `validate`), so
    /// the app can refuse a value before sending it. nil = valid.
    public func problem(with value: SettingValue) -> String? {
        if readOnly { return "read-only" }
        switch (kind, value) {
        case (.bool, .bool):
            return nil
        case (.int, .int(let i)):
            if let min, i < min { return "out of range" }
            if let max, i > max { return "out of range" }
            if let step, step > 1, (i - (min ?? 0)) % step != 0 { return "not a multiple of the step" }
            return nil
        case (.enumeration, _):
            return options.contains { $0.value == value } ? nil : "not one of the choices"
        case (.string, .string(let s)), (.secret, .string(let s)):
            if s.utf8.count > (maxLen ?? 255) { return "too long" }
            if s.unicodeScalars.contains(where: { $0.value < 0x20 || $0.value == 0x7F }) {
                return "control character"
            }
            return nil
        case (.list, .json(.array)), (.object, .json(.object)):
            return nil
        default:
            return "wrong type"
        }
    }
}

public struct SettingGroup: Sendable, Equatable, Identifiable {
    public var id: String
    public var title: String

    public init(id: String, title: String) {
        self.id = id
        self.title = title
    }
}

/// A one-shot command (`settings.action`).
public struct SettingAction: Sendable, Equatable, Identifiable {
    public var id: String
    public var group: String
    public var title: String
    public var danger: Bool
    public var hint: String?

    public init(id: String, group: String, title: String, danger: Bool = false, hint: String? = nil) {
        self.id = id
        self.group = group
        self.title = title
        self.danger = danger
        self.hint = hint
    }
}

/// Every page of `settings.schema`, merged.
public struct SettingsSchema: Sendable, Equatable {
    public var revSchema: String
    public var groups: [SettingGroup]
    public var actions: [SettingAction]
    public var items: [SettingItem]

    public init(revSchema: String, groups: [SettingGroup], actions: [SettingAction], items: [SettingItem]) {
        self.revSchema = revSchema
        self.groups = groups
        self.actions = actions
        self.items = items
    }

    public func item(_ id: String) -> SettingItem? { items.first { $0.id == id } }
    public func items(in group: String) -> [SettingItem] { items.filter { $0.group == group } }
    public func actions(in group: String) -> [SettingAction] { actions.filter { $0.group == group } }
}

/// A `settings.get` result.
public struct SettingsSnapshot: Sendable, Equatable {
    public var rev: UInt32
    public var revSchema: String
    public var values: [String: SettingValue]

    public init(rev: UInt32, revSchema: String, values: [String: SettingValue]) {
        self.rev = rev
        self.revSchema = revSchema
        self.values = values
    }
}

/// A `settings.set` result.
public struct SettingsSetResult: Sendable, Equatable {
    public var rev: UInt32
    public var applied: [String]
    public var reboot: Bool

    public init(rev: UInt32, applied: [String], reboot: Bool) {
        self.rev = rev
        self.applied = applied
        self.reboot = reboot
    }
}

/// `evt.settings.changed`.
public struct SettingsChanged: Sendable, Equatable {
    public static let event = "evt.settings.changed"
    public var rev: UInt32
    public var ids: [String]

    public init?(params: JSONValue) {
        guard let rev = params["rev"]?.doubleValue, case .array(let ids)? = params["ids"] else { return nil }
        self.rev = UInt32(rev)
        self.ids = ids.compactMap(\.stringValue)
    }
}

/// `evt.settings.job`.
public struct SettingsJob: Sendable, Equatable {
    public static let event = "evt.settings.job"
    public var job: String
    public var done: Bool
    public var ok: Bool
    public var msg: String
    public var result: JSONValue?

    public init?(params: JSONValue) {
        guard let job = params["job"]?.stringValue else { return nil }
        self.job = job
        done = params["done"]?.boolValue ?? false
        ok = params["ok"]?.boolValue ?? false
        msg = params["msg"]?.stringValue ?? ""
        result = params["result"]
    }
}

public enum SettingsError: Error, Equatable {
    /// `if_rev` was stale: the pager changed since the last read.
    case changed
    /// The pager answered with something this client cannot read.
    case badReply(String)
    /// An action's job ended with `ok:false`.
    case jobFailed(String)
}

/// `settings.*` calls with a schema cache keyed by `rev_schema`.
public actor SettingsService {
    /// The capability that gates all of this (`sys.hello` caps).
    public static let cap = "settings"
    /// A page loop that never ends is a pager bug; stop there.
    static let maxPages = 64

    private let session: LinkSession
    private var cached: SettingsSchema?
    private var jobWaiters: [String: CheckedContinuation<SettingsJob, Error>] = [:]
    private var earlyJobs: [String: SettingsJob] = [:]

    /// Schema pages fetched so far (tests read it to check the cache).
    public private(set) var schemaFetches = 0

    public init(session: LinkSession, cachedSchema: SettingsSchema? = nil) {
        self.session = session
        self.cached = cachedSchema
    }

    /// The schema, from the cache when its `rev_schema` is `expected` (or no
    /// revision is given and one is cached), else fetched page by page.
    public func schema(expected revSchema: String? = nil) async throws -> SettingsSchema {
        if let cached, revSchema == nil || cached.revSchema == revSchema { return cached }
        var params: [String: JSONValue] = [:]
        var merged: SettingsSchema?
        for _ in 0..<Self.maxPages {
            let page = try await session.call("settings.schema", params: params)
            schemaFetches += 1
            guard let rs = page["rev_schema"]?.stringValue, case .array(let items)? = page["items"] else {
                throw SettingsError.badReply("settings.schema")
            }
            if merged == nil {
                var groups: [SettingGroup] = []
                if case .array(let gs)? = page["groups"] {
                    groups = gs.compactMap { g in
                        guard let id = g["id"]?.stringValue, let title = g["title"]?.stringValue else { return nil }
                        return SettingGroup(id: id, title: title)
                    }
                }
                var actions: [SettingAction] = []
                if case .array(let acts)? = page["actions"] {
                    actions = acts.compactMap { a in
                        guard let id = a["id"]?.stringValue, let group = a["group"]?.stringValue,
                              let title = a["title"]?.stringValue else { return nil }
                        return SettingAction(id: id, group: group, title: title,
                                             danger: a["danger"]?.boolValue ?? false,
                                             hint: a["hint"]?.stringValue)
                    }
                }
                merged = SettingsSchema(revSchema: rs, groups: groups, actions: actions, items: [])
            } else if merged?.revSchema != rs {
                // The schema changed between pages (a firmware update mid-read).
                throw SettingsError.changed
            }
            merged?.items += items.compactMap(SettingItem.init)
            guard let cursor = page["cursor"]?.stringValue else {
                cached = merged
                return merged!
            }
            params = ["cursor": .string(cursor)]
        }
        throw SettingsError.badReply("settings.schema: too many pages")
    }

    /// The cached schema, if any (for the offline view).
    public func cachedSchema() -> SettingsSchema? { cached }

    /// `settings.get` for `ids` (every setting when nil).
    public func get(ids: [String]? = nil) async throws -> SettingsSnapshot {
        var params: [String: JSONValue] = [:]
        if let ids { params["ids"] = .array(ids.map { .string($0) }) }
        let reply = try await session.call("settings.get", params: params)
        guard let rev = reply["rev"]?.doubleValue, let rs = reply["rev_schema"]?.stringValue,
              case .object(let raw)? = reply["values"] else {
            throw SettingsError.badReply("settings.get")
        }
        var values: [String: SettingValue] = [:]
        for (id, wire) in raw {
            let kind = cached?.revSchema == rs ? cached?.item(id)?.kind : nil
            if let v = SettingValue(wire: wire, type: kind) { values[id] = v }
        }
        return SettingsSnapshot(rev: UInt32(rev), revSchema: rs, values: values)
    }

    /// `settings.set`. A secret is written with `.string` ("" clears it).
    /// A stale `ifRev` throws `SettingsError.changed`.
    public func set(_ values: [String: SettingValue], ifRev: UInt32? = nil) async throws -> SettingsSetResult {
        var wire: [String: JSONValue] = [:]
        for (id, v) in values {
            guard let w = v.wire else { throw SettingsError.badReply("\(id): a secret's state can't be written") }
            wire[id] = w
        }
        var params: [String: JSONValue] = ["values": .object(wire)]
        if let ifRev { params["if_rev"] = .number(Double(ifRev)) }
        let reply: JSONValue
        do {
            reply = try await session.call("settings.set", params: params)
        } catch let e as RPCError where e.code == "invalid_argument" && e.msg == "changed" {
            throw SettingsError.changed
        }
        guard let rev = reply["rev"]?.doubleValue else { throw SettingsError.badReply("settings.set") }
        var applied: [String] = []
        if case .array(let a)? = reply["applied"] { applied = a.compactMap(\.stringValue) }
        return SettingsSetResult(rev: UInt32(rev), applied: applied, reboot: reply["reboot"]?.boolValue ?? false)
    }

    /// `settings.action`: the immediate result (`{"job"}` for a long one).
    public func action(_ id: String, params: [String: JSONValue] = [:]) async throws -> JSONValue {
        var p: [String: JSONValue] = ["id": .string(id)]
        if !params.isEmpty { p["p"] = .object(params) }
        return try await session.call("settings.action", params: p)
    }

    /// Run an action and, when it answers with a job, wait for its
    /// `evt.settings.job` (delivered through `handleEvent`). Throws
    /// `SettingsError.jobFailed` when the job reports `ok:false`.
    public func run(_ id: String, params: [String: JSONValue] = [:],
                    timeout: Duration = .seconds(120)) async throws -> JSONValue {
        let reply = try await action(id, params: params)
        guard let job = reply["job"]?.stringValue else { return reply }
        let done = try await wait(job: job, timeout: timeout)
        guard done.ok else { throw SettingsError.jobFailed(done.msg) }
        return done.result ?? .object([:])
    }

    /// Feed every pager event here (the owner holds the one events stream).
    /// Returns the decoded change notice for `evt.settings.changed`.
    @discardableResult
    public func handleEvent(name: String, params: JSONValue) -> SettingsChanged? {
        if name == SettingsJob.event, let job = SettingsJob(params: params), job.done {
            if let waiter = jobWaiters.removeValue(forKey: job.job) {
                waiter.resume(returning: job)
            } else {
                earlyJobs[job.job] = job
            }
            return nil
        }
        if name == SettingsChanged.event { return SettingsChanged(params: params) }
        return nil
    }

    private func wait(job: String, timeout: Duration) async throws -> SettingsJob {
        if let early = earlyJobs.removeValue(forKey: job) { return early }
        let timer = Task { [weak self] in
            try? await Task.sleep(for: timeout)
            await self?.expire(job)
        }
        defer { timer.cancel() }
        return try await withCheckedThrowingContinuation { jobWaiters[job] = $0 }
    }

    private func expire(_ job: String) {
        jobWaiters.removeValue(forKey: job)?.resume(throwing: RPCError(code: "timeout", msg: "\(job): no result"))
    }
}

/// The three settings calls the app's state (`SettingsStore`) needs, so the
/// store can be driven by `SettingsService` on the link or by a fake in tests.
public protocol SettingsBackend: Sendable {
    /// The catalog, refetched unless its `revSchema` is already `expected`.
    func schema(expected revSchema: String?) async throws -> SettingsSchema
    /// The current values (`ids` nil = every setting).
    func get(ids: [String]?) async throws -> SettingsSnapshot
    /// Write values; a stale `ifRev` throws `SettingsError.changed`.
    func set(_ values: [String: SettingValue], ifRev: UInt32?) async throws -> SettingsSetResult
}

extension SettingsService: SettingsBackend {}
