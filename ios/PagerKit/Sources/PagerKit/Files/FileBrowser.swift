import Foundation

/// Path helpers for the Files screen. Paths are volume-absolute, `/`
/// separated, with no trailing slash except the root `"/"`, the form
/// `fs.list` / `fs.stat` take (PROTOCOL.md §6.1).
public enum FilePath {
    /// The volume root.
    public static let root = "/"

    /// `path` with repeated `/` and `.` segments collapsed, a leading `/`
    /// added and any trailing one dropped. `..` is kept as is: the pager
    /// rejects it, and the browser never builds one.
    public static func normalize(_ path: String) -> String {
        let parts = path.split(separator: "/", omittingEmptySubsequences: true).filter { $0 != "." }
        return "/" + parts.joined(separator: "/")
    }

    /// `name` inside directory `dir`.
    public static func join(_ dir: String, _ name: String) -> String {
        let base = normalize(dir)
        return base == root ? "/" + name : base + "/" + name
    }

    /// The directory holding `path` (`"/"` for a top-level entry and for the root).
    public static func parent(_ path: String) -> String {
        let normalized = normalize(path)
        guard let slash = normalized.lastIndex(of: "/"), slash != normalized.startIndex else { return root }
        return String(normalized[..<slash])
    }

    /// The last component of `path` (empty for the root).
    public static func name(_ path: String) -> String {
        let normalized = normalize(path)
        guard let slash = normalized.lastIndex(of: "/") else { return normalized }
        return String(normalized[normalized.index(after: slash)...])
    }

    /// One breadcrumb: the label shown and the directory it opens.
    public struct Crumb: Sendable, Equatable, Hashable {
        public let name: String
        public let path: String

        public init(name: String, path: String) {
            self.name = name
            self.path = path
        }
    }

    /// The breadcrumbs from the root to `path`; the root's label is
    /// `rootName` (the volume's name).
    public static func crumbs(_ path: String, rootName: String) -> [Crumb] {
        var out = [Crumb(name: rootName, path: root)]
        var current = root
        for part in normalize(path).split(separator: "/") {
            current = join(current, String(part))
            out.append(Crumb(name: String(part), path: current))
        }
        return out
    }

    /// Longest name the browser lets a user type (FAT long names are 255
    /// UTF-16 units; the pager's own paths stay far below this).
    public static let maxNameBytes = 128

    /// A name the user typed for a new folder, a rename or an upload: not
    /// empty, not `.`/`..`, no `/` or `\`, no control bytes, at most
    /// `maxNameBytes` UTF-8 bytes, and no leading/trailing spaces.
    public static func isValidName(_ name: String) -> Bool {
        guard !name.isEmpty, name != ".", name != "..",
              name.utf8.count <= maxNameBytes,
              name == name.trimmingCharacters(in: .whitespaces) else { return false }
        return !name.unicodeScalars.contains { $0 == "/" || $0 == "\\" || $0.value < 0x20 || $0.value == 0x7F }
    }

    /// `name` with any `/`, `\` or control character replaced by `_` and
    /// surrounding spaces trimmed, so a file picked on the phone can be
    /// uploaded under a name the pager accepts. Empty input becomes `"file"`.
    public static func sanitizedName(_ name: String) -> String {
        var scalars = String.UnicodeScalarView()
        for scalar in name.unicodeScalars {
            if scalar == "/" || scalar == "\\" || scalar.value < 0x20 || scalar.value == 0x7F {
                scalars.append("_")
            } else {
                scalars.append(scalar)
            }
        }
        var out = String(scalars).trimmingCharacters(in: .whitespaces)
        while out.utf8.count > maxNameBytes { out.removeLast() }
        if out.isEmpty || out == "." || out == ".." { return "file" }
        return out
    }

    /// `name` with ` (2)`, ` (3)`, … before its extension until `taken`
    /// says no (an upload never overwrites unless asked).
    public static func uniqueName(_ name: String, taken: (String) -> Bool) -> String {
        guard taken(name) else { return name }
        let dot = name.lastIndex(of: ".").flatMap { $0 == name.startIndex ? nil : $0 }
        let stem = dot.map { String(name[..<$0]) } ?? name
        let ext = dot.map { String(name[$0...]) } ?? ""
        var n = 2
        while true {
            let candidate = "\(stem) (\(n))\(ext)"
            if !taken(candidate) { return candidate }
            n += 1
        }
    }
}

/// How the Files screen opens a downloaded file.
public enum FilePreviewKind: Sendable, Equatable {
    /// Readable UTF-8 text: shown in the in-app text preview.
    case text
    /// Anything Quick Look can show (images, PDF, audio, …).
    case quickLook
    /// `Salted__` ciphertext: never decrypted here (Notes owns that).
    case encrypted

    /// Extensions treated as text even when the bytes are empty.
    public static let textExtensions: Set<String> = [
        "txt", "md", "json", "csv", "log", "ini", "cfg", "conf", "yml", "yaml", "xml", "html", "htm",
        "js", "py", "c", "h", "cpp", "sh", "toml",
    ]

    /// Decide from the name and the bytes: ciphertext first, then a text
    /// extension or bytes that are valid UTF-8 with no NULs (first 4 KiB).
    public static func of(name: String, bytes: [UInt8]) -> FilePreviewKind {
        if NoteCrypto.isEncrypted(bytes) { return .encrypted }
        if textExtensions.contains(fileExtension(name)) { return .text }
        let head = Array(bytes.prefix(4096))
        if head.isEmpty || head.contains(0) { return .quickLook }
        if isUTF8(head) || validPrefix(head) { return .text }
        return .quickLook
    }

    /// The lowercased extension of `name` ("" when there is none or the
    /// only dot leads the name, as in `.profile`).
    public static func fileExtension(_ name: String) -> String {
        guard let dot = name.lastIndex(of: "."), dot != name.startIndex else { return "" }
        return name[name.index(after: dot)...].lowercased()
    }

    private static func isUTF8(_ bytes: [UInt8]) -> Bool {
        String(bytes: bytes, encoding: .utf8) != nil
    }

    /// The 4 KiB cut can split a multi-byte character: accept a prefix that
    /// becomes valid UTF-8 after dropping up to three trailing bytes.
    private static func validPrefix(_ bytes: [UInt8]) -> Bool {
        guard bytes.count >= 4096 else { return false }
        for drop in 1...3 where isUTF8(Array(bytes.dropLast(drop))) {
            return true
        }
        return false
    }
}

/// The Files screen's link-side work (`IOS_COMPANION_PLAN.md` §6.1 "Files"):
/// listings cached per folder until `evt.fs.changed`, `fs.stat` for the
/// lock badge, folder create / rename / delete, and downloads and uploads
/// with progress, one transfer at a time (the pager allows two; the other
/// slot stays free for notes and tasks).
///
/// Uploads are `fs.write.begin` → DATA → `fs.write.commit`. PagerLink v1 has
/// no resume (`resume:true` is `unsupported`, and a dropped link deletes the
/// `.part`), so "resume after disconnect" means: an upload that fails with
/// `disconnected` / `timeout` is remembered in `interrupted`, and
/// `retryInterrupted` sends it again **from byte 0** on the next link.
public actor FileBrowser {
    /// One upload to (re)send.
    public struct Upload: Sendable, Equatable, Identifiable {
        public let id: UUID
        public let vol: String
        public let path: String
        public let data: [UInt8]
        public let overwrite: Bool

        public init(id: UUID = UUID(), vol: String, path: String, data: [UInt8], overwrite: Bool) {
            self.id = id
            self.vol = vol
            self.path = path
            self.data = data
            self.overwrite = overwrite
        }
    }

    /// Progress callback: bytes done, bytes total.
    public typealias Progress = @Sendable (_ done: UInt32, _ total: UInt32) -> Void

    /// What every call throws when there is no link.
    public static let notConnected = RPCError(code: "disconnected", msg: "not connected")

    private let timeout: Duration
    private var session: LinkSession?
    private var listings: [String: [FsEntry]] = [:]
    private var transferBusy = false
    private var transferWaiters: [CheckedContinuation<Void, Never>] = []

    /// Uploads cut off by a dropped link, oldest first.
    public private(set) var interrupted: [Upload] = []

    public init(timeout: Duration = .seconds(30)) {
        self.timeout = timeout
    }

    /// Attach the live link, or nil when it dropped. Listings from an old
    /// link are dropped (the pager may have changed meanwhile).
    public func attach(_ session: LinkSession?) {
        self.session = session
        listings.removeAll()
    }

    /// `evt.fs.changed`: every cached listing is stale.
    public func invalidate() {
        listings.removeAll()
    }

    /// Whether `vol:path` has a cached listing (tests, the UI's spinner).
    public func isCached(vol: String, path: String) -> Bool {
        listings[Self.key(vol, path)] != nil
    }

    // MARK: - Listing

    /// The entries of directory `path` on `vol` in the pager's order (folders
    /// first, then files, each by name): the cached listing unless `force`.
    public func list(vol: String, path: String, force: Bool = false) async throws -> [FsEntry] {
        let path = FilePath.normalize(path)
        let key = Self.key(vol, path)
        if !force, let cached = listings[key] { return cached }
        let entries = try await files().list(vol: vol, path: path)
        listings[key] = entries
        return entries
    }

    /// `fs.stat` — the lock badge reads `protected`.
    public func stat(vol: String, path: String) async throws -> FsStat {
        try await files().stat(vol: vol, path: FilePath.normalize(path))
    }

    // MARK: - Changes

    /// Make folder `name` in `dir`; returns its path.
    @discardableResult
    public func mkdir(vol: String, in dir: String, name: String) async throws -> String {
        guard FilePath.isValidName(name) else { throw RPCError(code: "invalid_argument", msg: "bad name") }
        let path = FilePath.join(dir, name)
        try await files().mkdir(vol: vol, path: path)
        drop(vol: vol, dir: dir)
        return path
    }

    /// Rename `path` to `newName` in the same folder; returns the new path.
    @discardableResult
    public func rename(vol: String, path: String, to newName: String) async throws -> String {
        guard FilePath.isValidName(newName) else { throw RPCError(code: "invalid_argument", msg: "bad name") }
        let dir = FilePath.parent(path)
        let target = FilePath.join(dir, newName)
        try await files().rename(vol: vol, from: FilePath.normalize(path), to: target)
        drop(vol: vol, dir: dir)
        listings.removeValue(forKey: Self.key(vol, FilePath.normalize(path)))
        return target
    }

    /// Delete a file or a folder (with its contents).
    public func delete(vol: String, path: String) async throws {
        let path = FilePath.normalize(path)
        try await files().delete(vol: vol, path: path)
        drop(vol: vol, dir: FilePath.parent(path))
        let prefix = Self.key(vol, path)
        for key in listings.keys where key == prefix || key.hasPrefix(prefix + "/") {
            listings.removeValue(forKey: key)
        }
    }

    // MARK: - Transfers

    /// `fs.read` the whole file (CRC-32 checked by the session).
    public func download(vol: String, path: String,
                         progress: Progress? = nil) async throws -> ([UInt8], FileInfo) {
        let session = try link()
        await acquireTransfer()
        defer { releaseTransfer() }
        return try await session.readFile(vol: vol, path: FilePath.normalize(path), timeout: timeout,
                                          progress: progress)
    }

    /// Upload `data` as `path`. Without `overwrite` an existing file is
    /// refused (`invalid_argument` "exists"). A link failure keeps the
    /// upload in `interrupted` for `retryInterrupted`, then rethrows.
    public func upload(_ upload: Upload, progress: Progress? = nil) async throws {
        interrupted.removeAll { $0.id == upload.id }
        do {
            let session = try link()
            await acquireTransfer()
            defer { releaseTransfer() }
            try await session.writeFile(vol: upload.vol, path: FilePath.normalize(upload.path), data: upload.data,
                                        overwrite: upload.overwrite, timeout: timeout, progress: progress)
        } catch let error as RPCError where Self.isLinkFailure(error) {
            interrupted.append(upload)
            throw error
        }
        drop(vol: upload.vol, dir: FilePath.parent(upload.path))
    }

    /// Send every interrupted upload again from byte 0 (there is no
    /// protocol resume in v1), oldest first; `progress` is told which one
    /// runs. Returns the ones that went through; the rest stay
    /// `interrupted` (or are dropped when the pager refused them for good).
    public func retryInterrupted(
        progress: (@Sendable (_ upload: Upload, _ done: UInt32, _ total: UInt32) -> Void)? = nil
    ) async -> (sent: [Upload], failed: [(Upload, RPCError)]) {
        let queue = interrupted
        var sent: [Upload] = []
        var failed: [(Upload, RPCError)] = []
        for item in queue {
            do {
                var itemProgress: Progress?
                if let progress {
                    itemProgress = { done, total in progress(item, done, total) }
                }
                try await upload(item, progress: itemProgress)
                sent.append(item)
            } catch let error as RPCError {
                failed.append((item, error))
                if Self.isLinkFailure(error) { break }
            } catch {
                failed.append((item, RPCError(code: "internal", msg: "\(error)")))
            }
        }
        return (sent, failed)
    }

    /// Forget an interrupted upload (the user cancelled it).
    public func discardInterrupted(_ id: UUID) {
        interrupted.removeAll { $0.id == id }
    }

    /// A dropped link or a stalled transfer: worth retrying on the next link.
    public static func isLinkFailure(_ error: RPCError) -> Bool {
        error.code == "disconnected" || error.code == "timeout"
    }

    // MARK: - Helpers

    private static func key(_ vol: String, _ path: String) -> String { "\(vol):\(FilePath.normalize(path))" }

    private func drop(vol: String, dir: String) {
        listings.removeValue(forKey: Self.key(vol, dir))
    }

    private func link() throws -> LinkSession {
        guard let session else { throw Self.notConnected }
        return session
    }

    private func files() throws -> FileService {
        FileService(session: try link(), timeout: timeout)
    }

    private func acquireTransfer() async {
        if !transferBusy {
            transferBusy = true
            return
        }
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            transferWaiters.append(continuation)
        }
    }

    private func releaseTransfer() {
        if transferWaiters.isEmpty {
            transferBusy = false
        } else {
            transferWaiters.removeFirst().resume()
        }
    }
}
