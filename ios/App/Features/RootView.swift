import SwiftUI

/// The app's two Phase-1 tabs: live device state and the link log.
struct RootView: View {
    var body: some View {
        TabView {
            DashboardView()
                .tabItem {
                    Label("Dashboard", systemImage: "gauge.with.dots.needle.50percent")
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
        .environment(PagerSession())
}
