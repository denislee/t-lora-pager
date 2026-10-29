import Foundation
import PagerKit

// End-to-end checks of the Files screen engine (PagerKit `FileBrowser`)
// against the real emulator firmware over TCP 7600 (S1 "Done when"):
//   1. phone → SD: upload, then fs.stat size + fs.read CRC-32 equal the local bytes;
//   2. SD → phone: a file written on the emulator's side of the SD volume
//      (FILES_E2E_SD_ROOT, the host directory behind vol "sd") downloads
//      byte for byte with the CRC-32 the pager reports;
//   3. folder create / rename / delete through the browser;
//   4. a write that does not go through the browser raises evt.fs.changed,
//      and after it the browser's cached listing is dropped and re-read.
// Exit status = number of failed checks.

final class Events: @unchecked Sendable {
    private let lock = NSLock()
    private var fsChanged = 0
    func add(_ e: (name: String, params: JSONValue)) {
        lock.withLock { if e.name == "evt.fs.changed" { fsChanged += 1 } }
    }
    var fsChangedCount: Int { lock.withLock { fsChanged } }
}

nonisolated(unsafe) var failures = 0
func check(_ ok: Bool, _ label: String, _ detail: @autoclosure () -> String = "") {
    print(ok ? "[PASS] \(label)" : "[FAIL] \(label) \(detail())")
    if !ok { failures += 1 }
}

/// Wait until `events` has seen more than `count` evt.fs.changed.
func waitFsChanged(_ events: Events, after count: Int, _ within: Duration) async -> Bool {
    let deadline = ContinuousClock.now + within
    while ContinuousClock.now < deadline {
        if events.fsChangedCount > count { return true }
        try? await Task.sleep(for: .milliseconds(20))
    }
    return false
}

/// Deterministic pseudo-random bytes (xorshift), so a failure reproduces.
func pattern(_ count: Int, seed: UInt32) -> [UInt8] {
    var x = seed == 0 ? 0x9E37_79B9 : seed
    return (0..<count).map { _ in
        x ^= x << 13
        x ^= x >> 17
        x ^= x << 5
        return UInt8(truncatingIfNeeded: x)
    }
}

let env = ProcessInfo.processInfo.environment
let dir = "/files-e2e-\(Int(Date().timeIntervalSince1970))"

let transport = try POSIXTransport(host: env["FILES_E2E_HOST"] ?? "127.0.0.1",
                                   port: UInt16(env["FILES_E2E_PORT"] ?? "") ?? 7600)
let session = LinkSession(transport: transport)
let events = Events()
await session.start()
let watcher = Task { for await e in session.events { events.add(e) } }

let hello = try await session.call("sys.hello", params: [
    "proto": 1, "app": "files-e2e", "caps": ["sys", "fs"],
], timeout: .seconds(5))
var caps: [String] = []
if case .array(let raw)? = hello["caps"] { caps = raw.compactMap(\.stringValue) }
check(caps.contains("fs"), "hello caps include fs", "\(caps)")

let browser = FileBrowser(timeout: .seconds(30))
await browser.attach(session)
let files = FileService(session: session, timeout: .seconds(10))

// ---- 3. Folders --------------------------------------------------------------
do {
    let made = try await browser.mkdir(vol: "sd", in: "/", name: String(dir.dropFirst()))
    check(made == dir, "mkdir \(dir) on SD")
    let root = try await browser.list(vol: "sd", path: "/", force: true)
    check(root.contains { $0.name == String(dir.dropFirst()) && $0.dir }, "new folder listed (folders first)")
} catch {
    check(false, "mkdir on SD", "\(error)")
}

// ---- 1. phone → SD -----------------------------------------------------------
let upBytes = pattern(150_000, seed: 0x5EED_0001)
let upPath = FilePath.join(dir, "up.bin")
do {
    let before = events.fsChangedCount
    try await browser.upload(FileBrowser.Upload(vol: "sd", path: upPath, data: upBytes, overwrite: false))
    let stat = try await files.stat(vol: "sd", path: upPath)
    check(stat.size == UInt32(upBytes.count), "uploaded size matches", "\(stat.size)")
    let (back, info) = try await browser.download(vol: "sd", path: upPath)
    check(info.crc32 == CRC32.checksum(upBytes), "pager CRC-32 equals the phone's", "\(info.crc32)")
    check(back == upBytes, "phone → SD → phone is byte for byte")
    check(await waitFsChanged(events, after: before, .seconds(3)), "upload raised evt.fs.changed")
} catch {
    check(false, "phone → SD round trip", "\(error)")
}

do {
    try await browser.upload(FileBrowser.Upload(vol: "sd", path: upPath, data: [1, 2, 3], overwrite: false))
    check(false, "upload without overwrite refuses an existing file")
} catch let error as RPCError {
    check(error.msg == "exists", "upload without overwrite refuses an existing file", error.msg)
}

// ---- 2. SD → phone -----------------------------------------------------------
if let sdRoot = env["FILES_E2E_SD_ROOT"] {
    let hostBytes = pattern(70_001, seed: 0x5EED_0002)
    let leaf = "from-pager.bin"
    let hostURL = URL(fileURLWithPath: sdRoot + dir).appendingPathComponent(leaf)
    do {
        try Data(hostBytes).write(to: hostURL)
        let (got, info) = try await browser.download(vol: "sd", path: FilePath.join(dir, leaf))
        check(info.crc32 == CRC32.checksum(hostBytes), "SD file's CRC-32 as reported by the pager")
        check(got == hostBytes, "SD → phone is byte for byte")
    } catch {
        check(false, "SD → phone download", "\(error)")
    }
} else {
    print("[SKIP] SD → phone of a pager-side file: set FILES_E2E_SD_ROOT to the emulator's <fs root>/sd")
}

// ---- 4. evt.fs.changed refreshes an open listing -----------------------------
do {
    let listed = try await browser.list(vol: "sd", path: dir)
    let before = events.fsChangedCount
    // A write the browser does not know about (the pager's own editor save
    // stands in as any other local write: both raise evt.fs.changed).
    try await session.writeFile(vol: "sd", path: FilePath.join(dir, "side.txt"), data: Array("side\n".utf8),
                                overwrite: true, timeout: .seconds(10))
    let cachedStill = try await browser.list(vol: "sd", path: dir)
    check(cachedStill.map(\.name) == listed.map(\.name), "the listing is served from the cache before the event")
    check(await waitFsChanged(events, after: before, .seconds(3)), "evt.fs.changed after a pager-side write")
    await browser.invalidate()  // what FilesModel.fsChanged does
    let fresh = try await browser.list(vol: "sd", path: dir)
    check(fresh.contains { $0.name == "side.txt" }, "re-listing after the event shows the new file",
          "\(fresh.map(\.name))")
} catch {
    check(false, "evt.fs.changed refresh", "\(error)")
}

// ---- 3. rename + delete, then clean up ---------------------------------------
do {
    let renamed = try await browser.rename(vol: "sd", path: upPath, to: "renamed.bin")
    let names = try await browser.list(vol: "sd", path: dir).map(\.name)
    check(names.contains("renamed.bin") && !names.contains("up.bin"), "rename", "\(names) \(renamed)")
    try await browser.delete(vol: "sd", path: dir)
    let root = try await browser.list(vol: "sd", path: "/", force: true)
    check(!root.contains { $0.name == String(dir.dropFirst()) }, "delete removes the folder and its files")
} catch {
    check(false, "rename / delete", "\(error)")
}

watcher.cancel()
await session.stop()
transport.close()
print(failures == 0 ? "files-e2e: all checks passed" : "files-e2e: \(failures) failed")
exit(Int32(failures))
