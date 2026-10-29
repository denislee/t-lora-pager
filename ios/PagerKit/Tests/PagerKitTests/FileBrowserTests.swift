import Foundation
import XCTest

@testable import PagerKit

/// Records progress callbacks from the session's actor.
private final class ProgressLog: @unchecked Sendable {
    private let lock = NSLock()
    private var values: [(UInt32, UInt32)] = []
    func add(_ done: UInt32, _ total: UInt32) { lock.withLock { values.append((done, total)) } }
    var all: [(UInt32, UInt32)] { lock.withLock { values } }
}

/// The Files screen's PagerKit side: path helpers, preview kinds, and
/// `FileBrowser` against a fake pager filesystem (listing cache, folder
/// changes, byte-exact transfers with progress, restart-from-zero uploads).
final class FileBrowserTests: XCTestCase {
    // MARK: - FilePath

    func testPathHelpers() {
        XCTAssertEqual(FilePath.normalize("notes//a/./b/"), "/notes/a/b")
        XCTAssertEqual(FilePath.normalize(""), "/")
        XCTAssertEqual(FilePath.join("/", "a.txt"), "/a.txt")
        XCTAssertEqual(FilePath.join("/notes", "a.txt"), "/notes/a.txt")
        XCTAssertEqual(FilePath.parent("/notes/a.txt"), "/notes")
        XCTAssertEqual(FilePath.parent("/a.txt"), "/")
        XCTAssertEqual(FilePath.parent("/"), "/")
        XCTAssertEqual(FilePath.name("/notes/a.txt"), "a.txt")
        XCTAssertEqual(FilePath.name("/"), "")
        XCTAssertEqual(FilePath.crumbs("/a/b", rootName: "SD").map(\.name), ["SD", "a", "b"])
        XCTAssertEqual(FilePath.crumbs("/a/b", rootName: "SD").map(\.path), ["/", "/a", "/a/b"])
        XCTAssertEqual(FilePath.crumbs("/", rootName: "Internal").count, 1)
    }

    func testNameValidationAndSanitizing() {
        XCTAssertTrue(FilePath.isValidName("photo 1.jpg"))
        for bad in ["", ".", "..", "a/b", "a\\b", " lead", "trail ", "x\u{01}", String(repeating: "a", count: 129)] {
            XCTAssertFalse(FilePath.isValidName(bad), bad)
        }
        XCTAssertEqual(FilePath.sanitizedName("a/b\\c\n.txt"), "a_b_c_.txt")
        XCTAssertEqual(FilePath.sanitizedName("  "), "file")
        XCTAssertEqual(FilePath.sanitizedName(".."), "file")
        let taken: Set<String> = ["a.txt", "a (2).txt", "README"]
        XCTAssertEqual(FilePath.uniqueName("a.txt", taken: taken.contains), "a (3).txt")
        XCTAssertEqual(FilePath.uniqueName("README", taken: taken.contains), "README (2)")
        XCTAssertEqual(FilePath.uniqueName("new.bin", taken: taken.contains), "new.bin")
        XCTAssertEqual(FilePath.uniqueName(".profile", taken: { $0 == ".profile" }), ".profile (2)")
    }

    func testPreviewKind() {
        XCTAssertEqual(FilePreviewKind.of(name: "a.txt", bytes: []), .text)
        XCTAssertEqual(FilePreviewKind.of(name: "a.bin", bytes: Array("héllo\n".utf8)), .text)
        XCTAssertEqual(FilePreviewKind.of(name: "a.jpg", bytes: [0xFF, 0xD8, 0x00, 0x10]), .quickLook)
        XCTAssertEqual(FilePreviewKind.of(name: "a.txt", bytes: Array("Salted__12345678".utf8) + [1, 2]), .encrypted)
        XCTAssertEqual(FilePreviewKind.of(name: "x", bytes: [0xC3]), .quickLook)  // invalid UTF-8, short
        // A 4 KiB cut through a two-byte character still reads as text.
        let long = Array(String(repeating: "a", count: 4095).utf8) + Array("é".utf8) + Array("tail".utf8)
        XCTAssertEqual(FilePreviewKind.of(name: "log", bytes: long), .text)
        XCTAssertEqual(FilePreviewKind.fileExtension("A.TXT"), "txt")
        XCTAssertEqual(FilePreviewKind.fileExtension(".profile"), "")
    }

    // MARK: - FileBrowser

    private func makeBrowser(_ fs: CompanionFakeFs) async -> (FileBrowser, LinkSession, LinkSession) {
        let (phone, pager) = await fs.makeSessions()
        let browser = FileBrowser(timeout: .seconds(5))
        await browser.attach(phone)
        return (browser, phone, pager)
    }

    func testListingIsCachedUntilInvalidated() async throws {
        let fs = CompanionFakeFs()
        fs.set("sd", "/docs/b.txt", Array("b".utf8))
        fs.set("sd", "/a.txt", Array("a".utf8))
        let (browser, _, _) = await makeBrowser(fs)

        let root = try await browser.list(vol: "sd", path: "/")
        XCTAssertEqual(root.map(\.name), ["docs", "a.txt"])  // folders first
        XCTAssertEqual(root.map(\.dir), [true, false])
        _ = try await browser.list(vol: "sd", path: "/")
        XCTAssertEqual(fs.methods.filter { $0 == "fs.list" }.count, 1, "second list served from cache")

        fs.set("sd", "/c.txt", [])
        await browser.invalidate()  // evt.fs.changed
        let fresh = try await browser.list(vol: "sd", path: "/")
        XCTAssertEqual(fresh.map(\.name), ["docs", "a.txt", "c.txt"])
        _ = try await browser.list(vol: "sd", path: "/", force: true)
        XCTAssertEqual(fs.methods.filter { $0 == "fs.list" }.count, 3)
    }

    func testFolderChanges() async throws {
        let fs = CompanionFakeFs()
        fs.set("int", "/old.txt", Array("x".utf8))
        let (browser, _, _) = await makeBrowser(fs)
        _ = try await browser.list(vol: "int", path: "/")

        let made = try await browser.mkdir(vol: "int", in: "/", name: "Photos")
        XCTAssertEqual(made, "/Photos")
        let names0 = try await browser.list(vol: "int", path: "/").map(\.name)
        XCTAssertEqual(names0, ["Photos", "old.txt"])

        let renamed = try await browser.rename(vol: "int", path: "/old.txt", to: "new.txt")
        XCTAssertEqual(renamed, "/new.txt")
        XCTAssertEqual(fs.bytes("int", "/new.txt"), Array("x".utf8))
        let names1 = try await browser.list(vol: "int", path: "/").map(\.name)
        XCTAssertEqual(names1, ["Photos", "new.txt"])

        try await browser.delete(vol: "int", path: "/Photos")
        XCTAssertFalse(fs.isDir("int", "/Photos"))
        let names2 = try await browser.list(vol: "int", path: "/").map(\.name)
        XCTAssertEqual(names2, ["new.txt"])

        do {
            _ = try await browser.mkdir(vol: "int", in: "/", name: "a/b")
            XCTFail("bad name accepted")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
        }
    }

    func testStatReportsProtected() async throws {
        let fs = CompanionFakeFs()
        fs.set("int", "/notes/s.txt", Array("Salted__abcdefgh".utf8) + [9, 9])
        let (browser, _, _) = await makeBrowser(fs)
        let stat = try await browser.stat(vol: "int", path: "/notes/s.txt")
        XCTAssertTrue(stat.protected)
    }

    func testRoundTripIsByteExactWithProgress() async throws {
        let fs = CompanionFakeFs()
        let (browser, _, _) = await makeBrowser(fs)
        // Several DATA chunks, not a multiple of the chunk size.
        let data = (0..<50_000).map { UInt8(truncatingIfNeeded: $0 &* 31 &+ 7) }

        let up = ProgressLog()
        try await browser.upload(FileBrowser.Upload(vol: "sd", path: "/big.bin", data: data, overwrite: false),
                                 progress: { up.add($0, $1) })
        XCTAssertEqual(fs.bytes("sd", "/big.bin"), data)
        XCTAssertEqual(up.all.last?.0, 50_000)
        XCTAssertTrue(up.all.allSatisfy { $0.1 == 50_000 })

        let down = ProgressLog()
        let (bytes, info) = try await browser.download(vol: "sd", path: "/big.bin", progress: { down.add($0, $1) })
        XCTAssertEqual(bytes, data)
        XCTAssertEqual(info.crc32, CRC32.checksum(data))
        XCTAssertEqual(down.all.last?.0, 50_000)
        XCTAssertEqual(down.all.map(\.0), down.all.map(\.0).sorted(), "progress never goes back")

        do {
            try await browser.upload(FileBrowser.Upload(vol: "sd", path: "/big.bin", data: [1], overwrite: false))
            XCTFail("overwrote without asking")
        } catch let error as RPCError {
            XCTAssertEqual(error.msg, "exists")
        }
        let interrupted = await browser.interrupted
        XCTAssertTrue(interrupted.isEmpty, "a refusal is not a link failure")
    }

    func testInterruptedUploadRestartsFromZero() async throws {
        let fs = CompanionFakeFs()
        let (browser, _, _) = await makeBrowser(fs)
        let data = Array(repeating: UInt8(0xAB), count: 9000)
        fs.failNextCommits(1)

        let upload = FileBrowser.Upload(vol: "int", path: "/x.bin", data: data, overwrite: true)
        do {
            try await browser.upload(upload)
            XCTFail("expected disconnected")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "disconnected")
        }
        var interrupted = await browser.interrupted
        XCTAssertEqual(interrupted.map(\.id), [upload.id])
        XCTAssertNil(fs.bytes("int", "/x.bin"))

        // A new link: the upload is sent again, from byte 0.
        let (phone2, _) = await fs.makeSessions()
        await browser.attach(phone2)
        let log = ProgressLog()
        let result = await browser.retryInterrupted { _, done, total in log.add(done, total) }
        XCTAssertEqual(result.sent.map(\.id), [upload.id])
        XCTAssertTrue(result.failed.isEmpty)
        XCTAssertEqual(fs.bytes("int", "/x.bin"), data)
        // The whole file went again (no resume in v1): every report is out of the full size.
        XCTAssertEqual(log.all.last?.0, 9000)
        XCTAssertTrue(log.all.allSatisfy { $0.1 == 9000 })
        interrupted = await browser.interrupted
        XCTAssertTrue(interrupted.isEmpty)
        XCTAssertEqual(fs.methods.filter { $0 == "fs.write.begin" }.count, 2)
    }

    func testDetachedThrowsNotConnected() async {
        let browser = FileBrowser()
        do {
            _ = try await browser.list(vol: "int", path: "/")
            XCTFail("listed without a link")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "disconnected")
        } catch {
            XCTFail("\(error)")
        }
    }
}
