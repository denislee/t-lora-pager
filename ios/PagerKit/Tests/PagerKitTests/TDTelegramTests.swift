import Foundation
import XCTest

@testable import PagerKit

/// A scripted TDLib: answers each request by its `@type` and records every
/// request it saw. `hang` types never answer (until cancelled), which is how
/// TDLib behaves while it waits out a network loss.
private final class FakeTD: TDQuerying, @unchecked Sendable {
    private let lock = NSLock()
    private var log: [JSONValue] = []
    private let answer: @Sendable (JSONValue) throws -> JSONValue
    private let hang: Set<String>

    init(hang: Set<String> = [], answer: @escaping @Sendable (JSONValue) throws -> JSONValue) {
        self.answer = answer
        self.hang = hang
    }

    var requests: [JSONValue] {
        lock.lock()
        defer { lock.unlock() }
        return log
    }

    private func record(_ request: JSONValue) {
        lock.lock()
        log.append(request)
        lock.unlock()
    }

    func types() -> [String] { requests.compactMap { $0["@type"]?.stringValue } }

    func query(_ request: JSONValue) async throws -> JSONValue {
        record(request)
        if let type = request["@type"]?.stringValue, hang.contains(type) {
            try await Task.sleep(for: .seconds(60))
        }
        return try answer(request)
    }
}

/// Stand-in native session for the routing tests.
private struct FakeNative: TelegramNative {
    let ready: Bool
    func isReady() async -> Bool { ready }
    func chats(limit: Int) async throws -> JSONValue { [["id": 7, "title": "Native", "unread": 1]] }
    func messages(chatID: Int64, limit: Int) async throws -> JSONValue { [] }
    func read(chatID: Int64, upTo: Int64) async throws {}
    func send(chatID: Int64, text: String) async throws {}
}

final class TDTelegramTests: XCTestCase {
    private static let shift = Double(1 << 20)

    private func message(_ server: Int, _ content: JSONValue, out: Bool = false,
                         sender: JSONValue = ["@type": "messageSenderUser", "user_id": 5]) -> JSONValue {
        [
            "@type": "message",
            "id": .number(Double(server) * Self.shift),
            "is_outgoing": .bool(out),
            "sender_id": sender,
            "content": content,
        ]
    }

    private func text(_ s: String) -> JSONValue {
        ["@type": "messageText", "text": ["@type": "formattedText", "text": .string(s)]]
    }

    func testChatsLoadOnceThenListInOrder() async throws {
        let td = FakeTD { req in
            switch req["@type"]?.stringValue {
            case "loadChats": throw TDError(code: 404, message: "Not Found")
            case "getChats": return ["@type": "chats", "total_count": 2, "chat_ids": [-1001234567890, 42]]
            case "getChat":
                let id = req["chat_id"]?.doubleValue ?? 0
                return id == 42
                    ? ["@type": "chat", "id": 42, "title": "Alice", "unread_count": 3]
                    : ["@type": "chat", "id": .number(id), "title": "Team", "unread_count": 0]
            default: throw TDError(code: 400, message: "unexpected")
            }
        }
        let tg = TDTelegram(td: td) { true }
        let chats = try await tg.chats(limit: 20)
        XCTAssertEqual(chats, [
            ["id": .number(-1001234567890), "title": "Team", "unread": 0],
            ["id": 42, "title": "Alice", "unread": 3],
        ])
        _ = try await tg.chats(limit: 20)
        XCTAssertEqual(td.types().filter { $0 == "loadChats" }.count, 1)
    }

    func testMessagesMapContentSenderAndShiftIds() async throws {
        let history: [JSONValue] = [
            message(12, text("newest"), out: true),
            message(11, ["@type": "messagePhoto", "caption": ["text": "pic"],
                         "photo": ["sizes": [["width": 90, "height": 60], ["width": 1280, "height": 960]]]]),
            message(10, ["@type": "messageSticker", "sticker": ["emoji": "👍", "width": 512, "height": 512]],
                    sender: ["@type": "messageSenderChat", "chat_id": -100]),
            message(9, ["@type": "messagePoll"]),
        ]
        let td = FakeTD { req in
            switch req["@type"]?.stringValue {
            case "getChatHistory":
                // Cold cache: the first page is one message, then the rest.
                let from = req["from_message_id"]?.doubleValue ?? 0
                return ["@type": "messages", "messages": .array(from == 0 ? [history[0]] : Array(history.dropFirst()))]
            case "getUser": return ["@type": "user", "first_name": "Bob", "last_name": "Lee"]
            case "getChat": return ["@type": "chat", "title": "Channel"]
            default: throw TDError(code: 400, message: "unexpected")
            }
        }
        let tg = TDTelegram(td: td) { true }
        let msgs = try await tg.messages(chatID: 42, limit: 10)
        guard case .array(let items) = msgs else { return XCTFail("expected array") }
        XCTAssertEqual(items.count, 4)
        XCTAssertEqual(items[0], ["id": 12, "text": "newest", "from": "Bob Lee", "out": true])
        XCTAssertEqual(items[1], ["id": 11, "text": "pic", "from": "Bob Lee", "out": false,
                                  "media": ["type": "photo", "w": 1280, "h": 960]])
        XCTAssertEqual(items[2], ["id": 10, "text": "👍", "from": "Channel", "out": false,
                                  "media": ["type": "sticker", "w": 512, "h": 512]])
        XCTAssertEqual(items[3]["text"], "[Poll]")
        // Second page starts from the oldest message seen; the user is cached.
        let pages = td.requests.filter { $0["@type"] == "getChatHistory" }
        XCTAssertEqual(pages[1]["from_message_id"]?.doubleValue, 12 * Self.shift)
        XCTAssertEqual(td.types().filter { $0 == "getUser" }.count, 1)
    }

    func testReadShiftsUpToAndSendBuildsText() async throws {
        let td = FakeTD { _ in ["@type": "ok"] }
        let tg = TDTelegram(td: td) { true }
        try await tg.read(chatID: 42, upTo: 12)
        try await tg.send(chatID: 42, text: "hi")
        let reqs = td.requests
        XCTAssertEqual(reqs[0]["message_ids"], [.number(12 * Self.shift)])
        XCTAssertEqual(reqs[0]["force_read"], true)
        XCTAssertEqual(reqs[1]["input_message_content"]?["text"]?["text"], "hi")
    }

    func testErrorsMapForThePager() async throws {
        let td = FakeTD { req in
            if req["@type"] == "getChatHistory" { throw TDError(code: 401, message: "Unauthorized") }
            throw TDError(code: 400, message: "Chat not found")
        }
        let tg = TDTelegram(td: td) { true }
        do {
            _ = try await tg.messages(chatID: 1, limit: 5)
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error, RPCError(code: "unauthorized", msg: "401"))
        }
        do {
            try await tg.send(chatID: 1, text: "x")
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error, RPCError(code: "http_error", msg: "400"))
        }
    }

    func testUnansweredRequestTimesOut() async throws {
        let td = FakeTD(hang: ["sendMessage"]) { _ in ["@type": "ok"] }
        let tg = TDTelegram(td: td, timeout: .milliseconds(100)) { true }
        do {
            try await tg.send(chatID: 1, text: "x")
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error, RPCError(code: "http_error", msg: "telegram timeout"))
        }
    }

    // MARK: - TelegramService routing

    private func expectChats(native: TelegramNative?, _ params: [String: JSONValue]) async throws -> JSONValue {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()
        defer { Task { await phone.stop(); await pager.stop() } }
        let server = StubServer { _ in StubResponse.json(200, #"[{"id":1,"title":"Bridge","unread":0}]"#) }
        await TelegramService(session: phone, urlSession: StubURLProtocol.session(server), native: native).register()
        return try await pager.call("tg.chats", params: params, timeout: .seconds(5))
    }

    func testReadyNativeAnswersWithoutBridge() async throws {
        let result = try await expectChats(native: FakeNative(ready: true), ["limit": 20])
        XCTAssertEqual(result["body"]?[0]?["title"], "Native")
    }

    func testReadyNativeWinsOverBridge() async throws {
        let result = try await expectChats(native: FakeNative(ready: true),
                                           ["bridge": "http://b", "token": "t", "limit": 20])
        XCTAssertEqual(result["body"]?[0]?["title"], "Native")
    }

    func testLoggedOutNativeFallsBackToBridge() async throws {
        let result = try await expectChats(native: FakeNative(ready: false),
                                           ["bridge": "http://b", "token": "t", "limit": 20])
        XCTAssertEqual(result["body"]?[0]?["title"], "Bridge")
    }

    func testNoSessionAndNoBridgeIsUnsupported() async throws {
        do {
            _ = try await expectChats(native: FakeNative(ready: false), ["limit": 20])
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error, RPCError(code: "unsupported", msg: "log in on phone"))
        }
    }
}

private extension JSONValue {
    subscript(index: Int) -> JSONValue? {
        guard case .array(let items) = self, items.indices.contains(index) else { return nil }
        return items[index]
    }
}
