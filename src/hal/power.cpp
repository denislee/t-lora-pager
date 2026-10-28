/**
 * @file      power.cpp
 * @brief     Battery, charger, OTG, monitor.
 */
#include "../core/kb_lock.h"
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

// --- Charge limit (P4.5) ------------------------------------------------

#ifdef ARDUINO
static bool hw_get_charge_enable();
#endif

// Battery-life conservation: with charge_limit_en, stop charging at >= 80 %
// and resume below 75 %; without it the charger follows charger_enable.
// Driven once a minute (awake: charge_limit_timer_cb; fake sleep: the charge
// task) and right after the Settings charger toggles change (DC2.3.15).
void hw_apply_charge_limit()
{
#ifdef ARDUINO
    monitor_params_t params;
    hw_get_monitor_params(params);
    int16_t percent = params.battery_percent;
    if (percent >= 0 && percent <= 100) {
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
#endif
}

void charge_limit_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    hw_apply_charge_limit();
}

// --- BQ27220 targeted reads (P7.16) -------------------------------------
//
// SensorLib's GaugeBQ27220::refresh() reads 54 bytes from 0x02 plus 4 from
// 0x3A into a cached struct, and every getter reads that struct. The callers
// here use only Voltage (0x08), BatteryStatus (0x0A) and StateOfCharge (0x2C)
// (register map: SensorLib REG/BQ27220Constants.h BQ27220_REG_STA_*), so read
// just those: one 4-byte burst at 0x08 and one 2-byte read at 0x2C. The
// gauge's `comm` is protected, so this talks to Wire directly, framed the
// same way as SensorLib's SensorCommI2C::readRegister (register write with a
// STOP, then requestFrom). Caller must hold core::ScopedI2cLock.
#if defined(ARDUINO) && defined(USING_BQ_GAUGE)
static bool gauge_read(uint8_t reg, uint8_t *buf, uint8_t len)
{
    Wire.beginTransmission(BQ27220_SLAVE_ADDRESS);
    Wire.write(reg);
    if (Wire.endTransmission(true) != 0) return false;
    if (Wire.requestFrom(BQ27220_SLAVE_ADDRESS, len) != len) return false;
    return Wire.readBytes(buf, len) == len;
}

static inline uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }

// SensorLib declares the register constants protected; re-export the two we
// need rather than restating their values.
struct GaugeReg : BQ27220Constants {
    using BQ27220Constants::BQ27220_REG_STA_BAT_VOLTAGE;      // 0x08
    using BQ27220Constants::BQ27220_REG_STA_STATE_OF_CHARGE;  // 0x2C
};

// Last good values. A failed read keeps them, as a failed refresh() left
// SensorLib's struct unchanged.
static uint16_t s_gauge_voltage = 0;
static uint16_t s_gauge_status  = 0;
static uint16_t s_gauge_soc     = 0;

static void gauge_read_targeted()
{
    uint8_t b[4];
    if (gauge_read(GaugeReg::BQ27220_REG_STA_BAT_VOLTAGE, b, 4)) {
        s_gauge_voltage = le16(b);       // 0x08-0x09 mV
        s_gauge_status  = le16(b + 2);   // 0x0A-0x0B BatteryStatus bits
    }
    if (gauge_read(GaugeReg::BQ27220_REG_STA_STATE_OF_CHARGE, b, 2)) {
        s_gauge_soc = le16(b);           // 0x2C-0x2D %
    }
}
#endif

// --- Battery voltage ---------------------------------------------------

int16_t hw_get_battery_voltage()
{
#ifdef ARDUINO

#if  defined(USING_BQ_GAUGE)
    if (HW_GAUGE_ONLINE & hw_get_device_online()) {
        core::ScopedI2cLock i2c;
        uint8_t b[2];
        if (gauge_read(GaugeReg::BQ27220_REG_STA_BAT_VOLTAGE, b, 2)) {
            s_gauge_voltage = le16(b);
        }
        return s_gauge_voltage;
    } else {
        log_d("Gauge Not online !");
        return 0;
    }
#elif defined(USING_PMU_MANAGE)
    core::ScopedI2cLock i2c;  // P7.45
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
    core::ScopedI2cLock i2c;  // P7.45
    return  instance.ppm.isEnableOTG();
#else
    return false;
#endif
}

bool hw_set_otg(bool enable)
{
#if defined(ARDUINO) && defined(USING_PPM_MANAGE)
    core::ScopedI2cLock i2c;  // P7.45
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

#ifdef ARDUINO
// Only hw_apply_charge_limit() asks (DC2.3.31).
static bool hw_get_charge_enable()
{
#if defined(USING_PPM_MANAGE)
    core::ScopedI2cLock i2c;  // P7.45
    return  instance.ppm.isEnableCharge();
#elif defined(USING_PMU_MANAGE)
    core::ScopedI2cLock i2c;  // P7.45
    return  instance.isEnableCharge();
#else
    return false;
#endif
}
#endif

void hw_set_charger(bool enable)
{
#ifdef ARDUINO
    core::ScopedI2cLock i2c;  // P7.45
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
    core::ScopedI2cLock i2c;  // P7.45
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
    uint16_t cur;
    {
        core::ScopedI2cLock i2c;  // P7.45
        cur = instance.getChargeCurrent();
    }
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
    {
        core::ScopedI2cLock i2c;  // P7.45
        instance.ppm.setChargerConstantCurr(level * dev_conts_var.charge_steps);
    }
    return  level * dev_conts_var.charge_steps;
#elif defined(USING_PMU_MANAGE) || !defined(ARDUINO)
    if (level > kChargeCurrentLevels - 1) level = kChargeCurrentLevels - 1;
    log_d("set charge current:%u mA", kChargeCurrentTable[level]);
#if defined(ARDUINO)
    {
        core::ScopedI2cLock i2c;  // P7.45
        instance.setChargeCurrent(kChargeCurrentTable[level]);
    }
#endif
    return kChargeCurrentTable[level];
#endif
}

// Milliamps a Settings level maps to, without touching the hardware.
static uint16_t charge_current_for_level(uint8_t level)
{
#if defined(ARDUINO) && defined(USING_PPM_MANAGE)
    return level * dev_conts_var.charge_steps;
#else
    if (level > kChargeCurrentLevels - 1) level = kChargeCurrentLevels - 1;
    return kChargeCurrentTable[level];
#endif
}

void hw_charge_current_options(std::vector<uint16_t> &ma)
{
    ma.clear();
    uint8_t nums = hw_get_charge_level_nums();
    for (uint8_t level = 1; level <= nums; ++level) {
        uint16_t value = charge_current_for_level(level);
        if (!ma.empty() && ma.back() == value) continue;  // no duplicates
        ma.push_back(value);
    }
}

uint8_t hw_charge_level_for_ma(uint16_t ma)
{
    uint8_t nums = hw_get_charge_level_nums();
    for (uint8_t level = 1; level <= nums; ++level) {
        if (charge_current_for_level(level) == ma) return level;
    }
    return 0;
}

// --- Monitor / power params -------------------------------------------

#ifdef ARDUINO
static monitor_params_t s_cached_monitor_params;
static uint32_t s_monitor_last_refresh = 0;
#endif

// On the pager none of these reads use the BQ25896 ADC (charge status is a
// status-register bit; voltage and percent come from the fuel gauge), which
// hw_init() turns off for good (DC2.3.20).
void hw_get_monitor_params(monitor_params_t &params)
{
#ifdef ARDUINO

    // Refresh at most once per second while charging (the status bar animates
    // the bolt / rising percent), but back off to 5 s when discharging: the
    // sweep below is 2 targeted gauge reads (6 bytes, P7.16) plus 2 PPM
    // status reads, under the bus lock, and an idle battery
    // percent barely moves second-to-second. A freshly-plugged charger is
    // picked up within one 5 s tick.
    uint32_t ttl_ms = s_cached_monitor_params.is_charging ? 1000 : 5000;
    if (s_monitor_last_refresh != 0 && (millis() - s_monitor_last_refresh < ttl_ms)) {
        params = s_cached_monitor_params;
        return;
    }
    s_monitor_last_refresh = millis();

    // The keyboard reader polls the TCA8418 on the same Wire bus without the
    // instance mutex (IS.1), and the link task calls this without it too.
    // Wire only locks inside each requestFrom(); the bytes are then read by
    // its shared rxBuffer unlocked, so a kb_reader poll landing in that window
    // made the gauge read come up short and readBytes() block for its full
    // timeout (measured: 1012 ms, ~1 in 20 link-task reads). Serialise the
    // whole sweep with the I2C lock (order instance -> i2c is allowed);
    // this also guards s_cached_monitor_params against concurrent callers.
    core::ScopedI2cLock kb_lock;

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
        gauge_read_targeted();  // P7.16: 6 bytes, was refresh()'s 54 + 4
        params.battery_percent = s_gauge_soc;
        params.battery_voltage = s_gauge_voltage;
        BatteryStatus batteryStatus(s_gauge_status);

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
