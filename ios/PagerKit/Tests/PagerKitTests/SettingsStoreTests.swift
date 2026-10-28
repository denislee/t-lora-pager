import Foundation
import XCTest

@testable import PagerKit

/// A lock-protected in-memory `SettingsBackend`. `staleOnce` makes the NEXT
/// `set` fail with `SettingsError.changed` after bumping `rev` (and applying
/// `pagerEdit` at the same time, as a concurrent pager write would); `gets`
/// and `sets` count the calls so a test can see the reload/retry.
private final class FakeBackend: SettingsBackend, @unchecked Sendable {
    private let lock = NSLock()
    private let schemaValue: SettingsSchema
    private var valuesValue: [String: SettingValue]
    private var revValue: UInt32
    private var getsValue = 0
    private var setsValue = 0
    private var staleOnceValue = false
    private var pagerEditValue: (String, SettingValue)?

    init(schema: SettingsSchema, values: [String: SettingValue], rev: UInt32) {
        schemaValue = schema
        valuesValue = values
        revValue = rev
    }

    var gets: Int { lock.withLock { getsValue } }
    var sets: Int { lock.withLock { setsValue } }
    var rev: UInt32 { lock.withLock { revValue } }
    var values: [String: SettingValue] { lock.withLock { valuesValue } }

    func setStaleOnce(_ value: Bool) { lock.withLock { staleOnceValue = value } }
    func setPagerEdit(_ edit: (String, SettingValue)?) { lock.withLock { pagerEditValue = edit } }

    /// Simulate a write made on the pager itself: update a value and `rev` (the
    /// next `set` will still be accepted unless `staleOnce` is also set).
    func pagerWrote(_ id: String, _ value: SettingValue, rev: UInt32) {
        lock.withLock {
            valuesValue[id] = value
            revValue = rev
        }
    }

    func schema(expected revSchema: String?) async throws -> SettingsSchema {
        lock.withLock { schemaValue }
    }

    func get(ids: [String]?) async throws -> SettingsSnapshot {
        lock.withLock {
            getsValue += 1
            var selected = valuesValue
            if let ids { selected = selected.filter { ids.contains($0.key) } }
            return SettingsSnapshot(rev: revValue, revSchema: schemaValue.revSchema, values: selected)
        }
    }

    func set(_ values: [String: SettingValue], ifRev: UInt32?) async throws -> SettingsSetResult {
        try lock.withLock {
            setsValue += 1
            if staleOnceValue {
                staleOnceValue = false
                revValue += 1
                if let (id, value) = pagerEditValue { valuesValue[id] = value }
                throw SettingsError.changed
            }
            if let ifRev, ifRev != revValue { throw SettingsError.changed }
            revValue += 1
            for (id, value) in values { valuesValue[id] = value }
            let applied = values.keys.sorted()
            let reboot = applied.contains { schemaValue.item($0)?.apply == .reboot }
            return SettingsSetResult(rev: revValue, applied: applied, reboot: reboot)
        }
    }
}

/// The fake's catalog: the four settings the store tests exercise.
private func makeSettingsSchema() -> SettingsSchema {
    SettingsSchema(
        revSchema: "rev-1",
        groups: [
            SettingGroup(id: "display", title: "Display & Backlight"),
            SettingGroup(id: "storage", title: "Storage"),
            SettingGroup(id: "nsync", title: "Notes Sync"),
            SettingGroup(id: "wifi", title: "Wi-Fi Networks"),
        ],
        actions: [SettingAction(id: "wifi.scan", group: "wifi", title: "Scan for networks")],
        items: [
            SettingItem(
                id: "display.brightness", group: "display", title: "Screen brightness",
                kind: .int, min: 0, max: 16, step: 1),
            SettingItem(
                id: "display.timeout_s", group: "display", title: "Screen timeout (s)",
                kind: .int, min: 0, max: 180, step: 1),
            SettingItem(
                id: "nsync.token", group: "nsync", title: "GitHub token (PAT)",
                kind: .secret, maxLen: 255, secret: true),
            SettingItem(
                id: "storage.msc_target", group: "storage", title: "USB drive shows",
                kind: .enumeration,
                options: [
                    SettingOption(value: .string("sd"), label: "SD card"),
                    SettingOption(value: .string("int"), label: "Internal flash"),
                ],
                apply: .reboot),
        ])
}

private func makeSettingsValues() -> [String: SettingValue] {
    [
        "display.brightness": .int(8),
        "display.timeout_s": .int(60),
        "nsync.token": .secret(set: false),
        "storage.msc_target": .string("sd"),
    ]
}

/// Covers `SettingsStore`: attach/reload, local validation, stale-revision
/// reload + single retry, drafts surviving a pager change, the reboot flag and
/// detaching into the read-only offline view.
@MainActor
final class SettingsStoreTests: XCTestCase {

    @MainActor
    private func makeStore() async -> (store: SettingsStore, backend: FakeBackend) {
        let backend = FakeBackend(schema: makeSettingsSchema(), values: makeSettingsValues(), rev: 100)
        let store = SettingsStore()
        await store.attach(backend)
        return (store, backend)
    }

    @MainActor
    func testAttachLoadsSchemaAndValues() async {
        let (store, backend) = await makeStore()

        XCTAssertTrue(store.connected)
        XCTAssertTrue(store.canEdit)
        XCTAssertEqual(store.schema?.revSchema, "rev-1")
        XCTAssertEqual(store.rev, 100)
        XCTAssertEqual(store.values["display.brightness"], .int(8))
        XCTAssertEqual(store.values["nsync.token"], .secret(set: false))
        // One get to learn the revision, then one more with the schema in hand.
        XCTAssertEqual(backend.gets, 2)
        XCTAssertNil(store.lastError)
        XCTAssertEqual(store.groups().map(\.id), ["display", "storage", "nsync", "wifi"])
    }

    @MainActor
    func testCommitUpdatesValueAndRev() async {
        let (store, backend) = await makeStore()

        let ok = await store.commit("display.brightness", .int(10))

        XCTAssertTrue(ok)
        XCTAssertEqual(store.value("display.brightness"), .int(10))
        XCTAssertEqual(store.rev, 101)
        XCTAssertEqual(backend.sets, 1)
        XCTAssertNil(store.lastError)
        XCTAssertTrue(store.committing.isEmpty)
    }

    @MainActor
    func testCommitRejectsInvalidValueLocally() async {
        let (store, backend) = await makeStore()

        let ok = await store.commit("display.brightness", .int(99))

        XCTAssertFalse(ok)
        XCTAssertEqual(backend.sets, 0)
        XCTAssertEqual(store.lastError, "Screen brightness: out of range")
        XCTAssertEqual(store.value("display.brightness"), .int(8), "never sent, never changed")
    }

    @MainActor
    func testCommitRetriesOnceWhenAnotherSettingChangedOnPager() async {
        let (store, backend) = await makeStore()
        backend.setStaleOnce(true)
        backend.setPagerEdit(("display.timeout_s", .int(120)))

        let ok = await store.commit("display.brightness", .int(5))

        XCTAssertTrue(ok)
        XCTAssertEqual(backend.sets, 2, "one stale attempt, one accepted retry")
        XCTAssertEqual(backend.gets, 3, "the store refetched after the stale write")
        XCTAssertEqual(store.value("display.brightness"), .int(5))
        XCTAssertEqual(store.value("display.timeout_s"), .int(120))
        XCTAssertEqual(store.values["display.timeout_s"], .int(120))
        XCTAssertNil(store.lastError)
    }

    @MainActor
    func testCommitRefusesWhenSameSettingChangedOnPager() async {
        let (store, backend) = await makeStore()
        backend.setStaleOnce(true)
        backend.setPagerEdit(("display.brightness", .int(3)))

        let ok = await store.commit("display.brightness", .int(5))

        XCTAssertFalse(ok)
        XCTAssertEqual(backend.sets, 1, "no retry when the pager changed this id")
        let error = store.lastError ?? ""
        XCTAssertTrue(error.contains("changed on the pager"), "lastError was \(error)")
        XCTAssertEqual(store.value("display.brightness"), .int(3), "the pager's value wins")
    }

    @MainActor
    func testPagerChangedKeepsDraftForEditedID() async throws {
        let (store, backend) = await makeStore()
        store.beginEdit("display.brightness")
        store.draft("display.brightness", .int(3))

        backend.pagerWrote("display.brightness", .int(12), rev: 101)
        let change = try XCTUnwrap(
            SettingsChanged(params: ["rev": 101, "ids": ["display.brightness"]]))
        await store.pagerChanged(change)

        XCTAssertEqual(store.values["display.brightness"], .int(12), "base value moved")
        XCTAssertEqual(store.value("display.brightness"), .int(3), "draft kept")
        XCTAssertEqual(store.rev, 101)
    }

    @MainActor
    func testPagerChangedIgnoresOwnRevision() async throws {
        let (store, backend) = await makeStore()
        let getsBefore = backend.gets
        let change = try XCTUnwrap(
            SettingsChanged(params: ["rev": .number(Double(store.rev)), "ids": ["display.brightness"]]))

        await store.pagerChanged(change)

        XCTAssertEqual(backend.gets, getsBefore, "our own write must not refetch")
    }

    @MainActor
    func testCommitSetsRebootFlag() async {
        let (store, _) = await makeStore()

        let ok = await store.commit("storage.msc_target", .string("int"))

        XCTAssertTrue(ok)
        XCTAssertTrue(store.rebootNeeded)
        XCTAssertEqual(store.value("storage.msc_target"), .string("int"))
    }

    @MainActor
    func testDetachKeepsValuesReadOnly() async {
        let (store, _) = await makeStore()

        store.detach()

        XCTAssertFalse(store.connected)
        XCTAssertFalse(store.canEdit)
        XCTAssertEqual(store.values["display.brightness"], .int(8))
        XCTAssertNotNil(store.schema)
        XCTAssertTrue(store.drafts.isEmpty)
    }
}
