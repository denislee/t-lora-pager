/**
 * @file      ui_companion.cpp
 * @brief     Phone messaging on the pager — see ui_companion.h.
 *
 * State reset: nothing here belongs to an app. The find-pager timer
 * (s_find_timer) lives until it runs out or pager.find {"stop"} deletes it;
 * the editor sink is cleared by the editor's own exit path.
 */
#include "ui_companion.h"

#include <lvgl.h>

#include <atomic>
#include <memory>

#include "core/input_focus.h"
#include "core/notify.h"
#include "core/one_shot_job.h"
#include "core/system_hooks.h"
#include "core/ui_post.h"
#include "hal/link.h"
#include "hal/link/link_json.h"
#include "hal_interface.h"  // hw_feedback

#if defined(ARDUINO) && defined(ARDUINO_T_LORA_PAGER)
// Vendor (lib/LilyGoLib/src/LilyGo_LoRa_Pager.cpp): flips fake sleep from any
// task; the rotary task performs the toggle.
extern "C" void lilygo_request_fake_sleep_toggle();
#endif

namespace {

std::atomic<bool> s_editor_open{false};
std::atomic<bool> s_input_focused{false};
std::function<void(const std::string &)> s_editor_sink;  // LVGL thread only

constexpr uint32_t kFindPeriodMs = 2000;
constexpr uint32_t kFindDurationMs = 30000;
lv_timer_t *s_find_timer = nullptr;  // LVGL thread only

void wake_display() {
#if defined(ARDUINO) && defined(ARDUINO_T_LORA_PAGER)
    if (ui_is_fake_sleep()) lilygo_request_fake_sleep_toggle();
#endif
}

lv_obj_t *focused_textarea() {
    lv_indev_t *indev = nullptr;
    while ((indev = lv_indev_get_next(indev)) != nullptr) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_KEYPAD) continue;
        lv_group_t *g = lv_indev_get_group(indev);
        lv_obj_t *f = g ? lv_group_get_focused(g) : nullptr;
        if (f && lv_obj_has_class(f, &lv_textarea_class)) return f;
    }
    return nullptr;
}

void banner(const char *title, const std::string &body, bool error) {
    core::notify::Notification n;
    n.icon = error ? LV_SYMBOL_WARNING : LV_SYMBOL_BLUETOOTH;
    n.title = title;
    n.body = body;
    n.severity = error ? core::notify::Severity::Error : core::notify::Severity::Info;
    core::notify::post(std::move(n));
}

/**
 * Type into a focused field the way the keyboard does, so the field's own key
 * handlers run: a newline is an ENTER key (one-line fields such as the SSH
 * prompt treat it as submit), everything else is inserted as text.
 */
void type_into(lv_obj_t *ta, const std::string &text) {
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        const size_t end = (nl == std::string::npos) ? text.size() : nl;
        if (end > start) lv_textarea_add_text(ta, text.substr(start, end - start).c_str());
        if (nl == std::string::npos) break;
        uint32_t key = LV_KEY_ENTER;
        lv_obj_send_event(ta, LV_EVENT_KEY, &key);
        start = nl + 1;
    }
}

int s_find_left = 0;  // buzzes still to come (LVGL thread only)

void find_tick(lv_timer_t *t) {
    hw_feedback();
    if (--s_find_left > 0) return;
    lv_timer_delete(t);
    s_find_timer = nullptr;
}

/* --- pager → phone calls ------------------------------------------------- */

struct CallResult {
    bool ok = false;
    std::string what;  // "Clipboard sent" / "Ringing your phone"
    std::string err;
};

void call_done(CallResult &r) {
    banner(r.ok ? r.what.c_str() : "Phone", r.ok ? std::string() : r.err, !r.ok);
}

core::OneShotJob<CallResult> s_call_job(call_done);

void start_call(const char *method, std::string params, const char *what) {
    if (s_call_job.busy()) {
        banner("Phone", "still sending the last request", true);
        return;
    }
    if (!hw_link_connected()) {
        banner("Phone", "no phone connected", true);
        return;
    }
    const std::string m(method);
    const std::string w(what);
    auto st = s_call_job.start(
        [m, params, w]() -> CallResult * {
            CallResult *r = new CallResult();
            r->what = w;
            const HwLinkResult res = hw_link_call(m.c_str(), params);
            if (res.ok) {
                r->ok = true;
            } else {
                hal_error_from_link(res, r->err);
            }
            return r;
        },
        6144, "companion_call", 2);
    if (st == core::OneShotJob<CallResult>::Start::Failed) banner("Phone", "out of memory", true);
}

}  // namespace

/* --- any task ------------------------------------------------------------ */

void ui_companion_deliver(const hal::msg::InboxEntry &e, const hal::msg::Delivery &d) {
    if (d.wake) wake_display();
    if (!d.banner) return;
    core::notify::Notification n;
    n.icon = e.kind == "clip" ? LV_SYMBOL_PASTE : LV_SYMBOL_ENVELOPE;
    n.title = e.src.empty() ? e.title : e.title + "  (" + e.src + ")";
    n.body = e.body;
    n.severity = core::notify::Severity::Info;
    n.duration_ms = e.prio == hal::msg::Prio::High ? 8000 : 4000;
    n.haptic = d.haptic;
    core::notify::post(std::move(n));
}

bool ui_companion_editor_open() { return s_editor_open.load(); }

bool ui_companion_text_input_focused() { return s_input_focused.load(); }

bool ui_companion_can_wake() {
#if defined(ARDUINO) && defined(ARDUINO_T_LORA_PAGER)
    return true;
#else
    return false;
#endif
}

void ui_companion_type(hal::msg::Target target, const std::string &text) {
    core::ui_post([target, text]() {
        if (target == hal::msg::Target::Editor) {
            if (s_editor_sink) {
                s_editor_sink(text);
                return;
            }
            banner("Phone text dropped", "the note editor closed", true);
            return;
        }
        lv_obj_t *ta = focused_textarea();
        if (!ta) {
            banner("Phone text dropped", "no text field has focus", true);
            return;
        }
        type_into(ta, text);
    });
}

void ui_companion_find_pager(bool start) {
    core::ui_post([start]() {
        if (s_find_timer) {
            lv_timer_delete(s_find_timer);
            s_find_timer = nullptr;
        }
        if (!start) return;
        wake_display();
        core::notify::Notification n;
        n.icon = LV_SYMBOL_BELL;
        n.title = "Find pager";
        n.body = "Your phone is looking for this pager";
        n.severity = core::notify::Severity::Error;
        n.duration_ms = kFindDurationMs;
        n.haptic = true;
        core::notify::post(std::move(n));
        // The first buzz came with the banner; the timer adds the rest and
        // deletes itself after the last one.
        s_find_left = kFindDurationMs / kFindPeriodMs - 1;
        s_find_timer = lv_timer_create(find_tick, kFindPeriodMs, nullptr);
    });
}

/* --- LVGL thread --------------------------------------------------------- */

void ui_companion_set_editor_sink(std::function<void(const std::string &)> sink) {
    s_editor_sink = std::move(sink);
    s_editor_open.store(static_cast<bool>(s_editor_sink));
}

void ui_companion_refresh_focus() { s_input_focused.store(core::isTextInputFocused()); }

void ui_companion_send_clip(const std::string &text) {
    const size_t n = hal::msg::utf8_prefix_len(text, hal::msg::kClipMax);
    if (n == 0) {
        banner("Phone", "nothing to send", true);
        return;
    }
    pagerlink::JsonWriter w;
    w.begin_object().key("text").str_val(text.substr(0, n)).end_object();
    start_call("clip.push", w.str(),
               n < text.size() ? "Sent to phone (first 4000 bytes)" : "Sent to phone clipboard");
}

void ui_companion_find_phone() { start_call("phone.find", "{}", "Ringing your phone"); }

bool ui_companion_call_busy() { return s_call_job.busy(); }
