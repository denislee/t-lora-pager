import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
import XCTest

@testable import PagerKit

/// A mutable HTTP status the stub closure reads synchronously. `StubServer`'s
/// handler is a synchronous `@Sendable` closure, so the mutable state it needs
/// lives here behind an `NSLock` rather than being captured from the test.
private final class StatusBox: @unchecked Sendable {
    private let lock = NSLock()
    private var value: Int

    init(_ status: Int) {
        self.value = status
    }

    var status: Int {
        get {
            lock.lock()
            defer { lock.unlock() }
            return value
        }
        set {
            lock.lock()
            value = newValue
            lock.unlock()
        }
    }
}

/// End-to-end coverage of `TelegramService` against the `StubURLProtocol`
/// bridge: it checks the exact request the phone makes (path, bearer, POST
/// body), the field trimming the pager depends on, the parameter validation
/// mirrored from `tgbridge.go`, the HTTP status mapping, and the bulk-reply
/// switch-over — bodies over 4096 B stream as DATA under the pager's `xfer`,
/// inline otherwise, and are refused when no `xfer` was offered.
final class TelegramServiceTests: XCTestCase {

    /// Drive `tg.chats` and assert the ERR it produced. `msg` is optional so a
    /// case can assert the code alone.
    private func expectChatsError(
        _ pager: LinkSession,
        _ params: [String: JSONValue],
        code: String,
        msg: String? = nil,
        file: StaticString = #filePath,
        line: UInt = #line
    ) async {
        do {
            _ = try await pager.call("tg.chats", params: params, timeout: .seconds(5))
            XCTFail("expected error", file: file, line: line)
        } catch let error as RPCError {
            XCTAssertEqual(error.code, code, file: file, line: line)
            if let msg = msg {
                XCTAssertEqual(error.msg, msg, file: file, line: line)
            }
        } catch {
            XCTFail("unexpected error: \(error)", file: file, line: line)
        }
    }

    /// Builds `count` chat records with a 100-byte title and an extra `junk`
    /// field, the shape that forces a bulk (over-4096-B) reply.
    private func bigChatsJSON(_ count: Int) -> String {
        let title = String(repeating: "x", count: 100)
        var items: [String] = []
        for i in 0..<count {
            items.append("{\"id\":\(i),\"title\":\"\(title)\",\"unread\":0,\"junk\":\"yyyy\"}")
        }
        return "[" + items.joined(separator: ",") + "]"
    }

    // MARK: - 1. Chats

    func testChatsPathAuthAndTrim() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        let json = #"[{"id":101,"title":"Alice","unread":2,"extra":"x","photo":{"big":1}},{"id":-100200,"title":"Team","unread":0}]"#
        let server = StubServer { _ in StubResponse.json(200, json) }
        let svc = TelegramService(session: phone, urlSession: StubURLProtocol.session(server))
        await svc.register()

        let result = try await pager.call("tg.chats", params: [
            "bridge": "http://bridge.local:8080/",
            "token": "secret",
            "limit": 20,
        ], timeout: .seconds(5))

        let requests = server.requests
        XCTAssertEqual(requests.count, 1)
        XCTAssertEqual(requests.first?.method, "GET")
        XCTAssertEqual(requests.first?.url.absoluteString, "http://bridge.local:8080/v1/chats?limit=20")
        XCTAssertEqual(requests.first?.header("Authorization"), "Bearer secret")

        guard case .array(let chats)? = result["body"] else {
            return XCTFail("expected body array")
        }
        XCTAssertEqual(chats.count, 2)
        guard case .object(let first) = chats[0] else { return XCTFail("expected object") }
        XCTAssertEqual(Set(first.keys), ["id", "title", "unread"])
        XCTAssertEqual(chats[0]["id"]?.intValue, 101)
        XCTAssertEqual(chats[0]["title"]?.stringValue, "Alice")
        XCTAssertEqual(chats[0]["unread"]?.intValue, 2)
        XCTAssertEqual(chats[1]["id"]?.intValue, -100200)

        await phone.stop()
        await pager.stop()
    }

    // MARK: - 2. Messages

    func testMessagesTrimMediaOnlyWhenPresent() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        let json = #"[{"id":3,"text":"are you there?","from":"Alice","out":false,"date":123},{"id":2,"text":"","from":"Alice","out":false,"media":{"type":"photo","w":800,"h":600,"file_id":"zzz"}}]"#
        let server = StubServer { _ in StubResponse.json(200, json) }
        let svc = TelegramService(session: phone, urlSession: StubURLProtocol.session(server))
        await svc.register()

        let result = try await pager.call("tg.messages", params: [
            "bridge": "https://b.example",
            "token": "t",
            "chat_id": 101,
            "limit": 5,
        ], timeout: .seconds(5))

        XCTAssertEqual(server.requests.first?.url.absoluteString,
                       "https://b.example/v1/chats/101/messages?limit=5")

        guard case .array(let messages)? = result["body"] else {
            return XCTFail("expected body array")
        }
        XCTAssertEqual(messages.count, 2)
        guard case .object(let first) = messages[0] else { return XCTFail("expected object") }
        XCTAssertEqual(Set(first.keys), ["id", "text", "from", "out"])

        guard case .object(let second) = messages[1] else { return XCTFail("expected object") }
        XCTAssertEqual(Set(second.keys), ["id", "text", "from", "out", "media"])
        guard let media = second["media"] else { return XCTFail("expected media") }
        XCTAssertEqual(media,
                       JSONValue.object(["type": .string("photo"), "w": .number(800), "h": .number(600)]))

        await phone.stop()
        await pager.stop()
    }

    // MARK: - 3. Non-integer chat_id

    func testNonIntegerChatIdRejected() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        let server = StubServer { _ in StubResponse.json(200, "[]") }
        let svc = TelegramService(session: phone, urlSession: StubURLProtocol.session(server))
        await svc.register()

        do {
            _ = try await pager.call("tg.messages", params: [
                "bridge": "http://b", "token": "t", "limit": 5, "chat_id": "101",
            ], timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
            XCTAssertEqual(error.msg, "chat_id must be an integer")
        }

        do {
            _ = try await pager.call("tg.messages", params: [
                "bridge": "http://b", "token": "t", "limit": 5, "chat_id": 7.5,
            ], timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
            XCTAssertEqual(error.msg, "chat_id must be an integer")
        }

        XCTAssertTrue(server.requests.isEmpty)

        await phone.stop()
        await pager.stop()
    }

    // MARK: - 4. Param validation

    func testParamValidation() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        let server = StubServer { _ in StubResponse.json(200, "[]") }
        let svc = TelegramService(session: phone, urlSession: StubURLProtocol.session(server))
        await svc.register()

        await expectChatsError(pager,
            ["bridge": "http://b", "token": "t", "limit": 0],
            code: "invalid_argument", msg: "limit must be 1..100")
        await expectChatsError(pager,
            ["bridge": "http://b", "token": "t", "limit": 101],
            code: "invalid_argument", msg: "limit must be 1..100")
        await expectChatsError(pager,
            ["bridge": "http://b", "token": "t", "limit": 20, "xfer": 4],
            code: "invalid_argument", msg: "xfer must be an odd 1..65535")
        await expectChatsError(pager,
            ["bridge": "http://b"],
            code: "invalid_argument", msg: "bridge and token are required")
        await expectChatsError(pager,
            ["bridge": "file:///etc/passwd", "token": "t", "limit": 20],
            code: "invalid_argument")

        XCTAssertTrue(server.requests.isEmpty)

        await phone.stop()
        await pager.stop()
    }

    // MARK: - 5. Status mapping

    func testStatusMapping() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        let box = StatusBox(401)
        let server = StubServer { _ in StubResponse.json(box.status, "{}") }
        let svc = TelegramService(session: phone, urlSession: StubURLProtocol.session(server))
        await svc.register()

        box.status = 401
        do {
            _ = try await pager.call("tg.chats", params: [
                "bridge": "http://b", "token": "secret", "limit": 1,
            ], timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "unauthorized")
            XCTAssertFalse(error.msg.contains("secret"))
        }

        box.status = 403
        do {
            _ = try await pager.call("tg.chats", params: [
                "bridge": "http://b", "token": "secret", "limit": 1,
            ], timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "unauthorized")
        }

        box.status = 502
        do {
            _ = try await pager.call("tg.chats", params: [
                "bridge": "http://b", "token": "secret", "limit": 1,
            ], timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "http_error")
            XCTAssertEqual(error.msg, "502")
        }

        box.status = 404
        do {
            _ = try await pager.call("tg.read", params: [
                "bridge": "http://b", "token": "secret", "chat_id": 101, "up_to": 3,
            ], timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "http_error")
            XCTAssertEqual(error.msg, "404")
        }

        await phone.stop()
        await pager.stop()
    }

    // MARK: - 6. Bulk switch-over

    func testBulkSwitchOver4096() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        let json = bigChatsJSON(60)
        let server = StubServer { _ in StubResponse.json(200, json) }
        let svc = TelegramService(session: phone, urlSession: StubURLProtocol.session(server))
        await svc.register()

        let id = await pager.openIncoming()
        XCTAssertEqual(id % 2, 1)

        let result = try await pager.call("tg.chats", params: [
            "bridge": "http://b", "token": "t", "limit": 100, "xfer": .number(Double(id)),
        ], timeout: .seconds(5))

        XCTAssertNil(result["body"])
        XCTAssertEqual(result["xfer"]?.intValue, Int(id))
        guard let size = result["size"]?.intValue, let crc = result["crc32"]?.intValue else {
            return XCTFail("expected size and crc32")
        }
        XCTAssertGreaterThan(size, 4096)

        let bytes = try await pager.receiveBytes(
            xfer: id, size: UInt32(size), crc32: UInt32(crc), timeout: .seconds(5))
        let decoded = try JSONValue.decode(bytes: bytes)
        guard case .array(let chats) = decoded else { return XCTFail("expected array") }
        XCTAssertEqual(chats.count, 60)
        for chat in chats {
            guard case .object(let object) = chat else { return XCTFail("expected object") }
            XCTAssertEqual(Set(object.keys), ["id", "title", "unread"])
        }
        XCTAssertEqual(CRC32.checksum(bytes), UInt32(crc))

        await phone.stop()
        await pager.stop()
    }

    // MARK: - 7. Bulk without xfer

    func testBulkWithoutXferRefused() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        let json = bigChatsJSON(60)
        let server = StubServer { _ in StubResponse.json(200, json) }
        let svc = TelegramService(session: phone, urlSession: StubURLProtocol.session(server))
        await svc.register()

        do {
            _ = try await pager.call("tg.chats", params: [
                "bridge": "http://b", "token": "t", "limit": 100,
            ], timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
            XCTAssertEqual(error.msg, "reply over 4096 B needs xfer")
        }

        await phone.stop()
        await pager.stop()
    }

    // MARK: - 8. Small reply stays inline

    func testSmallReplyStaysInlineEvenWithXfer() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        let server = StubServer { _ in StubResponse.json(200, #"[{"id":1,"title":"T","unread":0}]"#) }
        let svc = TelegramService(session: phone, urlSession: StubURLProtocol.session(server))
        await svc.register()

        let result = try await pager.call("tg.chats", params: [
            "bridge": "http://b", "token": "t", "limit": 100, "xfer": 9,
        ], timeout: .seconds(5))

        XCTAssertNotNil(result["body"])
        XCTAssertNil(result["xfer"])

        await phone.stop()
        await pager.stop()
    }

    // MARK: - 9. Read and send POST bodies

    func testReadAndSendPostBodies() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        let server = StubServer { _ in StubResponse.json(200, "{}") }
        let svc = TelegramService(session: phone, urlSession: StubURLProtocol.session(server))
        await svc.register()

        let readResult = try await pager.call("tg.read", params: [
            "bridge": "http://b", "token": "t", "chat_id": 101, "up_to": 3,
        ], timeout: .seconds(5))
        XCTAssertEqual(readResult, .object([:]))

        guard let readRequest = server.requests(path: "/read").first else {
            return XCTFail("expected read request")
        }
        XCTAssertEqual(readRequest.method, "POST")
        XCTAssertEqual(readRequest.url.absoluteString, "http://b/v1/chats/101/read")
        XCTAssertEqual(readRequest.header("Content-Type"), "application/json")
        XCTAssertEqual(try JSONValue.decode(bytes: Array(readRequest.body)), ["up_to": 3])

        let sendResult = try await pager.call("tg.send", params: [
            "bridge": "http://b", "token": "t", "chat_id": -100200, "text": "hi",
        ], timeout: .seconds(5))
        XCTAssertEqual(sendResult, .object([:]))

        guard let sendRequest = server.requests(path: "/messages").first else {
            return XCTFail("expected send request")
        }
        XCTAssertEqual(sendRequest.method, "POST")
        XCTAssertEqual(sendRequest.url.absoluteString, "http://b/v1/chats/-100200/messages")
        XCTAssertEqual(try JSONValue.decode(bytes: Array(sendRequest.body)), ["text": "hi"])

        do {
            _ = try await pager.call("tg.send", params: [
                "bridge": "http://b", "token": "t", "chat_id": 101, "text": "",
            ], timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
            XCTAssertEqual(error.msg, "text must be a non-empty string")
        }

        await phone.stop()
        await pager.stop()
    }

    // MARK: - Transport failure reasons (shown on the pager as "phone: <msg>")

    func testTransportReason() {
        XCTAssertEqual(TelegramService.transportReason(URLError(.timedOut)), "bridge timeout")
        XCTAssertEqual(TelegramService.transportReason(URLError(.cannotConnectToHost)), "bridge unreachable")
        XCTAssertEqual(TelegramService.transportReason(URLError(.appTransportSecurityRequiresSecureConnection)),
                       "bridge needs https")
        XCTAssertEqual(TelegramService.transportReason(URLError(.serverCertificateUntrusted)), "bridge TLS error")
        XCTAssertEqual(TelegramService.transportReason(CancellationError()), "network error")
    }

    // MARK: - 10. Param helpers

    func testParamHelpers() throws {
        XCTAssertEqual(try TelegramService.integer(["k": 7], "k"), 7)
        XCTAssertThrowsError(try TelegramService.integer(["k": "7"], "k"))
        XCTAssertThrowsError(try TelegramService.integer(["k": 7.5], "k"))
        XCTAssertThrowsError(try TelegramService.integer(["k": true], "k"))

        XCTAssertEqual(try TelegramService.xfer([:]), 0)
        XCTAssertEqual(try TelegramService.xfer(["xfer": 0]), 0)
        XCTAssertEqual(try TelegramService.xfer(["xfer": 65535]), 65535)
        XCTAssertThrowsError(try TelegramService.xfer(["xfer": 2]))
        XCTAssertThrowsError(try TelegramService.xfer(["xfer": 65537]))
    }
}
