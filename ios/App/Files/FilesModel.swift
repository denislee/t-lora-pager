import Foundation
import PagerKit

/// The Files tab's state (`IOS_COMPANION_PLAN.md` §6.1 "Files"): one folder
/// of the pager's internal flash or SD card at a time, with its breadcrumbs,
/// the lock badge for `protected` files, folder changes, and downloads and
/// uploads with progress. The link-side work is PagerKit's `FileBrowser`
/// (tested on Linux); this class only sequences it for the UI.
///
/// Owned by `PagerSession`, which attaches the live link in `bringUp`,
/// forwards `evt.fs.changed`, and detaches in `disconnect`. An upload cut
/// off by a dropped link is sent again from byte 0 when the next link comes
/// up (PagerLink v1 has no resume; see `FileBrowser`).
///
/// Privacy: encrypted files are never decrypted here (Notes owns that);
/// downloads go to a temporary folder that is emptied on every launch and
/// when the tab asks; the session log gets names and sizes, never contents.
@MainActor
final class FilesModel: ObservableObject {
    /// The two pager volumes.
    enum Volume: String, CaseIterable, Identifiable {
        case int
        case sd

        var id: String { rawValue }
        var title: String { self == .int ? "Internal" : "SD card" }
    }

    /// One row of the listing.
    struct Row: Identifiable, Equatable {
        let entry: FsEntry
        /// `fs.stat` says `Salted__`: shown with a lock, never decrypted.
        var protected: Bool

        var id: String { entry.name }
    }

    /// A transfer in flight, for the progress bar.
    struct Transfer: Equatable {
        enum Direction { case download, upload }
        let direction: Direction
        let name: String
        var done: UInt32
        var total: UInt32

        var fraction: Double { total == 0 ? 1 : Double(done) / Double(total) }
    }

    /// A finished download ready for Quick Look / the text preview / Share.
    struct Downloaded: Identifiable, Equatable {
        let id = UUID()
        let name: String
        let url: URL
        let kind: FilePreviewKind
        let crc32: UInt32
    }

    @Published var volume: Volume = .int {
        didSet { if volume != oldValue { open(FilePath.root) } }
    }
    @Published private(set) var path = FilePath.root
    @Published private(set) var rows: [Row] = []
    @Published private(set) var connected = false
    /// The pager advertised `"fs"` (the Files tab needs it).
    @Published private(set) var pagerHasFS = false
    @Published private(set) var loading = false
    @Published private(set) var transfer: Transfer?
    @Published private(set) var interruptedCount = 0
    @Published var downloaded: Downloaded?
    @Published var errorText: String?

    private let browser = FileBrowser(timeout: .seconds(30))
    private var listTask: Task<Void, Never>?
    private var statTask: Task<Void, Never>?
    /// Session-log sink (names and sizes only).
    var log: (String) -> Void = { _ in }

    init() {
        Self.clearDownloads()
    }

    var crumbs: [FilePath.Crumb] { FilePath.crumbs(path, rootName: volume.title) }
    var canEdit: Bool { connected && pagerHasFS && transfer == nil }

    // MARK: - Session wiring (PagerSession)

    func attach(_ session: LinkSession, pagerCaps: [String]) async {
        pagerHasFS = pagerCaps.contains("fs")
        connected = true
        await browser.attach(session)
        guard pagerHasFS else { return }
        await reload()
        await retryInterrupted()
    }

    func detach() async {
        listTask?.cancel()
        statTask?.cancel()
        await browser.attach(nil)
        connected = false
    }

    /// `evt.fs.changed`: drop every cached listing and re-read the open folder
    /// (an editor save on the pager shows up here at once).
    func fsChanged() {
        Task {
            await browser.invalidate()
            await reload(quiet: true)
        }
    }

    // MARK: - Navigation

    /// Open directory `path` on the current volume.
    func open(_ path: String) {
        self.path = FilePath.normalize(path)
        rows = []
        listTask?.cancel()
        listTask = Task { await reload() }
    }

    /// Pull to refresh: list the folder again from the pager.
    func reload(quiet: Bool = false, force: Bool = false) async {
        guard connected, pagerHasFS else { return }
        let vol = volume.rawValue
        let dir = path
        if !quiet { loading = true }
        defer { loading = false }
        do {
            let entries = try await browser.list(vol: vol, path: dir, force: force)
            guard vol == volume.rawValue, dir == path else { return }  // navigated meanwhile
            let previous = Dictionary(rows.map { ($0.id, $0.protected) }, uniquingKeysWith: { a, _ in a })
            rows = entries.map { Row(entry: $0, protected: previous[$0.name] ?? false) }
            errorText = nil
            statFiles(vol: vol, dir: dir)
        } catch let error as RPCError where error.isSDNotMounted {
            rows = []
            errorText = "No SD card in the pager."
        } catch {
            errorText = "List failed: \(NotesModel.describe(error))"
        }
    }

    /// `fs.stat` each file of the open folder (sequentially, up to 64) for
    /// the lock badge; `fs.list` does not carry `protected`.
    private func statFiles(vol: String, dir: String) {
        statTask?.cancel()
        let names = rows.filter { !$0.entry.dir && $0.entry.size >= 8 }.prefix(64).map(\.entry.name)
        let browser = self.browser
        statTask = Task { [weak self] in
            for name in names {
                guard !Task.isCancelled else { return }
                guard let stat = try? await browser.stat(vol: vol, path: FilePath.join(dir, name)) else { continue }
                guard let self, self.volume.rawValue == vol, self.path == dir else { return }
                if let index = self.rows.firstIndex(where: { $0.id == name }) {
                    self.rows[index].protected = stat.protected
                }
            }
        }
    }

    // MARK: - Changes

    func makeFolder(named name: String) async {
        await run("New folder") {
            _ = try await self.browser.mkdir(vol: self.volume.rawValue, in: self.path, name: name)
        }
    }

    func rename(_ row: Row, to name: String) async {
        await run("Rename") {
            _ = try await self.browser.rename(vol: self.volume.rawValue,
                                              path: FilePath.join(self.path, row.entry.name), to: name)
        }
    }

    func delete(_ row: Row) async {
        await run("Delete") {
            try await self.browser.delete(vol: self.volume.rawValue, path: FilePath.join(self.path, row.entry.name))
            self.log("files: deleted \(row.entry.name)")
        }
    }

    private func run(_ label: String, _ body: @escaping () async throws -> Void) async {
        do {
            try await body()
            errorText = nil
        } catch {
            errorText = "\(label) failed: \(NotesModel.describe(error))"
        }
        await reload(quiet: true)
    }

    // MARK: - Transfers

    /// Download a file to a temporary URL, then hand it to the preview.
    func download(_ row: Row) async {
        guard transfer == nil, !row.entry.dir else { return }
        let name = row.entry.name
        transfer = Transfer(direction: .download, name: name, done: 0, total: row.entry.size)
        defer { transfer = nil }
        do {
            let (bytes, info) = try await browser.download(
                vol: volume.rawValue, path: FilePath.join(path, name), progress: progressSink())
            let url = try Self.saveDownload(bytes, name: name)
            log("files: downloaded \(name) (\(bytes.count) B)")
            downloaded = Downloaded(name: name, url: url, kind: FilePreviewKind.of(name: name, bytes: bytes),
                                    crc32: info.crc32)
            errorText = nil
        } catch {
            errorText = "Download failed: \(NotesModel.describe(error))"
        }
    }

    /// Upload `data` into the open folder as `name` (sanitised; a free
    /// variant `name (2).ext` when it exists, so nothing is overwritten).
    func upload(_ data: [UInt8], named name: String) async {
        guard canEdit else { return }
        let taken = Set(rows.map(\.entry.name))
        let leaf = FilePath.uniqueName(FilePath.sanitizedName(name), taken: taken.contains)
        let item = FileBrowser.Upload(vol: volume.rawValue, path: FilePath.join(path, leaf), data: data, overwrite: false)
        transfer = Transfer(direction: .upload, name: leaf, done: 0, total: UInt32(clamping: data.count))
        defer { transfer = nil }
        do {
            try await browser.upload(item, progress: progressSink())
            log("files: uploaded \(leaf) (\(data.count) B)")
            errorText = nil
        } catch let error as RPCError where FileBrowser.isLinkFailure(error) {
            errorText = "Upload of \(leaf) interrupted; it starts again from the beginning when the pager reconnects."
        } catch {
            errorText = "Upload failed: \(NotesModel.describe(error))"
        }
        interruptedCount = await browser.interrupted.count
        await reload(quiet: true)
    }

    /// Resend uploads a dropped link cut off (from byte 0: no resume in v1).
    private func retryInterrupted() async {
        let pending = await browser.interrupted
        guard !pending.isEmpty else { return }
        log("files: resending \(pending.count) interrupted upload(s) from the start")
        let result = await browser.retryInterrupted { [weak self] upload, done, total in
            Task { @MainActor in
                self?.transfer = Transfer(direction: .upload, name: FilePath.name(upload.path), done: done, total: total)
            }
        }
        transfer = nil
        interruptedCount = await browser.interrupted.count
        if let first = result.failed.first {
            let (upload, error) = first
            errorText = "Upload of \(FilePath.name(upload.path)) failed: \(NotesModel.describe(error))"
            if !FileBrowser.isLinkFailure(error) { await browser.discardInterrupted(upload.id) }
            interruptedCount = await browser.interrupted.count
        }
        await reload(quiet: true)
    }

    /// Give up on uploads waiting for a reconnect.
    func discardInterrupted() async {
        for upload in await browser.interrupted { await browser.discardInterrupted(upload.id) }
        interruptedCount = 0
    }

    /// A progress callback that updates `transfer` on the main actor.
    private func progressSink() -> FileBrowser.Progress {
        { [weak self] done, total in
            Task { @MainActor in
                guard let self, var current = self.transfer else { return }
                current.done = done
                current.total = total
                self.transfer = current
            }
        }
    }

    // MARK: - Temporary downloads

    nonisolated private static var downloadsDirectory: URL {
        FileManager.default.temporaryDirectory.appendingPathComponent("pager-downloads", isDirectory: true)
    }

    /// Each download in its own folder so two files of one name never clash
    /// and Quick Look / Share see the real file name.
    nonisolated private static func saveDownload(_ bytes: [UInt8], name: String) throws -> URL {
        let folder = downloadsDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let url = folder.appendingPathComponent(FilePath.sanitizedName(name))
        try Data(bytes).write(to: url, options: [.atomic, .completeFileProtection])
        return url
    }

    /// Remove every temporary download.
    nonisolated static func clearDownloads() {
        try? FileManager.default.removeItem(at: downloadsDirectory)
    }
}
