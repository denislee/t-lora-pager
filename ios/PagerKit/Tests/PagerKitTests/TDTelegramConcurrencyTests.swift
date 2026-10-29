import Foundation
import XCTest

@testable import PagerKit

/// Flips to `true` once, so a scripted reply can be given only on the first ask.
private final class OneShot: @unchecked Sendable {
    private let lock = NSLock()
    private var taken = false

    /// `true` the first time it is called, `false` afterwards.
    func take() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        let wasTaken = taken
        taken = true
        return !wasTaken
    }
}

/// Scripted replies, at file scope so the `@Sendable` answer closures can use
/// them without capturing the (non-Sendable) test case.
private enum TDReply {
    static func chat(_ id: Int64) -> JSONValue {
        [
            "@type": "chat",
            "id": .number(Double(id)),
            "title": .string("c\(id)"),
            "unread_count": .number(Double(id)),
        ]
    }

    static func message(_ server: Int, user: Int64) -> JSONValue {
        TDFixture.message(server, TDFixture.text("hi"),
                          sender: ["@type": "messageSenderUser", "user_id": .number(Double(user))])
    }
}

final class TDTelegramConcurrencyTests: XCTestCase {

    // getChat runs for every chat at once (all three are in flight together;
    // a serial loop would only ever have one), and the answers still come
    // back in the chat_ids order — not the order they happened to finish in.
    func testGetChatRunsConcurrentlyAndKeepsOrder() async throws {
        let td = FakeTD(gather: ["getChat": 3]) { request in
            switch request["@type"]?.stringValue {
            case "loadChats": throw TDError(code: 404, message: "Not Found")
            case "getChats": return ["@type": "chats", "chat_ids": [3, 1, 2]]
            case "getChat":
                guard let id = Params.int64(request["chat_id"]) else {
                    throw TDError(code: 400, message: "bad chat_id")
                }
                return TDReply.chat(id)
            default: throw TDError(code: 400, message: "unexpected")
            }
        }
        let tg = TDTelegram(td: td, timeout: .seconds(5), budget: .seconds(5)) { true }
        let chats = try await tg.chats(limit: 20)
        XCTAssertEqual(chats, [
            ["id": 3, "title": "c3", "unread": 3],
            ["id": 1, "title": "c1", "unread": 1],
            ["id": 2, "title": "c2", "unread": 2],
        ])
        XCTAssertEqual(td.peak("getChat"), 3)
    }

    // A getChat that fails mid-list truncates there: the chats before it stand.
    func testFailedMiddleChatTruncatesList() async throws {
        let td = FakeTD { request in
            switch request["@type"]?.stringValue {
            case "loadChats": throw TDError(code: 404, message: "Not Found")
            case "getChats": return ["@type": "chats", "chat_ids": [1, 2, 3]]
            case "getChat":
                guard let id = Params.int64(request["chat_id"]) else {
                    throw TDError(code: 400, message: "bad chat_id")
                }
                if id == 2 { throw TDError(code: 400, message: "bad") }
                return TDReply.chat(id)
            default: throw TDError(code: 400, message: "unexpected")
            }
        }
        let tg = TDTelegram(td: td, timeout: .seconds(5), budget: .seconds(5)) { true }
        let chats = try await tg.chats(limit: 20)
        XCTAssertEqual(chats, [["id": 1, "title": "c1", "unread": 1]])
    }

    // Nothing to fall back on: the first chat failing fails the whole call.
    func testFirstChatFailureThrows() async throws {
        let td = FakeTD { request in
            switch request["@type"]?.stringValue {
            case "loadChats": throw TDError(code: 404, message: "Not Found")
            case "getChats": return ["@type": "chats", "chat_ids": [1, 2]]
            case "getChat": throw TDError(code: 400, message: "bad")
            default: throw TDError(code: 400, message: "unexpected")
            }
        }
        let tg = TDTelegram(td: td, timeout: .seconds(5), budget: .seconds(5)) { true }
        do {
            _ = try await tg.chats(limit: 20)
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error, RPCError(code: "http_error", msg: "400"))
        }
    }

    // A page's uncached senders are resolved at once (all three getUsers in
    // flight together), once each, and the messages still carry their names
    // (and their order).
    func testSendersResolvedConcurrently() async throws {
        let page = OneShot()
        let td = FakeTD(gather: ["getUser": 3]) { request in
            switch request["@type"]?.stringValue {
            case "getChatHistory":
                if page.take() {
                    return ["@type": "messages",
                            "messages": [TDReply.message(3, user: 10),
                                         TDReply.message(2, user: 11),
                                         TDReply.message(1, user: 12)]]
                }
                return ["@type": "messages", "messages": []]
            case "getUser":
                guard let id = Params.int64(request["user_id"]) else {
                    throw TDError(code: 400, message: "bad user_id")
                }
                return ["@type": "user", "first_name": .string("u\(id)")]
            default: throw TDError(code: 400, message: "unexpected")
            }
        }
        let tg = TDTelegram(td: td, timeout: .seconds(5), budget: .seconds(5)) { true }
        let msgs = try await tg.messages(chatID: 1, limit: 3)
        guard case .array(let items) = msgs else { return XCTFail("expected array") }
        XCTAssertEqual(items.count, 3)
        XCTAssertEqual(items.map { $0["id"]?.intValue }, [3, 2, 1])
        XCTAssertEqual(items.map { $0["from"]?.stringValue }, ["u10", "u11", "u12"])
        XCTAssertEqual(td.types().filter { $0 == "getUser" }.count, 3)
        XCTAssertEqual(td.peak("getUser"), 3)
    }
}
