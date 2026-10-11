/**
 * @file      power.cpp
 * @brief     Battery, charger, OTG, monitor.
 */
#include "power.h"
#include "system.h"
#include "internal.h"

#include <lvgl.h>

#ifdef ARDUINO
#include <LilyGoLib.h>
#else
#include <cstdlib>
#include <cstring>
#endif

// --- Battery history ---------------------------------------------------

static std::vector<int16_t> battery_history;
static const size_t MAX_BATTERY_HISTORY = 60; // 1 hour at 60 s interval (was "5 hours if 5 mins", corrected P4.30)

void hw_update_battery_history()
{
#ifdef ARDUINO
    monitor_params_t params;
    hw_get_monitor_params(params);
    int16_t percent = params.battery_percent;
    if (percent >= 0 && percent <= 100) {
        if (battery_history.size() >= MAX_BATTERY_HISTORY) {
            battery_history.erase(battery_history.begin());
        }
        battery_history.push_back(percent);
        log_d("Recorded battery percent: %d%%", percent);

        // Charge conservation logic: Stop charging if >= 80% and feature is enabled
        if (user_setting.charge_limit_en) {
            if (percent >= 80) {
                if (hw_get_charge_enable()) {
                    log_i("Battery life conservation: Reached %d%%, stopping charger.", percent);
                    hw_set_charger(false);
                }
            } else if (percent < 75) {
                // Re-enable charging if it drops below 75% while the limit feature is on
                if (user_setting.charger_enable && !hw_get_charge_enable()) {
                    log_i("Battery life conservation: Below 75%% (%d%%), re-enabling charger.", percent);
                    hw_set_charger(true);
                }
            }
        } else {
            // Limit disabled: Ensure charger follows the main charger_enable setting
            if (user_setting.charger_enable != hw_get_charge_enable()) {
                hw_set_charger(user_setting.charger_enable);
            }
        }
    }
#else
    static int16_t sim_percent = 100;
    if (battery_history.size() >= MAX_BATTERY_HISTORY) {
        battery_history.erase(battery_history.begin());
    }
    battery_history.push_back(sim_percent);
    sim_percent -= 2;
    if (sim_percent < 10) sim_percent = 100;
#endif
}

void battery_history_timer_cb(lv_timer_t *timer)
{
    hw_update_battery_history();
}

// --- Battery voltage ---------------------------------------------------

int16_t hw_get_battery_voltage()
{
#ifdef ARDUINO

#if  defined(USING_BQ_GAUGE)
    if (HW_GAUGE_ONLINE & hw_get_device_online()) {
        instance.gauge.refresh();
        return instance.gauge.getVoltage();
    } else {
        log_d("Gauge Not online !");
        return 0;
    }
#elif defined(USING_PMU_MANAGE)
    return instance.pmu.getBattVoltage();
#else
    return 0;
#endif

#else
    return 0;
#endif
}

// --- OTG ---------------------------------------------------------------

bool hw_get_otg_enable()
{
#if defined(ARDUINO) && defined(USING_PPM_MANAGE)
    return  instance.ppm.isEnableOTG();
#else
    return false;
#endif
}

bool hw_set_otg(bool enable)
{
#if defined(ARDUINO) && defined(USING_PPM_MANAGE)
    if (enable) {
        return  instance.ppm.enableOTG();
    } else {
        instance.ppm.disableOTG();
    }
    return true;
#endif
    return false;
}

bool hw_has_otg_function()
{
    // Only the Pager's BQ25896 PPM can source OTG power. The pager emulator
    // never sees the variant's USING_PPM_MANAGE, so it mirrors the hardware.
#if defined(USING_PPM_MANAGE) || (!defined(ARDUINO) && defined(ARDUINO_T_LORA_PAGER))
    return true;
#else
    return false;
#endif
}

// --- Charger -----------------------------------------------------------

bool hw_get_charge_enable()
{
#ifdef ARDUINO
#if defined(USING_PPM_MANAGE)
    return  instance.ppm.isEnableCharge();
#elif defined(USING_PMU_MANAGE)
    return  instance.isEnableCharge();
#endif
#else
    return false;
#endif
}

void hw_set_charger(bool enable)
{
#ifdef ARDUINO
#if defined(USING_PPM_MANAGE)
    if (enable) {
        instance.ppm.enableCharge();
    } else {
        instance.ppm.disableCharge();
    }
#elif defined(USING_PMU_MANAGE)
    if (enable) {
        instance.enableCharge();
    } else {
        instance.disableCharge();
    }
#endif
#endif
}

void hw_set_charger_current(uint16_t milliampere)
{
#ifdef ARDUINO
#if defined(USING_PPM_MANAGE)
    instance.ppm.setChargerConstantCurr(milliampere);
#elif defined(USING_PMU_MANAGE)
    instance.setChargeCurrent(milliampere);
#endif
#endif
}

// Charge-current steps (mA), indexed by the Settings charger-current level,
// for the PMU-managed boards and the emulator. PPM-managed boards (the pager)
// use multiples of dev_conts_var.charge_steps instead.
#if !defined(ARDUINO) || !defined(USING_PPM_MANAGE)
static constexpr uint16_t kChargeCurrentTable[] = {
    100, 125, 150, 175,
    200, 300, 400, 500,
    600, 700, 800, 900,
    1000
};
static constexpr uint8_t kChargeCurrentLevels =
    sizeof(kChargeCurrentTable) / sizeof(kChargeCurrentTable[0]);
#endif

uint8_t hw_get_charger_current_level()
{
#if defined(USING_PPM_MANAGE)
    return user_setting.charger_current / dev_conts_var.charge_steps;
#else
#if defined(USING_PMU_MANAGE)
    uint16_t cur = instance.getChargeCurrent();
#else
    uint16_t cur = user_setting.charger_current;
#endif
    for (uint8_t i = 0; i < kChargeCurrentLevels; ++i) {
        if (cur == kChargeCurrentTable[i]) return i;
    }
    return 0;
#endif
}

uint16_t hw_set_charger_current_level(uint8_t level)
{
#if defined(ARDUINO) && defined(USING_PPM_MANAGE)
    log_d("set charge current:%u mA", level * dev_conts_var.charge_steps);
    instance.ppm.setChargerConstantCurr(level * dev_conts_var.charge_steps);
    return  level * dev_conts_var.charge_steps;
#elif defined(USING_PMU_MANAGE) || !defined(ARDUINO)
    if (level > kChargeCurrentLevels - 1) level = kChargeCurrentLevels - 1;
    log_d("set charge current:%u mA", kChargeCurrentTable[level]);
#if defined(ARDUINO)
    instance.setChargeCurrent(kChargeCurrentTable[level]);
#endif
    return kChargeCurrentTable[level];
#endif
}

// --- Monitor / power params -------------------------------------------

#ifdef ARDUINO
static monitor_params_t s_cached_monitor_params;
static uint32_t s_monitor_last_refresh = 0;
#endif

// On the pager none of these reads use the BQ25896 ADC (charge status is a
// status-register bit; voltage and percent come from the fuel gauge), so the
// result stays valid in fake sleep, where hw_power_down_all() turns the ADC
// off.
void hw_get_monitor_params(monitor_params_t &params)
{
#ifdef ARDUINO

    // Refresh at most once per second while charging (the status bar animates
    // the bolt / rising percent), but back off to 5 s when discharging: the
    // full sweep below is ~4 I2C gauge register reads (9 removed by PB.14)
    // plus 2 PPM status reads, all under the instance mutex, and an idle battery
    // percent barely moves second-to-second. A freshly-plugged charger is
    // picked up within one 5 s tick.
    uint32_t ttl_ms = s_cached_monitor_params.is_charging ? 1000 : 5000;
    if (s_monitor_last_refresh != 0 && (millis() - s_monitor_last_refresh < ttl_ms)) {
        params = s_cached_monitor_params;
        return;
    }
    s_monitor_last_refresh = millis();

    params = monitor_params_t{};

#if defined(USING_PPM_MANAGE)
    // "Charging" means current is actually flowing into the cell. VBUS-in
    // alone over-reports — a plugged-in device that has hit termination is
    // not charging anymore. isChargeDone() drops to false during pre/fast
    // charge and rises to true at termination.
    params.is_charging = instance.ppm.isCharging() && !instance.ppm.isChargeDone();
#elif defined(USING_PMU_MANAGE)
    params.is_charging = instance.pmu.isCharging();
    params.battery_voltage = instance.pmu.getBattVoltage();
    params.battery_percent = instance.pmu.getBatteryPercent();
#endif

#ifdef USING_BQ_GAUGE
    if (hw_get_device_online() & HW_GAUGE_ONLINE) {
        instance.gauge.refresh();
        params.battery_percent = instance.gauge.getStateOfCharge();
        params.battery_voltage = instance.gauge.getVoltage();
        BatteryStatus batteryStatus = instance.gauge.getBatteryStatus();

        // Do NOT derive is_charging from the gauge alone — once the cell hits
        // termination current drops to ~0 and isInDischargeMode() reports
        // false, which would leave the lightning icon stuck at 100%. On the
        // pager the PPM (set above) is authoritative for charge state; we
        // only veto it here when the gauge confirms a full charge.
        if (batteryStatus.isFullChargeDetected()) {
            params.is_charging = false;
        }
    } else {
        // Gauge not online: Fallback to voltage-based percentage calculation
        params.battery_voltage = hw_get_battery_voltage();
        if (params.battery_voltage > 0) {
            // Simple linear mapping: 3200mV (0%) to 4200mV (100%)
            int16_t mv = params.battery_voltage;
            if (mv >= 4200) params.battery_percent = 100;
            else if (mv <= 3200) params.battery_percent = 0;
            else params.battery_percent = (mv - 3200) / 10;
        }
    }
#endif
    s_cached_monitor_params = params;
#else
    params.battery_percent = 30 + rand() % (100 - 30 + 1);
    params.battery_voltage = 4178;
    params.is_charging = true;
#endif
}

// --- Constants ---------------------------------------------------------

uint8_t hw_get_charge_level_nums()
{
    return dev_conts_var.charge_level_nums;
}
