/**
 * @file      keyboard_task.cpp
 * @brief     High-priority keyboard reader task + LVGL indev bridge.
 *
 * Why this exists: the stock LVGL keypad read_cb (in LilyGoLib's
 * LV_Helper_v9.cpp) calls `instance.kb.getKey()` synchronously from the
 * LVGL task. Any long operation holding the instance mutex — WiFi scan,
 * NFC discovery, heavy redraw, audio init — would delay or drop key
 * presses. Users saw "keyboard stops working" symptoms.
 *
 * Here we spin up a dedicated FreeRTOS task at the highest non-system
 * priority, pinned to core 0 (opposite of the Arduino loop on core 1),
 * that drains the TCA8418 every ~10 ms and pushes decoded key events
 * into a queue. We then replace the keypad indev's read_cb with one that
 * only drains the queue — zero I2C work on the LVGL task.
 *
 * Lock behaviour: reads take core::ScopedKbLock (kb_lock.h), a keyboard-only
 * mutex, NOT the instance mutex the LVGL task holds for each whole frame — so
 * keys are read and queued even while a long frame (app build, save, flush)
 * runs, and applied in one burst when it ends (continue_reading), same as the
 * rotary path.
 */
#include "keyboard_task.h"

#ifdef ARDUINO
#include <Arduino.h>   // pins_arduino.h defines USING_INPUT_DEV_KEYBOARD
#endif

#if defined(ARDUINO) && defined(USING_INPUT_DEV_KEYBOARD)

#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <lvgl.h>

#include "display.h"
#include "str_encode.h"
#include "task_util.h"
#include "../core/input_profile.h"
#include "../core/kb_lock.h"
#include "../core/system_hooks.h"

namespace {

struct KeyEvent {
    int  state;  // KB_PRESSED / KB_RELEASED
    char c;
#ifdef INPUT_PROFILE
    uint64_t t_us;  // enqueue time, for the key-latency histogram (IS.0)
#endif
};

constexpr UBaseType_t kTaskPriority     = configMAX_PRIORITIES - 2;
constexpr BaseType_t  kTaskCore         = 0;   // Arduino loop runs on core 1
// [P4.12] ISR-notify replaces the flat 50 Hz poll.  kFallbackMs is the
// ulTaskNotifyTake timeout: a safety net that catches any interrupt state the
// TCA8418 ISR missed (the vendor's own getKey() internal poll runs at 10 Hz /
// 100 ms for exactly this reason).  Idle wakes drop from 50/s to ~10/s; a
// real keypress wakes the task in ISR→task-switch time (<1 ms) rather than
// up to 20 ms late.  Auto-repeat still fires; repeat interval is evaluated
// on each wake, and a held key shortens the wait to kRepeatIntervalMs.
// [P7.45] 100 -> 500 ms: the ISR is the real wake path, and every idle
// fallback wake costs two I2C reads on the now bus-wide lock. While a key is
// held the timeout drops to kRepeatIntervalMs so auto-repeat keeps its rate.
constexpr uint32_t    kFallbackMs       = 500; // ~2 Hz safety-net poll
constexpr UBaseType_t kQueueDepth       = 32;
constexpr uint32_t    kRepeatDelayMs    = 500; // Hold time before auto-repeat kicks in.
constexpr uint32_t    kRepeatIntervalMs = 90;  // ~11 Hz repeat rate once engaged.

QueueHandle_t s_event_queue = nullptr;
TaskHandle_t  s_task        = nullptr;
uint32_t      s_last_key    = 0;

char       s_held_char   = '\0';
TickType_t s_press_tick  = 0;
TickType_t s_repeat_tick = 0;

// Keys whose release the vendor driver swallows (handleSpecialKeys returns -1
// on release for backspace, so no KB_RELEASED surfaces). We rely on having
// disabled vendor repeat (setRepeat(false) in display.cpp) so each physical
// press gives us exactly one KB_PRESSED — we treat each as an instantaneous
// tap, never tracking s_held_char for them, so no auto-repeat while held.
// 0x17 is Alt+Backspace (word delete); '\b' is plain backspace.
inline bool is_pingkey(char c) { return c == '\b' || c == 0x17; }

// Enter must not auto-repeat: holding it should fire exactly one activation,
// not a stream. The vendor driver does emit KB_RELEASED for it, so unlike
// pingkeys we let the natural press/release pair flow through — we just skip
// held-char tracking so the synthesized repeat below never engages.
inline bool is_no_repeat_key(char c) { return c == '\n'; }

void enqueue_event(KeyEvent ev)
{
    INPUT_PROF(ev.t_us = core::prof::now_us());
    // Drop oldest if the UI somehow falls behind the queue depth. Called
    // with the keyboard lock held by the drain loop — see
    // hw_input_event_wake() for why the wake is still safe.
    hal::queue_send_drop_oldest(s_event_queue, ev);
    hw_input_event_wake();
}

void keyboard_task_fn(void *)
{
    for (;;) {
        // While the display is off (fake-sleep) hw_power_down_all() cuts the
        // keyboard rail and instance.kb.end() detaches the TCA8418 ISR, so
        // this loop would take the keyboard lock and I2C-poll a dead chip —
        // burning power and contending with LVGL/NFC exactly when we mean to
        // be idle. Block on a task notify instead; ui_resume_timers() kicks us
        // via hw_keyboard_task_notify_wake() so the first keypress after wake
        // isn't delayed, and the timeout bounds latency if a notify is missed.
        // Drop any held-key state so a key held before sleep can't fire a
        // stale auto-repeat on wake.
        if (hal::park_if_fake_sleep()) {
            s_held_char = '\0';
            continue;
        }

        // [P4.12] Block until the TCA8418 ISR notifies us (via setNotifyTask /
        // vTaskNotifyGiveFromISR) or the safety-net timeout fires.  The notify
        // fires on every TCA8418 interrupt edge, so a real keypress unblocks
        // immediately.  kFallbackMs (500 ms, P7.45) bounds a missed edge;
        // getKey() still runs the vendor's INT_STAT safety check on each wake.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_held_char != '\0' ? kRepeatIntervalMs
                                                                  : kFallbackMs));

        if ((hw_get_device_online() & HW_KEYBOARD_ONLINE) == 0) {
            continue;
        }

        // Drain everything pending in a single lock hold so we don't
        // thrash the mutex on bursts (fast typing can emit several events
        // per poll).
        {
#ifdef INPUT_PROFILE
            uint64_t wait0 = core::prof::now_us();
#endif
            core::ScopedKbLock lock;
            INPUT_PROF(core::prof::kb_lock_wait(core::prof::now_us() - wait0));
            for (int i = 0; i < 16; ++i) {
                char c = '\0';
                int state = instance.kb.getKey(&c);
                if (state != KB_PRESSED && state != KB_RELEASED) {
                    break;
                }
                if (state == KB_PRESSED) {
                    TickType_t now = xTaskGetTickCount();
                    // Backspace and Alt+Backspace: the vendor's
                    // handleSpecialKeys swallows KB_RELEASED. With vendor
                    // repeat disabled (display.cpp), each physical press
                    // produces exactly one KB_PRESSED, so we can emit a
                    // synthetic PRESS+RELEASE pulse per event without
                    // worrying about distinguishing held from fast re-tap.
                    // Deliberately skip held-char tracking so the auto-
                    // repeat timer below never fires for these keys.
                    if (is_pingkey(c)) {
                        enqueue_event({KB_PRESSED,  c});
                        enqueue_event({KB_RELEASED, c});
                        continue;
                    }
                    // Enter: pass through natural press/release without
                    // engaging held-char tracking, so holding it never
                    // auto-repeats and the dedupe below can't swallow a
                    // legitimate second press.
                    if (is_no_repeat_key(c)) {
                        s_held_char = '\0';
                        enqueue_event({KB_PRESSED, c});
                        continue;
                    }
                    // Drop duplicate presses for the same held char — the
                    // TCA8418 occasionally reports the same key twice in
                    // a burst and we don't want LVGL to see it as two
                    // separate presses or to reset the repeat delay.
                    if (c != '\0' && c == s_held_char) {
                        continue;
                    }
                    s_held_char   = c;
                    s_press_tick  = now;
                    s_repeat_tick = now;
                } else {
                    // Any release cancels the held state. If we only
                    // cleared when c matched s_held_char, a release
                    // carrying a different char (or a '\0') would leave
                    // auto-repeat running forever.
                    s_held_char = '\0';
                }
                enqueue_event({state, c});
            }
        }

        // Synthesize auto-repeat while a regular key stays held — inject a
        // RELEASE + PRESS pair so LVGL treats it as a fresh event. Pingkeys
        // ('\b', 0x17) and Enter never reach this path because we don't set
        // s_held_char for them above.
        if (s_held_char != '\0') {
            TickType_t now       = xTaskGetTickCount();
            uint32_t   held_ms   = (now - s_press_tick)  * portTICK_PERIOD_MS;
            uint32_t   since_rep = (now - s_repeat_tick) * portTICK_PERIOD_MS;
            if (held_ms >= kRepeatDelayMs && since_rep >= kRepeatIntervalMs) {
                enqueue_event({KB_RELEASED, s_held_char});
                enqueue_event({KB_PRESSED,  s_held_char});
                s_repeat_tick = now;
            }
        }
    }
}

// Handle Alt+Backspace word delete inline (same behaviour as the stock
// LVGL keypad_read in LV_Helper_v9.cpp).
void handle_alt_backspace(lv_indev_t *drv)
{
    lv_group_t *g = lv_indev_get_group(drv);
    if (!g) return;
    lv_obj_t *focused = lv_group_get_focused(g);
    if (!focused || !lv_obj_has_class(focused, &lv_textarea_class)) return;

    bool deleting_spaces = true;
    while (true) {
        const char *txt = lv_textarea_get_text(focused);
        uint32_t cursor_pos = lv_textarea_get_cursor_pos(focused);
        if (cursor_pos == 0 || !txt) break;
        uint32_t bp = hal::utf8_byte_pos(txt, cursor_pos - 1);
        bool is_space = (txt[bp] == ' ');
        if (deleting_spaces) {
            if (!is_space) deleting_spaces = false;
        } else {
            if (is_space) break;
        }
        lv_textarea_delete_char(focused);
    }
}

void kb_read_cb(lv_indev_t *drv, lv_indev_data_t *data)
{
    // IS.5: a leave-the-app event queued a transition that runs after this
    // dispatch. Leave the remaining keys queued so they reach the new screen.
    if (ui_transition_pending()) {
        data->state = LV_INDEV_STATE_REL;
        data->key   = s_last_key;
        return;
    }
    KeyEvent ev;
    if (!s_event_queue || xQueueReceive(s_event_queue, &ev, 0) != pdTRUE) {
        data->state = LV_INDEV_STATE_REL;
        data->key   = s_last_key;
        return;
    }

    bool more = uxQueueMessagesWaiting(s_event_queue) > 0;
    INPUT_PROF(core::prof::key_latency(core::prof::now_us() - ev.t_us));

    if (ev.state == KB_PRESSED) {
        if (ev.c == 0x17) {  // Alt+Backspace
            handle_alt_backspace(drv);
            data->state = LV_INDEV_STATE_REL;
            data->continue_reading = more;
            return;
        }
        s_last_key = (uint32_t)(uint8_t)ev.c;
        data->key   = s_last_key;
        data->state = LV_INDEV_STATE_PR;
        data->continue_reading = more;
        return;
    }

    // Released
    data->state = LV_INDEV_STATE_REL;
    data->key   = ev.c ? (uint32_t)(uint8_t)ev.c : s_last_key;
    data->continue_reading = more;
}

}  // namespace

void hw_keyboard_task_notify_wake() { hal::notify_task(s_task); }

void hw_keyboard_flush_queue()
{
    if (!s_event_queue) return;
    // Under the keyboard lock so the reader can't be mid-burst between
    // dequeue-able events and its held-char update.
    core::ScopedKbLock lock;
    xQueueReset(s_event_queue);
    s_held_char = '\0';
}

#ifdef INPUT_PROFILE
void hw_keyboard_inject(int state, char c)
{
    if (!s_event_queue) return;
    core::ScopedKbLock lock;
    enqueue_event({state, c});
}
#endif

void hw_keyboard_task_start()
{
    if (s_task) return;
    if (!(hw_get_device_online() & HW_KEYBOARD_ONLINE)) return;

    s_event_queue = xQueueCreate(kQueueDepth, sizeof(KeyEvent));
    if (!s_event_queue) {
        log_e("kb_reader: queue alloc failed");
        return;
    }

    // Steal LVGL's keypad read_cb so LVGL drains our queue instead of
    // hitting I2C on its own task.
    if (lv_indev_t *kb = lv_get_keyboard_indev()) {
        lv_indev_set_read_cb(kb, kb_read_cb);
    } else {
        log_w("kb_reader: no LVGL keypad indev found; polling will still run");
    }

    if (!hal::start_pinned_task(keyboard_task_fn, "kb_reader", 4096,
                                kTaskPriority, kTaskCore, &s_task,
                                "kb_reader")) {
        return;
    }

    // [P4.12] Register our task handle with the vendor driver so keyboard_isr
    // can vTaskNotifyGiveFromISR us on every TCA8418 interrupt edge.  This
    // replaces the 50 Hz vTaskDelayUntil poll with an ISR-driven wake + 500 ms
    // fallback timeout (P7.45).  The task is never stopped; should it be deleted, the
    // handle goes stale, and the vendor's begin()/end() cycle re-attaches/detaches the ISR so no spurious
    // notifies can reach a dead handle between end() and the next begin().
    {
        core::ScopedKbLock lock;
        instance.kb.setNotifyTask(s_task);
    }
}

#else  // emulator, or a board without the keyboard

void hw_keyboard_task_start() {}
void hw_keyboard_task_notify_wake() {}
void hw_keyboard_flush_queue() {}
#ifdef INPUT_PROFILE
void hw_keyboard_inject(int, char) {}
#endif

#endif  // ARDUINO && USING_INPUT_DEV_KEYBOARD
