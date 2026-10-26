import PagerKit
import SwiftUI

/// One note's text: monospaced and selectable, with its size, volume and
/// `mtime` underneath. Encrypted notes read only while the passphrase is held
/// (`NotesModel.text(for:)`); sharing the plaintext is an explicit action.
struct NoteDetailView: View {
    let row: NotesModel.Row

    @EnvironmentObject private var notes: NotesModel
    @State private var state: LoadState = .loading

    private enum LoadState {
        case loading
        case loaded(String)
        case locked
        case failed(String)
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
        .toolbar {
            if case .loaded(let text) = state {
                ToolbarItem(placement: .navigationBarTrailing) {
                    ShareLink(item: text)
                }
            }
        }
        .task { await load() }
    }

    // MARK: - Content

    @ViewBuilder
    private var content: some View {
        switch state {
        case .loading:
            ProgressView()
        case .loaded(let text):
            ScrollView {
                Text(text)
                    .font(.system(.body, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding()
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
        let volume = row.entry.vol == "sd" ? "SD card" : "Internal"
        let date: String
        if let mtime = row.entry.mtime {
            date = Date(timeIntervalSince1970: TimeInterval(mtime))
                .formatted(date: .abbreviated, time: .shortened)
        } else {
            date = "date unknown"
        }
        return "\(row.entry.size) bytes · \(volume) · \(date)"
    }

    // MARK: - Loading

    private func load() async {
        state = .loading
        do {
            state = .loaded(try await notes.text(for: row))
        } catch let error as NoteCryptoError where error == .wrongPassphrase && row.locked {
            state = .locked
        } catch {
            state = .failed(NotesModel.describe(error))
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
