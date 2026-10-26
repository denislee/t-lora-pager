/**
 * @file      rotary_task.cpp
 * @brief     Rotary encoder -> LVGL indev bridge.
 *
 * Why this exists: the stock LVGL encoder read_cb (LV_Helper_v9.cpp's
 * `lv_encoder_read`) calls `instance.getRotary()` from the LVGL task.
 * That call blocks up to 50 ms on the vendor's rotary queue, and it
 * runs while the LVGL task holds the instance mutex. So:
 *   - Every idle indev poll pays a 50 ms wait under the mutex, and
 *   - If anything else takes the instance mutex for ~1 s (WiFi, NFC,
 *     FFat, radio), the LVGL task blocks entirely and scroll events
 *     stop reaching LVGL — the symptom users see is "scroll wheel
 *     stutters / freezes for 1–2 s, then catches up."
 *
 * Fix: the vendor rotaryTask hands every detent/click to
 * lilygo_rotary_event() below, which pushes it onto our own queue and
 * wakes the LVGL task; the LVGL encoder indev's read_cb pops that queue
 * non-blockingly. While the LVGL task is blocked on the mutex the queue
 * keeps filling; as soon as LVGL runs again, it drains everything in one
 * timer cycle (via `continue_reading`). DC2.4.27c: this used to be two
 * hops (vendor queue -> our rotary_reader task -> our queue); the relay
 * task and its 3 KB stack are gone, ordering is unchanged (one FIFO).
 *
 * In fake sleep the vendor task emits nothing (scrolls are dropped and a
 * click needs the display on), so there is nothing to park any more.
 */
#include "rotary_task.h"

#ifdef ARDUINO
#include <Arduino.h>   // pins_arduino.h defines USING_INPUT_DEV_ROTARY
#endif

#if defined(ARDUINO) && defined(USING_INPUT_DEV_ROTARY)

#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <lvgl.h>

#include "str_encode.h"
#include "task_util.h"
#include "../core/input_profile.h"
#include "../core/system_hooks.h"

namespace {

constexpr UBaseType_t kQueueDepth = 16;  // Burst of fast clicks while LVGL is briefly blocked.

QueueHandle_t s_event_queue = nullptr;

void enqueue_event(const RotaryMsg_t &ev)
{
    // Drop the oldest pending event when full — never stall the vendor
    // rotaryTask, which also samples the encoder and the button.
    hal::queue_send_drop_oldest(s_event_queue, ev);
    hw_input_event_wake();
}

bool is_word_boundary_char(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Mirror of LV_Helper_v9.cpp::textarea_jump_word so Alt+scroll still
// hops by word when our read_cb is in charge.
void textarea_jump_word(lv_obj_t *ta, int direction)
{
    const char *txt = lv_textarea_get_text(ta);
    if (!txt) return;

    if (direction > 0) {
        uint32_t len = lv_strlen(txt);
        bool skipped_word = false;
        while (true) {
            uint32_t pos = lv_textarea_get_cursor_pos(ta);
            if (pos >= len) break;
            uint32_t bp = hal::utf8_byte_pos(txt, pos);
            bool is_space = is_word_boundary_char(txt[bp]);
            if (!skipped_word) {
                if (!is_space) skipped_word = true;
            } else {
                if (is_space) break;
            }
            lv_textarea_cursor_right(ta);
        }
    } else {
        bool skipped_word = false;
        while (true) {
            uint32_t pos = lv_textarea_get_cursor_pos(ta);
            if (pos == 0) break;
            uint32_t bp = hal::utf8_byte_pos(txt, pos - 1);
            bool is_space = is_word_boundary_char(txt[bp]);
            if (!skipped_word) {
                if (!is_space) skipped_word = true;
            } else {
                if (is_space) break;
            }
            lv_textarea_cursor_left(ta);
        }
    }
}

void rotary_read_cb(lv_indev_t *drv, lv_indev_data_t *data)
{
    // IS.5: a queued transition has not run yet — hold input on this screen.
    if (ui_transition_pending()) {
        data->enc_diff = 0;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    static uint8_t last_dir = ROTARY_DIR_NONE;

    // Coalesce every queued rotary event into one aggregate enc_diff. A
    // fast flick emits many notches in quick succession; feeding them one
    // at a time forces a full LVGL scroll+flush per notch, which shows up
    // as stutter when the flush contends with the instance mutex. Summing
    // lets a single render cycle cover the whole flick.
    int     diff_total     = 0;
    bool    center_pressed = false;
    uint8_t latest_dir     = ROTARY_DIR_NONE;
    bool    any_event      = false;

    while (s_event_queue) {
        RotaryMsg_t msg;
        if (xQueueReceive(s_event_queue, &msg, 0) != pdTRUE) break;
        any_event = true;
        switch (msg.dir) {
        case ROTARY_DIR_UP:   diff_total += 1; break;
        case ROTARY_DIR_DOWN: diff_total -= 1; break;
        default: break;
        }
        if (msg.dir != ROTARY_DIR_NONE) latest_dir = msg.dir;
        if (msg.centerBtnPressed) center_pressed = true;
    }

    if (!any_event) {
        data->enc_diff = 0;
        data->state    = LV_INDEV_STATE_RELEASED;
        return;
    }

    // Alt+scroll jumps the textarea cursor by word. Matches vendor read_cb,
    // scaled by the aggregate so a 3-notch flick hops 3 words.
    if (diff_total != 0 && instance.isAltKeyPressed()) {
        lv_group_t *g = lv_indev_get_group(drv);
        if (g && lv_group_get_editing(g)) {
            lv_obj_t *focused = lv_group_get_focused(g);
            if (focused && lv_obj_has_class(focused, &lv_textarea_class)) {
                int dir   = diff_total > 0 ? 1 : -1;
                int steps = diff_total > 0 ? diff_total : -diff_total;
                for (int i = 0; i < steps; ++i) {
                    textarea_jump_word(focused, dir);
                }
                data->enc_diff = 0;
                data->state    = center_pressed ? LV_INDEV_STATE_PRESSED
                                                : LV_INDEV_STATE_RELEASED;
                if (last_dir != latest_dir || center_pressed) {
                    instance.feedback((void *)drv);
                }
                last_dir = latest_dir;
                data->continue_reading =
                    s_event_queue && uxQueueMessagesWaiting(s_event_queue) > 0;
                return;
            }
        }
    }

    data->enc_diff = diff_total;
    data->state    = center_pressed ? LV_INDEV_STATE_PRESSED
                                    : LV_INDEV_STATE_RELEASED;

    if (last_dir != latest_dir || center_pressed) {
        instance.feedback((void *)drv);
    }
    last_dir = latest_dir;

    // Any events that raced in after our drain will be picked up on the
    // next cb call; normally the drain loop above leaves the queue empty.
    data->continue_reading =
        s_event_queue && uxQueueMessagesWaiting(s_event_queue) > 0;
}

}  // namespace

// Vendor rotaryTask hook (lib/LilyGoLib LilyGo_LoRa_Pager.cpp, weak there).
// Runs on the vendor task for each detent/click; false until
// hw_rotary_task_start() has created the queue, so early events stay on the
// vendor queue for the stock read_cb.
extern "C" bool lilygo_rotary_event(const RotaryMsg_t *msg)
{
    if (!s_event_queue || !msg) return false;
    enqueue_event(*msg);
    return true;
}

#ifdef INPUT_PROFILE
void hw_rotary_inject(int dir, bool center)
{
    if (!s_event_queue) return;
    RotaryMsg_t msg;
    msg.dir = dir > 0 ? ROTARY_DIR_UP : dir < 0 ? ROTARY_DIR_DOWN : ROTARY_DIR_NONE;
    msg.centerBtnPressed = center;
    enqueue_event(msg);
}
#endif

void hw_rotary_task_start()
{
    if (s_event_queue) return;
    if (!instance.hasEncoder()) return;

    s_event_queue = xQueueCreate(kQueueDepth, sizeof(RotaryMsg_t));
    if (!s_event_queue) {
        log_e("rotary_task: queue alloc failed");
        return;
    }

    // Steal LVGL's encoder read_cb so LVGL drains our queue instead of
    // blocking on instance.getRotary() under the instance mutex. Events the
    // vendor task hands over before the swap wait in our queue for it.
    if (lv_indev_t *enc = lv_get_encoder_indev()) {
        lv_indev_set_read_cb(enc, rotary_read_cb);
    } else {
        log_w("rotary_task: no LVGL encoder indev found");
    }
}

#else  // emulator, or a board without the rotary encoder

void hw_rotary_task_start() {}
#ifdef INPUT_PROFILE
void hw_rotary_inject(int, bool) {}
#endif

#endif  // ARDUINO && USING_INPUT_DEV_ROTARY
