import Foundation

/// The Diagnostics screen's export (suggestion S19): one JSON document with
/// the app version, the last `sys.hello` (firmware hash, board, caps) and
/// `sys.info`, a battery-history summary and the session log, to attach to a
/// bug report. The session log already carries counts and ids only (never
/// note text, tokens or setting values), so the report holds nothing secret.
public struct DiagnosticsReport: Sendable, Equatable {
    public var generatedAt: Date
    public var appVersion: String
    public var appBuild: String
    public var system: String
    /// Connection state as the Dashboard shows it.
    public var linkState: String
    public var hello: JSONValue?
    public var info: JSONValue?
    public var battery: BatterySummary
    public var log: [String]
    /// The latest pager screen capture (S13), when there is one.
    public var screenshot: Screenshot?

    /// A screen capture as PNG (the app encodes it; PagerKit has no image codec).
    public struct Screenshot: Sendable, Equatable {
        public var png: [UInt8]
        public var width: Int
        public var height: Int
        public var capturedAt: Date

        public init(png: [UInt8], width: Int, height: Int, capturedAt: Date) {
            self.png = png
            self.width = width
            self.height = height
            self.capturedAt = capturedAt
        }
    }

    public init(generatedAt: Date, appVersion: String, appBuild: String, system: String,
                linkState: String, hello: JSONValue?, info: JSONValue?,
                battery: BatterySummary, log: [String], screenshot: Screenshot? = nil) {
        self.generatedAt = generatedAt
        self.appVersion = appVersion
        self.appBuild = appBuild
        self.system = system
        self.linkState = linkState
        self.hello = hello
        self.info = info
        self.battery = battery
        self.log = log
        self.screenshot = screenshot
    }

    /// The firmware hash from `sys.hello` (`fw`), if known.
    public var firmware: String? { hello?["fw"]?.stringValue }

    /// The report as a JSON value (keys sorted on encode).
    public var json: JSONValue {
        var battery: [String: JSONValue] = ["count": .number(Double(self.battery.count))]
        if let first = self.battery.first { battery["first"] = .string(Self.iso(first)) }
        if let last = self.battery.last { battery["last"] = .string(Self.iso(last)) }
        if let pct = self.battery.latestPct { battery["latest_pct"] = .number(Double(pct)) }
        if let pct = self.battery.minPct { battery["min_pct"] = .number(Double(pct)) }
        if let pct = self.battery.maxPct { battery["max_pct"] = .number(Double(pct)) }
        if let rate = self.battery.dischargePctPerHour {
            battery["discharge_pct_per_hour"] = .number((rate * 100).rounded() / 100)
        }
        return .object([
            "generated_at": .string(Self.iso(generatedAt)),
            "app": .object([
                "version": .string(appVersion),
                "build": .string(appBuild),
                "system": .string(system),
            ]),
            "link": .object([
                "state": .string(linkState),
                "firmware": firmware.map(JSONValue.string) ?? .null,
            ]),
            "sys_hello": hello ?? .null,
            "sys_info": info ?? .null,
            "battery_history": .object(battery),
            "session_log": .array(log.map(JSONValue.string)),
            "screenshot": screenshot.map { shot in
                .object([
                    "w": .number(Double(shot.width)), "h": .number(Double(shot.height)),
                    "captured_at": .string(Self.iso(shot.capturedAt)),
                    "png_base64": .string(Data(shot.png).base64EncodedString()),
                ])
            } ?? .null,
        ])
    }

    /// The JSON document, pretty-printed with sorted keys.
    public func encoded() throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .prettyPrinted]
        return try encoder.encode(json)
    }

    /// `pager-diagnostics-<fw7>-<yyyyMMdd-HHmmss>.json` (UTC), a name that
    /// sorts and says which firmware it came from.
    public var fileName: String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(identifier: "UTC")
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        let fw = firmware.map { String($0.filter { $0.isLetter || $0.isNumber }.prefix(7)) } ?? "nofw"
        return "pager-diagnostics-\(fw.isEmpty ? "nofw" : fw)-\(formatter.string(from: generatedAt)).json"
    }

    static func iso(_ date: Date) -> String {
        let formatter = ISO8601DateFormatter()
        formatter.timeZone = TimeZone(identifier: "UTC")
        return formatter.string(from: date)
    }
}
