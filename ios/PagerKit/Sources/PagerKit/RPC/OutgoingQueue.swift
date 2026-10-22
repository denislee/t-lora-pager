import Foundation

/// Two-lane FIFO for the LinkSession writer (P7.63): `next()` returns the
/// oldest control item if any, else the oldest bulk item, else waits. Each
/// lane keeps its own order. Many producers, ONE consumer (the writer task).
/// Lane choice happens per whole message, so packets never interleave.
///
/// `@unchecked Sendable`: all state (`control`, `controlHead`, `bulk`,
/// `bulkHead`, `waiter`, `finished`) is guarded by `lock`. `Element` must be
/// `Sendable` because a queued item crosses from a producer to the consumer.
final class OutgoingQueue<Element: Sendable>: @unchecked Sendable {
    private let lock = NSLock()
    private var control: [Element] = []
    private var controlHead = 0
    private var bulk: [Element] = []
    private var bulkHead = 0
    private var waiter: CheckedContinuation<Element?, Never>?
    private var finished = false

    /// Enqueue `item` on the control lane when `isControl`, else on the bulk
    /// lane. Silently dropped after `finish()`. A parked consumer takes the
    /// item directly instead of it being queued; that continuation is resumed
    /// outside the lock.
    func push(_ item: Element, control isControl: Bool) {
        lock.lock()
        if finished {
            lock.unlock()
            return
        }
        if let waiter {
            self.waiter = nil
            lock.unlock()
            waiter.resume(returning: item)
            return
        }
        if isControl {
            control.append(item)
        } else {
            bulk.append(item)
        }
        lock.unlock()
    }

    /// The oldest control item if any, else the oldest bulk item, else — while
    /// the queue is still open — a suspension until the next `push`/`finish`.
    /// Returns nil once `finish()` has been called and both lanes are drained.
    func next() async -> Element? {
        // All locking happens in the synchronous continuation body (NSLock is
        // noasync): the same critical section checks both lanes and parks, so a
        // producer can neither slip an item in nor miss the waiter.
        await withCheckedContinuation { (continuation: CheckedContinuation<Element?, Never>) in
            lock.lock()
            if let item = popControlLocked() ?? popBulkLocked() {
                lock.unlock()
                continuation.resume(returning: item)
                return
            }
            if finished {
                lock.unlock()
                continuation.resume(returning: nil)
                return
            }
            waiter = continuation
            lock.unlock()
        }
    }

    /// Close the queue: later `push`es are dropped and a parked consumer wakes
    /// with nil. Items already queued stay drainable through `next()`.
    func finish() {
        lock.lock()
        finished = true
        let waiter = self.waiter
        self.waiter = nil
        lock.unlock()
        waiter?.resume(returning: nil)
    }

    /// Items currently queued across both lanes (test helper).
    var count: Int {
        lock.lock()
        defer { lock.unlock() }
        return (control.count - controlHead) + (bulk.count - bulkHead)
    }

    /// Pop the control lane's head. Caller holds `lock`.
    private func popControlLocked() -> Element? {
        guard controlHead < control.count else { return nil }
        let item = control[controlHead]
        controlHead += 1
        // Reclaim consumed prefix once it dominates the array, so a long-lived
        // queue does not grow without bound (removeFirst is O(n), amortised).
        if controlHead > 32, controlHead * 2 > control.count {
            control.removeFirst(controlHead)
            controlHead = 0
        }
        return item
    }

    /// Pop the bulk lane's head. Caller holds `lock`.
    private func popBulkLocked() -> Element? {
        guard bulkHead < bulk.count else { return nil }
        let item = bulk[bulkHead]
        bulkHead += 1
        if bulkHead > 32, bulkHead * 2 > bulk.count {
            bulk.removeFirst(bulkHead)
            bulkHead = 0
        }
        return item
    }
}
