import Foundation
import XCTest

@testable import PagerKit

/// Drives a `LinkSession` against an in-test pager endpoint over a loopback
/// transport pair: request/response correlation, unknown methods, timeouts,
/// pager-initiated requests and disconnect.
final class SessionTests: XCTestCase {

    /// Minimal pager-side endpoint built on the same framing primitives.
    ///
    /// Answers `sys.ping` by echoing the params and everything else with ERR
    /// `unsupported`, unless `mute` is set (to exercise timeouts). Every decoded
    /// message is recorded so the test can observe a RES/ERR it initiated.
    private actor FakePager {
        private let transport: LoopbackTransport
        private let mute: Bool
        private var reassembler = Reassembler()
        private var segmenter = Segmenter()
        private var readTask: Task<Void, Never>?
        private(set) var received: [Message] = []

        init(transport: LoopbackTransport, mute: Bool = false) {
            self.transport = transport
            self.mute = mute
        }

        func start() {
            guard readTask == nil else { return }
            let transport = self.transport
            readTask = Task { [weak self] in
                for await packet in transport.packets {
                    guard let self else { return }
                    await self.feed(packet)
                }
            }
        }

        func stop() {
            readTask?.cancel()
            readTask = nil
        }

        /// Send a message toward the session (used for pager-initiated REQ).
        func send(_ message: Message) async {
            await sendMessage(type: message.type, id: message.id, payload: message.payload)
        }

        /// Wait until a RES/ERR with `id` has been received, or `timeout` elapses.
        func waitForResponse(id: UInt16, timeout: Duration) async -> Message? {
            let deadline = ContinuousClock.now + timeout
            while ContinuousClock.now < deadline {
                if let message = response(id: id) { return message }
                try? await Task.sleep(for: .milliseconds(10))
            }
            return response(id: id)
        }

        private func response(id: UInt16) -> Message? {
            received.first { $0.id == id && ($0.type == .res || $0.type == .err) }
        }

        private func feed(_ packet: [UInt8]) async {
            for event in reassembler.feed(packet) {
                guard case .message(let bytes) = event,
                      let message = try? Message.decode(bytes) else { continue }
                received.append(message)
                await respond(to: message)
            }
        }

        private func respond(to message: Message) async {
            guard !mute, message.type == .req else { return }
            guard let value = try? JSONValue.decode(bytes: message.payload),
                  case .object(let object) = value,
                  let method = object["m"]?.stringValue else {
                await sendError(id: message.id, code: "bad_request", msg: "bad request")
                return
            }
            if method == "sys.ping" {
                await sendResult(id: message.id, result: object["p"] ?? .object([:]))
            } else {
                await sendError(id: message.id, code: "unsupported", msg: "unknown method \(method)")
            }
        }

        private func sendResult(id: UInt16, result: JSONValue) async {
            guard let payload = try? result.encoded() else { return }
            await sendMessage(type: .res, id: id, payload: payload)
        }

        private func sendError(id: UInt16, code: String, msg: String) async {
            let object: [String: JSONValue] = ["code": .string(code), "msg": .string(msg)]
            guard let payload = try? JSONValue.object(object).encoded() else { return }
            await sendMessage(type: .err, id: id, payload: payload)
        }

        private func sendMessage(type: MsgType, id: UInt16, payload: [UInt8]) async {
            guard let encoded = try? Message(type: type, id: id, payload: payload).encoded() else { return }
            for packet in segmenter.segment(encoded, packetSize: transport.packetSize) {
                try? await transport.send(packet)
            }
        }
    }

    func testPingRoundTrip() async throws {
        let (phone, pagerSide) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let pager = FakePager(transport: pagerSide)
        await pager.start()
        let session = LinkSession(transport: phone)
        await session.start()

        let result = try await session.call("sys.ping", params: ["t": 42])
        XCTAssertEqual(result["t"]?.intValue, 42)

        await session.stop()
        await pager.stop()
    }

    func testUnknownMethodSurfacesError() async throws {
        let (phone, pagerSide) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let pager = FakePager(transport: pagerSide)
        await pager.start()
        let session = LinkSession(transport: phone)
        await session.start()

        do {
            _ = try await session.call("no.such.method")
            XCTFail("expected an error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "unsupported")
        }

        let requestCount = await pager.received.filter { $0.type == .req }.count
        XCTAssertEqual(requestCount, 1)

        await session.stop()
        await pager.stop()
    }

    func testTimeoutFailsCall() async throws {
        let (phone, pagerSide) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let pager = FakePager(transport: pagerSide, mute: true)
        await pager.start()
        let session = LinkSession(transport: phone)
        await session.start()

        do {
            _ = try await session.call("sys.ping", params: ["t": 1], timeout: .milliseconds(100))
            XCTFail("expected a timeout")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "timeout")
        }

        await session.stop()
        await pager.stop()
    }

    func testPagerInitiatedRequest() async throws {
        let (phone, pagerSide) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let pager = FakePager(transport: pagerSide)
        await pager.start()
        let session = LinkSession(transport: phone)
        await session.start()

        await session.handle("weather.get") { _ in .object(["city": "recife"]) }

        let payload = try JSONValue.object(["m": .string("weather.get")]).encoded()
        await pager.send(Message(type: .req, id: 1, payload: payload))

        let response = await pager.waitForResponse(id: 1, timeout: .seconds(2))
        XCTAssertEqual(response?.type, .res)
        XCTAssertEqual(response?.id, 1)
        let decoded = response.flatMap { try? JSONValue.decode(bytes: $0.payload) }
        XCTAssertEqual(decoded?["city"]?.stringValue, "recife")

        await session.stop()
        await pager.stop()
    }

    func testStopFailsPendingCall() async throws {
        let (phone, pagerSide) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let pager = FakePager(transport: pagerSide, mute: true)
        await pager.start()
        let session = LinkSession(transport: phone)
        await session.start()

        let call = Task { try await session.call("sys.ping", params: ["t": 1], timeout: .seconds(30)) }
        while true {
            if await session.pendingCount > 0 { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        await session.stop()

        do {
            _ = try await call.value
            XCTFail("expected a disconnect")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "disconnected")
        }

        await pager.stop()
    }
}
