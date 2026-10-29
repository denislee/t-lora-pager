import PagerKit
import SwiftUI
import UIKit

/// The Inbox tab (`IOS_COMPANION_PLAN.md` §6.1, PROTOCOL.md §6.5): write a
/// message to the pager (`notify.post`, with priority), type into the
/// pager's editor or focused field (`text.send`), paste to it (`clip.push`),
/// and the clips and find requests the pager sent. Each part shows only when
/// the pager's firmware has its cap. Fields are checked with the pager's own
/// rules (`MessagingRules`) before they are sent.
struct InboxView: View {
    @EnvironmentObject private var session: PagerSession
    @EnvironmentObject private var inbox: InboxModel

    @State private var title = ""
    @State private var body_ = ""
    @State private var prio: NotifyPriority = .normal
    @State private var typeText = ""
    @State private var target: TextTarget = .editor
    @State private var clipText = ""
    @State private var status: String?
    @State private var error: String?
    @State private var busy = false

    var body: some View {
        NavigationStack {
            Form {
                if let banner = inbox.banner {
                    Section { Label(banner, systemImage: "doc.on.clipboard").foregroundStyle(.green) }
                }
                if !connected {
                    Section { Text("Connect to the pager to send it messages.").foregroundStyle(.secondary) }
                }
                if session.has("notify") { composeSection }
                if session.has("text") { typeSection }
                if session.has("clip") { clipSection }
                if inbox.outboxCount > 0 {
                    Section {
                        Label("\(inbox.outboxCount) shared message(s) wait to be sent; they go out when the pager is connected.",
                              systemImage: "tray.and.arrow.up")
                            .font(.footnote)
                    }
                }
                if status != nil || error != nil {
                    Section {
                        if let status { Text(status).foregroundStyle(.green) }
                        if let error { Text(error).foregroundStyle(.red) }
                    }
                }
                receivedSection
            }
            .navigationTitle("Inbox")
            .disabled(busy)
        }
    }

    private var connected: Bool {
        if case .connected = session.state { return true }
        return false
    }

    // MARK: - Sections

    private var composeSection: some View {
        Section {
            TextField("Title", text: $title)
            TextField("Message (optional)", text: $body_, axis: .vertical)
                .lineLimit(2...6)
            Picker("Priority", selection: $prio) {
                Text("Low (inbox only)").tag(NotifyPriority.low)
                Text("Normal").tag(NotifyPriority.normal)
                Text("High (wakes the pager)").tag(NotifyPriority.high)
            }
            Button("Send to pager") { sendMessage() }
                .disabled(title.trimmingCharacters(in: .whitespaces).isEmpty)
        } header: {
            Text("Message")
        } footer: {
            Text(countLabel(title, max: MessagingRules.titleMax, label: "Title")
                 + " · " + countLabel(body_, max: MessagingRules.bodyMax, label: "message"))
        }
    }

    private var typeSection: some View {
        Section {
            TextField("Text", text: $typeText, axis: .vertical)
                .lineLimit(2...8)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            Picker("Into", selection: $target) {
                Text("Open note editor").tag(TextTarget.editor)
                Text("Focused field (SSH, Telegram…)").tag(TextTarget.kb)
            }
            Button("Type on pager") { typeOnPager() }
                .disabled(typeText.isEmpty)
        } header: {
            Text("Type on pager")
        } footer: {
            Text(countLabel(typeText, max: MessagingRules.textSendMax, label: "Text") + ". Nothing is stored on the pager.")
        }
    }

    private var clipSection: some View {
        Section {
            TextField("Text", text: $clipText, axis: .vertical)
                .lineLimit(2...8)
            Button("Use this phone's clipboard") {
                clipText = UIPasteboard.general.string ?? ""
            }
            Button("Paste to pager") { pasteToPager() }
                .disabled(clipText.isEmpty)
        } header: {
            Text("Paste to pager")
        } footer: {
            Text("Pasted at the cursor when a note is open on the pager, otherwise kept in its inbox.")
        }
    }

    @ViewBuilder
    private var receivedSection: some View {
        Section {
            if inbox.received.isEmpty {
                Text("Clips and find requests from the pager show up here.")
                    .foregroundStyle(.secondary)
            }
            ForEach(inbox.received) { item in
                VStack(alignment: .leading, spacing: 4) {
                    Label(item.kind == .clip ? "Clipboard from pager" : "Find phone",
                          systemImage: item.kind == .clip ? "doc.on.clipboard" : "iphone.radiowaves.left.and.right")
                        .font(.subheadline.weight(.semibold))
                    if let text = item.text {
                        Text(text)
                            .lineLimit(4)
                            .textSelection(.enabled)
                    }
                    Text(item.at.formatted(date: .omitted, time: .shortened))
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
                .swipeActions {
                    if item.kind == .clip {
                        Button("Copy") { inbox.copy(item) }
                            .tint(.accentColor)
                    }
                }
            }
        } header: {
            Text("From the pager")
        }
    }

    // MARK: - Actions

    private func countLabel(_ text: String, max: Int, label: String) -> String {
        "\(label) \(text.utf8.count)/\(max) bytes"
    }

    private func run(_ action: @escaping @MainActor () async throws -> String) {
        busy = true
        status = nil
        error = nil
        Task {
            do {
                status = try await action()
            } catch {
                self.error = NotesModel.describe(error)
            }
            busy = false
        }
    }

    private func sendMessage() {
        let body = body_.trimmingCharacters(in: .whitespacesAndNewlines)
        let message = NotifyMessage(
            title: title.trimmingCharacters(in: .whitespaces), body: body.isEmpty ? nil : body,
            id: NotifyMessage.newID(), src: "iPhone",
            ts: UInt32(clamping: Int(Date().timeIntervalSince1970)), prio: prio)
        run {
            let result = try await session.notifyPost(message)
            title = ""
            body_ = ""
            return result.dup ? "Already on the pager." : "Sent."
        }
    }

    private func typeOnPager() {
        let text = typeText
        let target = self.target
        run {
            let typed = try await session.textSend(text, target: target)
            typeText = ""
            return "Typed \(typed) bytes."
        }
    }

    private func pasteToPager() {
        let text = clipText
        run {
            let pasted = try await session.clipPush(text)
            return pasted ? "Pasted into the open note." : "Kept in the pager's inbox (no note open)."
        }
    }
}
