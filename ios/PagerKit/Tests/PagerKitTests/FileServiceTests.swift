import Foundation
import XCTest

@testable import PagerKit

// MARK: - Tests

/// Exercises `FileService` end to end over two real `LinkSession`s, with the
/// pager standing in for the phone's filesystem.
final class FileServiceTests: XCTestCase {

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

    // MARK: - fs.list

    func testListFollowsCursorPastOnePage() async throws {
        let (phone, pager) = await FileServiceTests.makeSessions()
        let fs = CompanionFakeFs()
        for index in 0..<150 {
            fs.set("int", String(format: "/notes/n%03d.txt", index), Array("x".utf8))
        }
        await fs.register(pager)

        let entries = try await FileServiceTests.makeService(phone).list(vol: "int", path: "/notes")

        XCTAssertEqual(entries.count, 150)
        XCTAssertEqual(entries.map { $0.name }, (0..<150).map { String(format: "n%03d.txt", $0) })

        let lists = fs.requests("fs.list")
        XCTAssertEqual(lists.count, 3, "150 entries at 64 per page")
        XCTAssertEqual(lists.map { $0["cursor"]?.intValue }, [nil, 64, 128])

        await phone.stop()
        await pager.stop()
    }

    func testListFoldersFirstAndFields() async throws {
        let (phone, pager) = await FileServiceTests.makeSessions()
        let fs = CompanionFakeFs()
        fs.set("int", "/notes/a.txt", Array("abcd".utf8))
        fs.set("int", "/notes/b.txt", Array("hello!".utf8))
        fs.mkdirs("int", "/notes/sub")
        await fs.register(pager)

        let entries = try await FileServiceTests.makeService(phone).list(vol: "int", path: "/notes")

        XCTAssertEqual(entries.count, 3)
        XCTAssertEqual(entries.first?.name, "sub")
        XCTAssertEqual(entries.first?.dir, true)
        XCTAssertEqual(entries[1], FsEntry(name: "a.txt", dir: false, size: 4, mtime: 1_700_000_000))
        XCTAssertEqual(entries[2], FsEntry(name: "b.txt", dir: false, size: 6, mtime: 1_700_000_000))

        await phone.stop()
        await pager.stop()
    }

    func testListSDNotMounted() async throws {
        let (phone, pager) = await FileServiceTests.makeSessions()
        let fs = CompanionFakeFs()
        fs.sdMounted = false
        await fs.register(pager)

        do {
            _ = try await FileServiceTests.makeService(phone).list(vol: "sd", path: "/")
            XCTFail("expected sd not mounted")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
            XCTAssertTrue(error.isSDNotMounted, "msg = \(error.msg)")
        }

        await phone.stop()
        await pager.stop()
    }

    func testListInvalidArgumentPassesThrough() async throws {
        let (phone, pager) = await FileServiceTests.makeSessions()
        let fs = CompanionFakeFs()
        fs.set("int", "/notes/a.txt", Array("a".utf8))
        await fs.register(pager)

        do {
            _ = try await FileServiceTests.makeService(phone).list(vol: "int", path: "/notes/missing")
            XCTFail("expected invalid_argument")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
            XCTAssertEqual(error.msg, "not a directory")
        }

        await phone.stop()
        await pager.stop()
    }

    func testListRejectsNonAdvancingCursor() async throws {
        let (phone, pager) = await FileServiceTests.makeSessions()
        await pager.handle("fs.list") { _ in
            [
                "entries": [["name": "a.txt", "dir": false, "size": 4, "mtime": 0]],
                "cursor": 0,
            ]
        }

        do {
            _ = try await FileServiceTests.makeService(phone).list(vol: "int", path: "/notes")
            XCTFail("expected internal")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "internal")
            XCTAssertEqual(error.msg, "fs.list: cursor did not advance")
        }

        await phone.stop()
        await pager.stop()
    }

    func testListBadResponse() async throws {
        let (phone, pager) = await FileServiceTests.makeSessions()
        await pager.handle("fs.list") { _ in
            ["entries": [["name": "a.txt", "dir": false, "mtime": 0]]]
        }

        do {
            _ = try await FileServiceTests.makeService(phone).list(vol: "int", path: "/notes")
            XCTFail("expected internal")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "internal")
            XCTAssertEqual(error.msg, "fs.list: bad response")
        }

        await phone.stop()
        await pager.stop()
    }

    // MARK: - fs.stat

    func testStat() async throws {
        let (phone, pager) = await FileServiceTests.makeSessions()
        let fs = CompanionFakeFs()
        let salted = Array("Salted__".utf8) + Array("ciphertext".utf8)
        fs.set("int", "/notes/secret.txt", salted)
        fs.mkdirs("int", "/notes/sub")
        await fs.register(pager)
        let service = FileServiceTests.makeService(phone)

        let file = try await service.stat(vol: "int", path: "/notes/secret.txt")
        XCTAssertEqual(file, FsStat(dir: false, size: UInt32(salted.count),
                                    mtime: 1_700_000_000, protected: true))

        let folder = try await service.stat(vol: "int", path: "/notes/sub")
        XCTAssertEqual(folder, FsStat(dir: true, size: 0, mtime: 1_700_000_000, protected: false))

        await phone.stop()
        await pager.stop()
    }

    // MARK: - fs.delete

    func testDeleteUnconditional() async throws {
        let (phone, pager) = await FileServiceTests.makeSessions()
        let fs = CompanionFakeFs()
        let bytes = Array("gone".utf8)
        fs.set("int", "/notes/gone.txt", bytes)
        await fs.register(pager)
        let service = FileServiceTests.makeService(phone)

        try await service.delete(vol: "int", path: "/notes/gone.txt")
        XCTAssertFalse(fs.exists("int", "/notes/gone.txt"))

        let deletes = fs.requests("fs.delete")
        XCTAssertEqual(deletes.count, 1)
        XCTAssertNil(deletes.first?["size"], "an unconditional delete carries no precondition")
        XCTAssertNil(deletes.first?["crc32"], "an unconditional delete carries no precondition")

        do {
            _ = try await service.stat(vol: "int", path: "/notes/gone.txt")
            XCTFail("expected not found")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
            XCTAssertEqual(error.msg, "not found")
        }

        await phone.stop()
        await pager.stop()
    }

    func testDeleteIfMatchingDeletes() async throws {
        let (phone, pager) = await FileServiceTests.makeSessions()
        let fs = CompanionFakeFs()
        let bytes = Array("hello".utf8)
        fs.set("int", "/notes/a.txt", bytes)
        await fs.register(pager)

        try await FileServiceTests.makeService(phone).delete(
            vol: "int", path: "/notes/a.txt",
            ifSize: UInt32(bytes.count), ifCRC: CRC32.checksum(bytes)
        )
        XCTAssertFalse(fs.exists("int", "/notes/a.txt"))

        let params = try XCTUnwrap(fs.requests("fs.delete").first)
        XCTAssertEqual(params["size"]?.intValue, bytes.count)
        XCTAssertEqual(params["crc32"]?.intValue, Int(CRC32.checksum(bytes)))

        await phone.stop()
        await pager.stop()
    }

    func testDeleteIfChangedKeepsFile() async throws {
        let (phone, pager) = await FileServiceTests.makeSessions()
        let fs = CompanionFakeFs()
        let bytes = Array("hello".utf8)
        fs.set("int", "/notes/a.txt", bytes)
        await fs.register(pager)

        do {
            try await FileServiceTests.makeService(phone).delete(
                vol: "int", path: "/notes/a.txt",
                ifSize: UInt32(bytes.count), ifCRC: CRC32.checksum(bytes) ^ 1
            )
            XCTFail("expected changed")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
            XCTAssertTrue(error.isChanged, "msg = \(error.msg)")
        }
        XCTAssertTrue(fs.exists("int", "/notes/a.txt"), "a changed file is kept")

        await phone.stop()
        await pager.stop()
    }

    func testDeleteIfStaleSizeKeepsFile() async throws {
        let (phone, pager) = await FileServiceTests.makeSessions()
        let fs = CompanionFakeFs()
        let bytes = Array("hello".utf8)
        fs.set("int", "/notes/a.txt", bytes)
        await fs.register(pager)

        do {
            try await FileServiceTests.makeService(phone).delete(
                vol: "int", path: "/notes/a.txt",
                ifSize: UInt32(bytes.count + 1), ifCRC: CRC32.checksum(bytes)
            )
            XCTFail("expected changed")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
            XCTAssertTrue(error.isChanged, "msg = \(error.msg)")
        }
        XCTAssertTrue(fs.exists("int", "/notes/a.txt"), "a stale size keeps the file")

        await phone.stop()
        await pager.stop()
    }
}
