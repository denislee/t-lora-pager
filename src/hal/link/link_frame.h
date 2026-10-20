/**
 * @file      link_frame.h
 * @brief     PagerLink v1 framing codec — message header, segmentation and reassembly.
 *
 * Pure C++17: no Arduino / ESP-IDF headers, so this compiles on the host for
 * the unit tests as well as on the pager. Mirrors the normative reference
 * implementation in protocol/tools/mkvectors.py (PROTOCOL.md §2-§4) and is
 * validated against the golden vectors in protocol/vectors/.
 *
 * All multi-byte integers on the wire are little-endian.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace pagerlink {

/** Wire protocol version; a message whose ver byte differs is malformed. */
constexpr uint8_t PROTO_VERSION = 1;
/** Message header size in bytes (ver, type, id u16, len u16). */
constexpr size_t HEADER_SIZE = 6;
/** Maximum payload carried by one message. */
constexpr size_t MAX_PAYLOAD = 4096;
/** Maximum reassembled message length: header + payload. */
constexpr size_t MAX_MESSAGE = HEADER_SIZE + MAX_PAYLOAD;
/** Packet flags: bit7 marks the first packet of a message. */
constexpr uint8_t FLAG_START = 0x80;
/** Packet flags: bit6 marks the last packet of a message. */
constexpr uint8_t FLAG_END = 0x40;
/** Packet flags: bits5..0 hold the rolling sequence number. */
constexpr uint8_t SEQ_MASK = 0x3F;
/** Minimum packet size (equivalent to ATT_MTU 23). */
constexpr size_t MIN_PACKET = 20;
/** Fixed packet size for the TCP transport (equivalent to MTU 247). */
constexpr size_t TCP_PACKET = 244;
/** Implicit initial bulk-transfer credit window, in DATA messages (up to 4092 B each), not packets. */
constexpr uint16_t INITIAL_CREDITS = 16;
/** Bulk sender retransmit timeout: no ACK for this long with bytes unacknowledged ->
 * rewind (PROTOCOL.md §4). */
constexpr uint32_t XFER_RETRANSMIT_MS = 2000;

/**
 * Link packets one message of `msg_len` bytes needs at `packet_size`: each
 * packet is one flags byte plus `packet_size - 1` message bytes. Returns 0
 * when `packet_size < 2` or `msg_len == 0`.
 */
constexpr size_t packets_per_message(size_t msg_len, size_t packet_size) {
    /* single return: the ESP32 toolchain builds as C++11 */
    return (packet_size < 2 || msg_len == 0) ? 0
                                             : (msg_len + (packet_size - 2)) / (packet_size - 1);
}

/**
 * Bulk credit grant that fits a receive queue of `queue_packets` packets: how
 * many MAX_MESSAGE-sized DATA messages fit at `packet_size`, clamped to
 * 1..INITIAL_CREDITS. 0 queue_packets = unbounded (the transport
 * back-pressures, e.g. TCP) -> INITIAL_CREDITS. packet_size < 2 -> 1.
 */
uint16_t rx_credits_for(size_t queue_packets, size_t packet_size);

/**
 * ack_every for a grant, so the receiver ACKs before the sender's window runs
 * out: 4 at >= 8 credits, 2 at 4..7, else 1.
 */
unsigned rx_ack_every_for(uint16_t credits);

/** Message type field. */
enum class MsgType : uint8_t { Req = 0, Res = 1, Err = 2, Evt = 3, Data = 4, Ack = 5 };

/** Why decode_message() rejected a buffer. */
enum class DecodeError : uint8_t { None, Short, Version, Type, Oversize, Length };

/** Human-readable name for a DecodeError ("none", "short", ...). */
const char *decode_error_name(DecodeError e);

/** A decoded message. `payload` holds the raw payload bytes. */
struct Message {
    MsgType type;
    uint16_t id;
    std::string payload; /* raw bytes */
};

/**
 * Encode a message header + payload into `out` (6-byte little-endian header
 * followed by the payload). Returns false and leaves `out` untouched when
 * `len > MAX_PAYLOAD`.
 */
bool encode_message(MsgType t, uint16_t id, const uint8_t *payload, size_t len, std::string &out);

/**
 * Decode a message header + payload. Checks, in order: `len < 6` -> Short,
 * `ver != 1` -> Version, `type > 5` -> Type, payload length `> 4096` ->
 * Oversize, `6 + len != buffer length` -> Length. On success `out` is filled
 * and DecodeError::None returned.
 */
DecodeError decode_message(const uint8_t *buf, size_t len, Message &out);

/** Splits messages into link-layer packets, tracking the rolling seq counter. */
class Segmenter {
public:
    /** Reset the sequence counter to 0. */
    void reset();
    /** Current sequence number (next packet to be sent). */
    uint8_t seq() const;
    /** Set the sequence counter; masked to 6 bits. */
    void set_seq(uint8_t s);

    /** Called per packet; returning false aborts the send. */
    using Sink = std::function<bool(const uint8_t *pkt, size_t len)>;

    /**
     * Split `msg` into packets of one flags byte + up to `packet_size - 1`
     * message bytes, invoking `sink` for each in order. START marks the first
     * packet, END the last; seq advances by one (mod 64) per packet accepted
     * by `sink`. Returns false if `packet_size < 2` or `len == 0`, or if
     * `sink` returns false (seq has then advanced only for the packets the
     * sink accepted).
     */
    bool send(const uint8_t *msg, size_t len, size_t packet_size, const Sink &sink);

private:
    uint8_t seq_ = 0;
};

/** Why the reassembler dropped a partial message. */
enum class ResetReason : uint8_t { Empty, SeqGap, UnexpectedStart, UnexpectedContinue, Oversize };

/** Human-readable name for a ResetReason ("empty", "seq_gap", ...). */
const char *reset_reason_name(ResetReason r);

/**
 * Reassembles packets back into whole messages, mirroring the reference
 * implementation packet-for-packet (including the discard state entered after
 * an out-of-sequence or oversize fault).
 */
class Reassembler {
public:
    /** Called with a complete message; the buffer is valid only for the call. */
    using MessageFn = std::function<void(const uint8_t *msg, size_t len)>;
    /** Called when a partial message is dropped. */
    using ResetFn = std::function<void(ResetReason)>;

    Reassembler(MessageFn on_message, ResetFn on_reset);

    /** Reset expected_seq to 0, drop any partial message, leave discard state. */
    void reset();

    /** Feed one packet (flags byte + message bytes). */
    void feed(const uint8_t *pkt, size_t len);

private:
    MessageFn on_message_;
    ResetFn on_reset_;
    std::string partial_;
    bool open_ = false;
    bool discarding_ = false;
    uint8_t expected_ = 0;
};

/**
 * Encode a DATA payload: u32 offset + raw bytes. Returns false when
 * `n > MAX_PAYLOAD - 4`; otherwise `out` is filled.
 */
bool encode_data(uint32_t offset, const uint8_t *bytes, size_t n, std::string &out);

/**
 * Decode a DATA payload. Returns false when `len < 4`; otherwise `bytes`/`n`
 * point into `payload` at the bytes following the offset.
 */
bool decode_data(const uint8_t *payload, size_t len, uint32_t &offset, const uint8_t *&bytes,
                 size_t &n);

/** Encode an ACK payload: u32 next_expected_offset + u16 credits. */
void encode_ack(uint32_t next, uint16_t credits, std::string &out);

/** Decode an ACK payload. Returns false unless `len == 6`. */
bool decode_ack(const uint8_t *payload, size_t len, uint32_t &next, uint16_t &credits);

}  // namespace pagerlink
