import Foundation

/// The list preview of a note (IOS_NOTES_VIEWER_PLAN.md §3.4).
public enum NotePreview {
    /// First non-empty line of a note's text, trimmed, at most 120 characters
    /// (list previews). Nil when the text has no non-empty line.
    public static func firstLine(_ text: String) -> String? {
        for line in text.split(separator: "\n", omittingEmptySubsequences: false) {
            let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !trimmed.isEmpty else { continue }
            return String(trimmed.prefix(maxLength))
        }
        return nil
    }

    /// Longest preview the list shows.
    private static let maxLength = 120
}

/// The viewer's link-side work (IOS_NOTES_VIEWER_PLAN.md §3.1–3.2): scan both
/// volumes, fill `mtime`/`protected` lazily with `fs.stat`, read notes through
/// the cache, and prefetch previews sequentially.
///
/// One link operation at a time: `refresh`, `stat` and a cache-missing `read`
/// queue on an async gate, so prefetch never makes the user's own tap collide
/// with a transfer in flight.
public actor NotesLibrary {
    /// What every call throws when there is no link.
    private static let notConnected = RPCError(code: "disconnected", msg: "not connected")

    private let cache: NotesCache
    private let readTimeout: Duration

    /// The live link, nil when disconnected.
    private var session: LinkSession?

    /// The one operation holding the gate, and the callers queued behind it.
    private var busy = false
    private var waiters: [CheckedContinuation<Void, Never>] = []

    public init(cache: NotesCache, readTimeout: Duration = .seconds(60)) {
        self.cache = cache
        self.readTimeout = readTimeout
    }

    /// Attach the live link, or nil when it dropped.
    public func attach(_ session: LinkSession?) {
        self.session = session
    }

    /// Whether a link is attached.
    public var isAttached: Bool { session != nil }

    // MARK: - Listing

    /// `fs.list` both volumes, save the result to the cache, and return it.
    /// Throws `RPCError` `disconnected` "not connected" when detached.
    public func refresh() async throws -> NotesIndexer.Scan {
        guard let session = self.session else { throw Self.notConnected }
        await acquire()
        defer { release() }

        let scan = try await NotesIndexer.scan(FileService(session: session, timeout: readTimeout))
        cache.saveScan(scan, at: Date())
        return scan
    }

    /// `fs.stat` one note: the entry with `mtime` (0 → nil) and `protected`
    /// filled.
    public func stat(_ entry: NoteEntry) async throws -> NoteEntry {
        guard let session = self.session else { throw Self.notConnected }
        await acquire()
        defer { release() }

        let info = try await FileService(session: session, timeout: readTimeout)
            .stat(vol: entry.vol, path: entry.path)
        var updated = entry
        updated.mtime = info.mtime == 0 ? nil : info.mtime
        updated.protected = info.protected
        return updated
    }

    /// The last saved listing, when there is one.
    public func cachedScan() -> (scan: NotesIndexer.Scan, at: Date, stale: Bool)? {
        cache.cachedScan()
    }

    // MARK: - Reading

    /// The note's raw bytes: a cache hit (`NotesCache.bytes(for:)`) or
    /// `fs.read` stored to the cache. A read whose size disagrees with the
    /// listing still returns and stores the bytes — the caller re-lists. A
    /// detached library serves only a full cache hit and otherwise throws
    /// `disconnected`.
    public func read(_ entry: NoteEntry) async throws -> [UInt8] {
        if let cached = cache.bytes(for: entry) { return cached }

        guard let session = self.session else { throw Self.notConnected }
        await acquire()
        defer { release() }

        // A read that queued behind another may have been cached meanwhile.
        if let cached = cache.bytes(for: entry) { return cached }

        let (bytes, _) = try await session.readFile(
            vol: entry.vol, path: entry.path, timeout: readTimeout)
        cache.store(entry, bytes: bytes, mtime: entry.mtime)
        return bytes
    }

    /// Sequential prefetch for list previews: for each entry, stop when
    /// `shouldContinue()` returns false; fill its mtime with `fs.stat` when
    /// attached and unknown, then read it (cache or link) and call `onRead`.
    /// One entry's error is skipped. Never two transfers at once.
    public func prefetch(
        _ entries: [NoteEntry],
        shouldContinue: @escaping @Sendable () async -> Bool,
        onRead: @escaping @Sendable (NoteEntry, [UInt8]) async -> Void
    ) async {
        for entry in entries {
            guard !Task.isCancelled, await shouldContinue() else { return }
            do {
                var item = entry
                if item.mtime == nil, isAttached {
                    item = try await stat(item)
                }
                let bytes = try await read(item)
                await onRead(item, bytes)
            } catch {
                continue
            }
        }
    }

    // MARK: - Editing

    /// Why a save or delete was refused before anything on the pager changed.
    public enum EditError: Error, Equatable, Sendable {
        /// The note on the pager is no longer the bytes the edit started
        /// from (the pager or another phone changed or removed it).
        case changedOnPager
    }

    /// Replace a note's bytes on the pager. `entry` is the copy the edit
    /// started from and `base` the CRC-32 of its bytes; `mirrors` are the
    /// other volumes' copies of the same name (the pager mirrors a save to
    /// internal and SD, so a save rewrites every copy, as the pager's own
    /// does). `entry` is read fresh first: when it no longer has `base`,
    /// this throws `EditError.changedOnPager` and writes nothing. Returns
    /// every written copy with its new size (its bytes cached).
    public func write(_ entry: NoteEntry, mirrors: [NoteEntry], bytes: [UInt8],
                      base: UInt32) async throws -> [NoteEntry] {
        guard let session = self.session else { throw Self.notConnected }
        guard let size = UInt32(exactly: bytes.count) else {
            throw RPCError(code: "invalid_argument", msg: "too large")
        }
        await acquire()
        defer { release() }

        do {
            let (current, _) = try await session.readFile(vol: entry.vol, path: entry.path, timeout: readTimeout)
            guard CRC32.checksum(current) == base else { throw EditError.changedOnPager }
        } catch let error as RPCError where error.code == "invalid_argument" {
            throw EditError.changedOnPager  // gone (or the card went out)
        }
        var written: [NoteEntry] = []
        for copy in [entry] + mirrors.filter({ $0.id != entry.id }) {
            try await session.writeFile(vol: copy.vol, path: copy.path, data: bytes,
                                        overwrite: true, timeout: readTimeout)
            var updated = copy
            updated.size = size
            updated.mtime = nil
            updated.crc32 = CRC32.checksum(bytes)
            updated.protected = NoteCrypto.isEncrypted(bytes)
            cache.store(updated, bytes: bytes, mtime: nil)
            written.append(updated)
        }
        return written
    }

    /// Write a new note `/notes/<name>` (never overwriting: `invalid_argument`
    /// "exists" passes through) to internal flash and, with `mirrorSD`, to the
    /// SD card too, as the pager's own save does (`hw_save_preferred_bytes`:
    /// internal, then a best-effort SD copy). An SD copy that fails — no
    /// `/notes` folder yet, the card went out — is retried once after
    /// `fs.mkdir /notes` and then skipped. Returns the written copies (bytes
    /// cached).
    public func create(name: String, bytes: [UInt8], mirrorSD: Bool) async throws -> [NoteEntry] {
        guard let session = self.session else { throw Self.notConnected }
        guard let size = UInt32(exactly: bytes.count) else {
            throw RPCError(code: "invalid_argument", msg: "too large")
        }
        await acquire()
        defer { release() }

        func entry(_ vol: String) -> NoteEntry {
            NoteEntry(vol: vol, name: name, size: size, mtime: nil,
                      protected: NoteCrypto.isEncrypted(bytes), crc32: CRC32.checksum(bytes))
        }
        let path = NotesIndexer.notesDir + "/" + name
        try await session.writeFile(vol: "int", path: path, data: bytes, overwrite: false, timeout: readTimeout)
        var written = [entry("int")]
        if mirrorSD {
            do {
                try await session.writeFile(vol: "sd", path: path, data: bytes, overwrite: false, timeout: readTimeout)
                written.append(entry("sd"))
            } catch let error as RPCError where !error.isSDNotMounted && error.code != "disconnected" {
                let fs = FileService(session: session, timeout: readTimeout)
                if (try? await fs.mkdir(vol: "sd", path: NotesIndexer.notesDir)) != nil,
                   (try? await session.writeFile(vol: "sd", path: path, data: bytes, overwrite: false,
                                                 timeout: readTimeout)) != nil {
                    written.append(entry("sd"))
                }
            } catch {
                // Best effort, like the pager's redundant SD save.
            }
        }
        for copy in written { cache.store(copy, bytes: bytes, mtime: nil) }
        return written
    }

    /// Delete a note from the pager: every copy in `copies` (the pager's own
    /// delete removes the internal and SD copies). With `conditional` (the
    /// pager's cap `fs.delete.if`) each copy goes only while it still has the
    /// size and CRC-32 `base` describes (`invalid_argument` "changed"
    /// otherwise, mapped to `EditError.changedOnPager`).
    public func delete(_ copies: [NoteEntry], base: (size: UInt32, crc32: UInt32)?,
                       conditional: Bool) async throws {
        guard let session = self.session else { throw Self.notConnected }
        await acquire()
        defer { release() }

        let fs = FileService(session: session, timeout: readTimeout)
        for copy in copies {
            do {
                if conditional, let base {
                    try await fs.delete(vol: copy.vol, path: copy.path, ifSize: base.size, ifCRC: base.crc32)
                } else {
                    try await fs.delete(vol: copy.vol, path: copy.path)
                }
            } catch let error as RPCError where error.isChanged {
                throw EditError.changedOnPager
            } catch let error as RPCError where error.code == "invalid_argument" && error.msg == "not found" {
                // Already gone: what a delete wants.
            }
            cache.remove(vol: copy.vol, name: copy.name)
        }
    }

    // MARK: - Pager events

    /// `evt.fs.changed`: the cached listing is stale; the caller re-lists.
    public func fsChanged() {
        cache.markStale()
    }

    /// The sync deleted a note on the pager: drop its cached bytes.
    public func noteDeleted(vol: String, name: String) {
        cache.remove(vol: vol, name: name)
    }

    // MARK: - Gate

    /// Take the one-operation gate, waiting for whoever holds it.
    private func acquire() async {
        if !busy {
            busy = true
            return
        }
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            waiters.append(continuation)
        }
        // Ownership was handed over by `release()`; `busy` is still true.
    }

    /// Hand the gate to the next caller, or free it.
    private func release() {
        if waiters.isEmpty {
            busy = false
        } else {
            waiters.removeFirst().resume()
        }
    }
}
