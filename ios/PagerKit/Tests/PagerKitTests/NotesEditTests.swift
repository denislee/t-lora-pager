import Foundation
import XCTest

@testable import PagerKit

// MARK: - Fake pager filesystem with writes

/// An in-memory pager filesystem behind `fs.stat`, `fs.read`, `fs.write.*`
/// and `fs.delete` (PROTOCOL.md §6.1), for the phone-side editing paths:
/// `LinkSession.writeFile`, `NotesLibrary.write/delete` and `TasksService`.
private final class EditFakeFs: @unchecked Sendable {
    private let lock = NSLock()
    private var files: [String: [UInt8]] = [:]
    private var uploads: [UInt16: (key: String, size: UInt32, crc: UInt32, overwrite: Bool, task: Task<[UInt8], Error>)] = [:]
    private var writeLog: [String] = []
    private var mountedSD = true

    private static func key(_ vol: String, _ path: String) -> String { "\(vol):\(path)" }

    var sdMounted: Bool {
        get { lock.withLock { mountedSD } }
        set { lock.withLock { mountedSD = newValue } }
    }

    func set(_ vol: String, _ path: String, _ bytes: [UInt8]) {
        lock.withLock { files[Self.key(vol, path)] = bytes }
    }

    func bytes(_ vol: String, _ path: String) -> [UInt8]? {
        lock.withLock { files[Self.key(vol, path)] }
    }

    /// "<vol>:<path>" of every committed write, in order.
    var writes: [String] { lock.withLock { writeLog } }

    private func check(_ vol: String) throws {
        if vol == "sd" && !sdMounted { throw RPCError(code: "invalid_argument", msg: "sd not mounted") }
    }

    func register(_ pager: LinkSession) async {
        await pager.handle("fs.stat") { params in
            let vol = params["vol"]?.stringValue ?? "int"
            try self.check(vol)
            guard let bytes = self.bytes(vol, params["path"]?.stringValue ?? "") else {
                throw RPCError(code: "invalid_argument", msg: "not found")
            }
            return ["dir": .bool(false), "size": .number(Double(bytes.count)), "mtime": .number(0),
                    "protected": .bool(NoteCrypto.isEncrypted(bytes))]
        }
        await pager.handle("fs.read") { params in
            let vol = params["vol"]?.stringValue ?? "int"
            try self.check(vol)
            guard let xfer = Params.uint16(params["xfer"]) else {
                throw RPCError(code: "invalid_argument", msg: "bad xfer")
            }
            guard let bytes = self.bytes(vol, params["path"]?.stringValue ?? "") else {
                throw RPCError(code: "invalid_argument", msg: "not found")
            }
            Task { try? await pager.sendBytes(xfer: xfer, data: bytes, timeout: .seconds(5)) }
            return ["xfer": .number(Double(xfer)), "size": .number(Double(bytes.count)),
                    "crc32": .number(Double(CRC32.checksum(bytes))), "protected": .bool(false)]
        }
        await pager.handle("fs.write.begin") { params in
            let vol = params["vol"]?.stringValue ?? "int"
            try self.check(vol)
            let path = params["path"]?.stringValue ?? ""
            guard let size = Params.uint32(params["size"]), let crc = Params.uint32(params["crc32"]) else {
                throw RPCError(code: "invalid_argument", msg: "bad size")
            }
            let id = await pager.openIncoming()
            let task = Task { try await pager.receiveBytes(xfer: id, size: size, crc32: crc, timeout: .seconds(5)) }
            self.lock.withLock {
                self.uploads[id] = (Self.key(vol, path), size, crc, params["overwrite"]?.boolValue ?? false, task)
            }
            return ["xfer": .number(Double(id)), "offset": .number(0)]
        }
        await pager.handle("fs.write.commit") { params in
            guard let id = Params.uint16(params["xfer"]),
                  let upload = self.lock.withLock({ self.uploads.removeValue(forKey: id) }) else {
                throw RPCError(code: "invalid_argument", msg: "no such transfer")
            }
            let data = try await upload.task.value
            try self.lock.withLock {
                if self.files[upload.key] != nil && !upload.overwrite {
                    throw RPCError(code: "invalid_argument", msg: "exists")
                }
                self.files[upload.key] = data
                self.writeLog.append(upload.key)
            }
            return .object([:])
        }
        await pager.handle("fs.write.abort") { params in
            if let id = Params.uint16(params["xfer"]) {
                self.lock.withLock { self.uploads.removeValue(forKey: id) }?.task.cancel()
            }
            return .object([:])
        }
        await pager.handle("fs.delete") { params in
            let vol = params["vol"]?.stringValue ?? "int"
            try self.check(vol)
            let key = Self.key(vol, params["path"]?.stringValue ?? "")
            return try self.lock.withLock {
                guard let bytes = self.files[key] else {
                    throw RPCError(code: "invalid_argument", msg: "not found")
                }
                if let size = params["size"], Params.uint32(size) != UInt32(bytes.count) {
                    throw RPCError(code: "invalid_argument", msg: "changed")
                }
                if let crc = params["crc32"], Params.uint32(crc) != CRC32.checksum(bytes) {
                    throw RPCError(code: "invalid_argument", msg: "changed")
                }
                self.files[key] = nil
                return .object([:])
            }
        }
    }
}

final class NotesEditTests: XCTestCase {
    private func makeSessions() async -> (phone: LinkSession, pager: LinkSession) {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()
        return (phone, pager)
    }

    private func makeLibrary(_ phone: LinkSession) async -> NotesLibrary {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("notes-edit-\(UUID().uuidString)", isDirectory: true)
        let library = NotesLibrary(cache: NotesCache(root: root), readTimeout: .seconds(5))
        await library.attach(phone)
        return library
    }

    // MARK: - LinkSession.writeFile

    func testWriteFileCreatesAndOverwrites() async throws {
        let (phone, pager) = await makeSessions()
        let fs = EditFakeFs()
        await fs.register(pager)

        // Bigger than one DATA chunk, so credits and ACKs are exercised.
        let big = (0..<10_000).map { UInt8($0 % 251) }
        try await phone.writeFile(vol: "int", path: "/notes/a.txt", data: big, overwrite: false, timeout: .seconds(5))
        XCTAssertEqual(fs.bytes("int", "/notes/a.txt"), big)

        do {
            try await phone.writeFile(vol: "int", path: "/notes/a.txt", data: [1], overwrite: false, timeout: .seconds(5))
            XCTFail("expected exists")
        } catch let error as RPCError {
            XCTAssertEqual(error.msg, "exists")
        }

        try await phone.writeFile(vol: "int", path: "/notes/a.txt", data: Array("new".utf8), overwrite: true,
                                  timeout: .seconds(5))
        XCTAssertEqual(fs.bytes("int", "/notes/a.txt"), Array("new".utf8))

        try await phone.writeFile(vol: "int", path: "/notes/empty.txt", data: [], overwrite: true, timeout: .seconds(5))
        XCTAssertEqual(fs.bytes("int", "/notes/empty.txt"), [])

        await phone.stop()
        await pager.stop()
    }

    // MARK: - NoteCrypto.encrypt

    func testEncryptRoundTripsAndUsesFreshSalt() throws {
        let one = try NoteCrypto.encrypt("hello pager", passphrase: "pw")
        let two = try NoteCrypto.encrypt("hello pager", passphrase: "pw")
        XCTAssertTrue(NoteCrypto.isEncrypted(one))
        XCTAssertNotEqual(try NoteCrypto.salt(of: one), try NoteCrypto.salt(of: two))
        XCTAssertEqual(try NoteCrypto.decrypt(one, passphrase: "pw"), "hello pager")
        XCTAssertThrowsError(try NoteCrypto.decrypt(one, passphrase: "nope"))

        let decryptor = NoteDecryptor(passphrase: "pw")
        let sealed = try decryptor.encrypt("é ✓")
        XCTAssertEqual(try decryptor.decrypt(sealed), "é ✓")
        XCTAssertEqual(decryptor.derivations, 1, "the key derived for the save is reused to read it back")
    }

    /// Same bytes as `openssl enc -aes-256-cbc -pbkdf2 -iter 10000 -md sha256
    /// -S 0001020304050607` for "abc" / "pw": decrypting the fixed-salt output
    /// with the independent read path is the cross-check.
    func testEncryptMatchesOpenSSLLayout() throws {
        let salt: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7]
        let sealed = try NoteCrypto.encrypt("abc", passphrase: "pw", salt: salt)
        XCTAssertEqual(Array(sealed.prefix(16)), NoteCrypto.magic + salt)
        XCTAssertEqual(sealed.count, 32, "header + one PKCS7-padded block")
        XCTAssertEqual(try NoteCrypto.decrypt(sealed, passphrase: "pw"), "abc")
    }

    // MARK: - NotesLibrary.write / delete

    func testLibraryWriteRewritesEveryCopy() async throws {
        let (phone, pager) = await makeSessions()
        let fs = EditFakeFs()
        let old = Array("old".utf8)
        fs.set("int", "/notes/n.txt", old)
        fs.set("sd", "/notes/n.txt", Array("older sd copy".utf8))
        await fs.register(pager)
        let library = await makeLibrary(phone)

        let entry = NoteEntry(vol: "int", name: "n.txt", size: 3)
        let mirror = NoteEntry(vol: "sd", name: "n.txt", size: 13)
        let written = try await library.write(entry, mirrors: [entry, mirror], bytes: Array("edited".utf8),
                                              base: CRC32.checksum(old))
        XCTAssertEqual(written.map(\.id), ["int:n.txt", "sd:n.txt"])
        XCTAssertEqual(written.map(\.size), [6, 6])
        XCTAssertEqual(fs.bytes("int", "/notes/n.txt"), Array("edited".utf8))
        XCTAssertEqual(fs.bytes("sd", "/notes/n.txt"), Array("edited".utf8))

        // The cache serves the new bytes without the link.
        await library.attach(nil)
        let cached = try await library.read(written[0])
        XCTAssertEqual(cached, Array("edited".utf8))

        await phone.stop()
        await pager.stop()
    }

    func testLibraryWriteRefusesWhenChangedOnPager() async throws {
        let (phone, pager) = await makeSessions()
        let fs = EditFakeFs()
        fs.set("int", "/notes/n.txt", Array("changed on the pager".utf8))
        await fs.register(pager)
        let library = await makeLibrary(phone)

        let entry = NoteEntry(vol: "int", name: "n.txt", size: 3)
        do {
            _ = try await library.write(entry, mirrors: [], bytes: Array("mine".utf8),
                                        base: CRC32.checksum(Array("old".utf8)))
            XCTFail("expected changedOnPager")
        } catch let error as NotesLibrary.EditError {
            XCTAssertEqual(error, .changedOnPager)
        }
        XCTAssertEqual(fs.bytes("int", "/notes/n.txt"), Array("changed on the pager".utf8))
        XCTAssertTrue(fs.writes.isEmpty)

        // Gone from the pager: also a conflict, nothing recreated.
        let gone = NoteEntry(vol: "int", name: "gone.txt", size: 3)
        do {
            _ = try await library.write(gone, mirrors: [], bytes: [1], base: 0)
            XCTFail("expected changedOnPager")
        } catch let error as NotesLibrary.EditError {
            XCTAssertEqual(error, .changedOnPager)
        }
        XCTAssertNil(fs.bytes("int", "/notes/gone.txt"))

        await phone.stop()
        await pager.stop()
    }

    func testLibraryDeleteConditional() async throws {
        let (phone, pager) = await makeSessions()
        let fs = EditFakeFs()
        let bytes = Array("bye".utf8)
        fs.set("int", "/notes/n.txt", bytes)
        fs.set("sd", "/notes/n.txt", bytes)
        fs.set("int", "/notes/m.txt", Array("edited meanwhile".utf8))
        await fs.register(pager)
        let library = await makeLibrary(phone)

        let base = (size: UInt32(bytes.count), crc32: CRC32.checksum(bytes))
        try await library.delete([NoteEntry(vol: "int", name: "n.txt", size: 3),
                                  NoteEntry(vol: "sd", name: "n.txt", size: 3)],
                                 base: base, conditional: true)
        XCTAssertNil(fs.bytes("int", "/notes/n.txt"))
        XCTAssertNil(fs.bytes("sd", "/notes/n.txt"))

        do {
            try await library.delete([NoteEntry(vol: "int", name: "m.txt", size: 3)], base: base, conditional: true)
            XCTFail("expected changedOnPager")
        } catch let error as NotesLibrary.EditError {
            XCTAssertEqual(error, .changedOnPager)
        }
        XCTAssertNotNil(fs.bytes("int", "/notes/m.txt"))

        await phone.stop()
        await pager.stop()
    }

    // MARK: - Tasks

    func testTasksFileMatchesFirmwareFormat() {
        let items = TasksFile.parse("[ ] milk\r\n\n[x] eggs\n[X] bread\nShopping\n[] not a task")
        XCTAssertEqual(items.map(\.isTask), [true, true, true, false, false])
        XCTAssertEqual(items.map(\.checked), [false, true, true, false, false])
        XCTAssertEqual(items.map(\.text), ["milk", "eggs", "bread", "Shopping", "[] not a task"])
        XCTAssertEqual(TasksFile.serialize(items), "[ ] milk\n[x] eggs\n[x] bread\nShopping\n[] not a task\n")
        XCTAssertEqual(TasksFile.serialize([TaskItem(text: "a\nb")]), "[ ] a b\n")
        XCTAssertEqual(TasksFile.parse(""), [])
    }

    func testTasksLoadPrefersSDAndSaveMirrors() async throws {
        let (phone, pager) = await makeSessions()
        let fs = EditFakeFs()
        fs.set("int", "/tasks.txt", Array("[ ] stale\n".utf8))
        fs.set("sd", "/tasks.txt", Array("[ ] milk\n[x] eggs\n".utf8))
        await fs.register(pager)
        let service = TasksService(session: phone, timeout: .seconds(5))

        var snapshot = try await service.load()
        XCTAssertEqual(snapshot.items.map(\.text), ["milk", "eggs"], "the pager reads the SD copy first")
        XCTAssertTrue(snapshot.sdMounted)

        snapshot.items[0].checked = true
        snapshot = try await service.save(snapshot.items, base: snapshot.base)
        XCTAssertEqual(fs.bytes("int", "/tasks.txt"), Array("[x] milk\n[x] eggs\n".utf8))
        XCTAssertEqual(fs.bytes("sd", "/tasks.txt"), Array("[x] milk\n[x] eggs\n".utf8))

        // Changed on the pager since: refused, nothing written.
        fs.set("sd", "/tasks.txt", Array("[ ] pager edit\n".utf8))
        let before = fs.writes.count
        do {
            _ = try await service.save([], base: snapshot.base)
            XCTFail("expected changedOnPager")
        } catch let error as TasksService.TasksError {
            XCTAssertEqual(error, .changedOnPager)
        }
        XCTAssertEqual(fs.writes.count, before)

        await phone.stop()
        await pager.stop()
    }

    func testTasksWithoutFileOrCard() async throws {
        let (phone, pager) = await makeSessions()
        let fs = EditFakeFs()
        fs.sdMounted = false
        await fs.register(pager)
        let service = TasksService(session: phone, timeout: .seconds(5))

        let empty = try await service.load()
        XCTAssertEqual(empty.items, [])
        XCTAssertNil(empty.base)
        XCTAssertFalse(empty.sdMounted)

        _ = try await service.save([TaskItem(text: "first")], base: nil)
        XCTAssertEqual(fs.bytes("int", "/tasks.txt"), Array("[ ] first\n".utf8))
        XCTAssertEqual(fs.writes, ["int:/tasks.txt"])

        await phone.stop()
        await pager.stop()
    }
}
