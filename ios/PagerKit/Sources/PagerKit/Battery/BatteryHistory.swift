import Foundation

/// One battery reading of the pager.
public struct BatterySample: Codable, Sendable, Equatable, Identifiable {
    /// Which message carried it.
    public enum Source: String, Codable, Sendable {
        /// `evt.battery` (a ≥ 5 % change or a charge-state flip).
        case event
        /// `sys.info` (on connect and on Refresh).
        case info
    }

    public var at: Date
    public var pct: Int
    public var charging: Bool
    public var source: Source

    public var id: Date { at }

    public init(at: Date, pct: Int, charging: Bool, source: Source) {
        self.at = at
        self.pct = pct
        self.charging = charging
        self.source = source
    }

    /// From `evt.battery` params `{"pct","charging"}`; nil when `pct` is
    /// missing or outside 0…100.
    public static func fromEvent(_ params: JSONValue, at: Date) -> BatterySample? {
        guard let pct = params["pct"]?.intValue, (0...100).contains(pct) else { return nil }
        return BatterySample(at: at, pct: pct, charging: params["charging"]?.boolValue ?? false, source: .event)
    }

    /// From a `sys.info` result (`batt_pct`, `charging`).
    public static func fromInfo(_ info: JSONValue, at: Date) -> BatterySample? {
        guard let pct = info["batt_pct"]?.intValue, (0...100).contains(pct) else { return nil }
        return BatterySample(at: at, pct: pct, charging: info["charging"]?.boolValue ?? false, source: .info)
    }
}

/// What the Diagnostics export and the chart header say about the history.
public struct BatterySummary: Sendable, Equatable, Codable {
    public var count: Int
    public var first: Date?
    public var last: Date?
    public var latestPct: Int?
    public var minPct: Int?
    public var maxPct: Int?
    /// Percent per hour lost over the latest discharge run (not charging),
    /// when it spans at least `BatteryHistoryStore.minRateSpan`.
    public var dischargePctPerHour: Double?

    public init(count: Int = 0, first: Date? = nil, last: Date? = nil, latestPct: Int? = nil,
                minPct: Int? = nil, maxPct: Int? = nil, dischargePctPerHour: Double? = nil) {
        self.count = count
        self.first = first
        self.last = last
        self.latestPct = latestPct
        self.minPct = minPct
        self.maxPct = maxPct
        self.dischargePctPerHour = dischargePctPerHour
    }
}

/// The phone's log of the pager's battery (`evt.battery`, and `sys.info` on
/// connect), to chart it over days and catch power regressions from a
/// firmware change. A ring persisted as one JSON file: samples older than
/// `maxAge` (30 days) and beyond `maxSamples` are dropped, and a reading
/// identical to the previous one within `dedupeWindow` is not stored again.
/// Thread-safe.
public final class BatteryHistoryStore: @unchecked Sendable {
    public static let defaultMaxAge: TimeInterval = 30 * 24 * 3600
    public static let defaultMaxSamples = 20_000
    public static let dedupeWindow: TimeInterval = 5 * 60
    /// Shortest discharge run a rate is computed over.
    public static let minRateSpan: TimeInterval = 30 * 60

    private let lock = NSLock()
    private let file: JSONFile<[BatterySample]>
    private let maxAge: TimeInterval
    private let maxSamples: Int
    private var samples: [BatterySample]

    /// `Application Support/battery-history.json` in the app container.
    public static func defaultURL() -> URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? FileManager.default.temporaryDirectory
        return base.appendingPathComponent("battery-history.json")
    }

    public init(url: URL = BatteryHistoryStore.defaultURL(),
                maxAge: TimeInterval = BatteryHistoryStore.defaultMaxAge,
                maxSamples: Int = BatteryHistoryStore.defaultMaxSamples) {
        self.file = JSONFile(url: url)
        self.maxAge = maxAge
        self.maxSamples = max(1, maxSamples)
        self.samples = (file.load() ?? []).sorted { $0.at < $1.at }
    }

    /// Every kept sample, oldest first.
    public var all: [BatterySample] { lock.withLock { samples } }

    /// Samples at or after `date`, oldest first.
    public func samples(since date: Date) -> [BatterySample] {
        lock.withLock { samples.filter { $0.at >= date } }
    }

    /// Store `sample` (unless it repeats the last one within the dedupe
    /// window), prune by age relative to it and by count, and persist.
    /// Returns whether it was stored.
    @discardableResult
    public func record(_ sample: BatterySample) -> Bool {
        lock.withLock {
            if let last = samples.last, last.pct == sample.pct, last.charging == sample.charging,
               sample.at.timeIntervalSince(last.at) < Self.dedupeWindow,
               sample.at >= last.at {
                return false
            }
            samples.append(sample)
            if samples.count > 1, samples[samples.count - 2].at > sample.at {
                samples.sort { $0.at < $1.at }
            }
            let cutoff = (samples.last?.at ?? sample.at).addingTimeInterval(-maxAge)
            samples.removeAll { $0.at < cutoff }
            if samples.count > maxSamples { samples.removeFirst(samples.count - maxSamples) }
            try? file.save(samples)
            return true
        }
    }

    /// Forget everything (and delete the file).
    public func clear() {
        lock.withLock {
            samples.removeAll()
            file.remove()
        }
    }

    public func summary() -> BatterySummary {
        Self.summary(all)
    }

    /// Count, span, extremes, the latest reading and the latest discharge
    /// rate of `samples` (oldest first).
    public static func summary(_ samples: [BatterySample]) -> BatterySummary {
        guard let first = samples.first, let last = samples.last else { return BatterySummary() }
        var summary = BatterySummary(
            count: samples.count, first: first.at, last: last.at, latestPct: last.pct,
            minPct: samples.map(\.pct).min(), maxPct: samples.map(\.pct).max()
        )
        // The trailing run of not-charging samples.
        var run: [BatterySample] = []
        for sample in samples.reversed() {
            guard !sample.charging else { break }
            run.insert(sample, at: 0)
        }
        if let start = run.first, let end = run.last {
            let span = end.at.timeIntervalSince(start.at)
            if span >= minRateSpan {
                summary.dischargePctPerHour = Double(start.pct - end.pct) / (span / 3600)
            }
        }
        return summary
    }
}
