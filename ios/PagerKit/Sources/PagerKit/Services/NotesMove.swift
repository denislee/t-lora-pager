import Foundation

// The "move notes to GitHub" engine's types and its one deletion rule
// (IOS_NOTES_VIEWER_PLAN.md §3.6). The engine itself lives in
// `NotesSyncService`, so both triggers (the app's "Sync to GitHub" and the
// pager's `notes.sync`) share it and never overlap.

/// A note the move left on the pager (or could not handle), with why.
public struct NotesMoveIssue: Sendable, Equatable {
    public let vol: String
    public let name: String
    public let reason: String

    public init(vol: String, name: String, reason: String) {
        self.vol = vol
        self.name = name
        self.reason = reason
    }
}

/// What one move did. `uploaded`: new in this run's commit; `already`: was in
/// the repo with exactly these bytes; `deleted`: verified on GitHub and then
/// removed from the pager; `kept`: still on the pager (not verified, changed
/// since the pull, link dropped, or a pager without conditional delete);
/// `errors`: notes that could not be pulled or uploaded.
public struct NotesMoveResult: Sendable, Equatable {
    public var uploaded = 0
    public var already = 0
    public var deleted = 0
    public var kept: [NotesMoveIssue] = []
    public var errors: [NotesMoveIssue] = []
    /// The commit this run made, nil when nothing needed uploading or it failed.
    public var commitSHA: String?
    /// The pager honours the `fs.delete` precondition (cap `fs.delete.if`);
    /// without it the move uploads and verifies but deletes nothing.
    public var canDelete = false

    public init() {}

    /// Nothing was on the pager to move.
    public var nothingToDo: Bool {
        uploaded == 0 && already == 0 && deleted == 0 && kept.isEmpty && errors.isEmpty
    }
}

/// Where a move is (for the app's progress line).
public enum NotesMoveProgress: Sendable, Equatable {
    case listing
    case pulling(done: Int, total: Int)
    case committing
    case verifying
    case deleting(done: Int, total: Int)
    /// The pager confirmed this delete: drop any cached copy.
    case deleted(vol: String, name: String)
}

/// One pulled note, its identity fixed by the bytes read from the pager.
struct PulledNote: Sendable, Equatable {
    let vol: String
    let name: String
    let size: UInt32
    let crc32: UInt32
    /// Git blob SHA-1 of the pulled bytes.
    let sha: String
    /// Its name under `notes/` in the repo (the pager name, or the
    /// content-derived `<stem>~<sha7>.txt` on a clash).
    let remoteName: String
    /// Goes up in this run's commit (false: the repo already has these bytes).
    let upload: Bool
}

/// What the run learned after the commit, all `deletable` looks at.
struct MoveFacts: Sendable, Equatable {
    /// Why the commit (or ref update) failed; nil when it landed or none was needed.
    var commitFailure: String?
    /// `notes/` of the new commit (or, with no commit, a fresh branch listing);
    /// nil when that read failed.
    var verified: [String: String]?
    var verifyFailure: String?
    /// Cap `fs.delete.if`.
    var canDelete: Bool
}

enum DeleteVerdict: Sendable, Equatable {
    case delete
    case keep(String)
}

extension NotesSyncService {
    static let keepCommitFailed = "commit failed"
    static let keepNotVerified = "not verified on GitHub"
    static let keepNoConditionalDelete = "update the pager firmware to remove synced notes"
    static let keepChanged = "changed on the pager"
    static let keepLinkDropped = "link dropped"
    static let keepCancelled = "cancelled"

    /// The one rule for deleting a note from the pager: only when the run's
    /// commit landed (or none was needed), the tree read back from GitHub has
    /// the note's path with exactly the blob SHA of the bytes pulled, and the
    /// pager can refuse a delete of a file that changed since (its own
    /// size + CRC check at delete time covers the rest).
    static func deletable(_ note: PulledNote, _ facts: MoveFacts) -> DeleteVerdict {
        if let failure = facts.commitFailure {
            return .keep("\(keepCommitFailed): \(failure)")
        }
        guard let verified = facts.verified else {
            return .keep("\(keepNotVerified): \(facts.verifyFailure ?? "read failed")")
        }
        guard verified[note.remoteName] == note.sha else { return .keep(keepNotVerified) }
        guard facts.canDelete else { return .keep(keepNoConditionalDelete) }
        return .delete
    }

    /// `<stem>~<sha[0..7]>.txt`: where a note goes when its name is taken by
    /// other bytes (D6). Depends only on the bytes, so a retry finds it.
    static func conflictName(_ name: String, sha: String) -> String {
        let tag = "~" + String(sha.prefix(7))
        if name.hasSuffix(".txt") { return String(name.dropLast(4)) + tag + ".txt" }
        return name + tag
    }
}
