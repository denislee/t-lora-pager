import SwiftUI

/// `sys.ping` round-trip and the live session log.
struct DiagnosticsView: View {
    @Environment(PagerSession.self) private var session

    var body: some View {
        NavigationStack {
            List {
                Section("Ping") {
                    Button("Ping") {
                        Task { await session.ping() }
                    }
                    .disabled(!isConnected)
                    LabeledContent("Last RTT", value: rttText)
                }
                Section("Log") {
                    if session.log.isEmpty {
                        Text("No log entries yet.")
                            .foregroundStyle(.secondary)
                    } else {
                        ForEach(Array(session.log.indices), id: \.self) { index in
                            Text(session.log[index])
                                .font(.system(.caption, design: .monospaced))
                                .textSelection(.enabled)
                        }
                    }
                }
            }
            .navigationTitle("Diagnostics")
        }
    }

    private var isConnected: Bool {
        if case .connected = session.state { return true }
        return false
    }

    private var rttText: String {
        guard let rtt = session.lastRTT else { return "—" }
        return PagerSession.describe(rtt)
    }
}

#Preview {
    DiagnosticsView()
        .environment(PagerSession())
}
