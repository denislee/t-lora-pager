import Foundation

/// The pager's weather icon for a WMO weather code, the mapping of
/// `wmo_to_icon` in `src/apps/ui_weather.cpp` (codes it does not list are
/// `unknown`), so the phone's preview shows the icon the pager draws.
public enum WeatherIcon: String, Sendable, CaseIterable {
    case sun, partly, cloud, fog, drizzle, rain, snow, thunder, unknown

    public init(wmo code: Int) {
        switch code {
        case 0: self = .sun
        case 1, 2: self = .partly
        case 3: self = .cloud
        case 45, 48: self = .fog
        case 51, 53, 55, 56, 57: self = .drizzle
        case 61, 63, 65, 66, 67, 80, 81, 82: self = .rain
        case 71, 73, 75, 77, 85, 86: self = .snow
        case 95, 96, 99: self = .thunder
        default: self = .unknown
        }
    }

    /// The SF Symbol closest to the pager's pictogram.
    public var symbolName: String {
        switch self {
        case .sun: return "sun.max.fill"
        case .partly: return "cloud.sun.fill"
        case .cloud: return "cloud.fill"
        case .fog: return "cloud.fog.fill"
        case .drizzle: return "cloud.drizzle.fill"
        case .rain: return "cloud.rain.fill"
        case .snow: return "cloud.snow.fill"
        case .thunder: return "cloud.bolt.rain.fill"
        case .unknown: return "questionmark.circle"
        }
    }

    /// A word for accessibility labels.
    public var label: String {
        switch self {
        case .sun: return "Clear"
        case .partly: return "Partly cloudy"
        case .cloud: return "Overcast"
        case .fog: return "Fog"
        case .drizzle: return "Drizzle"
        case .rain: return "Rain"
        case .snow: return "Snow"
        case .thunder: return "Thunderstorm"
        case .unknown: return "Unknown"
        }
    }
}

/// A ForecastPayload (PROTOCOL.md §6.3) read back for the phone's preview,
/// with the pager's labels (`render_cache` in `src/apps/ui_weather.cpp`):
/// hourly cells `"%02dh"`, `"%dC"`, `"%d%%"`, the slot holding "now"
/// highlighted; daily cells `"Tod"` then the weekday, `"hi/lo"`, `"pop%"`.
public struct WeatherForecast: Sendable, Equatable {
    public struct Hour: Sendable, Equatable, Identifiable {
        public let hr: Int
        public let code: Int
        public let temp: Int
        public let pop: Int

        public var id: Int { hr }
        public var icon: WeatherIcon { WeatherIcon(wmo: code) }
        public var hourLabel: String { String(format: "%02dh", hr) }
        public var tempLabel: String { "\(temp)C" }
        public var popLabel: String { "\(pop)%" }

        /// The 3-hour slot `[hr, hr+3)` contains `hour` (the pager's highlight).
        public func contains(hour: Int) -> Bool { hour >= hr && hour < hr + 3 }

        public init(hr: Int, code: Int, temp: Int, pop: Int) {
            self.hr = hr
            self.code = code
            self.temp = temp
            self.pop = pop
        }
    }

    public struct Day: Sendable, Equatable, Identifiable {
        public let date: String
        public let code: Int
        public let hi: Int
        public let lo: Int
        public let pop: Int

        public var id: String { date }
        public var icon: WeatherIcon { WeatherIcon(wmo: code) }
        public var rangeLabel: String { "\(hi)/\(lo)" }
        public var popLabel: String { "\(pop)%" }

        public init(date: String, code: Int, hi: Int, lo: Int, pop: Int) {
            self.date = date
            self.code = code
            self.hi = hi
            self.lo = lo
            self.pop = pop
        }
    }

    public let key: String
    /// Forecast time; nil for `ts` 0 (unknown clock).
    public let ts: Date?
    public let city: String?
    public let hourly: [Hour]
    public let daily: [Day]

    /// Read a ForecastPayload; nil when `key`, `hourly` or `daily` is missing.
    /// Entries missing a field are skipped (the pager would refuse the
    /// payload whole; the preview just shows what is there).
    public init?(payload: JSONValue) {
        guard let key = payload["key"]?.stringValue,
              case .array(let hours)? = payload["hourly"],
              case .array(let days)? = payload["daily"] else { return nil }
        self.key = key
        let ts = payload["ts"]?.doubleValue ?? 0
        self.ts = ts > 0 ? Date(timeIntervalSince1970: ts) : nil
        self.city = payload["city"]?.stringValue
        self.hourly = hours.prefix(8).compactMap { h in
            guard let hr = h["hr"]?.intValue, let code = h["code"]?.intValue,
                  let temp = h["temp"]?.intValue, let pop = h["pop"]?.intValue else { return nil }
            return Hour(hr: hr, code: code, temp: temp, pop: pop)
        }
        self.daily = days.prefix(7).compactMap { d in
            guard let date = d["date"]?.stringValue, let code = d["code"]?.intValue,
                  let hi = d["hi"]?.intValue, let lo = d["lo"]?.intValue,
                  let pop = d["pop"]?.intValue else { return nil }
            return Day(date: date, code: code, hi: hi, lo: lo, pop: pop)
        }
    }

    /// The daily cell's top label: `"Tod"` for the first day, else the
    /// weekday of `date` (`"---"` when it does not parse).
    public static func dayLabel(index: Int, date: String) -> String {
        if index == 0 { return "Tod" }
        let names = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]
        guard let wday = weekday(date) else { return "---" }
        return names[wday]
    }

    /// 0 = Sunday … 6 = Saturday for `"YYYY-MM-DD"` (Zeller's congruence, as
    /// the firmware's `weekday_from_date`).
    public static func weekday(_ date: String) -> Int? {
        let parts = date.split(separator: "-")
        guard date.count >= 10, parts.count == 3,
              var y = Int(parts[0]), var m = Int(parts[1]), let d = Int(parts[2]),
              (1...12).contains(m), (1...31).contains(d) else { return nil }
        if m < 3 {
            m += 12
            y -= 1
        }
        let k = y % 100
        let j = y / 100
        let h = (d + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7
        return (h + 6) % 7
    }
}
