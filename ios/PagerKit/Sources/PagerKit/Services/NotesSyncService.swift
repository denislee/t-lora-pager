import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

/// One note's raw on-disk bytes (an encrypted note is its `Salted__`
/// ciphertext, passed through untouched).
public struct NoteFile: Sendable, Equatable {
    public let name: String
    public let bytes: [UInt8]

    public init(name: String, bytes: [UInt8]) {
        self.name = name
        self.bytes = bytes
    }
}

/// A file the remote refused, with the reason.
public struct NotesPushError: Sendable, Equatable {
    public let name: String
    public let error: String

    public init(name: String, error: String) {
        self.name = name
        self.error = error
    }
}

/// lilyhub `SyncResponse`: what landed, what was already there, what failed.
public struct NotesPushResult: Sendable, Equatable {
    public var uploaded = 0
    public var already = 0
    public var errors: [NotesPushError] = []
    /// The commit that landed, nil when nothing was committed.
    public var commitSHA: String?

    public init(uploaded: Int = 0, already: Int = 0, errors: [NotesPushError] = [],
                commitSHA: String? = nil) {
        self.uploaded = uploaded
        self.already = already
        self.errors = errors
        self.commitSHA = commitSHA
    }
}

/// A failure that stops a whole push (config, listing).
public struct NotesSyncError: Error, Equatable, Sendable {
    public let msg: String

    public init(_ msg: String) { self.msg = msg }
}

/// An additive-only notes remote: list what is there (with blob SHAs), add
/// what is not, read a commit back.
public protocol NotesRemote: Sendable {
    /// Names currently under `notes/`.
    func existingNames() async throws -> Set<String>
    /// name → git blob SHA of every blob under `notes/` on the branch (404 → empty); a truncated listing throws.
    func existingBlobs() async throws -> [String: String]
    /// The same listing for the tree of one commit (pinned, not the moving branch head) — what the move engine verifies against.
    func blobs(atCommit sha: String) async throws -> [String: String]
    /// Add `files` (none of which exist remotely); `already` stays 0.
    func commit(_ files: [NoteFile]) async -> NotesPushResult
    /// Identifies the remote `existingNames` lists (repo and branch), so a
    /// cached listing is dropped when it changes. nil: don't cache.
    var cacheKey: String? { get }
}

extension NotesRemote {
    public var cacheKey: String? { nil }
}

/// The user's notes repo on GitHub. Repo, branch and token come from `config`
/// on every call (the app keeps them in the Keychain); the token is never
/// logged or stored here.
public struct GitHubNotesConfig: Sendable, Equatable {
    public var repo: String
    public var branch: String
    public var token: String

    public init(repo: String, branch: String = "main", token: String) {
        self.repo = repo
        self.branch = branch
        self.token = token
    }
}

/// Port of lilyhub `server/internal/notessync`: additive-only push through
/// the Git Data API — list `notes/`, create one blob per new file (bounded
/// parallelism), then one tree + commit + ref update on top of the branch head,
/// retried against a fresh head on a 422 non-fast-forward. Names already
/// present are skipped, never overwritten; nothing is ever deleted. Bytes go
/// up base64-encoded verbatim, so a `Salted__` note reaches GitHub byte for
/// byte.
public struct GitHubNotesBackend: NotesRemote {
    public static let defaultAPIBase = URL(string: "https://api.github.com")!
    /// Ref-update attempts on a non-fast-forward (lilyhub `maxRefAttempts`).
    public static let maxRefAttempts = 3

    private let config: @Sendable () -> GitHubNotesConfig?
    private let session: URLSession
    private let apiBase: String
    private let maxParallel: Int
    private let baseBackoff: Duration

    public init(
        config: @escaping @Sendable () -> GitHubNotesConfig?,
        session: URLSession = .shared,
        apiBase: URL = GitHubNotesBackend.defaultAPIBase,
        maxParallel: Int = 4,
        baseBackoff: Duration = .milliseconds(200)
    ) {
        self.config = config
        self.session = session
        var base = apiBase.absoluteString
        while base.hasSuffix("/") { base.removeLast() }
        self.apiBase = base
        self.maxParallel = max(1, maxParallel)
        self.baseBackoff = baseBackoff
    }

    public var cacheKey: String? {
        guard let cfg = config() else { return nil }
        return "\(cfg.repo)@\(cfg.branch)"
    }

    /// lilyhub `runSync`: list, skip names already there (and unsafe names),
    /// commit the rest in one commit.
    public func push(_ files: [NoteFile]) async throws -> NotesPushResult {
        let remote = try await existingNames()
        var out = NotesPushResult()
        var toUpload: [NoteFile] = []
        for file in files where !file.name.isEmpty {
            guard Self.safeName(file.name) else {
                out.errors.append(NotesPushError(name: file.name, error: "invalid name"))
                continue
            }
            if remote.contains(file.name) {
                out.already += 1
                continue
            }
            toUpload.append(file)
        }
        if toUpload.isEmpty { return out }
        let committed = await commit(toUpload)
        out.uploaded += committed.uploaded
        out.errors.append(contentsOf: committed.errors)
        return out
    }

    /// lilyhub `listRemote`: names under `notes/` on the branch; a 404 means
    /// the folder (or branch) does not exist yet. Uses the Git Trees API on
    /// `<branch>:notes` (P7.58): the Contents API stops at 1,000 entries, and
    /// the notes past it would look missing and be pulled and pushed again. A
    /// `truncated` tree is an error for the same reason.
    public func existingNames() async throws -> Set<String> {
        Set(try await existingBlobs().keys)
    }

    /// name → git blob SHA of every blob under `notes/` on the branch; a 404
    /// means the folder (or branch) does not exist yet.
    public func existingBlobs() async throws -> [String: String] {
        let cfg = try validConfig()
        return try await listNotes(ref: cfg.branch)
    }

    /// The same listing for the tree of one commit, not the moving branch
    /// head: a commit SHA pins what a landed commit actually contains.
    public func blobs(atCommit sha: String) async throws -> [String: String] {
        _ = try validConfig()
        guard !sha.isEmpty else { throw NotesSyncError("bad commit sha") }
        return try await listNotes(ref: sha)
    }

    /// The `notes/` blobs of `ref` (a branch or a commit), by name → blob SHA.
    private func listNotes(ref: String) async throws -> [String: String] {
        let cfg = try validConfig()
        let url = "\(apiBase)/repos/\(cfg.repo)/git/trees/\(Self.refPathEscape(ref)):notes"
        let reply = try await exchange("GET", url, cfg, body: nil, attempts: 2)
        if reply.status == 404 { return [:] }
        guard reply.status == 200 else { throw NotesSyncError(Self.githubError(reply)) }
        let listing = try JSONValue.decode(bytes: Array(reply.body))
        guard case .array(let entries)? = listing["tree"] else {
            throw NotesSyncError("parse: listing has no tree")
        }
        if listing["truncated"]?.boolValue == true {
            throw NotesSyncError("notes listing truncated")
        }
        var blobs = [String: String]()
        for entry in entries {
            guard entry["type"]?.stringValue == "blob", let name = entry["path"]?.stringValue,
                  let sha = entry["sha"]?.stringValue else { continue }
            blobs[name] = sha
        }
        return blobs
    }

    /// lilyhub `commitFiles`: blobs (parallel), then tree + commit + ref,
    /// retrying the last three on a 422. Failed blobs are reported and left
    /// out; if the commit itself fails every blobbed file is reported failed.
    /// On success the result carries the new commit SHA.
    public func commit(_ files: [NoteFile]) async -> NotesPushResult {
        var out = NotesPushResult()
        let cfg: GitHubNotesConfig
        do {
            cfg = try validConfig()
        } catch {
            out.errors = files.map { NotesPushError(name: $0.name, error: Self.describe(error)) }
            return out
        }

        // Blob creates, at most maxParallel in flight; results kept in input order.
        var shas = [String?](repeating: nil, count: files.count)
        var blobErrors = [String?](repeating: nil, count: files.count)
        await withTaskGroup(of: (Int, Result<String, Error>).self) { group in
            var next = 0
            func addNext() {
                guard next < files.count else { return }
                let index = next
                let file = files[index]
                next += 1
                group.addTask {
                    let payload: JSONValue = [
                        "content": .string(Data(file.bytes).base64EncodedString()),
                        "encoding": "base64",
                    ]
                    do {
                        return (index, .success(try await self.postForSHA(cfg, "blobs", "blob", payload)))
                    } catch {
                        return (index, .failure(error))
                    }
                }
            }
            for _ in 0..<min(maxParallel, files.count) { addNext() }
            for await (index, result) in group {
                switch result {
                case .success(let sha): shas[index] = sha
                case .failure(let error): blobErrors[index] = Self.describe(error)
                }
                addNext()
            }
        }

        var blobs: [(file: NoteFile, sha: String)] = []
        for (index, file) in files.enumerated() {
            if let sha = shas[index] {
                blobs.append((file, sha))
            } else {
                out.errors.append(NotesPushError(name: file.name, error: blobErrors[index] ?? "blob failed"))
            }
        }
        if blobs.isEmpty { return out }

        let message = Self.commitMessage(blobs.map(\.file.name))
        var lastError = "commit failed"
        for _ in 1...Self.maxRefAttempts {
            do {
                let (baseCommit, baseTree) = try await branchHead(cfg)
                let entries: [JSONValue] = blobs.map {
                    ["path": .string("notes/" + $0.file.name), "mode": "100644", "type": "blob", "sha": .string($0.sha)]
                }
                let tree = try await postForSHA(cfg, "trees", "tree",
                                                ["base_tree": .string(baseTree), "tree": .array(entries)])
                let commit = try await postForSHA(cfg, "commits", "commit", [
                    "message": .string(message), "tree": .string(tree), "parents": [.string(baseCommit)],
                ])
                let (conflict, error) = await updateRef(cfg, commit)
                guard let error else {
                    out.uploaded += blobs.count
                    out.commitSHA = commit
                    return out
                }
                lastError = error
                if !conflict { break }
                // The branch moved under us: refetch the head and retry with
                // the same blobs.
            } catch {
                lastError = Self.describe(error)
                break
            }
        }
        for blob in blobs {
            out.errors.append(NotesPushError(name: blob.file.name, error: lastError))
        }
        return out
    }

    // MARK: - GitHub calls

    private func validConfig() throws -> GitHubNotesConfig {
        guard var cfg = config() else { throw NotesSyncError("github not configured") }
        guard Self.validRepo(cfg.repo) else { throw NotesSyncError("repo must be owner/name") }
        if cfg.branch.isEmpty { cfg.branch = "main" }
        guard !cfg.token.isEmpty else { throw NotesSyncError("token required") }
        return cfg
    }

    private func exchange(_ method: String, _ url: String, _ cfg: GitHubNotesConfig,
                          body: JSONValue?, attempts: Int) async throws -> HTTPReply {
        guard let target = URL(string: url) else { throw NotesSyncError("bad url") }
        var request = try HTTP.request(url: target, method: method, timeout: 30, bearer: cfg.token, json: body)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        request.setValue("PagerKit/1.0", forHTTPHeaderField: "User-Agent")
        return try await HTTP.sendWithRetry(request, using: session, attempts: attempts, baseBackoff: baseBackoff)
    }

    /// lilyhub `branchHead`: the branch's commit SHA and its tree SHA.
    private func branchHead(_ cfg: GitHubNotesConfig) async throws -> (String, String) {
        let reply = try await exchange("GET", "\(apiBase)/repos/\(cfg.repo)/branches/\(Self.refPathEscape(cfg.branch))",
                                       cfg, body: nil, attempts: 3)
        guard reply.status == 200 else { throw NotesSyncError(Self.githubError(reply)) }
        let value = try JSONValue.decode(bytes: Array(reply.body))
        guard let commit = value["commit"]?["sha"]?.stringValue, !commit.isEmpty,
              let tree = value["commit"]?["commit"]?["tree"]?["sha"]?.stringValue, !tree.isEmpty else {
            throw NotesSyncError("branch head response missing commit/tree sha")
        }
        return (commit, tree)
    }

    /// lilyhub `postForSHA`: POST to `/git/<path>` and return the new object's sha.
    private func postForSHA(_ cfg: GitHubNotesConfig, _ path: String, _ what: String,
                            _ payload: JSONValue) async throws -> String {
        let reply = try await exchange("POST", "\(apiBase)/repos/\(cfg.repo)/git/\(path)", cfg,
                                       body: payload, attempts: 3)
        guard reply.status / 100 == 2 else { throw NotesSyncError(Self.githubError(reply)) }
        guard let value = try? JSONValue.decode(bytes: Array(reply.body)) else {
            throw NotesSyncError("parse \(what) response")
        }
        guard let sha = value["sha"]?.stringValue, !sha.isEmpty else {
            throw NotesSyncError("\(what) response missing sha")
        }
        return sha
    }

    /// lilyhub `updateRef`: fast-forward the branch; a 422 is a conflict.
    private func updateRef(_ cfg: GitHubNotesConfig, _ commit: String) async -> (conflict: Bool, error: String?) {
        do {
            let reply = try await exchange("PATCH",
                                           "\(apiBase)/repos/\(cfg.repo)/git/refs/heads/\(Self.refPathEscape(cfg.branch))",
                                           cfg, body: ["sha": .string(commit), "force": false], attempts: 3)
            guard reply.status / 100 == 2 else { return (reply.status == 422, Self.githubError(reply)) }
            return (false, nil)
        } catch {
            return (false, Self.describe(error))
        }
    }

    // MARK: - Helpers (lilyhub names)

    static func githubError(_ reply: HTTPReply) -> String {
        "github \(reply.status): \(HTTP.truncate(HTTP.text(reply.body), 200))"
    }

    static func describe(_ error: Error) -> String {
        if let error = error as? NotesSyncError { return error.msg }
        if let error = error as? HTTPStatusError { return "github \(error.status): \(error.body)" }
        return "\(error)"
    }

    /// Single file: `sync: add <name>`; a batch: a count subject plus the list.
    static func commitMessage(_ names: [String]) -> String {
        if names.count == 1 { return "sync: add " + names[0] }
        return "sync: add \(names.count) notes\n\n" + names.joined(separator: "\n")
    }

    /// Exactly `owner/name`, GitHub-legal characters, no `.` / `..` segment.
    static func validRepo(_ repo: String) -> Bool {
        let parts = repo.split(separator: "/", omittingEmptySubsequences: false)
        guard parts.count == 2 else { return false }
        return parts.allSatisfy { validRepoSegment(String($0)) }
    }

    private static func validRepoSegment(_ s: String) -> Bool {
        guard !s.isEmpty, s != ".", s != ".." else { return false }
        return s.unicodeScalars.allSatisfy { c in
            (c >= "A" && c <= "Z") || (c >= "a" && c <= "z") || (c >= "0" && c <= "9")
                || c == "." || c == "-" || c == "_"
        }
    }

    /// Non-empty, ≤ 200 bytes, no slash / backslash / NUL, no leading dot.
    public static func safeName(_ name: String) -> Bool {
        !name.isEmpty && name.utf8.count <= 200 && !name.hasPrefix(".")
            && !name.contains("/") && !name.contains("\\") && !name.contains("\u{0}")
    }

    /// Escape each `/`-separated segment as Go's `url.PathEscape` does,
    /// keeping the slashes.
    static func refPathEscape(_ ref: String) -> String {
        var allowed = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")
        allowed.insert(charactersIn: "-_.~$&+:=@")
        return ref.split(separator: "/", omittingEmptySubsequences: false)
            .map { $0.addingPercentEncoding(withAllowedCharacters: allowed) ?? String($0) }
            .joined(separator: "/")
    }
}

/// Answers the pager's `notes.sync` (PROTOCOL.md §6) on one `LinkSession`, and
/// runs the app's "Sync to GitHub" (`move`), both through one engine.
///
/// Target `"github"` **moves** the notes (IOS_NOTES_VIEWER_PLAN.md §3.6): list
/// the repo's `notes/` with blob SHAs, pull every note with `fs.read` (raw
/// on-disk bytes, so an encrypted note is its `Salted__` ciphertext, never
/// decrypted) and hash it as a git blob, upload what the repo lacks in one
/// commit (a name taken by other bytes goes up as `<stem>~<sha7>.txt`; nothing
/// on GitHub is overwritten), read the tree of that commit back, and delete
/// from the pager — with `fs.delete`'s size + CRC precondition — exactly the
/// notes whose blob SHA it shows. Anything short of that keeps the note on the
/// pager (`deletable`). Target `"phone"` stores the pulled bytes in
/// `phoneDirectory` and never deletes.
///
/// The pager's request is answered at once with `{job}`; progress goes out as
/// `evt.notes.sync` (cumulative counts, `deleted`/`kept` included) and once
/// more with `done:true`. Runs are serialised: a second trigger waits for the
/// first to finish.
public actor NotesSyncService {
    /// Idle bound per file pull.
    public static let defaultReadTimeout: Duration = .seconds(60)
    /// The capability a pager advertises when `fs.delete` honours size/crc32.
    public static let conditionalDeleteCap = "fs.delete.if"

    private struct Entry {
        let name: String
        let size: UInt32
        let crc32: UInt32
        let vol: String
    }

    /// One note to move; `expect` is the pager manifest's size + CRC.
    private struct MoveInput {
        let vol: String
        let name: String
        let expect: (size: UInt32, crc32: UInt32)?
    }

    private let session: LinkSession
    private let github: (any NotesRemote)?
    private let phoneDirectory: URL?
    private let readTimeout: Duration
    private let canDelete: Bool
    private let files: FileService
    private var job = 0
    private var running: [Int: Task<Void, Never>] = [:]
    /// The last queued run; the next one starts after it (serialised triggers).
    private var tail: Task<Void, Never>?
    /// Told after every pager-initiated move (session log, viewer refresh).
    private var reporter: (@Sendable (NotesMoveResult) async -> Void)?

    /// `pagerCaps` are the pager's `sys.hello` caps: without
    /// `"fs.delete.if"` a move uploads and verifies but deletes nothing.
    public init(session: LinkSession, github: (any NotesRemote)?, phoneDirectory: URL?,
                pagerCaps: [String] = [],
                readTimeout: Duration = NotesSyncService.defaultReadTimeout) {
        self.session = session
        self.github = github
        self.phoneDirectory = phoneDirectory
        self.readTimeout = readTimeout
        self.canDelete = pagerCaps.contains(Self.conditionalDeleteCap)
        self.files = FileService(session: session, timeout: .seconds(15))
    }

    /// Install the `notes.sync` handler on the session.
    public func register() async {
        await session.handle("notes.sync") { [weak self] params in
            guard let self else { throw RPCError(code: "unsupported", msg: "notes service gone") }
            return try await self.start(params)
        }
    }

    /// Called after each pager-initiated `"github"` job with its result:
    /// counts and reasons, never note contents or the token.
    public func onPagerMove(_ reporter: @escaping @Sendable (NotesMoveResult) async -> Void) {
        self.reporter = reporter
    }

    /// Wait for a job's background work (tests).
    public func wait(job: Int) async {
        await running[job]?.value
    }

    /// The app's "Sync to GitHub": move `notes` (every note on both volumes,
    /// the unmerged `NotesIndexer.Scan.all`). Waits for any run in progress.
    /// Cancelling before the commit changes nothing; after it, the commit
    /// stands and the remaining notes stay on the pager for the next run.
    public func move(_ notes: [NoteEntry],
                     progress: @escaping @Sendable (NotesMoveProgress) async -> Void = { _ in })
        async -> NotesMoveResult {
        let inputs = notes.map { MoveInput(vol: $0.vol, name: $0.name, expect: nil) }
        return await serialized { service in await service.runMove(inputs, progress: progress) }
    }

    /// Run `body` after every run queued before it.
    private func serialized<T: Sendable>(_ body: @escaping @Sendable (NotesSyncService) async -> T) async -> T {
        let previous = tail
        let work = Task { () -> T in
            await previous?.value
            return await body(self)
        }
        tail = Task { _ = await work.value }
        return await withTaskCancellationHandler {
            await work.value
        } onCancel: {
            work.cancel()
        }
    }

    func start(_ params: JSONValue) throws -> JSONValue {
        let bad = RPCError(code: "invalid_argument", msg: "bad notes.sync params")
        guard case .array(let rawTargets)? = params["targets"], !rawTargets.isEmpty else { throw bad }
        var targets = Set<String>()
        for target in rawTargets {
            guard let name = target.stringValue, name == "github" || name == "phone" else { throw bad }
            targets.insert(name)
        }
        guard case .array(let rawFiles)? = params["files"] else { throw bad }
        var entries: [Entry] = []
        for file in rawFiles {
            guard let name = file["name"]?.stringValue,
                  let size = Params.uint32(file["size"]),
                  let crc = Params.uint32(file["crc32"]),
                  file["mtime"] == nil || Params.int64(file["mtime"]) != nil,
                  file["vol"] == nil || file["vol"]?.stringValue != nil else { throw bad }
            let vol = file["vol"]?.stringValue ?? ""
            entries.append(Entry(name: name, size: size, crc32: crc, vol: vol.isEmpty ? "int" : vol))
        }

        job += 1
        let id = job
        let wanted = targets
        let manifest = entries
        running[id] = Task { [weak self] in
            guard let self else { return }
            await self.serialized { service in
                await service.run(job: id, targets: wanted, files: manifest)
            }
            await self.finished(id)
        }
        return ["job": .number(Double(id))]
    }

    private func finished(_ job: Int) {
        running[job] = nil
    }

    private func run(job: Int, targets: Set<String>, files: [Entry]) async {
        var phoneErrors: [String] = []
        var phoneStored = 0
        var phoneAlready = 0
        if targets.contains("phone") {
            for file in files {
                switch await deliverToPhone(file) {
                case .error: phoneErrors.append(file.name)
                case .already: phoneAlready += 1
                case .stored: phoneStored += 1
                }
                if !targets.contains("github") {
                    await emit(job, phase: "upload", uploaded: phoneStored, already: phoneAlready,
                               errors: phoneErrors, done: false)
                }
            }
        }
        guard targets.contains("github") else {
            await emit(job, phase: "upload", uploaded: phoneStored, already: phoneAlready,
                       errors: phoneErrors, done: true)
            return
        }

        let inputs = files.map { MoveInput(vol: $0.vol, name: $0.name, expect: ($0.size, $0.crc32)) }
        let pullErrors = phoneErrors
        // The pager fails a job after 120 s without an event, so every phase
        // and every file reports.
        let result = await runMove(inputs) { [weak self] step in
            guard let self else { return }
            let phase: String
            switch step {
            case .listing, .pulling: phase = "pull"
            case .committing: phase = "commit"
            case .verifying: phase = "verify"
            case .deleting, .deleted: phase = "delete"
            }
            await self.emit(job, phase: phase, uploaded: 0, already: 0, errors: pullErrors, done: false)
        }
        await emit(job, phase: "done", uploaded: result.uploaded, already: result.already,
                   deleted: result.deleted, kept: result.kept.count,
                   errors: phoneErrors + result.errors.map(\.name), done: true)
        await reporter?(result)
    }

    // MARK: - The move engine (§3.6)

    private func runMove(_ inputs: [MoveInput],
                         progress: @Sendable (NotesMoveProgress) async -> Void) async -> NotesMoveResult {
        var out = NotesMoveResult()
        out.canDelete = canDelete
        func fail(_ input: MoveInput, _ reason: String) {
            out.errors.append(NotesMoveIssue(vol: input.vol, name: input.name, reason: reason))
        }
        guard let github else {
            inputs.forEach { fail($0, "github not configured") }
            return out
        }
        if inputs.isEmpty { return out }

        // 1. Remote listing, always fresh. A truncated or failed listing
        // aborts before anything is pulled.
        await progress(.listing)
        let remote: [String: String]
        do {
            remote = try await github.existingBlobs()
        } catch {
            let reason = GitHubNotesBackend.describe(error)
            inputs.forEach { fail($0, reason) }
            return out
        }

        // 2 + 3. Pull and hash every note, then classify it against the repo
        // and the uploads planned so far. From here a note's identity is the
        // bytes pulled.
        var pulled: [PulledNote] = []
        var uploads: [NoteFile] = []
        var planned = remote
        for (index, input) in inputs.enumerated() {
            if Task.isCancelled {
                inputs[index...].forEach { fail($0, Self.keepCancelled) }
                return out
            }
            await progress(.pulling(done: index, total: inputs.count))
            guard GitHubNotesBackend.safeName(input.name) else {
                fail(input, "invalid name")
                continue
            }
            let bytes: [UInt8]
            do {
                (bytes, _) = try await session.readFile(vol: input.vol, path: "/notes/" + input.name,
                                                        timeout: readTimeout)
            } catch {
                fail(input, "read: \(GitHubNotesBackend.describe(error))")
                continue
            }
            let crc = CRC32.checksum(bytes)
            if let expect = input.expect,
               UInt32(exactly: bytes.count) != expect.size || crc != expect.crc32 {
                // The manifest describes the raw on-disk bytes; the note
                // changed after the pager built it.
                fail(input, "changed since the manifest")
                continue
            }
            guard let size = UInt32(exactly: bytes.count) else {
                fail(input, "too large")
                continue
            }
            let sha = GitBlob.sha1(bytes)
            var remoteName = input.name
            var upload = false
            if let existing = planned[remoteName], existing != sha {
                remoteName = Self.conflictName(input.name, sha: sha)
                guard GitHubNotesBackend.safeName(remoteName) else {
                    fail(input, "invalid name")
                    continue
                }
            }
            switch planned[remoteName] {
            case sha?: break // already there (or already planned) byte for byte
            case nil: upload = true
            case _?:
                fail(input, "name clash at \(remoteName)")
                continue
            }
            if upload {
                planned[remoteName] = sha
                uploads.append(NoteFile(name: remoteName, bytes: bytes))
            }
            pulled.append(PulledNote(vol: input.vol, name: input.name, size: size, crc32: crc,
                                     sha: sha, remoteName: remoteName, upload: upload))
        }
        await progress(.pulling(done: inputs.count, total: inputs.count))
        if Task.isCancelled {
            // Nothing committed, nothing deleted.
            for note in pulled {
                out.kept.append(NotesMoveIssue(vol: note.vol, name: note.name, reason: Self.keepCancelled))
            }
            return out
        }

        // 4. One commit for everything new. A failed commit means nothing is
        // deleted, not even the notes already in the repo.
        var facts = MoveFacts(canDelete: canDelete)
        var blobFailures: [String: String] = [:]
        if !uploads.isEmpty {
            await progress(.committing)
            let committed = await github.commit(uploads)
            if let sha = committed.commitSHA {
                out.commitSHA = sha
                for error in committed.errors { blobFailures[error.name] = error.error }
            } else {
                facts.commitFailure = committed.errors.first?.error ?? "no commit"
            }
        }

        // 5. Verify against the tree of the commit just made (pinned, not the
        // branch head), or a fresh listing when nothing needed uploading.
        if facts.commitFailure == nil {
            await progress(.verifying)
            do {
                if let sha = out.commitSHA {
                    facts.verified = try await github.blobs(atCommit: sha)
                } else {
                    facts.verified = try await github.existingBlobs()
                }
            } catch {
                facts.verifyFailure = GitHubNotesBackend.describe(error)
            }
        }

        var toDelete: [PulledNote] = []
        for note in pulled {
            if note.upload, let failure = blobFailures[note.remoteName] {
                out.errors.append(NotesMoveIssue(vol: note.vol, name: note.name, reason: failure))
                continue
            }
            if note.upload {
                if out.commitSHA != nil { out.uploaded += 1 }
            } else {
                out.already += 1
            }
            switch Self.deletable(note, facts) {
            case .delete: toDelete.append(note)
            case .keep(let reason):
                out.kept.append(NotesMoveIssue(vol: note.vol, name: note.name, reason: reason))
            }
        }

        // 6. Delete, one at a time, only if the pager still holds exactly the
        // bytes pulled. A link drop stops here; the next run finds the rest
        // as already in the repo and deletes them then.
        var stop: String?
        for (index, note) in toDelete.enumerated() {
            if stop == nil, Task.isCancelled { stop = Self.keepCancelled }
            if let stop {
                out.kept.append(NotesMoveIssue(vol: note.vol, name: note.name, reason: stop))
                continue
            }
            await progress(.deleting(done: index, total: toDelete.count))
            do {
                try await files.delete(vol: note.vol, path: "/notes/" + note.name,
                                       ifSize: note.size, ifCRC: note.crc32)
                out.deleted += 1
                await progress(.deleted(vol: note.vol, name: note.name))
            } catch let error as RPCError where error.isChanged {
                out.kept.append(NotesMoveIssue(vol: note.vol, name: note.name, reason: Self.keepChanged))
            } catch let error as RPCError where error.code == "disconnected" || error.code == "timeout" {
                stop = Self.keepLinkDropped
                out.kept.append(NotesMoveIssue(vol: note.vol, name: note.name, reason: Self.keepLinkDropped))
            } catch {
                out.errors.append(NotesMoveIssue(vol: note.vol, name: note.name,
                                                 reason: "delete: \(GitHubNotesBackend.describe(error))"))
            }
        }
        if !toDelete.isEmpty { await progress(.deleting(done: toDelete.count, total: toDelete.count)) }
        return out
    }

    // MARK: - Target "phone"

    private enum PhoneOutcome {
        case error
        case already
        case stored
    }

    /// Pull, check and store one manifest entry in the phone folder.
    private func deliverToPhone(_ file: Entry) async -> PhoneOutcome {
        guard GitHubNotesBackend.safeName(file.name) else { return .error }
        // A phone copy with the manifest's size and CRC32 needs no pull (P7.63).
        if phoneHas(file) { return .already }
        let bytes: [UInt8]
        do {
            (bytes, _) = try await session.readFile(vol: file.vol, path: "/notes/" + file.name, timeout: readTimeout)
        } catch {
            return .error
        }
        // The manifest describes the raw on-disk bytes; anything else (a file
        // that changed after the manifest was built) is an error.
        guard UInt32(exactly: bytes.count) == file.size, CRC32.checksum(bytes) == file.crc32 else {
            return .error
        }
        return store(file.name, bytes) ? .stored : .error
    }

    /// The phone folder already holds `file` byte for byte.
    private func phoneHas(_ file: Entry) -> Bool {
        guard let url = phoneDirectory?.appendingPathComponent(file.name),
              let data = try? Data(contentsOf: url), data.count == Int(file.size) else { return false }
        return CRC32.checksum(Array(data)) == file.crc32
    }

    /// Write `bytes` verbatim into the phone folder (atomic replace).
    private func store(_ name: String, _ bytes: [UInt8]) -> Bool {
        guard let directory = phoneDirectory else { return false }
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try Data(bytes).write(to: directory.appendingPathComponent(name), options: .atomic)
            return true
        } catch {
            return false
        }
    }

    private func emit(_ job: Int, phase: String, uploaded: Int, already: Int,
                      deleted: Int? = nil, kept: Int? = nil, errors: [String], done: Bool) async {
        var params: [String: JSONValue] = [
            "job": .number(Double(job)),
            "phase": .string(phase),
            "uploaded": .number(Double(uploaded)),
            "already": .number(Double(already)),
            "errors": .array(errors.map { .string($0) }),
            "done": .bool(done),
        ]
        if let deleted { params["deleted"] = .number(Double(deleted)) }
        if let kept { params["kept"] = .number(Double(kept)) }
        await session.emit("evt.notes.sync", params: .object(params))
    }
}
