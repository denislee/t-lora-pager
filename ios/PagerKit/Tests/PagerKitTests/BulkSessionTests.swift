import Foundation
import XCTest

@testable import PagerKit

/// Records the bulk transfer id a handler observed, so a test can assert on it
/// without capturing `self` (which is not `Sendable`) in a `@Sendable` closure.
private actor XferProbe {
    private(set) var value: UInt16?

    func record(_ id: UInt16) {
        value = id
    }
}

/// Holds the streaming task a handler started before answering, so the test can
/// await its completion instead of leaving it racing the end of the test.
private actor StreamHolder {
    private(set) var task: Task<Void, Never>?

    func hold(_ task: Task<Void, Never>) {
        self.task = task
    }
}

/// Drives two real `LinkSession`s against each other over a loopback transport
/// pair, covering the bulk paths end to end: `fs.read`, `acceptBytes`,
/// `sendBytes` and `receiveBytes`. Covers streaming DATA before the RES, CRC
/// checking, ERR pass-through, idle timeouts, stop-while-waiting and the
/// role-based request id parity.
final class BulkSessionTests: XCTestCase {

    /// The deterministic byte pattern (i*7+3) & 0xFF shared with the Go and C++
    /// engine tests.
    private func pattern(_ n: Int) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: n)
        for i in 0..<n {
            bytes[i] = UInt8((i * 7 + 3) & 0xFF)
        }
        return bytes
    }

    /// A 50 kB file is 13 chunks of 4092 bytes and more than the 16-packet
    /// initial credit window, so the pager's `sendBytes` must start streaming
    /// before it answers the `fs.read` request or the transfer would stall.
    func testReadFileStreamsBeforeResponse() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        let file = pattern(50_000)
        let probe = XferProbe()
        let holder = StreamHolder()
        await pager.handle("fs.read") { params in
            let xfer = UInt16(exactly: params["xfer"]!.intValue!)!
            await probe.record(xfer)
            let stream = Task { () -> Void in
                try? await pager.sendBytes(xfer: xfer, data: file, timeout: .seconds(5))
            }
            await holder.hold(stream)
            return [
                "xfer": .number(Double(xfer)),
                "size": .number(Double(file.count)),
                "crc32": .number(Double(CRC32.checksum(file))),
                "protected": true,
            ]
        }

        let (bytes, info) = try await phone.readFile(
            vol: "int", path: "/notes/a.txt", timeout: .seconds(5))
        XCTAssertEqual(bytes, file)
        XCTAssertEqual(info, FileInfo(size: 50_000, crc32: CRC32.checksum(file), protected: true))

        // The phone allocates even transfer ids, so the xfer it handed the
        // handler must be even.
        let observed = await probe.value
        XCTAssertNotNil(observed)
        XCTAssertEqual(observed! % 2, 0)

        if let stream = await holder.task { await stream.value }
        let remaining = await phone.transferCount
        XCTAssertEqual(remaining, 0)

        await phone.stop()
        await pager.stop()
    }

    func testReadFileCRCMismatch() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        let file = pattern(50_000)
        let holder = StreamHolder()
        await pager.handle("fs.read") { params in
            let xfer = UInt16(exactly: params["xfer"]!.intValue!)!
            let stream = Task { () -> Void in
                try? await pager.sendBytes(xfer: xfer, data: file, timeout: .seconds(5))
            }
            await holder.hold(stream)
            return [
                "xfer": .number(Double(xfer)),
                "size": .number(Double(file.count)),
                "crc32": .number(Double(CRC32.checksum(file) &+ 1)),
                "protected": true,
            ]
        }

        do {
            _ = try await phone.readFile(vol: "int", path: "/notes/a.txt", timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
            XCTAssertEqual(error.msg, "crc mismatch")
        }

        if let stream = await holder.task { await stream.value }
        await phone.stop()
        await pager.stop()
    }

    func testReadFileRPCErrorPassesThrough() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        await pager.handle("fs.read") { _ in
            throw RPCError(code: "invalid_argument", msg: "sd not mounted")
        }

        do {
            _ = try await phone.readFile(vol: "int", path: "/notes/a.txt", timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error, RPCError(code: "invalid_argument", msg: "sd not mounted"))
        }

        await phone.stop()
        await pager.stop()
    }

    func testReadFileIdleTimeout() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        // The handler answers with correct metadata but never streams DATA, so
        // the phone must give up on the idle gap.
        let file = pattern(10_000)
        await pager.handle("fs.read") { _ in
            [
                "size": .number(Double(file.count)),
                "crc32": .number(Double(CRC32.checksum(file))),
            ]
        }

        do {
            _ = try await phone.readFile(vol: "int", path: "/notes/a.txt", timeout: .milliseconds(300))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "timeout")
            XCTAssertEqual(error.msg, "fs.read: timeout")
        }

        await phone.stop()
        await pager.stop()
    }

    /// The chat-audio pattern: the phone allocates the receiver id, the pager
    /// streams the bytes under it.
    func testAcceptBytesFromPager() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        let data = pattern(70_000)
        let incoming = await phone.acceptBytes(size: UInt32(data.count), crc32: CRC32.checksum(data))
        XCTAssertEqual(incoming.xfer % 2, 0)

        async let sent: Void = pager.sendBytes(xfer: incoming.xfer, data: data, timeout: .seconds(5))
        let got = try await incoming.wait(timeout: .seconds(5))
        try await sent
        XCTAssertEqual(got, data)

        await phone.stop()
        await pager.stop()
    }

    func testAcceptBytesCRCMismatch() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        let data = pattern(70_000)
        let incoming = await phone.acceptBytes(
            size: UInt32(data.count), crc32: CRC32.checksum(data) ^ 1)
        XCTAssertEqual(incoming.xfer % 2, 0)

        async let sent: Void = pager.sendBytes(xfer: incoming.xfer, data: data, timeout: .seconds(5))
        do {
            _ = try await incoming.wait(timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
        }
        try await sent

        await phone.stop()
        await pager.stop()
    }

    func testAcceptZeroSize() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        let incoming = await phone.acceptBytes(size: 0, crc32: 123)
        let got = try await incoming.wait(timeout: .seconds(5))
        XCTAssertEqual(got, [])

        await phone.stop()
        await pager.stop()
    }

    /// The `tg.*` shape: the pager opens the receiver id for a bulk reply the
    /// phone sends. The phone starts sending first so the pager exercises its
    /// early-DATA buffer before `receiveBytes` names the size.
    func testPagerOffersXferForBulkReply() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        let body = pattern(9_000)
        let id = await pager.openIncoming()
        XCTAssertEqual(id % 2, 1)

        let send = Task {
            try await phone.sendBytes(xfer: id, data: body, timeout: .seconds(5))
        }
        try await Task.sleep(for: .milliseconds(50))
        let got = try await pager.receiveBytes(
            xfer: id, size: UInt32(body.count), crc32: CRC32.checksum(body), timeout: .seconds(5))
        try await send.value
        XCTAssertEqual(got, body)

        await phone.stop()
        await pager.stop()
    }

    func testSendBytesTimesOutWithoutAcks() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        // Nobody on the pager receives id 7: the DATA is unrouted and no ACK
        // ever comes back, so the sender's idle timer fires.
        do {
            try await phone.sendBytes(xfer: 7, data: pattern(100_000), timeout: .milliseconds(300))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "timeout")
        }

        let unrouted = await pager.stats.unroutedBulk
        XCTAssertGreaterThan(unrouted, 0)

        await phone.stop()
        await pager.stop()
    }

    func testStopFailsWaitingTransfer() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        let incoming = await phone.acceptBytes(size: 1000, crc32: 0)
        let t = Task { try await incoming.wait(timeout: .seconds(30)) }
        try await Task.sleep(for: .milliseconds(50))
        await phone.stop()

        do {
            _ = try await t.value
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "disconnected")
        }

        await pager.stop()
    }

    /// A pager-originated REQ uses odd ids, which the phone accepts, and the
    /// other way round for a phone-originated REQ.
    func testRequestIdParityByRole() async throws {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        let phone = LinkSession(transport: a)
        let pager = LinkSession(transport: b, role: .pager)
        await phone.start()
        await pager.start()

        await phone.handle("sys.ping") { p in p }
        let fromPager = try await pager.call("sys.ping", params: ["t": 5])
        XCTAssertEqual(fromPager["t"]?.intValue, 5)

        await pager.handle("sys.ping") { p in p }
        let fromPhone = try await phone.call("sys.ping", params: ["t": 5])
        XCTAssertEqual(fromPhone["t"]?.intValue, 5)

        await phone.stop()
        await pager.stop()
    }
}
