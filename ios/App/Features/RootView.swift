import SwiftUI

/// The app's tabs: live device state, the pager's notes, the phone's
/// Telegram login, and the link log.
struct RootView: View {
    @EnvironmentObject private var session: PagerSession
    @EnvironmentObject private var notes: NotesModel
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        TabView {
            DashboardView()
                .tabItem {
                    Label("Dashboard", systemImage: "square.grid.2x2.fill")
                }
            // Needs the pager's file service ("fs"); offline, the cached list.
            if notes.pagerHasFS || notes.listedAt != nil {
                NotesView()
                    .tabItem {
                        Label("Notes", systemImage: "note.text")
                    }
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
            // Background locks the notes and stops a move (foreground only).
            if phase == .active || phase == .background { notes.setActive(phase == .active) }
        }
        .task { session.autoConnectIfNeeded() }
    }
}

#Preview {
    RootView()
        .environmentObject(PagerSession())
        .environmentObject(PagerSession().notes)
        .environmentObject(TelegramAccount())
}
