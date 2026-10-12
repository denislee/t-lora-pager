import SwiftUI

/// The app's tabs: live device state, the phone's Telegram login, and the link log.
struct RootView: View {
    var body: some View {
        TabView {
            DashboardView()
                .tabItem {
                    Label("Dashboard", systemImage: "gauge.with.dots.needle.50percent")
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
    }
}

#Preview {
    RootView()
        .environmentObject(PagerSession())
        .environmentObject(TelegramAccount())
}
