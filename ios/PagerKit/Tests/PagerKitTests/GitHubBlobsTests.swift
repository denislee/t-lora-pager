import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
import XCTest

@testable import PagerKit

// MARK: - Blob listings and landed commit SHAs

/// The listings behind the move engine: `existingBlobs` reads the branch with
/// real git blob SHAs, `blobs(atCommit:)` pins one commit instead of the moving
/// head, and `commit` reports the SHA that actually landed.
final class GitHubBlobsTests: XCTestCase {

    // MARK: - Helpers

    /// The `Salted__` ciphertext bytes shared between the notes tests: the
    /// magic plus a deterministic `(i*37+11) & 0xFF` tail.
    private static func saltedBytes() -> [UInt8] {
        Array("Salted__".utf8) + (0..<64).map { UInt8(($0 * 37 + 11) & 0xFF) }
    }

    // MARK: - Branch listings

    func testExistingBlobsAreRealBlobSHAs() async throws {
        let repo = FakeGitHubRepo()
        let alpha = Array("alpha".utf8)
        let salted = GitHubBlobsTests.saltedBytes()
        repo.seed([
            "a.txt": alpha,
            "b.txt": salted,
            "sub/x.txt": Array("nested".utf8),
        ])
        let (backend, _) = FakeGitHubRepo.backend(repo)

        let blobs = try await backend.existingBlobs()
        // A deeper path is one tree entry, never a note.
        XCTAssertEqual(blobs, [
            "a.txt": GitBlob.sha1(alpha),
            "b.txt": GitBlob.sha1(salted),
        ])
    }

    func testExistingBlobsEmptyRepo404() async throws {
        let repo = FakeGitHubRepo()
        let (backend, _) = FakeGitHubRepo.backend(repo)

        let blobs = try await backend.existingBlobs()
        XCTAssertEqual(blobs, [:])
    }

    func testExistingBlobsTruncatedThrows() async throws {
        let repo = FakeGitHubRepo()
        repo.seed(["a.txt": Array("alpha".utf8)])
        repo.truncateListing = true
        let (backend, _) = FakeGitHubRepo.backend(repo)

        do {
            _ = try await backend.existingBlobs()
            XCTFail("expected error")
        } catch let error as NotesSyncError {
            // A truncated tree would look like missing notes and get them
            // pushed again, so it is a hard error.
            XCTAssertTrue(error.msg.contains("truncated"), "msg = \(error.msg)")
        } catch {
            XCTFail("unexpected error \(error)")
        }
    }

    func testExistingNamesStillWorks() async throws {
        let repo = FakeGitHubRepo()
        repo.seed([
            "a.txt": Array("alpha".utf8),
            "b.txt": Array("bravo".utf8),
        ])
        let (backend, _) = FakeGitHubRepo.backend(repo)

        let names = try await backend.existingNames()
        XCTAssertEqual(names, Set(["a.txt", "b.txt"]))
    }

    // MARK: - Commits

    func testCommitReturnsCommitSHA() async throws {
        let repo = FakeGitHubRepo()
        let alpha = Array("alpha".utf8)
        let (backend, _) = FakeGitHubRepo.backend(repo)

        let result = await backend.commit([NoteFile(name: "a.txt", bytes: alpha)])
        XCTAssertEqual(result.commitSHA, repo.headSHA)
        XCTAssertEqual(result.uploaded, 1)
        XCTAssertTrue(result.errors.isEmpty)
        XCTAssertEqual(repo.notes()["a.txt"], alpha)
    }

    func testCommitSHAAfterRefConflictRetry() async throws {
        let repo = FakeGitHubRepo()
        repo.refConflictsLeft = 1
        let alpha = Array("alpha".utf8)
        let (backend, _) = FakeGitHubRepo.backend(repo)

        let result = await backend.commit([NoteFile(name: "a.txt", bytes: alpha)])
        XCTAssertEqual(result.commitSHA, repo.headSHA)
        XCTAssertEqual(repo.refPatches, 2, "conflict then success")
        XCTAssertEqual(repo.notes()["a.txt"], alpha)
        XCTAssertEqual(repo.notes()["concurrent-1.txt"], Array("c".utf8),
                       "the concurrent push stayed on the branch")
    }

    func testCommitFailureHasNoCommitSHA() async throws {
        let repo = FakeGitHubRepo()
        repo.failCommits = true
        let (backend, _) = FakeGitHubRepo.backend(repo)

        let result = await backend.commit([NoteFile(name: "a.txt", bytes: Array("alpha".utf8))])
        XCTAssertNil(result.commitSHA, "nothing landed")
        XCTAssertEqual(result.uploaded, 0)
        XCTAssertEqual(result.errors.count, 1)
    }

    // MARK: - Pinned commit listings

    func testBlobsAtCommitPinnedNotBranch() async throws {
        let repo = FakeGitHubRepo()
        let alpha = Array("alpha".utf8)
        let (backend, _) = FakeGitHubRepo.backend(repo)

        let first = await backend.commit([NoteFile(name: "a.txt", bytes: alpha)])
        let commit = try XCTUnwrap(first.commitSHA)

        // Someone else moves the branch after the commit landed.
        repo.addCommit(["b.txt": Array("bravo".utf8)])
        XCTAssertNotEqual(repo.headSHA, commit)

        let branchCallsBefore = repo.branchListCalls
        let commitCallsBefore = repo.commitListCalls
        let blobs = try await backend.blobs(atCommit: commit)

        XCTAssertEqual(blobs, ["a.txt": GitBlob.sha1(alpha)], "the pinned commit holds no b.txt")
        XCTAssertEqual(repo.branchListCalls, branchCallsBefore, "the branch listing must not be used")
        XCTAssertEqual(repo.commitListCalls, commitCallsBefore + 1)
        XCTAssertEqual(repo.commitListRefs, [commit])
    }

    func testBlobsAtCommitFailureThrows() async throws {
        let repo = FakeGitHubRepo()
        repo.failCommitTreeReads = true
        let (backend, _) = FakeGitHubRepo.backend(repo)

        do {
            _ = try await backend.blobs(atCommit: "commit-0")
            XCTFail("expected error")
        } catch let error as NotesSyncError {
            XCTAssertTrue(error.msg.contains("github 500"), "msg = \(error.msg)")
        } catch {
            XCTFail("unexpected error \(error)")
        }
    }
}
