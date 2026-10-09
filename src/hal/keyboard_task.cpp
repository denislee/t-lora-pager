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
 * Lock behaviour: reads still go through the shared `instance` mutex, but
 * (a) we hold it only for the duration of a getKey() burst and (b) the
 * mutex has priority inheritance (xSemaphoreCreateMutex), so if a
 * low-priority holder is running while we block, it gets boosted to our
 * priority and releases sooner.
 */
#include "keyboard_task.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "display.h"
#include "system.h"
#include "../core/scoped_lock.h"
#include "../core/system_hooks.h"

#ifdef USING_INPUT_DEV_KEYBOARD

namespace {

struct KeyEvent {
    int  state;  // KB_PRESSED / KB_RELEASED
    char c;
};

constexpr UBaseType_t kTaskPriority     = configMAX_PRIORITIES - 2;
constexpr BaseType_t  kTaskCore         = 0;   // Arduino loop runs on core 1
// [P4.12] ISR-notify replaces the flat 50 Hz poll.  kFallbackMs is the
// ulTaskNotifyTake timeout: a safety net that catches any interrupt state the
// TCA8418 ISR missed (the vendor's own getKey() internal poll runs at 10 Hz /
// 100 ms for exactly this reason).  Idle wakes drop from 50/s to ~10/s; a
// real keypress wakes the task in ISR→task-switch time (<1 ms) rather than
// up to 20 ms late.  Auto-repeat still fires; repeat interval is evaluated
// on each wake so it may be up to kFallbackMs late — imperceptible.
constexpr uint32_t    kFallbackMs       = 100; // ~10 Hz safety-net poll
constexpr UBaseType_t kQueueDepth       = 32;
constexpr uint32_t    kRepeatDelayMs    = 500; // Hold time before auto-repeat kicks in.
constexpr uint32_t    kRepeatIntervalMs = 90;  // ~11 Hz repeat rate once engaged.
// Fallback re-check while display off. hw_keyboard_task_notify_wake() from
// ui_resume_timers() is the real wake path; 1 s here is a safety net only.
constexpr uint32_t    kFakeSleepIdleMs  = 1000;

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

void enqueue_event(const KeyEvent &ev)
{
    // Drop oldest if the UI somehow falls behind the queue depth —
    // better to lose a stale key than to stall the reader.
    if (xQueueSend(s_event_queue, &ev, 0) != pdTRUE) {
        KeyEvent discard;
        xQueueReceive(s_event_queue, &discard, 0);
        xQueueSend(s_event_queue, &ev, 0);
    }
    // Wake the LVGL task immediately so input renders without waiting for
    // the kMaxTickMs (200 ms) fallback sleep. xTaskNotifyGive is cheap and
    // idempotent — multiple gives before the task's ulTaskNotifyTake
    // collapse to one. Safe here even though the drain loop holds the
    // instance mutex: the LVGL task will simply block on it until we release.
    hw_cpu_boost_for_input();
    hw_lvgl_task_notify_wake();
}

void keyboard_task_fn(void *)
{
    for (;;) {
        // While the display is off (fake-sleep) hw_power_down_all() cuts the
        // keyboard rail and instance.kb.end() detaches the TCA8418 ISR, so
        // this loop would take the instance mutex and I2C-poll a dead chip —
        // burning power and contending with LVGL/NFC exactly when we mean to
        // be idle. Block on a task notify instead; ui_resume_timers() kicks us
        // via hw_keyboard_task_notify_wake() so the first keypress after wake
        // isn't delayed, and the timeout bounds latency if a notify is missed.
        // Drop any held-key state so a key held before sleep can't fire a
        // stale auto-repeat on wake.
        if (ui_is_fake_sleep()) {
            s_held_char = '\0';
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kFakeSleepIdleMs));
            continue;
        }

        // [P4.12] Block until the TCA8418 ISR notifies us (via setNotifyTask /
        // vTaskNotifyGiveFromISR) or the safety-net timeout fires.  The notify
        // fires on every TCA8418 interrupt edge, so a real keypress unblocks
        // immediately.  kFallbackMs (100 ms = 10 Hz) matches the vendor
        // driver's own internal safety-poll cadence inside getKey(), so we
        // never miss an interrupt state it would have caught.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kFallbackMs));

        if ((hw_get_device_online() & HW_KEYBOARD_ONLINE) == 0) {
            continue;
        }

        // Drain everything pending in a single lock hold so we don't
        // thrash the mutex on bursts (fast typing can emit several events
        // per poll).
        {
            core::ScopedInstanceLock lock;
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

uint32_t get_byte_pos(const char *txt, uint32_t char_pos)
{
    uint32_t bp = 0;
    while (char_pos > 0 && txt[bp] != '\0') {
        uint8_t c = txt[bp];
        if      ((c & 0x80) == 0x00) bp += 1;
        else if ((c & 0xE0) == 0xC0) bp += 2;
        else if ((c & 0xF0) == 0xE0) bp += 3;
        else if ((c & 0xF8) == 0xF0) bp += 4;
        else                         bp += 1;
        char_pos--;
    }
    return bp;
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
        uint32_t bp = get_byte_pos(txt, cursor_pos - 1);
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
    KeyEvent ev;
    if (!s_event_queue || xQueueReceive(s_event_queue, &ev, 0) != pdTRUE) {
        data->state = LV_INDEV_STATE_REL;
        data->key   = s_last_key;
        return;
    }

    bool more = uxQueueMessagesWaiting(s_event_queue) > 0;

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

void hw_keyboard_task_notify_wake()
{
    // Safe from any task context; a give while the task is running (not
    // blocked) simply leaves the notification pending for the next take.
    if (s_task) xTaskNotifyGive(s_task);
}

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

    BaseType_t ok = xTaskCreatePinnedToCore(
        keyboard_task_fn, "kb_reader", 4096, nullptr,
        kTaskPriority, &s_task, kTaskCore);
    if (ok != pdPASS) {
        log_e("kb_reader: task create failed");
        s_task = nullptr;
        return;
    }

    // [P4.12] Register our task handle with the vendor driver so keyboard_isr
    // can vTaskNotifyGiveFromISR us on every TCA8418 interrupt edge.  This
    // replaces the 50 Hz vTaskDelayUntil poll with an ISR-driven wake + 100 ms
    // fallback timeout.  Cleared in hw_keyboard_task_stop() (if added later)
    // or implicitly when the task is deleted and the handle becomes stale —
    // vendor's begin()/end() cycle re-attaches/detaches the ISR so no spurious
    // notifies can reach a dead handle between end() and the next begin().
    {
        core::ScopedInstanceLock lock;
        instance.kb.setNotifyTask(s_task);
    }
}

#else  // !USING_INPUT_DEV_KEYBOARD

void hw_keyboard_task_start() {}
void hw_keyboard_task_notify_wake() {}

#endif  // USING_INPUT_DEV_KEYBOARD

#else  // !ARDUINO

void hw_keyboard_task_start() {}
void hw_keyboard_task_notify_wake() {}

#endif  // ARDUINO
