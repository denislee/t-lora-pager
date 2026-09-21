/**
 * @file      charge_task.cpp
 * @brief     Charge-indicator overlay while in fake light sleep.
 *
 * When the user long-presses the rotary to put the device into fake light
 * sleep (display off, peripherals down, LVGL task idle — see
 * `lib/LilyGoLib/.../rotaryTask` and `ui_pause_timers`), we still want a
 * cable-plug event to surface as user-visible feedback. This task polls
 * VBUS on its own core-0 thread; on the rising edge while fake-sleep is
 * active it briefly wakes the display, throws up a "Charging XX%" overlay
 * for a few seconds, then puts the device back to sleep.
 *
 * The vendor rotary task owns its own `display_off` static, which we don't
 * touch — the round-trip ends with the display in the same logical state
 * as before, so a subsequent long-press still wakes correctly.
 */
#include "charge_task.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <LilyGoLib.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../core/scoped_lock.h"
#include "../core/system_hooks.h"
#include "../ui_define.h"
#include "internal.h"
#include "power.h"

namespace {

constexpr UBaseType_t kTaskPriority       = 3;
constexpr BaseType_t  kTaskCore           = 0;
// Base (fast) poll interval — used for the first kFastPhaseMs after sleep
// entry or a VBUS edge (P4.8 progressive backoff).
constexpr uint32_t    kPollFastMs         = 500;
// Progressive back-off stages (P4.8): fast → medium → slow.
constexpr uint32_t    kFastPhaseMs        = 30000;   // first 30 s
constexpr uint32_t    kMediumPhaseMs      = 90000;   // 30–90 s
constexpr uint32_t    kPollMediumMs       = 2000;
constexpr uint32_t    kPollSlowMs         = 5000;
// P5.3 — fourth back-off stage. Past kDormantPhaseMs of uninterrupted fake
// sleep this task is the last periodic I2C consumer on the bus, and the only
// event it can still catch is a cable plug. Someone who plugs in a device that
// has been asleep for ten minutes is not watching for the overlay in the first
// few seconds, so trade that latency for a 3x cut in the residual poll rate.
// Any VBUS edge resets last_reset_ms and drops straight back to kPollFastMs.
constexpr uint32_t    kDormantPhaseMs     = 600000;  // 10 min
constexpr uint32_t    kPollDormantMs      = 15000;
constexpr uint32_t    kShowMs             = 4000;
// Slow tick for 80 % charge-cap enforcement during fake sleep (P4.5).
// lv_timers are frozen while fake-sleeping, so this task drives the check.
constexpr uint32_t    kBatteryCheckMs     = 60000;  // once per minute
// Fallback re-check while awake. Normally we block until ui_pause_timers()
// notifies us that fake-sleep began; this bounds recovery if a notify is
// ever missed, without polling VBUS over I2C around the clock.
constexpr uint32_t    kAwakeBlockMs       = 3000;

TaskHandle_t s_task = nullptr;

bool poll_vbus_locked()
{
#if defined(USING_PPM_MANAGE)
    return instance.ppm.isVbusIn();
#elif defined(USING_PMU_MANAGE)
    return instance.pmu.isVbusIn();
#else
    return false;
#endif
}

// Pulse the charge icon's opacity so the overlay reads as "alive" — without
// it, a static screen at full brightness for 4 s feels dead. Only opacity
// is animated; the icon glyph itself never moves.
static void charge_icon_pulse_cb(void *var, int32_t v)
{
    lv_obj_set_style_text_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

lv_obj_t *build_charge_overlay()
{
    monitor_params_t p;
    hw_get_monitor_params(p);

    lv_obj_t *overlay = ui_popup_create(NULL);
    lv_obj_set_style_pad_row(overlay, 12, 0);

    // Big pulsing lightning bolt — the dominant visual anchor.
    lv_obj_t *icon = lv_label_create(overlay);
    lv_label_set_text(icon, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(icon, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_opa(icon, LV_OPA_COVER, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, icon);
    lv_anim_set_exec_cb(&a, charge_icon_pulse_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_30);
    lv_anim_set_time(&a, 700);
    lv_anim_set_playback_time(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    // Hero percentage in a much larger face than the rest of the firmware.
    lv_obj_t *pct = lv_label_create(overlay);
    lv_label_set_text_fmt(pct, "%d%%", p.battery_percent);
    lv_obj_set_style_text_color(pct, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(pct, &lv_font_montserrat_48, 0);

    // Battery progress bar — concrete read of how full the cell is right now.
    // Ranged 0..100 so the user sees absolute fill, not a relative animation.
    lv_obj_t *bar = lv_bar_create(overlay);
    lv_obj_set_size(bar, lv_pct(70), 10);
    lv_bar_set_range(bar, 0, 100);
    int32_t bar_val = p.battery_percent;
    if (bar_val < 0) bar_val = 0;
    if (bar_val > 100) bar_val = 100;
    lv_bar_set_value(bar, bar_val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, UI_COLOR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_palette_main(LV_PALETTE_GREEN),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 5, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(bar, lv_palette_main(LV_PALETTE_GREEN),
                                  LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(bar, 16, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_opa(bar, LV_OPA_50, LV_PART_INDICATOR);

    lv_obj_t *sub = lv_label_create(overlay);
    lv_label_set_text(sub, "Charging");
    lv_obj_set_style_text_color(sub, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_18, 0);

    return overlay;
}

void charge_task_fn(void *)
{
    bool     prev_vbus            = false;
    bool     primed               = false;  // ignore first sample; we want edges, not boot state
    uint32_t last_reset_ms        = 0;      // P4.8: reset on sleep entry or VBUS edge
    uint32_t last_battery_chk_ms  = 0;      // P4.5: 80% enforcement slow tick

    for (;;) {
        if (!ui_is_fake_sleep()) {
            // The charging overlay only ever shows during fake-sleep, so there
            // is nothing to detect while the device is awake. Block until
            // fake-sleep is entered (ui_pause_timers notifies us) rather than
            // polling VBUS over I2C at 2 Hz around the clock. The timeout is a
            // safety net so a lost notify can't strand us.
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kAwakeBlockMs));
            // Re-prime so the first fake-sleep sample only sets the baseline —
            // a cable already plugged before sleeping must not fire the overlay.
            primed         = false;
            last_reset_ms  = millis();   // P4.8: restart fast phase on sleep entry
            continue;
        }

        // P4.8 — progressive back-off: fast for the first 30 s after sleep
        // entry or a VBUS edge, medium for the next minute, then slow.
        uint32_t elapsed_since_reset = millis() - last_reset_ms;
        uint32_t poll_ms;
        if (elapsed_since_reset < kFastPhaseMs) {
            poll_ms = kPollFastMs;
        } else if (elapsed_since_reset < kMediumPhaseMs) {
            poll_ms = kPollMediumMs;
        } else if (elapsed_since_reset < kDormantPhaseMs) {
            poll_ms = kPollSlowMs;
        } else {
            poll_ms = kPollDormantMs;   // P5.3
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));

        bool cur_vbus;
        {
            core::ScopedInstanceLock lock;
            cur_vbus = poll_vbus_locked();
        }

        bool rising = primed && cur_vbus && !prev_vbus;
        prev_vbus = cur_vbus;
        primed = true;

        if (rising) {
            // P4.8: VBUS edge resets the fast-phase counter.
            last_reset_ms = millis();
        }

        // P4.29 — age the notes-passphrase grace period. This task is the only
        // thing still ticking during fake sleep, so it owns the aging; the call
        // is a cheap no-op outside the grace window.
        hw_fake_sleep_tick(millis());

        // P4.5 — 80 % charge-cap slow tick.  lv_timers are frozen during
        // fake sleep so the cap would never fire overnight.  Drive the same
        // hw_update_battery_history() that the awake lv_timer calls, once
        // per minute.  Only battery_percent (from the fuel gauge) matters
        // for enforcement — the BQ gauge does not need ppm.enableMeasure().
        {
            uint32_t now = millis();
            if (now - last_battery_chk_ms >= kBatteryCheckMs) {
                last_battery_chk_ms = now;
                core::ScopedInstanceLock lock;
                hw_update_battery_history();
                // The BQ25896 ADC is off in fake sleep (PB.4), so the read this
                // just did populated the TTL cache with zeroed usb/sys voltages.
                // Drop it so the first post-wake reader gets live values rather
                // than up to 5 s of stale zeros — the same P4.7 symptom.
#if defined(USING_PPM_MANAGE)
                hw_invalidate_monitor_cache();
#endif
            }
        }

        if (!rising) continue;
        if (!ui_is_fake_sleep()) continue;

        uint8_t target_brightness = user_setting.brightness_level;
        if (target_brightness == 0) target_brightness = 100;

        lv_obj_t *overlay = nullptr;
        {
            core::ScopedInstanceLock lock;
            // P4.7 — the BQ25896 ADC is disabled during fake sleep
            // (hw_power_down_all did ppm.disableMeasure()).  Invalidate the
            // TTL cache so build_charge_overlay() reads live ADC values, then
            // re-enable the ADC before the read so usb_voltage / sys_voltage
            // are not zero on the overlay or on the Info page after wake.
#if defined(USING_PPM_MANAGE)
            hw_invalidate_monitor_cache();
            instance.ppm.enableMeasure();
#endif
            instance.wakeupDisplay();
            instance.setBrightness(target_brightness);
            ui_resume_timers();
            lv_display_trigger_activity(NULL);
            overlay = build_charge_overlay();
        }

        // Hold the overlay on screen while LVGL renders. The instance lock
        // is released so the LVGL task can pump and flush.
        vTaskDelay(pdMS_TO_TICKS(kShowMs));

        {
            core::ScopedInstanceLock lock;
            if (overlay) ui_popup_destroy(overlay);
            ui_pause_timers();
            instance.setBrightness(0);
            instance.sleepDisplay();
            // P4.7 — put the ADC back to sleep to match hw_power_down_all()
            // which disabled it on fake-sleep entry.
#if defined(USING_PPM_MANAGE)
            instance.ppm.disableMeasure();
#endif
        }

        // Refresh prev_vbus from a fresh sample so a still-asserted VBUS
        // doesn't immediately re-fire after we've shown the popup once.
        {
            core::ScopedInstanceLock lock;
            prev_vbus = poll_vbus_locked();
        }
    }
}

}  // namespace

void hw_charge_task_on_fake_sleep_enter()
{
    if (s_task) xTaskNotifyGive(s_task);
}

void hw_charge_task_start()
{
    if (s_task) return;
    BaseType_t ok = xTaskCreatePinnedToCore(
        charge_task_fn, "charge_ind", 4096, nullptr,
        kTaskPriority, &s_task, kTaskCore);
    if (ok != pdPASS) {
        log_e("charge_task: task create failed");
        s_task = nullptr;
    }
}

#else  // !ARDUINO

void hw_charge_task_start() {}
void hw_charge_task_on_fake_sleep_enter() {}

#endif  // ARDUINO
