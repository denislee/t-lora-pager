import Foundation
import XCTest

@testable import PagerKit

/// A transport whose packet stream the test can end, standing in for a peer
/// EOF or a dropped BLE link.
private final class EndingTransport: PacketTransport, @unchecked Sendable {
    let packetSize = PagerLink.tcpPacket
    let packets: AsyncStream<[UInt8]>
    private let continuation: AsyncStream<[UInt8]>.Continuation

    init() {
        var continuation: AsyncStream<[UInt8]>.Continuation!
        packets = AsyncStream { continuation = $0 }
        self.continuation = continuation
    }

    func send(_ packet: [UInt8]) async throws {}

    func end() { continuation.finish() }
}

/// Once the transport has ended, bulk transfers fail at once with
/// `disconnected` instead of waiting out their idle timeout, matching `call`.
final class LinkClosedTests: XCTestCase {
    private func endedSession() async -> LinkSession {
        let transport = EndingTransport()
        let session = LinkSession(transport: transport)
        await session.start()
        transport.end()
        for await _ in session.events {}  // finishes when the session sees the end
        return session
    }

    func testSendBytesAfterTransportEndFailsFast() async {
        let session = await endedSession()
        let start = ContinuousClock.now
        do {
            try await session.sendBytes(xfer: 1, data: [UInt8](repeating: 7, count: 20_000), timeout: .seconds(5))
            XCTFail("expected disconnected")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "disconnected")
        } catch {
            XCTFail("unexpected \(error)")
        }
        XCTAssertLessThan(ContinuousClock.now - start, .seconds(1))
    }

    func testAcceptBytesWaitAfterTransportEndFailsFast() async {
        let session = await endedSession()
        let start = ContinuousClock.now
        let incoming = await session.acceptBytes(size: 1000, crc32: 0)
        do {
            _ = try await incoming.wait(timeout: .seconds(5))
            XCTFail("expected disconnected")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "disconnected")
        } catch {
            XCTFail("unexpected \(error)")
        }
        XCTAssertLessThan(ContinuousClock.now - start, .seconds(1))
    }
}
