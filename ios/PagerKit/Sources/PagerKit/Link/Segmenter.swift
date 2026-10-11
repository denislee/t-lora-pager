/// Splits messages into link-layer packets, tracking the rolling seq counter
/// (PROTOCOL.md §2). Mirrors `Segmenter` in `src/hal/link/link_frame.cpp`.
public struct Segmenter: Sendable {
    /// Next sequence number to send; masked to 6 bits.
    public var seq: UInt8 = 0

    public init() {}

    /// Split `message` into packets of one flags byte plus up to
    /// `packetSize - 1` message bytes, in order. Returns an empty array when
    /// `packetSize < 2` or `message` is empty. The first packet has START, the
    /// last has END; `seq` advances by one (mod 64) per packet.
    public mutating func segment(_ message: [UInt8], packetSize: Int) -> [[UInt8]] {
        guard packetSize >= 2, !message.isEmpty else { return [] }
        let room = packetSize - 1
        var out: [[UInt8]] = []
        var offset = 0
        while offset < message.count {
            let take = min(room, message.count - offset)
            var flags = seq & PagerLink.seqMask
            if offset == 0 { flags |= PagerLink.flagStart }
            if offset + room >= message.count { flags |= PagerLink.flagEnd }
            var packet: [UInt8] = [flags]
            packet.append(contentsOf: message[offset..<(offset + take)])
            out.append(packet)
            seq = (seq + 1) & PagerLink.seqMask
            offset += take
        }
        return out
    }
}
