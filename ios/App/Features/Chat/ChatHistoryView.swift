import PagerKit
import SwiftUI
import UIKit

/// The phone's copy of the pager's chats (S11): every `chat.ask` the phone
/// answered (text or voice transcript, the reply, failures) with new
/// conversations marked where the pager reset, searchable, copyable and
/// exportable as a text file. Also holds the Groq API key `ChatService` and
/// the voice-memo import use. State lives in `ChatHistoryModel`.
struct ChatHistoryView: View {
    @EnvironmentObject private var chat: ChatHistoryModel

    @State private var query = ""
    @State private var keyDraft = ""
    @State private var keyError: String?
    @State private var showClear = false
    @State private var exportURL: URL?

    var body: some View {
        NavigationStack {
            List {
                keySection
                let shown = chat.search(query)
                if shown.isEmpty {
                    Text(query.isEmpty ? "No chats yet. Ask from the pager's Chat app while the phone is connected."
                                       : "No matches.")
                        .foregroundStyle(.secondary)
                }
                ForEach(shown) { record in
                    ChatRecordRow(record: record)
                        .contextMenu {
                            Button {
                                UIPasteboard.general.string = Self.copyText(record)
                            } label: {
                                Label("Copy", systemImage: "doc.on.doc")
                            }
                        }
                }
            }
            .navigationTitle("Chat")
            .searchable(text: $query, prompt: "Search chats")
            .toolbar {
                ToolbarItemGroup(placement: .navigationBarTrailing) {
                    if let exportURL {
                        ShareLink(item: exportURL) {
                            Label("Export", systemImage: "square.and.arrow.up")
                        }
                    }
                    Button(role: .destructive) {
                        showClear = true
                    } label: {
                        Label("Clear history", systemImage: "trash")
                    }
                    .disabled(chat.records.isEmpty)
                }
            }
            .confirmationDialog("Clear the chat history on this phone?", isPresented: $showClear,
                                titleVisibility: .visible) {
                Button("Clear", role: .destructive) { chat.clear() }
            } message: {
                Text("The pager keeps its own last few turns; this only clears the phone's copy.")
            }
            .task(id: chat.records.count) {
                exportURL = chat.records.isEmpty ? nil : try? chat.exportFile(chat.records)
            }
        }
    }

    @ViewBuilder
    private var keySection: some View {
        Section {
            LabeledContent("Groq API key", value: chat.hasGroqKey ? "Set" : "Not set")
            SecureField("Paste a new key", text: $keyDraft)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            HStack {
                Button("Save key") { saveKey(keyDraft) }
                    .disabled(keyDraft.trimmingCharacters(in: .whitespaces).isEmpty)
                Spacer()
                if chat.hasGroqKey {
                    Button("Remove", role: .destructive) { saveKey("") }
                }
            }
            .buttonStyle(.borderless)
            if let keyError {
                Text(keyError)
                    .font(.footnote)
                    .foregroundStyle(.red)
            }
        } footer: {
            Text("The phone answers the pager's Chat with Groq (Whisper for voice, Llama for replies). "
                 + "The key stays in this phone's Keychain and never goes to the pager.")
        }
    }

    private func saveKey(_ key: String) {
        do {
            try chat.setGroqKey(key)
            keyDraft = ""
            keyError = nil
        } catch {
            keyError = "Could not store the key: \(error)"
        }
    }

    /// What "Copy" puts on the pasteboard: the question and the answer.
    private static func copyText(_ record: ChatRecord) -> String {
        switch record.kind {
        case .turn: return "\(record.prompt ?? "")\n\n\(record.reply ?? "")"
        case .error: return [record.prompt, record.error].compactMap { $0 }.joined(separator: "\n\n")
        case .reset: return ""
        }
    }
}

/// One record: the pager's prompt and the reply as bubbles, a failure in
/// red, or a "new conversation" divider for a reset.
private struct ChatRecordRow: View {
    let record: ChatRecord

    var body: some View {
        switch record.kind {
        case .reset:
            HStack {
                VStack { Divider() }
                Text("New conversation · \(stamp)")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .fixedSize()
                VStack { Divider() }
            }
        case .turn, .error:
            VStack(alignment: .leading, spacing: 6) {
                if let prompt = record.prompt {
                    HStack(alignment: .top, spacing: 6) {
                        Image(systemName: record.voice ? "mic.fill" : "text.bubble")
                            .foregroundStyle(.secondary)
                        Text(prompt)
                            .textSelection(.enabled)
                    }
                }
                if let reply = record.reply {
                    Text(reply)
                        .textSelection(.enabled)
                        .padding(8)
                        .background(Color.accentColor.opacity(0.12), in: RoundedRectangle(cornerRadius: 8))
                }
                if let error = record.error {
                    Label(error, systemImage: "exclamationmark.triangle")
                        .font(.footnote)
                        .foregroundStyle(.red)
                }
                Text(stamp)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
            .padding(.vertical, 2)
        }
    }

    private var stamp: String { record.at.formatted(date: .abbreviated, time: .shortened) }
}

#Preview {
    ChatHistoryView()
        .environmentObject(PagerSession().chat)
}
