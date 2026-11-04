import XCTest

@testable import PagerKit

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
        let fs = CompanionFakeFs()
        fs.set("int", "/notes/a.txt", [0], mtime: 0)
        fs.set("int", "/notes/b.txt", [0], mtime: 0)
        fs.set("sd", "/notes/b.txt", [0], mtime: 0)
        fs.set("sd", "/notes/c.txt", [0], mtime: 0)
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
        let fs = CompanionFakeFs()
        fs.set("int", "/notes/a.txt", [0], mtime: 0)
        fs.set("sd", "/notes/b.txt", [0], mtime: 0)
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
        let fs = CompanionFakeFs()
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
