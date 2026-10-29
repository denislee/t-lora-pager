import PagerKit
import SwiftUI

/// The pager's voice memos (S8): each recording on the SD card with its
/// date and length, imported as text (Groq Whisper) into a new note or
/// today's journal entry, and optionally removed from the pager afterwards.
/// State lives in `VoiceMemosModel`.
struct VoiceMemosView: View {
    @EnvironmentObject private var memos: VoiceMemosModel
    @EnvironmentObject private var chat: ChatHistoryModel

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Picker("Save transcript as", selection: $memos.destination) {
                        ForEach(VoiceMemosModel.Destination.allCases) { destination in
                            Text(destination.rawValue).tag(destination)
                        }
                    }
                    Toggle("Delete from pager after import", isOn: $memos.deleteAfterImport)
                } footer: {
                    Text(chat.hasGroqKey
                         ? "Recordings are transcribed with Groq Whisper and saved like any note (encrypted when your notes are)."
                         : "Set a Groq API key in the Chat tab to transcribe recordings.")
                }
                statusSection
                Section("Recordings") {
                    if memos.loading && memos.memos.isEmpty {
                        ProgressView()
                    } else if memos.memos.isEmpty {
                        Text(memos.connected ? "No recordings on the pager." : "Connect to the pager to see its recordings.")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(memos.memos) { memo in
                        MemoRow(memo: memo, importing: memos.importing?.name == memo.name,
                                fraction: memos.importing?.fraction ?? 0)
                            .swipeActions(edge: .trailing) {
                                Button {
                                    Task { await memos.importMemo(memo) }
                                } label: {
                                    Label("Import", systemImage: "text.badge.plus")
                                }
                                .tint(.accentColor)
                                .disabled(!memos.canImport)
                            }
                            .contextMenu {
                                Button {
                                    Task { await memos.importMemo(memo) }
                                } label: {
                                    Label("Import as text", systemImage: "text.badge.plus")
                                }
                                .disabled(!memos.canImport)
                            }
                    }
                }
            }
            .navigationTitle("Voice memos")
            .refreshable { await memos.reload() }
        }
    }

    @ViewBuilder
    private var statusSection: some View {
        if let result = memos.lastResult {
            Section {
                Label(result, systemImage: "checkmark.circle")
                    .font(.footnote)
            }
        }
        if let error = memos.errorText {
            Section {
                Text(error)
                    .font(.footnote)
                    .foregroundStyle(.red)
            }
        }
    }
}

/// One recording: when, how long, and the pull progress while importing.
private struct MemoRow: View {
    let memo: VoiceMemo
    let importing: Bool
    let fraction: Double

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Image(systemName: "waveform")
                    .foregroundStyle(.secondary)
                Text(title)
                Spacer()
                Text(length)
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
            }
            if importing {
                ProgressView(value: fraction) {
                    Text(fraction < 1 ? "Pulling from the pager…" : "Transcribing…")
                        .font(.caption)
                }
            }
        }
    }

    private var title: String {
        guard let date = memo.recordedAt.flatMap({ Calendar.current.date(from: $0) }) else { return memo.name }
        return date.formatted(date: .abbreviated, time: .shortened)
    }

    private var length: String {
        let seconds = Int(memo.duration.rounded())
        return "\(seconds / 60):" + String(format: "%02d", seconds % 60)
    }
}

#Preview {
    let session = PagerSession()
    return VoiceMemosView()
        .environmentObject(session.voiceMemos)
        .environmentObject(session.chat)
}
