import XCTest

@testable import PagerKit

final class AsyncGateTests: XCTestCase {
    /// Overlapping calls that each suspend while holding the gate.
    private actor Holder {
        private let gate = AsyncGate()
        private var active = 0
        private(set) var maxActive = 0
        private(set) var finished = 0

        func run() async {
            await gate.acquire()
            defer { gate.release() }
            active += 1
            maxActive = max(maxActive, active)
            try? await Task.sleep(for: .milliseconds(5))
            active -= 1
            finished += 1
        }
    }

    func testOneHolderAtATimeAndEveryWaiterRuns() async {
        let holder = Holder()
        await withTaskGroup(of: Void.self) { group in
            for _ in 0..<8 { group.addTask { await holder.run() } }
        }
        let maxActive = await holder.maxActive
        let finished = await holder.finished
        XCTAssertEqual(maxActive, 1)
        XCTAssertEqual(finished, 8)
    }
}
