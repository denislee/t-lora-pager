import Foundation
import PagerKit
import WidgetKit
#if canImport(ActivityKit)
import ActivityKit
#endif

/// Shares the last-known pager state with the PagerWidgets extension (S16):
/// a `PagerStatusSnapshot` in the App Group's `UserDefaults`, written by
/// `PagerSession` on connect, `sys.info`, `evt.battery` and disconnect, and a
/// widget timeline reload after each write. Without the App Group (a build
/// whose signing has no group) the writes go nowhere and the widget shows
/// "No pager yet".
@MainActor
final class PagerStatusPublisher {
    /// The widget kind `PagerWidgets` registers (`StatusWidget.kind`).
    static let widgetKind = "PagerStatus"

    private let store: PagerStatusStore?
    private var snapshot: PagerStatusSnapshot

    init(bundleID: String? = Bundle.main.bundleIdentifier) {
        let group = PagerStatusStore.appGroupID(bundleID: bundleID ?? "")
        let store = UserDefaults(suiteName: group).map(PagerStatusStore.init(defaults:))
        self.store = store
        snapshot = store?.load() ?? PagerStatusSnapshot(link: .disconnected, updatedAt: Date())
    }

    func link(_ link: PagerStatusSnapshot.Link, name: String? = nil) {
        guard link != snapshot.link || (name != nil && name != snapshot.name) else { return }
        snapshot = snapshot.with(link: link, name: name, at: Date())
        publish()
    }

    func battery(_ sample: BatterySample) {
        guard sample.pct != snapshot.batteryPct || sample.charging != snapshot.charging else { return }
        snapshot = snapshot.with(battery: sample)
        publish()
    }

    private func publish() {
        guard let store else { return }
        store.save(snapshot)
        WidgetCenter.shared.reloadTimelines(ofKind: Self.widgetKind)
    }
}

/// The notes-sync Live Activity (S16): started on the first `evt.notes.sync`
/// of a pager-initiated job, updated on every one after, and ended (kept on
/// the Lock Screen for a minute) on `done:true`. iOS 16.1+; a no-op where
/// Live Activities are off or unavailable.
@MainActor
final class NotesSyncLiveActivity {
    /// The running activity's `id`. `Activity` is not `Sendable`, so only the
    /// id is kept here and each end/update looks the activity up off the main
    /// actor.
    private var currentID: String?
    private var job: Int?

    func handle(_ event: NotesSyncProgressEvent) {
        #if canImport(ActivityKit)
        if #available(iOS 16.1, *) { update(event) }
        #endif
    }

    #if canImport(ActivityKit)
    @available(iOS 16.1, *)
    private func update(_ event: NotesSyncProgressEvent) {
        let state = NotesSyncActivityAttributes.ContentState(
            phase: event.phaseLabel, summary: event.summary, uploaded: event.uploaded, already: event.already,
            deleted: event.deleted ?? 0, errors: event.errors, done: event.done)
        if let id = currentID, job == event.job {
            if event.done {
                currentID = nil
                job = nil
                Task.detached {
                    await Self.activity(id)?.end(using: state, dismissalPolicy: .after(Date().addingTimeInterval(60)))
                }
            } else {
                Task.detached { await Self.activity(id)?.update(using: state) }
            }
            return
        }
        if let stale = currentID {
            Task.detached { await Self.activity(stale)?.end(dismissalPolicy: .immediate) }
            currentID = nil
            job = nil
        }
        // A job that is already done (one message) is not worth an activity.
        guard !event.done, ActivityAuthorizationInfo().areActivitiesEnabled else { return }
        currentID = try? Activity.request(
            attributes: NotesSyncActivityAttributes(job: event.job, startedAt: Date()),
            contentState: state, pushType: nil).id
        job = event.job
    }

    @available(iOS 16.1, *)
    private nonisolated static func activity(_ id: String) -> Activity<NotesSyncActivityAttributes>? {
        Activity<NotesSyncActivityAttributes>.activities.first { $0.id == id }
    }
    #endif
}
