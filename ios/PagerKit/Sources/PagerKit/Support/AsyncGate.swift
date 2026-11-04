/// A one-at-a-time async gate with a FIFO queue, for an actor whose operations
/// must not overlap across their suspension points:
///
///     await gate.acquire()
///     defer { gate.release() }
///
/// Not an actor, so `release()` stays synchronous and can sit in a `defer`.
/// Owned by one actor (`let gate = AsyncGate()`); `acquire` runs on the
/// caller's isolation, so taking a free gate never leaves that actor.
final class AsyncGate {
    private var busy = false
    private var waiters: [CheckedContinuation<Void, Never>] = []

    init() {}

    /// Take the gate, waiting for whoever holds it.
    func acquire(isolation: isolated (any Actor)? = #isolation) async {
        if !busy {
            busy = true
            return
        }
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            waiters.append(continuation)
        }
        // Ownership was handed over by `release()`; `busy` is still true.
    }

    /// Hand the gate to the next caller, or free it.
    func release() {
        if waiters.isEmpty {
            busy = false
        } else {
            waiters.removeFirst().resume()
        }
    }
}
