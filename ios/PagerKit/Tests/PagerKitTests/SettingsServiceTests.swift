import Foundation
import XCTest

@testable import PagerKit

/// A lock-protected counter a `@Sendable` handler can bump.
private final class SettingsCallCounter: @unchecked Sendable {
    private let lock = NSLock()
    private var value = 0

    func bump() {
        lock.lock()
        defer { lock.unlock() }
        value += 1
    }

    var count: Int {
        lock.lock()
        defer { lock.unlock() }
        return value
    }
}

/// Why a vector fixture could not be read.
private enum VectorFailure: Error {
    case missing(String)
}

/// The vectors live at `<repo-root>/protocol/vectors/settings.json`. From this
/// file (`Tests/PagerKitTests/SettingsServiceTests.swift`) that is five path
/// components up, as in `VectorTests`.
private func settingsVectorURL() -> URL {
    var url = URL(fileURLWithPath: #filePath)
    for _ in 0..<5 { url.deleteLastPathComponent() }
    return url
        .appendingPathComponent("protocol")
        .appendingPathComponent("vectors")
        .appendingPathComponent("settings.json")
}

private func loadSettingsVectors() throws -> JSONValue {
    let data = try Data(contentsOf: settingsVectorURL())
    return try JSONValue.decode(bytes: [UInt8](data))
}

/// One `cases[]` entry's `result`.
private func vectorCaseResult(_ name: String, in vectors: JSONValue) throws -> JSONValue {
    guard case .array(let cases)? = vectors["cases"],
          let entry = cases.first(where: { $0["name"]?.stringValue == name }),
          let result = entry["result"]
    else { throw VectorFailure.missing("case \(name)") }
    return result
}

/// One `events[]` entry, as `(name, params)`.
private func vectorEvent(_ name: String, in vectors: JSONValue) throws -> (name: String, params: JSONValue) {
    guard case .array(let events)? = vectors["events"],
          let entry = events.first(where: { $0["name"]?.stringValue == name }),
          let event = entry["e"]?.stringValue,
          let params = entry["p"]
    else { throw VectorFailure.missing("event \(name)") }
    return (event, params)
}

/// Covers `SettingsService` against the `protocol/vectors/settings.json`
/// catalog served by an in-process pager `LinkSession`: page merging and the
/// `rev_schema` cache, typed value decoding, writes and stale revisions, the
/// vector events, and an action job delivered as `evt.settings.job`.
final class SettingsServiceTests: XCTestCase {

    /// A fresh two-session harness: the phone runs the `SettingsService`, the
    /// pager serves the vector catalog.
    private func makePair() -> (phone: LinkSession, pager: LinkSession) {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        return (LinkSession(transport: a), LinkSession(transport: b, role: .pager))
    }

    /// Serve `settings.schema` (two pages: the first vector page cut to four
    /// items plus `"cursor": "4"`, then `schema_cursor`), `settings.get`
    /// (`get_all`) and `settings.set` (`set_ok`, or `changed` for `if_rev` 99).
    /// Each schema page bumps `fetches`.
    private func serveSettings(_ pager: LinkSession, _ vectors: JSONValue,
                               fetches: SettingsCallCounter) async throws {
        let firstPage = try vectorCaseResult("schema_first_page", in: vectors)
        let secondPage = try vectorCaseResult("schema_cursor", in: vectors)
        let getAll = try vectorCaseResult("get_all", in: vectors)
        let setOK = try vectorCaseResult("set_ok", in: vectors)

        await pager.handle("settings.schema") { params in
            fetches.bump()
            guard params["cursor"]?.stringValue == nil else { return secondPage }
            guard case .object(var page) = firstPage,
                  case .array(let items)? = page["items"]
            else { throw SettingsError.badReply("settings.schema: bad fixture") }
            page["items"] = .array(Array(items.prefix(4)))
            page["cursor"] = .string("4")
            return .object(page)
        }
        await pager.handle("settings.get") { _ in getAll }
        await pager.handle("settings.set") { params in
            if params["if_rev"]?.intValue == 99 {
                throw RPCError(code: "invalid_argument", msg: "changed")
            }
            return setOK
        }
    }

    // MARK: - Schema

    func testSchemaMergesPages() async throws {
        let (phone, pager) = makePair()
        let vectors = try loadSettingsVectors()
        let fetches = SettingsCallCounter()
        try await serveSettings(pager, vectors, fetches: fetches)
        await phone.start()
        await pager.start()
        let service = SettingsService(session: phone)

        let schema = try await service.schema()

        XCTAssertEqual(schema.revSchema, "ebdafa8d")
        XCTAssertEqual(schema.groups.map(\.id), ["display", "storage", "nsync", "wifi"])
        XCTAssertEqual(schema.actions.map(\.id), ["wifi.scan"])
        XCTAssertEqual(schema.items.map(\.id), [
            "display.brightness",
            "display.timeout_s",
            "display.sleep_mode",
            "storage.msc_target",
            "nsync.token",
            "wifi.saved",
        ])
        XCTAssertEqual(schema.item("display.brightness")?.kind, .int)
        XCTAssertEqual(schema.item("wifi.saved")?.readOnly, true)
        XCTAssertEqual(fetches.count, 2)

        await phone.stop()
        await pager.stop()
    }

    func testSchemaCacheByRevSchema() async throws {
        let (phone, pager) = makePair()
        let vectors = try loadSettingsVectors()
        let fetches = SettingsCallCounter()
        try await serveSettings(pager, vectors, fetches: fetches)
        await phone.start()
        await pager.start()
        let service = SettingsService(session: phone)

        _ = try await service.schema()
        let afterFirst = await service.schemaFetches
        XCTAssertEqual(afterFirst, 2)

        // Same revision: answered from the cache, no page fetched.
        _ = try await service.schema(expected: "ebdafa8d")
        let cached = await service.schemaFetches
        XCTAssertEqual(cached, afterFirst)

        // A different revision: fetch the pages again.
        _ = try await service.schema(expected: "other")
        let refetched = await service.schemaFetches
        XCTAssertEqual(refetched, afterFirst + 2)

        await phone.stop()
        await pager.stop()
    }

    // MARK: - Values

    func testGetDecodesValuesWithSchemaTypes() async throws {
        let (phone, pager) = makePair()
        let vectors = try loadSettingsVectors()
        let fetches = SettingsCallCounter()
        try await serveSettings(pager, vectors, fetches: fetches)
        await phone.start()
        await pager.start()
        let service = SettingsService(session: phone)

        _ = try await service.schema()
        let snapshot = try await service.get()

        XCTAssertEqual(snapshot.rev, 100)
        XCTAssertEqual(snapshot.revSchema, "ebdafa8d")
        XCTAssertEqual(snapshot.values["nsync.token"], .secret(set: false))
        XCTAssertEqual(snapshot.values["display.brightness"], .int(8))
        XCTAssertEqual(snapshot.values["display.sleep_mode"], .string("light"))
        XCTAssertEqual(snapshot.values["wifi.saved"], .json(.array([.string("home")])))

        await phone.stop()
        await pager.stop()
    }

    // MARK: - Writes

    func testSetStaleRevision() async throws {
        let (phone, pager) = makePair()
        let vectors = try loadSettingsVectors()
        let fetches = SettingsCallCounter()
        try await serveSettings(pager, vectors, fetches: fetches)
        await phone.start()
        await pager.start()
        let service = SettingsService(session: phone)

        do {
            _ = try await service.set(["display.brightness": .int(5)], ifRev: 99)
            XCTFail("expected SettingsError.changed")
        } catch let error as SettingsError {
            XCTAssertEqual(error, .changed)
        }

        await phone.stop()
        await pager.stop()
    }

    func testSetOK() async throws {
        let (phone, pager) = makePair()
        let vectors = try loadSettingsVectors()
        let fetches = SettingsCallCounter()
        try await serveSettings(pager, vectors, fetches: fetches)
        await phone.start()
        await pager.start()
        let service = SettingsService(session: phone)

        let result = try await service.set(["display.brightness": .int(5)], ifRev: 100)
        XCTAssertEqual(result.rev, 101)
        XCTAssertEqual(result.applied, ["display.brightness"])
        XCTAssertFalse(result.reboot)

        await phone.stop()
        await pager.stop()
    }

    // MARK: - Events

    func testVectorEventsDecode() throws {
        let vectors = try loadSettingsVectors()

        let changed = try vectorEvent("changed", in: vectors)
        XCTAssertEqual(changed.name, SettingsChanged.event)
        let change = try XCTUnwrap(SettingsChanged(params: changed.params))
        XCTAssertEqual(change.rev, 101)
        XCTAssertEqual(change.ids, ["display.brightness"])

        let scan = try vectorEvent("job_scan", in: vectors)
        let job = try XCTUnwrap(SettingsJob(params: scan.params))
        XCTAssertEqual(job.job, "j1")
        XCTAssertTrue(job.done)
        XCTAssertTrue(job.ok)
        XCTAssertEqual(job.msg, "ok")
        guard case .array(let networks)? = job.result else {
            return XCTFail("job_scan result should be an array")
        }
        XCTAssertEqual(networks.count, 1)
        XCTAssertEqual(networks[0]["ssid"]?.stringValue, "home")

        let failed = try vectorEvent("job_failed", in: vectors)
        let failure = try XCTUnwrap(SettingsJob(params: failed.params))
        XCTAssertEqual(failure.job, "j2")
        XCTAssertTrue(failure.done)
        XCTAssertFalse(failure.ok)
        XCTAssertEqual(failure.msg, "NTP timed out")
        XCTAssertNil(failure.result)
    }

    func testRunWaitsForJobEvent() async throws {
        let (phone, pager) = makePair()
        let vectors = try loadSettingsVectors()
        let fetches = SettingsCallCounter()
        try await serveSettings(pager, vectors, fetches: fetches)
        let scan = try vectorEvent("job_scan", in: vectors)

        await pager.handle("settings.action") { _ in
            Task { await pager.emit(scan.name, params: scan.params) }
            return ["job": "j1"]
        }
        await phone.start()
        await pager.start()
        let service = SettingsService(session: phone)
        let events = Task {
            for await (name, params) in phone.events {
                await service.handleEvent(name: name, params: params)
            }
        }

        let result = try await service.run("wifi.scan", timeout: .seconds(5))
        guard case .array(let networks) = result else {
            return XCTFail("expected an array result")
        }
        XCTAssertEqual(networks.count, 1)
        XCTAssertEqual(networks[0]["ssid"]?.stringValue, "home")
        XCTAssertEqual(networks[0]["rssi"]?.intValue, -50)

        events.cancel()
        _ = await events.value
        await phone.stop()
        await pager.stop()
    }

    func testRunThrowsWhenJobFails() async throws {
        let (phone, pager) = makePair()
        let vectors = try loadSettingsVectors()
        let fetches = SettingsCallCounter()
        try await serveSettings(pager, vectors, fetches: fetches)
        let failed = try vectorEvent("job_failed", in: vectors)

        await pager.handle("settings.action") { _ in
            Task { await pager.emit(failed.name, params: failed.params) }
            return ["job": "j2"]
        }
        await phone.start()
        await pager.start()
        let service = SettingsService(session: phone)
        let events = Task {
            for await (name, params) in phone.events {
                await service.handleEvent(name: name, params: params)
            }
        }

        do {
            _ = try await service.run("ntp.sync", timeout: .seconds(5))
            XCTFail("expected SettingsError.jobFailed")
        } catch let error as SettingsError {
            XCTAssertEqual(error, .jobFailed("NTP timed out"))
        }

        events.cancel()
        _ = await events.value
        await phone.stop()
        await pager.stop()
    }

    // MARK: - Local validation

    func testItemProblem() {
        let brightness = SettingItem(
            id: "display.brightness", group: "display", title: "Screen brightness",
            kind: .int, min: 0, max: 16, step: 1)
        XCTAssertEqual(brightness.problem(with: .int(17)), "out of range")
        XCTAssertNil(brightness.problem(with: .int(8)))

        let sleepMode = SettingItem(
            id: "display.sleep_mode", group: "display", title: "Sleep mode",
            kind: .enumeration,
            options: [
                SettingOption(value: .string("light"), label: "Light"),
                SettingOption(value: .string("deep"), label: "Deep"),
            ])
        XCTAssertEqual(sleepMode.problem(with: .string("nap")), "not one of the choices")
        XCTAssertNil(sleepMode.problem(with: .string("light")))

        let saved = SettingItem(
            id: "wifi.saved", group: "wifi", title: "Saved networks",
            kind: .list, readOnly: true)
        XCTAssertEqual(saved.problem(with: .json(.array([]))), "read-only")
    }
}
