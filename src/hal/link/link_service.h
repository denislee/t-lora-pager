/**
 * @file      link_service.h
 * @brief     Transport-agnostic PagerLink service core, shared by every transport.
 *
 * One global instance owns the framing (Segmenter/Reassembler) and the RPC
 * layer and exposes a tiny connect / packet / tick / disconnect surface that a
 * transport (TCP on the emulator, BLE GATT on hardware) drives. Every entry
 * point locks one std::recursive_mutex, so the Reassembler callbacks — which
 * run while that lock is held — may safely re-enter (e.g. a handler calling
 * service_packet_size()). Handlers must not call the connect/packet/tick
 * lifecycle functions.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "hal/link.h"
#include "hal/link/link_frame.h"
#include "hal/link/link_rpc.h"

namespace pagerlink {

/** Transport hooks the service drives; supplied by the transport on connect. */
struct TransportOps {
    /** Write one link-layer packet; false aborts the send. */
    std::function<bool(const uint8_t *pkt, size_t len)> send_packet;
    /** Negotiated packet size P (PROTOCOL.md §2). */
    size_t packet_size = 0;
    /** Packets the transport can buffer ahead of the service; 0 = unbounded
     * (it back-pressures, e.g. TCP). Bulk receivers size their credit grant to
     * it (rx_credits_for, P7.30). */
    size_t rx_queue_packets = 0;
    /** Optional: true when the transport can take `packets` more packets right
     * now without dropping. Empty = always (e.g. TCP blocks instead). The service
     * checks it before segmenting a message so a message is queued whole or not
     * at all (P7.32). */
    std::function<bool(size_t packets)> tx_room;
    /** Optional: called when a bulk transfer starts and on every DATA/ACK in
     * either direction, with the service lock held — so it must only flag (BLE
     * holds its transfer connection parameters on it, P7.33). Empty = ignored. */
    std::function<void()> bulk_activity;
    /** Optional: a request or transfer deadline was registered from outside the
     * transport task (service_call, service_send_bytes, service_accept_expect),
     * so a transport that sleeps until service_next_tick_ms() must re-evaluate
     * its wait. Runs with the service lock held: it must only signal (P7.35).
     * Empty = ignored (TCP ticks on its own cadence). */
    std::function<void()> wake;
    /** Human-readable peer name for the log, e.g. "tcp:127.0.0.1:54321". */
    std::string peer_name;
};

/** A transport connected: reset framing, (re)build the RPC and announce. */
void service_on_connect(const TransportOps &ops);
/** One link-layer packet arrived from the transport. No-op when disconnected. */
void service_on_packet(const uint8_t *pkt, size_t len);
/** The transport dropped: fail outstanding requests and tear the RPC down. */
void service_on_disconnect();
/** Periodic pump: expire requests and stalled transfers; publish battery and fs
 * changes. The battery is read before the service lock is taken (P7.35). */
void service_tick();
/**
 * Milliseconds until service_tick() next has work, capped at `cap`: the nearest
 * RPC deadline, read retransmit/stall, outbound retransmit/stall or inbound
 * stall; 0 when one is due or the filesystem generation moved since the last
 * evt.fs.changed. `cap` when disconnected. Transport task only (P7.35).
 */
uint32_t service_next_tick_ms(uint32_t cap);

/** True while a transport is connected. */
bool service_connected();
/** Negotiated packet size P (0 when no transport is connected). */
size_t service_packet_size();
/** The transport renegotiated its packet size (BLE MTU exchange after the
 * subscribe, P7.33): later messages segment at `packet_size`. Ignored when
 * disconnected or `packet_size` < 2. Not for handlers: the transport task only. */
void service_set_packet_size(size_t packet_size);
/** A bulk transfer is starting (TransportOps::bulk_activity). Takes the
 * service lock; handlers may call it. */
void service_bulk_activity();
/** Credit grant and ack_every for a new bulk receiver, sized to the connected
 * transport's receive queue (rx_credits_for / rx_ack_every_for). Takes the
 * service lock. */
void service_rx_window(uint16_t &credits, unsigned &ack_every);

/** Register the sys.* handlers on `rpc` (implemented in handlers_sys.cpp). */
void register_sys_handlers(Rpc &rpc);

/*
 * File service (handlers_fs.cpp, PROTOCOL.md §4 and the fs.* rows of §6).
 * Every call runs with the service lock held, like the handlers themselves.
 */
/** Register the fs.* handlers on `rpc` and adopt it for bulk sends. */
void register_fs_handlers(Rpc &rpc);
/** Register the weather.* handlers on `rpc` (handlers_weather.cpp). */
void register_weather_handlers(Rpc &rpc);
/** Route one DATA/ACK message (Rpc::on_bulk) to its transfer; returns true when
 * the xfer id belongs to an fs transfer, false when it is unknown. */
bool fs_on_bulk(MsgType type, uint16_t xfer, const uint8_t *payload, size_t len);
/** Periodic pump: drop transfers idle for longer than the stall timeout. */
void fs_tick(uint32_t now_ms);
/** Milliseconds until fs_tick() next has work (a read retransmit or a stall
 * drop), capped at `cap`; 0 when some is due (P7.35). */
uint32_t fs_next_wait_ms(uint32_t cap);
/** The link dropped: abort every transfer and delete upload `.part` files. */
void fs_reset();

/* ------------------------------------------------------------------------- */
/* Pager-initiated RPC and bulk sends (link_call.cpp, handlers_test.cpp).    */
/* The public UI surface is hal/link.h; these are the service-core entry      */
/* points the transport-agnostic hw_link_* wrappers call. Every entry point   */
/* takes the service lock, like the handlers.                                 */
/* ------------------------------------------------------------------------- */

/**
 * Send a REQ to the phone. Returns false — and never runs `cb` — when there is
 * no phone or the Rpc refuses (MAX_PENDING). Otherwise `cb` runs later on the
 * link task with the service lock held.
 */
bool service_call(const std::string &method, const std::string &params_json, uint32_t timeout_ms,
                  Rpc::ResponseFn cb);

/**
 * Stream `len` bytes to the phone as DATA under the phone's credits for
 * transfer `xfer` (the even id it allocated in the announcing RPC). `done`
 * runs exactly once on the link task: true when every byte is ACKed, false on
 * send failure, on a stall (no ACK progress for `stall_ms`, checked in
 * service_tick) or on disconnect. Returns false — and never runs `done` —
 * when there is no phone, or `xfer` is 0, odd or already an outbound transfer.
 * `data` must stay valid until `done` runs.
 */
bool service_send_bytes(uint16_t xfer, const uint8_t *data, size_t len, uint32_t stall_ms,
                        std::function<void(bool ok)> done);

/**
 * Inbound (phone -> pager) bulk into memory, backing hw_link_accept_*.
 * service_accept_open() allocates an odd id (skipping fs.write ids) and buffers
 * DATA until service_accept_expect() names the size and CRC; `done` then runs
 * exactly once on the link task (or inline): true with the bytes when all
 * `size` arrived and the CRC matches; false on size > max, CRC mismatch, a
 * stall (no DATA for `stall_ms`, checked in service_tick), disconnect or
 * close. service_accept_expect() returns false — and never runs `done` — for
 * an unknown id or a second expect. service_accept_close() never runs `done`.
 */
uint16_t service_accept_open(uint32_t max_size);
bool service_accept_expect(uint16_t xfer, uint32_t size, uint32_t crc32, uint32_t stall_ms,
                           std::function<void(bool ok, std::string &bytes)> done);
void service_accept_close(uint16_t xfer);
/** True when `xfer` is an open inbound receiver (handlers_fs id allocation). */
bool service_accept_owns(uint16_t xfer);
/** True when `xfer` is a live fs transfer (handlers_fs.cpp). */
bool fs_owns_xfer(uint16_t xfer);

/** Send an EVT from any task. Returns false when there is no phone. */
bool service_emit(const std::string &event, const std::string &params_json);

/**
 * Register `fn` for incoming EVT `event`; it runs on the link task with the
 * service lock held and receives the event's params as JSON object text.
 * Returns a token (> 0). Subscriptions survive reconnects.
 */
int service_subscribe(const std::string &event, std::function<void(const std::string &params_json)> fn);

/** Remove a subscription; unknown tokens are ignored. Safe from any task. */
void service_unsubscribe(int token);

/** Register the test.callback handler (handlers_test.cpp; emulator builds only). */
void register_test_handlers(Rpc &rpc);

}  // namespace pagerlink
