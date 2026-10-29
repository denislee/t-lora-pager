import Foundation

@testable import PagerKit

/// An in-memory pager filesystem behind every `fs.*` method (PROTOCOL.md
/// §6.1) with the firmware's error strings (`src/hal/link/handlers_fs.cpp`),
/// for the Files screen, voice memo and journal tests. Directories are
/// explicit (`mkdir`), and a write needs its parent to exist, as on the pager.
final class CompanionFakeFs: @unchecked Sendable {
    private let lock = NSLock()
    private var files: [String: [UInt8]] = [:]
    private var dirs: Set<String> = []
    private var uploads: [UInt16: (key: String, size: UInt32, crc: UInt32, overwrite: Bool, task: Task<[UInt8], Error>)] = [:]
    private var mountedSD = true
    private var log: [String] = []
    /// The next `fs.write.commit` fails with `disconnected` this many times.
    private var dropCommits = 0

    init() {
        dirs = ["int:/", "sd:/"]
    }

    private static func key(_ vol: String, _ path: String) -> String { "\(vol):\(FilePath.normalize(path))" }

    var sdMounted: Bool {
        get { lock.withLock { mountedSD } }
        set { lock.withLock { mountedSD = newValue } }
    }

    /// Every handled method, oldest first.
    var methods: [String] { lock.withLock { log } }

    func failNextCommits(_ count: Int) { lock.withLock { dropCommits = count } }

    func set(_ vol: String, _ path: String, _ bytes: [UInt8]) {
        lock.withLock {
            files[Self.key(vol, path)] = bytes
            var dir = FilePath.parent(path)
            while true {
                dirs.insert(Self.key(vol, dir))
                if dir == "/" { break }
                dir = FilePath.parent(dir)
            }
        }
    }

    func mkdirs(_ vol: String, _ path: String) {
        lock.withLock {
            var dir = FilePath.normalize(path)
            while true {
                dirs.insert(Self.key(vol, dir))
                if dir == "/" { break }
                dir = FilePath.parent(dir)
            }
        }
    }

    func bytes(_ vol: String, _ path: String) -> [UInt8]? {
        lock.withLock { files[Self.key(vol, path)] }
    }

    func isDir(_ vol: String, _ path: String) -> Bool {
        lock.withLock { dirs.contains(Self.key(vol, path)) }
    }

    private func check(_ params: JSONValue) throws -> String {
        let vol = params["vol"]?.stringValue ?? ""
        guard vol == "int" || vol == "sd" else { throw RPCError(code: "invalid_argument", msg: "bad vol") }
        if vol == "sd" && !sdMounted { throw RPCError(code: "invalid_argument", msg: "sd not mounted") }
        return vol
    }

    private func note(_ method: String) { lock.withLock { log.append(method) } }

    // swiftlint:disable:next function_body_length
    func register(_ pager: LinkSession) async {
        await pager.handle("fs.list") { params in
            self.note("fs.list")
            let vol = try self.check(params)
            let path = FilePath.normalize(params["path"]?.stringValue ?? "/")
            return try self.lock.withLock {
                guard self.dirs.contains(Self.key(vol, path)) else {
                    throw RPCError(code: "invalid_argument", msg: "not a directory")
                }
                var entries: [(String, Bool, Int)] = []
                let prefix = "\(vol):"
                for dir in self.dirs where dir.hasPrefix(prefix) {
                    let p = String(dir.dropFirst(prefix.count))
                    if p != "/" && FilePath.parent(p) == path { entries.append((FilePath.name(p), true, 0)) }
                }
                for (key, bytes) in self.files where key.hasPrefix(prefix) {
                    let p = String(key.dropFirst(prefix.count))
                    if FilePath.parent(p) == path { entries.append((FilePath.name(p), false, bytes.count)) }
                }
                entries.sort { $0.1 != $1.1 ? $0.1 : $0.0 < $1.0 }
                return ["entries": .array(entries.map {
                    ["name": .string($0.0), "dir": .bool($0.1), "size": .number(Double($0.2)), "mtime": 0]
                })]
            }
        }
        await pager.handle("fs.stat") { params in
            self.note("fs.stat")
            let vol = try self.check(params)
            let path = params["path"]?.stringValue ?? ""
            if self.isDir(vol, path) {
                return ["dir": true, "size": 0, "mtime": 0, "protected": false]
            }
            guard let bytes = self.bytes(vol, path) else { throw RPCError(code: "invalid_argument", msg: "not found") }
            return ["dir": false, "size": .number(Double(bytes.count)), "mtime": 1_700_000_000,
                    "protected": .bool(NoteCrypto.isEncrypted(bytes))]
        }
        await pager.handle("fs.read") { params in
            self.note("fs.read")
            let vol = try self.check(params)
            guard let xfer = Params.uint16(params["xfer"]) else { throw RPCError(code: "invalid_argument", msg: "bad xfer") }
            guard let bytes = self.bytes(vol, params["path"]?.stringValue ?? "") else {
                throw RPCError(code: "invalid_argument", msg: "not found")
            }
            Task { try? await pager.sendBytes(xfer: xfer, data: bytes, timeout: .seconds(5)) }
            return ["xfer": .number(Double(xfer)), "size": .number(Double(bytes.count)),
                    "crc32": .number(Double(CRC32.checksum(bytes))), "protected": .bool(NoteCrypto.isEncrypted(bytes))]
        }
        await pager.handle("fs.write.begin") { params in
            self.note("fs.write.begin")
            let vol = try self.check(params)
            let path = FilePath.normalize(params["path"]?.stringValue ?? "")
            guard let size = Params.uint32(params["size"]), let crc = Params.uint32(params["crc32"]) else {
                throw RPCError(code: "invalid_argument", msg: "bad size")
            }
            let overwrite = params["overwrite"]?.boolValue ?? false
            if self.bytes(vol, path) != nil && !overwrite { throw RPCError(code: "invalid_argument", msg: "exists") }
            guard self.isDir(vol, FilePath.parent(path)) else {
                throw RPCError(code: "invalid_argument", msg: "not a directory")
            }
            let id = await pager.openIncoming()
            let task = Task { try await pager.receiveBytes(xfer: id, size: size, crc32: crc, timeout: .seconds(5)) }
            self.lock.withLock { self.uploads[id] = (Self.key(vol, path), size, crc, overwrite, task) }
            return ["xfer": .number(Double(id)), "offset": 0]
        }
        await pager.handle("fs.write.commit") { params in
            self.note("fs.write.commit")
            guard let id = Params.uint16(params["xfer"]),
                  let upload = self.lock.withLock({ self.uploads.removeValue(forKey: id) }) else {
                throw RPCError(code: "invalid_argument", msg: "no such transfer")
            }
            let data = try await upload.task.value
            try self.lock.withLock {
                if self.dropCommits > 0 {
                    self.dropCommits -= 1
                    throw RPCError.disconnected()
                }
                self.files[upload.key] = data
            }
            return .object([:])
        }
        await pager.handle("fs.write.abort") { params in
            self.note("fs.write.abort")
            if let id = Params.uint16(params["xfer"]) {
                self.lock.withLock { self.uploads.removeValue(forKey: id) }?.task.cancel()
            }
            return .object([:])
        }
        await pager.handle("fs.delete") { params in
            self.note("fs.delete")
            let vol = try self.check(params)
            let path = FilePath.normalize(params["path"]?.stringValue ?? "")
            let key = Self.key(vol, path)
            return try self.lock.withLock {
                if self.dirs.contains(key) {
                    if params["size"] != nil || params["crc32"] != nil {
                        throw RPCError(code: "invalid_argument", msg: "not a file")
                    }
                    self.dirs = self.dirs.filter { $0 != key && !$0.hasPrefix(key + "/") }
                    self.files = self.files.filter { !$0.key.hasPrefix(key + "/") }
                    return .object([:])
                }
                guard let bytes = self.files[key] else { throw RPCError(code: "invalid_argument", msg: "not found") }
                if let size = params["size"], Params.uint32(size) != UInt32(bytes.count) {
                    throw RPCError(code: "invalid_argument", msg: "changed")
                }
                if let crc = params["crc32"], Params.uint32(crc) != CRC32.checksum(bytes) {
                    throw RPCError(code: "invalid_argument", msg: "changed")
                }
                self.files[key] = nil
                return .object([:])
            }
        }
        await pager.handle("fs.mkdir") { params in
            self.note("fs.mkdir")
            let vol = try self.check(params)
            let path = FilePath.normalize(params["path"]?.stringValue ?? "")
            return try self.lock.withLock {
                let key = Self.key(vol, path)
                if self.dirs.contains(key) || self.files[key] != nil {
                    throw RPCError(code: "invalid_argument", msg: "exists")
                }
                guard self.dirs.contains(Self.key(vol, FilePath.parent(path))) else {
                    throw RPCError(code: "invalid_argument", msg: "not a directory")
                }
                self.dirs.insert(key)
                return .object([:])
            }
        }
        await pager.handle("fs.rename") { params in
            self.note("fs.rename")
            let vol = try self.check(params)
            let from = Self.key(vol, params["from"]?.stringValue ?? "")
            let to = Self.key(vol, params["to"]?.stringValue ?? "")
            return try self.lock.withLock {
                if self.files[to] != nil || self.dirs.contains(to) {
                    throw RPCError(code: "invalid_argument", msg: "exists")
                }
                if let bytes = self.files.removeValue(forKey: from) {
                    self.files[to] = bytes
                } else if self.dirs.contains(from) {
                    self.dirs = Set(self.dirs.map { $0 == from || $0.hasPrefix(from + "/") ? to + $0.dropFirst(from.count) : $0 })
                    var moved: [String: [UInt8]] = [:]
                    for (k, v) in self.files {
                        moved[k.hasPrefix(from + "/") ? to + k.dropFirst(from.count) : k] = v
                    }
                    self.files = moved
                } else {
                    throw RPCError(code: "invalid_argument", msg: "not found")
                }
                return .object([:])
            }
        }
    }

    /// A phone/pager session pair over a loopback transport, the pager side
    /// serving this filesystem.
    func makeSessions() async -> (phone: LinkSession, pager: LinkSession) {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await register(pager)
        await phone.start()
        await pager.start()
        return (phone, pager)
    }
}
