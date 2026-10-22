import XCTest

@testable import PagerKit

// P7.63: the LinkSession writer's two-lane FIFO. Control messages (everything
// except DATA) jump queued DATA, and each lane keeps its own order — the lane
// choice happens per whole message, so a message's packets still go out back
// to back.

final class OutgoingQueueTests: XCTestCase {
    /// Control items drain before bulk ones, each in push order.
    func testControlJumpsBulk() async {
        let queue = OutgoingQueue<Int>()
        queue.push(1, control: false)
        queue.push(2, control: false)
        queue.push(3, control: false)
        queue.push(10, control: true)
        queue.push(4, control: false)
        queue.push(11, control: true)

        var got: [Int] = []
        for _ in 0..<6 {
            let item = await queue.next()
            got.append(item!)
        }
        XCTAssertEqual(got, [10, 11, 1, 2, 3, 4])
        XCTAssertEqual(queue.count, 0)
    }

    /// Interleaved pushes come out as the whole control lane, then the whole
    /// bulk lane, each in its own push order.
    func testEachLaneKeepsOrder() async {
        let queue = OutgoingQueue<Int>()
        for i in 0..<100 {
            queue.push(i, control: false)
            queue.push(1000 + i, control: true)
        }
        XCTAssertEqual(queue.count, 200)

        var got: [Int] = []
        for _ in 0..<200 {
            let item = await queue.next()
            got.append(item!)
        }
        XCTAssertEqual(got, Array(1000..<1100) + Array(0..<100))
        XCTAssertEqual(queue.count, 0)
    }

    /// A consumer parked in `next()` is handed the next push directly.
    func testWaiterGetsLaterPush() async {
        let queue = OutgoingQueue<Int>()
        let consumer = Task { await queue.next() }
        try? await Task.sleep(nanoseconds: 20_000_000)
        queue.push(7, control: true)

        let value = await consumer.value
        XCTAssertEqual(value, 7)
        XCTAssertEqual(queue.count, 0)
    }

    /// `finish()` wakes a parked consumer with nil.
    func testFinishWakesWaiterWithNil() async {
        let queue = OutgoingQueue<Int>()
        let consumer = Task { await queue.next() }
        try? await Task.sleep(nanoseconds: 20_000_000)
        queue.finish()

        let value = await consumer.value
        XCTAssertNil(value)
    }

    /// Queued items stay drainable after `finish()`; then nil, and later
    /// pushes are dropped.
    func testFinishDrainsThenNil() async {
        let queue = OutgoingQueue<Int>()
        queue.push(1, control: false)
        queue.finish()

        let first = await queue.next()
        XCTAssertEqual(first, 1)
        let second = await queue.next()
        XCTAssertNil(second)

        queue.push(2, control: true)
        XCTAssertEqual(queue.count, 0)
    }

    /// Compacting a consumed prefix does not disturb lane order.
    func testCompactionKeepsOrder() async {
        let queue = OutgoingQueue<Int>()
        for i in 0..<1000 {
            queue.push(i, control: false)
        }
        for _ in 0..<600 {
            let item = await queue.next()
            XCTAssertNotNil(item)
        }
        for i in 0..<10 {
            queue.push(1000 + i, control: true)
        }

        var got: [Int] = []
        for _ in 0..<410 {
            let item = await queue.next()
            got.append(item!)
        }
        XCTAssertEqual(got, Array(1000..<1010) + Array(600..<1000))
        XCTAssertEqual(queue.count, 0)
    }
}
