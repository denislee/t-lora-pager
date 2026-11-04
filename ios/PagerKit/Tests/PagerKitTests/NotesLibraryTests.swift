import Foundation
import XCTest

@testable import PagerKit

// MARK: - Shared test state

/// A lock-protected counter the `@Sendable` prefetch closures share with the
/// test.
private final class LibraryCounter: @unchecked Sendable {
    private let lock = NSLock()
    private var count = 0

    func increment() {
        lock.lock()
        defer { lock.unlock() }
        count += 1
    }

    var value: Int {
        lock.lock()
        defer { lock.unlock() }
        return count
    }
}

// MARK: - Tests

/// Exercises `NotesLibrary` over two real `LinkSession`s with the pager
/// standing in for its notes filesystem, plus the preview helper.
final class NotesLibraryTests: XCTestCase {

    /// Temp cache roots made by a test, removed in `tearDown`.
    private var roots: [URL] = []

    override func tearDown() {
        for root in roots { try? FileManager.default.removeItem(at: root) }
        roots = []
        super.tearDown()
    }

    // MARK: - Helpers

    private static func makeLibrarySessions() async -> (phone: LinkSession, pager: LinkSession) {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()
        return (phone, pager)
    }

    private func makeCache() -> (cache: NotesCache, root: URL) {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        roots.append(root)
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

    // MARK: - Listing

    func testRefreshMergesAndCaches() async throws {
        let (phone, pager) = await NotesLibraryTests.makeLibrarySessions()
        let fs = CompanionFakeFs()
        fs.set("int", "/notes/a.txt", NotesLibraryTests.bytes("alpha"))
        fs.set("int", "/notes/b.txt", NotesLibraryTests.bytes("bravo"))
        fs.set("sd", "/notes/b.txt", NotesLibraryTests.bytes("bravo sd"))
        fs.set("sd", "/notes/c.txt", NotesLibraryTests.bytes("charlie"))
        await fs.register(pager)

        let (cache, _) = makeCache()
        let library = NotesLibrary(cache: cache)
        await library.attach(phone)

        let scan = try await library.refresh()

        XCTAssertEqual(scan.merged.map(\.name), ["c.txt", "b.txt", "a.txt"])
        XCTAssertEqual(scan.merged.first { $0.name == "b.txt" }?.vol, "int")
        XCTAssertEqual(scan.all.map(\.id),
                       ["int:b.txt", "int:a.txt", "sd:c.txt", "sd:b.txt"])
        XCTAssertTrue(scan.sdMounted)

        let cached = await library.cachedScan()
        XCTAssertEqual(cached?.scan, scan)
        XCTAssertNotNil(cached?.at)
        XCTAssertEqual(cached?.stale, false)

        await phone.stop()
        await pager.stop()
    }

    // MARK: - Reading

    func testReadUsesCacheOnSecondCall() async throws {
        let (phone, pager) = await NotesLibraryTests.makeLibrarySessions()
        let fs = CompanionFakeFs()
        let data = NotesLibraryTests.bytes("hello")
        fs.set("int", "/notes/a.txt", data)
        await fs.register(pager)

        let (cache, _) = makeCache()
        let library = NotesLibrary(cache: cache)
        await library.attach(phone)

        // The listing has no CRC; the first read learns it.
        let entry = NoteEntry(vol: "int", name: "a.txt", size: UInt32(data.count))
        let first = try await library.read(entry)
        XCTAssertEqual(first, data)

        let known = NoteEntry(vol: "int", name: "a.txt", size: UInt32(data.count),
                              crc32: CRC32.checksum(data))
        let second = try await library.read(known)
        XCTAssertEqual(second, data)
        XCTAssertEqual(fs.reads("int", "/notes/a.txt"), 1, "the second read is a cache hit")

        await phone.stop()
        await pager.stop()
    }

    func testReadDetachedServesCacheOnly() async throws {
        let (phone, pager) = await NotesLibraryTests.makeLibrarySessions()
        let fs = CompanionFakeFs()
        let data = NotesLibraryTests.bytes("hello")
        fs.set("int", "/notes/a.txt", data)
        await fs.register(pager)

        let (cache, _) = makeCache()
        let library = NotesLibrary(cache: cache)
        await library.attach(phone)
        let entry = NoteEntry(vol: "int", name: "a.txt", size: UInt32(data.count))
        _ = try await library.read(entry)

        await library.attach(nil)
        let attached = await library.isAttached
        XCTAssertFalse(attached)

        let known = NoteEntry(vol: "int", name: "a.txt", size: UInt32(data.count),
                              crc32: CRC32.checksum(data))
        let offline = try await library.read(known)
        XCTAssertEqual(offline, data)
        XCTAssertEqual(fs.reads("int", "/notes/a.txt"), 1, "an offline read never touches the link")

        await phone.stop()
        await pager.stop()
    }

    func testReadDetachedMissThrows() async throws {
        let (cache, _) = makeCache()
        let library = NotesLibrary(cache: cache)
        await library.attach(nil)

        let entry = NoteEntry(vol: "int", name: "a.txt", size: 5)
        do {
            _ = try await library.read(entry)
            XCTFail("expected disconnected")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "disconnected")
            XCTAssertEqual(error.msg, "not connected")
        } catch {
            XCTFail("unexpected error \(error)")
        }
    }

    // MARK: - Stat

    func testStatFillsMtimeAndProtected() async throws {
        let (phone, pager) = await NotesLibraryTests.makeLibrarySessions()
        let fs = CompanionFakeFs()
        let data = NotesLibraryTests.saltedBytes()
        fs.set("int", "/notes/secret.txt", data, mtime: 1_700_000_123)
        await fs.register(pager)

        let (cache, _) = makeCache()
        let library = NotesLibrary(cache: cache)
        await library.attach(phone)

        let entry = NoteEntry(vol: "int", name: "secret.txt", size: UInt32(data.count))
        let filled = try await library.stat(entry)

        XCTAssertEqual(filled.mtime, 1_700_000_123)
        XCTAssertEqual(filled.protected, true, "Salted__ ciphertext is protected")
        XCTAssertEqual(filled.size, entry.size)

        await phone.stop()
        await pager.stop()
    }

    // MARK: - Prefetch

    func testPrefetchSequentialAndStoppable() async throws {
        let (phone, pager) = await NotesLibraryTests.makeLibrarySessions()
        let fs = CompanionFakeFs()
        fs.holdReads = .milliseconds(5) // overlapping reads would show in peakConcurrentReads
        var entries: [NoteEntry] = []
        for index in 1...5 {
            let name = "note\(index).txt"
            let data = NotesLibraryTests.bytes("note \(index)")
            fs.set("int", "/notes/" + name, data)
            entries.append(NoteEntry(vol: "int", name: name, size: UInt32(data.count)))
        }
        await fs.register(pager)

        let (cache, _) = makeCache()
        let library = NotesLibrary(cache: cache)
        await library.attach(phone)

        let shown = LibraryCounter()
        await library.prefetch(entries, shouldContinue: { shown.value < 2 }, onRead: { _, _ in
            shown.increment()
        })

        XCTAssertEqual(shown.value, 2, "the third shouldContinue() stops the run")
        XCTAssertEqual(fs.peakConcurrentReads, 1, "prefetch never runs two transfers at once")

        await phone.stop()
        await pager.stop()
    }

    // MARK: - Deletes

    func testNoteDeletedDropsCache() async throws {
        let (phone, pager) = await NotesLibraryTests.makeLibrarySessions()
        let fs = CompanionFakeFs()
        let data = NotesLibraryTests.bytes("bye")
        fs.set("int", "/notes/a.txt", data)
        await fs.register(pager)

        let (cache, root) = makeCache()
        let library = NotesLibrary(cache: cache)
        await library.attach(phone)
        let entry = NoteEntry(vol: "int", name: "a.txt", size: UInt32(data.count))
        _ = try await library.read(entry)
        XCTAssertNotNil(cache.record(vol: "int", name: "a.txt")?.size)

        await library.noteDeleted(vol: "int", name: "a.txt")

        XCTAssertNil(cache.record(vol: "int", name: "a.txt")?.size)
        XCTAssertNil(cache.bytes(for: NoteEntry(
            vol: "int", name: "a.txt", size: UInt32(data.count), crc32: CRC32.checksum(data))))
        XCTAssertFalse(FileManager.default.fileExists(
            atPath: root.appendingPathComponent("int").appendingPathComponent("a.txt").path))

        await phone.stop()
        await pager.stop()
    }

    // MARK: - Preview

    func testFirstLine() {
        XCTAssertEqual(NotePreview.firstLine("\n\n  hello world  \nsecond"), "hello world")
        XCTAssertNil(NotePreview.firstLine(""))
        XCTAssertNil(NotePreview.firstLine("  \n\t\r\n "))
        XCTAssertEqual(NotePreview.firstLine(String(repeating: "a", count: 300)),
                       String(repeating: "a", count: 120))
    }
}
