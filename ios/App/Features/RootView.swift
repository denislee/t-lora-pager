import SwiftUI

/// The app's tabs: live device state, the pager's notes and tasks, its
/// settings, the phone's Telegram login, and the link log.
struct RootView: View {
    @EnvironmentObject private var session: PagerSession
    @EnvironmentObject private var notes: NotesModel
    @EnvironmentObject private var tasks: TasksModel
    @EnvironmentObject private var settings: SettingsModel
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
            if tasks.pagerHasFS || tasks.loaded {
                TasksView()
                    .tabItem {
                        Label("Tasks", systemImage: "checklist")
                    }
            }
            TelegramView()
                .tabItem {
                    Label("Telegram", systemImage: "paperplane")
                }
            // Needs the pager's "settings" cap; offline, the last values
            // read-only.
            if settings.available {
                SettingsView()
                    .tabItem {
                        Label("Settings", systemImage: "gearshape")
                    }
            }
            DiagnosticsView()
                .tabItem {
                    Label("Diagnostics", systemImage: "stethoscope")
                }
        }
        // Auto-connect (if enabled) on launch and on every return to the foreground; a live link re-sends the clock.
        .onChange(of: scenePhase) { phase in
            if phase == .active {
                session.autoConnectIfNeeded()
                session.resyncClockIfConnected(reason: "foreground")
            }
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
        .environmentObject(PagerSession().tasks)
        .environmentObject(PagerSession().settings)
        .environmentObject(TelegramAccount())
}
