/**
 * @file      link_xfer.h
 * @brief     PagerLink v1 bulk-transfer engine (PROTOCOL.md §4) + CRC-32.
 *
 * Pure C++17, no Arduino / ESP-IDF headers, so it is unit-tested on the host.
 * One XferSender or XferReceiver per transfer; the owner (handlers_fs.cpp on
 * the pager) feeds it the DATA / ACK payloads routed by Rpc::on_bulk() and
 * supplies the storage and send callbacks. Not thread-safe: like Rpc, every
 * call runs on the link service task.
 *
 * Flow control:
 *  - Credits count DATA messages. The sender keeps at most `credits` DATA
 *    messages past the last acknowledged offset; the implicit initial grant
 *    is INITIAL_CREDITS (link_frame.h).
 *  - ACK.next acknowledges every byte below it and its credits replace the
 *    sender's window, counting from that offset.
 *  - The receiver only ACKs after DATA arrives: every `ack_every` messages,
 *    on completion, at most two ACKs per gap (one that tells the sender the
 *    offset if it has not seen it, then the duplicate that triggers its one
 *    rewind) when a DATA offset is not the one it expects, and again after
 *    Done so a sender whose completion ACK was lost can finish.
 *  - The sender rewinds (go-back-N) at most ONCE per acknowledged offset: the
 *    first ACK with no progress while bytes are in flight moves the send point
 *    back to the acknowledged offset, later duplicates only update credits.
 *  - The owner drives a retransmit timeout (XFER_RETRANSMIT_MS, link_frame.h):
 *    when no ACK arrives for that long while bytes are unacknowledged it calls
 *    retransmit() and then pump() to resend from the acknowledged offset.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "hal/link/link_buf.h"

namespace pagerlink {

/** Largest DATA chunk: MAX_PAYLOAD minus the u32 offset. */
constexpr size_t MAX_DATA_CHUNK = 4092;

/**
 * CRC-32/ISO-HDLC (zlib.crc32, Go crc32.ChecksumIEEE), incremental:
 * crc32_update(crc32_update(0, a), b) == CRC of a followed by b.
 * Start a new checksum with crc = 0.
 */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t n);

enum class XferState : uint8_t { Active, Done, Failed };

/** Sends `size` bytes as DATA messages under the receiver's credit window. */
class XferSender {
public:
    /** Fill `buf` with exactly `n` bytes of the source at `offset`; false fails the transfer. */
    using ReadFn = std::function<bool(uint32_t offset, uint8_t *buf, size_t n)>;
    /** Send one encoded DATA payload (u32 offset + bytes); false fails the transfer.
     * The bytes are valid only for the call (a reused buffer, P7.36). */
    using SendFn = std::function<bool(const uint8_t *data_payload, size_t len)>;

    /**
     * `chunk` is the DATA size (clamped to 1..MAX_DATA_CHUNK); every message
     * but the last carries exactly `chunk` bytes. `start` (≤ size) is the
     * first offset to send — a resumed read. A zero-length remainder is Done
     * immediately.
     */
    XferSender(uint32_t size, size_t chunk, ReadFn read, SendFn send, uint32_t start = 0);

    /** Send DATA while the window allows. Returns the state afterwards. */
    XferState pump();
    /**
     * Feed one ACK payload. A malformed payload, an ACK below the current
     * acknowledged offset, or one beyond the highest offset ever sent is
     * ignored. The first no-progress ACK while bytes are in flight rewinds the
     * send point to the acknowledged offset; later duplicates only replace
     * credits. Does not send; call pump() next.
     */
    XferState on_ack(const uint8_t *payload, size_t len);
    /**
     * Retransmit timeout: the owner calls it when no ACK has arrived for the
     * retransmit timeout while bytes are unacknowledged, then calls pump().
     * Rewinds the send point to the acknowledged offset. Returns whether it
     * rewound (false when not Active or nothing is unacknowledged).
     */
    bool retransmit();

    XferState state() const { return state_; }
    uint32_t acked() const { return acked_; }
    /** next offset to send (it moves back on a rewind) */
    uint32_t sent() const { return next_; }
    uint32_t size() const { return size_; }

private:
    uint32_t size_;
    size_t chunk_;
    ReadFn read_;
    SendFn send_;
    uint32_t acked_;
    uint32_t next_;
    uint32_t high_; /* highest offset ever handed to send */
    bool rewound_;  /* the current acked_ already used its one rewind */
    uint16_t credits_;
    XferState state_;
    /* The DATA payload being sent: ReadFn fills it in place (P7.36). One
     * MAX_PAYLOAD block, PSRAM-first, allocated at the first pump. */
    ByteBuf payload_;
};

/** Receives `size` bytes of DATA in order and acknowledges them. */
class XferReceiver {
public:
    /** Persist `n` bytes at `offset` (always the next expected offset); false fails the transfer. */
    using WriteFn = std::function<bool(uint32_t offset, const uint8_t *bytes, size_t n)>;
    /** Send one encoded ACK payload (u32 next + u16 credits); false fails the transfer. */
    using AckFn = std::function<bool(const std::string &ack_payload)>;

    /**
     * `window` is the credit grant sent in every ACK, `ack_every` (≥ 1) how
     * many in-order DATA messages arrive between ACKs. `start` is the first
     * expected offset. A zero-length transfer is Done immediately.
     */
    XferReceiver(uint32_t size, WriteFn write, AckFn ack, uint16_t window = 16,
                 unsigned ack_every = 4, uint32_t start = 0);

    /**
     * Feed one DATA payload. Malformed → ignored. Wrong offset → at most two
     * ACKs per gap: one that tells the sender the offset if it has not seen
     * it, then the duplicate that triggers its one rewind; bytes dropped.
     * Bytes past `size` or a failed write / ACK → Failed. After Done a
     * well-formed retransmission is re-ACKed (the sender may not have seen the
     * completion ACK) and the state stays Done. Returns the state afterwards.
     */
    XferState on_data(const uint8_t *payload, size_t len);

    XferState state() const { return state_; }
    uint32_t received() const { return next_; }
    uint32_t size() const { return size_; }
    /**
     * CRC-32/ISO-HDLC of the bytes accepted so far (from `start`), in order:
     * exactly the bytes handed to WriteFn. With start 0 and state Done it is
     * the whole transfer's CRC, so the owner need not re-read what it wrote.
     */
    uint32_t crc() const { return crc_; }

private:
    bool send_ack();

    uint32_t size_;
    WriteFn write_;
    AckFn ack_;
    uint16_t window_;
    unsigned ack_every_;
    unsigned since_ack_;
    uint32_t next_;
    uint32_t last_ack_; /* offset of the last ACK sent (the sender's acked) */
    bool gap_acked_;    /* a re-ACK has gone out for the current gap */
    uint32_t crc_;      /* running CRC of the accepted bytes */
    XferState state_;
};

}  // namespace pagerlink
