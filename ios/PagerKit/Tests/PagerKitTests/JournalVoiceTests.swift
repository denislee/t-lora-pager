import Foundation
import XCTest

@testable import PagerKit

/// The Journal tab's grouping/naming/append rules (`ui_journal.cpp`,
/// `ui_text_editor.cpp`), `NotesLibrary.create`, and the voice-memo importer
/// (`ui_audio_notes.cpp` layout) against a fake pager filesystem.
final class JournalVoiceTests: XCTestCase {
    private var utc: Calendar {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(identifier: "UTC")!
        return calendar
    }

    // MARK: - Journal

    func testTimestampParsing() {
        XCTAssertEqual(Journal.timestamp(ofNoteName: "20260418_120530.txt"),
                       DateComponents(year: 2026, month: 4, day: 18, hour: 12, minute: 5, second: 30))
        XCTAssertNotNil(Journal.timestamp(ofNoteName: "20260418_120530_2.txt"))
        for bad in ["shopping.txt", "20261318_120530.txt", "20260418-120530.txt", "20260418_250530.txt", "2026"] {
            XCTAssertNil(Journal.timestamp(ofNoteName: bad), bad)
        }
        XCTAssertEqual(Journal.title(ofNoteName: "20260418_120530.txt"), "Apr 18, 2026 - 12:05:30")
        XCTAssertEqual(Journal.title(ofNoteName: "ideas.txt"), "ideas.txt")
    }

    func testNoteNamesMatchTheEditor() {
        let date = Date(timeIntervalSince1970: 1_761_700_000)  // 2025-10-29 01:06:40 UTC
        XCTAssertEqual(Journal.noteName(for: date, calendar: utc), "20251029_010640.txt")
        let taken: Set<String> = ["20251029_010640.txt", "20251029_010640_2.txt"]
        XCTAssertEqual(Journal.uniqueNoteName(for: date, calendar: utc, taken: taken.contains),
                       "20251029_010640_3.txt")
        XCTAssertNotNil(Journal.timestamp(ofNoteName: "20251029_010640_3.txt"))
    }

    func testGroupingAndTodayEntry() {
        let names = ["20261029_080000.txt", "20261028_230000.txt", "20261029_210000.txt", "ideas.txt",
                     "20261029_120000.txt"]
        let entries = names.map { NoteEntry(vol: "int", name: $0, size: 1) }
        let grouped = Journal.group(entries)
        XCTAssertEqual(grouped.days.map(\.id), ["2026-10-29", "2026-10-28"])
        XCTAssertEqual(grouped.days[0].entries.map(\.name),
                       ["20261029_210000.txt", "20261029_120000.txt", "20261029_080000.txt"])
        XCTAssertEqual(grouped.undated.map(\.name), ["ideas.txt"])

        let today = DateComponents(year: 2026, month: 10, day: 29)
        XCTAssertEqual(Journal.todayEntry(entries, today: today)?.name, "20261029_210000.txt")
        XCTAssertNil(Journal.todayEntry(entries, today: DateComponents(year: 2026, month: 10, day: 30)))
    }

    func testAppending() {
        XCTAssertEqual(Journal.appending("  more  ", to: "first line\n\n"), "first line\n\nmore\n")
        XCTAssertEqual(Journal.appending("only", to: " \n"), "only\n")
    }

    func testLibraryCreateMirrorsToSDAndMakesTheFolder() async throws {
        let fs = CompanionFakeFs()
        fs.mkdirs("int", "/notes")  // SD has no /notes yet
        let (phone, _) = await fs.makeSessions()
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("lib-\(UUID().uuidString)")
        let library = NotesLibrary(cache: NotesCache(root: root), readTimeout: .seconds(5))
        await library.attach(phone)

        let bytes = Array("hello\n".utf8)
        let written = try await library.create(name: "20261029_080000.txt", bytes: bytes, mirrorSD: true)
        XCTAssertEqual(written.map(\.vol), ["int", "sd"])
        XCTAssertEqual(fs.bytes("int", "/notes/20261029_080000.txt"), bytes)
        XCTAssertEqual(fs.bytes("sd", "/notes/20261029_080000.txt"), bytes)
        XCTAssertEqual(written.first?.crc32, CRC32.checksum(bytes))

        do {
            _ = try await library.create(name: "20261029_080000.txt", bytes: [1], mirrorSD: false)
            XCTFail("overwrote an existing note")
        } catch let error as RPCError {
            XCTAssertEqual(error.msg, "exists")
        }

        fs.sdMounted = false
        let internalOnly = try await library.create(name: "20261029_090000.txt", bytes: bytes, mirrorSD: true)
        XCTAssertEqual(internalOnly.map(\.vol), ["int"], "card out: internal only")
    }

    // MARK: - Voice memos

    func testMemoListingAndNaming() {
        let listing = [
            FsEntry(name: "note_20260418_120530.wav", dir: false, size: 44 + 32_000 * 65, mtime: 0),
            FsEntry(name: "note_20260419_080000.wav", dir: false, size: 44, mtime: 0),
            FsEntry(name: "readme.txt", dir: false, size: 3, mtime: 0),
            FsEntry(name: "old.WAV", dir: false, size: 3, mtime: 0),
            FsEntry(name: "sub.wav", dir: true, size: 0, mtime: 0),
        ]
        let memos = VoiceMemos.memos(from: listing)
        XCTAssertEqual(memos.map(\.name), ["note_20260419_080000.wav", "note_20260418_120530.wav"])
        XCTAssertEqual(memos[1].duration, 65, accuracy: 0.001)
        XCTAssertEqual(memos[0].duration, 0)
        XCTAssertEqual(memos[1].path, "/mental_notes/note_20260418_120530.wav")
        XCTAssertEqual(memos[1].recordedAt?.hour, 12)
        XCTAssertEqual(VoiceMemos.noteName(for: memos[1], fallback: Date(), calendar: utc), "20260418_120530.txt")
        let odd = VoiceMemo(name: "x.wav", size: 100)
        XCTAssertEqual(VoiceMemos.noteName(for: odd, fallback: Date(timeIntervalSince1970: 1_761_700_000),
                                           calendar: utc), "20251029_010640.txt")
        XCTAssertEqual(VoiceMemos.noteText(for: memos[1], transcript: " olá mundo \n"),
                       "Voice memo note_20260418_120530.wav (1:05)\n\nolá mundo\n")
    }

    private func importer(_ phone: LinkSession, caps: [String], transcript: String = "hello there")
        -> VoiceMemoImporter {
        VoiceMemoImporter(session: phone, pagerCaps: caps, timeout: .seconds(5)) { wav in
            XCTAssertEqual(Array(wav.prefix(4)), Array("RIFF".utf8))
            return transcript
        }
    }

    private static let wav: [UInt8] = Array("RIFF".utf8) + Array(repeating: 0, count: 40) + Array(repeating: 7, count: 3200)

    func testImportWritesNoteAndDeletesConditionally() async throws {
        let fs = CompanionFakeFs()
        fs.set("sd", "/mental_notes/note_20260418_120530.wav", Self.wav)
        let (phone, _) = await fs.makeSessions()
        let memos = try await importer(phone, caps: ["fs", "fs.delete.if"]).list()
        XCTAssertEqual(memos.map(\.name), ["note_20260418_120530.wav"])

        let notes = NotesBox()
        let outcome = try await importer(phone, caps: ["fs", "fs.delete.if"])
            .importMemo(memos[0], deleteAfterImport: true) { name, text in
                notes.add(name, text)
                return name
            }
        XCTAssertEqual(outcome.noteName, "20260418_120530.txt")
        XCTAssertEqual(outcome.transcript, "hello there")
        XCTAssertTrue(outcome.deleted)
        XCTAssertNil(fs.bytes("sd", "/mental_notes/note_20260418_120530.wav"))
        XCTAssertEqual(notes.all.first?.1.hasSuffix("hello there\n"), true)
    }

    func testImportKeepsTheRecordingByDefaultAndWhenNothingWasHeard() async throws {
        let fs = CompanionFakeFs()
        fs.set("sd", "/mental_notes/note_20260418_120530.wav", Self.wav)
        let (phone, _) = await fs.makeSessions()
        let memo = VoiceMemo(name: "note_20260418_120530.wav", size: UInt32(Self.wav.count))

        let kept = try await importer(phone, caps: ["fs"]).importMemo(memo, deleteAfterImport: false) { n, _ in n }
        XCTAssertFalse(kept.deleted)
        XCTAssertNotNil(fs.bytes("sd", memo.path))

        do {
            _ = try await importer(phone, caps: ["fs"], transcript: "  ")
                .importMemo(memo, deleteAfterImport: true) { n, _ in n }
            XCTFail("empty transcript saved")
        } catch let error as VoiceMemoImporter.ImportError {
            XCTAssertEqual(error, .emptyTranscript)
        }
        XCTAssertNotNil(fs.bytes("sd", memo.path), "nothing deleted without a note")

        do {
            _ = try await importer(phone, caps: ["fs"])
                .importMemo(VoiceMemo(name: "big.wav", size: VoiceMemos.maxUploadBytes + 1), deleteAfterImport: true) { n, _ in n }
            XCTFail("too large accepted")
        } catch let error as VoiceMemoImporter.ImportError {
            XCTAssertEqual(error, .tooLarge)
        }
    }

    func testImportWithoutConditionalCapDeletesPlainly() async throws {
        let fs = CompanionFakeFs()
        fs.set("sd", "/mental_notes/note_20260418_120530.wav", Self.wav)
        let (phone, _) = await fs.makeSessions()
        let memo = VoiceMemo(name: "note_20260418_120530.wav", size: UInt32(Self.wav.count))
        let outcome = try await importer(phone, caps: ["fs"]).importMemo(memo, deleteAfterImport: true) { n, _ in n }
        XCTAssertTrue(outcome.deleted)
    }

    func testListWithoutFolderOrCard() async throws {
        let fs = CompanionFakeFs()
        let (phone, _) = await fs.makeSessions()
        let empty = try await importer(phone, caps: []).list()
        XCTAssertTrue(empty.isEmpty, "no /mental_notes yet")
        fs.sdMounted = false
        do {
            _ = try await importer(phone, caps: []).list()
            XCTFail("listed without a card")
        } catch let error as RPCError {
            XCTAssertTrue(error.isSDNotMounted)
        }
    }
}

/// Notes the importer's writer was handed.
private final class NotesBox: @unchecked Sendable {
    private let lock = NSLock()
    private var items: [(String, String)] = []
    func add(_ name: String, _ text: String) { lock.withLock { items.append((name, text)) } }
    var all: [(String, String)] { lock.withLock { items } }
}
