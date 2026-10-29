/**
 * @file      companion_msg.h
 * @brief     PagerLink messaging rules (PROTOCOL.md §6.5) and the inbox ring.
 *
 * Pure C++ (C++11 on the device, gnu++17 in native_test): validates the params
 * of notify.post / clip.push / text.send, decides how a message is delivered
 * (banner, haptic, wake), and keeps the small inbox ring that
 * hal/companion.cpp persists as /inbox.jsonl. Every rule here is pinned by
 * protocol/vectors/messaging.json (test/test_companion_msg); the phone and the
 * Go tool validate the same params client-side.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hal/link/link_json.h"

namespace hal {
namespace msg {

/** notify.post `prio`. */
enum class Prio : uint8_t { Low, Normal, High };

/** Wire name of a priority ("low" / "normal" / "high"). */
const char *prio_name(Prio p);

/** A validated notify.post. Absent optional fields are empty / 0. */
struct NotifyPost {
    std::string title;
    std::string body;
    std::string id;
    std::string src;
    uint32_t ts = 0;
    Prio prio = Prio::Normal;
};

/** text.send `target`. */
enum class Target : uint8_t { Editor, Kb };

/** Field limits (bytes), PROTOCOL.md §6.5. */
constexpr size_t kTitleMax = 64;
constexpr size_t kBodyMax = 512;
constexpr size_t kIdMax = 32;
constexpr size_t kSrcMax = 32;
constexpr size_t kClipMax = 4000;
constexpr size_t kTextSendMax = 2000;
/** Messages the inbox ring keeps (oldest dropped first). */
constexpr size_t kInboxCap = 32;

/**
 * Validate notify.post params. On failure returns false with `why` naming the
 * field ("title: too long") and leaves `out` unspecified. Fields are checked in
 * a fixed order (title, body, id, src, ts, prio); the first failure wins.
 */
bool parse_notify_post(const pagerlink::JsonObject &params, NotifyPost &out, std::string &why);

/** Validate clip.push params (either direction): `text`, 1..kClipMax bytes. */
bool parse_clip_push(const pagerlink::JsonObject &params, std::string &text, std::string &why);

/** Validate text.send params: `text` (1..kTextSendMax bytes) then `target`. */
bool parse_text_send(const pagerlink::JsonObject &params, std::string &text, Target &target,
                     std::string &why);

/** How one notify.post reaches the user. */
struct Delivery {
    bool banner = false;  // show a notify:: banner
    bool haptic = false;  // buzz with it (still gated by the global haptic toggle)
    bool wake = false;    // wake the display from fake sleep first
};

/**
 * The delivery policy of §6.5: low → inbox only; normal → banner + haptic
 * while awake, silent during fake sleep; high → banner + haptic, and wake the
 * display when the board can (`can_wake`: the T-LoRa-Pager).
 */
Delivery delivery_for(Prio p, bool fake_sleep, bool can_wake);

/** One inbox message as stored in /inbox.jsonl. */
struct InboxEntry {
    std::string id;     // notify.post id ("" = none; never deduped)
    std::string kind;   // "msg" (notify.post) or "clip" (clip.push with no editor open)
    std::string title;
    std::string body;   // message body, or the clipboard text for kind "clip"
    std::string src;
    uint32_t ts = 0;    // sender's timestamp (0 = unknown)
    uint32_t rx = 0;    // pager clock when it arrived (0 = clock not set)
    Prio prio = Prio::Normal;
    bool read = false;
};

/**
 * Append `e` (newest last). Returns false and changes nothing when `e.id` is
 * non-empty and an entry with that id is already in the ring. Drops the oldest
 * entries beyond `cap`.
 */
bool inbox_add(std::vector<InboxEntry> &ring, const InboxEntry &e, size_t cap = kInboxCap);

/** Entries not yet marked read. */
size_t inbox_unread(const std::vector<InboxEntry> &ring);

/** JSON Lines, oldest first, one compact object per line (each ends in '\n').
 * Empty strings, ts/rx 0 and read=false are omitted. */
std::string inbox_encode(const std::vector<InboxEntry> &ring);

/** Parse inbox_encode() output. Lines that are not a valid entry (no title, bad
 * JSON — e.g. a torn last write) are skipped; at most `cap` newest are kept. */
void inbox_decode(const std::string &text, std::vector<InboxEntry> &out, size_t cap = kInboxCap);

/** Length of the longest prefix of `s` that is at most `max` bytes and does not
 * split a UTF-8 sequence (used to cut a note to the clip.push limit). */
size_t utf8_prefix_len(const std::string &s, size_t max);

}  // namespace msg
}  // namespace hal
