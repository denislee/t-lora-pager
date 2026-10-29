import CoreTransferable
import PagerKit
import SwiftUI
import UIKit
import UniformTypeIdentifiers

/// `sys.ping` round-trip, the diagnostics export (one JSON file: app
/// version, last `sys.hello` / `sys.info`, battery summary, session log),
/// the battery chart and the live session log.
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
                if session.has("screen") || session.lastScreenshot != nil {
                    ScreenCaptureSection()
                }
                if session.has("ota") {
                    Section {
                        NavigationLink("Firmware update") { FirmwareView() }
                    }
                }
                Section {
                    ShareLink(item: DiagnosticsExport(session: session),
                              preview: SharePreview("Pager diagnostics")) {
                        Label("Export diagnostics", systemImage: "square.and.arrow.up")
                    }
                    NavigationLink("Battery history") {
                        BatteryHistoryView()
                    }
                } footer: {
                    Text("A JSON file with the app version, firmware hash and caps, the last sys.info, "
                         + "a battery summary, the latest screen capture and this log. "
                         + "The log holds counts and ids, never note text or keys.")
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
    let session = PagerSession()
    return DiagnosticsView()
        .environmentObject(session)
        .environmentObject(session.battery)
        .environmentObject(session.firmware)
}

/// The diagnostics file for the share sheet, built when it is shared (so it
/// has the log up to that moment) as `DiagnosticsReport` JSON.
struct DiagnosticsExport: Transferable {
    let session: PagerSession

    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(exportedContentType: .json) { export in
            let report = await MainActor.run { export.session.diagnosticsReport() }
            let url = FileManager.default.temporaryDirectory.appendingPathComponent(report.fileName)
            try report.encoded().write(to: url, options: .atomic)
            return SentTransferredFile(url)
        }
    }
}

/// Screen capture (S13, PROTOCOL.md §6.6): grab the pager's screen as a PNG,
/// show it, share it; the latest one also goes into the diagnostics export.
private struct ScreenCaptureSection: View {
    @EnvironmentObject private var session: PagerSession
    @State private var busy = false
    @State private var error: String?
    @State private var fileURL: URL?

    var body: some View {
        Section {
            Button {
                capture()
            } label: {
                Label(busy ? "Capturing…" : "Capture pager screen", systemImage: "camera.viewfinder")
            }
            .disabled(busy || !session.has("screen"))
            if let shot = session.lastScreenshot {
                Image(uiImage: shot.image)
                    .resizable()
                    .interpolation(.none)
                    .aspectRatio(contentMode: .fit)
                    .accessibilityLabel("Pager screen \(shot.width) by \(shot.height)")
                HStack {
                    Text("\(shot.width)×\(shot.height) · \(shot.capturedAt.formatted(date: .omitted, time: .standard))")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer()
                    if let fileURL {
                        ShareLink(item: fileURL) { Label("Share", systemImage: "square.and.arrow.up") }
                    }
                }
            }
            if let error {
                Text(error).font(.footnote).foregroundStyle(.red)
            }
        } header: {
            Text("Screen")
        }
        .task(id: session.lastScreenshot?.id) {
            fileURL = try? session.lastScreenshot?.fileURL()
        }
    }

    private func capture() {
        busy = true
        error = nil
        Task {
            do {
                try await session.captureScreen()
            } catch {
                self.error = "Capture failed: \(NotesModel.describe(error))"
            }
            busy = false
        }
    }
}
