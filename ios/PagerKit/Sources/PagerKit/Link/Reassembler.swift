/// Why the reassembler dropped a partial message; the raw value is the reason
/// carried by the local `link.reset` event.
public enum ResetReason: String, Sendable {
    case empty
    case seqGap = "seq_gap"
    case unexpectedStart = "unexpected_start"
    case unexpectedContinue = "unexpected_continue"
    case oversize
}

/// One reassembler outcome: a delivered message, or a dropped partial.
public enum ReassemblyEvent: Equatable, Sendable {
    case message([UInt8])
    case reset(ResetReason)
}

/// Reassembles packets back into whole messages, mirroring the reference
/// implementation packet-for-packet, including the discard state entered after
/// an out-of-sequence or oversize fault (PROTOCOL.md §2). Mirrors `Reassembler`
/// in `protocol/tools/mkvectors.py` and `src/hal/link/link_frame.cpp`.
public struct Reassembler: Sendable {
    private var expected: UInt8 = 0
    private var partial: [UInt8]?
    private var discarding = false

    public init() {}

    /// Reset expected_seq to 0, drop any partial message, leave discard state.
    public mutating func reset() {
        expected = 0
        partial = nil
        discarding = false
    }

    /// Feed one packet (flags byte + message bytes) and return the events it
    /// produced.
    public mutating func feed(_ packet: [UInt8]) -> [ReassemblyEvent] {
        var events: [ReassemblyEvent] = []

        // Empty packet (0 bytes or only the flags byte): reset, drop.
        guard packet.count >= 2 else {
            events.append(.reset(.empty))
            return events
        }
        let flags = packet[0]
        let data = packet[1...]
        let seq = flags & PagerLink.seqMask

        if seq != expected {
            events.append(.reset(.seqGap))
            partial = nil
            discarding = true
        }
        expected = (seq + 1) & PagerLink.seqMask

        if flags & PagerLink.flagStart != 0 {
            discarding = false
            if partial != nil { events.append(.reset(.unexpectedStart)) }
            partial = []
        } else if partial == nil {
            if !discarding { events.append(.reset(.unexpectedContinue)) }
            return events
        }

        if partial!.count + data.count > PagerLink.maxMessage {
            events.append(.reset(.oversize))
            partial = nil
            discarding = true
            return events
        }
        partial!.append(contentsOf: data)
        if flags & PagerLink.flagEnd != 0 {
            events.append(.message(partial!))
            partial = nil
        }
        return events
    }
}
