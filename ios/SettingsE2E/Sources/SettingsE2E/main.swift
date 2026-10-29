import Foundation
import PagerKit

// End-to-end check of the settings registry against the real emulator firmware
// over TCP 7600 (IOS_SETTINGS_PLAN.md W12). `write` sets every writable setting
// over PagerLink, reads it back and saves the expected values; the emulator is
// then killed (-9) and restarted by run.sh, and `verify` reads everything back
// again. Exit status = number of failed checks.

let usage = "usage: SettingsE2E write|verify <state.json>"

nonisolated(unsafe) var failures = 0
func check(_ ok: Bool, _ label: String, _ detail: @autoclosure () -> String = "") {
    print(ok ? "[PASS] \(label)" : "[FAIL] \(label) \(detail())")
    if !ok { failures += 1 }
}

/// Value equality for a read-back. weather.location is compared on its "name"
/// member only: the pager may add or normalise lat/lon around it.
func same(_ id: String, _ expected: SettingValue?, _ got: SettingValue?) -> Bool {
    guard let expected, let got else { return false }
    if id == "weather.location" {
        if case .json(let want) = expected, case .json(let have) = got {
            return want["name"] != nil && want["name"] == have["name"]
        }
    }
    return expected == got
}

/// The string a `string` setting is written with.
func stringTarget(_ id: String) -> String {
    switch id {
    case "hub.url": return "http://e2e.local:8080"
    case "tg.url": return "https://tg.e2e.local"
    case "nsync.repo": return "e2e-owner/e2e-notes"
    case "nsync.branch": return "e2e"
    case "ssh.host": return "e2e.local"
    case "ssh.user": return "e2e"
    case "ssh.hosts": return "lab=e2e@e2e.local:2222; box=e2e@box.e2e.local"
    default: return "e2e"
    }
}

/// A value for `item` that differs from `current`, or nil when the setting is
/// not covered (a list other than tg.favorites).
func writeTarget(_ item: SettingItem, current: SettingValue?) -> SettingValue? {
    switch item.kind {
    case .bool:
        return .bool(!(current?.boolValue ?? false))
    case .int:
        let lo = item.min ?? 0
        let hi = item.max ?? lo
        let step = item.step ?? 1
        let now = current?.intValue ?? lo
        let next = now == lo ? Swift.min(lo + step, hi) : lo
        return .int(Swift.min(Swift.max(next, lo), hi))
    case .enumeration:
        // time.tz reads best as a real zone; still move off the current one.
        if item.id == "time.tz", let lisbon = item.options.first(where: { $0.value == .string("Europe/Lisbon") }),
           lisbon.value != current {
            return lisbon.value
        }
        return item.options.first { $0.value != current }?.value
    case .string:
        return .string(stringTarget(item.id))
    case .object:
        if item.id == "weather.location" {
            return .json(.object([
                "name": .string("Lisbon"), "lat": .number(38.72), "lon": .number(-9.14),
            ]))
        }
        return nil
    case .list:
        if item.id == "tg.favorites" {
            return .json(.array([.object(["id": .number(424242), "title": .string("E2E chat")])]))
        }
        return nil
    case .secret:
        return nil
    }
}

// ---- Phase write: set, read back, save the expected values ----------------
func writePhase(_ svc: SettingsService, _ schema: SettingsSchema, _ snapshot: SettingsSnapshot,
                _ path: String) async throws {
    print("== Phase write: set every writable setting and read it back")
    var expected: [String: JSONValue] = [:]
    for item in schema.items where !item.readOnly && item.kind != .secret && item.id != "power.otg" {
        guard let target = writeTarget(item, current: snapshot.values[item.id]) else { continue }
        do {
            let before = try await svc.get(ids: [item.id]).rev
            let result = try await svc.set([item.id: target], ifRev: before)
            check(result.applied.contains(item.id), "set \(item.id)",
                  "applied \(result.applied)")
            let back = try await svc.get(ids: [item.id])
            check(same(item.id, target, back.values[item.id]), "read back \(item.id)",
                  "expected \(String(describing: target)) got \(String(describing: back.values[item.id]))")
            if let wire = target.wire { expected[item.id] = wire }
        } catch {
            check(false, "set \(item.id)", "\(error)")
        }
    }

    // power.otg is live hardware state, not saved across a reboot: read it back
    // but leave it out of the state file.
    if let otg = schema.item("power.otg"), !otg.readOnly {
        let target = SettingValue.bool(!(snapshot.values["power.otg"]?.boolValue ?? false))
        do {
            let before = try await svc.get(ids: ["power.otg"]).rev
            let result = try await svc.set(["power.otg": target], ifRev: before)
            check(result.applied.contains("power.otg"), "set power.otg")
            let back = try await svc.get(ids: ["power.otg"])
            check(same("power.otg", target, back.values["power.otg"]), "read back power.otg",
                  "expected \(String(describing: target)) got \(String(describing: back.values["power.otg"]))")
        } catch let error as RPCError where error.code == "internal" {
            // The emulator's charger stub has no OTG: the refusal path, and
            // the value must be unchanged.
            let back = try? await svc.get(ids: ["power.otg"])
            check(back?.values["power.otg"] == snapshot.values["power.otg"],
                  "power.otg refused (\(error.msg)), value unchanged")
        } catch {
            check(false, "set power.otg", "\(error)")
        }
    }

    // A secret with no notes encryption on the pager: refused, and still unset.
    if schema.item("nsync.token") != nil {
        do {
            _ = try await svc.set(["nsync.token": .string("e2e-not-a-token")])
            check(false, "nsync.token is refused while notes are locked", "no error")
        } catch let error as RPCError {
            check(error.code == "unauthorized", "nsync.token is refused while notes are locked",
                  "\(error.code): \(error.msg)")
        } catch {
            check(false, "nsync.token is refused while notes are locked", "\(error)")
        }
        let token = try await svc.get(ids: ["nsync.token"])
        check(token.values["nsync.token"] == .secret(set: false), "nsync.token is still unset",
              "\(String(describing: token.values["nsync.token"]))")
    }

    // An unknown id: the whole set is refused.
    do {
        _ = try await svc.set(["nope.e2e": .int(1)])
        check(false, "an unknown setting is refused", "no error")
    } catch let error as RPCError {
        check(error.code == "invalid_argument", "an unknown setting is refused",
              "\(error.code): \(error.msg)")
    } catch {
        check(false, "an unknown setting is refused", "\(error)")
    }

    let bytes = try JSONValue.object(expected).encodedSorted()
    let url = URL(fileURLWithPath: path)
    try FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                            withIntermediateDirectories: true)
    try Data(bytes).write(to: url)
    print("       \(expected.count) values saved to \(path)")
}

// ---- Phase verify: everything still there after the restart --------------
func verifyPhase(_ svc: SettingsService, _ schema: SettingsSchema, _ path: String) async throws {
    print("== Phase verify: read every saved value back after the restart")
    let bytes = [UInt8](try Data(contentsOf: URL(fileURLWithPath: path)))
    guard case .object(let saved) = try JSONValue.decode(bytes: bytes) else {
        throw RPCError(code: "invalid_argument", msg: "\(path): not a JSON object")
    }
    let snapshot = try await svc.get()
    for (id, wire) in saved.sorted(by: { $0.key < $1.key }) {
        let expected = SettingValue(wire: wire, type: schema.item(id)?.kind)
        check(same(id, expected, snapshot.values[id]), "after restart \(id)",
              "expected \(wire) got \(String(describing: snapshot.values[id]))")
    }
    print("       \(saved.count) settings compared")
}

let args = CommandLine.arguments
guard args.count == 3, args[1] == "write" || args[1] == "verify" else {
    print(usage)
    exit(2)
}
let phase = args[1]
let statePath = args[2]

var transport: POSIXTransport?
var session: LinkSession?
do {
    let t = try POSIXTransport(host: "127.0.0.1", port: 7600)
    transport = t
    let s = LinkSession(transport: t)
    session = s
    await s.start()

    let hello = try await s.call("sys.hello", params: [
        "proto": 1, "app": "settings-e2e", "caps": ["sys", "settings"],
    ], timeout: .seconds(5))
    var caps: [String] = []
    if case .array(let raw)? = hello["caps"] { caps = raw.compactMap(\.stringValue) }
    check(caps.contains(SettingsService.cap), "hello caps include settings", "\(caps)")

    let svc = SettingsService(session: s)
    let schema = try await svc.schema()
    let snap = try await svc.get()

    if phase == "write" {
        try await writePhase(svc, schema, snap, statePath)
    } else {
        try await verifyPhase(svc, schema, statePath)
    }
} catch {
    check(false, "unexpected error", "\(error)")
}

await session?.stop()
transport?.close()
print(failures == 0 ? "settings-e2e \(phase): all checks passed" : "settings-e2e \(phase): \(failures) failed")
exit(Int32(failures))
