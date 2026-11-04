import Foundation
import XCTest

@testable import PagerKit

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
        let fs = CompanionFakeFs()
        fs.mkdirs("int", "/notes")
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
        let fs = CompanionFakeFs()
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
        let fs = CompanionFakeFs()
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
        let fs = CompanionFakeFs()
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
        let fs = CompanionFakeFs()
        fs.set("int", "/tasks.txt", Array("[ ] stale\n".utf8))
        fs.set("sd", "/tasks.txt", Array("[ ] milk\n[x] eggs\n".utf8))
        await fs.register(pager)
        let service = TasksService(session: phone, timeout: .seconds(5))

        var snapshot = try await service.load()
        XCTAssertEqual(snapshot.items.map(\.text), ["milk", "eggs"], "the pager reads the SD copy first")

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
        let fs = CompanionFakeFs()
        fs.sdMounted = false
        await fs.register(pager)
        let service = TasksService(session: phone, timeout: .seconds(5))

        let empty = try await service.load()
        XCTAssertEqual(empty.items, [])
        XCTAssertNil(empty.base)

        _ = try await service.save([TaskItem(text: "first")], base: nil)
        XCTAssertEqual(fs.bytes("int", "/tasks.txt"), Array("[ ] first\n".utf8))
        XCTAssertEqual(fs.writes, ["int:/tasks.txt"])

        await phone.stop()
        await pager.stop()
    }
}
