/// PagerLink v1 framing constants (PROTOCOL.md §2, §4). Mirrors
/// `src/hal/link/link_frame.h` and the reference implementation in
/// `protocol/tools/mkvectors.py`.
public enum PagerLink {
    /// Wire protocol version; a message whose ver byte differs is malformed.
    public static let protoVersion: UInt8 = 1
    /// Message header size in bytes (ver, type, id u16, len u16).
    public static let headerSize = 6
    /// Maximum payload carried by one message.
    public static let maxPayload = 4096
    /// Maximum reassembled message length: header + payload.
    public static let maxMessage = headerSize + maxPayload
    /// Packet flags: bit7 marks the first packet of a message.
    public static let flagStart: UInt8 = 0x80
    /// Packet flags: bit6 marks the last packet of a message.
    public static let flagEnd: UInt8 = 0x40
    /// Packet flags: bits5..0 hold the rolling sequence number.
    public static let seqMask: UInt8 = 0x3F
    /// Minimum packet size (equivalent to ATT_MTU 23).
    public static let minPacket = 20
    /// Fixed packet size for the TCP transport (equivalent to MTU 247).
    public static let tcpPacket = 244
    /// Implicit initial bulk-transfer credit window, in packets.
    public static let initialCredits: UInt16 = 16
}

/// Message-layer `type` field (PROTOCOL.md §3).
public enum MsgType: UInt8, Sendable {
    case req = 0
    case res = 1
    case err = 2
    case evt = 3
    case data = 4
    case ack = 5
}

/// Why `Message.decode` rejected a buffer; the raw value is the token used by
/// the reference implementation and the golden vectors.
public enum DecodeError: String, Error, Equatable, Sendable {
    case short
    case version
    case type
    case oversize
    case length
}

/// Little-endian wire helpers shared by the frame and bulk codecs.
enum LE {
    static func u16(_ bytes: [UInt8], _ index: Int) -> UInt16 {
        UInt16(bytes[index]) | (UInt16(bytes[index + 1]) << 8)
    }

    static func u32(_ bytes: [UInt8], _ index: Int) -> UInt32 {
        UInt32(bytes[index])
            | (UInt32(bytes[index + 1]) << 8)
            | (UInt32(bytes[index + 2]) << 16)
            | (UInt32(bytes[index + 3]) << 24)
    }

    static func appendU16(_ out: inout [UInt8], _ value: UInt16) {
        out.append(UInt8(value & 0xFF))
        out.append(UInt8((value >> 8) & 0xFF))
    }

    static func appendU32(_ out: inout [UInt8], _ value: UInt32) {
        out.append(UInt8(value & 0xFF))
        out.append(UInt8((value >> 8) & 0xFF))
        out.append(UInt8((value >> 16) & 0xFF))
        out.append(UInt8((value >> 24) & 0xFF))
    }
}

/// A decoded message: the 6-byte little-endian header plus the raw payload.
public struct Message: Equatable, Sendable {
    public var type: MsgType
    public var id: UInt16
    public var payload: [UInt8]

    public init(type: MsgType, id: UInt16, payload: [UInt8]) {
        self.type = type
        self.id = id
        self.payload = payload
    }

    /// Encode the header plus payload. Throws `DecodeError.oversize` when the
    /// payload exceeds `PagerLink.maxPayload`.
    public func encoded() throws -> [UInt8] {
        guard payload.count <= PagerLink.maxPayload else { throw DecodeError.oversize }
        var out: [UInt8] = []
        out.reserveCapacity(PagerLink.headerSize + payload.count)
        out.append(PagerLink.protoVersion)
        out.append(type.rawValue)
        LE.appendU16(&out, id)
        LE.appendU16(&out, UInt16(payload.count))
        out.append(contentsOf: payload)
        return out
    }

    /// Decode a message header + payload. Checks, in order: shorter than 6 bytes
    /// → `short`, `ver != 1` → `version`, `type > 5` → `type`, payload longer
    /// than 4096 → `oversize`, `6 + len != length` → `length`.
    public static func decode(_ bytes: [UInt8]) throws -> Message {
        guard bytes.count >= PagerLink.headerSize else { throw DecodeError.short }
        let version = bytes[0]
        let id = LE.u16(bytes, 2)
        let length = Int(LE.u16(bytes, 4))
        guard version == PagerLink.protoVersion else { throw DecodeError.version }
        guard let type = MsgType(rawValue: bytes[1]) else { throw DecodeError.type }
        guard length <= PagerLink.maxPayload else { throw DecodeError.oversize }
        guard PagerLink.headerSize + length == bytes.count else { throw DecodeError.length }
        return Message(type: type, id: id, payload: Array(bytes[PagerLink.headerSize...]))
    }
}

/// Bulk-transfer payload codec (PROTOCOL.md §4): DATA and ACK.
public enum Bulk {
    /// Encode a DATA payload: u32 offset + raw bytes. Throws when `bytes`
    /// exceeds `PagerLink.maxPayload - 4`.
    public static func encodeData(offset: UInt32, bytes: [UInt8]) throws -> [UInt8] {
        guard bytes.count <= PagerLink.maxPayload - 4 else { throw DecodeError.oversize }
        var out: [UInt8] = []
        out.reserveCapacity(4 + bytes.count)
        LE.appendU32(&out, offset)
        out.append(contentsOf: bytes)
        return out
    }

    /// Decode a DATA payload; nil when shorter than 4 bytes.
    public static func decodeData(_ payload: [UInt8]) -> (offset: UInt32, bytes: [UInt8])? {
        guard payload.count >= 4 else { return nil }
        return (LE.u32(payload, 0), Array(payload[4...]))
    }

    /// Encode an ACK payload: u32 next_expected_offset + u16 credits (6 bytes).
    public static func encodeAck(next: UInt32, credits: UInt16) -> [UInt8] {
        var out: [UInt8] = []
        out.reserveCapacity(6)
        LE.appendU32(&out, next)
        LE.appendU16(&out, credits)
        return out
    }

    /// Decode an ACK payload; nil unless exactly 6 bytes.
    public static func decodeAck(_ payload: [UInt8]) -> (next: UInt32, credits: UInt16)? {
        guard payload.count == 6 else { return nil }
        return (LE.u32(payload, 0), LE.u16(payload, 4))
    }
}
