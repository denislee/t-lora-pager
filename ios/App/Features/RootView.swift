import SwiftUI

/// The app's tabs, as `TabView` selection tags. iOS shows the first five
/// and puts the rest under "More", so the everyday ones come first and
/// Diagnostics last. Settings is the fifth when every tab shows (Dashboard,
/// Inbox, Notes, Files, Settings), so the Dashboard's "Open pager settings"
/// lands on it directly.
enum AppTab: Hashable {
    case dashboard
    case inbox
    case notes
    case files
    case settings
    case journal
    case tasks
    case weather
    case voice
    case chat
    case telegram
    case diagnostics
}

/// The app's tabs: live device state, the pager's notes, files, journal and
/// tasks, its settings, weather, voice memos, the chat history, the phone's
/// Telegram login, and the link log.
struct RootView: View {
    @EnvironmentObject private var session: PagerSession
    @EnvironmentObject private var notes: NotesModel
    @EnvironmentObject private var tasks: TasksModel
    @EnvironmentObject private var settings: SettingsModel
    @EnvironmentObject private var files: FilesModel
    @EnvironmentObject private var voiceMemos: VoiceMemosModel
    @Environment(\.scenePhase) private var scenePhase

    @State private var tab: AppTab = .dashboard

    var body: some View {
        TabView(selection: $tab) {
            DashboardView(selectedTab: $tab)
                .tabItem {
                    Label("Dashboard", systemImage: "square.grid.2x2.fill")
                }
                .tag(AppTab.dashboard)
            // Messages to and from the pager (caps "notify" / "text" / "clip").
            if session.has("notify") || session.has("text") || session.has("clip") {
                InboxView()
                    .tabItem {
                        Label("Inbox", systemImage: "tray")
                    }
                    .tag(AppTab.inbox)
            }
            // Needs the pager's file service ("fs"); offline, the cached list.
            if notes.pagerHasFS || notes.listedAt != nil {
                NotesView()
                    .tabItem {
                        Label("Notes", systemImage: "note.text")
                    }
                    .tag(AppTab.notes)
            }
            if files.pagerHasFS {
                FilesView()
                    .tabItem {
                        Label("Files", systemImage: "folder")
                    }
                    .tag(AppTab.files)
            }
            // Needs the pager's "settings" cap; offline, the last values
            // read-only.
            if settings.available {
                SettingsView()
                    .tabItem {
                        Label("Settings", systemImage: "gearshape")
                    }
                    .tag(AppTab.settings)
            }
            // The Journal is the notes by day, under More next to Tasks.
            if notes.pagerHasFS || notes.listedAt != nil {
                JournalView()
                    .tabItem {
                        Label("Journal", systemImage: "calendar")
                    }
                    .tag(AppTab.journal)
            }
            if tasks.pagerHasFS || tasks.loaded {
                TasksView()
                    .tabItem {
                        Label("Tasks", systemImage: "checklist")
                    }
                    .tag(AppTab.tasks)
            }
            WeatherView()
                .tabItem {
                    Label("Weather", systemImage: "cloud.sun")
                }
                .tag(AppTab.weather)
            if voiceMemos.pagerHasFS {
                VoiceMemosView()
                    .tabItem {
                        Label("Voice memos", systemImage: "waveform")
                    }
                    .tag(AppTab.voice)
            }
            ChatHistoryView()
                .tabItem {
                    Label("Chat", systemImage: "bubble.left.and.bubble.right")
                }
                .tag(AppTab.chat)
            TelegramView()
                .tabItem {
                    Label("Telegram", systemImage: "paperplane")
                }
                .tag(AppTab.telegram)
            DiagnosticsView()
                .tabItem {
                    Label("Diagnostics", systemImage: "stethoscope")
                }
                .tag(AppTab.diagnostics)
        }
        // Auto-connect (if enabled) on launch and on every return to the foreground; a live link re-sends the clock.
        .onChange(of: scenePhase) { phase in
            if phase == .active {
                session.autoConnectIfNeeded()
                session.resyncClockIfConnected(reason: "foreground")
                // Messages the share extension queued while the app was away.
                session.inbox.outboxChanged()
                Task { await session.flushOutbox() }
            }
            // Background locks the notes and stops a move (foreground only).
            if phase == .active || phase == .background { notes.setActive(phase == .active) }
        }
        .task { session.autoConnectIfNeeded() }
    }
}

#Preview {
    let session = PagerSession()
    return RootView()
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
        .environmentObject(TelegramAccount())
}
