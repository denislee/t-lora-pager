/**
 * @file      ui_chat.cpp
 * @brief     LLM chat client: the phone first (PagerLink chat.ask), the
 *            lilyhub /api/chat as a hardware-only fallback.
 *
 * The app gives the user two ways to ask a question — type it, or tap the
 * mic — and renders the model's reply as a wrapped text line in a
 * scrolling log. The on-device firmware does not talk to Groq directly.
 *
 * Route, picked per request in kick_off_send (HUB_REMOVAL_PLAN.md H2.4):
 *   1. Phone connected over PagerLink → chat_link_ask() (hal/chat_link.h):
 *      voice goes up as chat.audio.begin + DATA, then chat.ask. The phone
 *      holds the Groq key and the per-pager history. Works in the emulator.
 *   2. Hardware only, no phone → the LAN hub (POST <hub>/api/chat), unchanged
 *      until the hub is deleted (H4.2, gated on BLE hardware: D-H1).
 *   3. Neither → a clean "Connect your phone" error.
 * Voice memos are capped at CHAT_AUDIO_MAX_MS (15 s): the poll timer stops
 * the recording at the cap and sends it.
 *
 * State reset on onStop (exit_cb):
 *   widgets: s_log_scroll, s_log_label, s_input_ta, s_mic_btn,
 *            s_status_lbl                  — nulled (deleted with parent)
 *   timers:  s_poll_timer (also drives the
 *            15 s recording cap)           — lv_timer_del + null
 *   tasks:   send_task (FreeRTOS task on hardware, detached std::thread on
 *            the emulator; no handle kept) — abandoned via s_ctx handoff
 *   hw:      mic recording                 — stopped via hw_rec_stop()
 *   ctx:     s_ctx (SendCtx*)              — abandoned + freed if worker done
 * If you add new cached state, list it here AND extend exit_cb() — a
 * leaked timer pointer will fire into a destroyed LVGL tree on next entry.
 */
#include "../ui_define.h"
#include "../hal/chat_link.h"
#include "../hal/hub.h"
#include "../hal/link.h"
#include "../hal/wireless.h"
#include "../hal/storage.h"
#include "../hal/audio.h"
#include "../hal/system.h"
#include "../hal/str_encode.h"
#include "../core/app.h"
#include "../core/app_manager.h"
#include "../core/scoped_lock.h"
#include "app_registry.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef ARDUINO
#include <Arduino.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <LilyGoLib.h>
#include <cctype>
#include <cstdlib>
extern "C" {
#include "cJSON.h"
}
#else
#include <thread>
#endif

namespace {

#ifdef ARDUINO
static const char *CHAT_DIR        = "/chat";
static const char *DEFAULT_DEVICE  = "pager-01";
#endif

// LVGL widgets — single-instance, lifetime tied to enter()/exit_cb().
static lv_obj_t *s_log_scroll  = nullptr;
static lv_obj_t *s_log_label   = nullptr;
static lv_obj_t *s_input_ta    = nullptr;
static lv_obj_t *s_mic_btn     = nullptr;
static lv_obj_t *s_status_lbl  = nullptr;
static lv_timer_t *s_poll_timer = nullptr;

static std::string s_log_text;
static std::string s_pending_audio_path;
static bool        s_recording = false;
static bool        s_busy      = false;

// --- async send context ---------------------------------------------------
//
// The send task runs off the LVGL thread because chat_link_ask / chat_post
// block.
// We coordinate with atomics rather than the global instance lock — the
// LVGL task already holds that lock around lv_timer_handler(), and the
// instance mutex is non-recursive, so a poll-timer callback that tried
// to retake it would deadlock the entire UI.
//
// `done` flips when the worker has filled in result fields. After that,
// non-atomic fields (reply/transcript/error/ok) are stable and safe to
// read from the UI thread (release/acquire pairing on `done`).
//
// `abandoned` is set by exit_cb when the user navigates away while a
// request is in flight. Whichever party (worker on done, exit_cb on
// abandon-after-done) wins the `claimed` CAS owns deletion; the other
// leaves the buffer alone.
struct SendCtx {
    bool        phone = false;  // true: chat_link_ask; false: hub chat_post
    std::string text;           // phone path: the typed question
    std::string url;
    std::string body;
    // Voice memo. Hub path: the worker streams this WAV in as base64
    // (appended to `body`, which the UI thread left open after
    // `..."audio_b64":"`). Phone path: chat_link_ask uploads it as DATA.
    std::string audio_path;
    std::string transcript;
    std::string reply;
    std::string error;
    uint32_t    start_ms = 0;
    bool        ok       = false;
    std::atomic<bool> done{false};
    std::atomic<bool> abandoned{false};
    std::atomic<bool> claimed{false};
};
static SendCtx *s_ctx = nullptr;

// Watchdog: if the worker task hasn't flipped done within this window, give
// up on it. It is a UI-side backstop just past the worker's own budget: it
// surfaces a visible error and abandons the task so the UI doesn't sit on
// "thinking..." forever if the worker overruns.
//  - hub: chat_post's raw WiFiClient bounds itself (5 s connect + 15 s read).
//  - phone: chat.ask waits CHAT_ASK_TIMEOUT_MS (60 s) after the voice upload,
//    so the budget covers that plus the upload of a capped memo.
#ifdef ARDUINO
static const uint32_t SEND_WATCHDOG_MS = 20000;
#endif
static const uint32_t PHONE_WATCHDOG_MS = 75000;

static uint32_t chat_now_ms()
{
#ifdef ARDUINO
    return millis();
#else
    return lv_tick_get();
#endif
}

static void log_append(const char *prefix, const std::string &text)
{
    if (!s_log_label) return;
    if (!s_log_text.empty()) s_log_text.push_back('\n');
    if (prefix) s_log_text.append(prefix);
    s_log_text.append(text);

    // Bound the transcript so a long chat can't grow this string (and the label
    // it backs) without limit. Drop whole lines from the front once over cap.
    constexpr size_t kLogMax = 8000;
    if (s_log_text.size() > kLogMax) {
        size_t cut = s_log_text.size() - kLogMax;
        size_t nl = s_log_text.find('\n', cut);
        s_log_text.erase(0, nl == std::string::npos ? cut : nl + 1);
    }

    lv_label_set_text(s_log_label, s_log_text.c_str());
    if (s_log_scroll) {
        lv_obj_update_layout(s_log_scroll);
        lv_obj_scroll_to_y(s_log_scroll, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

static void set_status(const char *txt)
{
    if (!s_status_lbl) return;
    if (txt && *txt) {
        lv_label_set_text(s_status_lbl, txt);
        lv_obj_remove_flag(s_status_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_status_lbl, "");
        lv_obj_add_flag(s_status_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_busy(bool busy)
{
    s_busy = busy;

    // Crucial ordering: we must un-hide the status label BEFORE asking
    // the group to focus it. lv_group_focus_obj() is a no-op on hidden
    // objects, so if we left the label hidden the focus would stay on
    // the textarea — which we're about to disable — and both keyboard
    // and encoder would have no live target to deliver events to.
    if (busy && s_status_lbl) {
        lv_label_set_text(s_status_lbl, "thinking... (tap to cancel)");
        lv_obj_remove_flag(s_status_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    auto apply = [&](lv_obj_t *o) {
        if (!o) return;
        if (busy) lv_obj_add_state(o, LV_STATE_DISABLED);
        else      lv_obj_remove_state(o, LV_STATE_DISABLED);
    };
    apply(s_mic_btn);
    apply(s_input_ta);

    if (s_status_lbl && s_input_ta) {
        if (busy) {
            lv_group_focus_obj(s_status_lbl);
        } else {
            lv_group_focus_obj(s_input_ta);
        }
    }
}

// Detach an in-flight send. The HTTP call itself is blocking and lives in
// the worker task — we can't truly abort it, but we can stop caring about
// the result. Whoever wins the `claimed` CAS frees the buffer.
static void cancel_inflight()
{
    if (!s_ctx) return;
    SendCtx *c = s_ctx;
    s_ctx = nullptr;
    c->abandoned.store(true);
    // If the worker already finished, take responsibility for delete here.
    // Otherwise the worker will see abandoned=true on its way out and
    // delete via the same CAS.
    if (c->done.load() && !c->claimed.exchange(true)) {
        delete c;
    }
}

static void status_click_cb(lv_event_t *)
{
    if (!s_busy) return;
#ifdef ARDUINO
    // Filter the synthesized click from the same Enter press that just
    // submitted the message. The keypad indev sees LV_KEY_ENTER on
    // press → textarea fires LV_EVENT_READY → we move focus to the
    // status label → on release, the indev fires LV_EVENT_CLICKED on
    // the now-focused status label, which would instantly cancel the
    // request the user just sent. A short post-start grace window
    // absorbs the spurious release-click without delaying real cancels.
    if (s_ctx && s_ctx->start_ms != 0 &&
        (millis() - s_ctx->start_ms) < 400) {
        Serial.println("[chat] ignoring synthesized click within grace window");
        return;
    }
#endif
    cancel_inflight();
    set_busy(false);
    log_append("info: ", "cancelled");
    set_status("");
}

#ifdef ARDUINO
static std::string current_device_id()
{
    // A pager is single-user; one fixed ID is sufficient for the hub to
    // key history. If multiple pagers ever share one hub we'll derive
    // this from the WiFi MAC.
    return DEFAULT_DEVICE;
}

// Self-contained POST over raw WiFiClient. We avoid hw_http_request /
// Arduino HTTPClient because their getString() has been observed to
// hang indefinitely on this device, even when the server clearly sent
// a Content-Length + Connection: close response. With a raw client we
// have full control over timeouts and we log every step, which makes
// stalls visible on the serial monitor instead of looking like a UI bug.
//
// Returns true with status_code/resp filled on a successful HTTP exchange
// (2xx or otherwise — 4xx/5xx still come back true with the body), false
// only when the transport itself failed.
static bool chat_post(const std::string &url,
                      const std::string &body,
                      std::string &resp,
                      int &status_code,
                      std::string &error)
{
    status_code = 0;

    // We only support http://host[:port]/path. The hub is on the LAN; if
    // anyone ever points the device at https we'll route through a TLS
    // path then.
    if (url.compare(0, 7, "http://") != 0) {
        error = "only http:// supported (got: " + url.substr(0, 16) + ")";
        return false;
    }
    hal::UrlParts u;
    if (!hal::parse_url(url, u)) {
        error = "no host in hub URL";
        return false;
    }
    const std::string &host = u.host;
    const std::string &path = u.path;
    const int port = u.port;
    Serial.printf("[chat] connect %s:%d path=%s\n",
                  host.c_str(), port, path.c_str());

    WiFiClient client;
    if (!client.connect(host.c_str(), (uint16_t)port, 5000)) {
        error = "connect failed";
        return false;
    }
    Serial.println("[chat] connected");

    // Send headers + body in one go. Connection: close is critical so
    // the server signals EOF by closing — we then read until !connected.
    char header[320];
    int hlen = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: LilyGoLib-chat/1.0\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "Accept: application/json\r\n"
        "\r\n",
        path.c_str(), host.c_str(), port, (unsigned)body.size());
    client.write((const uint8_t *)header, (size_t)hlen);
    if (!body.empty()) {
        client.write((const uint8_t *)body.data(), body.size());
    }
    Serial.printf("[chat] sent headers=%d body=%u\n", hlen, (unsigned)body.size());

    // Drain the response. 15s overall budget; per-iteration we sleep
    // briefly while waiting so the FreeRTOS scheduler can run others.
    std::string raw;
    raw.reserve(1024);
    const uint32_t deadline = millis() + 15000;
    bool eof = false;
    while (millis() < deadline) {
        while (client.available()) {
            int c = client.read();
            if (c < 0) break;
            raw.push_back((char)c);
        }
        if (!client.connected() && !client.available()) {
            eof = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    client.stop();
    Serial.printf("[chat] read %u bytes, eof=%d, elapsed_ok=%d\n",
                  (unsigned)raw.size(), (int)eof,
                  (int)(millis() < deadline));

    if (raw.empty()) {
        error = eof ? "server closed without response" : "read timeout";
        return false;
    }

    // Parse status line: "HTTP/1.1 200 OK\r\n"
    size_t eol = raw.find("\r\n");
    if (eol == std::string::npos) {
        error = "no status line";
        return false;
    }
    {
        std::string sl = raw.substr(0, eol);
        size_t sp1 = sl.find(' ');
        if (sp1 == std::string::npos) { error = "bad status line"; return false; }
        size_t sp2 = sl.find(' ', sp1 + 1);
        std::string code_str = (sp2 == std::string::npos)
            ? sl.substr(sp1 + 1)
            : sl.substr(sp1 + 1, sp2 - sp1 - 1);
        status_code = atoi(code_str.c_str());
    }

    // Find header/body boundary.
    size_t hb = raw.find("\r\n\r\n");
    if (hb == std::string::npos) {
        error = "no header terminator";
        return false;
    }
    std::string headers = raw.substr(0, hb);
    std::string raw_body = raw.substr(hb + 4);

    // Cheap case-insensitive search for "transfer-encoding: chunked".
    bool chunked = false;
    {
        std::string lower = headers;
        for (auto &c : lower) c = (char)tolower((unsigned char)c);
        chunked = lower.find("transfer-encoding: chunked") != std::string::npos;
    }

    if (!chunked) {
        resp = std::move(raw_body);
    } else {
        // Decode chunked body. Each chunk: "<hex_len>\r\n<bytes>\r\n",
        // terminated by a "0\r\n" chunk.
        size_t pos = 0;
        while (pos < raw_body.size()) {
            size_t crlf = raw_body.find("\r\n", pos);
            if (crlf == std::string::npos) break;
            std::string lenstr = raw_body.substr(pos, crlf - pos);
            unsigned long n = strtoul(lenstr.c_str(), nullptr, 16);
            pos = crlf + 2;
            if (n == 0) break;
            if (pos + n > raw_body.size()) break;
            resp.append(raw_body, pos, (size_t)n);
            pos += n + 2;
        }
    }
    Serial.printf("[chat] parsed status=%d body=%u chunked=%d\n",
                  status_code, (unsigned)resp.size(), (int)chunked);
    return true;
}

// Streaming base64 sink for hw_read_sd_stream: appends the encoded chunk
// straight onto the request body. `len` is a multiple of 3 for every chunk
// except the last, so the base64 stitches together without mid-stream padding.
// Bails (returns false) if the user navigated away mid-encode.
static bool chat_b64_sink(void *user, const uint8_t *data, size_t len)
{
    SendCtx *ctx = (SendCtx *)user;
    if (ctx->abandoned.load()) return false;
    std::string enc;
    if (!hal::base64_encode(data, len, enc)) return false;
    ctx->body += enc;
    return true;
}
#endif

// Single worker exit: publish the result (release pairs with
// poll_timer_cb's acquire), free ctx if the UI already abandoned it, and
// self-delete.
static void send_task_exit(SendCtx *ctx)
{
    ctx->done.store(true, std::memory_order_release);
    if (ctx->abandoned.load() && !ctx->claimed.exchange(true)) {
        delete ctx;
    }
#ifdef ARDUINO
    vTaskDelete(NULL);
#endif
}

// Phone path: one chat_link_ask() turn (voice upload + chat.ask). No
// device_id: the phone keeps the history per pager.
static void send_phone(SendCtx *ctx)
{
    std::string err;
    HalResult<ChatReply> r = chat_link_ask(ctx->text, ctx->audio_path, err);
    ctx->audio_path.clear();
    if (r) {
        ctx->transcript = r.value().transcript;
        ctx->reply      = r.value().reply;
        ctx->ok         = true;
    } else {
        ctx->error = err.empty() ? hal_error_string(r.error()) : err;
    }
}

#ifdef ARDUINO
static void send_hub(SendCtx *ctx)
{

    // Voice memo: stream the WAV off the SD card as base64 directly into the
    // body here (the UI thread left it open after `..."audio_b64":"`). Doing
    // it on the worker in ~3 KB chunks means we never hold the raw bytes and a
    // full base64 copy at once — the old UI-thread path peaked at ~3.6x the
    // WAV size before the request even started.
    if (!ctx->audio_path.empty()) {
        bool enc_ok = hw_read_sd_stream(ctx->audio_path.c_str(), 3072,
                                        chat_b64_sink, ctx);
        ctx->audio_path.clear();
        if (enc_ok) {
            ctx->body += "\"}";
        } else {
            ctx->error = "audio read failed";
            ctx->body.clear();
            ctx->body.shrink_to_fit();
            return;
        }
        // Restart the watchdog clock now that the (potentially multi-second)
        // encode is done, so it bounds only the HTTP call as before.
        ctx->start_ms = millis();
    }

    Serial.printf("[chat] POST %s body=%u bytes\n",
                  ctx->url.c_str(), (unsigned)ctx->body.size());

    std::string resp;
    int code = 0;
    std::string terr;
    bool http_ok = chat_post(ctx->url, ctx->body, resp, code, terr);
    // Drop the request body promptly — base64 of a voice memo can be
    // hundreds of KB and we no longer need it.
    ctx->body.clear();
    ctx->body.shrink_to_fit();

    Serial.printf("[chat] http_ok=%d code=%d resp=%u bytes terr='%s'\n",
                  (int)http_ok, code, (unsigned)resp.size(), terr.c_str());
    if (!resp.empty()) {
        Serial.printf("[chat] resp[0..160]: %s\n",
                      resp.substr(0, 160).c_str());
    }

    if (!http_ok || code / 100 != 2) {
        if (!terr.empty()) {
            ctx->error = terr;
        } else {
            char buf[24];
            snprintf(buf, sizeof(buf), "HTTP %d", code);
            ctx->error = buf;
        }
        if (!resp.empty()) ctx->error += ": " + resp.substr(0, 160);
    } else {
        cJSON *j = cJSON_Parse(resp.c_str());
        if (!j) {
            ctx->error = "bad json from hub: ";
            ctx->error += resp.substr(0, 120);
        } else {
            cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "transcript");
            cJSON *r = cJSON_GetObjectItemCaseSensitive(j, "reply");
            if (t && cJSON_IsString(t) && t->valuestring) ctx->transcript = t->valuestring;
            if (r && cJSON_IsString(r) && r->valuestring) ctx->reply      = r->valuestring;
            cJSON_Delete(j);
            // Surface the silent-success case: 2xx + parseable JSON but
            // neither field populated. Without this, the UI just sits on
            // "thinking..." and never logs anything.
            if (ctx->reply.empty() && ctx->transcript.empty()) {
                ctx->error = "hub returned no reply: ";
                ctx->error += resp.substr(0, 120);
            } else {
                ctx->ok = true;
            }
        }
    }

}
#endif

static void send_task(void *arg)
{
    SendCtx *ctx = (SendCtx *)arg;
#ifdef ARDUINO
    if (ctx->phone) send_phone(ctx);
    else            send_hub(ctx);
#else
    send_phone(ctx);
#endif
    // All result fields are set; send_task_exit publishes them via `done`
    // so the UI sees a consistent ctx (reply/transcript/ok all populated).
    send_task_exit(ctx);
}

#ifdef ARDUINO
static void finish_recording();
#endif

static void poll_timer_cb(lv_timer_t *)
{
#ifdef ARDUINO
    // Voice memo cap: stop at CHAT_AUDIO_MAX_MS and send what we have, so the
    // upload to the phone stays bounded (chat_link_ask also refuses larger
    // files as a backstop).
    if (s_recording && hw_rec_running() &&
        hw_rec_elapsed_ms() >= CHAT_AUDIO_MAX_MS) {
        log_append("info: ", "15 s limit, sending");
        finish_recording();
    }
#endif
    if (!s_ctx) return;
    // Acquire pairs with the worker's release-store of `done` — once we
    // see done=true, every other field of *s_ctx is safe to read here.
    // We do NOT take ScopedInstanceLock: this callback runs inside
    // lv_timer_handler() which already holds the (non-recursive)
    // instance mutex from lvgl_task_fn, and re-taking it here deadlocks
    // the entire UI task.
    bool done = s_ctx->done.load(std::memory_order_acquire);
    if (!done) {
        if (s_ctx->start_ms != 0) {
            uint32_t elapsed = chat_now_ms() - s_ctx->start_ms;
#ifdef ARDUINO
            uint32_t budget = s_ctx->phone ? PHONE_WATCHDOG_MS : SEND_WATCHDOG_MS;
#else
            uint32_t budget = PHONE_WATCHDOG_MS;
#endif
            // Watchdog — surface stuck calls instead of leaving the UI on
            // "thinking..." indefinitely. The worker keeps running but its
            // result will be discarded.
            if (elapsed > budget) {
                char msg[64];
                snprintf(msg, sizeof(msg), "request timeout (no response in %us)",
                         (unsigned)(budget / 1000));
#ifdef ARDUINO
                Serial.printf("[chat] watchdog: send_task stuck >%us, abandoning\n",
                              (unsigned)(budget / 1000));
#endif
                cancel_inflight();
                set_busy(false);
                log_append("err: ", msg);
                set_status("");
                return;
            }
            // Live progress — gives the user feedback that the UI is
            // alive and counts up to the watchdog. Updated every poll
            // tick (~200ms) but the seconds value only changes once a
            // second so it's not noisy.
            char buf[48];
            snprintf(buf, sizeof(buf),
                     "thinking %us... (tap to cancel)",
                     (unsigned)(elapsed / 1000));
            set_status(buf);
        }
        return;
    }

    SendCtx *ctx = s_ctx;
    s_ctx = nullptr;

#ifdef ARDUINO
    Serial.printf("[chat] poll: ok=%d reply=%u transcript=%u err='%s'\n",
                  (int)ctx->ok, (unsigned)ctx->reply.size(),
                  (unsigned)ctx->transcript.size(), ctx->error.c_str());
#endif

    if (ctx->ok) {
        if (!ctx->transcript.empty()) {
            // Voice path — the user line was deferred until we knew what
            // we heard, so render it now ahead of the reply.
            log_append("you: ", ctx->transcript);
        }
        if (!ctx->reply.empty()) log_append("ai:  ", ctx->reply);
        set_status("");
    } else {
        log_append("err: ", ctx->error);
        set_status("");
    }
    set_busy(false);
    delete ctx;
}

// Spawn send_task for ctx (already in s_ctx). Returns false on spawn failure.
static bool spawn_send_task(SendCtx *ctx)
{
#ifdef ARDUINO
    // Pin to core 0 — LVGL runs on core 1, so the blocking I/O and the UI
    // redraws never compete for the same scheduler slot. Priority 1 keeps it
    // well below the LVGL task (priority 8).
    return xTaskCreatePinnedToCore(send_task, "chat_send", 8192, ctx, 1,
                                   nullptr, 0) == pdPASS;
#else
    // Emulator: no FreeRTOS; a detached thread plays the worker task (the
    // link test hook does the same).
    try {
        std::thread(send_task, ctx).detach();
        return true;
    } catch (...) {
        return false;
    }
#endif
}

static void kick_off_send(const std::string &user_text,
                          const std::string &audio_path)
{
    // 1. Phone first.
    if (hw_link_connected()) {
        SendCtx *ctx = new SendCtx();
        ctx->phone      = true;
        ctx->text       = user_text;
        ctx->audio_path = audio_path;
        s_ctx = ctx;
        ctx->start_ms = chat_now_ms();
        set_status(audio_path.empty() ? "thinking... (tap to cancel)"
                                      : "uploading... (tap to cancel)");
        if (!spawn_send_task(ctx)) {
            s_ctx = nullptr;
            delete ctx;
            log_append("err: ", "task spawn failed");
            set_busy(false);
        }
        return;
    }

#ifdef ARDUINO
    // 2. Hub fallback (hardware only) — unchanged until H4.2.
    std::string hub = hal::hub_get_url();
    if (hub.empty()) {
        log_append("err: ", "Connect your phone (or set Settings " "\xC2\xBB" " Local Hub)");
        set_busy(false);
        return;
    }
    if (!hw_get_wifi_connected()) {
        log_append("err: ", "WiFi not connected");
        set_busy(false);
        return;
    }

    SendCtx *ctx = new SendCtx();
    ctx->url = hub + "/api/chat";
    // Build the JSON envelope up front; the base64 of any voice memo is
    // streamed in on the worker (see send_task) so it never coexists in RAM
    // with the raw WAV bytes. Reserve for the eventual base64 (~4/3 of the
    // file) so the streaming appends don't repeatedly reallocate the body.
    size_t audio_bytes = audio_path.empty() ? 0 : hw_get_file_size(audio_path.c_str());
    ctx->body.reserve(256 + (audio_bytes * 4 / 3) + user_text.size() * 2);
    ctx->body += "{\"device_id\":\"";
    ctx->body += hal::json_escape(current_device_id());
    ctx->body += "\"";
    if (!user_text.empty()) {
        ctx->body += ",\"text\":\"";
        ctx->body += hal::json_escape(user_text);
        ctx->body += "\"";
    }
    if (!audio_path.empty()) {
        // Leave the string open after the opening quote; send_task streams the
        // base64 in and closes it with `"}`. (base64 needs no JSON escaping.)
        ctx->body += ",\"audio_b64\":\"";
        ctx->audio_path = audio_path;
    } else {
        ctx->body += "}";
    }

    s_ctx = ctx;
    ctx->start_ms = millis();
    set_status("thinking... (tap to cancel)");
    if (!spawn_send_task(ctx)) {
        s_ctx = nullptr;
        delete ctx;
        log_append("err: ", "task spawn failed");
        set_busy(false);
    }
#else
    // 3. Emulator with no phone: there is no hub path here.
    (void)user_text; (void)audio_path;
    log_append("err: ", "Connect your phone");
    set_busy(false);
#endif
}

static void send_btn_cb(lv_event_t *)
{
    if (s_busy || !s_input_ta) return;

    // Leave edit mode so the user can navigate away, especially if input is empty
    lv_group_t *g = lv_obj_get_group(s_input_ta);
    if (g) {
        lv_group_set_editing(g, false);
    }

    const char *raw = lv_textarea_get_text(s_input_ta);
    std::string txt = hal::trim(raw ? raw : "");
    if (txt.empty()) return;
    set_busy(true);
    log_append("you: ", txt);
    lv_textarea_set_text(s_input_ta, "");
    kick_off_send(txt, std::string());
}

#ifdef ARDUINO
static std::string build_chat_audio_path()
{
    struct tm info = {};
    hw_get_date_time(info);
    char buf[64];
    snprintf(buf, sizeof(buf),
             "%s/utt_%04d%02d%02d_%02d%02d%02d.wav",
             CHAT_DIR,
             info.tm_year + 1900, info.tm_mon + 1, info.tm_mday,
             info.tm_hour, info.tm_min, info.tm_sec);
    return buf;
}

static void ensure_chat_dir()
{
    static bool s_chat_dir_ok = false;
    if (s_chat_dir_ok) return;
    if (!(HW_SD_ONLINE & hw_get_device_online())) return;
    SD.mkdir(CHAT_DIR); // no-op if it already exists
    s_chat_dir_ok = true;
}
#endif

static void mic_btn_cb(lv_event_t *)
{
    if (s_busy) return;
#ifdef ARDUINO
    if (!s_recording) {
        if (!hw_mic_available()) {
            log_append("err: ", "no microphone");
            return;
        }
        if (!(HW_SD_ONLINE & hw_get_device_online())) {
            log_append("err: ", "SD card not mounted");
            return;
        }
        ensure_chat_dir();
        s_pending_audio_path = build_chat_audio_path();
        if (!hw_rec_start(s_pending_audio_path.c_str())) {
            log_append("err: ", "recording start failed");
            return;
        }
        s_recording = true;
        set_status("recording (max 15 s)... tap mic to stop");
        if (s_mic_btn) {
            lv_obj_set_style_bg_color(s_mic_btn,
                lv_palette_main(LV_PALETTE_RED), 0);
        }
        return;
    }

    finish_recording();
#else
    log_append("err: ", "mic only on hardware");
#endif
}

#ifdef ARDUINO
// Stop the recording and hand the WAV path to the worker (tap on the mic, or
// the 15 s cap in poll_timer_cb). The worker reads the file itself — no
// whole-file read on the UI thread.
static void finish_recording()
{
    if (hw_rec_running()) hw_rec_stop();
    s_recording = false;
    if (s_mic_btn) {
        lv_obj_set_style_bg_color(s_mic_btn, UI_COLOR_ACCENT, 0);
    }
    set_status("transcribing...");
    set_busy(true);

    // Cheap size-only check (one file open, no read) so an empty capture still
    // fails fast without pulling the bytes in.
    if (hw_get_file_size(s_pending_audio_path.c_str()) <= 44) {
        log_append("err: ", "no audio captured");
        set_busy(false);
        return;
    }
    kick_off_send(std::string(), s_pending_audio_path);
}
#endif

static void back_btn_cb(lv_event_t *) { menu_show(); }

// While the log is focused via the encoder, UP/DOWN should scroll the log
// itself rather than move focus to the next/prev widget. Without this the
// user can focus the log but can't scroll it without a touch.
static void log_key_cb(lv_event_t *e)
{
    if (!s_log_scroll) return;
    if (ui_scroll_key_clamped(s_log_scroll, lv_event_get_key(e), 24, LV_ANIM_OFF))
        lv_event_stop_processing(e);
}

// Encoder click on the focused log container exits scroll mode by handing
// focus back to the input. Without this the user gets stuck — UP/DOWN keep
// scrolling and the indev never advances to the next group object.
static void log_click_cb(lv_event_t *)
{
    if (s_input_ta) lv_group_focus_obj(s_input_ta);
}

static void enter(lv_obj_t *parent)
{
    s_log_text.clear();
    s_busy = false;
    s_recording = false;
    enable_keyboard();
    ui_show_back_button(back_btn_cb);

    lv_obj_set_style_bg_color(parent, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_row(parent, 4, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Conversation log — same look as notes-sync's scrolling log.
    s_log_scroll = lv_obj_create(parent);
    lv_obj_set_width(s_log_scroll, lv_pct(100));
    lv_obj_set_flex_grow(s_log_scroll, 1);
    lv_obj_set_style_bg_color(s_log_scroll, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_log_scroll, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_log_scroll, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(s_log_scroll, 1, 0);
    lv_obj_set_style_radius(s_log_scroll, UI_RADIUS, 0);
    lv_obj_set_style_pad_all(s_log_scroll, 6, 0);
    lv_obj_set_scroll_dir(s_log_scroll, LV_DIR_VER);

    s_log_label = lv_label_create(s_log_scroll);
    lv_label_set_long_mode(s_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_log_label, lv_pct(100));
    lv_obj_set_style_text_color(s_log_label, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(s_log_label, get_chat_font(), 0);
    lv_label_set_text(s_log_label, "");

    // The log is added to the focus group so the encoder/scrollwheel can
    // drive it. LVGL's default behavior on a focused scrollable container
    // is to scroll on encoder rotation, which is what the user expects.
    // Touch users can already drag-scroll the same container regardless.
    lv_obj_add_flag(s_log_scroll, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_add_obj(lv_group_get_default(), s_log_scroll);
    lv_obj_add_event_cb(s_log_scroll, log_key_cb, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(s_log_scroll, log_click_cb, LV_EVENT_CLICKED, nullptr);

    // Status line — "thinking...", "recording...", error hints.
    s_status_lbl = lv_label_create(parent);
    lv_obj_set_width(s_status_lbl, lv_pct(100));
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_status_lbl, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_status_lbl, get_small_font(), 0);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_add_flag(s_status_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_status_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_status_lbl, status_click_cb, LV_EVENT_CLICKED, nullptr);
    // Add to the input group so the encoder/scrollwheel can focus and
    // press it — set_busy() shifts focus here while a request is in
    // flight so the user can cancel without a touchscreen.
    lv_group_add_obj(lv_group_get_default(), s_status_lbl);

    // Input row.
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    auto make_btn = [&](const char *icon, lv_event_cb_t cb) -> lv_obj_t * {
        lv_obj_t *b = lv_btn_create(row);
        lv_obj_set_height(b, 40);
        lv_obj_set_style_bg_color(b, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_radius(b, UI_RADIUS, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, icon);
        lv_obj_set_style_text_color(l, UI_COLOR_ON_ACCENT, 0);
        lv_obj_center(l);
        lv_group_add_obj(lv_group_get_default(), b);
        return b;
    };
    s_mic_btn  = make_btn(LV_SYMBOL_AUDIO, mic_btn_cb);

    s_input_ta = lv_textarea_create(row);
    lv_textarea_set_placeholder_text(s_input_ta, "Ask anything...");
    lv_textarea_set_one_line(s_input_ta, true);
    lv_textarea_set_max_length(s_input_ta, 500);
    lv_obj_set_flex_grow(s_input_ta, 1);
    lv_obj_set_style_radius(s_input_ta, UI_RADIUS, 0);
    lv_group_add_obj(lv_group_get_default(), s_input_ta);

    // One-line textarea fires LV_EVENT_READY when Enter is pressed (or
    // when the on-screen keyboard's OK key is tapped). Treat that as send.
    lv_obj_add_event_cb(s_input_ta, send_btn_cb, LV_EVENT_READY, nullptr);

    lv_group_focus_obj(s_input_ta);

    if (hal::hub_get_url().empty()) {
        log_append("info: ",
                   "hub disabled. Settings " "\xC2\xBB" " Local Hub.");
    }

    if (!s_poll_timer) {
        s_poll_timer = lv_timer_create(poll_timer_cb, 200, nullptr);
    }
}

static void exit_cb()
{
    ui_hide_back_button();
    disable_keyboard();
    if (s_poll_timer) {
        lv_timer_del(s_poll_timer);
        s_poll_timer = nullptr;
    }
#ifdef ARDUINO
    if (s_recording && hw_rec_running()) hw_rec_stop();
#endif
    s_recording = false;

    // Abandon any in-flight request; the worker frees it on its way out.
    cancel_inflight();

    s_log_scroll = s_log_label = nullptr;
    s_input_ta = s_mic_btn = s_status_lbl = nullptr;
    s_log_text.clear();
    s_pending_audio_path.clear();
    s_busy = false;
}

class ChatApp : public core::App {
public:
    ChatApp() : core::App("Chat") {}
    void onStart(lv_obj_t *parent) override {
        setRoot(parent);
        enter(parent);
    }
    void onStop() override {
        exit_cb();
        core::App::onStop();
    }
};

} // namespace

namespace apps {
APP_FACTORY(make_chat_app, ChatApp)
}
