import Foundation
import XCTest

@testable import PagerKit

/// A chat backend that echoes, or fails the LLM step with `failLLM`.
private struct EchoBackend: ChatBackend {
    var failLLM = false
    func transcribe(wav: [UInt8], deadline: ContinuousClock.Instant) async throws -> String { "spoken words" }
    func complete(history: [ChatTurn], deadline: ContinuousClock.Instant) async throws -> String {
        if failLLM { throw ChatBackendError.upstream("upstream 500: boom") }
        return "echo: \(history.last?.content ?? "")"
    }
}

/// Collects records from `ChatService.onRecord`.
private final class RecordSink: @unchecked Sendable {
    private let lock = NSLock()
    private var items: [ChatRecord] = []
    func add(_ record: ChatRecord) { lock.withLock { items.append(record) } }
    var all: [ChatRecord] { lock.withLock { items } }
}

/// The phone-side stores: chat history (and `ChatService`'s recorder hook),
/// battery history, the widget's status snapshot, the diagnostics report and
/// the `evt.notes.sync` reader.
final class CompanionStoresTests: XCTestCase {
    private func tempURL(_ name: String) -> URL {
        FileManager.default.temporaryDirectory
            .appendingPathComponent("companion-\(UUID().uuidString)", isDirectory: true)
            .appendingPathComponent(name)
    }

    // MARK: - Chat history

    func testChatServiceRecordsTurnsResetsAndErrors() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        let chat = ChatService(session: phone, backend: EchoBackend())
        let sink = RecordSink()
        await chat.onRecord { sink.add($0) }
        await chat.register()
        await phone.start()
        await pager.start()

        let reply = try await pager.call("chat.ask", params: ["text": " hello "], timeout: .seconds(5))
        XCTAssertEqual(reply["reply"]?.stringValue, "echo: hello")
        _ = try await pager.call("chat.ask", params: ["reset": true], timeout: .seconds(5))

        let failing = ChatService(session: phone, backend: EchoBackend(failLLM: true))
        await failing.onRecord { sink.add($0) }
        await failing.register()
        do {
            _ = try await pager.call("chat.ask", params: ["text": "again"], timeout: .seconds(5))
            XCTFail("expected http_error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "http_error")
        }

        let records = sink.all
        XCTAssertEqual(records.map(\.kind), [.turn, .reset, .error])
        XCTAssertEqual(records[0].prompt, "hello")
        XCTAssertEqual(records[0].reply, "echo: hello")
        XCTAssertFalse(records[0].voice)
        XCTAssertEqual(records[2].prompt, "again")
        XCTAssertEqual(records[2].error, "llm: upstream 500: boom")
    }

    func testChatHistoryPersistsCapsSearchesAndExports() throws {
        let url = tempURL("chat.json")
        let t0 = Date(timeIntervalSince1970: 1_761_700_000)
        let store = ChatHistoryStore(url: url, capacity: 4)
        store.append(ChatRecord(at: t0, kind: .turn, prompt: "Qual é a capital?", reply: "Brasília"))
        store.append(ChatRecord(at: t0.addingTimeInterval(1), kind: .reset))
        store.append(ChatRecord(at: t0.addingTimeInterval(2), kind: .turn, prompt: "weather", voice: true,
                                reply: "sunny"))
        store.append(ChatRecord(at: t0.addingTimeInterval(3), kind: .error, prompt: "x", error: "llm: boom"))

        let reopened = ChatHistoryStore(url: url, capacity: 4)
        XCTAssertEqual(reopened.all, store.all)
        XCTAssertEqual(reopened.all.count, 4)

        XCTAssertEqual(reopened.search("brasilia").map(\.reply), ["Brasília"], "diacritic-insensitive")
        XCTAssertEqual(reopened.search("SUNNY").count, 1)
        XCTAssertEqual(reopened.search("boom").map(\.kind), [.error])
        XCTAssertEqual(reopened.search("  ").count, 4)

        let conversations = ChatHistoryStore.conversations(reopened.all)
        XCTAssertEqual(conversations.map(\.count), [1, 2])

        let text = ChatHistoryStore.exportText(reopened.all)
        XCTAssertTrue(text.contains("[2025-10-29T01:06:40Z] Pager: Qual é a capital?"), text)
        XCTAssertTrue(text.contains("--- new conversation"))
        XCTAssertTrue(text.contains("Pager (voice): weather"))
        XCTAssertTrue(text.contains("Error: llm: boom"))
        let json = try ChatHistoryStore.exportJSON(reopened.all)
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .secondsSince1970
        XCTAssertEqual(try decoder.decode([ChatRecord].self, from: json), reopened.all)

        reopened.append(ChatRecord(at: t0.addingTimeInterval(4), kind: .reset))
        XCTAssertEqual(reopened.all.count, 4, "capacity drops the oldest")
        XCTAssertEqual(reopened.all.first?.kind, .reset)

        reopened.clear()
        XCTAssertTrue(ChatHistoryStore(url: url).all.isEmpty)
    }

    func testCorruptHistoryStartsEmpty() throws {
        let url = tempURL("chat.json")
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data("not json".utf8).write(to: url)
        XCTAssertTrue(ChatHistoryStore(url: url).all.isEmpty)
    }

    // MARK: - Battery history

    func testBatterySamplesParse() {
        let at = Date(timeIntervalSince1970: 1_000)
        XCTAssertEqual(BatterySample.fromEvent(["pct": 80, "charging": true], at: at),
                       BatterySample(at: at, pct: 80, charging: true, source: .event))
        XCTAssertEqual(BatterySample.fromInfo(["batt_pct": 55, "charging": false], at: at)?.source, .info)
        XCTAssertNil(BatterySample.fromEvent(["pct": 101], at: at))
        XCTAssertNil(BatterySample.fromInfo(["uptime_s": 3], at: at))
    }

    func testBatteryRingDedupesPrunesAndPersists() {
        let url = tempURL("battery.json")
        let t0 = Date(timeIntervalSince1970: 1_761_700_000)
        let store = BatteryHistoryStore(url: url, maxAge: 3 * 86_400, maxSamples: 100)
        XCTAssertTrue(store.record(BatterySample(at: t0, pct: 90, charging: false, source: .info)))
        XCTAssertFalse(store.record(BatterySample(at: t0.addingTimeInterval(60), pct: 90, charging: false, source: .info)),
                       "same reading within 5 min")
        XCTAssertTrue(store.record(BatterySample(at: t0.addingTimeInterval(400), pct: 90, charging: false, source: .info)))
        XCTAssertTrue(store.record(BatterySample(at: t0.addingTimeInterval(500), pct: 85, charging: false, source: .event)))
        XCTAssertEqual(BatteryHistoryStore(url: url, maxAge: 3 * 86_400, maxSamples: 100).all.count, 3)

        // Four days later the first three are past maxAge.
        store.record(BatterySample(at: t0.addingTimeInterval(4 * 86_400), pct: 40, charging: true, source: .event))
        XCTAssertEqual(store.all.map(\.pct), [40])

        let capped = BatteryHistoryStore(url: tempURL("b.json"), maxSamples: 3)
        for i in 0..<5 { capped.record(BatterySample(at: t0.addingTimeInterval(Double(i) * 600), pct: 50 + i,
                                                     charging: false, source: .event)) }
        XCTAssertEqual(capped.all.map(\.pct), [52, 53, 54])
        XCTAssertEqual(capped.samples(since: t0.addingTimeInterval(1800)).map(\.pct), [53, 54])

        // An out-of-order sample is kept sorted.
        capped.record(BatterySample(at: t0.addingTimeInterval(1500), pct: 10, charging: false, source: .info))
        XCTAssertEqual(capped.all.map(\.at), capped.all.map(\.at).sorted())
    }

    func testBatterySummaryDischargeRate() {
        let t0 = Date(timeIntervalSince1970: 1_761_700_000)
        let samples = [
            BatterySample(at: t0, pct: 50, charging: true, source: .event),
            BatterySample(at: t0.addingTimeInterval(3600), pct: 100, charging: false, source: .event),
            BatterySample(at: t0.addingTimeInterval(3 * 3600), pct: 95, charging: false, source: .event),
            BatterySample(at: t0.addingTimeInterval(5 * 3600), pct: 90, charging: false, source: .info),
        ]
        let summary = BatteryHistoryStore.summary(samples)
        XCTAssertEqual(summary.count, 4)
        XCTAssertEqual(summary.latestPct, 90)
        XCTAssertEqual(summary.minPct, 50)
        XCTAssertEqual(summary.maxPct, 100)
        XCTAssertEqual(summary.dischargePctPerHour ?? 0, 2.5, accuracy: 0.0001)  // 10 % over 4 h
        XCTAssertNil(BatteryHistoryStore.summary(Array(samples.prefix(2))).dischargePctPerHour, "run too short")
        XCTAssertEqual(BatteryHistoryStore.summary([]), BatterySummary())
    }

    // MARK: - Widget status

    func testStatusSnapshotRoundTripsThroughDefaults() throws {
        let suite = "companion-tests-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let store = PagerStatusStore(defaults: defaults)
        XCTAssertNil(store.load())

        let t0 = Date(timeIntervalSince1970: 1_761_700_000)
        var snapshot = PagerStatusSnapshot(link: .connected, name: "lilygo-pager", updatedAt: t0)
        snapshot = snapshot.with(battery: BatterySample(at: t0.addingTimeInterval(5), pct: 77, charging: true,
                                                        source: .event))
        store.save(snapshot)
        XCTAssertEqual(store.load(), snapshot)
        XCTAssertEqual(store.load()?.batteryPct, 77)

        let dropped = snapshot.with(link: .disconnected, at: t0.addingTimeInterval(10))
        XCTAssertEqual(dropped.batteryPct, 77, "last known battery kept")
        XCTAssertEqual(dropped.name, "lilygo-pager")
        XCTAssertEqual(dropped.link, .disconnected)
    }

    func testAppGroupID() {
        XCTAssertEqual(PagerStatusStore.appGroupID(bundleID: "dev.lilygo.pagercompanion"),
                       "group.dev.lilygo.pagercompanion.shared")
        XCTAssertEqual(PagerStatusStore.appGroupID(bundleID: "dev.lilygo.pagercompanion.widgets"),
                       "group.dev.lilygo.pagercompanion.shared")
    }

    func testNotesSyncProgressEvent() {
        let event = NotesSyncProgressEvent(params: [
            "job": 3, "phase": "upload", "uploaded": 2, "already": 1, "deleted": 2, "kept": 1,
            "errors": ["a.txt"], "done": false,
        ])
        XCTAssertEqual(event?.job, 3)
        XCTAssertEqual(event?.errors, 1)
        XCTAssertEqual(event?.phaseLabel, "Uploading")
        XCTAssertEqual(event?.summary, "2 up · 1 already · 2 moved · 1 kept · 1 errors")
        let done = NotesSyncProgressEvent(params: ["job": 3, "phase": "done", "uploaded": 0, "already": 0,
                                                   "errors": [], "done": true])
        XCTAssertEqual(done?.phaseLabel, "Done")
        XCTAssertNil(NotesSyncProgressEvent(params: ["phase": "upload"]))
    }

    func testNotesSyncServiceReportsEveryEvent() async throws {
        let fs = CompanionFakeFs()
        let (phone, pager) = await fs.makeSessions()
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("phone-\(UUID().uuidString)")
        let service = NotesSyncService(session: phone, github: nil, phoneDirectory: dir)
        let seen = RecordedEvents()
        await service.onSyncEvent { seen.add($0) }
        await service.register()
        fs.set("int", "/notes/a.txt", Array("a".utf8))
        let reply = try await pager.call("notes.sync", params: [
            "targets": ["phone"],
            "files": [["name": "a.txt", "size": 1, "mtime": 0, "crc32": .number(Double(CRC32.checksum(Array("a".utf8)))),
                       "vol": "int"]],
        ], timeout: .seconds(5))
        let job = try XCTUnwrap(reply["job"]?.intValue)
        await service.wait(job: job)
        let events = seen.all.compactMap(NotesSyncProgressEvent.init(params:))
        XCTAssertFalse(events.isEmpty)
        XCTAssertEqual(events.last?.done, true)
        XCTAssertTrue(events.allSatisfy { $0.job == job })
    }

    // MARK: - Diagnostics

    func testDiagnosticsReport() throws {
        let report = DiagnosticsReport(
            generatedAt: Date(timeIntervalSince1970: 1_761_700_000), appVersion: "0.1", appBuild: "1",
            system: "iOS 17.5", linkState: "Connected",
            hello: ["fw": "dd840b4-dirty", "board": "pager", "caps": ["fs", "settings"]],
            info: ["batt_pct": 80, "charging": false],
            battery: BatterySummary(count: 2, latestPct: 80, minPct: 79, maxPct: 80, dischargePctPerHour: 1.23456),
            log: ["[10:00:00] sys.hello ok", "[10:00:01] time synced"]
        )
        XCTAssertEqual(report.firmware, "dd840b4-dirty")
        XCTAssertEqual(report.fileName, "pager-diagnostics-dd840b4-20251029-010640.json")
        let value = try JSONValue.decode(bytes: Array(try report.encoded()))
        XCTAssertEqual(value["app"]?["version"]?.stringValue, "0.1")
        XCTAssertEqual(value["link"]?["firmware"]?.stringValue, "dd840b4-dirty")
        XCTAssertEqual(value["sys_hello"]?["board"]?.stringValue, "pager")
        XCTAssertEqual(value["sys_info"]?["batt_pct"]?.intValue, 80)
        XCTAssertEqual(value["battery_history"]?["discharge_pct_per_hour"]?.doubleValue, 1.23)
        guard case .array(let log)? = value["session_log"] else { return XCTFail("no log") }
        XCTAssertEqual(log.count, 2)
        XCTAssertEqual(value["generated_at"]?.stringValue, "2025-10-29T01:06:40Z")

        var offline = report
        offline.hello = nil
        XCTAssertTrue(offline.fileName.hasPrefix("pager-diagnostics-nofw-"))
        XCTAssertEqual(try JSONValue.decode(bytes: Array(try offline.encoded()))["sys_hello"], .null)
    }
}

/// Collects `evt.notes.sync` params.
private final class RecordedEvents: @unchecked Sendable {
    private let lock = NSLock()
    private var items: [JSONValue] = []
    func add(_ value: JSONValue) { lock.withLock { items.append(value) } }
    var all: [JSONValue] { lock.withLock { items } }
}
