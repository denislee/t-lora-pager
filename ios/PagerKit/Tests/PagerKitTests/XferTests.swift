import XCTest

@testable import PagerKit

/// The Swift error fed to the sender's read callback (and mirroring Go's
/// `errors.New("read")`), whose throw fails the transfer.
private struct TestError: Error {}

// These tests mirror the Go engine tests in
// `tools/pagerlink/internal/pagerlink/xfer_test.go` (themselves a port of
// test/test_link_xfer/test_main.cpp): a Sender and a Receiver are wired together
// over a pair of payload queues so a test can deliver, drop or reorder DATA and
// ACK messages by hand, and a loopback helper carries the whole transfer to
// completion.

/// `xferLoop` carries encoded DATA payloads one way and encoded ACK payloads the
/// other, mirroring the C++ Loop harness.
final class XferLoop {
    var data: [[UInt8]] = [] // DATA payloads: sender -> receiver
    var ack: [[UInt8]] = []  // ACK payloads: receiver -> sender

    func sendData(_ payload: [UInt8]) throws {
        data.append(payload)
    }

    func sendAck(_ payload: [UInt8]) throws {
        ack.append(payload)
    }

    func deliverData(_ rx: XferReceiver) {
        let p = data.removeFirst()
        rx.onData(p)
    }

    func deliverAck(_ tx: XferSender) {
        let p = ack.removeFirst()
        tx.onAck(p)
    }

    func deliverAllData(_ rx: XferReceiver) {
        while !data.isEmpty {
            deliverData(rx)
        }
    }

    func deliverAllAcks(_ tx: XferSender) {
        while !ack.isEmpty {
            deliverAck(tx)
        }
    }
}

/// `makeSource` is the deterministic pattern (i*7+3) & 0xFF used by the C++ tests.
private func makeSource(_ n: Int) -> [UInt8] {
    var v = [UInt8](repeating: 0, count: n)
    for i in 0..<n {
        v[i] = UInt8((i * 7 + 3) & 0xFF)
    }
    return v
}

/// `feedData` encodes one DATA message and delivers it to `rx`.
private func feedData(_ rx: XferReceiver, _ offset: UInt32, _ b: [UInt8]) throws {
    try rx.onData(Bulk.encodeData(offset: offset, bytes: b))
}

/// `readFrom` copies from `src`, the read callback shape the Sender expects.
private func readFrom(_ src: [UInt8]) -> (UInt32, Int) throws -> [UInt8] {
    { off, n in Array(src[Int(off)..<(Int(off) + n)]) }
}

final class XferTests: XCTestCase {

    func testXferRoundTripLossless() throws {
        let size: UInt32 = 100_000
        let src = makeSource(Int(size))
        var dst = [UInt8](repeating: 0, count: Int(size))

        let loop = XferLoop()
        let tx = XferSender(size: size, chunk: 4092, start: 0, read: readFrom(src), send: loop.sendData)
        let rx = XferReceiver(size: size, window: Xfer.initialCredits, ackEvery: 4, start: 0,
            write: { off, b in dst.replaceSubrange(Int(off)..<(Int(off) + b.count), with: b) },
            ack: loop.sendAck)

        var spins = 0
        while spins < 1_000_000 && (tx.state != .done || rx.state != .done) {
            tx.pump()
            loop.deliverAllData(rx)
            loop.deliverAllAcks(tx)
            spins += 1
        }

        guard tx.state == .done && rx.state == .done else {
            XCTFail("states = \(tx.state) / \(rx.state), want Done/Done")
            return
        }
        XCTAssertEqual(rx.received, size, "received = \(rx.received), want \(size)")
        for i in src.indices where src[i] != dst[i] {
            XCTFail("byte \(i) = \(dst[i]), want \(src[i])")
            return
        }
    }

    func testSenderRespectsCredits() throws {
        let size: UInt32 = 20 * 4092
        let src = makeSource(Int(size))

        let loop = XferLoop()
        let tx = XferSender(size: size, chunk: 4092, start: 0, read: readFrom(src), send: loop.sendData)

        tx.pump() // no ACKs delivered yet

        XCTAssertEqual(loop.data.count, Int(Xfer.initialCredits),
                       "in-flight DATA = \(loop.data.count), want \(Xfer.initialCredits)")
        XCTAssertEqual(tx.sent, UInt32(16 * 4092), "sent = \(tx.sent), want \(16 * 4092)")
        XCTAssertEqual(tx.state, .active, "state = \(tx.state), want Active")
    }

    func testAckReplacesWindow() throws {
        let size: UInt32 = 20 * 4092
        let src = makeSource(Int(size))

        let loop = XferLoop()
        let tx = XferSender(size: size, chunk: 4092, start: 0, read: readFrom(src), send: loop.sendData)

        tx.pump() // 16 messages in flight, next = 16*4092

        // Shrink the window to 2 starting from 4*4092: 12 messages are still in
        // flight, so the sender must not emit anything more.
        tx.onAck(Bulk.encodeAck(next: UInt32(4 * 4092), credits: 2))

        let before = tx.sent
        tx.pump()

        XCTAssertEqual(tx.sent, before, "sent = \(tx.sent) (before \(before)), want \(16 * 4092)")
        XCTAssertEqual(tx.sent, UInt32(16 * 4092), "sent = \(tx.sent) (before \(before)), want \(16 * 4092)")
        XCTAssertEqual(loop.data.count, 16, "in-flight DATA = \(loop.data.count), want 16")
        XCTAssertEqual(tx.acked, UInt32(4 * 4092), "acked = \(tx.acked), want \(4 * 4092)")
    }

    func testRewindOnDuplicateAck() throws {
        let size: UInt32 = 40 * 4092
        let src = makeSource(Int(size))
        var dst = [UInt8](repeating: 0, count: Int(size))

        let loop = XferLoop()
        let tx = XferSender(size: size, chunk: 4092, start: 0, read: readFrom(src), send: loop.sendData)
        let rx = XferReceiver(size: size, window: Xfer.initialCredits, ackEvery: 4, start: 0,
            write: { off, b in dst.replaceSubrange(Int(off)..<(Int(off) + b.count), with: b) },
            ack: loop.sendAck)

        tx.pump()
        guard !loop.data.isEmpty else {
            XCTFail("sender produced no DATA")
            return
        }

        // Drop the first DATA message and deliver the rest: every one is at the
        // wrong offset, so the receiver re-ACKs offset 0 without writing.
        loop.data.removeFirst()
        loop.deliverAllData(rx)
        XCTAssertEqual(rx.received, 0, "received = \(rx.received), want 0")

        // Feeding the re-ACK rewinds the sender to offset 0.
        guard !loop.ack.isEmpty else {
            XCTFail("receiver sent no re-ACK")
            return
        }
        loop.deliverAllAcks(tx)
        XCTAssertEqual(tx.sent, 0, "sent/acked = \(tx.sent)/\(tx.acked), want 0/0")
        XCTAssertEqual(tx.acked, 0, "sent/acked = \(tx.sent)/\(tx.acked), want 0/0")

        // The transfer now completes losslessly.
        var spins = 0
        while spins < 1_000_000 && (tx.state != .done || rx.state != .done) {
            tx.pump()
            loop.deliverAllData(rx)
            loop.deliverAllAcks(tx)
            spins += 1
        }

        guard tx.state == .done && rx.state == .done else {
            XCTFail("states = \(tx.state) / \(rx.state), want Done/Done")
            return
        }
        for i in src.indices where src[i] != dst[i] {
            XCTFail("byte \(i) = \(dst[i]), want \(src[i])")
            return
        }
    }

    func testReceiverIgnoresWrongOffset() throws {
        let size: UInt32 = 3 * 4092

        let loop = XferLoop()
        var writes = 0
        let rx = XferReceiver(size: size, window: Xfer.initialCredits, ackEvery: 4, start: 0,
            write: { _, _ in writes += 1 },
            ack: loop.sendAck)

        try feedData(rx, 4092, [UInt8](repeating: 0, count: 4092))

        XCTAssertEqual(writes, 0, "writes = \(writes), want 0")
        XCTAssertEqual(rx.received, 0, "received = \(rx.received), want 0")
        XCTAssertEqual(rx.state, .active, "state = \(rx.state), want Active")
        XCTAssertEqual(loop.ack.count, 1, "ACKs = \(loop.ack.count), want 1")
        let ack = Bulk.decodeAck(loop.ack[0])
        XCTAssertNotNil(ack, "re-ACK next = \(String(describing: ack?.next)), want 0")
        XCTAssertEqual(ack?.next, 0, "re-ACK next = \(String(describing: ack?.next)), want 0")
    }

    func testReceiverOverflowFails() throws {
        let loop = XferLoop()
        let rx = XferReceiver(size: 10, window: Xfer.initialCredits, ackEvery: 4, start: 0,
            write: { _, _ in },
            ack: loop.sendAck)

        try feedData(rx, 0, [UInt8](repeating: 0, count: 11)) // one byte past size

        XCTAssertEqual(rx.state, .failed, "state = \(rx.state), want Failed")
        XCTAssertEqual(rx.received, 0, "received = \(rx.received), want 0")
    }

    func testZeroSizeDone() throws {
        let loop = XferLoop()
        var reads = 0
        var writes = 0
        let tx = XferSender(size: 0, chunk: 4092, start: 0,
            read: { _, _ in reads += 1; return [] },
            send: loop.sendData)
        let rx = XferReceiver(size: 0, window: Xfer.initialCredits, ackEvery: 4, start: 0,
            write: { _, _ in writes += 1 },
            ack: loop.sendAck)

        guard tx.state == .done && rx.state == .done else {
            XCTFail("states = \(tx.state) / \(rx.state), want Done/Done")
            return
        }
        let pumped = tx.pump()
        XCTAssertEqual(pumped, .done, "Pump = \(pumped), want Done")
        XCTAssertEqual(reads, 0, "reads/writes = \(reads)/\(writes), want 0/0")
        XCTAssertEqual(writes, 0, "reads/writes = \(reads)/\(writes), want 0/0")
        XCTAssertEqual(loop.data.count, 0, "messages = \(loop.data.count)/\(loop.ack.count), want 0/0")
        XCTAssertEqual(loop.ack.count, 0, "messages = \(loop.data.count)/\(loop.ack.count), want 0/0")
    }

    func testResumeStartOffset() throws {
        let size: UInt32 = 4 * 4092
        let src = makeSource(Int(size))
        var dst = [UInt8](repeating: 0, count: Int(size))

        let loop = XferLoop()
        let tx = XferSender(size: size, chunk: 4092, start: 8184, read: readFrom(src), send: loop.sendData)

        tx.pump()
        guard !loop.data.isEmpty else {
            XCTFail("sender produced no DATA")
            return
        }
        let first = Bulk.decodeData(loop.data[0])
        XCTAssertNotNil(first, "first DATA offset = \(String(describing: first?.offset)), want 8184")
        XCTAssertEqual(first?.offset, 8184, "first DATA offset = \(String(describing: first?.offset)), want 8184")

        // A receiver resuming at the same offset accepts it.
        let rx = XferReceiver(size: size, window: Xfer.initialCredits, ackEvery: 4, start: 8184,
            write: { o, b in dst.replaceSubrange(Int(o)..<(Int(o) + b.count), with: b) },
            ack: loop.sendAck)
        XCTAssertEqual(rx.state, .active, "state = \(rx.state), want Active")
        loop.deliverData(rx)
        XCTAssertEqual(rx.received, UInt32(8184 + 4092), "received = \(rx.received), want \(8184 + 4092)")
    }

    func testReadErrorFails() throws {
        let size: UInt32 = 2 * 4092
        let loop = XferLoop()
        let tx = XferSender(size: size, chunk: 4092, start: 0,
            read: { _, _ in throw TestError() },
            send: loop.sendData)

        let pumped = tx.pump()
        XCTAssertEqual(pumped, .failed, "Pump = \(pumped), want Failed")
        XCTAssertEqual(loop.data.count, 0, "DATA sent = \(loop.data.count), want 0")
    }

    func testAckEvery() throws {
        let size: UInt32 = 6 * 4092
        let src = makeSource(Int(size))

        let loop = XferLoop()
        let rx = XferReceiver(size: size, window: Xfer.initialCredits, ackEvery: 4, start: 0,
            write: { _, _ in },
            ack: loop.sendAck)

        // The first three in-order messages go unacknowledged.
        for i in 0..<3 {
            try feedData(rx, UInt32(i * 4092), Array(src[i * 4092..<(i + 1) * 4092]))
        }
        XCTAssertEqual(loop.ack.count, 0, "ACKs = \(loop.ack.count), want 0")

        // The fourth crosses the threshold: exactly one ACK, for 4*4092.
        try feedData(rx, UInt32(3 * 4092), Array(src[3 * 4092..<4 * 4092]))
        XCTAssertEqual(loop.ack.count, 1, "ACKs = \(loop.ack.count), want 1")
        let firstAck = Bulk.decodeAck(loop.ack[0])
        XCTAssertNotNil(firstAck, "ACK next = \(String(describing: firstAck?.next)), want \(4 * 4092)")
        XCTAssertEqual(firstAck?.next, UInt32(4 * 4092),
                       "ACK next = \(String(describing: firstAck?.next)), want \(4 * 4092)")

        // The fifth adds nothing; the sixth completes and ACKs once more.
        try feedData(rx, UInt32(4 * 4092), Array(src[4 * 4092..<5 * 4092]))
        XCTAssertEqual(loop.ack.count, 1, "ACKs = \(loop.ack.count), want 1")
        try feedData(rx, UInt32(5 * 4092), Array(src[5 * 4092..<6 * 4092]))
        XCTAssertEqual(loop.ack.count, 2, "ACKs = \(loop.ack.count), want 2")
        XCTAssertEqual(rx.state, .done, "state = \(rx.state), want Done")
        let finalAck = Bulk.decodeAck(loop.ack[loop.ack.count - 1])
        XCTAssertNotNil(finalAck, "final ACK next = \(String(describing: finalAck?.next)), want \(size)")
        XCTAssertEqual(finalAck?.next, size, "final ACK next = \(String(describing: finalAck?.next)), want \(size)")
    }

    /// `testDataAckCodec` covers the u32-offset DATA and u32+u16 ACK payload
    /// encodings and the short-payload rejections, independent of the golden
    /// vectors.
    func testDataAckCodec() throws {
        let offsets: [UInt32] = [0, 1, 4092, 12345, 4294967295]
        for off in offsets {
            let body: [UInt8] = [0xAA, 0xBB]
            let payload = try Bulk.encodeData(offset: off, bytes: body)
            XCTAssertEqual(payload.count, 4 + body.count, "EncodeData(\(off)) length = \(payload.count)")
            let decoded = Bulk.decodeData(payload)
            XCTAssertNotNil(decoded, "DecodeData(EncodeData(\(off))) rejected")
            XCTAssertEqual(decoded?.offset, off, "DecodeData(EncodeData(\(off))) offset")
            XCTAssertEqual(decoded?.bytes, body, "DecodeData(EncodeData(\(off))) bytes")
        }

        // Empty body still carries the offset.
        let empty = try Bulk.encodeData(offset: 7, bytes: [])
        XCTAssertEqual(empty.count, 4, "EncodeData(7, nil) = \(empty)")
        XCTAssertEqual(empty[0], 7, "EncodeData(7, nil) = \(empty)")
        XCTAssertNil(Bulk.decodeData([1, 2, 3]), "DecodeData accepted a 3-byte payload")

        let ack = Bulk.encodeAck(next: 4096, credits: 16)
        XCTAssertEqual(ack.count, 6, "EncodeAck(4096, 16) = \(ack)")
        XCTAssertEqual(ack[0], 0, "EncodeAck(4096, 16) = \(ack)")
        XCTAssertEqual(ack[1], 0x10, "EncodeAck(4096, 16) = \(ack)")
        XCTAssertEqual(ack[4], 16, "EncodeAck(4096, 16) = \(ack)")

        let maxAck = Bulk.decodeAck(Bulk.encodeAck(next: 4294967295, credits: 65535))
        XCTAssertNotNil(maxAck, "DecodeAck(EncodeAck(max)) rejected")
        XCTAssertEqual(maxAck?.next, 4294967295, "DecodeAck(EncodeAck(max)) next")
        XCTAssertEqual(maxAck?.credits, 65535, "DecodeAck(EncodeAck(max)) credits")

        for n in [0, 5, 7] {
            XCTAssertNil(Bulk.decodeAck([UInt8](repeating: 0, count: n)),
                         "DecodeAck accepted a \(n)-byte payload")
        }
    }

    func testCRC32KnownValues() {
        XCTAssertEqual(CRC32.checksum([] as [UInt8]), 0)
        XCTAssertEqual(CRC32.checksum(Array("123456789".utf8)), 0xCBF4_3926)

        let src = makeSource(1000)
        let a = Array(src[0..<333])
        let b = Array(src[333...])
        XCTAssertEqual(CRC32.update(CRC32.checksum(a), b), CRC32.checksum(a + b))
    }
}
