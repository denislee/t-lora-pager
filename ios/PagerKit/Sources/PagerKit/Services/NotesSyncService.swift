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

    public init(uploaded: Int = 0, already: Int = 0, errors: [NotesPushError] = []) {
        self.uploaded = uploaded
        self.already = already
        self.errors = errors
    }
}

/// A failure that stops a whole push (config, listing).
public struct NotesSyncError: Error, Equatable, Sendable {
    public let msg: String

    public init(_ msg: String) { self.msg = msg }
}

/// An additive-only notes remote: list what is there, add what is not.
public protocol NotesRemote: Sendable {
    /// Names currently under `notes/`.
    func existingNames() async throws -> Set<String>
    /// Add `files` (none of which exist remotely); `already` stays 0.
    func commit(_ files: [NoteFile]) async -> NotesPushResult
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
    /// the folder does not exist yet.
    public func existingNames() async throws -> Set<String> {
        let cfg = try validConfig()
        let url = "\(apiBase)/repos/\(cfg.repo)/contents/notes?ref=\(Self.queryEscape(cfg.branch))"
        let reply = try await exchange("GET", url, cfg, body: nil, attempts: 2)
        if reply.status == 404 { return [] }
        guard reply.status == 200 else { throw NotesSyncError(Self.githubError(reply)) }
        guard case .array(let entries) = try JSONValue.decode(bytes: Array(reply.body)) else {
            throw NotesSyncError("parse: listing is not an array")
        }
        var names = Set<String>()
        for entry in entries {
            let type = entry["type"]?.stringValue ?? ""
            guard type.isEmpty || type == "file", let name = entry["name"]?.stringValue else { continue }
            names.insert(name)
        }
        return names
    }

    /// lilyhub `commitFiles`: blobs (parallel), then tree + commit + ref,
    /// retrying the last three on a 422. Failed blobs are reported and left
    /// out; if the commit itself fails every blobbed file is reported failed.
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
        var request = URLRequest(url: target)
        request.httpMethod = method
        request.timeoutInterval = 30
        request.setValue("Bearer \(cfg.token)", forHTTPHeaderField: "Authorization")
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        request.setValue("PagerKit/1.0", forHTTPHeaderField: "User-Agent")
        if let body {
            request.httpBody = Data(try body.encoded())
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        }
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

    static func queryEscape(_ s: String) -> String {
        var allowed = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")
        allowed.insert(charactersIn: "-_.~")
        return s.addingPercentEncoding(withAllowedCharacters: allowed) ?? s
    }
}

/// Answers the pager's `notes.sync` (PROTOCOL.md §6) on one `LinkSession`.
///
/// The request is answered at once with `{job}`; a background task then pulls
/// each manifest file it needs with `fs.read` (`vol` defaulting to `"int"`,
/// path `/notes/<name>`), checks the bytes against the manifest's `size` and
/// `crc32` — the raw on-disk bytes, so an encrypted note is compared and
/// delivered as its `Salted__` ciphertext, never decrypted — and delivers them:
/// `"github"` through `github` in one commit at the end (names already in the
/// repo count as `already` and are not even pulled), `"phone"` as files in
/// `phoneDirectory`. Progress goes out as `evt.notes.sync` after every file and
/// once more with `done:true`, shaped like the mock phone's `emitNotes`.
public actor NotesSyncService {
    /// Idle bound per file pull.
    public static let defaultReadTimeout: Duration = .seconds(60)

    private struct Entry {
        let name: String
        let size: UInt32
        let crc32: UInt32
        let vol: String
    }

    private let session: LinkSession
    private let github: (any NotesRemote)?
    private let phoneDirectory: URL?
    private let readTimeout: Duration
    private var job = 0
    private var running: [Int: Task<Void, Never>] = [:]

    public init(session: LinkSession, github: (any NotesRemote)?, phoneDirectory: URL?,
                readTimeout: Duration = NotesSyncService.defaultReadTimeout) {
        self.session = session
        self.github = github
        self.phoneDirectory = phoneDirectory
        self.readTimeout = readTimeout
    }

    /// Install the `notes.sync` handler on the session.
    public func register() async {
        await session.handle("notes.sync") { [weak self] params in
            guard let self else { throw RPCError(code: "unsupported", msg: "notes service gone") }
            return try await self.start(params)
        }
    }

    /// Wait for a job's background work (tests).
    public func wait(job: Int) async {
        await running[job]?.value
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
        running[id] = Task { [weak self] in
            await self?.run(job: id, targets: targets, files: entries)
        }
        return ["job": .number(Double(id))]
    }

    private func run(job: Int, targets: Set<String>, files: [Entry]) async {
        defer { running[job] = nil }
        let wantGitHub = targets.contains("github")
        let wantPhone = targets.contains("phone")
        var uploaded = 0
        var already = 0
        var errors: [String] = []

        var remote: Set<String>?
        if wantGitHub, let github {
            remote = try? await github.existingNames()
        }

        var queue: [NoteFile] = []
        for file in files {
            switch await deliver(file, remote: remote, github: wantGitHub, phone: wantPhone) {
            case .error: errors.append(file.name)
            case .already: already += 1
            case .uploaded: uploaded += 1
            case .queued(let note): queue.append(note)
            }
            await emit(job, uploaded, already, errors, done: false)
        }

        if !queue.isEmpty, let github {
            let result = await github.commit(queue)
            uploaded += result.uploaded
            errors.append(contentsOf: result.errors.map(\.name))
        }
        await emit(job, uploaded, already, errors, done: true)
    }

    private enum Outcome {
        case error
        case already
        case uploaded
        case queued(NoteFile)
    }

    /// Pull, check and deliver one manifest entry.
    private func deliver(_ file: Entry, remote: Set<String>?, github wantGitHub: Bool,
                         phone wantPhone: Bool) async -> Outcome {
        guard GitHubNotesBackend.safeName(file.name), !(wantGitHub && remote == nil) else { return .error }
        let inRepo = wantGitHub && remote?.contains(file.name) == true
        if inRepo && !wantPhone { return .already }

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
        if wantPhone, !store(file.name, bytes) { return .error }
        if inRepo { return .already }
        if wantGitHub { return .queued(NoteFile(name: file.name, bytes: bytes)) }
        return .uploaded
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

    private func emit(_ job: Int, _ uploaded: Int, _ already: Int, _ errors: [String], done: Bool) async {
        let params: JSONValue = [
            "job": .number(Double(job)),
            "phase": "upload",
            "uploaded": .number(Double(uploaded)),
            "already": .number(Double(already)),
            "errors": .array(errors.map { .string($0) }),
            "done": .bool(done),
        ]
        await session.emit("evt.notes.sync", params: params)
    }
}
