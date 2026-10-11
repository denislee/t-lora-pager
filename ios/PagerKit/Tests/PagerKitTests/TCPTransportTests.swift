#if canImport(Network)
import Foundation
import Network
import XCTest

@testable import PagerKit

/// Drives `TCPTransport` + `LinkSession` against an in-test pager listening on a
/// real loopback socket, the way the app talks to the emulator (PROTOCOL.md §5).
final class TCPTransportTests: XCTestCase {

    /// Loopback TCP pager: accepts one connection, reads u16-LE length-prefixed
    /// packets, and answers every REQ with a RES echoing its params.
    private final class FakeTCPPager: @unchecked Sendable {
        let listener: NWListener
        private let queue = DispatchQueue(label: "FakeTCPPager")
        private var connection: NWConnection?
        private var reassembler = Reassembler()
        private var segmenter = Segmenter()

        init() throws {
            listener = try NWListener(using: .tcp, on: .any)
        }

        /// Start listening and return the bound port.
        func start() async throws -> UInt16 {
            listener.newConnectionHandler = { [self] connection in
                self.connection = connection
                connection.start(queue: queue)
                receiveLength(on: connection)
            }
            return try await withCheckedThrowingContinuation { continuation in
                listener.stateUpdateHandler = { [listener] state in
                    switch state {
                    case .ready:
                        listener.stateUpdateHandler = nil
                        continuation.resume(returning: listener.port!.rawValue)
                    case .failed(let error):
                        listener.stateUpdateHandler = nil
                        continuation.resume(throwing: error)
                    default:
                        break
                    }
                }
                listener.start(queue: queue)
            }
        }

        func stop() {
            connection?.cancel()
            listener.cancel()
        }

        private func receiveLength(on connection: NWConnection) {
            connection.receive(minimumIncompleteLength: 2, maximumLength: 2) { [self] data, _, _, error in
                guard error == nil, let data, data.count == 2 else { return }
                let length = Int(data[data.startIndex]) | (Int(data[data.startIndex + 1]) << 8)
                connection.receive(minimumIncompleteLength: length, maximumLength: length) { [self] body, _, _, error in
                    guard error == nil, let body, body.count == length else { return }
                    feed([UInt8](body), on: connection)
                    receiveLength(on: connection)
                }
            }
        }

        private func feed(_ packet: [UInt8], on connection: NWConnection) {
            for event in reassembler.feed(packet) {
                guard case .message(let bytes) = event,
                      let message = try? Message.decode(bytes),
                      message.type == .req,
                      let value = try? JSONValue.decode(bytes: message.payload),
                      case .object(let object) = value else { continue }
                let result = object["p"] ?? .object([:])
                guard let payload = try? result.encoded(),
                      let encoded = try? Message(type: .res, id: message.id, payload: payload).encoded()
                else { continue }
                for out in segmenter.segment(encoded, packetSize: PagerLink.tcpPacket) {
                    var record = [UInt8(out.count & 0xFF), UInt8(out.count >> 8)]
                    record.append(contentsOf: out)
                    connection.send(content: Data(record), completion: .idempotent)
                }
            }
        }
    }

    /// The app calls `sys.hello` straight after `start()`; the first request
    /// must not race the connection becoming ready.
    func testCallImmediatelyAfterStart() async throws {
        let pager = try FakeTCPPager()
        let port = try await pager.start()
        defer { pager.stop() }

        let transport = TCPTransport(host: "127.0.0.1", port: port)
        try await transport.start()
        let session = LinkSession(transport: transport, defaultTimeout: .seconds(2))
        await session.start()

        let result = try await session.call("sys.ping", params: ["t": .number(42)])
        XCTAssertEqual(result["t"]?.intValue, 42)

        await session.stop()
        await transport.stop()
    }

    /// Nothing listening: `start()` fails promptly instead of hanging in
    /// NWConnection's `.waiting` state.
    func testStartFailsWhenNothingListens() async throws {
        let pager = try FakeTCPPager()
        let port = try await pager.start()
        pager.stop()
        try await Task.sleep(for: .milliseconds(100))

        let transport = TCPTransport(host: "127.0.0.1", port: port)
        let start = ContinuousClock.now
        do {
            try await transport.start()
            XCTFail("start() should throw when the port is closed")
        } catch {
            XCTAssertLessThan(ContinuousClock.now - start, .seconds(5))
        }
    }

    /// The peer closing the socket finishes the packet stream and fails any
    /// outstanding request with `disconnected` instead of waiting for timeout.
    func testPeerCloseFailsPending() async throws {
        let pager = try FakeTCPPager()
        let port = try await pager.start()

        let transport = TCPTransport(host: "127.0.0.1", port: port)
        try await transport.start()
        let session = LinkSession(transport: transport, defaultTimeout: .seconds(10))
        await session.start()
        _ = try await session.call("sys.ping", params: ["t": .number(1)])

        pager.stop()
        let start = ContinuousClock.now
        do {
            _ = try await session.call("sys.ping", params: ["t": .number(2)])
            XCTFail("call should fail once the peer closed")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "disconnected")
            XCTAssertLessThan(ContinuousClock.now - start, .seconds(5))
        }
    }
}
#endif
