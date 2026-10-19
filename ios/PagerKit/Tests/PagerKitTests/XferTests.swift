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
// completion. The `LossyLoop` harness adds the P7.29 cases: it counts every
// send, drops a chosen one, and simulates the owner's retransmit timer when
// both queues drain with bytes still unacknowledged.

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

/// `LossyLoop` is the lossy loopback of the P7.29 tests: DATA payloads travel
/// sender -> receiver and ACK payloads receiver -> sender, every send is
/// counted, and one send can be discarded by its 0-based index.
final class LossyLoop {
    var data: [[UInt8]] = [] // DATA payloads: sender -> receiver
    var ack: [[UInt8]] = []  // ACK payloads: receiver -> sender

    /// Every DATA send, including retransmissions.
    var dataSends = 0
    /// Every ACK send, including re-ACKs.
    var ackSends = 0
    /// How often the harness simulated the owner's retransmit timer.
    var timerFirings = 0

    private let dropData: Int?
    private let dropAck: Int?

    init(dropData: Int? = nil, dropAck: Int? = nil) {
        self.dropData = dropData
        self.dropAck = dropAck
    }

    func sendData(_ payload: [UInt8]) throws {
        let index = dataSends
        dataSends += 1
        if index == dropData { return }
        data.append(payload)
    }

    func sendAck(_ payload: [UInt8]) throws {
        let index = ackSends
        ackSends += 1
        if index == dropAck { return }
        ack.append(payload)
    }

    func deliverAllData(_ rx: XferReceiver) {
        while !data.isEmpty {
            rx.onData(data.removeFirst())
        }
    }

    func deliverAllAcks(_ tx: XferSender) {
        while !ack.isEmpty {
            tx.onAck(ack.removeFirst())
        }
    }

    /// Interleaved mode: one queued DATA to the receiver.
    func deliverData(_ rx: XferReceiver) {
        rx.onData(data.removeFirst())
    }

    /// Interleaved mode: one queued ACK to the sender (the caller pumps).
    func deliverAck(_ tx: XferSender) {
        tx.onAck(ack.removeFirst())
    }

    /// Simulate the owner's retransmit timer expiring: rewind then pump.
    func fireTimer(_ tx: XferSender) {
        timerFirings += 1
        _ = tx.retransmit()
        tx.pump()
    }
}

/// One completed (or bounded) lossy-loopback run.
private struct LossyRun {
    let loop: LossyLoop
    let sender: XferSender
    let receiver: XferReceiver
    let source: [UInt8]
    let destination: [UInt8]
}

/// Drive `size` bytes through a `LossyLoop`, bounded to `maxSteps`: deliver all
/// queued DATA, deliver all queued ACKs then pump, and when both queues drain
/// while the sender is not Done, fire the retransmit timer.
private func runLossy(
    size: UInt32,
    dropData: Int? = nil,
    dropAck: Int? = nil,
    maxSteps: Int = 100_000
) throws -> LossyRun {
    let source = makeSource(Int(size))
    var destination = [UInt8](repeating: 0, count: Int(size))
    let loop = LossyLoop(dropData: dropData, dropAck: dropAck)
    let sender = XferSender(size: size, chunk: Xfer.maxDataChunk, start: 0,
        read: readFrom(source), send: loop.sendData)
    let receiver = XferReceiver(size: size, window: Xfer.initialCredits, ackEvery: 4, start: 0,
        write: { off, b in destination.replaceSubrange(Int(off)..<(Int(off) + b.count), with: b) },
        ack: loop.sendAck)

    sender.pump()
    var steps = 0
    while steps < maxSteps {
        loop.deliverAllData(receiver)
        loop.deliverAllAcks(sender)
        sender.pump()
        if sender.state == .done && receiver.state == .done && loop.data.isEmpty && loop.ack.isEmpty {
            break
        }
        if loop.data.isEmpty && loop.ack.isEmpty && sender.state != .done {
            loop.fireTimer(sender)
        }
        steps += 1
    }
    return LossyRun(loop: loop, sender: sender, receiver: receiver,
                    source: source, destination: destination)
}

/// One interleaved lossy-loopback run: the completed `LossyRun`, the number of
/// loop steps it took, and whether it stopped at the step bound instead of at
/// Done.
private struct InterleavedRun {
    let run: LossyRun
    let steps: Int
    let hitStepBound: Bool
}

/// Drive `size` bytes through a `LossyLoop` in interleaved mode, bounded to
/// `maxSteps`: with DATA queued, deliver exactly one DATA and then every
/// pending ACK one at a time, pumping after each; with only ACKs queued, drain
/// them the same way; and when both queues drain while the sender is not Done,
/// fire the retransmit timer. Runs until both sides are Done or the step bound
/// is reached.
private func runLossyInterleaved(
    size: UInt32,
    dropData: Int? = nil,
    dropAck: Int? = nil,
    maxSteps: Int = 200_000
) throws -> InterleavedRun {
    let source = makeSource(Int(size))
    var destination = [UInt8](repeating: 0, count: Int(size))
    let loop = LossyLoop(dropData: dropData, dropAck: dropAck)
    let sender = XferSender(size: size, chunk: Xfer.maxDataChunk, start: 0,
        read: readFrom(source), send: loop.sendData)
    let receiver = XferReceiver(size: size, window: Xfer.initialCredits, ackEvery: 4, start: 0,
        write: { off, b in destination.replaceSubrange(Int(off)..<(Int(off) + b.count), with: b) },
        ack: loop.sendAck)

    sender.pump()
    var steps = 0
    while steps < maxSteps {
        if sender.state == .done && receiver.state == .done
            && loop.data.isEmpty && loop.ack.isEmpty {
            break
        }
        if !loop.data.isEmpty {
            // One DATA per step, then every ACK it drew, pumping after each.
            loop.deliverData(receiver)
            while !loop.ack.isEmpty {
                loop.deliverAck(sender)
                sender.pump()
            }
        } else if !loop.ack.isEmpty {
            while !loop.ack.isEmpty {
                loop.deliverAck(sender)
                sender.pump()
            }
        } else if sender.state != .done {
            loop.fireTimer(sender)
        } else {
            break
        }
        steps += 1
    }
    let finished = sender.state == .done && receiver.state == .done
        && loop.data.isEmpty && loop.ack.isEmpty
    return InterleavedRun(
        run: LossyRun(loop: loop, sender: sender, receiver: receiver,
                      source: source, destination: destination),
        steps: steps,
        hitStepBound: !finished)
}

/// Fail when an interleaved run stopped at the step bound rather than at Done,
/// reporting the counts.
private func assertInterleavedCompleted(
    _ result: InterleavedRun,
    file: StaticString = #filePath,
    line: UInt = #line
) {
    XCTAssertFalse(result.hitStepBound,
                   "stopped after \(result.steps) steps: DATA sends = \(result.run.loop.dataSends), ACK sends = \(result.run.loop.ackSends), timer firings = \(result.run.loop.timerFirings)",
                   file: file, line: line)
}

/// Assert the receiver wrote exactly the source bytes.
private func assertReceived(
    _ run: LossyRun,
    file: StaticString = #filePath,
    line: UInt = #line
) {
    XCTAssertEqual(run.receiver.received, UInt32(run.source.count),
                   "received = \(run.receiver.received), want \(run.source.count)",
                   file: file, line: line)
    for i in run.source.indices where run.source[i] != run.destination[i] {
        XCTFail("byte \(i) = \(run.destination[i]), want \(run.source[i])", file: file, line: line)
        return
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

        // Drop the first DATA message and deliver the rest: they are all at the
        // wrong offset, so the receiver re-ACKs offset 0 once for the gap
        // without writing.
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

    // MARK: - Lossy loopback (P7.29)

    /// Drop DATA #6 (0-based index 5) of a 100 KB transfer. The loss lands just
    /// after the first 4-message ACK boundary: the regular ACK reaches offset
    /// 4*4092, then the gap's first ACK tells the sender the expected offset
    /// 5*4092 and the duplicate that follows triggers its one rewind, so no
    /// retransmit timer is needed.
    func testLossyDropMidStreamData() throws {
        let run = try runLossy(size: 102_400, dropData: 5)

        XCTAssertEqual(run.sender.state, .done, "sender state = \(run.sender.state), want Done")
        XCTAssertEqual(run.receiver.state, .done, "receiver state = \(run.receiver.state), want Done")
        assertReceived(run)
        XCTAssertLessThanOrEqual(run.loop.dataSends, 52,
                                 "DATA sends = \(run.loop.dataSends), want <= 52")
        XCTAssertEqual(run.loop.timerFirings, 0,
                       "timer firings = \(run.loop.timerFirings), want 0")
    }

    /// Drop DATA #5 (0-based index 4) of a 100 KB transfer, the message right
    /// after the first 4-message ACK boundary. The gap's only ACK repeats the
    /// offset the sender already knows, so it rewinds at once; no retransmit
    /// timer is needed either.
    func testLossyDropOnAckBoundary() throws {
        let run = try runLossy(size: 102_400, dropData: 4)

        XCTAssertEqual(run.sender.state, .done, "sender state = \(run.sender.state), want Done")
        XCTAssertEqual(run.receiver.state, .done, "receiver state = \(run.receiver.state), want Done")
        assertReceived(run)
        XCTAssertLessThanOrEqual(run.loop.dataSends, 52,
                                 "DATA sends = \(run.loop.dataSends), want <= 52")
        XCTAssertEqual(run.loop.timerFirings, 0,
                       "timer firings = \(run.loop.timerFirings), want 0")
    }

    /// The last DATA of an 8 KB transfer is lost. The receiver is waiting for
    /// it, so no re-ACK can arrive; exactly one retransmit timer firing
    /// rewinds the sender and carries the transfer to completion.
    func testLossyDropLastData() throws {
        let run = try runLossy(size: 8_192, dropData: 2)

        XCTAssertEqual(run.sender.state, .done, "sender state = \(run.sender.state), want Done")
        XCTAssertEqual(run.receiver.state, .done, "receiver state = \(run.receiver.state), want Done")
        assertReceived(run)
        XCTAssertEqual(run.loop.timerFirings, 1,
                       "timer firings = \(run.loop.timerFirings), want 1")
    }

    /// DATA #0 of a 256 KB transfer is lost. The first wrong-offset DATA re-ACKs
    /// offset 0 (a duplicate of the initial acknowledged offset), so the sender
    /// rewinds once and the transfer completes without a timer.
    func testLossyDropFirstData() throws {
        let run = try runLossy(size: 262_144, dropData: 0)

        XCTAssertEqual(run.sender.state, .done, "sender state = \(run.sender.state), want Done")
        XCTAssertEqual(run.receiver.state, .done, "receiver state = \(run.receiver.state), want Done")
        assertReceived(run)
        XCTAssertLessThanOrEqual(run.loop.dataSends, 130,
                                 "DATA sends = \(run.loop.dataSends), want <= 130")
        XCTAssertLessThanOrEqual(run.loop.timerFirings, 1,
                                 "timer firings = \(run.loop.timerFirings), want <= 1")
    }

    /// The receiver's completion ACK is lost (the seventh ACK send of a 100 KB
    /// transfer). The receiver is Done; one timer firing retransmits, the Done
    /// receiver re-ACKs its final offset, and the sender completes.
    func testLossyDropFinalAck() throws {
        let run = try runLossy(size: 102_400, dropAck: 6)

        XCTAssertEqual(run.receiver.state, .done, "receiver state = \(run.receiver.state), want Done")
        XCTAssertEqual(run.sender.state, .done, "sender state = \(run.sender.state), want Done")
        assertReceived(run)
        XCTAssertEqual(run.loop.timerFirings, 1,
                       "timer firings = \(run.loop.timerFirings), want 1")
    }

    /// Lossless runs: the DATA-send baselines are 3, 26 and 65 for 8 KB, 100 KB
    /// and 256 KB, and no retransmit timer fires.
    func testLossyLosslessCounts() throws {
        let cases: [(size: UInt32, sends: Int)] = [
            (size: 8_192, sends: 3),
            (size: 102_400, sends: 26),
            (size: 262_144, sends: 65),
        ]
        for testCase in cases {
            let run = try runLossy(size: testCase.size)

            XCTAssertEqual(run.sender.state, .done,
                           "size \(testCase.size): sender state = \(run.sender.state), want Done")
            XCTAssertEqual(run.receiver.state, .done,
                           "size \(testCase.size): receiver state = \(run.receiver.state), want Done")
            assertReceived(run)
            XCTAssertEqual(run.loop.dataSends, testCase.sends,
                           "size \(testCase.size): DATA sends = \(run.loop.dataSends), want \(testCase.sends)")
            XCTAssertEqual(run.loop.timerFirings, 0,
                           "size \(testCase.size): timer firings = \(run.loop.timerFirings), want 0")
        }
    }

    // MARK: - Interleaved loopback

    /// Interleaved loop: drop DATA #6 (0-based index 5) of a 100 KB transfer.
    /// One DATA per step, ACKs drained after each, so the gap's two ACKs reach
    /// the sender promptly and its one rewind resends the lost message; no
    /// retransmit timer is needed.
    func testInterleavedDropMidData() throws {
        let result = try runLossyInterleaved(size: 102_400, dropData: 5)
        assertInterleavedCompleted(result)
        let run = result.run

        XCTAssertEqual(run.sender.state, .done, "sender state = \(run.sender.state), want Done")
        XCTAssertEqual(run.receiver.state, .done, "receiver state = \(run.receiver.state), want Done")
        assertReceived(run)
        XCTAssertLessThanOrEqual(run.loop.dataSends, 52,
                                 "DATA sends = \(run.loop.dataSends), want <= 52")
        XCTAssertEqual(run.loop.timerFirings, 0,
                       "timer firings = \(run.loop.timerFirings), want 0")
    }

    /// Interleaved loop: DATA #0 (0-based index 0) of a 256 KB transfer is
    /// lost. Its first wrong-offset DATA re-ACKs offset 0, the sender rewinds
    /// once, and the transfer completes with at most one timer firing.
    func testInterleavedDropFirstData() throws {
        let result = try runLossyInterleaved(size: 262_144, dropData: 0)
        assertInterleavedCompleted(result)
        let run = result.run

        XCTAssertEqual(run.sender.state, .done, "sender state = \(run.sender.state), want Done")
        XCTAssertEqual(run.receiver.state, .done, "receiver state = \(run.receiver.state), want Done")
        assertReceived(run)
        XCTAssertLessThanOrEqual(run.loop.dataSends, 130,
                                 "DATA sends = \(run.loop.dataSends), want <= 130")
        XCTAssertLessThanOrEqual(run.loop.timerFirings, 1,
                                 "timer firings = \(run.loop.timerFirings), want <= 1")
    }

    /// Interleaved lossless baseline: a 100 KB transfer takes exactly 26 DATA
    /// sends and no retransmit timer fires.
    func testInterleavedLossless() throws {
        let result = try runLossyInterleaved(size: 102_400)
        assertInterleavedCompleted(result)
        let run = result.run

        XCTAssertEqual(run.sender.state, .done, "sender state = \(run.sender.state), want Done")
        XCTAssertEqual(run.receiver.state, .done, "receiver state = \(run.receiver.state), want Done")
        assertReceived(run)
        XCTAssertEqual(run.loop.dataSends, 26,
                       "DATA sends = \(run.loop.dataSends), want 26")
        XCTAssertEqual(run.loop.timerFirings, 0,
                       "timer firings = \(run.loop.timerFirings), want 0")
    }

    /// Two identical no-progress ACKs while bytes are in flight: the first
    /// rewinds, the second must not rewind again.
    func testDuplicateAckRewindsOnce() throws {
        let size: UInt32 = 20 * 4092
        let src = makeSource(Int(size))
        let loop = XferLoop()
        let tx = XferSender(size: size, chunk: 4092, start: 0, read: readFrom(src), send: loop.sendData)

        tx.pump()
        XCTAssertEqual(tx.sent, UInt32(16 * 4092), "sent = \(tx.sent), want \(16 * 4092)")

        let duplicate = Bulk.encodeAck(next: 0, credits: Xfer.initialCredits)
        tx.onAck(duplicate)
        XCTAssertEqual(tx.sent, 0, "sent/acked = \(tx.sent)/\(tx.acked), want 0/0")
        XCTAssertEqual(tx.acked, 0, "sent/acked = \(tx.sent)/\(tx.acked), want 0/0")

        tx.pump()
        let resent = tx.sent
        XCTAssertGreaterThan(resent, tx.acked,
                             "sent/acked = \(resent)/\(tx.acked), want bytes in flight")

        tx.onAck(duplicate)
        XCTAssertEqual(tx.sent, resent, "sent = \(tx.sent) (after \(resent)), want no second rewind")
        XCTAssertGreaterThan(tx.sent, tx.acked,
                             "sent/acked = \(tx.sent)/\(tx.acked), want bytes still in flight")
    }

    /// Three wrong-offset DATA draw exactly one re-ACK when the sender already
    /// knows the expected offset (a fresh receiver's last ACK is `start`, 0
    /// here); the expected DATA resets the gap, and a later wrong offset draws
    /// the two ACKs of a fresh gap: one that tells the sender the new offset
    /// 4092, then the duplicate that triggers its one rewind.
    func testReceiverReAcksOncePerGap() throws {
        let size: UInt32 = 4 * 4092
        let loop = XferLoop()
        let rx = XferReceiver(size: size, window: Xfer.initialCredits, ackEvery: 4, start: 0,
            write: { _, _ in },
            ack: loop.sendAck)

        // The sender starts with acked == start, so lastAck == next == 0 and
        // the gap's only ACK is the duplicate.
        for _ in 0..<3 {
            try feedData(rx, 4092, [UInt8](repeating: 0, count: 4092))
        }
        XCTAssertEqual(loop.ack.count, 1, "ACKs = \(loop.ack.count), want 1")
        XCTAssertEqual(rx.received, 0, "received = \(rx.received), want 0")
        XCTAssertEqual(Bulk.decodeAck(loop.ack[0])?.next, 0,
                       "re-ACK next = \(String(describing: Bulk.decodeAck(loop.ack[0])?.next)), want 0")

        // The expected offset arrives: the gap closes and no ACK is due yet.
        try feedData(rx, 0, [UInt8](repeating: 1, count: 4092))
        XCTAssertEqual(rx.received, 4092, "received = \(rx.received), want 4092")
        XCTAssertEqual(loop.ack.count, 1, "ACKs = \(loop.ack.count), want 1")

        // A fresh gap now draws two ACKs, both at the new expected offset: the
        // first tells the sender 4092 (its last ACK was still 0), the second is
        // the duplicate that triggers its one rewind.
        try feedData(rx, 2 * 4092, [UInt8](repeating: 2, count: 4092))
        XCTAssertEqual(loop.ack.count, 3, "ACKs = \(loop.ack.count), want 3")
        XCTAssertEqual(Bulk.decodeAck(loop.ack[1])?.next, 4092,
                       "re-ACK next = \(String(describing: Bulk.decodeAck(loop.ack[1])?.next)), want 4092")
        XCTAssertEqual(Bulk.decodeAck(loop.ack[2])?.next, 4092,
                       "re-ACK next = \(String(describing: Bulk.decodeAck(loop.ack[2])?.next)), want 4092")
    }

    /// `retransmit` is false with nothing unacknowledged and true with bytes in
    /// flight, where it moves `sent` back to `acked`.
    func testRetransmitOnlyWithBytesInFlight() throws {
        // A fresh zero-size transfer is already Done.
        let empty = XferSender(size: 0, chunk: 4092, start: 0,
            read: { _, _ in [] },
            send: { _ in })
        XCTAssertFalse(empty.retransmit(), "retransmit() = true on a zero-size sender, want false")

        // After Done there is nothing left to resend.
        let size: UInt32 = 2 * 4092
        let src = makeSource(Int(size))
        let loop = XferLoop()
        let finished = XferSender(size: size, chunk: 4092, start: 0,
            read: readFrom(src), send: loop.sendData)
        finished.pump()
        finished.onAck(Bulk.encodeAck(next: size, credits: Xfer.initialCredits))
        XCTAssertEqual(finished.state, .done, "state = \(finished.state), want Done")
        XCTAssertFalse(finished.retransmit(), "retransmit() = true after Done, want false")

        // With bytes in flight it rewinds to the acknowledged offset.
        let loop2 = XferLoop()
        let tx = XferSender(size: size, chunk: 4092, start: 0,
            read: readFrom(src), send: loop2.sendData)
        tx.pump()
        XCTAssertGreaterThan(tx.sent, tx.acked,
                             "sent/acked = \(tx.sent)/\(tx.acked), want bytes in flight")
        XCTAssertTrue(tx.retransmit(), "retransmit() = false with bytes in flight, want true")
        XCTAssertEqual(tx.sent, tx.acked,
                       "sent/acked = \(tx.sent)/\(tx.acked), want a rewind")
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
