import SwiftUI

/// `sys.ping` round-trip and the live session log.
struct DiagnosticsView: View {
    @EnvironmentObject private var session: PagerSession

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
                    LogRows(log: session.log)
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

/// The log lines, observed on their own (`SessionLog`), keyed by stable ids.
private struct LogRows: View {
    @ObservedObject var log: SessionLog

    var body: some View {
        if log.lines.isEmpty {
            Text("No log entries yet.")
                .foregroundStyle(.secondary)
        } else {
            ForEach(log.lines) { line in
                Text(line.text)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
            }
        }
    }
}

#Preview {
    DiagnosticsView()
        .environmentObject(PagerSession())
}
