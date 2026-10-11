import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
import XCTest

@testable import PagerKit

// MARK: - Mock GitHub

/// In-memory stand-in for the handful of GitHub Git Data API endpoints
/// `GitHubNotesBackend.commit` drives. A faithful port of the Go
/// `mockGitHubState` + `newMockGitHubServer` (`server/internal/notessync`),
/// answering `StubRequest`s in-process instead of through an httptest server.
private final class MockGitHub: @unchecked Sendable {
    private let lock = NSLock()

    // Configuration (the Go struct's fields, same names).
    var notesFiles: Set<String> = []      // listRemote result
    var headSHA = "commit-0"
    var treeSHA = "tree-0"
    var blobFailContent: Set<String> = [] // content_b64 -> fail this blob create
    var refConflictsLeft = 0              // PATCH /git/refs returns 422 this many times

    // Counters and last-request capture.
    private var blobSeq = 0
    private var treeSeq = 0
    private var commitSeq = 0
    var branchHeadCalls = 0
    var blobCalls = 0
    var treeCalls = 0
    var commitCalls = 0
    var refCalls = 0
    var lastTreeEntries: [(path: String, sha: String)] = []
    var lastCommitMsg = ""
    var blobContents: [String] = [] // every base64 content a blob create received
    var unexpected = 0              // requests no route matched (must stay 0)

    func respond(_ r: StubRequest) -> StubResponse {
        lock.lock()
        defer { lock.unlock() }

        switch (r.method, r.path) {
        case ("GET", let path) where path.hasSuffix("/contents/notes"):
            if notesFiles.isEmpty { return StubResponse(status: 404) }
            let entries: [JSONValue] = notesFiles.sorted().map {
                .object(["name": .string($0), "type": "file"])
            }
            return .jsonValue(200, .array(entries))

        case ("GET", let path) where path.contains("/branches/"):
            branchHeadCalls += 1
            return .jsonValue(200, .object([
                "commit": .object([
                    "sha": .string(headSHA),
                    "commit": .object(["tree": .object(["sha": .string(treeSHA)])]),
                ]),
            ]))

        case ("POST", let path) where path.hasSuffix("/git/blobs"):
            blobCalls += 1
            let body = try? JSONValue.decode(bytes: Array(r.body))
            let content = body?["content"]?.stringValue ?? ""
            blobContents.append(content)
            if blobFailContent.contains(content) {
                return .json(422, #"{"message":"blob create failed"}"#)
            }
            blobSeq += 1
            return .jsonValue(201, .object(["sha": .string("blob-\(blobSeq)")]))

        case ("POST", let path) where path.hasSuffix("/git/trees"):
            treeCalls += 1
            if let body = try? JSONValue.decode(bytes: Array(r.body)),
               case .array(let entries)? = body["tree"] {
                lastTreeEntries = entries.map {
                    (path: $0["path"]?.stringValue ?? "", sha: $0["sha"]?.stringValue ?? "")
                }
            } else {
                lastTreeEntries = []
            }
            treeSeq += 1
            return .jsonValue(201, .object(["sha": .string("tree-\(treeSeq)")]))

        case ("POST", let path) where path.hasSuffix("/git/commits"):
            commitCalls += 1
            if let body = try? JSONValue.decode(bytes: Array(r.body)) {
                lastCommitMsg = body["message"]?.stringValue ?? ""
            }
            commitSeq += 1
            return .jsonValue(201, .object(["sha": .string("commit-\(commitSeq)")]))

        case ("PATCH", let path) where path.contains("/git/refs/heads/"):
            refCalls += 1
            let body = try? JSONValue.decode(bytes: Array(r.body))
            if refConflictsLeft > 0 {
                refConflictsLeft -= 1
                // Simulate someone else's concurrent commit moving the branch
                // between this attempt's branchHead read and now.
                headSHA = "concurrent-" + headSHA
                treeSHA = "concurrent-" + treeSHA
                return .json(422, #"{"message":"Update is not a fast forward"}"#)
            }
            if let sha = body?["sha"]?.stringValue { headSHA = sha }
            return .json(200, "{}")

        default:
            unexpected += 1
            return .json(500, "{}")
        }
    }
}

private extension StubResponse {
    /// A JSON response whose body is `value` encoded with sorted keys.
    static func jsonValue(_ status: Int, _ value: JSONValue) -> StubResponse {
        let text = (try? value.encoded()).map { String(decoding: $0, as: UTF8.self) } ?? "{}"
        return .json(status, text)
    }
}

// MARK: - Two-session helpers

/// A lock-protected notes store the pager-side `fs.read` handler serves from,
/// recording every `(vol, path)` it was asked for.
private final class NotesFs: @unchecked Sendable {
    private let lock = NSLock()
    private var files: [String: [UInt8]] = [:]
    private var log: [(vol: String, path: String)] = []

    private func key(_ vol: String, _ path: String) -> String { "\(vol):\(path)" }

    func set(_ vol: String, _ path: String, _ bytes: [UInt8]) {
        lock.lock()
        defer { lock.unlock() }
        files[key(vol, path)] = bytes
    }

    func bytes(_ vol: String, _ path: String) -> [UInt8]? {
        lock.lock()
        defer { lock.unlock() }
        return files[key(vol, path)]
    }

    func record(_ vol: String, _ path: String) {
        lock.lock()
        defer { lock.unlock() }
        log.append((vol, path))
    }

    func asked(_ vol: String, _ path: String) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return log.contains { $0.vol == vol && $0.path == path }
    }
}

/// Collects `evt.notes.sync` params from a session's events, shared between the
/// consuming task and the test under a lock.
private final class NotesCollector: @unchecked Sendable {
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

// MARK: - Tests

/// Ports `server/internal/notessync/notessync_test.go` to `GitHubNotesBackend`
/// and exercises `NotesSyncService` end to end over two real `LinkSession`s,
/// with the pager standing in for the phone's notes store.
final class NotesSyncServiceTests: XCTestCase {

    // MARK: - Helpers

    /// The `Salted__` ciphertext bytes shared between the backend and service
    /// tests: the magic plus a deterministic `(i*37+11) & 0xFF` tail.
    private static func saltedBytes() -> [UInt8] {
        Array("Salted__".utf8) + (0..<64).map { UInt8(($0 * 37 + 11) & 0xFF) }
    }

    /// A backend wired to `mock` through a fresh stub session, plus the server
    /// so tests can inspect the requests it saw.
    private static func makeBackend(_ mock: MockGitHub) -> (GitHubNotesBackend, StubServer) {
        let server = StubServer { mock.respond($0) }
        let backend = GitHubNotesBackend(
            config: { GitHubNotesConfig(repo: "octocat/vault", branch: "main", token: "tok") },
            session: StubURLProtocol.session(server),
            baseBackoff: .milliseconds(1)
        )
        return (backend, server)
    }

    private static func makeSessions() async -> (phone: LinkSession, pager: LinkSession) {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()
        return (phone, pager)
    }

    /// Register the pager-side `fs.read` handler serving `fs`.
    private static func registerFsRead(_ pager: LinkSession, fs: NotesFs) async {
        let salted = Array("Salted__".utf8)
        await pager.handle("fs.read") { params in
            let vol = params["vol"]?.stringValue ?? "int"
            let path = params["path"]?.stringValue ?? ""
            let xfer = UInt16(exactly: params["xfer"]!.intValue!)!
            fs.record(vol, path)
            guard let bytes = fs.bytes(vol, path) else {
                throw RPCError(code: "invalid_argument", msg: "not found")
            }
            Task { () -> Void in
                try? await pager.sendBytes(xfer: xfer, data: bytes, timeout: .seconds(5))
            }
            return [
                "xfer": .number(Double(xfer)),
                "size": .number(Double(bytes.count)),
                "crc32": .number(Double(CRC32.checksum(bytes))),
                "protected": .bool(bytes.starts(with: salted)),
            ]
        }
    }

    /// Collect `evt.notes.sync` params until the terminal `done:true` event or
    /// `timeout`, whichever comes first.
    private static func collectNotes(_ pager: LinkSession, timeout: Duration) async -> [JSONValue] {
        let collector = NotesCollector()
        let consumer = Task {
            for await event in pager.events where event.name == "evt.notes.sync" {
                if collector.record(event.params) { return }
            }
        }
        // Return as soon as the terminal event arrives, or when the timeout
        // elapses; either way the consumer stops well before the test ends.
        let deadline = ContinuousClock.now + timeout
        while !collector.isFinished && ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(2))
        }
        consumer.cancel()
        return collector.values
    }

    // MARK: - Backend guards

    func testValidRepo() {
        let valid = [
            "octocat/Hello-World",
            "my.org/my.repo",
            "a_b/c-d",
            "user123/repo.git",
        ]
        for repo in valid {
            XCTAssertTrue(GitHubNotesBackend.validRepo(repo), "validRepo(\(repo)) = false, want true")
        }

        let invalid = [
            "",
            "noslash",
            "owner/",
            "/repo",
            "owner/re/po",     // extra segment
            "../../x",         // traversal
            "a/..",            // dot-dot segment
            "../b",            // dot-dot segment
            ".",               // no slash + dot
            "owner/repo\u{0}", // NUL
            "own er/repo",     // space
            "owner/re?po",     // query char
        ]
        for repo in invalid {
            XCTAssertFalse(GitHubNotesBackend.validRepo(repo), "validRepo(\(repo)) = true, want false")
        }
    }

    func testSafeNameRejectsTraversal() {
        let ok = ["20250101_120000.txt", "note.md", "a-b_c.txt"]
        for name in ok {
            XCTAssertTrue(GitHubNotesBackend.safeName(name), "safeName(\(name)) rejected, want accepted")
        }

        let bad = [
            "",
            "../secrets.txt",
            "a/b",
            "..",
            ".",
            ".hidden",
            "x\u{0}y",
            "a\\b",
            String(repeating: "a", count: 201),
        ]
        for name in bad {
            XCTAssertFalse(GitHubNotesBackend.safeName(name), "safeName(\(name)) accepted, want rejected")
        }
    }

    func testRefPathEscape() {
        let cases: [(String, String)] = [
            ("main", "main"),
            ("feature/foo", "feature/foo"),
            ("weird branch", "weird%20branch"),
            ("feat/weird branch", "feat/weird%20branch"),
        ]
        for (input, want) in cases {
            XCTAssertEqual(GitHubNotesBackend.refPathEscape(input), want)
        }
    }

    func testCommitMessage() {
        XCTAssertEqual(GitHubNotesBackend.commitMessage(["a.txt"]), "sync: add a.txt")
        let multi = GitHubNotesBackend.commitMessage(["a.txt", "b.txt"])
        XCTAssertTrue(multi.hasPrefix("sync: add 2 notes\n\n"), "commitMessage multi = \(multi)")
        XCTAssertTrue(multi.contains("a.txt"), "commitMessage multi = \(multi)")
        XCTAssertTrue(multi.contains("b.txt"), "commitMessage multi = \(multi)")
    }

    // MARK: - runSync ports

    func testRunSyncCommitsNewFilesInOneCommit() async throws {
        let mock = MockGitHub()
        mock.notesFiles = ["already.txt"]
        let (backend, _) = NotesSyncServiceTests.makeBackend(mock)

        let result = try await backend.push([
            NoteFile(name: "already.txt", bytes: Array("existing".utf8)),
            NoteFile(name: "a.txt", bytes: Array("a".utf8)),
            NoteFile(name: "b.txt", bytes: Array("b".utf8)),
        ])
        XCTAssertEqual(result.already, 1)
        XCTAssertEqual(result.uploaded, 2)
        XCTAssertTrue(result.errors.isEmpty)

        XCTAssertEqual(mock.blobCalls, 2)
        XCTAssertEqual(mock.treeCalls, 1)
        XCTAssertEqual(mock.commitCalls, 1)
        XCTAssertEqual(mock.refCalls, 1)
        XCTAssertEqual(mock.lastTreeEntries.count, 2)
        XCTAssertEqual(Set(mock.lastTreeEntries.map { $0.path }), ["notes/a.txt", "notes/b.txt"])
        XCTAssertTrue(mock.lastCommitMsg.contains("a.txt"), "commit message = \(mock.lastCommitMsg)")
        XCTAssertTrue(mock.lastCommitMsg.contains("b.txt"), "commit message = \(mock.lastCommitMsg)")
        XCTAssertEqual(mock.unexpected, 0)
    }

    func testRunSyncPartialBlobFailureExcludesFileFromCommit() async throws {
        let mock = MockGitHub()
        mock.blobFailContent = ["YmFk"] // base64("bad") fails to blob
        let (backend, _) = NotesSyncServiceTests.makeBackend(mock)

        let result = try await backend.push([
            NoteFile(name: "good.txt", bytes: Array("good".utf8)),
            NoteFile(name: "bad.txt", bytes: Array("bad".utf8)),
        ])
        XCTAssertEqual(result.uploaded, 1)
        XCTAssertEqual(result.errors.count, 1)
        XCTAssertEqual(result.errors.first?.name, "bad.txt")
        XCTAssertEqual(mock.lastTreeEntries.map { $0.path }, ["notes/good.txt"])
    }

    func testRunSyncAllBlobFailuresSkipCommit() async throws {
        let mock = MockGitHub()
        mock.blobFailContent = ["YmFk"]
        let (backend, _) = NotesSyncServiceTests.makeBackend(mock)

        let result = try await backend.push([NoteFile(name: "bad.txt", bytes: Array("bad".utf8))])
        XCTAssertEqual(result.uploaded, 0)
        XCTAssertEqual(result.errors.count, 1)
        XCTAssertEqual(mock.branchHeadCalls, 0)
        XCTAssertEqual(mock.treeCalls, 0)
        XCTAssertEqual(mock.commitCalls, 0)
        XCTAssertEqual(mock.refCalls, 0)
    }

    func testRunSyncNoNewFilesSkipsGitDataAPICalls() async throws {
        let mock = MockGitHub()
        mock.notesFiles = ["a.txt"]
        let (backend, _) = NotesSyncServiceTests.makeBackend(mock)

        let result = try await backend.push([NoteFile(name: "a.txt", bytes: Array("a".utf8))])
        XCTAssertEqual(result.already, 1)
        XCTAssertEqual(result.uploaded, 0)
        XCTAssertTrue(result.errors.isEmpty)

        XCTAssertEqual(mock.branchHeadCalls, 0)
        XCTAssertEqual(mock.blobCalls, 0)
        XCTAssertEqual(mock.treeCalls, 0)
        XCTAssertEqual(mock.commitCalls, 0)
        XCTAssertEqual(mock.refCalls, 0)
    }

    func testRunSyncRefConflictRetriesAgainstFreshHead() async throws {
        let mock = MockGitHub()
        mock.refConflictsLeft = 1
        let (backend, _) = NotesSyncServiceTests.makeBackend(mock)

        let result = try await backend.push([NoteFile(name: "a.txt", bytes: Array("a".utf8))])
        XCTAssertEqual(result.uploaded, 1)
        XCTAssertTrue(result.errors.isEmpty)

        XCTAssertEqual(mock.branchHeadCalls, 2, "refetch after the conflict")
        XCTAssertEqual(mock.refCalls, 2, "conflict then success")
        XCTAssertEqual(mock.blobCalls, 1, "blobs aren't recreated on a ref-update retry")
    }

    func testRunSyncRefConflictExhaustsRetriesReportsErrors() async throws {
        let mock = MockGitHub()
        mock.refConflictsLeft = 999 // always conflicts
        let (backend, _) = NotesSyncServiceTests.makeBackend(mock)

        let result = try await backend.push([NoteFile(name: "a.txt", bytes: Array("a".utf8))])
        XCTAssertEqual(result.uploaded, 0)
        XCTAssertEqual(result.errors.count, 1)
        XCTAssertEqual(result.errors.first?.name, "a.txt")

        XCTAssertEqual(mock.refCalls, 3, "maxRefAttempts")
        XCTAssertEqual(mock.blobCalls, 1, "not recreated across ref-update retries")
    }

    func testListFailureThrows() async throws {
        let server = StubServer { _ in .json(401, #"{"message":"Bad credentials"}"#) }
        let backend = GitHubNotesBackend(
            config: { GitHubNotesConfig(repo: "octocat/vault", branch: "main", token: "tok") },
            session: StubURLProtocol.session(server),
            baseBackoff: .milliseconds(1)
        )

        do {
            _ = try await backend.push([NoteFile(name: "a.txt", bytes: Array("a".utf8))])
            XCTFail("expected error")
        } catch let error as NotesSyncError {
            XCTAssertTrue(error.msg.contains("github 401"), "msg = \(error.msg)")
        } catch {
            XCTFail("unexpected error \(error)")
        }
    }

    // MARK: - Headers, config and raw bytes

    func testRequestsCarryGitHubHeaders() async throws {
        let mock = MockGitHub()
        let (backend, server) = NotesSyncServiceTests.makeBackend(mock)
        _ = try await backend.push([NoteFile(name: "a.txt", bytes: Array("a".utf8))])

        XCTAssertFalse(server.requests.isEmpty)
        for request in server.requests {
            XCTAssertEqual(request.header("Authorization"), "Bearer tok")
            XCTAssertEqual(request.header("Accept"), "application/vnd.github+json")
        }
        let listing = server.requests.first { $0.path.hasSuffix("/contents/notes") }
        XCTAssertNotNil(listing)
        XCTAssertEqual(listing?.query, "ref=main")
    }

    func testConfigValidation() async throws {
        func makeBackend(_ config: GitHubNotesConfig?) -> (GitHubNotesBackend, StubServer) {
            let server = StubServer { _ in .json(500, "{}") }
            let backend = GitHubNotesBackend(
                config: { config },
                session: StubURLProtocol.session(server),
                baseBackoff: .milliseconds(1)
            )
            return (backend, server)
        }

        let cases: [(GitHubNotesConfig?, String)] = [
            (nil, "github not configured"),
            (GitHubNotesConfig(repo: "../x", branch: "main", token: "tok"), "repo must be owner/name"),
            (GitHubNotesConfig(repo: "octocat/vault", branch: "main", token: ""), "token required"),
        ]
        for (config, message) in cases {
            let (backend, server) = makeBackend(config)
            do {
                _ = try await backend.push([NoteFile(name: "a.txt", bytes: Array("a".utf8))])
                XCTFail("expected \(message)")
            } catch let error as NotesSyncError {
                XCTAssertEqual(error, NotesSyncError(message))
            } catch {
                XCTFail("unexpected error \(error)")
            }
            XCTAssertTrue(server.requests.isEmpty, "no request may be sent for an invalid config")
        }
    }

    func testSaltedNoteBlobIsRawBytes() async throws {
        let mock = MockGitHub()
        let (backend, server) = NotesSyncServiceTests.makeBackend(mock)
        let raw = NotesSyncServiceTests.saltedBytes()

        let result = try await backend.push([NoteFile(name: "secret.txt", bytes: raw)])
        XCTAssertEqual(result.uploaded, 1)
        XCTAssertTrue(result.errors.isEmpty)
        XCTAssertEqual(mock.blobCalls, 1)

        let blobRequests = server.requests(path: "/git/blobs")
        XCTAssertEqual(blobRequests.count, 1)
        guard let blobRequest = blobRequests.first else {
            XCTFail("no blob request seen")
            return
        }
        let body = try JSONValue.decode(bytes: Array(blobRequest.body))
        XCTAssertEqual(body["encoding"]?.stringValue, "base64")
        let content = body["content"]?.stringValue ?? ""
        guard let decoded = Data(base64Encoded: content) else {
            XCTFail("blob content is not base64")
            return
        }
        XCTAssertEqual(Array(decoded), raw)
    }

    // MARK: - NotesSyncService over two sessions

    func testPhoneTargetWritesFilesAndEmitsProgress() async throws {
        let (phone, pager) = await NotesSyncServiceTests.makeSessions()
        let fs = NotesFs()
        let aBytes = Array("alpha".utf8)
        let bBytes = Array("bravo".utf8)
        fs.set("int", "/notes/a.txt", aBytes)
        fs.set("sd", "/notes/b.txt", bBytes)
        await NotesSyncServiceTests.registerFsRead(pager, fs: fs)

        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let notes = NotesSyncService(session: phone, github: nil, phoneDirectory: dir)
        await notes.register()

        async let events = NotesSyncServiceTests.collectNotes(pager, timeout: .seconds(5))
        let result = try await pager.call("notes.sync", params: [
            "targets": ["phone"],
            "files": [
                ["name": "a.txt", "size": .number(Double(aBytes.count)), "mtime": 0,
                 "crc32": .number(Double(CRC32.checksum(aBytes)))],
                ["name": "b.txt", "size": .number(Double(bBytes.count)), "mtime": 0,
                 "crc32": .number(Double(CRC32.checksum(bBytes))), "vol": "sd"],
            ],
        ], timeout: .seconds(5))
        let collected = await events

        XCTAssertEqual(result["job"]?.intValue, 1)
        XCTAssertEqual(collected.count, 3)
        for event in collected {
            XCTAssertEqual(event["phase"]?.stringValue, "upload")
            XCTAssertEqual(event["job"]?.intValue, 1)
        }
        XCTAssertEqual(collected[0]["done"]?.boolValue, false)
        XCTAssertEqual(collected[1]["done"]?.boolValue, false)
        let last = collected[2]
        XCTAssertEqual(last["done"]?.boolValue, true)
        XCTAssertEqual(last["uploaded"]?.intValue, 2)
        XCTAssertEqual(last["already"]?.intValue, 0)
        XCTAssertEqual(last["errors"], JSONValue.array([]))

        let writtenA = try Data(contentsOf: dir.appendingPathComponent("a.txt"))
        let writtenB = try Data(contentsOf: dir.appendingPathComponent("b.txt"))
        XCTAssertEqual(Array(writtenA), aBytes)
        XCTAssertEqual(Array(writtenB), bBytes)

        XCTAssertTrue(fs.asked("int", "/notes/a.txt"))
        XCTAssertTrue(fs.asked("sd", "/notes/b.txt"))

        await phone.stop()
        await pager.stop()
    }

    func testManifestCRCMismatchIsError() async throws {
        let (phone, pager) = await NotesSyncServiceTests.makeSessions()
        let fs = NotesFs()
        let bytes = Array("alpha".utf8)
        fs.set("int", "/notes/a.txt", bytes)
        await NotesSyncServiceTests.registerFsRead(pager, fs: fs)

        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let notes = NotesSyncService(session: phone, github: nil, phoneDirectory: dir)
        await notes.register()

        async let events = NotesSyncServiceTests.collectNotes(pager, timeout: .seconds(5))
        _ = try await pager.call("notes.sync", params: [
            "targets": ["phone"],
            "files": [
                ["name": "a.txt", "size": .number(Double(bytes.count)), "mtime": 0,
                 "crc32": .number(Double(CRC32.checksum(bytes) ^ 1))],
            ],
        ], timeout: .seconds(5))
        let collected = await events

        let last = collected.last
        XCTAssertEqual(last?["uploaded"]?.intValue, 0)
        XCTAssertEqual(last?["errors"], JSONValue.array([.string("a.txt")]))
        XCTAssertFalse(FileManager.default.fileExists(atPath: dir.appendingPathComponent("a.txt").path))

        await phone.stop()
        await pager.stop()
    }

    func testManifestSizeMismatchIsError() async throws {
        let (phone, pager) = await NotesSyncServiceTests.makeSessions()
        let fs = NotesFs()
        let bytes = Array("alpha".utf8)
        fs.set("int", "/notes/a.txt", bytes)
        await NotesSyncServiceTests.registerFsRead(pager, fs: fs)

        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let notes = NotesSyncService(session: phone, github: nil, phoneDirectory: dir)
        await notes.register()

        async let events = NotesSyncServiceTests.collectNotes(pager, timeout: .seconds(5))
        _ = try await pager.call("notes.sync", params: [
            "targets": ["phone"],
            "files": [
                ["name": "a.txt", "size": .number(Double(bytes.count + 1)), "mtime": 0,
                 "crc32": .number(Double(CRC32.checksum(bytes)))],
            ],
        ], timeout: .seconds(5))
        let collected = await events

        let last = collected.last
        XCTAssertEqual(last?["uploaded"]?.intValue, 0)
        XCTAssertEqual(last?["errors"], JSONValue.array([.string("a.txt")]))

        await phone.stop()
        await pager.stop()
    }

    func testGitHubTargetSkipsExistingAndCommitsRest() async throws {
        let (phone, pager) = await NotesSyncServiceTests.makeSessions()
        let fs = NotesFs()
        let existing = Array("existing".utf8)
        let hello = Array("hello".utf8)
        let raw = NotesSyncServiceTests.saltedBytes()
        fs.set("int", "/notes/already.txt", existing)
        fs.set("int", "/notes/new.txt", hello)
        fs.set("int", "/notes/salted.txt", raw)
        await NotesSyncServiceTests.registerFsRead(pager, fs: fs)

        let mock = MockGitHub()
        mock.notesFiles = ["already.txt"]
        let server = StubServer { mock.respond($0) }
        let backend = GitHubNotesBackend(
            config: { GitHubNotesConfig(repo: "octocat/vault", branch: "main", token: "tok") },
            session: StubURLProtocol.session(server),
            baseBackoff: .milliseconds(1)
        )

        let notes = NotesSyncService(session: phone, github: backend, phoneDirectory: nil)
        await notes.register()

        async let events = NotesSyncServiceTests.collectNotes(pager, timeout: .seconds(5))
        _ = try await pager.call("notes.sync", params: [
            "targets": ["github"],
            "files": [
                ["name": "already.txt", "size": .number(Double(existing.count)), "mtime": 0,
                 "crc32": .number(Double(CRC32.checksum(existing)))],
                ["name": "new.txt", "size": .number(Double(hello.count)), "mtime": 0,
                 "crc32": .number(Double(CRC32.checksum(hello)))],
                ["name": "salted.txt", "size": .number(Double(raw.count)), "mtime": 0,
                 "crc32": .number(Double(CRC32.checksum(raw)))],
            ],
        ], timeout: .seconds(5))
        let collected = await events

        let last = collected.last
        XCTAssertEqual(last?["uploaded"]?.intValue, 2)
        XCTAssertEqual(last?["already"]?.intValue, 1)
        XCTAssertEqual(last?["errors"], JSONValue.array([]))

        XCTAssertFalse(fs.asked("int", "/notes/already.txt"), "an existing name must not be pulled")
        XCTAssertEqual(mock.blobCalls, 2)
        let blobBytes = mock.blobContents.compactMap { Data(base64Encoded: $0) }.map { Array($0) }
        XCTAssertEqual(Set(blobBytes), Set([hello, raw]))
        XCTAssertEqual(Set(mock.lastTreeEntries.map { $0.path }), Set(["notes/new.txt", "notes/salted.txt"]))
        XCTAssertEqual(mock.commitCalls, 1)
        XCTAssertEqual(mock.refCalls, 1)

        await phone.stop()
        await pager.stop()
    }

    func testGitHubNotConfiguredErrorsEveryFile() async throws {
        let (phone, pager) = await NotesSyncServiceTests.makeSessions()
        let fs = NotesFs()
        fs.set("int", "/notes/a.txt", Array("a".utf8))
        fs.set("int", "/notes/b.txt", Array("b".utf8))
        await NotesSyncServiceTests.registerFsRead(pager, fs: fs)

        let server = StubServer { _ in .json(500, "{}") }
        let backend = GitHubNotesBackend(
            config: { nil },
            session: StubURLProtocol.session(server)
        )
        let notes = NotesSyncService(session: phone, github: backend, phoneDirectory: nil)
        await notes.register()

        async let events = NotesSyncServiceTests.collectNotes(pager, timeout: .seconds(5))
        _ = try await pager.call("notes.sync", params: [
            "targets": ["github"],
            "files": [
                ["name": "a.txt", "size": .number(4), "mtime": 0, "crc32": .number(4)],
                ["name": "b.txt", "size": .number(4), "mtime": 0, "crc32": .number(4)],
            ],
        ], timeout: .seconds(5))
        let collected = await events

        let last = collected.last
        XCTAssertEqual(last?["uploaded"]?.intValue, 0)
        XCTAssertEqual(last?["errors"], JSONValue.array([.string("a.txt"), .string("b.txt")]))
        XCTAssertFalse(fs.asked("int", "/notes/a.txt"))

        await phone.stop()
        await pager.stop()
    }

    func testBadParams() async throws {
        let (phone, pager) = await NotesSyncServiceTests.makeSessions()
        let notes = NotesSyncService(session: phone, github: nil, phoneDirectory: nil)
        await notes.register()

        let cases: [[String: JSONValue]] = [
            ["targets": ["dropbox"], "files": []],
            ["targets": ["phone"]],
            ["targets": ["phone"], "files": [["name": "a.txt", "size": "1", "crc32": 0]]],
        ]
        for params in cases {
            do {
                _ = try await pager.call("notes.sync", params: params, timeout: .seconds(5))
                XCTFail("expected invalid_argument for \(params)")
            } catch let error as RPCError {
                XCTAssertEqual(error.code, "invalid_argument")
            }
        }

        await phone.stop()
        await pager.stop()
    }
}
