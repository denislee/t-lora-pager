/**
 * @file      telegram_test.h
 * @brief     Emulator-only Telegram test hook for `pagerlink smoke`.
 *
 * handlers_test.cpp (test.callback {tg}) drives these so the smoke can run
 * the Telegram worker code end to end against the mock phone's stub bridge.
 * Never compiled for hardware.
 */
#pragma once

#ifndef ARDUINO

#include <string>

namespace apps {

// Outcome of one tg_test_step().
struct TgTestResult {
    bool ok = false;
    std::string status;      // the status-pill text the app would show
    std::string uplink;      // "direct" | "phone" | "none" (as the step saw it)
    int count = 0;           // chats / messages parsed
    int unread_total = 0;    // chats, notify: sum of positive unread counts
    int newest_id = 0;       // msgs: newest message id parsed
    int marked_to = 0;       // msgs: mark-read high-water sent (0 = none)
    long long text_bytes = 0;// msgs: sum of parsed message text sizes
    int notified = 0;        // notify: notifier bursts this tick fired
};

// Run one Telegram step with explicit bridge creds (blocking; a worker thread):
//   "chats"  - the foreground chat-list fetch
//   "msgs"   - open chat `chat_id`: fetch its messages + mark-read
//   "send"   - send `text` to `chat_id`
//   "notify" - one background unread-notifier tick; `reset` forgets the
//              baseline first, `link_down` makes the tick see no phone link.
TgTestResult tg_test_step(const std::string &op, const std::string &bridge,
                          const std::string &token, long long chat_id,
                          const std::string &text, bool reset, bool link_down);

}  // namespace apps

#endif  // !ARDUINO
