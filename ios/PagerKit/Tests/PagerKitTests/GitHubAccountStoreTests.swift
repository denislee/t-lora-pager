import Foundation
import XCTest

@testable import PagerKit

/// `GitHubAccountStore`: repo/branch in `UserDefaults`, the PAT in a
/// `SecretStore` (IOS_NOTES_VIEWER_PLAN.md §3.7, HUB_REMOVAL_PLAN.md D-H2).
final class GitHubAccountStoreTests: XCTestCase {
    private var suiteName = ""
    private var defaults: UserDefaults!
    private var secrets: MemorySecretStore!

    override func setUp() {
        super.setUp()
        suiteName = "GitHubAccountStoreTests-\(UUID().uuidString)"
        defaults = UserDefaults(suiteName: suiteName)!
        secrets = MemorySecretStore()
    }

    override func tearDown() {
        defaults.removePersistentDomain(forName: suiteName)
        defaults = nil
        secrets = nil
        super.tearDown()
    }

    private func makeStore() -> GitHubAccountStore {
        GitHubAccountStore(defaults: defaults, secrets: secrets)
    }

    func testSaveLoad() throws {
        let store = makeStore()
        try store.save(repo: "octocat/notes", branch: "main", token: "ghp_secret")

        XCTAssertEqual(store.repo, "octocat/notes")
        XCTAssertEqual(store.branch, "main")
        XCTAssertTrue(store.hasToken)
        XCTAssertTrue(store.isConnected)
        XCTAssertEqual(store.config(),
                       GitHubNotesConfig(repo: "octocat/notes", branch: "main", token: "ghp_secret"))
        XCTAssertEqual(secrets.read(GitHubAccountStore.tokenKey), "ghp_secret")
    }

    func testDefaultsBranchMain() throws {
        let store = makeStore()
        try store.save(repo: "octocat/notes", branch: "", token: "ghp_secret")

        XCTAssertEqual(store.branch, "main")
        XCTAssertEqual(store.config()?.branch, "main")
    }

    func testReplaceTokenKeepsWhenNil() throws {
        let store = makeStore()
        try store.save(repo: "octocat/notes", branch: "main", token: "ghp_secret")

        // "Replace token" left alone: the stored token survives a branch change.
        try store.save(repo: "octocat/notes", branch: "dev", token: nil)
        XCTAssertEqual(store.branch, "dev")
        XCTAssertEqual(secrets.read(GitHubAccountStore.tokenKey), "ghp_secret")
        XCTAssertEqual(store.config()?.token, "ghp_secret")

        // An empty token means the same as nil.
        try store.save(repo: "octocat/notes", branch: "dev", token: "")
        XCTAssertEqual(store.config()?.token, "ghp_secret")

        // A new token replaces the old one.
        try store.save(repo: "octocat/notes", branch: "dev", token: "new")
        XCTAssertEqual(secrets.read(GitHubAccountStore.tokenKey), "new")
        XCTAssertEqual(store.config()?.token, "new")
    }

    func testRejectsBadRepo() throws {
        let store = makeStore()
        try store.save(repo: "octocat/notes", branch: "main", token: "ghp_secret")
        let before = store.config()

        for bad in ["noslash", "a/b/c", "../x"] {
            XCTAssertThrowsError(try store.save(repo: bad, branch: "dev", token: "other")) { error in
                XCTAssertEqual(error as? GitHubAccountError, GitHubAccountError("repo must be owner/name"),
                               "repo \(bad)")
            }
            XCTAssertEqual(store.repo, "octocat/notes", "repo \(bad)")
            XCTAssertEqual(store.branch, "main", "repo \(bad)")
            XCTAssertEqual(secrets.read(GitHubAccountStore.tokenKey), "ghp_secret", "repo \(bad)")
            XCTAssertEqual(store.config(), before, "repo \(bad)")
        }
    }

    func testTokenRequired() throws {
        let store = makeStore()

        XCTAssertThrowsError(try store.save(repo: "octocat/notes", branch: "main", token: nil)) { error in
            XCTAssertEqual(error as? GitHubAccountError, GitHubAccountError("token required"))
        }
        XCTAssertThrowsError(try store.save(repo: "octocat/notes", branch: "main", token: "")) { error in
            XCTAssertEqual(error as? GitHubAccountError, GitHubAccountError("token required"))
        }

        // Nothing was stored on the way to the throw.
        XCTAssertNil(store.repo)
        XCTAssertFalse(store.hasToken)
        XCTAssertFalse(store.isConnected)
        XCTAssertNil(store.config())
    }

    func testDisconnectClearsAll() throws {
        let store = makeStore()
        try store.save(repo: "octocat/notes", branch: "dev", token: "ghp_secret")

        store.disconnect()

        XCTAssertNil(store.repo)
        XCTAssertFalse(store.hasToken)
        XCTAssertFalse(store.isConnected)
        XCTAssertNil(store.config())
        XCTAssertNil(secrets.read(GitHubAccountStore.tokenKey))
    }

    func testTokenNeverInDescription() throws {
        let store = makeStore()
        try store.save(repo: "octocat/notes", branch: "main", token: "ghp_secret")

        XCTAssertTrue(store.description.contains("octocat/notes@main"), store.description)
        XCTAssertFalse(store.description.contains("ghp_secret"))
        XCTAssertFalse(String(describing: store).contains("ghp_secret"))
        XCTAssertFalse("\(store)".contains("ghp_secret"))
    }

    func testTrimsWhitespace() throws {
        let store = makeStore()
        try store.save(repo: " octocat/notes ", branch: " main ", token: " tok\n")

        XCTAssertEqual(store.repo, "octocat/notes")
        XCTAssertEqual(store.branch, "main")
        XCTAssertEqual(secrets.read(GitHubAccountStore.tokenKey), "tok")
        XCTAssertEqual(store.config()?.token, "tok")
    }

    func testConfigReadsLatestValues() throws {
        let store = makeStore()
        try store.save(repo: "octocat/notes", branch: "main", token: "ghp_secret")
        let backend = GitHubNotesBackend(config: { store.config() })
        XCTAssertEqual(backend.cacheKey, "octocat/notes@main")

        // The backend re-reads on every call: a second save is visible at once.
        try store.save(repo: "octocat/notes", branch: "dev", token: nil)
        XCTAssertEqual(store.config(),
                       GitHubNotesConfig(repo: "octocat/notes", branch: "dev", token: "ghp_secret"))
        XCTAssertEqual(backend.cacheKey, "octocat/notes@dev")
    }
}
