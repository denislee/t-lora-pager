import Foundation
import PagerKit

// End-to-end checks of the notes viewer + move engine against the real
// emulator firmware over TCP 7600 (IOS_NOTES_VIEWER_PLAN.md W7, W12, W14),
// with GitHub replaced by the in-process FakeGitHubRepo (real blob SHAs).
// Exit status = number of failed checks.

final class Events: @unchecked Sendable {
    private let lock = NSLock()
    private var results: [JSONValue] = []
    private var fsChanged = 0
    func add(_ e: (name: String, params: JSONValue)) {
        lock.lock(); defer { lock.unlock() }
        if e.name == "evt.test.result" { results.append(e.params) }
        if e.name == "evt.fs.changed" { fsChanged += 1 }
    }
    var testResults: [JSONValue] { return results }
    var fsChangedCount: Int { return fsChanged }
}

final class Reports: @unchecked Sendable {
    private let lock = NSLock()
    private var items: [NotesMoveResult] = []
    func add(_ r: NotesMoveResult) { lock.withLock { items.append(r) } }
    var all: [NotesMoveResult] { lock.withLock { items } }
}

nonisolated(unsafe) var failures = 0
func check(_ ok: Bool, _ label: String, _ detail: @autoclosure () -> String = "") {
    print(ok ? "[PASS] \(label)" : "[FAIL] \(label) \(detail())")
    if !ok { failures += 1 }
}

func hexBytes(_ s: String) -> [UInt8] {
    var out: [UInt8] = []
    var it = s.makeIterator()
    while let a = it.next(), let b = it.next() { out.append(UInt8(String([a, b]), radix: 16)!) }
    return out
}

func writeFile(_ s: LinkSession, _ vol: String, _ path: String, _ bytes: [UInt8]) async throws {
    let begin = try await s.call("fs.write.begin", params: [
        "vol": .string(vol), "path": .string(path), "size": .number(Double(bytes.count)),
        "crc32": .number(Double(CRC32.checksum(bytes))), "overwrite": true,
    ], timeout: .seconds(10))
    let xfer = UInt16(begin["xfer"]!.intValue!)
    try await s.sendBytes(xfer: xfer, data: bytes, timeout: .seconds(10))
    _ = try await s.call("fs.write.commit", params: ["xfer": .number(Double(xfer))], timeout: .seconds(20))
}

func waitTestResult(_ events: Events, after count: Int, _ within: Duration) async -> JSONValue? {
    let deadline = ContinuousClock.now + within
    while ContinuousClock.now < deadline {
        if events.testResults.count > count { return events.testResults[count] }
        try? await Task.sleep(for: .milliseconds(20))
    }
    return nil
}

let transport = try POSIXTransport(host: "127.0.0.1", port: 7600)
let session = LinkSession(transport: transport)
let events = Events()
await session.start()
let watcher = Task { for await e in session.events { events.add(e) } }

let hello = try await session.call("sys.hello", params: [
    "proto": 1, "app": "notes-e2e", "caps": ["sys", "fs"],
], timeout: .seconds(5))
var caps: [String] = []
if case .array(let raw)? = hello["caps"] { caps = raw.compactMap(\.stringValue) }
check(caps.contains("fs") && caps.contains(NotesSyncService.conditionalDeleteCap),
      "hello caps include fs and fs.delete.if", "\(caps)")

let repo = FakeGitHubRepo()
let (backend, _) = FakeGitHubRepo.backend(repo)
let service = NotesSyncService(session: session, github: backend, phoneDirectory: nil, pagerCaps: caps)
let reports = Reports()
await service.onPagerMove { reports.add($0) }
await service.register()
let files = FileService(session: session, timeout: .seconds(10))
let cacheRoot = FileManager.default.temporaryDirectory.appendingPathComponent("notes-e2e-\(UUID().uuidString)")
let library = NotesLibrary(cache: NotesCache(root: cacheRoot))
await library.attach(session)

// ---- Phase A: the pager's own Notes Sync (W12) ----------------------------
print("== Phase A: pager-initiated move (test.callback notes=github)")
let before = events.testResults.count
_ = try await session.call("test.callback", params: ["notes": "github"], timeout: .seconds(5))
let a = await waitTestResult(events, after: before, .seconds(30))
check(a?["ok"]?.boolValue == true, "pager notes sync ok", "\(String(describing: a))")
check(a?["uploaded"]?.intValue == 3 && a?["deleted"]?.intValue == 3 && a?["kept"]?.intValue == 0
      && a?["errors"]?.intValue == 0,
      "pager summary uploaded=3 deleted=3 kept=0 errors=0",
      "uploaded=\(a?["uploaded"]?.intValue ?? -1) deleted=\(a?["deleted"]?.intValue ?? -1) kept=\(a?["kept"]?.intValue ?? -1)")
let seeded = a?["seeded"]
var repoOK = true
for (label, name) in [("int/a-plain.txt", "a-plain.txt"), ("int/b-shared.txt", "b-shared.txt"),
                      ("int/c-secret.txt", "c-secret.txt")] {
    let want = hexBytes(seeded?[label]?.stringValue ?? "")
    if repo.notes()[name] != want || want.isEmpty { repoOK = false; print("       repo \(name) differs") }
}
check(repoOK, "repo holds the 3 internal notes byte for byte (c-secret.txt as Salted__ ciphertext)")
check(hexBytes(seeded?["int/c-secret.txt"]?.stringValue ?? "").starts(with: Array("Salted__".utf8)),
      "c-secret.txt went up as ciphertext")
var scan = try await library.refresh()
check(scan.all.filter { $0.vol == "int" }.isEmpty, "internal notes gone from the pager after the move",
      "\(scan.all.map(\.id))")
check(scan.merged.map(\.id) == ["sd:d-sdonly.txt", "sd:b-shared.txt"],
      "SD notes untouched (pager manifest is internal-only)", "\(scan.merged.map(\.id))")
check(reports.all.last?.deleted == 3, "phone session saw the pager's move (onPagerMove)")

// Second press of the pager's Sync button: its manifest is now empty.
let reportsBefore = reports.all.count
_ = try await session.call("test.callback", params: [
    "method": "notes.sync", "params": ["targets": ["github"], "files": []], "timeout_ms": 5000,
], timeout: .seconds(5))
let deadline = ContinuousClock.now + .seconds(10)
while reports.all.count == reportsBefore && ContinuousClock.now < deadline { try? await Task.sleep(for: .milliseconds(20)) }
check(reports.all.count > reportsBefore && reports.all.last!.nothingToDo, "second pager press: nothing to do")

// ---- Phase B: listing order (W7) + phone-initiated move (W14) ------------
print("== Phase B: phone-initiated move over int + sd")
let seed: [(String, String, String)] = [
    ("int", "2026-10-26_0900.txt", "morning note\n"),
    ("int", "b-shared.txt", "internal v2 of shared\n"),
    ("int", "Zeta.txt", "capital Z sorts before lower case\n"),
    ("int", "f-edit.txt", "will be edited during verify\n"),
]
for (vol, name, text) in seed { try await writeFile(session, vol, "/notes/" + name, Array(text.utf8)) }
scan = try await library.refresh()
check(scan.merged.map(\.id) == ["int:f-edit.txt", "sd:d-sdonly.txt", "int:b-shared.txt", "int:Zeta.txt",
                                "int:2026-10-26_0900.txt"],
      "Journal order: name descending by bytes, internal wins the b-shared clash", "\(scan.merged.map(\.id))")
check(scan.all.count == 6 && scan.all.contains { $0.id == "sd:b-shared.txt" },
      "unmerged list keeps the shadowed SD note")
let zeta = scan.merged.first { $0.name == "Zeta.txt" }!
let zetaBytes = try await library.read(zeta)
check(zetaBytes == Array("capital Z sorts before lower case\n".utf8), "read through NotesLibrary")
let fsChangedBefore = events.fsChangedCount

final class Edited: @unchecked Sendable { var done = false }
let edited = Edited()
let result = await service.move(scan.all) { step in
    if case .verifying = step, !edited.done {
        edited.done = true
        try? await writeFile(session, "int", "/notes/f-edit.txt", Array("edited during verify!\n".utf8))
    }
}
print("       result: uploaded=\(result.uploaded) already=\(result.already) deleted=\(result.deleted) kept=\(result.kept.map { "\($0.name): \($0.reason)" }) errors=\(result.errors.map { "\($0.name): \($0.reason)" })")
check(result.uploaded == 6 && result.deleted == 5 && result.errors.isEmpty,
      "move: 6 uploaded, 5 deleted, no errors")
check(result.kept.map(\.name) == ["f-edit.txt"] && result.kept.first?.reason == "changed on the pager",
      "note edited during verifying is kept with \"changed\"")
let names = Set(repo.notes().keys)
check(names.contains("b-shared.txt") && names.filter { $0.hasPrefix("b-shared~") }.count == 2,
      "both other b-shared copies went up under content names, nothing overwritten", "\(names.sorted())")
check(repo.notes()["b-shared.txt"] == hexBytes(seeded?["int/b-shared.txt"]?.stringValue ?? ""),
      "the first b-shared.txt on GitHub is unchanged")
scan = try await library.refresh()
check(scan.merged.map(\.id) == ["int:f-edit.txt"], "only the edited note is left on the pager",
      "\(scan.merged.map(\.id))")
let fsDeadline = ContinuousClock.now + .seconds(3)
while events.fsChangedCount <= fsChangedBefore && ContinuousClock.now < fsDeadline {
    try? await Task.sleep(for: .milliseconds(50))
}
print("       evt.fs.changed: \(fsChangedBefore) before the move, \(events.fsChangedCount) after")
check(events.fsChangedCount > fsChangedBefore, "evt.fs.changed fired for the deletes")

let second = await service.move(scan.all)
check(second.uploaded == 1 && second.deleted == 1 && repo.notes().keys.contains { $0.hasPrefix("f-edit~") },
      "next run moves the edited note under a content name")
scan = try await library.refresh()
let third = await service.move(scan.all)
check(scan.all.isEmpty && third.nothingToDo, "third run: nothing to do, the Notes tab is empty")
check(repo.commitPosts == 3, "one commit per run that uploaded (pager, phone, retry)", "\(repo.commitPosts)")

watcher.cancel()
await session.stop()
transport.close()
try? FileManager.default.removeItem(at: cacheRoot)
print(failures == 0 ? "notes-e2e: all checks passed" : "notes-e2e: \(failures) failed")
exit(Int32(failures))
