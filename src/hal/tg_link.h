/**
 * @file      tg_link.h
 * @brief     Telegram bridge calls over the PagerLink companion (tg.*).
 *
 * When the pager has no internet but a phone is connected, ui_telegram runs
 * its four tg-bridge calls through the phone (HUB_REMOVAL_PLAN.md §2.2,
 * H2.6). The phone builds the fixed /v1/... path from typed params and calls
 * the bridge with the URL + bearer the pager sends per call (D-H7); it never
 * stores the token. This is the only header ui_telegram includes for it.
 *
 * Blocking: tg_link_request() drives hw_link_call() / hw_link_accept_wait(),
 * which wait on the phone — see the threading note in link.h.
 */
#pragma once

#include <cstdint>
#include <string>

// Per-call timeout for every tg.* request (and the bulk stall window).
constexpr uint32_t TG_LINK_TIMEOUT_MS = 15000;
// Largest bulk reply the pager accepts for tg.chats / tg.messages.
constexpr uint32_t TG_LINK_MAX_BODY = 64 * 1024;

// Run one tg.* request on the phone.
//   method       "tg.chats" | "tg.messages" | "tg.read" | "tg.send"
//   params_json  the typed params object text, e.g.
//                {"bridge":"https://b","token":"t","chat_id":7,"limit":20}
//                (no "xfer"; the caller scrubs its own copy afterwards).
// For tg.chats / tg.messages a receiver is opened (hw_link_accept_open) and
// its odd id is added to the params as "xfer"; it is closed on every exit.
// The phone answers {"body":<bridge JSON>} inline, or {"xfer","size","crc32"}
// and streams the body as DATA.
//
// true  -> body_out = the bridge's JSON body text ("{}" when the reply has no
//          body, as for tg.read / tg.send), status = 200, err cleared.
// false -> err is UI-ready and status says why:
//          link down / dropped ("disconnected")      -> "Phone disconnected", 0
//          ERR unauthorized                          -> "HTTP 401", 401
//          ERR http_error, msg = decimal HTTP status -> "HTTP <n>", n
//          ERR timeout                               -> "Phone timeout", 0
//          a bad reply / failed bulk transfer        -> "phone: bad reply" or
//                                                       "phone: transfer failed", 0
//          any other ERR                             -> "phone: <code>: <msg>", 0
// The internal params copy (it carries the token) is scrubbed with
// hal::secret_scrub before returning.
bool tg_link_request(const char *method, const std::string &params_json, std::string &body_out,
                     int &status, std::string &err);
