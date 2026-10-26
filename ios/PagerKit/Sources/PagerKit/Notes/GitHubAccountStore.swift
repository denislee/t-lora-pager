import Foundation

/// Where the PAT lives: the Keychain in the app (`kSecClassGenericPassword`,
/// this-device-only), memory in tests.
public protocol SecretStore: Sendable {
    func read(_ key: String) -> String?
    func write(_ key: String, _ value: String) throws
    func delete(_ key: String)
}

/// An in-memory `SecretStore` (tests, previews).
public final class MemorySecretStore: SecretStore, @unchecked Sendable {
    private let lock = NSLock()
    private var values: [String: String] = [:]

    public init() {}

    public func read(_ key: String) -> String? {
        lock.lock()
        defer { lock.unlock() }
        return values[key]
    }

    public func write(_ key: String, _ value: String) throws {
        lock.lock()
        defer { lock.unlock() }
        values[key] = value
    }

    public func delete(_ key: String) {
        lock.lock()
        defer { lock.unlock() }
        values.removeValue(forKey: key)
    }
}

/// A rejected GitHub notes connection setting.
public struct GitHubAccountError: Error, Equatable, Sendable {
    public let msg: String

    public init(_ msg: String) { self.msg = msg }
}

/// The phone's GitHub notes connection (IOS_NOTES_VIEWER_PLAN.md §3.7,
/// HUB_REMOVAL_PLAN.md D-H2): repo `owner/name` and branch in `UserDefaults`,
/// the fine-grained PAT (`contents:write`) in a `SecretStore`. The token is
/// never returned by `description`, never logged.
///
/// `@unchecked Sendable`: `defaults` and `secrets` are each thread-safe on
/// their own, and every compound read/write of the two is serialised by
/// `lock`, so `config()` cannot pair a repo from one save with a token from
/// another.
public final class GitHubAccountStore: @unchecked Sendable, CustomStringConvertible {
    public static let repoKey = "GitHub.notes.repo"
    public static let branchKey = "GitHub.notes.branch"
    public static let tokenKey = "github.notes.token"

    private let defaults: UserDefaults
    private let secrets: any SecretStore
    private let lock = NSLock()

    public init(defaults: UserDefaults = .standard, secrets: any SecretStore) {
        self.defaults = defaults
        self.secrets = secrets
    }

    /// The repo `owner/name`, nil when unset or empty.
    public var repo: String? {
        lock.lock()
        defer { lock.unlock() }
        return storedRepo()
    }

    /// The branch, `"main"` when unset or empty.
    public var branch: String {
        lock.lock()
        defer { lock.unlock() }
        return storedBranch()
    }

    /// True when a token is stored.
    public var hasToken: Bool {
        lock.lock()
        defer { lock.unlock() }
        return storedToken() != nil
    }

    /// Connected = repo + token present.
    public var isConnected: Bool {
        lock.lock()
        defer { lock.unlock() }
        return storedRepo() != nil && storedToken() != nil
    }

    /// Save repo/branch, and the token when `token` is non-nil and non-empty
    /// (nil keeps the stored one: "Replace token" is optional). Whitespace is
    /// trimmed on all three. An empty branch becomes `"main"`. Every check runs
    /// before anything is written, so a throw leaves the store as it was.
    public func save(repo: String, branch: String, token: String?) throws {
        let normalizedRepo = repo.trimmed
        guard GitHubNotesBackend.validRepo(normalizedRepo) else {
            throw GitHubAccountError("repo must be owner/name")
        }
        let trimmedBranch = branch.trimmed
        let resolvedBranch = trimmedBranch.isEmpty ? "main" : trimmedBranch
        let supplied = (token ?? "").trimmed

        lock.lock()
        defer { lock.unlock() }
        let tokenToStore = supplied.isEmpty ? storedToken() : supplied
        guard let tokenToStore else { throw GitHubAccountError("token required") }
        // The secret write can fail; do it first so the defaults never move
        // ahead of the token they belong with.
        if !supplied.isEmpty {
            try secrets.write(Self.tokenKey, tokenToStore)
        }
        defaults.set(normalizedRepo, forKey: Self.repoKey)
        defaults.set(resolvedBranch, forKey: Self.branchKey)
    }

    /// The config `GitHubNotesBackend` reads on every call; nil unless
    /// connected.
    public func config() -> GitHubNotesConfig? {
        lock.lock()
        defer { lock.unlock() }
        guard let repo = storedRepo(), let token = storedToken() else { return nil }
        return GitHubNotesConfig(repo: repo, branch: storedBranch(), token: token)
    }

    /// "Disconnect": forget repo, branch and token.
    public func disconnect() {
        lock.lock()
        defer { lock.unlock() }
        defaults.removeObject(forKey: Self.repoKey)
        defaults.removeObject(forKey: Self.branchKey)
        secrets.delete(Self.tokenKey)
    }

    /// `"owner/name@branch, token set"` | `"not connected"` — never the token.
    public var description: String {
        lock.lock()
        defer { lock.unlock() }
        guard let repo = storedRepo(), storedToken() != nil else { return "not connected" }
        return "\(repo)@\(storedBranch()), token set"
    }

    // MARK: - Stored values (call with `lock` held)

    private func storedRepo() -> String? {
        let repo = (defaults.string(forKey: Self.repoKey) ?? "").trimmed
        return repo.isEmpty ? nil : repo
    }

    private func storedBranch() -> String {
        let branch = (defaults.string(forKey: Self.branchKey) ?? "").trimmed
        return branch.isEmpty ? "main" : branch
    }

    private func storedToken() -> String? {
        let token = (secrets.read(Self.tokenKey) ?? "").trimmed
        return token.isEmpty ? nil : token
    }
}

private extension String {
    /// Surrounding whitespace and newlines removed (a pasted token often has
    /// both).
    var trimmed: String { trimmingCharacters(in: .whitespacesAndNewlines) }
}
