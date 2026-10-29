import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
import XCTest

@testable import PagerKit

/// The Weather screen's PagerKit side: the pager's WMO icon mapping and
/// labels (`src/apps/ui_weather.cpp`), reading a ForecastPayload back, and
/// `WeatherService.activity()` (last fetch with its location source, last
/// push, last pager request).
final class WeatherPreviewTests: XCTestCase {
    func testIconMappingMatchesFirmware() {
        XCTAssertEqual(WeatherIcon(wmo: 0), .sun)
        XCTAssertEqual([1, 2].map(WeatherIcon.init(wmo:)), [.partly, .partly])
        XCTAssertEqual(WeatherIcon(wmo: 3), .cloud)
        XCTAssertEqual([45, 48].map(WeatherIcon.init(wmo:)), [.fog, .fog])
        XCTAssertEqual([51, 53, 55, 56, 57].map(WeatherIcon.init(wmo:)), Array(repeating: .drizzle, count: 5))
        XCTAssertEqual([61, 63, 65, 66, 67, 80, 81, 82].map(WeatherIcon.init(wmo:)), Array(repeating: .rain, count: 8))
        XCTAssertEqual([71, 73, 75, 77, 85, 86].map(WeatherIcon.init(wmo:)), Array(repeating: .snow, count: 6))
        XCTAssertEqual([95, 96, 99].map(WeatherIcon.init(wmo:)), Array(repeating: .thunder, count: 3))
        for code in [4, 50, 60, 255] { XCTAssertEqual(WeatherIcon(wmo: code), .unknown, "\(code)") }
        XCTAssertTrue(WeatherIcon.allCases.allSatisfy { !$0.symbolName.isEmpty && !$0.label.isEmpty })
    }

    func testWeekdayAndLabels() {
        XCTAssertEqual(WeatherForecast.weekday("2026-10-29"), 4)  // Thursday
        XCTAssertEqual(WeatherForecast.weekday("2024-02-29"), 4)  // leap day, Thursday
        XCTAssertEqual(WeatherForecast.weekday("2026-01-04"), 0)  // Sunday (Zeller m<3 branch)
        XCTAssertNil(WeatherForecast.weekday("2026-13-01"))
        XCTAssertNil(WeatherForecast.weekday("bad"))
        XCTAssertEqual(WeatherForecast.dayLabel(index: 0, date: "2026-10-29"), "Tod")
        XCTAssertEqual(WeatherForecast.dayLabel(index: 1, date: "2026-10-30"), "Fri")
        XCTAssertEqual(WeatherForecast.dayLabel(index: 2, date: "x"), "---")

        let hour = WeatherForecast.Hour(hr: 9, code: 61, temp: -3, pop: 40)
        XCTAssertEqual(hour.hourLabel, "09h")
        XCTAssertEqual(hour.tempLabel, "-3C")
        XCTAssertEqual(hour.popLabel, "40%")
        XCTAssertTrue(hour.contains(hour: 11))
        XCTAssertFalse(hour.contains(hour: 12))
        XCTAssertEqual(WeatherForecast.Day(date: "2026-10-29", code: 0, hi: 28, lo: 17, pop: 5).rangeLabel, "28/17")
    }

    func testForecastReadsBackABuiltPayload() throws {
        let json = """
            {"hourly":{"time":["2026-10-29T00:00","2026-10-29T01:00","2026-10-29T02:00","2026-10-29T03:00"],
              "temperature_2m":[18.2,18,17,16.9],"weathercode":[0,0,0,3],"precipitation_probability":[0,0,0,10]},
             "daily":{"time":["2026-10-29","2026-10-30"],"weathercode":[2,95],
              "temperature_2m_max":[28.6,30],"temperature_2m_min":[17.9,20],"precipitation_probability_max":[0,80]}}
            """
        let forecast = try JSONDecoder().decode(OpenMeteoForecast.self, from: Data(json.utf8))
        let payload = WeatherService.build(key: "Recife", place: WeatherPlace(lat: -8, lon: -35, city: "Recife"),
                                           forecast: forecast, ts: Date(timeIntervalSince1970: 1_761_700_000))
        let parsed = try XCTUnwrap(WeatherForecast(payload: payload))
        XCTAssertEqual(parsed.key, "Recife")
        XCTAssertEqual(parsed.city, "Recife")
        XCTAssertEqual(parsed.ts, Date(timeIntervalSince1970: 1_761_700_000))
        XCTAssertEqual(parsed.hourly.map(\.hr), [0, 3])
        XCTAssertEqual(parsed.hourly.map(\.icon), [.sun, .cloud])
        XCTAssertEqual(parsed.daily.map(\.icon), [.partly, .thunder])
        XCTAssertEqual(parsed.daily.map(\.hi), [28, 30])

        XCTAssertNil(WeatherForecast(payload: ["key": "x"]))
        let unknownClock = WeatherForecast(payload: ["key": "x", "ts": 0, "hourly": [], "daily": []])
        XCTAssertNil(unknownClock?.ts)
    }

    // MARK: - Activity

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
        if request.path.hasSuffix("/v1/forecast") { return .json(200, forecastBody) }
        if request.url.host == "ipwho.is" {
            return .json(200, #"{"success":true,"city":"Olinda","latitude":-8.0,"longitude":-34.8}"#)
        }
        return StubResponse(status: 404)
    }

    func testActivityRecordsFetchSourceAndPushes() async throws {
        let clock = ClockBox(Date(timeIntervalSince1970: 1_792_000_000))
        let server = StubServer { Self.answer($0) }
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        let refuse = BoolBox()
        await pager.handle("weather.push") { _ in
            if refuse.value { throw RPCError(code: "invalid_argument", msg: "hourly[0].temp out of range") }
            return .object([:])
        }
        await phone.start()
        await pager.start()
        let weather = WeatherService(session: phone, urlSession: StubURLProtocol.session(server),
                                     locator: { WeatherPlace(lat: -8.1, lon: -34.9, city: "Recife") },
                                     now: { clock.now })

        var activity = weather.activity()
        XCTAssertNil(activity.lastFetch)

        _ = try await weather.payload(key: "Recife")
        activity = weather.activity()
        XCTAssertEqual(activity.lastFetch?.source, .city)
        XCTAssertEqual(activity.lastFetch?.fromCache, false)
        XCTAssertEqual(activity.lastFetch?.at, clock.now)
        XCTAssertNotNil(WeatherForecast(payload: try XCTUnwrap(activity.lastFetch?.payload)))

        clock.set(clock.now.addingTimeInterval(60))
        try await weather.push(key: "Recife")
        activity = weather.activity()
        XCTAssertEqual(activity.lastFetch?.fromCache, true)
        XCTAssertEqual(activity.lastPush?.key, "Recife")
        XCTAssertNil(activity.lastPush?.error)

        _ = try await weather.payload(key: "auto")
        XCTAssertEqual(weather.activity().lastFetch?.source, .phoneLocation)

        refuse.value = true
        do {
            try await weather.push(key: "Recife")
            XCTFail("expected refusal")
        } catch {}
        XCTAssertEqual(weather.activity().lastPush?.error, "invalid_argument: hourly[0].temp out of range")

        // weather.get from the pager is recorded too.
        await weather.register()
        _ = try await pager.call("weather.get", params: ["key": "Recife"], timeout: .seconds(5))
        XCTAssertEqual(weather.activity().lastRequest?.key, "Recife")
    }

    func testAutoWithoutPhoneFixIsIPGeolocation() async throws {
        let server = StubServer { Self.answer($0) }
        let (a, _) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let weather = WeatherService(session: LinkSession(transport: a), urlSession: StubURLProtocol.session(server),
                                     locator: { throw RPCError(code: "unauthorized") })
        _ = try await weather.payload(key: "auto")
        XCTAssertEqual(weather.activity().lastFetch?.source, .ipGeolocation)
        XCTAssertEqual(weather.activity().lastFetch?.city, "Olinda")
    }
}

/// A lock-protected flag a handler reads.
private final class BoolBox: @unchecked Sendable {
    private let lock = NSLock()
    private var _value = false
    var value: Bool {
        get { lock.withLock { _value } }
        set { lock.withLock { _value = newValue } }
    }
}

/// A settable clock for `now:` closures.
private final class ClockBox: @unchecked Sendable {
    private let lock = NSLock()
    private var value: Date
    init(_ value: Date) { self.value = value }
    var now: Date { lock.withLock { value } }
    func set(_ date: Date) { lock.withLock { value = date } }
}
