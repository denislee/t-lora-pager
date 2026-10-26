/// One note file on the pager (PROTOCOL.md §6.1): `/notes/<name>` on `vol`
/// ("int" or "sd"). `mtime` is nil until an fs.stat fills it (hardware
/// listings report 0); `protected`/`crc32` are nil until a read or stat tells.
public struct NoteEntry: Sendable, Equatable, Hashable, Codable, Identifiable {
    public var vol: String
    public var name: String
    public var size: UInt32
    public var mtime: Int64?
    public var protected: Bool?
    public var crc32: UInt32?

    public init(vol: String, name: String, size: UInt32,
                mtime: Int64? = nil, protected: Bool? = nil, crc32: UInt32? = nil) {
        self.vol = vol
        self.name = name
        self.size = size
        self.mtime = mtime
        self.protected = protected
        self.crc32 = crc32
    }

    /// "<vol>:<name>"
    public var id: String { "\(vol):\(name)" }

    /// "/notes/<name>"
    public var path: String { NotesIndexer.notesDir + "/" + name }
}

/// Mirrors the pager Journal's note list so both screens agree:
/// - `hw_get_preferred_txt_files_info` (src/hal/storage.cpp): non-directory
///   leaves of /notes whose name ends in ".txt" (case-sensitive,
///   `String::endsWith`), internal first, an SD name already present internally
///   dropped (internal wins);
/// - `refresh_journal_entries` (src/apps/ui_journal.cpp): drops
///   `notes_path_is_tasks_file` paths — which only match the root `/tasks.txt`,
///   so a `/notes/tasks.txt` stays listed (test_notes_path
///   `test_tasks_file_matches_root_only`) — then sorts by path descending as
///   `std::string` byte comparison.
public enum NotesIndexer {
    public static let notesDir = "/notes"

    /// The firmware's `list_files(..., ".txt")` filter.
    public static func isNoteFile(_ entry: FsEntry) -> Bool {
        !entry.dir && entry.name.hasSuffix(".txt")
    }

    /// Journal order: name descending by UTF-8 bytes (same as "notes/<name>"
    /// descending).
    public static func journalOrder(_ a: NoteEntry, _ b: NoteEntry) -> Bool {
        b.name.utf8.lexicographicallyPrecedes(a.name.utf8)
    }

    /// Note entries of one volume's listing, filtered, mtime 0 → nil, Journal
    /// order.
    public static func notes(from listing: [FsEntry], vol: String) -> [NoteEntry] {
        listing
            .filter(Self.isNoteFile)
            .map { entry in
                NoteEntry(
                    vol: vol,
                    name: entry.name,
                    size: entry.size,
                    mtime: entry.mtime == 0 ? nil : entry.mtime
                )
            }
            .sorted(by: Self.journalOrder)
    }

    /// The Journal's merged view: internal wins a name clash; Journal order.
    public static func merge(internal int: [NoteEntry], sd: [NoteEntry]) -> [NoteEntry] {
        let internalNames = Set(int.map(\.name))
        return (int + sd.filter { !internalNames.contains($0.name) })
            .sorted(by: Self.journalOrder)
    }

    public struct Scan: Sendable, Equatable {
        /// What the Journal shows.
        public var merged: [NoteEntry]
        /// Every note file on the pager, internal then SD (each in Journal
        /// order), including an SD note shadowed by an internal one of the same
        /// name — the sync moves those too.
        public var all: [NoteEntry]
        public var sdMounted: Bool

        public init(merged: [NoteEntry], all: [NoteEntry], sdMounted: Bool) {
            self.merged = merged
            self.all = all
            self.sdMounted = sdMounted
        }
    }

    /// fs.list both volumes. An internal or SD folder that does not exist
    /// (invalid_argument "not a directory") is empty; SD "sd not mounted" (or
    /// any other invalid_argument on sd, e.g. the card pulled mid-scan) means
    /// no SD notes and sdMounted false for "sd not mounted", true otherwise.
    /// Other errors (link drop, timeout, internal) throw.
    public static func scan(_ fs: FileService) async throws -> Scan {
        let internalNotes = notes(from: try await listNotes(fs, vol: "int"), vol: "int")

        var sdNotes: [NoteEntry] = []
        var sdMounted = true
        do {
            sdNotes = notes(from: try await fs.list(vol: "sd", path: notesDir), vol: "sd")
        } catch let error as RPCError where error.code == "invalid_argument" {
            sdMounted = !error.isSDNotMounted
        }

        return Scan(
            merged: merge(internal: internalNotes, sd: sdNotes),
            all: internalNotes + sdNotes,
            sdMounted: sdMounted
        )
    }

    /// One volume's /notes listing; a missing folder is empty, any other error
    /// passes through.
    private static func listNotes(_ fs: FileService, vol: String) async throws -> [FsEntry] {
        do {
            return try await fs.list(vol: vol, path: notesDir)
        } catch let error as RPCError where error.code == "invalid_argument" && error.msg == "not a directory" {
            return []
        }
    }
}
