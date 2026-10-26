import XCTest

@testable import PagerKit

// MARK: - Fake pager filesystem

/// A lock-protected in-memory filesystem the pager-side `fs.list` handler
/// serves from: the subset of `FakeFs` that `NotesIndexer.scan` needs, without
/// fs.stat or fs.delete. Hardware listings report mtime 0.
private final class IndexerFakeFs: @unchecked Sendable {
    private struct Node {
        let dir: Bool
        let size: UInt32
    }

    private let lock = NSLock()
    private var volumes: [String: [String: Node]] = [:]
    private var mountedSD = true

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

    func set(_ vol: String, _ path: String, size: UInt32 = 1) {
        lock.lock()
        defer { lock.unlock() }
        volumes[vol, default: [:]][path] = Node(dir: false, size: size)
    }

    func mkdir(_ vol: String, _ path: String) {
        lock.lock()
        defer { lock.unlock() }
        volumes[vol, default: [:]][path] = Node(dir: true, size: 0)
    }

    /// Install the `fs.list` handler on a pager `LinkSession`.
    func register(_ pager: LinkSession) async {
        await pager.handle("fs.list") { params in
            let vol = params["vol"]?.stringValue ?? "int"
            let path = params["path"]?.stringValue ?? ""
            try self.check(vol: vol)
            let items = try self.items(vol, path)

            let limit = params["limit"].flatMap { Params.int64($0) }.map { Int($0) } ?? 64
            let cursor = params["cursor"].flatMap { Params.int64($0) }.map { Int($0) } ?? 0
            let start = min(max(0, cursor), items.count)
            let end = min(start + max(1, min(64, limit)), items.count)
            let page = items[start..<end]

            var reply: [String: JSONValue] = [
                "entries": .array(page.map { item -> JSONValue in
                    [
                        "name": .string(item.name),
                        "dir": .bool(item.dir),
                        "size": .number(Double(item.size)),
                        "mtime": .number(0),
                    ]
                }),
            ]
            let next = start + page.count
            if next < items.count { reply["cursor"] = .number(Double(next)) }
            return .object(reply)
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

    /// The direct children of `path`: folders first, then files, each by name.
    /// A folder exists when it was made or anything lives under it; a volume
    /// with no such folder answers "not a directory".
    private func items(_ vol: String, _ path: String) throws -> [(name: String, dir: Bool, size: UInt32)] {
        lock.lock()
        defer { lock.unlock() }
        let nodes = volumes[vol] ?? [:]
        // A folder exists when it was made or anything lives under it.
        let implied = nodes.keys.contains { IndexerFakeFs.parent(of: $0) == path }
        guard path == "/" || nodes[path]?.dir == true || implied else {
            throw RPCError(code: "invalid_argument", msg: "not a directory")
        }
        var out: [(name: String, dir: Bool, size: UInt32)] = []
        for (candidate, node) in nodes where IndexerFakeFs.parent(of: candidate) == path {
            out.append((IndexerFakeFs.name(of: candidate), node.dir, node.size))
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

// MARK: - Tests

/// Exercises `NotesIndexer` against the Journal's rules, then end to end over
/// two real `LinkSession`s with the pager standing in for its filesystem.
final class NotesIndexerTests: XCTestCase {

    // MARK: - Helpers

    private static func makeSessions() async -> (phone: LinkSession, pager: LinkSession) {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()
        return (phone, pager)
    }

    private static func makeService(_ phone: LinkSession) -> FileService {
        FileService(session: phone, timeout: .seconds(5))
    }

    private static func entry(_ name: String, dir: Bool = false,
                              size: UInt32 = 1, mtime: Int64 = 0) -> FsEntry {
        FsEntry(name: name, dir: dir, size: size, mtime: mtime)
    }

    // MARK: - Filtering

    func testFiltersDirsAndNonTxt() {
        let listing = [
            NotesIndexerTests.entry("sub", dir: true),
            NotesIndexerTests.entry("a.txt"),
            NotesIndexerTests.entry("b.md"),
            NotesIndexerTests.entry("c.TXT"),
            NotesIndexerTests.entry("d.txt.bak"),
            NotesIndexerTests.entry("e.txt"),
        ]

        let names = NotesIndexer.notes(from: listing, vol: "int").map(\.name)

        XCTAssertEqual(names, ["e.txt", "a.txt"])
    }

    func testKeepsNotesTasksFileLikeJournal() {
        let listing = [NotesIndexerTests.entry("tasks.txt")]

        let notes = NotesIndexer.notes(from: listing, vol: "int")

        XCTAssertEqual(notes.map(\.name), ["tasks.txt"],
                       "only the root /tasks.txt is dropped, not /notes/tasks.txt")
        XCTAssertEqual(notes.first?.path, "/notes/tasks.txt")
    }

    // MARK: - Order

    func testDescendingByteOrder() {
        let listing = [
            NotesIndexerTests.entry("20250101_120000.txt"),
            NotesIndexerTests.entry("20251231_235959.txt"),
            NotesIndexerTests.entry("B.txt"),
            NotesIndexerTests.entry("a.txt"),
            NotesIndexerTests.entry("é.txt"),
        ]

        let names = NotesIndexer.notes(from: listing, vol: "int").map(\.name)

        XCTAssertEqual(names, [
            "é.txt",
            "a.txt",
            "B.txt",
            "20251231_235959.txt",
            "20250101_120000.txt",
        ])
    }

    // MARK: - mtime

    func testMtimeZeroBecomesNil() {
        let listing = [
            NotesIndexerTests.entry("zero.txt", mtime: 0),
            NotesIndexerTests.entry("stamped.txt", mtime: 1_700_000_123),
        ]

        let notes = NotesIndexer.notes(from: listing, vol: "int")

        XCTAssertNil(notes.first { $0.name == "zero.txt" }?.mtime,
                     "hardware listings report 0")
        XCTAssertEqual(notes.first { $0.name == "stamped.txt" }?.mtime, 1_700_000_123)
    }

    // MARK: - Merge

    func testMergeInternalWins() {
        let internalNotes = [
            NoteEntry(vol: "int", name: "a.txt", size: 1),
            NoteEntry(vol: "int", name: "b.txt", size: 2),
        ]
        let sdNotes = [
            NoteEntry(vol: "sd", name: "b.txt", size: 9),
            NoteEntry(vol: "sd", name: "c.txt", size: 3),
        ]

        let merged = NotesIndexer.merge(internal: internalNotes, sd: sdNotes)

        XCTAssertEqual(merged.map(\.name), ["c.txt", "b.txt", "a.txt"])
        XCTAssertEqual(merged.first { $0.name == "b.txt" },
                       NoteEntry(vol: "int", name: "b.txt", size: 2),
                       "an internal note shadows the SD one of the same name")
    }

    // MARK: - scan

    func testScanShadowedSDNoteInAll() async throws {
        let (phone, pager) = await NotesIndexerTests.makeSessions()
        let fs = IndexerFakeFs()
        fs.set("int", "/notes/a.txt")
        fs.set("int", "/notes/b.txt")
        fs.set("sd", "/notes/b.txt")
        fs.set("sd", "/notes/c.txt")
        await fs.register(pager)

        let scan = try await NotesIndexer.scan(NotesIndexerTests.makeService(phone))

        XCTAssertEqual(scan.merged.map(\.name), ["c.txt", "b.txt", "a.txt"])
        XCTAssertEqual(scan.merged.first { $0.name == "b.txt" }?.vol, "int")
        XCTAssertEqual(scan.all.map(\.id),
                       ["int:b.txt", "int:a.txt", "sd:c.txt", "sd:b.txt"],
                       "the sync moves the shadowed SD note too")
        XCTAssertTrue(scan.sdMounted)

        await phone.stop()
        await pager.stop()
    }

    func testScanSDNotMounted() async throws {
        let (phone, pager) = await NotesIndexerTests.makeSessions()
        let fs = IndexerFakeFs()
        fs.set("int", "/notes/a.txt")
        fs.set("sd", "/notes/b.txt")
        fs.sdMounted = false
        await fs.register(pager)

        let scan = try await NotesIndexer.scan(NotesIndexerTests.makeService(phone))

        XCTAssertEqual(scan.merged.map(\.id), ["int:a.txt"])
        XCTAssertEqual(scan.all.map(\.id), ["int:a.txt"])
        XCTAssertFalse(scan.sdMounted)

        await phone.stop()
        await pager.stop()
    }

    func testScanMissingNotesFolder() async throws {
        let (phone, pager) = await NotesIndexerTests.makeSessions()
        let fs = IndexerFakeFs()
        await fs.register(pager)

        let scan = try await NotesIndexer.scan(NotesIndexerTests.makeService(phone))

        XCTAssertTrue(scan.merged.isEmpty)
        XCTAssertTrue(scan.all.isEmpty)
        XCTAssertTrue(scan.sdMounted, "an SD card with no notes folder is still mounted")

        await phone.stop()
        await pager.stop()
    }

    func testScanLinkErrorThrows() async throws {
        let (phone, pager) = await NotesIndexerTests.makeSessions()
        await pager.handle("fs.list") { params in
            let vol = params["vol"]?.stringValue ?? "int"
            if vol == "int" {
                throw RPCError(code: "internal", msg: "io error")
            }
            return .object(["entries": .array([])])
        }

        do {
            _ = try await NotesIndexer.scan(NotesIndexerTests.makeService(phone))
            XCTFail("expected internal")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "internal")
            XCTAssertEqual(error.msg, "io error")
        }

        await phone.stop()
        await pager.stop()
    }
}
