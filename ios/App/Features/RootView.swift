import SwiftUI

/// The app's tabs: live device state, the phone's Telegram login, and the link log.
struct RootView: View {
    @EnvironmentObject private var session: PagerSession
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        TabView {
            DashboardView()
                .tabItem {
                    Label("Dashboard", systemImage: "square.grid.2x2.fill")
                }
            TelegramView()
                .tabItem {
                    Label("Telegram", systemImage: "paperplane")
                }
            DiagnosticsView()
                .tabItem {
                    Label("Diagnostics", systemImage: "stethoscope")
                }
        }
        // Auto-connect (if enabled) on launch and on every return to the foreground.
        .onChange(of: scenePhase) { phase in
            if phase == .active { session.autoConnectIfNeeded() }
        }
        .task { session.autoConnectIfNeeded() }
    }
}

#Preview {
    RootView()
        .environmentObject(PagerSession())
        .environmentObject(TelegramAccount())
}
