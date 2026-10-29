import Foundation
import XCTest

@testable import PagerKit

/// `<repo>/protocol/vectors/<name>.json` as a JSONValue.
func loadVector(_ name: String, file: String = #filePath) throws -> JSONValue {
    var url = URL(fileURLWithPath: file)
    for _ in 0..<5 { url.deleteLastPathComponent() }
    url = url.appendingPathComponent("protocol/vectors/\(name).json")
    return try JSONValue.decode(bytes: Array(try Data(contentsOf: url)))
}

func hexBytes(_ s: String) -> [UInt8] {
    var out: [UInt8] = []
    var it = s.makeIterator()
    while let a = it.next(), let b = it.next() { out.append(UInt8(String([a, b]), radix: 16)!) }
    return out
}

/// Collects what the pager → phone handlers delivered.
private final class Inbox: @unchecked Sendable {
    private let lock = NSLock()
    private var clips: [String] = []
    private var finds = 0
    func clip(_ text: String) { lock.withLock { clips.append(text) } }
    func find() { lock.withLock { finds += 1 } }
    var allClips: [String] { lock.withLock { clips } }
    var findCount: Int { lock.withLock { finds } }
}

/// The PROTOCOL.md §6.5 rules against `vectors/messaging.json` (every case,
/// from its `params_json`, like the Go test), `MessagingService` calls and
/// handlers over a loopback session, shared-content messages and the outbox.
final class MessagingTests: XCTestCase {
    func testEveryMessagingVectorVerdict() throws {
        let vectors = try loadVector("messaging")
        guard case .array(let cases)? = vectors["cases"] else { return XCTFail("no cases") }
        XCTAssertGreaterThan(cases.count, 30)
        for c in cases {
            let name = c["name"]?.stringValue ?? "?"
            let method = try XCTUnwrap(c["method"]?.stringValue)
            let params = try JSONValue.decode(bytes: Array(try XCTUnwrap(c["params_json"]?.stringValue).utf8))
            let verdict = MessagingRules.check(method, params)
            if let error = c["error"] {
                XCTAssertEqual(error["code"]?.stringValue, "invalid_argument", name)
                XCTAssertEqual(verdict, error["msg"]?.stringValue, name)
            } else {
                XCTAssertNil(verdict, name)
            }
        }
        let limits = try XCTUnwrap(vectors["limits"])
        XCTAssertEqual(limits["title"]?.intValue, MessagingRules.titleMax)
        XCTAssertEqual(limits["body"]?.intValue, MessagingRules.bodyMax)
        XCTAssertEqual(limits["id"]?.intValue, MessagingRules.idMax)
        XCTAssertEqual(limits["src"]?.intValue, MessagingRules.srcMax)
        XCTAssertEqual(limits["clip"]?.intValue, MessagingRules.clipMax)
        XCTAssertEqual(limits["text_send"]?.intValue, MessagingRules.textSendMax)
        XCTAssertEqual(MessagingRules.check("x", "not an object"), "params: expected object")
    }

    private func pair() async -> (phone: LinkSession, pager: LinkSession) {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()
        return (phone, pager)
    }

    func testServiceCallsAndResults() async throws {
        let (phone, pager) = await pair()
        let seen = Inbox()
        await pager.handle("notify.post") { p in
            let id = p["id"]?.stringValue
            return id == "dup-1" ? ["stored": false, "dup": true] : ["stored": true]
        }
        await pager.handle("clip.push") { p in ["pasted": .bool(p["text"]?.stringValue == "into editor")] }
        await pager.handle("text.send") { p in
            if p["target"]?.stringValue == "editor" { throw RPCError(code: "invalid_argument", msg: "editor not open") }
            return ["typed": .number(Double(p["text"]?.stringValue?.utf8.count ?? 0))]
        }
        await pager.handle("pager.find") { p in
            seen.clip(p["stop"]?.boolValue == true ? "stop" : "start")
            return .object([:])
        }
        let messaging = MessagingService(session: phone, timeout: .seconds(5))

        let stored = try await messaging.notifyPost(NotifyMessage(title: "Hi", body: "a\nb", id: "x-1", prio: .high))
        XCTAssertEqual(stored, NotifyResult(stored: true, dup: false))
        let dup = try await messaging.notifyPost(NotifyMessage(title: "Hi", id: "dup-1"))
        XCTAssertEqual(dup, NotifyResult(stored: false, dup: true))
        let pasted = try await messaging.clipPush("into editor")
        XCTAssertTrue(pasted)
        let kept = try await messaging.clipPush("elsewhere")
        XCTAssertFalse(kept)
        let typed = try await messaging.textSend("héllo", target: .kb)
        XCTAssertEqual(typed, 6)
        do {
            _ = try await messaging.textSend("x", target: .editor)
            XCTFail("editor closed")
        } catch let error as RPCError {
            XCTAssertEqual(error.msg, "editor not open")
        }
        try await messaging.findPager()
        try await messaging.findPager(stop: true)
        XCTAssertEqual(seen.allClips, ["start", "stop"])

        // Refused on the phone, before anything is sent.
        do {
            _ = try await messaging.notifyPost(NotifyMessage(title: String(repeating: "x", count: 65)))
            XCTFail("long title sent")
        } catch let error as RPCError {
            XCTAssertEqual(error, RPCError(code: "invalid_argument", msg: "title: too long"))
        }
        do {
            _ = try await messaging.clipPush("a\rb")
            XCTFail("CR sent")
        } catch let error as RPCError {
            XCTAssertEqual(error.msg, "text: control character")
        }
    }

    func testPagerToPhoneHandlers() async throws {
        let (phone, pager) = await pair()
        let inbox = Inbox()
        await MessagingService(session: phone).register(onClip: { inbox.clip($0) }, onFind: { inbox.find() })

        _ = try await pager.call("clip.push", params: ["text": "copy\tthis\nplease"], timeout: .seconds(5))
        _ = try await pager.call("phone.find", timeout: .seconds(5))
        XCTAssertEqual(inbox.allClips, ["copy\tthis\nplease"])
        XCTAssertEqual(inbox.findCount, 1)
        do {
            _ = try await pager.call("clip.push", params: ["text": ""], timeout: .seconds(5))
            XCTFail("empty clip accepted")
        } catch let error as RPCError {
            XCTAssertEqual(error, RPCError(code: "invalid_argument", msg: "text: empty"))
        }
        XCTAssertEqual(inbox.allClips.count, 1)
    }

    func testSharedContentAlwaysPassesTheRules() {
        let url = URL(string: "https://example.com/a?b=c")!
        let plain = NotifyMessage.shared(text: "Buy milk\r\nand eggs\tplease", url: nil, src: "Notes")
        XCTAssertEqual(plain.title, "Buy milk")
        XCTAssertEqual(plain.body, "and eggs please")
        XCTAssertEqual(plain.src, "Notes")

        let link = NotifyMessage.shared(text: nil, url: url, src: "Safari")
        XCTAssertEqual(link.title, "example.com")
        XCTAssertEqual(link.body, url.absoluteString)

        let long = NotifyMessage.shared(text: String(repeating: "é", count: 100) + "\n" + String(repeating: "b", count: 900),
                                        url: url, src: String(repeating: "s", count: 40), id: "share-1", ts: 5)
        XCTAssertLessThanOrEqual(long.title.utf8.count, 64)
        XCTAssertTrue(long.title.hasSuffix("..."))
        XCTAssertLessThanOrEqual(long.body?.utf8.count ?? 0, 512)

        for message in [plain, link, long, NotifyMessage.shared(text: "  \n ", url: nil, src: nil)] {
            XCTAssertNil(MessagingRules.check("notify.post", .object(message.params)), "\(message)")
        }
        XCTAssertEqual(MessagingRules.truncate("aé", maxBytes: 2), "a")
        XCTAssertNil(MessagingRules.check("notify.post", ["title": "t", "id": .string(NotifyMessage.newID())]))
    }

    func testOutboxFlushes() async throws {
        let suite = "outbox-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let store = PagerOutboxStore(defaults: defaults)
        let t0 = Date(timeIntervalSince1970: 1_761_700_000)
        store.enqueue(OutboxMessage(id: "share-a", title: "one", createdAt: t0))
        store.enqueue(OutboxMessage(id: "share-b", title: "two", body: "x", createdAt: t0))
        store.enqueue(OutboxMessage(id: "share-c", title: "three", createdAt: t0))
        store.enqueue(OutboxMessage(id: "share-d", title: "four", createdAt: t0))
        XCTAssertEqual(PagerOutboxStore(defaults: defaults).pending().map(\.id), ["share-a", "share-b", "share-c", "share-d"])
        XCTAssertEqual(store.pending().first?.notify.ts, 1_761_700_000)

        // a: sent, b: already there, c: refused for good, d: link drops.
        let report = await PagerOutbox.flush(store) { message in
            switch message.id {
            case "share-a": return NotifyResult(stored: true, dup: false)
            case "share-b": return NotifyResult(stored: false, dup: true)
            case "share-c": throw RPCError(code: "invalid_argument", msg: "title: too long")
            default: throw RPCError.disconnected()
            }
        }
        XCTAssertEqual(report.sent, 1)
        XCTAssertEqual(report.duplicates, 1)
        XCTAssertEqual(report.dropped, ["title: too long"])
        XCTAssertEqual(report.stoppedBy?.code, "disconnected")
        XCTAssertEqual(store.pending().map(\.id), ["share-d"], "the unsent one stays queued, same id")

        for i in 0..<60 { store.enqueue(OutboxMessage(id: "m-\(i)", title: "t")) }
        XCTAssertEqual(store.pending().count, PagerOutboxStore.capacity)
        XCTAssertEqual(store.pending().last?.id, "m-59")
        XCTAssertEqual(PagerStatusStore.appGroupID(bundleID: "dev.lilygo.pagercompanion.share"),
                       "group.dev.lilygo.pagercompanion.shared")
    }

    // MARK: - SSH key

    func testSSHKeyCheck() {
        let ed25519 = """
            -----BEGIN OPENSSH PRIVATE KEY-----
            \(Data(Array("openssh-key-v1".utf8) + [0, 0, 0, 0, 4] + Array("none".utf8) + [1, 2, 3]).base64EncodedString())
            -----END OPENSSH PRIVATE KEY-----
            """
        XCTAssertEqual(SSHKeyCheck.openSSHCipher(ed25519), "none")
        XCTAssertNil(SSHKeyCheck.problem(SSHKeyCheck.normalize(ed25519)))
        XCTAssertTrue(SSHKeyCheck.normalize("a\r\nb\r\n\n").hasSuffix("a\nb\n"))

        let encrypted = ed25519.replacingOccurrences(
            of: Data(Array("openssh-key-v1".utf8) + [0, 0, 0, 0, 4] + Array("none".utf8) + [1, 2, 3]).base64EncodedString(),
            with: Data(Array("openssh-key-v1".utf8) + [0, 0, 0, 0, 10] + Array("aes256-ctr".utf8)).base64EncodedString())
        XCTAssertEqual(SSHKeyCheck.openSSHCipher(encrypted), "aes256-ctr")
        XCTAssertNotNil(SSHKeyCheck.problem(encrypted)?.range(of: "passphrase"))

        let pem = "-----BEGIN RSA PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\nabc\n-----END RSA PRIVATE KEY-----\n"
        XCTAssertNotNil(SSHKeyCheck.problem(pem)?.range(of: "passphrase"))
        XCTAssertNotNil(SSHKeyCheck.problem("ssh-ed25519 AAAA… me@host")?.range(of: "not a private key"))
        let big = "-----BEGIN RSA PRIVATE KEY-----\n" + String(repeating: "A", count: 3000) + "\n-----END RSA PRIVATE KEY-----\n"
        XCTAssertNotNil(SSHKeyCheck.problem(big)?.range(of: "at most 3000"))
        XCTAssertEqual(SSHKeyCheck.problem(" "), "The key is empty.")
    }
}

/// `vectors/screen.json` through `RLE565`, RGB565 → RGBA8, and
/// `ScreenCaptureService` against a pager that streams a capture.
final class ScreenCaptureTests: XCTestCase {
    private func pixels(_ hex: String) -> [UInt16] {
        let b = hexBytes(hex)
        return stride(from: 0, to: b.count, by: 2).map { UInt16(b[$0]) | UInt16(b[$0 + 1]) << 8 }
    }

    func testScreenVectors() throws {
        let vectors = try loadVector("screen")
        XCTAssertEqual(vectors["fmt"]?.stringValue, RLE565.format)
        guard case .array(let images)? = vectors["images"], case .array(let malformed)? = vectors["malformed"] else {
            return XCTFail("vector shape")
        }
        for image in images {
            let name = image["name"]?.stringValue ?? "?"
            let w = try XCTUnwrap(image["w"]?.intValue), h = try XCTUnwrap(image["h"]?.intValue)
            let rle = hexBytes(try XCTUnwrap(image["rle_hex"]?.stringValue))
            let expected: [UInt16]
            if let fill = image["fill"]?.intValue {
                expected = Array(repeating: UInt16(fill), count: w * h)
            } else {
                expected = pixels(try XCTUnwrap(image["pixels_hex"]?.stringValue))
            }
            XCTAssertEqual(try RLE565.decode(rle, width: w, height: h), expected, name)
            XCTAssertEqual(RLE565.encode(expected), rle, name)
        }
        let errors: [String: RLE565.DecodeError] = [
            "odd_length": .lengthNotMultipleOf4, "zero_count": .zeroLengthRun,
            "too_few": .fewerPixelsThanImage, "too_many": .morePixelsThanImage,
        ]
        XCTAssertEqual(malformed.count, errors.count)
        for bad in malformed {
            let name = bad["name"]?.stringValue ?? "?"
            XCTAssertThrowsError(try RLE565.decode(hexBytes(bad["rle_hex"]?.stringValue ?? ""),
                                                   width: bad["w"]?.intValue ?? 0, height: bad["h"]?.intValue ?? 0)) {
                XCTAssertEqual($0 as? RLE565.DecodeError, errors[name], name)
            }
        }
    }

    func testRGBAExpansion() {
        // White, red, green, blue, and a mid value: v * 255 / max, integer division.
        let rgba = RLE565.rgba([0xFFFF, 0xF800, 0x07E0, 0x001F, 0x0841])
        XCTAssertEqual(Array(rgba[0..<4]), [255, 255, 255, 255])
        XCTAssertEqual(Array(rgba[4..<8]), [255, 0, 0, 255])
        XCTAssertEqual(Array(rgba[8..<12]), [0, 255, 0, 255])
        XCTAssertEqual(Array(rgba[12..<16]), [0, 0, 255, 255])
        XCTAssertEqual(Array(rgba[16..<20]), [8, 8, 8, 255])  // r=1 → 8, g=2 → 8, b=1 → 8
    }

    private func capturePair(rle: [UInt8], w: Int, h: Int, overtake: Bool = false, fmt: String = "rgb565-rle")
        async -> (ScreenCaptureService, LinkSession) {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        let service = ScreenCaptureService(session: phone, timeout: .seconds(5))
        await service.register()
        await pager.handle("screen.capture") { _ in
            let send: @Sendable () async throws -> Void = {
                let reply = try await pager.call("screen.data", params: [
                    "job": "7", "w": .number(Double(w)), "h": .number(Double(h)), "fmt": .string(fmt),
                    "size": .number(Double(rle.count)), "crc32": .number(Double(CRC32.checksum(rle))),
                ], timeout: .seconds(5))
                try await pager.sendBytes(xfer: UInt16(reply["xfer"]!.intValue!), data: rle, timeout: .seconds(5))
            }
            if overtake { try await send() } else { Task { try? await send() } }
            return ["job": "7"]
        }
        await phone.start()
        await pager.start()
        return (service, pager)
    }

    func testCaptureDecodesTheStream() async throws {
        var px: [UInt16] = []
        for y in 0..<222 { for x in 0..<480 { px.append(UInt16((x / 40 + y / 37) % 3 == 0 ? 0xF800 : 0x001F)) } }
        let rle = RLE565.encode(px)
        for overtake in [false, true] {
            let (service, _) = await capturePair(rle: rle, w: 480, h: 222, overtake: overtake)
            let image = try await service.capture()
            XCTAssertEqual(image.width, 480)
            XCTAssertEqual(image.height, 222)
            XCTAssertEqual(image.pixels, px, "overtake \(overtake)")
            XCTAssertEqual(image.rgba.count, 480 * 222 * 4)
        }
    }

    func testCaptureFailures() async throws {
        let (bad, _) = await capturePair(rle: [3, 0, 0xFF, 0xFF], w: 2, h: 2)
        do {
            _ = try await bad.capture()
            XCTFail("short image decoded")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
        }
        let (otherFmt, pager) = await capturePair(rle: [1, 0, 0, 0], w: 1, h: 1, fmt: "png")
        _ = otherFmt
        do {
            _ = try await pager.call("screen.data", params: [
                "job": "8", "w": 1, "h": 1, "fmt": "png", "size": 4, "crc32": 0,
            ], timeout: .seconds(5))
            XCTFail("unsupported fmt accepted")
        } catch let error as RPCError {
            XCTAssertEqual(error.msg, "fmt: unsupported")
        }
    }
}
