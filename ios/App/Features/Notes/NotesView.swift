import PagerKit
import SwiftUI

/// The pager's notes: every note in Journal order, one passphrase entry for
/// the encrypted ones, and the move to GitHub (`IOS_NOTES_VIEWER_PLAN.md`
/// §3.4). State lives in `NotesModel`; this view only sequences it.
struct NotesView: View {
    @EnvironmentObject private var notes: NotesModel

    @State private var query = ""
    @State private var showSettings = false
    @State private var showUnlock = false
    @State private var showConfirm = false
    @State private var showCiphertextNotice = false
    @State private var shownResult: ResultBox?

    var body: some View {
        NavigationStack {
            List {
                progressSection
                offlineSection
                unlockSection
                notesSection
            }
            .navigationTitle("Notes")
            .searchable(text: $query, prompt: "Search notes")
            .refreshable { await notes.refresh() }
            .navigationDestination(for: String.self) { id in
                if let row = notes.rows.first(where: { $0.id == id }) {
                    NoteDetailView(row: row)
                }
            }
            .toolbar {
                if notes.unlocked {
                    ToolbarItem(placement: .navigationBarLeading) {
                        Button {
                            notes.lock()
                        } label: {
                            Label("Lock", systemImage: "lock")
                        }
                    }
                }
                ToolbarItemGroup(placement: .navigationBarTrailing) {
                    Button {
                        showSettings = true
                    } label: {
                        Label("GitHub settings", systemImage: "gearshape")
                    }
                    Button {
                        syncTapped()
                    } label: {
                        Label(
                            notes.account.isConnected ? "Sync to GitHub" : "Connect GitHub",
                            systemImage: "arrow.up.doc")
                    }
                    .disabled(!(notes.connected && notes.syncStage == .idle))
                }
            }
            .sheet(isPresented: $showSettings) {
                GitHubSettingsView()
                    .environmentObject(notes)
            }
            .sheet(isPresented: $showUnlock) {
                UnlockNotesView()
                    .environmentObject(notes)
            }
            .sheet(item: $shownResult) { box in
                SyncResultView(result: box.result)
                    .environmentObject(notes)
            }
            .alert("Encrypted notes", isPresented: $showCiphertextNotice) {
                Button("Continue") {
                    notes.ciphertextNoticeShown()
                    showConfirm = true
                }
                Button("Cancel", role: .cancel) {}
            } message: {
                Text(
                    "Encrypted notes are stored on GitHub as ciphertext. "
                        + "After they leave the pager, your notes passphrase is the only way to read them.")
            }
            .confirmationDialog(Text(confirmTitle), isPresented: $showConfirm, titleVisibility: .visible) {
                Button("Move to GitHub") { notes.startSync() }
                Button("Cancel", role: .cancel) {}
            } message: {
                Text("Each note is deleted from the pager only after GitHub confirms it.")
            }
            .onChange(of: notes.syncResult) { value in
                guard let value else { return }
                shownResult = ResultBox(result: value)
                notes.syncResult = nil
            }
        }
    }

    // MARK: - Sections

    @ViewBuilder
    private var progressSection: some View {
        if notes.syncStage != .idle {
            Section {
                HStack(spacing: 8) {
                    ProgressView()
                    Text(syncStageText)
                }
                Button("Cancel", role: .cancel) { notes.cancelSync() }
            }
        }
    }

    @ViewBuilder
    private var offlineSection: some View {
        if !notes.connected {
            Section {
                Label("Offline — last synced \(lastSyncedText)", systemImage: "wifi.slash")
                    .foregroundStyle(.secondary)
            }
        }
        if let error = notes.errorText {
            Section {
                Text(error).foregroundStyle(.red)
            }
        }
    }

    @ViewBuilder
    private var unlockSection: some View {
        if !notes.unlocked && notes.rows.contains(where: { $0.locked }) {
            Section {
                Button {
                    showUnlock = true
                } label: {
                    Label("Unlock notes", systemImage: "lock.open")
                }
            } footer: {
                Text(
                    "Encrypted notes stay locked until you enter the passphrase once. "
                        + "It is kept in memory only.")
            }
        }
    }

    @ViewBuilder
    private var notesSection: some View {
        if notes.rows.isEmpty {
            Section {
                Text(
                    notes.connected
                        ? "No notes on the pager."
                        : "Connect to the pager to see its notes."
                )
                .foregroundStyle(.secondary)
            }
        } else if filteredRows.isEmpty {
            Section {
                Text("No notes match \"\(query)\".")
                    .foregroundStyle(.secondary)
            }
        } else {
            Section {
                ForEach(filteredRows) { row in
                    NavigationLink(value: row.id) {
                        rowLabel(row)
                    }
                }
            }
        }
    }

    // MARK: - Rows

    private func rowLabel(_ row: NotesModel.Row) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(row.preview ?? row.entry.name)
                .lineLimit(1)
            HStack(spacing: 6) {
                Text(subtitle(row))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
                if row.entry.vol == "sd" {
                    sdBadge
                }
                if row.locked {
                    Image(systemName: "lock.fill")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
    }

    private var sdBadge: some View {
        Text("SD")
            .font(.caption2)
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .foregroundStyle(.secondary)
            .background(Color.secondary.opacity(0.15), in: Capsule())
    }

    /// `mtime` when the listing carried one, otherwise the file name.
    private func subtitle(_ row: NotesModel.Row) -> String {
        guard let mtime = row.entry.mtime else { return row.entry.name }
        return Date(timeIntervalSince1970: TimeInterval(mtime))
            .formatted(date: .abbreviated, time: .shortened)
    }

    private var filteredRows: [NotesModel.Row] {
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !needle.isEmpty else { return notes.rows }
        return notes.rows.filter { row in
            row.entry.name.localizedCaseInsensitiveContains(needle)
                || (row.preview?.localizedCaseInsensitiveContains(needle) ?? false)
        }
    }

    // MARK: - Sync

    private var lastSyncedText: String {
        guard let listedAt = notes.listedAt else { return "not yet" }
        return listedAt.formatted(date: .abbreviated, time: .shortened)
    }

    private var syncStageText: String {
        switch notes.syncStage {
        case .idle: return ""
        case .listing: return "Listing GitHub…"
        case .pulling(let done, let total): return "Pulled \(done)/\(total)"
        case .committing: return "Committing…"
        case .verifying: return "Verifying…"
        case .deleting(let done, let total): return "Deleting \(done)/\(total)"
        }
    }

    private var confirmTitle: String {
        let repo = notes.account.repo ?? "GitHub"
        return "Move \(notes.movableCount) notes (\(notes.sdNoteCount) on SD) "
            + "to \(repo)@\(notes.account.branch)?"
    }

    private func syncTapped() {
        guard notes.account.isConnected else {
            showSettings = true
            return
        }
        if notes.needsCiphertextNotice {
            showCiphertextNotice = true
        } else {
            showConfirm = true
        }
    }
}

/// `NotesMoveResult` is not `Identifiable`, so the result sheet gets one from
/// this box.
private struct ResultBox: Identifiable {
    let id = UUID()
    let result: NotesMoveResult
}

/// The passphrase prompt. One entry unlocks every encrypted note; the value is
/// never logged and is cleared as soon as it is submitted.
private struct UnlockNotesView: View {
    @EnvironmentObject private var notes: NotesModel
    @Environment(\.dismiss) private var dismiss

    @State private var passphrase = ""
    @State private var wrong = false
    @State private var busy = false

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    SecureField("Notes passphrase", text: $passphrase)
                        .textContentType(.password)
                    if wrong {
                        Text("Wrong passphrase")
                            .foregroundStyle(.red)
                    }
                } footer: {
                    Text(
                        "The passphrase is held in memory and dropped when the notes are "
                            + "locked or the app leaves the foreground.")
                }
                Section {
                    Button("Unlock") { unlock() }
                        .disabled(passphrase.isEmpty || busy)
                }
            }
            .navigationTitle("Unlock notes")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") {
                        passphrase = ""
                        dismiss()
                    }
                }
            }
            .interactiveDismissDisabled(busy)
        }
    }

    private func unlock() {
        let candidate = passphrase
        passphrase = ""
        wrong = false
        busy = true
        Task {
            let ok = await notes.unlock(passphrase: candidate)
            busy = false
            if ok {
                dismiss()
            } else {
                wrong = true
            }
        }
    }
}

/// What one move did: counts, the notes still on the pager with why, and the
/// commit on GitHub.
private struct SyncResultView: View {
    let result: NotesMoveResult

    @EnvironmentObject private var notes: NotesModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                if result.nothingToDo {
                    Section {
                        Text("Nothing to do: the pager has no notes to move.")
                    }
                } else {
                    Section {
                        LabeledContent("Uploaded", value: "\(result.uploaded)")
                        LabeledContent("Already on GitHub", value: "\(result.already)")
                        LabeledContent("Deleted from pager", value: "\(result.deleted)")
                        LabeledContent("Kept on pager", value: "\(result.kept.count)")
                        LabeledContent("Errors", value: "\(result.errors.count)")
                    } footer: {
                        if !result.canDelete {
                            Text("Update the pager firmware to remove synced notes.")
                        }
                    }
                }
                if !result.kept.isEmpty {
                    Section("Kept on the pager") {
                        ForEach(result.kept.indices, id: \.self) { index in
                            let issue = result.kept[index]
                            Text("\(issue.name) — \(issue.reason)")
                                .font(.callout)
                        }
                    }
                }
                if !result.errors.isEmpty {
                    Section("Errors") {
                        ForEach(result.errors.indices, id: \.self) { index in
                            let issue = result.errors[index]
                            Text("\(issue.name) — \(issue.reason)")
                                .font(.callout)
                                .foregroundStyle(.red)
                        }
                    }
                }
                if let url = notes.commitURL(result) {
                    Section {
                        Link("View commit on GitHub", destination: url)
                    }
                }
            }
            .navigationTitle("Sync result")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }
}

#Preview {
    NotesView()
        .environmentObject(PagerSession().notes)
}
