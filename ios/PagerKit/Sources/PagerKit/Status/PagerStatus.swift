import Foundation

/// The last-known pager state the Home / Lock Screen widget shows (S16):
/// connection, battery and when it was last heard from. The app writes it
/// to the App Group's `UserDefaults` on connect, `sys.info`, `evt.battery`
/// and disconnect; the widget extension only reads it.
public struct PagerStatusSnapshot: Codable, Sendable, Equatable {
    public enum Link: String, Codable, Sendable {
        case connected
        case connecting
        case disconnected
    }

    public var link: Link
    /// The pager's `sys.hello` `name` (e.g. `lilygo-pager`), when known.
    public var name: String?
    public var batteryPct: Int?
    public var charging: Bool?
    /// When the battery reading was taken.
    public var batteryAt: Date?
    /// When this snapshot was written.
    public var updatedAt: Date

    public init(link: Link, name: String? = nil, batteryPct: Int? = nil, charging: Bool? = nil,
                batteryAt: Date? = nil, updatedAt: Date) {
        self.link = link
        self.name = name
        self.batteryPct = batteryPct
        self.charging = charging
        self.batteryAt = batteryAt
        self.updatedAt = updatedAt
    }

    /// The same snapshot with a new battery reading.
    public func with(battery sample: BatterySample) -> PagerStatusSnapshot {
        var copy = self
        copy.batteryPct = sample.pct
        copy.charging = sample.charging
        copy.batteryAt = sample.at
        copy.updatedAt = max(updatedAt, sample.at)
        return copy
    }

    /// The same snapshot in another link state (battery kept: it is the
    /// last known reading).
    public func with(link: Link, name: String? = nil, at: Date) -> PagerStatusSnapshot {
        var copy = self
        copy.link = link
        if let name { copy.name = name }
        copy.updatedAt = at
        return copy
    }
}

/// Reads and writes `PagerStatusSnapshot` as JSON under one key of a
/// `UserDefaults` (the App Group suite in the app and the widget; any suite
/// in tests).
public struct PagerStatusStore: @unchecked Sendable {
    /// The App Group both targets carry (`project.yml` entitlements). The
    /// bundle id prefix is per developer, so the group id is built from it
    /// at runtime by the app; this is the suffix.
    public static let appGroupSuffix = "pagercompanion.shared"
    public static let key = "PagerStatus.snapshot"

    private let defaults: UserDefaults

    public init(defaults: UserDefaults) {
        self.defaults = defaults
    }

    /// `group.<bundle id prefix>.pagercompanion.shared`, given the bundle id
    /// of the app (`<prefix>.pagercompanion`) or of one of its extensions
    /// (`….widgets`, `….share`).
    public static func appGroupID(bundleID: String) -> String {
        var parts = bundleID.split(separator: ".").map(String.init)
        while let last = parts.last, ["widgets", "share", "pagercompanion"].contains(last) { parts.removeLast() }
        let prefix = parts.joined(separator: ".")
        return "group." + (prefix.isEmpty ? "" : prefix + ".") + appGroupSuffix
    }

    public func load() -> PagerStatusSnapshot? {
        guard let data = defaults.data(forKey: Self.key) else { return nil }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .secondsSince1970
        return try? decoder.decode(PagerStatusSnapshot.self, from: data)
    }

    public func save(_ snapshot: PagerStatusSnapshot) {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .secondsSince1970
        guard let data = try? encoder.encode(snapshot) else { return }
        defaults.set(data, forKey: Self.key)
    }
}

/// One `evt.notes.sync` progress message (PROTOCOL.md §6), read back for the
/// notes-sync Live Activity. Counts are cumulative within the job.
public struct NotesSyncProgressEvent: Sendable, Equatable, Codable {
    public var job: Int
    public var phase: String
    public var uploaded: Int
    public var already: Int
    public var deleted: Int?
    public var kept: Int?
    public var errors: Int
    public var done: Bool

    public init(job: Int, phase: String, uploaded: Int, already: Int, deleted: Int? = nil,
                kept: Int? = nil, errors: Int, done: Bool) {
        self.job = job
        self.phase = phase
        self.uploaded = uploaded
        self.already = already
        self.deleted = deleted
        self.kept = kept
        self.errors = errors
        self.done = done
    }

    /// Nil when `job`, `phase` or `done` is missing.
    public init?(params: JSONValue) {
        guard let job = params["job"]?.intValue, let phase = params["phase"]?.stringValue,
              let done = params["done"]?.boolValue else { return nil }
        var errors = 0
        if case .array(let list)? = params["errors"] { errors = list.count }
        self.init(job: job, phase: phase, uploaded: params["uploaded"]?.intValue ?? 0,
                  already: params["already"]?.intValue ?? 0, deleted: params["deleted"]?.intValue,
                  kept: params["kept"]?.intValue, errors: errors, done: done)
    }

    /// One line for the Live Activity.
    public var summary: String {
        var parts = ["\(uploaded) up", "\(already) already"]
        if let deleted { parts.append("\(deleted) moved") }
        if let kept, kept > 0 { parts.append("\(kept) kept") }
        if errors > 0 { parts.append("\(errors) errors") }
        return parts.joined(separator: " · ")
    }

    /// The phase as a word (`upload` → "Uploading", …).
    public var phaseLabel: String {
        if done { return errors > 0 ? "Finished with errors" : "Done" }
        switch phase {
        case "pull": return "Pulling from pager"
        case "upload", "commit": return "Uploading"
        case "verify": return "Verifying"
        case "delete": return "Removing from pager"
        default: return phase.prefix(1).uppercased() + phase.dropFirst()
        }
    }
}
