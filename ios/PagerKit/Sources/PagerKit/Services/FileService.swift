/// One fs.list entry (PROTOCOL.md §6.1). `mtime` is 0 in hardware listings.
public struct FsEntry: Sendable, Equatable {
    public let name: String
    public let dir: Bool
    public let size: UInt32
    public let mtime: Int64

    public init(name: String, dir: Bool, size: UInt32, mtime: Int64) {
        self.name = name
        self.dir = dir
        self.size = size
        self.mtime = mtime
    }
}

/// fs.stat result.
public struct FsStat: Sendable, Equatable {
    public let dir: Bool
    public let size: UInt32
    public let mtime: Int64
    public let protected: Bool

    public init(dir: Bool, size: UInt32, mtime: Int64, protected: Bool) {
        self.dir = dir
        self.size = size
        self.mtime = mtime
        self.protected = protected
    }
}

/// Typed wrappers over the pager's fs.list / fs.stat / fs.delete on one
/// LinkSession (fs.read is `LinkSession.readFile`). RPC errors pass through as
/// `RPCError`; a malformed reply is `RPCError(code: "internal", msg:
/// "<method>: bad response")`.
public struct FileService: Sendable {
    /// Entries asked for per `fs.list` (the protocol's maximum).
    public static let pageLimit = 64
    /// Hard stop for a runaway cursor loop.
    public static let maxPages = 1000

    public let session: LinkSession
    public let timeout: Duration?

    public init(session: LinkSession, timeout: Duration? = nil) {
        self.session = session
        self.timeout = timeout
    }

    /// Every entry of `path`, following `cursor` until it is absent (requests
    /// `limit` `pageLimit`). Throws `internal` "fs.list: cursor did not advance"
    /// if a returned cursor is not greater than the previous one, and "fs.list:
    /// too many pages" past `maxPages`.
    public func list(vol: String, path: String) async throws -> [FsEntry] {
        var entries: [FsEntry] = []
        var cursor: Int64?
        var pages = 0
        while true {
            var params: [String: JSONValue] = [
                "vol": .string(vol),
                "path": .string(path),
                "limit": .number(Double(Self.pageLimit)),
            ]
            if let cursor { params["cursor"] = .number(Double(cursor)) }
            let reply = try await session.call("fs.list", params: params, timeout: timeout)
            guard case .array(let rawEntries)? = reply["entries"] else {
                throw Self.badResponse("fs.list")
            }
            for rawEntry in rawEntries { entries.append(try Self.entry(rawEntry)) }

            pages += 1
            guard let rawCursor = reply["cursor"] else { return entries }
            guard let next = Params.int64(rawCursor) else { throw Self.badResponse("fs.list") }
            if let cursor, next <= cursor {
                throw RPCError(code: "internal", msg: "fs.list: cursor did not advance")
            }
            guard pages < Self.maxPages else {
                throw RPCError(code: "internal", msg: "fs.list: too many pages")
            }
            cursor = next
        }
    }

    /// `fs.stat` — one path's type, size, mtime and `Salted__` flag.
    public func stat(vol: String, path: String) async throws -> FsStat {
        let reply = try await session.call("fs.stat", params: [
            "vol": .string(vol),
            "path": .string(path),
        ], timeout: timeout)
        guard let dir = reply["dir"]?.boolValue,
              let size = Params.uint32(reply["size"]),
              reply["mtime"] == nil || Params.int64(reply["mtime"]) != nil,
              reply["protected"] == nil || reply["protected"]?.boolValue != nil else {
            throw Self.badResponse("fs.stat")
        }
        return FsStat(
            dir: dir,
            size: size,
            mtime: Params.int64(reply["mtime"]) ?? 0,
            protected: reply["protected"]?.boolValue ?? false
        )
    }

    /// `fs.delete`; with `ifSize`/`ifCRC` the pager deletes only if the file
    /// still matches (only meaningful when the pager has cap `fs.delete.if`) —
    /// a mismatch throws `RPCError` `invalid_argument` "changed"
    /// (`isChanged`).
    public func delete(vol: String, path: String,
                       ifSize: UInt32? = nil, ifCRC: UInt32? = nil) async throws {
        var params: [String: JSONValue] = [
            "vol": .string(vol),
            "path": .string(path),
        ]
        if let size = ifSize { params["size"] = .number(Double(size)) }
        if let crc = ifCRC { params["crc32"] = .number(Double(crc)) }
        _ = try await session.call("fs.delete", params: params, timeout: timeout)
    }

    /// `fs.mkdir` — one level; the parent must exist.
    public func mkdir(vol: String, path: String) async throws {
        _ = try await session.call("fs.mkdir", params: [
            "vol": .string(vol),
            "path": .string(path),
        ], timeout: timeout)
    }

    /// `fs.rename` — `to` must not exist; a directory can't move into itself.
    public func rename(vol: String, from: String, to: String) async throws {
        _ = try await session.call("fs.rename", params: [
            "vol": .string(vol),
            "from": .string(from),
            "to": .string(to),
        ], timeout: timeout)
    }

    // MARK: - Reply parsing

    /// One listing entry: a non-empty name, an optional bool `dir` (default
    /// false), a required integer `size` and an optional integer `mtime`.
    private static func entry(_ value: JSONValue) throws -> FsEntry {
        guard let name = value["name"]?.stringValue, !name.isEmpty,
              value["dir"] == nil || value["dir"]?.boolValue != nil,
              let size = Params.uint32(value["size"]),
              value["mtime"] == nil || Params.int64(value["mtime"]) != nil else {
            throw badResponse("fs.list")
        }
        return FsEntry(
            name: name,
            dir: value["dir"]?.boolValue ?? false,
            size: size,
            mtime: Params.int64(value["mtime"]) ?? 0
        )
    }

    private static func badResponse(_ method: String) -> RPCError {
        RPCError(code: "internal", msg: "\(method): bad response")
    }
}

extension RPCError {
    /// fs.delete refused because the file no longer matches its precondition.
    public var isChanged: Bool { code == "invalid_argument" && msg == "changed" }

    /// The SD card is out (any fs.* on vol "sd").
    public var isSDNotMounted: Bool { code == "invalid_argument" && msg == "sd not mounted" }
}
