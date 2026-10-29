/**
 * @file      companion.h
 * @brief     The phone-messaging inbox (PROTOCOL.md §6.5): the last
 *            hal::msg::kInboxCap messages and clips the phone sent.
 *
 * Kept in RAM and persisted as /inbox.jsonl on preferred storage (internal
 * FFat) after every change, so a reboot keeps it. Thread-safe: the link task
 * adds, the Inbox app reads and marks read on the LVGL thread. No LVGL here —
 * the banners and typing live in ui_companion.cpp.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "companion_msg.h"

namespace hal {
namespace companion {

/** Storage path of the ring. */
constexpr const char *kInboxPath = "/inbox.jsonl";

/** Append `e` (loads the file on first use) and persist. False on a duplicate
 * id, with nothing changed. */
bool inbox_add(const msg::InboxEntry &e);

/** A copy of the ring, oldest first. */
std::vector<msg::InboxEntry> inbox_snapshot();

/** Messages not yet read (the home tile badge). */
int inbox_unread();

/** Mark everything read and persist (opening the Inbox app). */
void inbox_mark_all_read();

/** Remove one entry by its position in inbox_snapshot() order; persist. */
void inbox_remove(size_t index);

/** Empty the ring and persist. */
void inbox_clear();

/** Bumps on every change, so an open Inbox app knows to re-render. */
uint32_t inbox_generation();

}  // namespace companion
}  // namespace hal
