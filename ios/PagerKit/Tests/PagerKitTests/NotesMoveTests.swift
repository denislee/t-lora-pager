import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
import XCTest

@testable import PagerKit

// MARK: - Pager-side faults

private extension CompanionFakeFs {
    /// The k-th `fs.delete` (0-based) and every later one drop the link; nil
    /// restores it.
    func dropLinkAtDelete(_ index: Int?) {
        guard let index else {
            before("fs.delete", nil)
            return
        }
        before("fs.delete") { [self] _ in
            if requests("fs.delete").count > index {
                throw RPCError(code: "disconnected", msg: "link dropped")
            }
        }
    }

    /// `path` gains one `edit\n` when an `fs.delete` for it arrives, so the
    /// delete's precondition fails.
    func editBeforeDelete(_ vol: String, _ path: String) {
        before("fs.delete") { [self] params in
            if params["path"]?.stringValue == path, let current = bytes(vol, path) {
                set(vol, path, current + Array("edit\n".utf8))
            }
        }
    }
}

// MARK: - Event collector

/// Collects `evt.notes.sync` params from a session's events, shared between the
/// consuming task and the test under a lock.
private final class MoveEvents: @unchecked Sendable {
    private let lock = NSLock()
    private var notes: [JSONValue] = []
    private var finished = false

    /// Record one event; returns true when it was the terminal (`done`) event.
    @discardableResult
    func record(_ params: JSONValue) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        notes.append(params)
        let done = params["done"]?.boolValue == true
        if done { finished = true }
        return done
    }

    var isFinished: Bool {
        lock.lock()
        defer { lock.unlock() }
        return finished
    }

    var values: [JSONValue] {
        lock.lock()
        defer { lock.unlock() }
        return notes
    }
}

// MARK: - Request log

/// A lock-protected record of the HTTP requests a backend sent, in order.
private final class MoveRequestLog: @unchecked Sendable {
    private let lock = NSLock()
    private var entries: [String] = []

    func append(_ entry: String) {
        lock.lock()
        defer { lock.unlock() }
        entries.append(entry)
    }

    var all: [String] {
        lock.lock()
        defer { lock.unlock() }
        return entries
    }
}

// MARK: - Tests

/// The "move notes to GitHub" engine: pull, upload what is missing, verify
/// against the new commit's tree, then delete only what is provably there.
final class NotesMoveTests: XCTestCase {

    // MARK: - Helpers

    private static func makeSessions() async -> (phone: LinkSession, pager: LinkSession) {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()
        return (phone, pager)
    }

    private static func makeService(_ phone: LinkSession, _ repo: FakeGitHubRepo,
                                    caps: [String] = ["fs.delete.if"]) -> NotesSyncService {
        let (backend, _) = FakeGitHubRepo.backend(repo)
        return NotesSyncService(session: phone, github: backend, phoneDirectory: nil,
                                pagerCaps: caps)
    }

    private static func note(_ vol: String, _ name: String) -> NoteEntry {
        NoteEntry(vol: vol, name: name, size: 0)
    }

    /// One `notes.sync` manifest entry built from the note's real bytes.
    private static func manifest(_ vol: String, _ name: String, _ bytes: [UInt8],
                                 crc32: UInt32? = nil) -> JSONValue {
        .object([
            "name": .string(name),
            "size": .number(Double(bytes.count)),
            "mtime": .number(0),
            "crc32": .number(Double(crc32 ?? CRC32.checksum(bytes))),
            "vol": .string(vol),
        ])
    }

    /// Collect `evt.notes.sync` params until the terminal `done:true` event or
    /// `timeout`, whichever comes first.
    private static func collectNotes(_ pager: LinkSession, timeout: Duration) async -> [JSONValue] {
        let collector = MoveEvents()
        let consumer = Task {
            for await event in pager.events where event.name == "evt.notes.sync" {
                if collector.record(event.params) { return }
            }
        }
        let deadline = ContinuousClock.now + timeout
        while !collector.isFinished && ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(2))
        }
        consumer.cancel()
        return collector.values
    }

    // MARK: - The move engine

    func testNewNoteUploadedVerifiedDeleted() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        let bytes = Array("hello".utf8)
        fs.set("int", "/notes/a.txt", bytes)
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        let service = NotesMoveTests.makeService(phone, repo)
        let result = await service.move([NotesMoveTests.note("int", "a.txt")])

        XCTAssertEqual(result.uploaded, 1)
        XCTAssertEqual(result.deleted, 1)
        XCTAssertTrue(result.kept.isEmpty, "kept = \(result.kept)")
        XCTAssertTrue(result.errors.isEmpty, "errors = \(result.errors)")
        XCTAssertEqual(repo.notes()["a.txt"] ?? [], bytes)
        XCTAssertFalse(fs.exists("int", "/notes/a.txt"))
        XCTAssertEqual(result.commitSHA, repo.headSHA)
        XCTAssertEqual(repo.commitListRefs, [result.commitSHA ?? "no commit"],
                       "verified against the new commit's tree, not the branch")

        await phone.stop()
        await pager.stop()
    }

    func testSameNameSameSHAIsAlreadyAndDeletedWithoutUpload() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        let bytes = Array("same".utf8)
        fs.set("int", "/notes/a.txt", bytes)
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        repo.seed(["a.txt": bytes])
        let service = NotesMoveTests.makeService(phone, repo)
        let result = await service.move([NotesMoveTests.note("int", "a.txt")])

        XCTAssertEqual(result.already, 1)
        XCTAssertEqual(result.uploaded, 0)
        XCTAssertEqual(result.deleted, 1)
        XCTAssertEqual(repo.commitPosts, 0)
        XCTAssertEqual(repo.blobPosts, 0)
        XCTAssertNil(result.commitSHA)
        XCTAssertFalse(fs.exists("int", "/notes/a.txt"))

        await phone.stop()
        await pager.stop()
    }

    func testSameNameOtherSHAGoesUpUnderContentName() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        let old = Array("old".utf8)
        let new = Array("new".utf8)
        fs.set("int", "/notes/a.txt", new)
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        repo.seed(["a.txt": old])
        let service = NotesMoveTests.makeService(phone, repo)
        let result = await service.move([NotesMoveTests.note("int", "a.txt")])

        XCTAssertEqual(result.uploaded, 1)
        XCTAssertEqual(result.deleted, 1)
        let conflict = NotesSyncService.conflictName("a.txt", sha: GitBlob.sha1(new))
        let remote = repo.notes()
        XCTAssertEqual(remote["a.txt"] ?? [], old, "the repo's old bytes are never overwritten")
        XCTAssertEqual(remote[conflict] ?? [], new)
        XCTAssertNotNil(conflict.range(of: #"^a~[0-9a-f]{7}\.txt$"#, options: .regularExpression),
                        "conflict name = \(conflict)")

        await phone.stop()
        await pager.stop()
    }

    func testRetryAfterCrashBetweenCommitAndDelete() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        let bytes = Array("retry".utf8)
        fs.set("int", "/notes/a.txt", bytes)
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        let service = NotesMoveTests.makeService(phone, repo)

        // The commit lands, then the link drops on the first delete: the note
        // stays on the pager.
        fs.dropLinkAtDelete(0)
        let first = await service.move([NotesMoveTests.note("int", "a.txt")])
        XCTAssertEqual(first.uploaded, 1)
        XCTAssertEqual(first.deleted, 0)
        XCTAssertEqual(first.kept.count, 1)
        XCTAssertEqual(first.kept.first?.reason, NotesSyncService.keepLinkDropped)
        XCTAssertTrue(fs.exists("int", "/notes/a.txt"))

        // The retry finds the note already in the repo and deletes it without
        // a second commit.
        fs.dropLinkAtDelete(nil)
        let second = await service.move([NotesMoveTests.note("int", "a.txt")])
        XCTAssertEqual(second.already, 1)
        XCTAssertEqual(second.uploaded, 0)
        XCTAssertEqual(second.deleted, 1)
        XCTAssertEqual(repo.commitPosts, 1, "no new commit on the retry")
        XCTAssertFalse(fs.exists("int", "/notes/a.txt"))

        await phone.stop()
        await pager.stop()
    }

    func testCommitFailsNothingDeleted() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        let aBytes = Array("alpha".utf8)
        let bBytes = Array("bravo".utf8)
        fs.set("int", "/notes/a.txt", aBytes)
        fs.set("int", "/notes/b.txt", bBytes)
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        repo.seed(["b.txt": bBytes])
        repo.failCommits = true
        let service = NotesMoveTests.makeService(phone, repo)
        let result = await service.move([
            NotesMoveTests.note("int", "a.txt"),
            NotesMoveTests.note("int", "b.txt"),
        ])

        XCTAssertEqual(result.deleted, 0)
        XCTAssertTrue(fs.exists("int", "/notes/a.txt"))
        XCTAssertTrue(fs.exists("int", "/notes/b.txt"))
        XCTAssertEqual(result.kept.count, 2)
        for issue in result.kept {
            XCTAssertTrue(issue.reason.hasPrefix(NotesSyncService.keepCommitFailed),
                          "reason = \(issue.reason)")
        }
        XCTAssertTrue(fs.requests("fs.delete").isEmpty)

        await phone.stop()
        await pager.stop()
    }

    func testVerifyShowsDifferentSHAKept() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        let bytes = Array("tampered".utf8)
        fs.set("int", "/notes/a.txt", bytes)
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        repo.tamper = ["a.txt": "0000000000000000000000000000000000000000"]
        let service = NotesMoveTests.makeService(phone, repo)
        let result = await service.move([NotesMoveTests.note("int", "a.txt")])

        XCTAssertEqual(result.kept.count, 1)
        XCTAssertEqual(result.kept.first?.reason, NotesSyncService.keepNotVerified)
        XCTAssertEqual(result.deleted, 0)
        XCTAssertTrue(fs.exists("int", "/notes/a.txt"))
        XCTAssertEqual(result.uploaded, 1)

        await phone.stop()
        await pager.stop()
    }

    func testVerifyRequestFailsAllKept() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        fs.set("int", "/notes/a.txt", Array("alpha".utf8))
        fs.set("int", "/notes/b.txt", Array("bravo".utf8))
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        repo.failCommitTreeReads = true
        let service = NotesMoveTests.makeService(phone, repo)
        let result = await service.move([
            NotesMoveTests.note("int", "a.txt"),
            NotesMoveTests.note("int", "b.txt"),
        ])

        XCTAssertEqual(result.kept.count, 2)
        for issue in result.kept {
            XCTAssertTrue(issue.reason.hasPrefix(NotesSyncService.keepNotVerified),
                          "reason = \(issue.reason)")
        }
        XCTAssertEqual(result.deleted, 0)
        XCTAssertTrue(fs.requests("fs.delete").isEmpty)

        await phone.stop()
        await pager.stop()
    }

    func testPagerAnswersChangedKept() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        let bytes = Array("editme".utf8)
        fs.set("int", "/notes/a.txt", bytes)
        fs.editBeforeDelete("int", "/notes/a.txt")
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        let service = NotesMoveTests.makeService(phone, repo)
        let result = await service.move([NotesMoveTests.note("int", "a.txt")])

        XCTAssertEqual(result.kept.count, 1)
        XCTAssertEqual(result.kept.first?.reason, NotesSyncService.keepChanged)
        XCTAssertEqual(result.deleted, 0)
        XCTAssertTrue(fs.exists("int", "/notes/a.txt"))
        XCTAssertEqual(fs.bytes("int", "/notes/a.txt") ?? [], bytes + Array("edit\n".utf8))

        await phone.stop()
        await pager.stop()
    }

    func testNoConditionalDeleteCapUploadsButDeletesNothing() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        let bytes = Array("alpha".utf8)
        fs.set("int", "/notes/a.txt", bytes)
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        let service = NotesMoveTests.makeService(phone, repo, caps: [])
        let result = await service.move([NotesMoveTests.note("int", "a.txt")])

        XCTAssertEqual(result.uploaded, 1)
        XCTAssertEqual(result.deleted, 0)
        XCTAssertEqual(result.kept.count, 1)
        XCTAssertEqual(result.kept.first?.reason, NotesSyncService.keepNoConditionalDelete)
        XCTAssertFalse(result.canDelete)
        XCTAssertTrue(fs.requests("fs.delete").isEmpty)
        XCTAssertEqual(repo.notes()["a.txt"] ?? [], bytes)
        XCTAssertTrue(fs.exists("int", "/notes/a.txt"))

        await phone.stop()
        await pager.stop()
    }

    func testLinkDropsMidDeleteRestKept() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        fs.set("int", "/notes/a.txt", Array("a".utf8))
        fs.set("int", "/notes/b.txt", Array("b".utf8))
        fs.set("int", "/notes/c.txt", Array("c".utf8))
        fs.dropLinkAtDelete(1)
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        let service = NotesMoveTests.makeService(phone, repo)
        let result = await service.move([
            NotesMoveTests.note("int", "a.txt"),
            NotesMoveTests.note("int", "b.txt"),
            NotesMoveTests.note("int", "c.txt"),
        ])

        XCTAssertEqual(result.deleted, 1)
        XCTAssertEqual(result.kept.count, 2)
        for issue in result.kept {
            XCTAssertEqual(issue.reason, NotesSyncService.keepLinkDropped,
                           "reason = \(issue.reason)")
        }
        XCTAssertEqual(fs.requests("fs.delete").count, 2, "the engine stops after the first drop")
        XCTAssertFalse(fs.exists("int", "/notes/a.txt"))
        XCTAssertTrue(fs.exists("int", "/notes/b.txt"))
        XCTAssertTrue(fs.exists("int", "/notes/c.txt"))

        await phone.stop()
        await pager.stop()
    }

    func testTruncatedListingPullsNothing() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        fs.set("int", "/notes/a.txt", Array("alpha".utf8))
        fs.set("int", "/notes/b.txt", Array("bravo".utf8))
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        repo.seed(["remote.txt": Array("remote".utf8)])
        repo.truncateListing = true
        let service = NotesMoveTests.makeService(phone, repo)
        let result = await service.move([
            NotesMoveTests.note("int", "a.txt"),
            NotesMoveTests.note("int", "b.txt"),
        ])

        XCTAssertEqual(result.errors.count, 2, "every input reports the failed listing")
        XCTAssertEqual(result.uploaded, 0)
        XCTAssertTrue(fs.requests("fs.read").isEmpty, "a failed listing aborts before any pull")
        XCTAssertEqual(repo.commitPosts, 0)
        XCTAssertEqual(result.deleted, 0)
        XCTAssertTrue(fs.requests("fs.delete").isEmpty)
        XCTAssertTrue(fs.exists("int", "/notes/a.txt"))
        XCTAssertTrue(fs.exists("int", "/notes/b.txt"))

        await phone.stop()
        await pager.stop()
    }

    func testSDAndInternalSameNameBothMoved() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        let one = Array("one".utf8)
        let two = Array("two".utf8)
        fs.set("int", "/notes/a.txt", one)
        fs.set("sd", "/notes/a.txt", two)
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        let service = NotesMoveTests.makeService(phone, repo)
        let result = await service.move([
            NotesMoveTests.note("int", "a.txt"),
            NotesMoveTests.note("sd", "a.txt"),
        ])

        XCTAssertEqual(result.deleted, 2)
        let conflict = NotesSyncService.conflictName("a.txt", sha: GitBlob.sha1(two))
        let remote = repo.notes()
        XCTAssertEqual(remote["a.txt"] ?? [], one)
        XCTAssertEqual(remote[conflict] ?? [], two)
        XCTAssertEqual(repo.commitPosts, 1)

        await phone.stop()
        await pager.stop()
    }

    func testIdenticalSDAndInternalCopiesUploadedOnce() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        let bytes = Array("identical".utf8)
        fs.set("int", "/notes/a.txt", bytes)
        fs.set("sd", "/notes/a.txt", bytes)
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        let service = NotesMoveTests.makeService(phone, repo)
        let result = await service.move([
            NotesMoveTests.note("int", "a.txt"),
            NotesMoveTests.note("sd", "a.txt"),
        ])

        XCTAssertEqual(result.uploaded, 1)
        XCTAssertEqual(result.already, 1)
        XCTAssertEqual(result.deleted, 2)
        XCTAssertEqual(repo.blobPosts, 1)
        XCTAssertEqual(repo.notes()["a.txt"] ?? [], bytes)

        await phone.stop()
        await pager.stop()
    }

    func testSecondRunFindsNothingToDo() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        let bytes = Array("alpha".utf8)
        fs.set("int", "/notes/a.txt", bytes)
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        let service = NotesMoveTests.makeService(phone, repo)
        let first = await service.move([NotesMoveTests.note("int", "a.txt")])
        XCTAssertEqual(first.deleted, 1)

        let empty = await service.move([])
        XCTAssertTrue(empty.nothingToDo)

        // The note is gone from the pager; naming it again is a read error,
        // never a crash or a phantom delete.
        let again = await service.move([NotesMoveTests.note("int", "a.txt")])
        XCTAssertEqual(again.errors.count, 1)
        XCTAssertEqual(again.deleted, 0)

        await phone.stop()
        await pager.stop()
    }

    func testUnsafeNameIsErrorAndKeptOnPager() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        let service = NotesMoveTests.makeService(phone, repo)
        let result = await service.move([NotesMoveTests.note("int", ".hidden.txt")])

        XCTAssertEqual(result.errors.count, 1)
        XCTAssertTrue(fs.requests("fs.read").isEmpty, "an unsafe name is rejected before any pull")
        XCTAssertEqual(result.deleted, 0)
        XCTAssertTrue(fs.requests("fs.delete").isEmpty)
        XCTAssertEqual(repo.commitPosts, 0)

        await phone.stop()
        await pager.stop()
    }

    func testEncryptedNoteMovesAsCiphertext() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        let cipher = Array("Salted__".utf8) + (0..<40).map { UInt8(($0 * 17 + 3) & 0xFF) }
        fs.set("int", "/notes/a.txt", cipher)
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        let service = NotesMoveTests.makeService(phone, repo)
        let result = await service.move([NotesMoveTests.note("int", "a.txt")])

        XCTAssertEqual(result.uploaded, 1)
        XCTAssertEqual(result.deleted, 1)
        XCTAssertEqual(repo.notes()["a.txt"] ?? [], cipher, "the Salted__ ciphertext goes up verbatim")

        await phone.stop()
        await pager.stop()
    }

    // MARK: - The pager's notes.sync trigger

    func testPagerTriggerMovesAndReportsDeletedKept() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        let aBytes = Array("alpha".utf8)
        let bBytes = Array("bravo".utf8)
        fs.set("int", "/notes/a.txt", aBytes)
        fs.set("int", "/notes/b.txt", bBytes)
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        let service = NotesMoveTests.makeService(phone, repo)
        let reported = ReportedResults()
        await service.onPagerMove { result in reported.append(result) }
        await service.register()

        async let events = NotesMoveTests.collectNotes(pager, timeout: .seconds(5))
        let reply = try await pager.call("notes.sync", params: [
            "targets": ["github"],
            "files": [
                NotesMoveTests.manifest("int", "a.txt", aBytes),
                NotesMoveTests.manifest("int", "b.txt", bBytes),
            ],
        ], timeout: .seconds(5))
        let collected = await events
        await service.wait(job: reply["job"]?.intValue ?? 0)
        // The app's session log and viewer hear about the pager's move.
        XCTAssertEqual(reported.values.map(\.deleted), [2])

        let last = collected.last
        XCTAssertEqual(last?["uploaded"]?.intValue, 2)
        XCTAssertEqual(last?["already"]?.intValue, 0)
        XCTAssertEqual(last?["deleted"]?.intValue, 2)
        XCTAssertEqual(last?["kept"]?.intValue, 0)
        XCTAssertEqual(last?["errors"], JSONValue.array([]))
        XCTAssertFalse(fs.exists("int", "/notes/a.txt"))
        XCTAssertFalse(fs.exists("int", "/notes/b.txt"))

        await phone.stop()
        await pager.stop()
    }

    func testPagerTriggerManifestMismatchIsErrorNotDeleted() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        let bytes = Array("alpha".utf8)
        fs.set("int", "/notes/a.txt", bytes)
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        let service = NotesMoveTests.makeService(phone, repo)
        await service.register()

        async let events = NotesMoveTests.collectNotes(pager, timeout: .seconds(5))
        _ = try await pager.call("notes.sync", params: [
            "targets": ["github"],
            "files": [
                NotesMoveTests.manifest("int", "a.txt", bytes,
                                        crc32: CRC32.checksum(bytes) ^ 1),
            ],
        ], timeout: .seconds(5))
        let collected = await events

        let last = collected.last
        XCTAssertEqual(last?["errors"], JSONValue.array([.string("a.txt")]))
        XCTAssertTrue(fs.exists("int", "/notes/a.txt"))
        XCTAssertEqual(last?["deleted"]?.intValue, 0)

        await phone.stop()
        await pager.stop()
    }

    func testTwoTriggersRunOneAfterTheOther() async throws {
        let (phone, pager) = await NotesMoveTests.makeSessions()
        let fs = CompanionFakeFs()
        let aBytes = Array("alpha".utf8)
        let bBytes = Array("bravo".utf8)
        fs.set("int", "/notes/a.txt", aBytes)
        fs.set("int", "/notes/b.txt", bBytes)
        await fs.register(pager)

        let repo = FakeGitHubRepo()
        let log = MoveRequestLog()
        let server = StubServer { r in
            log.append(r.method + " " + r.path)
            return repo.respond(r)
        }
        let backend = GitHubNotesBackend(
            config: {
                GitHubNotesConfig(repo: FakeGitHubRepo.repoName,
                                  branch: FakeGitHubRepo.branchName, token: "tok")
            },
            session: StubURLProtocol.session(server),
            baseBackoff: .milliseconds(1)
        )
        let service = NotesSyncService(session: phone, github: backend, phoneDirectory: nil,
                                       pagerCaps: [NotesSyncService.conditionalDeleteCap])
        await service.register()

        async let events = NotesMoveTests.collectNotes(pager, timeout: .seconds(5))
        async let first = service.move([NotesMoveTests.note("int", "a.txt")])
        _ = try await pager.call("notes.sync", params: [
            "targets": ["github"],
            "files": [NotesMoveTests.manifest("int", "b.txt", bBytes)],
        ], timeout: .seconds(5))
        let collected = await events
        let firstResult = await first

        XCTAssertEqual(firstResult.deleted, 1)
        XCTAssertEqual(collected.last?["deleted"]?.intValue, 1)
        XCTAssertFalse(fs.exists("int", "/notes/a.txt"))
        XCTAssertFalse(fs.exists("int", "/notes/b.txt"))

        let entries = log.all
        let mainListings = entries.indices.filter {
            entries[$0].hasSuffix("/git/trees/main:notes")
        }
        let commitReads = entries.indices.filter {
            entries[$0].contains("/git/trees/commit-") && entries[$0].hasSuffix(":notes")
        }
        XCTAssertEqual(mainListings.count, 2, "one fresh listing per run; log = \(entries)")
        XCTAssertFalse(commitReads.isEmpty, "log = \(entries)")
        if mainListings.count >= 2, let firstCommitRead = commitReads.first {
            XCTAssertGreaterThan(mainListings[1], firstCommitRead,
                                 "the second run starts only after the first verified its commit")
        }

        await phone.stop()
        await pager.stop()
    }

    // MARK: - The rule (no sessions)

    func testDeletableRules() {
        let note = PulledNote(vol: "int", name: "a.txt", size: 1, crc32: 1,
                              sha: "abc", remoteName: "a.txt", upload: true)

        XCTAssertEqual(
            NotesSyncService.deletable(note, MoveFacts(commitFailure: "x", verified: nil,
                                                       verifyFailure: nil, canDelete: true)),
            .keep("\(NotesSyncService.keepCommitFailed): x")
        )

        let noRead = NotesSyncService.deletable(
            note, MoveFacts(commitFailure: nil, verified: nil,
                            verifyFailure: "boom", canDelete: true))
        if case .keep(let reason) = noRead {
            XCTAssertTrue(reason.contains("boom"), "reason = \(reason)")
        } else {
            XCTFail("want keep, got \(noRead)")
        }

        XCTAssertEqual(
            NotesSyncService.deletable(note, MoveFacts(commitFailure: nil,
                                                       verified: ["a.txt": "zzz"],
                                                       verifyFailure: nil, canDelete: true)),
            .keep(NotesSyncService.keepNotVerified)
        )
        XCTAssertEqual(
            NotesSyncService.deletable(note, MoveFacts(commitFailure: nil,
                                                       verified: ["a.txt": "abc"],
                                                       verifyFailure: nil, canDelete: false)),
            .keep(NotesSyncService.keepNoConditionalDelete)
        )
        XCTAssertEqual(
            NotesSyncService.deletable(note, MoveFacts(commitFailure: nil,
                                                       verified: ["a.txt": "abc"],
                                                       verifyFailure: nil, canDelete: true)),
            .delete
        )
    }

    func testConflictName() {
        XCTAssertEqual(NotesSyncService.conflictName("a.txt", sha: "0123456789"), "a~0123456.txt")
        XCTAssertEqual(NotesSyncService.conflictName("x", sha: "0123456789"), "x~0123456")
    }
}

/// Results handed to `NotesSyncService.onPagerMove`, collected under a lock.
private final class ReportedResults: @unchecked Sendable {
    private let lock = NSLock()
    private var results: [NotesMoveResult] = []

    func append(_ result: NotesMoveResult) {
        lock.lock()
        defer { lock.unlock() }
        results.append(result)
    }

    var values: [NotesMoveResult] {
        lock.lock()
        defer { lock.unlock() }
        return results
    }
}
