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
    /** Human-readable peer name, e.g. "tcp:127.0.0.1:54321". */
    std::string peer_name;
};

/** A transport connected: reset framing, (re)build the RPC and announce. */
void service_on_connect(const TransportOps &ops);
/** One link-layer packet arrived from the transport. No-op when disconnected. */
void service_on_packet(const uint8_t *pkt, size_t len);
/** The transport dropped: fail outstanding requests and tear the RPC down. */
void service_on_disconnect();
/** Periodic pump: expire requests and stalled transfers; publish battery and fs changes. */
void service_tick();

/** True while a transport is connected. */
bool service_connected();
/** Connected peer name, or "" when disconnected. */
std::string service_peer_name();
/** Copy the running counters (see HwLinkStats) into `out`. */
void service_get_stats(HwLinkStats &out);
/** Negotiated packet size P (0 when no transport is connected). */
size_t service_packet_size();

/** Register the sys.* handlers on `rpc` (implemented in handlers_sys.cpp). */
void register_sys_handlers(Rpc &rpc);

/*
 * File service (handlers_fs.cpp, PROTOCOL.md §4 and the fs.* rows of §6).
 * Every call runs with the service lock held, like the handlers themselves.
 */
/** Register the fs.* handlers on `rpc` and adopt it for bulk sends. */
void register_fs_handlers(Rpc &rpc);
/** Route one DATA/ACK message (Rpc::on_bulk) to its transfer. */
void fs_on_bulk(MsgType type, uint16_t xfer, const uint8_t *payload, size_t len);
/** Periodic pump: drop transfers idle for longer than the stall timeout. */
void fs_tick(uint32_t now_ms);
/** The link dropped: abort every transfer and delete upload `.part` files. */
void fs_reset();

}  // namespace pagerlink
