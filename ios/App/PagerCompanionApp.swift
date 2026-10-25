import SwiftUI

/// Pager Companion — the iOS half of the PagerLink v1 link.
///
/// The app owns one `PagerSession` (the observable state the UI reads, over
/// BLE or the emulator's TCP) and the phone's `TelegramAccount`, and injects
/// both into the view tree; see `IOS_COMPANION_PLAN.md`.
@main
struct PagerCompanionApp: App {
    @StateObject private var telegram: TelegramAccount
    @StateObject private var session: PagerSession

    init() {
        let telegram = TelegramAccount()
        _telegram = StateObject(wrappedValue: telegram)
        _session = StateObject(wrappedValue: PagerSession(telegram: telegram.native))
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(session)
                .environmentObject(telegram)
        }
    }
}
