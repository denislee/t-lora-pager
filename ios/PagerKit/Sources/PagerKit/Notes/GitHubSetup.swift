import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

/// A repository as GitHub describes it to the setup screen.
public struct GitHubRepoInfo: Sendable, Equatable, Identifiable {
    /// `owner/name`, with GitHub's own capitalisation.
    public let fullName: String
    public let defaultBranch: String
    public let isPrivate: Bool

    public var id: String { fullName }

    public init(fullName: String, defaultBranch: String, isPrivate: Bool) {
        self.fullName = fullName
        self.defaultBranch = defaultBranch
        self.isPrivate = isPrivate
    }
}

/// The helpers behind the GitHub setup screen: accept whatever the user
/// pastes for a repository, open GitHub's token page already filled in, list
/// the repositories a token reaches, and check a repo + branch before saving.
/// The token only ever goes to `apiBase` as a bearer header.
public enum GitHubSetup {
    /// `owner/name` and, for a `/tree/<branch>` link, the branch, from any of
    /// `owner/name`, `https://github.com/owner/name(.git)(/tree/<branch>)`,
    /// `github.com/owner/name`, `git@github.com:owner/name.git` or
    /// `ssh://git@github.com/owner/name.git`. Nil when it isn't one of those.
    public static func parseRepo(_ input: String) -> (repo: String, branch: String?)? {
        var s = input.trimmingCharacters(in: .whitespacesAndNewlines)
        for prefix in ["git@github.com:", "ssh://git@github.com/", "https://", "http://"]
        where s.lowercased().hasPrefix(prefix) {
            s = String(s.dropFirst(prefix.count))
            break
        }
        for host in ["www.github.com/", "github.com/"] where s.lowercased().hasPrefix(host) {
            s = String(s.dropFirst(host.count))
            break
        }
        if let cut = s.firstIndex(where: { $0 == "?" || $0 == "#" }) { s = String(s[..<cut]) }
        var parts = s.split(separator: "/", omittingEmptySubsequences: true).map(String.init)
        guard parts.count >= 2 else { return nil }
        if parts[1].lowercased().hasSuffix(".git") { parts[1] = String(parts[1].dropLast(4)) }
        let repo = "\(parts[0])/\(parts[1])"
        guard GitHubNotesBackend.validRepo(repo) else { return nil }
        if parts.count == 2 { return (repo, nil) }
        // Only a branch link carries more segments we understand.
        guard parts[2] == "tree", parts.count > 3 else { return nil }
        return (repo, parts[3...].joined(separator: "/"))
    }

    /// GitHub's new fine-grained token page with the name, description,
    /// expiry and the one permission notes need (`contents: write`) filled
    /// in; `owner` preselects the resource owner. The user still picks the
    /// repository there (GitHub has no parameter for it).
    public static func tokenCreationURL(owner: String? = nil, expiresInDays: Int = 365) -> URL {
        var components = URLComponents(string: "https://github.com/settings/personal-access-tokens/new")!
        var items = [
            URLQueryItem(name: "name", value: "Pager notes"),
            URLQueryItem(name: "description", value: "PagerCompanion: move notes from the pager into a repository"),
            URLQueryItem(name: "expires_in", value: String(expiresInDays)),
            URLQueryItem(name: "contents", value: "write"),
        ]
        if let owner, !owner.isEmpty { items.insert(URLQueryItem(name: "target_name", value: owner), at: 2) }
        components.queryItems = items
        return components.url!
    }

    /// The repositories `token` can reach, most recently pushed first (first
    /// 100). A fine-grained token lists only the repos it was granted.
    public static func repositories(
        token: String, session: URLSession = .shared, apiBase: URL = GitHubNotesBackend.defaultAPIBase
    ) async throws -> [GitHubRepoInfo] {
        let reply = try await get("/user/repos?per_page=100&sort=pushed", token: token,
                                  session: session, apiBase: apiBase)
        guard reply.status == 200 else { throw GitHubAccountError(describe(reply, what: "list repositories")) }
        guard case .array(let items) = try JSONValue.decode(bytes: Array(reply.body)) else {
            throw GitHubAccountError("unexpected repository list from GitHub")
        }
        return items.compactMap(repoInfo)
    }

    /// Check that `token` reaches `repo` and, when `branch` is given, that
    /// the branch exists. Returns GitHub's view of the repo (its default
    /// branch fills an empty branch field).
    public static func inspect(
        repo: String, branch: String?, token: String,
        session: URLSession = .shared, apiBase: URL = GitHubNotesBackend.defaultAPIBase
    ) async throws -> GitHubRepoInfo {
        guard GitHubNotesBackend.validRepo(repo) else { throw GitHubAccountError("repo must be owner/name") }
        let reply = try await get("/repos/\(repo)", token: token, session: session, apiBase: apiBase)
        guard reply.status == 200 else { throw GitHubAccountError(describe(reply, what: repo)) }
        guard let info = repoInfo(try JSONValue.decode(bytes: Array(reply.body))) else {
            throw GitHubAccountError("unexpected repository reply from GitHub")
        }
        if let branch, !branch.isEmpty, branch != info.defaultBranch {
            let path = "/repos/\(repo)/branches/\(GitHubNotesBackend.refPathEscape(branch))"
            let check = try await get(path, token: token, session: session, apiBase: apiBase)
            if check.status == 404 { throw GitHubAccountError("branch \"\(branch)\" not found in \(info.fullName)") }
            guard check.status == 200 else { throw GitHubAccountError(describe(check, what: branch)) }
        }
        return info
    }

    // MARK: - Helpers

    private static func get(_ path: String, token: String, session: URLSession,
                            apiBase: URL) async throws -> HTTPReply {
        var base = apiBase.absoluteString
        while base.hasSuffix("/") { base.removeLast() }
        guard let url = URL(string: base + path) else { throw GitHubAccountError("bad url") }
        let trimmed = token.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { throw GitHubAccountError("token required") }
        var request = try HTTP.request(url: url, timeout: 20, bearer: trimmed)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        request.setValue("PagerKit/1.0", forHTTPHeaderField: "User-Agent")
        return try await HTTP.send(request, using: session)
    }

    static func repoInfo(_ value: JSONValue) -> GitHubRepoInfo? {
        guard let name = value["full_name"]?.stringValue, !name.isEmpty else { return nil }
        return GitHubRepoInfo(fullName: name,
                              defaultBranch: value["default_branch"]?.stringValue ?? "main",
                              isPrivate: value["private"]?.boolValue ?? false)
    }

    /// Plain words for the replies a wrong token or repo gets.
    static func describe(_ reply: HTTPReply, what: String) -> String {
        switch reply.status {
        case 401: return "GitHub rejected the token (expired or mistyped)"
        case 403: return "the token isn't allowed to read \(what)"
        case 404: return "\(what) not found, or the token wasn't granted access to it"
        default: return GitHubNotesBackend.githubError(reply)
        }
    }
}
