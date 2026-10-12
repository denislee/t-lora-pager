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
}
