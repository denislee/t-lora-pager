import Foundation
import XCTest

@testable import PagerKit

/// `GitHubSetup`: what the setup screen accepts as a repository, the
/// prefilled token page, and the repo / branch checks against a stub GitHub.
final class GitHubSetupTests: XCTestCase {
    func testParseRepoAcceptsCommonForms() {
        let cases: [(String, String, String?)] = [
            ("owner/notes", "owner/notes", nil),
            ("  owner/notes\n", "owner/notes", nil),
            ("https://github.com/owner/notes", "owner/notes", nil),
            ("https://github.com/owner/notes/", "owner/notes", nil),
            ("https://github.com/owner/notes.git", "owner/notes", nil),
            ("http://www.github.com/owner/notes?tab=readme", "owner/notes", nil),
            ("github.com/owner/notes", "owner/notes", nil),
            ("git@github.com:owner/notes.git", "owner/notes", nil),
            ("ssh://git@github.com/owner/notes.git", "owner/notes", nil),
            ("https://github.com/owner/notes/tree/dev", "owner/notes", "dev"),
            ("https://github.com/owner/notes/tree/feature/x", "owner/notes", "feature/x"),
        ]
        for (input, repo, branch) in cases {
            let parsed = GitHubSetup.parseRepo(input)
            XCTAssertEqual(parsed?.repo, repo, input)
            XCTAssertEqual(parsed?.branch, branch, input)
        }
    }

    func testParseRepoRejectsOthers() {
        for input in ["", "owner", "https://github.com/owner", "owner/no te",
                      "https://github.com/owner/notes/blob/main/x.md", "owner/.."] {
            XCTAssertNil(GitHubSetup.parseRepo(input), input)
        }
    }

    func testTokenCreationURLPrefillsContentsWrite() throws {
        let url = GitHubSetup.tokenCreationURL(owner: "owner")
        let components = try XCTUnwrap(URLComponents(url: url, resolvingAgainstBaseURL: false))
        XCTAssertEqual(components.host, "github.com")
        XCTAssertEqual(components.path, "/settings/personal-access-tokens/new")
        let items = Dictionary(uniqueKeysWithValues: (components.queryItems ?? []).map { ($0.name, $0.value ?? "") })
        XCTAssertEqual(items["contents"], "write")
        XCTAssertEqual(items["target_name"], "owner")
        XCTAssertEqual(items["expires_in"], "365")
        XCTAssertNil(URLComponents(url: GitHubSetup.tokenCreationURL(), resolvingAgainstBaseURL: false)?
            .queryItems?.first { $0.name == "target_name" })
    }

    func testInspectReturnsDefaultBranchAndSendsToken() async throws {
        let server = StubServer { request in
            XCTAssertEqual(request.header("Authorization"), "Bearer tok")
            return .json(200, #"{"full_name":"Owner/Notes","default_branch":"trunk","private":true}"#)
        }
        let info = try await GitHubSetup.inspect(repo: "owner/notes", branch: nil, token: " tok\n",
                                                 session: StubURLProtocol.session(server))
        XCTAssertEqual(info, GitHubRepoInfo(fullName: "Owner/Notes", defaultBranch: "trunk", isPrivate: true))
        XCTAssertEqual(server.requests.map(\.path), ["/repos/owner/notes"])
    }

    func testInspectChecksANonDefaultBranch() async throws {
        let server = StubServer { request in
            if request.path.hasSuffix("/branches/dev") { return .json(404, #"{"message":"Branch not found"}"#) }
            return .json(200, #"{"full_name":"owner/notes","default_branch":"main"}"#)
        }
        do {
            _ = try await GitHubSetup.inspect(repo: "owner/notes", branch: "dev", token: "tok",
                                              session: StubURLProtocol.session(server))
            XCTFail("expected a missing-branch error")
        } catch let error as GitHubAccountError {
            XCTAssertEqual(error.msg, "branch \"dev\" not found in owner/notes")
        }
    }

    func testInspectExplainsAMissingRepoOrBadToken() async throws {
        for (status, expected) in [
            (404, "owner/notes not found, or the token wasn't granted access to it"),
            (401, "GitHub rejected the token (expired or mistyped)"),
        ] {
            let server = StubServer { _ in .json(status, #"{"message":"x"}"#) }
            do {
                _ = try await GitHubSetup.inspect(repo: "owner/notes", branch: nil, token: "tok",
                                                  session: StubURLProtocol.session(server))
                XCTFail("expected an error for \(status)")
            } catch let error as GitHubAccountError {
                XCTAssertEqual(error.msg, expected)
            }
        }
    }

    func testRepositoriesListsWhatTheTokenReaches() async throws {
        let server = StubServer { _ in
            .json(200, #"[{"full_name":"owner/notes","default_branch":"main","private":true},{"full_name":"owner/site","default_branch":"gh-pages"}]"#)
        }
        let repos = try await GitHubSetup.repositories(token: "tok", session: StubURLProtocol.session(server))
        XCTAssertEqual(repos.map(\.fullName), ["owner/notes", "owner/site"])
        XCTAssertEqual(repos.map(\.defaultBranch), ["main", "gh-pages"])
        XCTAssertEqual(server.requests.first?.path, "/user/repos")
    }
}
