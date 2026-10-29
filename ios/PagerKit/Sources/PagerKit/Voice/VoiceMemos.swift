import Foundation

/// One recording of the pager's voice-memo app (`src/apps/ui_audio_notes.cpp`):
/// `/mental_notes/note_YYYYMMDD_HHMMSS.wav` on the SD card, a 44-byte WAV
/// header then 16 kHz / 16-bit mono PCM (`HW_REC_SAMPLE_RATE`,
/// `src/hal/audio.h`).
public struct VoiceMemo: Sendable, Equatable, Identifiable, Hashable {
    public let name: String
    public let size: UInt32

    public var id: String { name }
    public var path: String { VoiceMemos.directory + "/" + name }

    public init(name: String, size: UInt32) {
        self.name = name
        self.size = size
    }

    /// When it was recorded (pager local time, from the name).
    public var recordedAt: DateComponents? { VoiceMemos.timestamp(ofMemoName: name) }

    /// Seconds of audio, as the pager computes it: size minus the 44-byte
    /// header over 32 000 bytes per second.
    public var duration: TimeInterval {
        Double(size > VoiceMemos.headerBytes ? size - VoiceMemos.headerBytes : 0) / Double(VoiceMemos.bytesPerSecond)
    }
}

/// Where and how the pager stores voice memos, and how an imported one
/// becomes a note.
public enum VoiceMemos {
    public static let volume = "sd"
    public static let directory = "/mental_notes"
    public static let headerBytes: UInt32 = 44
    public static let bytesPerSecond: UInt32 = 16_000 * 2
    /// Groq's Whisper upload limit (25 MB); about 13 minutes at the pager's rate.
    public static let maxUploadBytes: UInt32 = 25 * 1024 * 1024

    /// The recordings in a `/mental_notes` listing: files ending in `.wav`
    /// (case-sensitive, like `hw_list_sd_entries(…, ".wav", …)`), newest
    /// first by name.
    public static func memos(from listing: [FsEntry]) -> [VoiceMemo] {
        listing
            .filter { !$0.dir && $0.name.hasSuffix(".wav") }
            .map { VoiceMemo(name: $0.name, size: $0.size) }
            .sorted { $1.name.utf8.lexicographicallyPrecedes($0.name.utf8) }
    }

    /// `note_YYYYMMDD_HHMMSS.wav` → its local timestamp.
    public static func timestamp(ofMemoName name: String) -> DateComponents? {
        guard name.hasPrefix("note_") else { return nil }
        return Journal.timestamp(ofNoteName: String(name.dropFirst(5)))
    }

    /// The note an imported memo becomes: `YYYYMMDD_HHMMSS.txt` from the
    /// recording time, so the Journal files it under the day it was spoken
    /// (`fallback`'s time when the name has none).
    public static func noteName(for memo: VoiceMemo, fallback: Date, calendar: Calendar = .current) -> String {
        guard let t = memo.recordedAt, let y = t.year, let mo = t.month, let d = t.day,
              let h = t.hour, let mi = t.minute, let s = t.second else {
            return Journal.noteName(for: fallback, calendar: calendar)
        }
        return String(format: "%04d%02d%02d_%02d%02d%02d.txt", y, mo, d, h, mi, s)
    }

    /// The note's text: a header line naming the recording, a blank line,
    /// then the transcript.
    public static func noteText(for memo: VoiceMemo, transcript: String) -> String {
        let seconds = Int(memo.duration.rounded())
        return "Voice memo \(memo.name) (\(seconds / 60):\(String(format: "%02d", seconds % 60)))\n\n"
            + transcript.trimmingCharacters(in: .whitespacesAndNewlines) + "\n"
    }
}

/// Pulls voice memos off the pager, transcribes them and saves each as a
/// note (S8). The transcriber is Groq Whisper (`GroqChatBackend.transcribe`,
/// the same path `ChatService` uses for `chat.ask`); writing the note is the
/// caller's (the app writes it the way the Notes tab does, encrypted when the
/// notes are). After a saved note, `deleteAfterImport` removes the recording
/// from the pager — with the size + CRC-32 precondition when the pager has
/// cap `fs.delete.if`, so a file that changed since the pull stays.
public actor VoiceMemoImporter {
    /// What one import did.
    public struct Outcome: Sendable, Equatable {
        public let memo: VoiceMemo
        public let transcript: String
        /// The note's leaf name as written.
        public let noteName: String
        /// The recording was removed from the pager.
        public let deleted: Bool
        /// Why a requested delete did not happen (`changed`, an RPC error).
        public let deleteSkipped: String?
    }

    /// Why an import stopped before anything was written.
    public enum ImportError: Error, Equatable, Sendable {
        /// Larger than Whisper takes (`VoiceMemos.maxUploadBytes`).
        case tooLarge
        /// Whisper heard nothing.
        case emptyTranscript
    }

    public typealias Transcriber = @Sendable (_ wav: [UInt8]) async throws -> String
    /// Writes the note text under the suggested leaf name; returns the name
    /// actually written.
    public typealias NoteWriter = @Sendable (_ name: String, _ text: String) async throws -> String

    private let session: LinkSession
    private let transcribe: Transcriber
    private let timeout: Duration
    private let conditionalDelete: Bool
    private let now: @Sendable () -> Date

    /// `pagerCaps` decides whether deletes carry the precondition.
    public init(session: LinkSession, pagerCaps: [String], timeout: Duration = .seconds(60),
                now: @escaping @Sendable () -> Date = Date.init, transcribe: @escaping Transcriber) {
        self.session = session
        self.transcribe = transcribe
        self.timeout = timeout
        self.conditionalDelete = pagerCaps.contains(NotesSyncService.conditionalDeleteCap)
        self.now = now
    }

    /// The recordings on the SD card, newest first. A missing folder (no
    /// recording yet) is an empty list; a card that is out throws
    /// `RPCError.isSDNotMounted`.
    public func list() async throws -> [VoiceMemo] {
        do {
            let listing = try await FileService(session: session, timeout: timeout)
                .list(vol: VoiceMemos.volume, path: VoiceMemos.directory)
            return VoiceMemos.memos(from: listing)
        } catch let error as RPCError where error.code == "invalid_argument"
            && (error.msg == "not found" || error.msg == "not a directory") {
            return []
        }
    }

    /// Pull `memo`, transcribe it, write the note with `writeNote`, then
    /// (with `deleteAfterImport`) delete the recording. Nothing is deleted
    /// unless the note was written. `progress` sees the pull.
    public func importMemo(_ memo: VoiceMemo, deleteAfterImport: Bool,
                           progress: (@Sendable (_ done: UInt32, _ total: UInt32) -> Void)? = nil,
                           writeNote: NoteWriter) async throws -> Outcome {
        guard memo.size <= VoiceMemos.maxUploadBytes else { throw ImportError.tooLarge }
        let (wav, info) = try await session.readFile(vol: VoiceMemos.volume, path: memo.path,
                                                     timeout: timeout, progress: progress)
        let transcript = try await transcribe(wav).trimmingCharacters(in: .whitespacesAndNewlines)
        guard !transcript.isEmpty else { throw ImportError.emptyTranscript }
        let name = try await writeNote(VoiceMemos.noteName(for: memo, fallback: now()),
                                       VoiceMemos.noteText(for: memo, transcript: transcript))
        guard deleteAfterImport else {
            return Outcome(memo: memo, transcript: transcript, noteName: name, deleted: false, deleteSkipped: nil)
        }
        let fs = FileService(session: session, timeout: timeout)
        do {
            if conditionalDelete {
                try await fs.delete(vol: VoiceMemos.volume, path: memo.path, ifSize: info.size, ifCRC: info.crc32)
            } else {
                try await fs.delete(vol: VoiceMemos.volume, path: memo.path)
            }
            return Outcome(memo: memo, transcript: transcript, noteName: name, deleted: true, deleteSkipped: nil)
        } catch let error as RPCError {
            let reason = error.isChanged ? "changed on the pager" : (error.msg.isEmpty ? error.code : error.msg)
            return Outcome(memo: memo, transcript: transcript, noteName: name, deleted: false, deleteSkipped: reason)
        }
    }
}
