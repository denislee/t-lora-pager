import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

@testable import PagerKit

// MARK: - Fake GitHub repo

/// In-memory stand-in for the GitHub Git Data API of one notes repo
/// (`octocat/vault`, branch `main`), with real git blob SHAs. Unlike
/// `MockGitHub` it keeps actual commits and trees, so a listing can be read
/// back at a pinned commit instead of only at the moving branch head.
///
/// Answers `StubRequest`s in-process through `respond`, the same way
/// `MockGitHub` does, and records what it was asked so tests can assert the
/// pinned read never touched the branch.
final class FakeGitHubRepo: @unchecked Sendable {
    static let repoName = "octocat/vault"
    static let branchName = "main"

    private let lock = NSLock()

    // Git objects. `trees` maps a tree SHA to full path ("notes/<name>") →
    // blob SHA; `blobs` maps a blob SHA to its bytes.
    private var blobs: [String: [UInt8]] = [:]
    private var trees: [String: [String: String]] = [:]
    private var commits: [String: (tree: String, parent: String?)] = [:]
    private var head = "commit-0"

    private var treeSeq = 0
    private var commitSeq = 0
    private var concurrentSeq = 0

    // MARK: Knobs

    /// The trees listing claims `truncated: true`.
    var truncateListing = false
    /// GET trees at a commit ref (not the branch name) fails with a 500.
    var failCommitTreeReads = false
    /// name → SHA reported instead of the real one, for commit-ref reads only.
    var tamper: [String: String] = [:]
    /// POST /git/blobs fails with a 422.
    var failBlobs = false
    /// POST /git/commits fails with a 500.
    var failCommits = false
    /// PATCH /git/refs fails with a 500.
    var failRefs = false
    /// PATCH /git/refs returns a 422 this many times, each time moving the
    /// branch with someone else's commit.
    var refConflictsLeft = 0

    // MARK: Counters (read under the lock)

    var branchListCalls = 0
    var commitListCalls = 0
    var commitListRefs: [String] = []
    var blobPosts = 0
    var commitPosts = 0
    var refPatches = 0
    /// Requests no route matched (must stay 0).
    var unexpected = 0

    init() {
        trees["tree-0"] = [:]
        commits["commit-0"] = (tree: "tree-0", parent: nil)
    }

    // MARK: State helpers

    /// Adds `notes` in a new commit on top of the branch head.
    func seed(_ notes: [String: [UInt8]]) {
        _ = addCommit(notes)
    }

    /// Adds `notes` in a new commit on top of the branch head; returns its SHA.
    @discardableResult
    func addCommit(_ notes: [String: [UInt8]]) -> String {
        lock.lock()
        defer { lock.unlock() }
        return addCommitLocked(notes)
    }

    private func addCommitLocked(_ notes: [String: [UInt8]]) -> String {
        var tree = trees[commits[head]?.tree ?? "tree-0"] ?? [:]
        for (name, bytes) in notes {
            let sha = GitBlob.sha1(bytes)
            blobs[sha] = bytes
            tree["notes/\(name)"] = sha
        }
        treeSeq += 1
        let treeSHA = "tree-\(treeSeq)"
        trees[treeSHA] = tree

        commitSeq += 1
        let sha = "commit-\(commitSeq)"
        commits[sha] = (tree: treeSHA, parent: head)
        head = sha
        return sha
    }

    var headSHA: String {
        lock.lock()
        defer { lock.unlock() }
        return head
    }

    /// name → bytes under `notes/` for `commit` (default: the branch head).
    func notes(at commit: String? = nil) -> [String: [UInt8]] {
        lock.lock()
        defer { lock.unlock() }
        guard let current = commits[commit ?? head],
              let tree = trees[current.tree] else { return [:] }
        var out: [String: [UInt8]] = [:]
        for (path, sha) in tree where path.hasPrefix("notes/") {
            let name = String(path.dropFirst("notes/".count))
            if let bytes = blobs[sha] { out[name] = bytes }
        }
        return out
    }

    /// The blob SHA of `notes/<name>` at `commit` (default: the branch head).
    func blobSHA(_ name: String, at commit: String? = nil) -> String? {
        lock.lock()
        defer { lock.unlock() }
        guard let current = commits[commit ?? head],
              let tree = trees[current.tree] else { return nil }
        return tree["notes/\(name)"]
    }

    // MARK: Routing

    func respond(_ r: StubRequest) -> StubResponse {
        lock.lock()
        defer { lock.unlock() }

        let prefix = "/repos/\(Self.repoName)"
        guard r.path.hasPrefix(prefix) else {
            unexpected += 1
            return .json(500, "{}")
        }
        let rest = String(r.path.dropFirst(prefix.count))

        // GET /git/trees/<ref>:notes — the notes listing at a branch or commit.
        if r.method == "GET", rest.hasPrefix("/git/trees/"), rest.hasSuffix(":notes") {
            let encoded = String(rest.dropFirst("/git/trees/".count).dropLast(":notes".count))
            return listTree(ref: encoded.removingPercentEncoding ?? encoded)
        }

        // GET /branches/main — the branch head and its tree.
        if r.method == "GET", rest == "/branches/\(Self.branchName)" {
            let tree = commits[head]?.tree ?? "tree-0"
            return Self.jsonValue(200, .object([
                "commit": .object([
                    "sha": .string(head),
                    "commit": .object(["tree": .object(["sha": .string(tree)])]),
                ]),
            ]))
        }

        // POST /git/blobs — create one blob from base64 bytes.
        if r.method == "POST", rest == "/git/blobs" {
            blobPosts += 1
            if failBlobs { return .json(422, #"{"message":"blob create failed"}"#) }
            let body = try? JSONValue.decode(bytes: Array(r.body))
            guard let content = body?["content"]?.stringValue,
                  let decoded = Data(base64Encoded: content) else {
                return .json(422, #"{"message":"bad blob body"}"#)
            }
            let bytes = Array(decoded)
            let sha = GitBlob.sha1(bytes)
            blobs[sha] = bytes
            return Self.jsonValue(201, .object(["sha": .string(sha)]))
        }

        // POST /git/trees — a new tree on top of `base_tree`.
        if r.method == "POST", rest == "/git/trees" {
            let body = try? JSONValue.decode(bytes: Array(r.body))
            guard let body, let base = body["base_tree"]?.stringValue,
                  var tree = trees[base] else {
                return .json(422, #"{"message":"bad base tree"}"#)
            }
            if case .array(let entries)? = body["tree"] {
                for entry in entries {
                    guard let path = entry["path"]?.stringValue,
                          let sha = entry["sha"]?.stringValue else { continue }
                    tree[path] = sha
                }
            }
            treeSeq += 1
            let sha = "tree-\(treeSeq)"
            trees[sha] = tree
            return Self.jsonValue(201, .object(["sha": .string(sha)]))
        }

        // POST /git/commits — a new commit on top of its first parent.
        if r.method == "POST", rest == "/git/commits" {
            commitPosts += 1
            if failCommits { return .json(500, #"{"message":"commit create failed"}"#) }
            let body = try? JSONValue.decode(bytes: Array(r.body))
            guard let body, let tree = body["tree"]?.stringValue,
                  trees[tree] != nil else {
                return .json(422, #"{"message":"bad commit tree"}"#)
            }
            var parent: String?
            if case .array(let parents)? = body["parents"] {
                parent = parents.first?.stringValue
            }
            commitSeq += 1
            let sha = "commit-\(commitSeq)"
            commits[sha] = (tree: tree, parent: parent)
            return Self.jsonValue(201, .object(["sha": .string(sha)]))
        }

        // PATCH /git/refs/heads/main — fast-forward only.
        if r.method == "PATCH", rest == "/git/refs/heads/\(Self.branchName)" {
            refPatches += 1
            if failRefs { return .json(500, #"{"message":"ref update failed"}"#) }
            if refConflictsLeft > 0 {
                refConflictsLeft -= 1
                // Someone else pushed between this attempt's head read and now.
                concurrentSeq += 1
                _ = addCommitLocked(["concurrent-\(concurrentSeq).txt": Array("c".utf8)])
                return .json(422, #"{"message":"Update is not a fast forward"}"#)
            }
            let body = try? JSONValue.decode(bytes: Array(r.body))
            guard let sha = body?["sha"]?.stringValue, let commit = commits[sha] else {
                return .json(422, #"{"message":"bad commit sha"}"#)
            }
            guard commit.parent == head else {
                return .json(422, #"{"message":"Update is not a fast forward"}"#)
            }
            head = sha
            return .json(200, "{}")
        }

        unexpected += 1
        return .json(500, "{}")
    }

    /// The `notes/` listing of `ref`: direct children as blobs, deeper paths
    /// collapsed into one tree entry per directory. The lock must be held.
    private func listTree(ref: String) -> StubResponse {
        let isBranch = (ref == Self.branchName)
        let commit: String
        if isBranch {
            branchListCalls += 1
            commit = head
        } else {
            commitListCalls += 1
            commitListRefs.append(ref)
            if failCommitTreeReads { return .json(500, #"{"message":"boom"}"#) }
            guard commits[ref] != nil else {
                return .json(404, #"{"message":"Not Found"}"#)
            }
            commit = ref
        }

        let tree = trees[commits[commit]?.tree ?? "tree-0"] ?? [:]
        var files: [String: String] = [:]
        var dirs = Set<String>()
        for (path, sha) in tree where path.hasPrefix("notes/") {
            let relative = String(path.dropFirst("notes/".count))
            if let slash = relative.firstIndex(of: "/") {
                dirs.insert(String(relative[relative.startIndex..<slash]))
            } else {
                files[relative] = sha
            }
        }
        guard !files.isEmpty || !dirs.isEmpty else {
            return .json(404, #"{"message":"Not Found"}"#)
        }

        var entries: [JSONValue] = []
        for name in files.keys.sorted() {
            var sha = files[name]!
            if !isBranch, let reported = tamper[name] { sha = reported }
            entries.append(.object([
                "path": .string(name), "type": "blob", "mode": "100644", "sha": .string(sha),
            ]))
        }
        for dir in dirs.sorted() {
            entries.append(.object([
                "path": .string(dir), "type": "tree", "mode": "040000", "sha": .string("t"),
            ]))
        }
        return Self.jsonValue(200, .object([
            "sha": .string("notes-tree"),
            "tree": .array(entries),
            "truncated": .bool(truncateListing),
        ]))
    }

    // MARK: Wiring

    /// A backend wired to `repo` through a fresh stub session, plus the server
    /// so tests can inspect the requests it saw.
    static func backend(_ repo: FakeGitHubRepo) -> (GitHubNotesBackend, StubServer) {
        let server = StubServer { repo.respond($0) }
        let backend = GitHubNotesBackend(
            config: { GitHubNotesConfig(repo: repoName, branch: branchName, token: "tok") },
            session: StubURLProtocol.session(server),
            baseBackoff: .milliseconds(1)
        )
        return (backend, server)
    }

    /// A JSON response whose body is `value` encoded with sorted keys.
    private static func jsonValue(_ status: Int, _ value: JSONValue) -> StubResponse {
        let text = (try? value.encoded()).map { String(decoding: $0, as: UTF8.self) } ?? "{}"
        return .json(status, text)
    }
}
