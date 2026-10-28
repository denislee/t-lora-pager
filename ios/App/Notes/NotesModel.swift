import Foundation
import PagerKit

/// The Notes tab's state (`IOS_NOTES_VIEWER_PLAN.md` §3): every note on the
/// pager in Journal order, readable text for plain notes and, after one
/// passphrase entry, for encrypted ones, and the "move to GitHub" sync.
///
/// Owned by `PagerSession`, which attaches the live link in `bringUp` and
/// detaches it in `disconnect`. The link-side work is `NotesLibrary` and
/// `NotesSyncService` (PagerKit, tested on Linux); this class only sequences
/// it for the UI.
///
/// Privacy: the passphrase and decrypted text live in memory only and are
/// dropped on `lock()` (also when the app goes to the background). The cache
/// holds raw bytes, so encrypted notes stay ciphertext on disk. Nothing here
/// logs note contents or the token.
@MainActor
final class NotesModel: ObservableObject {
    /// One list row.
    struct Row: Identifiable, Equatable {
        var entry: NoteEntry
        /// First non-empty line, once read (and decrypted).
        var preview: String?
        /// Encrypted and the passphrase is not entered yet.
        var locked: Bool

        var id: String { entry.id }
    }

    /// Where a phone-initiated move is.
    enum SyncStage: Equatable {
        case idle
        case listing
        case pulling(done: Int, total: Int)
        case committing
        case verifying
        case deleting(done: Int, total: Int)
    }

    @Published private(set) var rows: [Row] = []
    @Published private(set) var connected = false
    /// The pager advertised `"fs"` (the Notes tab needs it).
    @Published private(set) var pagerHasFS = false
    /// When the listing shown was taken (from the cache while offline).
    @Published private(set) var listedAt: Date?
    @Published private(set) var refreshing = false
    @Published private(set) var errorText: String?
    /// A passphrase is held in memory.
    @Published private(set) var unlocked = false
    @Published private(set) var syncStage: SyncStage = .idle
    /// The last finished phone-initiated move, for the result sheet.
    @Published var syncResult: NotesMoveResult?

    let account: GitHubAccountStore
    private let cache: NotesCache
    private let library: NotesLibrary
    private var sync: NotesSyncService?
    /// Every note on both volumes, SD notes shadowed by an internal one
    /// included: what a move takes (D7).
    private var allNotes: [NoteEntry] = []
    private var sdMounted = false
    private var decryptor: NoteDecryptor?
    /// Text of the notes read this session (plain or decrypted), by row id.
    /// Memory only.
    private var texts: [String: String] = [:]
    /// Size and CRC-32 of the bytes each text came from, by row id: what a
    /// save or delete checks the pager still holds.
    private var bases: [String: (size: UInt32, crc32: UInt32)] = [:]
    private var pagerCaps: [String] = []
    private var prefetchTask: Task<Void, Never>?
    private var syncTask: Task<Void, Never>?
    private var active = true
    /// Session-log sink (counts and stages only).
    var log: (String) -> Void = { _ in }

    nonisolated private static let ciphertextNoticeDefault = "Notes.ciphertextNoticeShown"

    init(account: GitHubAccountStore, cacheRoot: URL) {
        self.account = account
        let cache = NotesCache(root: cacheRoot)
        self.cache = cache
        self.library = NotesLibrary(cache: cache)
        if let cached = cache.cachedScan() {
            apply(cached.scan, at: cached.at)
        }
    }

    /// `Caches/notes` in the app container.
    static func defaultCacheRoot() -> URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("notes", isDirectory: true)
    }

    // MARK: - Session wiring (PagerSession)

    /// The link is up: list the notes. `sync` answers the pager's
    /// `notes.sync` too, and is the engine "Sync to GitHub" runs.
    func attach(_ session: LinkSession, sync: NotesSyncService, pagerCaps: [String]) async {
        self.sync = sync
        pagerHasFS = pagerCaps.contains("fs")
        self.pagerCaps = pagerCaps
        await library.attach(session)
        connected = true
        guard pagerHasFS else { return }
        await refresh()
    }

    /// The link dropped or was closed: keep showing the cached list.
    func detach() async {
        prefetchTask?.cancel()
        prefetchTask = nil
        syncTask?.cancel()
        sync = nil
        await library.attach(nil)
        connected = false
        if let cached = await library.cachedScan() {
            listedAt = cached.at
        }
    }

    /// `evt.fs.changed`: something on the pager changed; list again.
    func fsChanged() {
        Task {
            await library.fsChanged()
            await refresh()
        }
    }

    /// A pager-initiated move finished (its Notes Sync button).
    func pagerMoveFinished(_ result: NotesMoveResult) {
        log("notes.sync (pager): \(Self.counts(result))")
        Task { await refresh() }
    }

    /// Scene phase: leaving the foreground locks the notes, pauses
    /// prefetch, and cancels a move (before its commit that changes nothing;
    /// after it, the rest is finished by the next run).
    func setActive(_ isActive: Bool) {
        active = isActive
        guard !isActive else { return }
        lock()
        prefetchTask?.cancel()
        prefetchTask = nil
        if syncStage != .idle { cancelSync() }
    }

    // MARK: - Listing and reading

    /// Pull to refresh: `fs.list` both volumes (offline: the cached list).
    func refresh() async {
        guard connected, pagerHasFS else { return }
        refreshing = true
        defer { refreshing = false }
        do {
            let scan = try await library.refresh()
            errorText = nil
            apply(scan, at: Date())
            startPrefetch()
        } catch {
            errorText = "List failed: \(Self.describe(error))"
        }
    }

    /// Notes moved from SD in the last listing (for the confirm sheet).
    var sdNoteCount: Int { allNotes.filter { $0.vol == "sd" }.count }
    /// Notes a move would take.
    var movableCount: Int { allNotes.count }

    /// The note's full text for the detail view: plain text, or decrypted
    /// with the held passphrase. Throws `NoteCryptoError.wrongPassphrase`
    /// (locked or bad passphrase) or the read error (e.g. a CRC mismatch
    /// when the note changed between list and read: retry).
    func text(for row: Row) async throws -> String {
        if let text = texts[row.id] { return text }
        let bytes = try await library.read(row.entry)
        let encrypted = NoteCrypto.isEncrypted(bytes)
        update(row.id) { $0.entry.protected = encrypted }
        bases[row.id] = Self.base(of: bytes)
        let text = try await decode(bytes)
        texts[row.id] = text
        update(row.id) { current in
            current.preview = NotePreview.firstLine(text)
            current.locked = false
        }
        return text
    }

    /// Hold `passphrase` and decrypt every encrypted note available. The
    /// first successful decrypt validates it (the pager's canary is not
    /// readable over fs.*); false means "wrong passphrase".
    func unlock(passphrase: String) async -> Bool {
        let candidate = NoteDecryptor(passphrase: passphrase)
        // Check it against the first encrypted note that can be read. With
        // none readable it is held anyway; a later failure shows "wrong
        // passphrase".
        for row in rows where row.locked {
            guard let bytes = try? await library.read(row.entry) else { continue }
            guard let text = try? await Self.decrypt(bytes, with: candidate) else { return false }
            texts[row.id] = text
            break
        }
        decryptor = candidate
        unlocked = true
        Task { await decryptLockedRows() }
        return true
    }

    /// Forget the passphrase and every decrypted text.
    func lock() {
        decryptor = nil
        unlocked = false
        for row in rows where row.entry.protected == true || row.locked {
            texts[row.id] = nil
            update(row.id) { current in
                current.preview = nil
                current.locked = true
            }
        }
    }

    /// "Clear cached notes" in the GitHub/notes settings.
    func clearCache() {
        cache.wipe()
        if !connected {
            rows = []
            allNotes = []
            listedAt = nil
        }
    }

    // MARK: - Editing

    /// The note can be edited now: the link is up, no move runs, and an
    /// encrypted note has its passphrase.
    func canEdit(_ row: Row) -> Bool {
        connected && pagerHasFS && syncStage == .idle && !(row.locked || (row.entry.protected == true && decryptor == nil))
    }

    /// The pager deletes only a note that still matches (cap
    /// `fs.delete.if`).
    var pagerHasConditionalDelete: Bool { pagerCaps.contains(NotesSyncService.conditionalDeleteCap) }

    /// Write `text` back to the pager as the note's new content, encrypted
    /// again (fresh salt) when the note was. Every copy of the name (internal
    /// and SD) is rewritten. Throws `NotesLibrary.EditError.changedOnPager`
    /// when the note changed on the pager since it was read (nothing
    /// written: reopen it), or the link error.
    func save(_ row: Row, text: String) async throws {
        guard let base = bases[row.id] else { throw NotesLibrary.EditError.changedOnPager }
        let bytes: [UInt8]
        if row.entry.protected == true {
            guard let decryptor else { throw NoteCryptoError.wrongPassphrase }
            bytes = try await Task.detached(priority: .userInitiated) { try decryptor.encrypt(text) }.value
        } else {
            bytes = Array(text.utf8)
        }
        let mirrors = allNotes.filter { $0.name == row.entry.name && $0.id != row.id }
        let written = try await library.write(row.entry, mirrors: mirrors, bytes: bytes, base: base.crc32)
        log("notes: saved 1 note (\(written.count) copies)")
        for copy in written {
            texts[copy.id] = text
            bases[copy.id] = Self.base(of: bytes)
            if let index = allNotes.firstIndex(where: { $0.id == copy.id }) { allNotes[index] = copy }
            update(copy.id) { current in
                current.entry = copy
                current.preview = NotePreview.firstLine(text)
                current.locked = false
            }
        }
    }

    /// Delete the note from the pager (every copy of its name, as the
    /// pager's own delete does). With `fs.delete.if` a note changed on the
    /// pager since it was read is kept and this throws
    /// `NotesLibrary.EditError.changedOnPager`.
    func delete(_ row: Row) async throws {
        let copies = allNotes.filter { $0.name == row.entry.name }
        if bases[row.id] == nil {
            // Not read yet (a swipe on the list): the precondition is what the
            // pager holds now.
            bases[row.id] = Self.base(of: try await library.read(row.entry))
        }
        try await library.delete(copies.isEmpty ? [row.entry] : copies, base: bases[row.id],
                                 conditional: pagerHasConditionalDelete)
        log("notes: deleted 1 note")
        for copy in copies + [row.entry] {
            texts[copy.id] = nil
            bases[copy.id] = nil
        }
        rows.removeAll { $0.entry.name == row.entry.name }
        allNotes.removeAll { $0.name == row.entry.name }
    }

    // MARK: - Sync (move to GitHub)

    /// The first sync with encrypted notes shows the ciphertext notice once.
    var needsCiphertextNotice: Bool {
        rows.contains { $0.entry.protected == true || $0.locked }
            && !UserDefaults.standard.bool(forKey: Self.ciphertextNoticeDefault)
    }

    func ciphertextNoticeShown() {
        UserDefaults.standard.set(true, forKey: Self.ciphertextNoticeDefault)
    }

    var canSync: Bool { connected && account.isConnected && syncStage == .idle && sync != nil }

    /// Run the move over every note on the pager (after the confirm sheet).
    func startSync() {
        guard canSync, let sync else { return }
        let notes = allNotes
        prefetchTask?.cancel()
        prefetchTask = nil
        syncStage = .listing
        log("notes sync: moving \(notes.count) notes to \(account.description)")
        syncTask = Task { [weak self] in
            guard let self else { return }
            let result = await sync.move(notes) { step in
                await self.progress(step)
            }
            await self.syncFinished(result)
        }
    }

    func cancelSync() {
        syncTask?.cancel()
    }

    private func progress(_ step: NotesMoveProgress) async {
        switch step {
        case .listing: syncStage = .listing
        case .pulling(let done, let total): syncStage = .pulling(done: done, total: total)
        case .committing: syncStage = .committing
        case .verifying: syncStage = .verifying
        case .deleting(let done, let total): syncStage = .deleting(done: done, total: total)
        case .deleted(let vol, let name):
            await library.noteDeleted(vol: vol, name: name)
            let id = NoteEntry(vol: vol, name: name, size: 0).id
            texts[id] = nil
            rows.removeAll { $0.id == id }
            allNotes.removeAll { $0.id == id }
        }
    }

    private func syncFinished(_ result: NotesMoveResult) async {
        syncStage = .idle
        syncTask = nil
        syncResult = result
        log("notes sync: \(Self.counts(result))")
        await refresh()
    }

    /// `uploaded=… already=… deleted=… kept=… errors=…`
    nonisolated static func counts(_ result: NotesMoveResult) -> String {
        "uploaded=\(result.uploaded) already=\(result.already) deleted=\(result.deleted) "
            + "kept=\(result.kept.count) errors=\(result.errors.count)"
    }

    /// The commit on GitHub, for the result sheet's link.
    func commitURL(_ result: NotesMoveResult) -> URL? {
        guard let sha = result.commitSHA, let repo = account.repo else { return nil }
        return URL(string: "https://github.com/\(repo)/commit/\(sha)")
    }

    // MARK: - Helpers

    nonisolated private static func base(of bytes: [UInt8]) -> (size: UInt32, crc32: UInt32) {
        (size: UInt32(bytes.count), crc32: CRC32.checksum(bytes))
    }

    private func apply(_ scan: NotesIndexer.Scan, at date: Date) {
        allNotes = scan.all
        sdMounted = scan.sdMounted
        listedAt = date
        let previous = Dictionary(rows.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        rows = scan.merged.map { entry in
            var row = Row(entry: entry, preview: nil, locked: entry.protected == true && decryptor == nil)
            if let old = previous[entry.id], old.entry.size == entry.size {
                row.entry.mtime = entry.mtime ?? old.entry.mtime
                row.entry.protected = entry.protected ?? old.entry.protected
                row.preview = old.preview
                row.locked = old.locked && decryptor == nil
            } else {
                texts[entry.id] = nil
                bases[entry.id] = nil
            }
            return row
        }
    }

    /// Read and preview the listed notes one at a time, visible order,
    /// while the app is active and no move runs.
    private func startPrefetch() {
        prefetchTask?.cancel()
        let entries = rows.filter { $0.preview == nil }.map(\.entry)
        let library = self.library
        let shouldContinue: @Sendable () async -> Bool = { [weak self] in
            await self?.mayPrefetch() ?? false
        }
        let onRead: @Sendable (NoteEntry, [UInt8]) async -> Void = { [weak self] entry, bytes in
            await self?.prefetched(entry, bytes)
        }
        prefetchTask = Task {
            await library.prefetch(entries, shouldContinue: shouldContinue, onRead: onRead)
        }
    }

    private func mayPrefetch() -> Bool {
        active && syncStage == .idle
    }

    private func prefetched(_ entry: NoteEntry, _ bytes: [UInt8]) async {
        let encrypted = NoteCrypto.isEncrypted(bytes)
        bases[entry.id] = Self.base(of: bytes)
        update(entry.id) { row in
            row.entry.mtime = entry.mtime ?? row.entry.mtime
            row.entry.protected = encrypted
        }
        if encrypted && decryptor == nil {
            update(entry.id) { $0.locked = true }
            return
        }
        guard let text = try? await decode(bytes) else { return }
        texts[entry.id] = text
        update(entry.id) { row in
            row.preview = NotePreview.firstLine(text)
            row.locked = false
        }
    }

    private func decryptLockedRows() async {
        for row in rows where row.locked {
            guard decryptor != nil else { return }
            if let text = texts[row.id] {
                update(row.id) { current in
                    current.preview = NotePreview.firstLine(text)
                    current.locked = false
                }
                continue
            }
            guard let bytes = try? await library.read(row.entry),
                  let text = try? await decode(bytes) else { continue }
            texts[row.id] = text
            bases[row.id] = Self.base(of: bytes)
            update(row.id) { current in
                current.preview = NotePreview.firstLine(text)
                current.locked = false
            }
        }
    }

    /// Plain bytes as UTF-8 text; ciphertext through the held passphrase.
    private func decode(_ bytes: [UInt8]) async throws -> String {
        guard NoteCrypto.isEncrypted(bytes) else { return String(decoding: bytes, as: UTF8.self) }
        guard let decryptor else { throw NoteCryptoError.wrongPassphrase }
        return try await Self.decrypt(bytes, with: decryptor)
    }

    /// PBKDF2 + AES off the main actor.
    nonisolated private static func decrypt(_ bytes: [UInt8], with decryptor: NoteDecryptor) async throws -> String {
        try await Task.detached(priority: .userInitiated) {
            try decryptor.decrypt(bytes)
        }.value
    }

    private func update(_ id: String, _ change: (inout Row) -> Void) {
        guard let index = rows.firstIndex(where: { $0.id == id }) else { return }
        change(&rows[index])
    }

    nonisolated static func describe(_ error: Error) -> String {
        if let error = error as? RPCError { return error.msg.isEmpty ? error.code : "\(error.code): \(error.msg)" }
        if let error = error as? NotesLibrary.EditError, error == .changedOnPager {
            return "the note changed on the pager; reopen it and try again"
        }
        if let error = error as? TasksService.TasksError, error == .changedOnPager {
            return "the tasks changed on the pager; reload and try again"
        }
        if let error = error as? NoteCryptoError {
            return error == .wrongPassphrase ? "wrong passphrase" : "not a notes ciphertext (\(error))"
        }
        return "\(error)"
    }
}
