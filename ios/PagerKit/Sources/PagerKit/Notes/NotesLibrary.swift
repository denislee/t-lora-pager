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
