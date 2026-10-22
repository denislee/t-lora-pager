import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

/// A resolved place for a forecast.
public struct WeatherPlace: Sendable, Equatable {
    public var lat: Double
    public var lon: Double
    public var city: String

    public init(lat: Double, lon: Double, city: String) {
        self.lat = lat
        self.lon = lon
        self.city = city
    }
}

/// Answers the pager's `weather.get` and sends `weather.push` (PROTOCOL.md
/// §6.3) with forecasts from Open-Meteo, requested with exactly the fields the
/// firmware's direct path asks for (`src/apps/ui_weather.cpp` `fetch_weather`)
/// and reduced the same way: 8 three-hourly slots of today and 7 days, values
/// truncated toward zero like the firmware's `(int)valuedouble`.
///
/// Locations: key `"auto"` uses `locator` (the app's CoreLocation fix) and
/// falls back to IP geolocation over HTTPS; any other key is a city name
/// resolved with Open-Meteo geocoding, as the pager does.
public struct WeatherService: Sendable {
    public static let forecastURL = URL(string: "https://api.open-meteo.com/v1/forecast")!
    public static let geocodeURL = URL(string: "https://geocoding-api.open-meteo.com/v1/search")!
    public static let ipGeoURL = URL(string: "https://ipwho.is/")!
    /// Per-request timeout; three requests must fit the pager's 15 s.
    public static let requestTimeout: TimeInterval = 5
    /// How long a forecast for one location is reused (P7.60).
    public static let forecastTTL: TimeInterval = 10 * 60

    private let session: LinkSession
    private let http: URLSession
    private let locator: (@Sendable () async throws -> WeatherPlace)?
    private let now: @Sendable () -> Date
    private let cache = WeatherCache()

    public init(
        session: LinkSession,
        urlSession: URLSession = .shared,
        locator: (@Sendable () async throws -> WeatherPlace)? = nil,
        now: @escaping @Sendable () -> Date = Date.init
    ) {
        self.session = session
        self.http = urlSession
        self.locator = locator
        self.now = now
    }

    /// Install the `weather.get` handler. `onRequest` sees each requested key
    /// (the app remembers it for push-on-connect).
    public func register(onRequest: (@Sendable (String) -> Void)? = nil) async {
        let service = self
        await session.handle("weather.get") { params in
            guard let key = params["key"]?.stringValue, Self.validKey(key) else {
                throw RPCError(code: "invalid_argument", msg: "key must be 1..64 bytes")
            }
            onRequest?(key)
            return try await service.payload(key: key)
        }
    }

    /// Fetch the forecast for `key` and send it with `weather.push`.
    public func push(key: String) async throws {
        guard case .object(let params) = try await payload(key: key) else { return }
        _ = try await session.call("weather.push", params: params)
    }

    /// The ForecastPayload for `key`.
    public func payload(key: String) async throws -> JSONValue {
        guard Self.validKey(key) else {
            throw RPCError(code: "invalid_argument", msg: "key must be 1..64 bytes")
        }
        let place = try await resolve(key: key)
        let url = Self.forecastQuery(lat: place.lat, lon: place.lon)
        let cached = cache.forecast(url.absoluteString, now: now(), ttl: Self.forecastTTL)
        let body: Data
        if let cached {
            body = cached
        } else {
            body = try await get(url, stage: "forecast")
        }
        let forecast: OpenMeteoForecast
        do {
            forecast = try JSONDecoder().decode(OpenMeteoForecast.self, from: body)
        } catch {
            throw RPCError(code: "http_error", msg: "forecast: bad response")
        }
        if cached == nil { cache.store(forecast: body, for: url.absoluteString, at: now()) }
        return Self.build(key: key, place: place, forecast: forecast, ts: now())
    }

    // MARK: - Location

    func resolve(key: String) async throws -> WeatherPlace {
        if key == "auto" {
            if let locator, let place = try? await locator() {
                return place
            }
            return try await ipPlace()
        }
        return try await geocode(city: key)
    }

    private func geocode(city: String) async throws -> WeatherPlace {
        if let place = cache.place(city) { return place }
        var c = URLComponents(url: Self.geocodeURL, resolvingAgainstBaseURL: false)!
        c.queryItems = [
            URLQueryItem(name: "name", value: city),
            URLQueryItem(name: "count", value: "1"),
            URLQueryItem(name: "format", value: "json"),
        ]
        let body = try await get(c.url!, stage: "geo")
        struct Reply: Decodable {
            struct Result: Decodable { let latitude: Double; let longitude: Double }
            let results: [Result]?
        }
        guard let r = (try? JSONDecoder().decode(Reply.self, from: body))?.results?.first else {
            throw RPCError(code: "not_found", msg: "city not found")
        }
        let place = WeatherPlace(lat: r.latitude, lon: r.longitude, city: city)
        cache.store(place: place, for: city)
        return place
    }

    private func ipPlace() async throws -> WeatherPlace {
        let body = try await get(Self.ipGeoURL, stage: "geo")
        struct Reply: Decodable {
            let success: Bool?
            let city: String?
            let latitude: Double?
            let longitude: Double?
        }
        guard let r = try? JSONDecoder().decode(Reply.self, from: body), r.success != false,
              let lat = r.latitude, let lon = r.longitude else {
            throw RPCError(code: "http_error", msg: "geo: no location")
        }
        return WeatherPlace(lat: lat, lon: lon, city: r.city ?? "")
    }

    // MARK: - HTTP

    private func get(_ url: URL, stage: String) async throws -> Data {
        var request = URLRequest(url: url)
        request.timeoutInterval = Self.requestTimeout
        let reply: HTTPReply
        do {
            reply = try await HTTP.send(request, using: http)
        } catch {
            throw RPCError(code: "http_error", msg: "\(stage): \(error.localizedDescription)")
        }
        guard (200..<300).contains(reply.status) else {
            throw RPCError(code: "http_error", msg: "\(stage): HTTP \(reply.status)")
        }
        return reply.body
    }

    // MARK: - Pure helpers (tested)

    static func validKey(_ key: String) -> Bool {
        (1...64).contains(key.utf8.count)
    }

    static func forecastQuery(lat: Double, lon: Double) -> URL {
        var c = URLComponents(url: forecastURL, resolvingAgainstBaseURL: false)!
        c.queryItems = [
            URLQueryItem(name: "latitude", value: String(format: "%.3f", lat)),
            URLQueryItem(name: "longitude", value: String(format: "%.3f", lon)),
            URLQueryItem(name: "hourly", value: "temperature_2m,weathercode,precipitation_probability"),
            URLQueryItem(name: "daily", value: "weathercode,temperature_2m_max,temperature_2m_min,precipitation_probability_max"),
            URLQueryItem(name: "forecast_days", value: "7"),
            URLQueryItem(name: "timezone", value: "auto"),
        ]
        return c.url!
    }

    /// Reduce an Open-Meteo reply to a ForecastPayload, mirroring
    /// `build_hourly_cache` / `build_daily_cache` in the firmware.
    static func build(key: String, place: WeatherPlace, forecast: OpenMeteoForecast, ts: Date) -> JSONValue {
        func int(_ v: Double?) -> Int {
            guard let v, v.isFinite else { return 0 }
            return Int(v.rounded(.towardZero))
        }
        func clamp(_ v: Int, _ lo: Int, _ hi: Int) -> Int { min(max(v, lo), hi) }
        func at(_ a: [Double?]?, _ i: Int) -> Double? {
            guard let a, i < a.count else { return nil }
            return a[i]
        }

        var hourly: [JSONValue] = []
        if let h = forecast.hourly {
            var i = 0
            while i < min(h.time.count, 24), hourly.count < 8 {
                let t = h.time[i]
                let hr = t.count >= 13 ? Int(t.dropFirst(11).prefix(2)) ?? 0 : 0
                hourly.append([
                    "hr": .number(Double(clamp(hr, 0, 23))),
                    "code": .number(Double(clamp(int(at(h.weathercode, i)), 0, 255))),
                    "temp": .number(Double(clamp(int(at(h.temperature_2m, i)), -128, 127))),
                    "pop": .number(Double(clamp(int(at(h.precipitation_probability, i)), 0, 100))),
                ])
                i += 3
            }
        }

        var daily: [JSONValue] = []
        if let d = forecast.daily {
            for i in 0..<min(d.time.count, 7) where d.time[i].count == 10 {
                daily.append([
                    "date": .string(d.time[i]),
                    "code": .number(Double(clamp(int(at(d.weathercode, i)), 0, 255))),
                    "hi": .number(Double(clamp(int(at(d.temperature_2m_max, i)), -128, 127))),
                    "lo": .number(Double(clamp(int(at(d.temperature_2m_min, i)), -128, 127))),
                    "pop": .number(Double(clamp(int(at(d.precipitation_probability_max, i)), 0, 100))),
                ])
            }
        }

        var payload: [String: JSONValue] = [
            "key": .string(key),
            "ts": .number(Double(UInt32(clamping: Int(ts.timeIntervalSince1970)))),
            "hourly": .array(hourly),
            "daily": .array(daily),
        ]
        // The pager stores the location only when city, lat and lon are all
        // present and in range.
        if !place.city.isEmpty, (-90...90).contains(place.lat), (-180...180).contains(place.lon) {
            payload["city"] = .string(place.city)
            payload["lat"] = .number(place.lat)
            payload["lon"] = .number(place.lon)
        }
        return .object(payload)
    }
}

/// The subset of Open-Meteo's `/v1/forecast` reply the firmware reads.
/// Entries can be JSON null, hence the optional elements.
struct OpenMeteoForecast: Decodable {
    struct Hourly: Decodable {
        let time: [String]
        let temperature_2m: [Double?]?
        let weathercode: [Double?]?
        let precipitation_probability: [Double?]?
    }
    struct Daily: Decodable {
        let time: [String]
        let weathercode: [Double?]?
        let temperature_2m_max: [Double?]?
        let temperature_2m_min: [Double?]?
        let precipitation_probability_max: [Double?]?
    }
    let hourly: Hourly?
    let daily: Daily?
}

/// Geocoded places per city key (coordinates don't move) and forecast bodies
/// per request URL for `WeatherService.forecastTTL` (P7.60). Shared by copies
/// of one `WeatherService`.
final class WeatherCache: @unchecked Sendable {
    static let maxEntries = 32

    private let lock = NSLock()
    private var places: [String: WeatherPlace] = [:]
    private var forecasts: [String: (body: Data, at: Date)] = [:]

    func place(_ key: String) -> WeatherPlace? {
        lock.withLock { places[key] }
    }

    func store(place: WeatherPlace, for key: String) {
        lock.withLock {
            if places.count >= Self.maxEntries, places[key] == nil { places.removeAll() }
            places[key] = place
        }
    }

    func forecast(_ key: String, now: Date, ttl: TimeInterval) -> Data? {
        lock.withLock {
            guard let entry = forecasts[key] else { return nil }
            let age = now.timeIntervalSince(entry.at)
            return age >= 0 && age < ttl ? entry.body : nil
        }
    }

    func store(forecast body: Data, for key: String, at: Date) {
        lock.withLock {
            if forecasts.count >= Self.maxEntries, forecasts[key] == nil { forecasts.removeAll() }
            forecasts[key] = (body, at)
        }
    }
}
