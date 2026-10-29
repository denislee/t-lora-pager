import Foundation
import PagerKit

/// The Voice memos tab's state (S8): the pager's recordings
/// (`/mental_notes/*.wav` on the SD card), each importable as text —
/// pulled with `fs.read`, transcribed by Groq Whisper (the backend
/// `ChatService` uses), and saved as a new note or appended to today's
/// journal entry through `NotesModel` (so it is encrypted when the notes
/// are). "Delete from pager after import" is off by default; when on, the
/// recording goes only after the note is written, with the size + CRC-32
/// precondition when the pager has `fs.delete.if`.
///
/// Owned by `PagerSession`, attached per link like `TasksModel`.
@MainActor
final class VoiceMemosModel: ObservableObject {
    /// Where an imported transcript goes.
    enum Destination: String, CaseIterable, Identifiable {
        case note = "New note"
        case journal = "Append to today"

        var id: String { rawValue }
    }

    @Published private(set) var memos: [VoiceMemo] = []
    @Published private(set) var connected = false
    @Published private(set) var pagerHasFS = false
    @Published private(set) var loading = false
    /// The memo being imported and how much of it has been pulled.
    @Published private(set) var importing: (name: String, fraction: Double)?
    @Published private(set) var lastResult: String?
    @Published var errorText: String?
    @Published var deleteAfterImport = UserDefaults.standard.bool(forKey: VoiceMemosModel.deleteDefault) {
        didSet { UserDefaults.standard.set(deleteAfterImport, forKey: Self.deleteDefault) }
    }
    @Published var destination: Destination = .note

    private var importer: VoiceMemoImporter?
    private let notes: NotesModel
    private let groqKey: @Sendable () -> String?
    /// Session-log sink (names and counts, never transcripts).
    var log: (String) -> Void = { _ in }

    nonisolated private static let deleteDefault = "VoiceMemos.deleteAfterImport"

    init(notes: NotesModel, groqKey: @escaping @Sendable () -> String?) {
        self.notes = notes
        self.groqKey = groqKey
    }

    var canImport: Bool { connected && pagerHasFS && importing == nil && notes.canCreate }

    // MARK: - Session wiring (PagerSession)

    func attach(_ session: LinkSession, pagerCaps: [String]) async {
        pagerHasFS = pagerCaps.contains("fs")
        connected = true
        guard pagerHasFS else { return }
        let backend = GroqChatBackend(apiKey: groqKey)
        importer = VoiceMemoImporter(session: session, pagerCaps: pagerCaps) { wav in
            try await backend.transcribe(wav: wav)
        }
        await reload()
    }

    func detach() {
        importer = nil
        connected = false
    }

    /// `evt.fs.changed`: a new recording may be there.
    func fsChanged() {
        guard importing == nil else { return }
        Task { await reload(quiet: true) }
    }

    func reload(quiet: Bool = false) async {
        guard let importer else { return }
        if !quiet { loading = true }
        defer { loading = false }
        do {
            memos = try await importer.list()
            errorText = nil
        } catch let error as RPCError where error.isSDNotMounted {
            memos = []
            errorText = "No SD card in the pager: recordings live on the card."
        } catch {
            errorText = "List failed: \(NotesModel.describe(error))"
        }
    }

    // MARK: - Import

    func importMemo(_ memo: VoiceMemo) async {
        guard canImport, let importer else { return }
        guard !(groqKey() ?? "").isEmpty else {
            errorText = "Set a Groq API key under Chat to transcribe recordings."
            return
        }
        importing = (memo.name, 0)
        defer { importing = nil }
        let notes = self.notes
        let destination = self.destination
        do {
            let outcome = try await importer.importMemo(
                memo, deleteAfterImport: deleteAfterImport,
                progress: { [weak self] done, total in
                    Task { @MainActor in
                        guard let self, self.importing?.name == memo.name else { return }
                        self.importing = (memo.name, total == 0 ? 1 : Double(done) / Double(total))
                    }
                },
                writeNote: { name, text in
                    switch destination {
                    case .note:
                        return try await notes.createNote(text: text, preferredName: name)
                    case .journal:
                        try await notes.appendToToday(text)
                        return "today's entry"
                    }
                })
            var line = "Saved \(memo.name) to \(outcome.noteName)."
            if outcome.deleted {
                line += " Removed from the pager."
            } else if let reason = outcome.deleteSkipped {
                line += " Kept on the pager (\(reason))."
            }
            lastResult = line
            errorText = nil
            log("voice: imported 1 memo\(outcome.deleted ? ", deleted on pager" : "")")
        } catch {
            errorText = "Import failed: \(NotesModel.describe(error))"
        }
        await reload(quiet: true)
    }
}
