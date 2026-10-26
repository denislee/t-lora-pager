import Foundation

/// The app's `Caches/notes` store (IOS_NOTES_VIEWER_PLAN.md §3.2): the raw
/// bytes of every note the viewer has pulled, plus the last scan of the pager
/// and one record per cached note. Raw bytes only — an encrypted note stays
/// `Salted__` ciphertext on disk, exactly as `fs.read` returned it; decrypted
/// text is never stored.
///
/// Layout under `root`:
///
///     <root>/<vol>/<name>   the note's raw bytes
///     <root>/index.json     records + the cached scan
///
/// A size match alone is never a hit: a same-size edit would show stale text.
/// `bytes(for:)` also needs a matching CRC-32 or a matching non-zero mtime,
/// and then re-checks the file on disk against the record. Thread-safe.
public final class NotesCache: @unchecked Sendable {
    /// What one cached file was when it was stored. `size`/`crc32` are the
    /// bytes as written, so a listing that disagrees is a stale listing.
    public struct Record: Codable, Sendable, Equatable {
        public var vol: String
        public var name: String
        public var size: UInt32
        public var crc32: UInt32
        public var mtime: Int64?

        public init(vol: String, name: String, size: UInt32, crc32: UInt32, mtime: Int64? = nil) {
            self.vol = vol
            self.name = name
            self.size = size
            self.crc32 = crc32
            self.mtime = mtime
        }
    }

    /// The last listing of both volumes, and whether `evt.fs.changed` has
    /// invalidated it since (`stale` → re-list).
    public struct CachedScan: Codable, Sendable, Equatable {
        public var merged: [NoteEntry]
        public var all: [NoteEntry]
        public var sdMounted: Bool
        public var at: Date
        public var stale: Bool

        public init(merged: [NoteEntry], all: [NoteEntry], sdMounted: Bool,
                    at: Date, stale: Bool = false) {
            self.merged = merged
            self.all = all
            self.sdMounted = sdMounted
            self.at = at
            self.stale = stale
        }
    }

    /// `index.json`.
    private struct Index: Codable {
        var records: [Record] = []
        var scan: CachedScan?
    }

    /// The notes file (the bytes are the pager's; names come from it).
    private let root: URL
    private let lock = NSLock()
    private var index = Index()
    private var loaded = false

    public init(root: URL) {
        self.root = root
    }

    private var indexURL: URL {
        root.appendingPathComponent("index.json", isDirectory: false)
    }

    private func fileURL(_ vol: String, _ name: String) -> URL {
        root.appendingPathComponent(vol, isDirectory: true).appendingPathComponent(name, isDirectory: false)
    }

    // MARK: - Reading

    /// The note's raw bytes when the cache holds exactly what `entry` describes
    /// and the file on disk still matches its record; nil otherwise. Every
    /// other answer is a miss, including a corrupt or tampered file.
    public func bytes(for entry: NoteEntry) -> [UInt8]? {
        guard Self.isSafe(vol: entry.vol, name: entry.name) else { return nil }

        lock.lock()
        loadIfNeeded()
        let record = index.records.first { $0.vol == entry.vol && $0.name == entry.name }
        lock.unlock()

        guard let record, Self.matches(entry, record) else { return nil }
        guard let data = try? Data(contentsOf: fileURL(entry.vol, entry.name)) else { return nil }
        let bytes = [UInt8](data)
        guard bytes.count == Int(record.size), CRC32.checksum(bytes) == record.crc32 else { return nil }
        return bytes
    }

    /// The record for `(vol, name)`, if one is cached.
    public func record(vol: String, name: String) -> (size: UInt32, crc32: UInt32, mtime: Int64?)? {
        guard Self.isSafe(vol: vol, name: name) else { return nil }
        lock.lock()
        defer { lock.unlock() }
        loadIfNeeded()
        guard let record = index.records.first(where: { $0.vol == vol && $0.name == name }) else {
            return nil
        }
        return (size: record.size, crc32: record.crc32, mtime: record.mtime)
    }

    /// The last saved scan, when there is one.
    public func cachedScan() -> (scan: NotesIndexer.Scan, at: Date, stale: Bool)? {
        lock.lock()
        loadIfNeeded()
        let cached = index.scan
        lock.unlock()

        guard let cached else { return nil }
        let scan = NotesIndexer.Scan(merged: cached.merged, all: cached.all, sdMounted: cached.sdMounted)
        return (scan: scan, at: cached.at, stale: cached.stale)
    }

    // MARK: - Writing

    /// Cache `bytes` as `entry`'s note: the record's size and CRC-32 come from
    /// the bytes, its mtime from `mtime` (0 counts as unknown).
    public func store(_ entry: NoteEntry, bytes: [UInt8], mtime: Int64?) {
        guard Self.isSafe(vol: entry.vol, name: entry.name),
              let size = UInt32(exactly: bytes.count) else { return }

        let url = fileURL(entry.vol, entry.name)
        do {
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data(bytes).write(to: url, options: .atomic)
        } catch {
            return
        }

        var stamped: Int64?
        if let value = mtime, value != 0 {
            stamped = value
        }
        let record = Record(
            vol: entry.vol,
            name: entry.name,
            size: size,
            crc32: CRC32.checksum(bytes),
            mtime: stamped
        )
        lock.lock()
        loadIfNeeded()
        if let existing = index.records.firstIndex(where: {
            $0.vol == entry.vol && $0.name == entry.name
        }) {
            index.records[existing] = record
        } else {
            index.records.append(record)
        }
        saveIndex()
        lock.unlock()
    }

    /// Drop a note's file and record, and forget it in the cached scan (the
    /// sync deleted it on the pager).
    public func remove(vol: String, name: String) {
        guard Self.isSafe(vol: vol, name: name) else { return }
        try? FileManager.default.removeItem(at: fileURL(vol, name))

        lock.lock()
        loadIfNeeded()
        index.records.removeAll { $0.vol == vol && $0.name == name }
        if var scan = index.scan {
            scan.merged.removeAll { $0.vol == vol && $0.name == name }
            scan.all.removeAll { $0.vol == vol && $0.name == name }
            index.scan = scan
        }
        saveIndex()
        lock.unlock()
    }

    /// Save a fresh listing (not stale) and drop the records and files of
    /// notes it no longer holds.
    public func saveScan(_ scan: NotesIndexer.Scan, at: Date) {
        lock.lock()
        loadIfNeeded()
        let live = Set(scan.all.map { "\($0.vol):\($0.name)" })
        var kept: [Record] = []
        var gone: [Record] = []
        for record in index.records {
            if live.contains("\(record.vol):\(record.name)") {
                kept.append(record)
            } else {
                gone.append(record)
            }
        }
        index.records = kept
        index.scan = CachedScan(
            merged: scan.merged,
            all: scan.all,
            sdMounted: scan.sdMounted,
            at: at,
            stale: false
        )
        saveIndex()
        lock.unlock()

        for record in gone where Self.isSafe(vol: record.vol, name: record.name) {
            try? FileManager.default.removeItem(at: fileURL(record.vol, record.name))
        }
    }

    /// `evt.fs.changed`: the cached listing is stale and must be re-listed.
    public func markStale() {
        lock.lock()
        defer { lock.unlock() }
        loadIfNeeded()
        guard var scan = index.scan, !scan.stale else { return }
        scan.stale = true
        index.scan = scan
        saveIndex()
    }

    /// Delete everything under `root` ("Clear cached notes" / "Forget device").
    public func wipe() {
        lock.lock()
        defer { lock.unlock() }
        loaded = true
        index = Index()
        try? FileManager.default.removeItem(at: root)
    }

    // MARK: - Index file

    /// Load `index.json` on first use. Missing or corrupt = empty cache.
    /// Call with `lock` held.
    private func loadIfNeeded() {
        guard !loaded else { return }
        loaded = true
        guard let data = try? Data(contentsOf: indexURL),
              let decoded = try? JSONDecoder().decode(Index.self, from: data) else { return }
        index = decoded
    }

    /// Write `index.json` atomically. Call with `lock` held.
    private func saveIndex() {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        guard let data = try? encoder.encode(index) else { return }
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        try? data.write(to: indexURL, options: .atomic)
    }

    // MARK: - Validity

    /// Names come from the pager: only the two volumes, and only a single
    /// ordinary file-name component inside them.
    private static func isSafe(vol: String, name: String) -> Bool {
        guard vol == "int" || vol == "sd" else { return false }
        guard !name.isEmpty, !name.hasPrefix(".") else { return false }
        return !name.contains("/") && !name.contains("\\") && !name.contains("\0")
    }

    /// A record only vouches for `entry` when the sizes agree and an identity
    /// is known on both sides: the CRC-32, or a non-zero mtime.
    private static func matches(_ entry: NoteEntry, _ record: Record) -> Bool {
        guard record.size == entry.size else { return false }

        if let crc = entry.crc32, crc == record.crc32 { return true }
        if let mtime = entry.mtime, mtime != 0, let cached = record.mtime, mtime == cached { return true }
        return false
    }
}
