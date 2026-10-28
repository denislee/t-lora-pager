import PagerKit
import SwiftUI

/// One note's text: monospaced and selectable, with its size, volume and
/// `mtime` underneath. Encrypted notes read only while the passphrase is held
/// (`NotesModel.text(for:)`); sharing the plaintext is an explicit action.
/// "Edit" turns the text into an editor; "Save" writes it back to the pager
/// (encrypted again when the note was), "Delete" removes the note from it.
struct NoteDetailView: View {
    let row: NotesModel.Row

    @EnvironmentObject private var notes: NotesModel
    @Environment(\.dismiss) private var dismiss
    @State private var state: LoadState = .loading
    @State private var editing = false
    @State private var draft = ""
    @State private var busy = false
    @State private var showDeleteConfirm = false
    @State private var showDiscardConfirm = false
    @State private var actionError: String?

    private enum LoadState {
        case loading
        case loaded(String)
        case locked
        case failed(String)
    }

    /// The row as the model has it now (size/preview change after a save).
    private var current: NotesModel.Row {
        notes.rows.first { $0.id == row.id } ?? row
    }

    private var loadedText: String? {
        if case .loaded(let text) = state { return text }
        return nil
    }

    var body: some View {
        VStack(spacing: 0) {
            content
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            Divider()
            footer
        }
        .navigationTitle(row.entry.name)
        .navigationBarTitleDisplayMode(.inline)
        .navigationBarBackButtonHidden(editing)
        .toolbar { toolbar }
        .disabled(busy)
        .overlay {
            if busy { ProgressView().controlSize(.large) }
        }
        .confirmationDialog("Delete this note from the pager?", isPresented: $showDeleteConfirm,
                            titleVisibility: .visible) {
            Button("Delete", role: .destructive) { Task { await deleteNote() } }
        } message: {
            Text("\(row.entry.name) is removed from the pager's internal storage and SD card. This cannot be undone.")
        }
        .confirmationDialog("Discard your changes?", isPresented: $showDiscardConfirm,
                            titleVisibility: .visible) {
            Button("Discard", role: .destructive) { editing = false }
        }
        .alert("Could not finish", isPresented: Binding(
            get: { actionError != nil }, set: { if !$0 { actionError = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(actionError ?? "")
        }
        .task { await load() }
    }

    // MARK: - Toolbar

    @ToolbarContentBuilder
    private var toolbar: some ToolbarContent {
        if editing {
            ToolbarItem(placement: .navigationBarLeading) {
                Button("Cancel") {
                    if draft == loadedText { editing = false } else { showDiscardConfirm = true }
                }
            }
            ToolbarItem(placement: .navigationBarTrailing) {
                Button("Save") { Task { await save() } }
                    .bold()
                    .disabled(draft == loadedText || !notes.canEdit(current))
            }
        } else if let text = loadedText {
            ToolbarItemGroup(placement: .navigationBarTrailing) {
                ShareLink(item: text)
                Menu {
                    Button {
                        draft = text
                        editing = true
                    } label: {
                        Label("Edit", systemImage: "pencil")
                    }
                    .disabled(!notes.canEdit(current))
                    Button(role: .destructive) {
                        showDeleteConfirm = true
                    } label: {
                        Label("Delete from pager", systemImage: "trash")
                    }
                    .disabled(!notes.canEdit(current))
                } label: {
                    Label("More", systemImage: "ellipsis.circle")
                }
            }
        }
    }

    // MARK: - Content

    @ViewBuilder
    private var content: some View {
        switch state {
        case .loading:
            ProgressView()
        case .loaded(let text):
            if editing {
                TextEditor(text: $draft)
                    .font(.system(.body, design: .monospaced))
                    .autocorrectionDisabled()
                    .padding(.horizontal, 8)
            } else {
                ScrollView {
                    Text(text)
                        .font(.system(.body, design: .monospaced))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding()
                }
            }
        case .locked:
            Text("Locked — unlock notes to read this one")
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding()
        case .failed(let message):
            VStack(spacing: 12) {
                Text("Could not read the note: \(message)")
                    .multilineTextAlignment(.center)
                Button("Retry") {
                    Task { await load() }
                }
            }
            .padding()
        }
    }

    private var footer: some View {
        Text(footerText)
            .font(.caption)
            .foregroundStyle(.secondary)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal)
            .padding(.vertical, 8)
    }

    private var footerText: String {
        let entry = current.entry
        let volume = entry.vol == "sd" ? "SD card" : "Internal"
        let date: String
        if let mtime = entry.mtime {
            date = Date(timeIntervalSince1970: TimeInterval(mtime))
                .formatted(date: .abbreviated, time: .shortened)
        } else {
            date = "date unknown"
        }
        let lock = entry.protected == true ? " · encrypted" : ""
        let offline = notes.connected ? "" : " · offline, read-only"
        return "\(entry.size) bytes · \(volume) · \(date)\(lock)\(offline)"
    }

    // MARK: - Actions

    private func load() async {
        state = .loading
        do {
            state = .loaded(try await notes.text(for: current))
        } catch let error as NoteCryptoError where error == .wrongPassphrase && current.locked {
            state = .locked
        } catch {
            state = .failed(NotesModel.describe(error))
        }
    }

    private func save() async {
        busy = true
        defer { busy = false }
        do {
            try await notes.save(current, text: draft)
            state = .loaded(draft)
            editing = false
        } catch {
            actionError = "Save failed: \(NotesModel.describe(error))"
        }
    }

    private func deleteNote() async {
        busy = true
        defer { busy = false }
        do {
            try await notes.delete(current)
            dismiss()
        } catch {
            actionError = "Delete failed: \(NotesModel.describe(error))"
        }
    }
}

#Preview {
    NoteDetailView(
        row: NotesModel.Row(
            entry: NoteEntry(vol: "int", name: "shopping.txt", size: 42),
            preview: "milk",
            locked: false
        )
    )
    .environmentObject(PagerSession().notes)
}
