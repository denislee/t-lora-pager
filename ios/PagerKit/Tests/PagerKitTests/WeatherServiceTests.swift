import Foundation
import XCTest

@testable import PagerKit

/// `WeatherService.build` must produce what the firmware's direct path would
/// cache (`build_hourly_cache` / `build_daily_cache`) and stay inside the
/// ForecastPayload ranges `weather_payload_parse` enforces (PROTOCOL.md §6.3).
final class WeatherServiceTests: XCTestCase {
    private func forecast() throws -> OpenMeteoForecast {
        var times: [String] = []
        var temps: [String] = []
        var codes: [String] = []
        var pops: [String] = []
        for h in 0..<48 {
            times.append("\"2026-10-\(h < 24 ? 11 : 12)T\(String(format: "%02d", h % 24)):00\"")
            temps.append(h == 3 ? "-2.7" : h == 6 ? "200.0" : h == 9 ? "null" : "\(Double(h) + 0.9)")
            codes.append("\(h == 12 ? 300 : 61)")
            pops.append("\(h == 15 ? 150 : h)")
        }
        let days = (11...18).map { "\"2026-10-\($0)\"" }
        let json = """
            {"hourly":{"time":[\(times.joined(separator: ","))],
              "temperature_2m":[\(temps.joined(separator: ","))],
              "weathercode":[\(codes.joined(separator: ","))],
              "precipitation_probability":[\(pops.joined(separator: ","))]},
             "daily":{"time":[\(days.joined(separator: ","))],
              "weathercode":[0,1,2,3,45,61,95,99],
              "temperature_2m_max":[28.6,27,26,25,24,23,22,21],
              "temperature_2m_min":[17.9,16,15,14,13,12,11,10],
              "precipitation_probability_max":[0,10,20,30,40,50,60,70]}}
            """
        return try JSONDecoder().decode(OpenMeteoForecast.self, from: Data(json.utf8))
    }

    func testBuildMatchesFirmwareReduction() throws {
        let place = WeatherPlace(lat: -23.55, lon: -46.63, city: "São Paulo")
        let p = WeatherService.build(key: "auto", place: place, forecast: try forecast(),
                                     ts: Date(timeIntervalSince1970: 1_760_000_000))

        XCTAssertEqual(p["key"]?.stringValue, "auto")
        XCTAssertEqual(p["ts"]?.intValue, 1_760_000_000)
        XCTAssertEqual(p["city"]?.stringValue, "São Paulo")
        XCTAssertEqual(p["lat"]?.doubleValue, -23.55)

        guard case .array(let hourly) = p["hourly"], case .array(let daily) = p["daily"] else {
            return XCTFail("missing arrays")
        }
        // Every third hour of today only: 00, 03, …, 21.
        XCTAssertEqual(hourly.map { $0["hr"]?.intValue }, [0, 3, 6, 9, 12, 15, 18, 21])
        XCTAssertEqual(hourly[0]["temp"]?.intValue, 0)     // 0.9 truncates
        XCTAssertEqual(hourly[1]["temp"]?.intValue, -2)    // toward zero, like (int)
        XCTAssertEqual(hourly[2]["temp"]?.intValue, 127)   // clamped to int8
        XCTAssertEqual(hourly[3]["temp"]?.intValue, 0)     // null → 0
        XCTAssertEqual(hourly[4]["code"]?.intValue, 255)   // clamped
        XCTAssertEqual(hourly[5]["pop"]?.intValue, 100)    // clamped

        XCTAssertEqual(daily.count, 7)
        XCTAssertEqual(daily[0]["date"]?.stringValue, "2026-10-11")
        XCTAssertEqual(daily[0]["hi"]?.intValue, 28)
        XCTAssertEqual(daily[0]["lo"]?.intValue, 17)
        XCTAssertEqual(daily[6]["code"]?.intValue, 95)
    }

    func testLocationOmittedWithoutCity() throws {
        let p = WeatherService.build(key: "auto", place: WeatherPlace(lat: 1, lon: 2, city: ""),
                                     forecast: try forecast(), ts: Date())
        XCTAssertNil(p["city"])
        XCTAssertNil(p["lat"])
        XCTAssertNil(p["lon"])
    }

    func testForecastQueryMatchesFirmware() {
        let url = WeatherService.forecastQuery(lat: -23.5507, lon: -46.6333).absoluteString
        XCTAssertTrue(url.hasPrefix("https://api.open-meteo.com/v1/forecast?latitude=-23.551&longitude=-46.633"))
        XCTAssertTrue(url.contains("hourly=temperature_2m,weathercode,precipitation_probability"))
        XCTAssertTrue(url.contains("forecast_days=7&timezone=auto"))
    }

    func testKeyBounds() {
        XCTAssertFalse(WeatherService.validKey(""))
        XCTAssertTrue(WeatherService.validKey("auto"))
        XCTAssertTrue(WeatherService.validKey(String(repeating: "a", count: 64)))
        XCTAssertFalse(WeatherService.validKey(String(repeating: "a", count: 65)))
    }

    // MARK: - City search

    func testSearchCitiesDecodesLabels() async throws {
        let json = #"""
            {"results":[
              {"name":"Recife","latitude":-8.05,"longitude":-34.9,"admin1":"Pernambuco","country":"Brazil"},
              {"name":"Recife","latitude":-3.0,"longitude":-40.0,"country":"Brazil"}]}
            """#
        let server = StubServer { _ in .json(200, json) }
        let matches = try await WeatherService.searchCities("Recife", urlSession: StubURLProtocol.session(server))

        XCTAssertEqual(matches.count, 2)
        XCTAssertEqual(matches[0].name, "Recife")
        XCTAssertEqual(matches[0].label, "Recife, Pernambuco, Brazil")
        XCTAssertEqual(matches[0].lat, -8.05)
        XCTAssertEqual(matches[0].lon, -34.9)
        XCTAssertEqual(matches[1].label, "Recife, Brazil")
        XCTAssertNotEqual(matches[0].id, matches[1].id)

        let request = try XCTUnwrap(server.requests(path: "/v1/search").first)
        XCTAssertEqual(request.method, "GET")
        let query = try XCTUnwrap(request.query)
        XCTAssertTrue(query.contains("name=Recife"))
        XCTAssertTrue(query.contains("count=10"))
        XCTAssertTrue(query.contains("language=en"))
        XCTAssertTrue(query.contains("format=json"))
    }

    func testSearchCitiesEmptyBodyGivesNoMatches() async throws {
        let server = StubServer { _ in .json(200, "{}") }
        let matches = try await WeatherService.searchCities("Nowhere", urlSession: StubURLProtocol.session(server))
        XCTAssertTrue(matches.isEmpty)
    }

    // MARK: - Caching (P7.60)

    /// A minimal reply the pager can reduce (see `OpenMeteoForecast`).
    private static let forecastBody = #"""
        {"hourly":{"time":["2026-10-22T00:00"],"temperature_2m":[25],
          "weathercode":[1],"precipitation_probability":[10]},
         "daily":{"time":["2026-10-22"],"weathercode":[1],
          "temperature_2m_max":[30],"temperature_2m_min":[22],
          "precipitation_probability_max":[20]}}
        """#

    private static func answer(_ request: StubRequest) -> StubResponse {
        if request.path.hasSuffix("/v1/search") {
            return .json(200, #"{"results":[{"latitude":-8.05,"longitude":-34.9}]}"#)
        }
        if request.path.hasSuffix("/v1/forecast") {
            return .json(200, forecastBody)
        }
        return StubResponse(status: 404)
    }

    private func service(_ server: StubServer, now: @escaping @Sendable () -> Date) -> WeatherService {
        let (a, _) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        return WeatherService(session: LinkSession(transport: a),
                              urlSession: StubURLProtocol.session(server),
                              now: now)
    }

    func testGeocodeCachedPerKey() async throws {
        let clock = DateBox(Date(timeIntervalSince1970: 1_792_000_000))
        let server = StubServer { Self.answer($0) }
        let weather = service(server, now: { clock.now })

        _ = try await weather.payload(key: "Recife")
        _ = try await weather.payload(key: "Recife")
        XCTAssertEqual(server.requests(path: "/v1/search").count, 1)

        _ = try await weather.payload(key: "Olinda")
        XCTAssertEqual(server.requests(path: "/v1/search").count, 2)
    }

    func testForecastCachedForTenMinutes() async throws {
        let start = Date(timeIntervalSince1970: 1_792_000_000)
        let clock = DateBox(start)
        let server = StubServer { Self.answer($0) }
        let weather = service(server, now: { clock.now })

        _ = try await weather.payload(key: "Recife")
        _ = try await weather.payload(key: "Recife")
        XCTAssertEqual(server.requests(path: "/v1/forecast").count, 1)

        clock.set(start.addingTimeInterval(599))
        _ = try await weather.payload(key: "Recife")
        XCTAssertEqual(server.requests(path: "/v1/forecast").count, 1)

        clock.set(start.addingTimeInterval(601))
        _ = try await weather.payload(key: "Recife")
        XCTAssertEqual(server.requests(path: "/v1/forecast").count, 2)
    }

    func testFailedForecastNotCached() async throws {
        let clock = DateBox(Date(timeIntervalSince1970: 1_792_000_000))
        let counter = CounterBox()
        let server = StubServer { request in
            if request.path.hasSuffix("/v1/forecast") {
                return counter.next() == 1 ? StubResponse(status: 500) : Self.answer(request)
            }
            return Self.answer(request)
        }
        let weather = service(server, now: { clock.now })

        do {
            _ = try await weather.payload(key: "Recife")
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "http_error")
        }

        let payload = try await weather.payload(key: "Recife")
        XCTAssertEqual(payload["key"]?.stringValue, "Recife")
        XCTAssertEqual(server.requests(path: "/v1/forecast").count, 2)
    }
}

/// The clock `WeatherService` reads, mutable from the test and `Sendable` for
/// the escaping `now` closure.
private final class DateBox: @unchecked Sendable {
    private let lock = NSLock()
    private var value: Date

    init(_ value: Date) {
        self.value = value
    }

    var now: Date {
        lock.withLock { value }
    }

    func set(_ date: Date) {
        lock.withLock { value = date }
    }
}

/// A monotonic counter for `StubServer`'s synchronous handler.
private final class CounterBox: @unchecked Sendable {
    private let lock = NSLock()
    private var value = 0

    /// The next count, starting at 1.
    func next() -> Int {
        lock.withLock {
            value += 1
            return value
        }
    }
}
