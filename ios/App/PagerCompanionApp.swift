import SwiftUI

/// Pager Companion — the iOS half of the PagerLink v1 link.
///
/// The app owns one `PagerSession` (the observable state the UI reads, over
/// BLE or the emulator's TCP) and the phone's `TelegramAccount`, and injects
/// both — plus the session's per-tab models — into the view tree; see
/// `IOS_COMPANION_PLAN.md`.
@main
struct PagerCompanionApp: App {
    @StateObject private var telegram: TelegramAccount
    @StateObject private var session: PagerSession

    init() {
        let telegram = TelegramAccount()
        _telegram = StateObject(wrappedValue: telegram)
        _session = StateObject(wrappedValue: PagerSession(telegram: telegram.native))
        // Clip / find-phone notifications from the pager (Inbox).
        NotificationRouter.install()
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(session)
                .environmentObject(session.notes)
                .environmentObject(session.tasks)
                .environmentObject(session.settings)
                .environmentObject(session.files)
                .environmentObject(session.chat)
                .environmentObject(session.voiceMemos)
                .environmentObject(session.battery)
                .environmentObject(session.inbox)
                .environmentObject(session.firmware)
                .environmentObject(telegram)
        }
    }
}
