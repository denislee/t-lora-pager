import Foundation
import XCTest

@testable import PagerKit

/// Exercises `NotesCache` (IOS_NOTES_VIEWER_PLAN.md §3.2): a hit needs the
/// listing's size *and* a known identity (CRC-32 or non-zero mtime), and the
/// file on disk must still match the record. Raw bytes only — ciphertext stays
/// ciphertext.
final class NotesCacheTests: XCTestCase {

    /// Temp base directories made by the test, removed in `tearDown`.
    private var roots: [URL] = []

    override func tearDown() {
        for root in roots { try? FileManager.default.removeItem(at: root) }
        roots = []
        super.tearDown()
    }

    // MARK: - Helpers

    private func makeCache() -> (cache: NotesCache, root: URL) {
        // A unique parent so a traversal attempt lands inside it, not in the
        // shared temporary directory.
        let base = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let root = base.appendingPathComponent("notes", isDirectory: true)
        roots.append(base)
        return (NotesCache(root: root), root)
    }

    private static func bytes(_ text: String) -> [UInt8] {
        Array(text.utf8)
    }

    /// The `Salted__` ciphertext the crypto tests use: the magic plus a
    /// deterministic `(i*37+11) & 0xFF` tail.
    private static func saltedBytes() -> [UInt8] {
        Array("Salted__".utf8) + (0..<64).map { UInt8(($0 * 37 + 11) & 0xFF) }
    }

    private static func entry(_ name: String, vol: String = "int", size: UInt32,
                              mtime: Int64? = nil, crc32: UInt32? = nil) -> NoteEntry {
        NoteEntry(vol: vol, name: name, size: size, mtime: mtime, crc32: crc32)
    }

    private static func fileURL(_ root: URL, _ vol: String, _ name: String) -> URL {
        root.appendingPathComponent(vol).appendingPathComponent(name)
    }

    // MARK: - Hits

    func testHitOnMatchingCRC() {
        let (cache, _) = makeCache()
        let data = NotesCacheTests.bytes("hello journal")
        let stored = NotesCacheTests.entry("a.txt", size: UInt32(data.count))
        cache.store(stored, bytes: data, mtime: nil)

        let wanted = NotesCacheTests.entry(
            "a.txt", size: UInt32(data.count), crc32: CRC32.checksum(data))

        XCTAssertEqual(cache.bytes(for: wanted), data)
    }

    func testHitOnMatchingMtime() {
        let (cache, _) = makeCache()
        let data = NotesCacheTests.bytes("hello journal")
        let stored = NotesCacheTests.entry("a.txt", size: UInt32(data.count))
        cache.store(stored, bytes: data, mtime: 1_700_000_123)

        let wanted = NotesCacheTests.entry(
            "a.txt", size: UInt32(data.count), mtime: 1_700_000_123)

        XCTAssertEqual(cache.bytes(for: wanted), data, "a non-zero mtime match is a hit without the CRC")
    }

    // MARK: - Misses

    func testMissOnSizeOnly() {
        let (cache, _) = makeCache()
        let data = NotesCacheTests.bytes("hello journal")
        cache.store(NotesCacheTests.entry("a.txt", size: UInt32(data.count)), bytes: data, mtime: nil)

        let wanted = NotesCacheTests.entry("a.txt", size: UInt32(data.count))

        XCTAssertNil(cache.bytes(for: wanted),
                     "size alone is never a hit: a same-size edit would show stale text")
    }

    func testStaleOnSize() {
        let (cache, _) = makeCache()
        let data = NotesCacheTests.bytes("hello journal")
        cache.store(NotesCacheTests.entry("a.txt", size: UInt32(data.count)), bytes: data, mtime: nil)

        let wanted = NotesCacheTests.entry(
            "a.txt", size: UInt32(data.count) + 1, crc32: CRC32.checksum(data))

        XCTAssertNil(cache.bytes(for: wanted))
    }

    func testStaleOnCRC() {
        let (cache, _) = makeCache()
        let data = NotesCacheTests.bytes("hello journal")
        cache.store(NotesCacheTests.entry("a.txt", size: UInt32(data.count)), bytes: data, mtime: nil)

        let wanted = NotesCacheTests.entry(
            "a.txt", size: UInt32(data.count), crc32: CRC32.checksum(data) ^ 1)

        XCTAssertNil(cache.bytes(for: wanted))
    }

    func testMissWhenFileTampered() {
        let (cache, root) = makeCache()
        let data = NotesCacheTests.bytes("hello journal")
        cache.store(NotesCacheTests.entry("a.txt", size: UInt32(data.count)), bytes: data, mtime: nil)

        // Same length, different bytes: only the file's own CRC catches it.
        let tampered = NotesCacheTests.bytes("HELLO JOURNAL")
        XCTAssertEqual(tampered.count, data.count)
        try? Data(tampered).write(to: NotesCacheTests.fileURL(root, "int", "a.txt"))

        let wanted = NotesCacheTests.entry(
            "a.txt", size: UInt32(data.count), crc32: CRC32.checksum(data))

        XCTAssertNil(cache.bytes(for: wanted), "the record no longer describes the file on disk")
    }

    func testRejectsUnsafeNames() {
        let (cache, root) = makeCache()
        let data: [UInt8] = [1, 2, 3]

        let names = ["../x", "", ".hidden", "a/b", "a\\b", "x\u{0}y"]
        for name in names {
            cache.store(NotesCacheTests.entry(name, size: UInt32(data.count)), bytes: data, mtime: nil)
            XCTAssertNil(cache.record(vol: "int", name: name)?.size, "store(\(name)) must be a no-op")
        }
        cache.store(NotesCacheTests.entry("x.txt", vol: "usb", size: UInt32(data.count)),
                    bytes: data, mtime: nil)
        XCTAssertNil(cache.record(vol: "usb", name: "x.txt")?.size, "only int and sd exist")

        XCTAssertFalse(FileManager.default.fileExists(atPath: root.path),
                       "a rejected write creates nothing")
        XCTAssertFalse(FileManager.default.fileExists(
            atPath: root.deletingLastPathComponent().appendingPathComponent("x").path),
                       "nothing is written outside the root")
    }

    // MARK: - Scans

    func testStaleOnFsChanged() {
        let (cache, _) = makeCache()
        let scan = NotesIndexer.Scan(
            merged: [NotesCacheTests.entry("a.txt", size: 1)],
            all: [NotesCacheTests.entry("a.txt", size: 1)],
            sdMounted: true
        )

        cache.saveScan(scan, at: Date(timeIntervalSince1970: 1_700_000_000))
        XCTAssertEqual(cache.cachedScan()?.stale, false)

        cache.markStale()
        XCTAssertEqual(cache.cachedScan()?.stale, true, "evt.fs.changed asks for a re-list")

        cache.saveScan(scan, at: Date())
        XCTAssertEqual(cache.cachedScan()?.stale, false, "a fresh scan clears the flag")
    }

    func testRemovedAfterDelete() {
        let (cache, root) = makeCache()
        let data = NotesCacheTests.bytes("gone soon")
        let entry = NotesCacheTests.entry("a.txt", size: UInt32(data.count))
        cache.store(entry, bytes: data, mtime: nil)
        cache.saveScan(NotesIndexer.Scan(merged: [entry], all: [entry], sdMounted: true), at: Date())

        cache.remove(vol: "int", name: "a.txt")

        let wanted = NotesCacheTests.entry(
            "a.txt", size: UInt32(data.count), crc32: CRC32.checksum(data))
        XCTAssertNil(cache.bytes(for: wanted))
        XCTAssertNil(cache.record(vol: "int", name: "a.txt")?.size)

        let cached = cache.cachedScan()
        XCTAssertEqual(cached?.scan.merged.count, 0)
        XCTAssertEqual(cached?.scan.all.count, 0)
        XCTAssertFalse(FileManager.default.fileExists(
            atPath: NotesCacheTests.fileURL(root, "int", "a.txt").path))
    }

    func testScanDropsGoneRecords() {
        let (cache, root) = makeCache()
        let aBytes = NotesCacheTests.bytes("alpha")
        let bBytes = NotesCacheTests.bytes("bravo")
        let a = NotesCacheTests.entry("a.txt", size: UInt32(aBytes.count))
        let b = NotesCacheTests.entry("b.txt", size: UInt32(bBytes.count))
        cache.store(a, bytes: aBytes, mtime: nil)
        cache.store(b, bytes: bBytes, mtime: nil)

        cache.saveScan(NotesIndexer.Scan(merged: [a], all: [a], sdMounted: false), at: Date())

        XCTAssertNil(cache.record(vol: "int", name: "b.txt")?.size, "b.txt is gone from the pager")
        XCTAssertFalse(FileManager.default.fileExists(
            atPath: NotesCacheTests.fileURL(root, "int", "b.txt").path))
        XCTAssertNotNil(cache.record(vol: "int", name: "a.txt")?.size)
        XCTAssertEqual(
            cache.bytes(for: NotesCacheTests.entry("a.txt", size: UInt32(aBytes.count),
                                                  crc32: CRC32.checksum(aBytes))),
            aBytes)
    }

    // MARK: - Bytes and lifetime

    func testCiphertextStoredRaw() {
        let (cache, _) = makeCache()
        let raw = NotesCacheTests.saltedBytes()
        let entry = NotesCacheTests.entry("secret.txt", size: UInt32(raw.count))
        cache.store(entry, bytes: raw, mtime: nil)

        let wanted = NotesCacheTests.entry(
            "secret.txt", size: UInt32(raw.count), crc32: CRC32.checksum(raw))

        XCTAssertEqual(cache.bytes(for: wanted), raw, "ciphertext round-trips byte for byte")
    }

    func testWipe() {
        let (cache, root) = makeCache()
        let aBytes = NotesCacheTests.bytes("alpha")
        let bBytes = NotesCacheTests.bytes("bravo")
        let a = NotesCacheTests.entry("a.txt", size: UInt32(aBytes.count))
        let b = NotesCacheTests.entry("b.txt", vol: "sd", size: UInt32(bBytes.count))
        cache.store(a, bytes: aBytes, mtime: 1_700_000_001)
        cache.store(b, bytes: bBytes, mtime: nil)
        cache.saveScan(NotesIndexer.Scan(merged: [a, b], all: [a, b], sdMounted: true), at: Date())

        cache.wipe()

        XCTAssertNil(cache.record(vol: "int", name: "a.txt")?.size)
        XCTAssertFalse(FileManager.default.fileExists(
            atPath: NotesCacheTests.fileURL(root, "sd", "b.txt").path))
        XCTAssertNil(cache.cachedScan()?.at)
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.path),
                       "wipe deletes everything under the root")
    }

    func testPersistsAcrossInstances() {
        let (cache, root) = makeCache()
        let data = NotesCacheTests.bytes("survives")
        let entry = NotesCacheTests.entry("a.txt", size: UInt32(data.count))
        cache.store(entry, bytes: data, mtime: 1_700_000_123)
        cache.saveScan(NotesIndexer.Scan(merged: [entry], all: [entry], sdMounted: false),
                       at: Date(timeIntervalSince1970: 1_700_000_000))

        let reopened = NotesCache(root: root)
        let record = reopened.record(vol: "int", name: "a.txt")
        XCTAssertEqual(record?.size, UInt32(data.count))
        XCTAssertEqual(record?.crc32, CRC32.checksum(data))
        XCTAssertEqual(record?.mtime, 1_700_000_123)

        let wanted = NotesCacheTests.entry(
            "a.txt", size: UInt32(data.count), crc32: CRC32.checksum(data))
        XCTAssertEqual(reopened.bytes(for: wanted), data)
        XCTAssertEqual(reopened.cachedScan()?.scan.merged.map(\.name), ["a.txt"])
        XCTAssertEqual(reopened.cachedScan()?.scan.sdMounted, false)
        XCTAssertEqual(reopened.cachedScan()?.stale, false)
    }
}
