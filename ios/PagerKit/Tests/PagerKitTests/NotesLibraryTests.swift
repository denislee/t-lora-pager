import Foundation
import XCTest

@testable import PagerKit

// MARK: - Fake pager filesystem

/// A lock-protected in-memory pager filesystem behind `fs.list`, `fs.stat` and
/// `fs.read`: the subset `NotesLibrary` needs, with the listing's mtime, the
/// `Salted__` flag, and the peak number of `fs.read` handlers running at once.
private final class LibraryFakeFs: @unchecked Sendable {
    private struct Node {
        let dir: Bool
        let mtime: Int64
    }

    private struct Item {
        let name: String
        let dir: Bool
        let size: UInt32
        let mtime: Int64
    }

    private let lock = NSLock()
    private var volumes: [String: [String: Node]] = [:]
    private var contents: [String: [UInt8]] = [:]
    private var log: [(vol: String, path: String)] = []
    private var activeReads = 0
    private var peakReads = 0
    private var mountedSD = true

    private static func key(_ vol: String, _ path: String) -> String { "\(vol):\(path)" }

    /// Whether vol "sd" is in the pager.
    var sdMounted: Bool {
        get {
            lock.lock()
            defer { lock.unlock() }
            return mountedSD
        }
        set {
            lock.lock()
            defer { lock.unlock() }
            mountedSD = newValue
        }
    }

    /// The most `fs.read` handlers that were running at the same moment.
    var peakConcurrentReads: Int {
        lock.lock()
        defer { lock.unlock() }
        return peakReads
    }

    func set(_ vol: String, _ path: String, bytes: [UInt8], mtime: Int64 = 1_700_000_000) {
        lock.lock()
        defer { lock.unlock() }
        volumes[vol, default: [:]][path] = Node(dir: false, mtime: mtime)
        contents[Self.key(vol, path)] = bytes
    }

    func bytes(_ vol: String, _ path: String) -> [UInt8]? {
        lock.lock()
        defer { lock.unlock() }
        return contents[Self.key(vol, path)]
    }

    /// How many times a `(vol, path)` was read over the link.
    func reads(_ vol: String, _ path: String) -> Int {
        lock.lock()
        defer { lock.unlock() }
        return log.filter { $0.vol == vol && $0.path == path }.count
    }

    // MARK: - Handlers

    /// Install the pager-side `fs.list` / `fs.stat` / `fs.read` handlers.
    func register(_ pager: LinkSession) async {
        await pager.handle("fs.list") { params in
            let vol = params["vol"]?.stringValue ?? "int"
            let path = params["path"]?.stringValue ?? ""
            try self.check(vol: vol)
            let items = try self.children(vol, path)

            let limit = params["limit"].flatMap { Params.int64($0) }.map { Int($0) } ?? 64
            let cursor = params["cursor"].flatMap { Params.int64($0) }.map { Int($0) } ?? 0
            let start = min(max(0, cursor), items.count)
            let end = min(start + max(1, min(64, limit)), items.count)
            let page = items[start..<end]

            var reply: [String: JSONValue] = [
                "entries": .array(page.map { item -> JSONValue in
                    .object([
                        "name": .string(item.name),
                        "dir": .bool(item.dir),
                        "size": .number(Double(item.size)),
                        "mtime": .number(Double(item.mtime)),
                    ])
                }),
            ]
            let next = start + page.count
            if next < items.count { reply["cursor"] = .number(Double(next)) }
            return .object(reply)
        }

        await pager.handle("fs.stat") { params in
            let vol = params["vol"]?.stringValue ?? "int"
            let path = params["path"]?.stringValue ?? ""
            try self.check(vol: vol)
            guard let item = self.node(vol, path) else {
                throw RPCError(code: "invalid_argument", msg: "not found")
            }
            return .object([
                "dir": .bool(item.dir),
                "size": .number(Double(item.size)),
                "mtime": .number(Double(item.mtime)),
                "protected": .bool(self.isProtected(vol, path)),
            ])
        }

        await pager.handle("fs.read") { params in
            let vol = params["vol"]?.stringValue ?? "int"
            let path = params["path"]?.stringValue ?? ""
            guard let xfer = Params.uint16(params["xfer"]) else {
                throw RPCError(code: "invalid_argument", msg: "bad xfer")
            }
            try self.check(vol: vol)
            self.record(vol, path)
            guard let bytes = self.bytes(vol, path) else {
                throw RPCError(code: "invalid_argument", msg: "not found")
            }

            self.beginRead()
            defer { self.endRead() }
            // Keep the handler open long enough that two overlapping requests
            // would both be visible in peakConcurrentReads.
            try? await Task.sleep(for: .milliseconds(5))
            Task {
                try? await pager.sendBytes(xfer: xfer, data: bytes, timeout: .seconds(5))
            }
            return .object([
                "xfer": .number(Double(xfer)),
                "size": .number(Double(bytes.count)),
                "crc32": .number(Double(CRC32.checksum(bytes))),
                "protected": .bool(NoteCrypto.isEncrypted(bytes)),
            ])
        }
    }

    // MARK: - Contract

    /// vol "sd" while the card is out.
    private func check(vol: String) throws {
        lock.lock()
        defer { lock.unlock() }
        if vol == "sd" && !mountedSD {
            throw RPCError(code: "invalid_argument", msg: "sd not mounted")
        }
    }

    private func record(_ vol: String, _ path: String) {
        lock.lock()
        defer { lock.unlock() }
        log.append((vol, path))
    }

    private func beginRead() {
        lock.lock()
        defer { lock.unlock() }
        activeReads += 1
        peakReads = max(peakReads, activeReads)
    }

    private func endRead() {
        lock.lock()
        defer { lock.unlock() }
        activeReads = max(0, activeReads - 1)
    }

    /// One path's type, size and mtime; nil when it does not exist.
    private func node(_ vol: String, _ path: String) -> Item? {
        lock.lock()
        defer { lock.unlock() }
        guard let stored = volumes[vol]?[path] else { return nil }
        return Item(
            name: Self.name(of: path),
            dir: stored.dir,
            size: UInt32(contents[Self.key(vol, path)]?.count ?? 0),
            mtime: stored.mtime
        )
    }

    private func isProtected(_ vol: String, _ path: String) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return NoteCrypto.isEncrypted(contents[Self.key(vol, path)] ?? [])
    }

    /// The direct children of `path`: folders first, then files, each by name.
    /// A folder exists when it was made or anything lives under it; a volume
    /// with no such folder answers "not a directory".
    private func children(_ vol: String, _ path: String) throws -> [Item] {
        lock.lock()
        defer { lock.unlock() }
        let nodes = volumes[vol] ?? [:]
        let implied = nodes.keys.contains { Self.parent(of: $0) == path }
        guard path == "/" || nodes[path]?.dir == true || implied else {
            throw RPCError(code: "invalid_argument", msg: "not a directory")
        }
        var out: [Item] = []
        for (candidate, stored) in nodes where Self.parent(of: candidate) == path {
            let name = Self.name(of: candidate)
            out.append(Item(
                name: name,
                dir: stored.dir,
                size: UInt32(contents[Self.key(vol, candidate)]?.count ?? 0),
                mtime: stored.mtime
            ))
        }
        out.sort { left, right in
            if left.dir != right.dir { return left.dir }
            return left.name < right.name
        }
        return out
    }

    // MARK: - Paths

    private static func parent(of path: String) -> String {
        guard let slash = path.lastIndex(of: "/") else { return "/" }
        let head = String(path[..<slash])
        return head.isEmpty ? "/" : head
    }

    private static func name(of path: String) -> String {
        guard let slash = path.lastIndex(of: "/") else { return path }
        return String(path[path.index(after: slash)...])
    }
}

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
        let fs = LibraryFakeFs()
        fs.set("int", "/notes/a.txt", bytes: NotesLibraryTests.bytes("alpha"))
        fs.set("int", "/notes/b.txt", bytes: NotesLibraryTests.bytes("bravo"))
        fs.set("sd", "/notes/b.txt", bytes: NotesLibraryTests.bytes("bravo sd"))
        fs.set("sd", "/notes/c.txt", bytes: NotesLibraryTests.bytes("charlie"))
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
        let fs = LibraryFakeFs()
        let data = NotesLibraryTests.bytes("hello")
        fs.set("int", "/notes/a.txt", bytes: data)
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
        let fs = LibraryFakeFs()
        let data = NotesLibraryTests.bytes("hello")
        fs.set("int", "/notes/a.txt", bytes: data)
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
        let fs = LibraryFakeFs()
        let data = NotesLibraryTests.saltedBytes()
        fs.set("int", "/notes/secret.txt", bytes: data, mtime: 1_700_000_123)
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
        let fs = LibraryFakeFs()
        var entries: [NoteEntry] = []
        for index in 1...5 {
            let name = "note\(index).txt"
            let data = NotesLibraryTests.bytes("note \(index)")
            fs.set("int", "/notes/" + name, bytes: data)
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
        let fs = LibraryFakeFs()
        let data = NotesLibraryTests.bytes("bye")
        fs.set("int", "/notes/a.txt", bytes: data)
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
