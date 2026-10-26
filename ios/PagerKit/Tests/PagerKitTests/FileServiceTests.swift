import Foundation
import XCTest

@testable import PagerKit

// MARK: - Fake pager filesystem

/// A lock-protected in-memory filesystem the pager-side `fs.list` / `fs.stat` /
/// `fs.delete` handlers serve from, recording every request it answered.
private final class FakeFs: @unchecked Sendable {
    private struct Node {
        let dir: Bool
        let bytes: [UInt8]
    }

    /// Every listing reports this mtime.
    private static let mtime: Int64 = 1_700_000_000
    /// What fs.delete answers when the file no longer matches its precondition.
    private static let changed = RPCError(code: "invalid_argument", msg: "changed")

    private let lock = NSLock()
    private var volumes: [String: [String: Node]] = [:]
    private var mountedSD = true
    private var log: [(method: String, params: JSONValue)] = []

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

    func set(_ vol: String, _ path: String, _ bytes: [UInt8]) {
        lock.lock()
        defer { lock.unlock() }
        volumes[vol, default: [:]][path] = Node(dir: false, bytes: bytes)
    }

    func mkdir(_ vol: String, _ path: String) {
        lock.lock()
        defer { lock.unlock() }
        volumes[vol, default: [:]][path] = Node(dir: true, bytes: [])
    }

    func exists(_ vol: String, _ path: String) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return volumes[vol]?[path] != nil
    }

    func record(_ method: String, _ params: JSONValue) {
        lock.lock()
        defer { lock.unlock() }
        log.append((method, params))
    }

    /// Every request, oldest first.
    var requests: [(method: String, params: JSONValue)] {
        lock.lock()
        defer { lock.unlock() }
        return log
    }

    /// The params of every request for `method`, oldest first.
    func requests(_ method: String) -> [JSONValue] {
        lock.lock()
        defer { lock.unlock() }
        return log.filter { $0.method == method }.map { $0.params }
    }

    // MARK: - Handlers

    /// Install the three handlers on a pager `LinkSession`.
    func register(_ pager: LinkSession) async {
        await pager.handle("fs.list") { params in
            self.record("fs.list", params)
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
                        "mtime": .number(Double(FakeFs.mtime)),
                    ]
                }),
            ]
            let next = start + page.count
            if next < items.count { reply["cursor"] = .number(Double(next)) }
            return .object(reply)
        }

        await pager.handle("fs.stat") { params in
            self.record("fs.stat", params)
            let vol = params["vol"]?.stringValue ?? "int"
            let path = params["path"]?.stringValue ?? ""
            try self.check(vol: vol)
            return try self.stat(vol: vol, path: path)
        }

        await pager.handle("fs.delete") { params in
            self.record("fs.delete", params)
            let vol = params["vol"]?.stringValue ?? "int"
            let path = params["path"]?.stringValue ?? ""
            try self.check(vol: vol)
            try self.delete(vol: vol, path: path, params: params)
            return .object([:])
        }
    }

    // MARK: - Contract

    /// vol "sd" while the card is out, and volumes the pager does not have.
    private func check(vol: String) throws {
        lock.lock()
        defer { lock.unlock() }
        if vol == "sd" && !mountedSD {
            throw RPCError(code: "invalid_argument", msg: "sd not mounted")
        }
        guard volumes[vol] != nil else {
            throw RPCError(code: "invalid_argument", msg: "bad vol")
        }
    }

    /// The direct children of `path`: folders first, then files, each by name.
    private func items(_ vol: String, _ path: String) throws -> [(name: String, dir: Bool, size: UInt32)] {
        lock.lock()
        defer { lock.unlock() }
        guard let nodes = volumes[vol] else {
            throw RPCError(code: "invalid_argument", msg: "bad vol")
        }
        // A folder exists when it was made or anything lives under it.
        let implied = nodes.keys.contains { FakeFs.parent(of: $0) == path }
        guard path == "/" || nodes[path]?.dir == true || implied else {
            throw RPCError(code: "invalid_argument", msg: "not a directory")
        }
        var out: [(name: String, dir: Bool, size: UInt32)] = []
        for (candidate, node) in nodes where FakeFs.parent(of: candidate) == path {
            out.append((FakeFs.name(of: candidate), node.dir, UInt32(node.bytes.count)))
        }
        out.sort { left, right in
            if left.dir != right.dir { return left.dir }
            return left.name < right.name
        }
        return out
    }

    private func stat(vol: String, path: String) throws -> JSONValue {
        lock.lock()
        defer { lock.unlock() }
        guard let node = volumes[vol]?[path] else {
            throw RPCError(code: "invalid_argument", msg: "not found")
        }
        return [
            "dir": .bool(node.dir),
            "size": .number(Double(node.bytes.count)),
            "mtime": .number(Double(FakeFs.mtime)),
            "protected": .bool(node.bytes.starts(with: Array("Salted__".utf8))),
        ]
    }

    /// Remove `path`, but only while it still has the size/CRC32 the request
    /// carries (PROTOCOL.md §6.1, cap `fs.delete.if`).
    private func delete(vol: String, path: String, params: JSONValue) throws {
        lock.lock()
        defer { lock.unlock() }
        guard let node = volumes[vol]?[path] else {
            throw RPCError(code: "invalid_argument", msg: "not found")
        }
        if let size = params["size"], Params.uint32(size) != UInt32(node.bytes.count) {
            throw FakeFs.changed
        }
        if let crc = params["crc32"], Params.uint32(crc) != CRC32.checksum(node.bytes) {
            throw FakeFs.changed
        }
        volumes[vol]?[path] = nil
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
        let fs = FakeFs()
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
        let fs = FakeFs()
        fs.set("int", "/notes/a.txt", Array("abcd".utf8))
        fs.set("int", "/notes/b.txt", Array("hello!".utf8))
        fs.mkdir("int", "/notes/sub")
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
        let fs = FakeFs()
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
        let fs = FakeFs()
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
        let fs = FakeFs()
        let salted = Array("Salted__".utf8) + Array("ciphertext".utf8)
        fs.set("int", "/notes/secret.txt", salted)
        fs.mkdir("int", "/notes/sub")
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
        let fs = FakeFs()
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
        let fs = FakeFs()
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
        let fs = FakeFs()
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
        let fs = FakeFs()
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
