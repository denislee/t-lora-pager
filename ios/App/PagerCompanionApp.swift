import SwiftUI

/// Pager Companion — the iOS half of the PagerLink v1 link.
///
/// The app owns one `PagerSession` (the observable state the UI reads) and
/// injects it into the view tree. BLE transport and the remaining feature
/// screens land in later phases; see `IOS_COMPANION_PLAN.md`.
@main
struct PagerCompanionApp: App {
    @StateObject private var session = PagerSession()

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(session)
        }
    }
}
