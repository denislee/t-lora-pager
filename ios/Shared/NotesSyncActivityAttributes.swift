// Compiled into both the app and the PagerWidgets extension (project.yml),
// so the Live Activity the app starts and the UI the extension draws share
// one type.
#if canImport(ActivityKit) && os(iOS)
import ActivityKit
import Foundation

/// The notes-sync Live Activity (S16): one per pager-initiated sync job,
/// updated from every `evt.notes.sync` the phone sends the pager.
@available(iOS 16.1, *)
struct NotesSyncActivityAttributes: ActivityAttributes, Sendable {
    struct ContentState: Codable, Hashable, Sendable {
        /// "Uploading", "Verifying", "Done", … (`NotesSyncProgressEvent.phaseLabel`).
        var phase: String
        /// "2 up · 1 already · 2 moved" (`NotesSyncProgressEvent.summary`).
        var summary: String
        var uploaded: Int
        var already: Int
        var deleted: Int
        var errors: Int
        var done: Bool
    }

    /// The pager's job id.
    var job: Int
    var startedAt: Date
}
#endif
