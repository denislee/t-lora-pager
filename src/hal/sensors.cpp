/**
 * @file      sensors.cpp
 * @brief     GPS power toggle, IMU.
 */
#include "sensors.h"
#include "board_config.h"  // USING_BHI260_SENSOR / USING_BMA423_SENSOR (DC2.0.1)
#include "system.h"
#include "internal.h"
#include "../core/kb_lock.h"

#ifdef ARDUINO
#include <LilyGoLib.h>
#endif

// Pager only, not USING_BHI260_SENSOR: the Ultra's begin() still brings the
// BHI260 up itself (it needs the sensor's GPIO expander early).
#if defined(ARDUINO) && defined(ARDUINO_T_LORA_PAGER)
#include <mutex>
// P6.2 / BT.2: lazy BHI260 bring-up. The mutex is a leaf lock (initSensor()
// takes no other project lock), so it cannot deadlock with the instance lock
// the LVGL-task callers already hold. One attempt per boot, like the vendor
// begin() did; the IMU debug page's "Try sensor.begin" re-probes by hand.
static std::mutex s_imu_start_mtx;
static bool s_imu_start_attempted = false;
#endif

void hw_imu_ensure_started()
{
#if defined(ARDUINO) && defined(ARDUINO_T_LORA_PAGER)
    std::lock_guard<std::mutex> guard(s_imu_start_mtx);
    if (s_imu_start_attempted) return;
    s_imu_start_attempted = true;
    uint32_t t0 = millis();
    bool ok;
    {
        core::ScopedI2cLock i2c;  // P7.45
        ok = instance.initSensor();
    }
    log_i("BHI260 lazy init %s in %lu ms", ok ? "ok" : "FAILED", (unsigned long)(millis() - t0));
    (void)t0; (void)ok;  // log_i compiles out at CORE_DEBUG_LEVEL=0
#endif
}

// --- GPS ---------------------------------------------------------------
// Only the power-rail toggle survives. The NMEA decode + PPS interrupt path
// was removed because no app consumes GPS data. There is no persisted
// enable setting either (see OPTIMIZATION_PHASE3.md P2.5): with nothing to
// continuously consume a fix, latching the rail on 24/7 only burned tens of
// mA for nothing. hw_start_time_sync_gps() (gps_time_sync.cpp) is now the
// sole owner of GPS power, asserting it only transiently for the duration
// of a sync attempt; this just drives the rail and the UART behind it.
void hw_set_gps_powered(bool on) {
#ifdef ARDUINO
    {
        core::ScopedI2cLock i2c;  // P7.45
        instance.powerControl(POWER_GPS, on);
    }
    delay(10);
    if (!on) {
        Serial1.end();
    } else {
        Serial1.begin(38400, SERIAL_8N1, GPS_RX, GPS_TX);
    }
#else
    (void)on;
#endif
}

// --- IMU ---------------------------------------------------------------

// Fed by the BHI260 callbacks; the emulator just reports the zeroed state.
#if !defined(ARDUINO) || defined(USING_BHI260_SENSOR)
static imu_params_t imu_params = {0, 0, 0, 0};
#endif

void hw_get_imu_params(imu_params_t &params)
{
#ifdef ARDUINO
#if defined(USING_BHI260_SENSOR)
    if (hw_get_device_online() & HW_BHI260AP_ONLINE) {
        params =  imu_params;
    }
#elif defined(USING_BMA423_SENSOR)
    if (hw_get_device_online() & HW_BMA423_ONLINE) {
        core::ScopedI2cLock i2c;  // P7.45
        params.orientation = instance.sensor.direction();
    }
#endif // SENSOR
#else
    params =  imu_params;
#endif //ARDUINO
}

// Diagnostic counters surfaced via hw_get_imu_diag(). Updated from the BHI260
// callbacks (which run from the LVGL task under the instance lock) and from
// hw_register_imu_process() at boot. Defined before hw_is_face_down() so the
// face-down logic can read the live accel/quaternion state.
static imu_diag_t s_imu_diag = {};

#if  defined(ARDUINO) && defined(USING_BHI260_SENSOR)
static void imu_data_process(uint8_t sensor_id, uint8_t *data_ptr, uint32_t len, uint64_t *timestamp, void *user_data)
{
    float roll, pitch, yaw;
    bhy2_quaternion_to_euler(data_ptr, &roll,  &pitch, &yaw);
    imu_params.roll = roll;
    imu_params.pitch = pitch;
    imu_params.heading = yaw;
    s_imu_diag.grv_events++;
}

// DEVICE_ORIENTATION delivers a single byte enum (0=portrait-up, 1=landscape-
// left, 2=portrait-down, 3=landscape-right per BHY2 reference). We surface the
// raw value so the debug overlay can show which mapping the loaded firmware
// actually uses — boards remap axes, so the spec value isn't always the same.
static void imu_dev_orient_process(uint8_t sensor_id, uint8_t *data_ptr, uint32_t len, uint64_t *timestamp, void *user_data)
{
    if (data_ptr && len >= 1) {
        s_imu_diag.dev_orient_value = data_ptr[0];
        imu_params.orientation = data_ptr[0];
    }
    s_imu_diag.dev_orient_events++;
}

// ACCEL_PASSTHROUGH is the lowest-level virtual sensor on the BHI260 — it's
// in every firmware variant including the GPIO-only build the LoRa Pager
// uses, which is why we lean on it as the primary face-down signal. Data is
// three int16 g-units that getScaling() converts to actual g.
static void imu_accel_process(uint8_t sensor_id, uint8_t *data_ptr, uint32_t len, uint64_t *timestamp, void *user_data)
{
    if (!data_ptr || len < 6) {
        s_imu_diag.accel_events++;
        return;
    }
    bhy2_data_xyz raw{};
    bhy2_parse_xyz(data_ptr, &raw);
    float scale = instance.sensor.getScaling(sensor_id);
    s_imu_diag.accel_g_x = raw.x * scale;
    s_imu_diag.accel_g_y = raw.y * scale;
    s_imu_diag.accel_g_z = raw.z * scale;
    s_imu_diag.accel_events++;
}
#endif //ARDUINO

bool hw_is_face_down()
{
#ifdef ARDUINO
#if defined(USING_BMA423_SENSOR)
    if (hw_get_device_online() & HW_BMA423_ONLINE) {
        core::ScopedI2cLock i2c;  // P7.45
        return instance.sensor.direction() == SensorBMA423::DIRECTION_BOTTOM;
    }
#elif defined(USING_BHI260_SENSOR)
    if (hw_get_device_online() & HW_BHI260AP_ONLINE) {
        // Prefer raw accelerometer when ACCEL_PASSTHROUGH is flowing — it's
        // the lowest-level virtual sensor and works regardless of which
        // firmware variant is loaded. Face-down ≈ gravity points up out of
        // the screen, so az ≈ -1 g (sign depends on the LilyGoLib axis
        // remap applied at boot).
        if (s_imu_diag.accel_events > 0) {
            float ax = s_imu_diag.accel_g_x;
            float ay = s_imu_diag.accel_g_y;
            float az = s_imu_diag.accel_g_z;
            float mag2 = ax * ax + ay * ay + az * az;
            return mag2 > 0.7f && mag2 < 1.5f && az < -0.6f;
        }
        if (s_imu_diag.grv_events > 0) {
            float roll = imu_params.roll;
            float pitch = imu_params.pitch;
            if (roll < 0) roll = -roll;
            if (pitch < 0) pitch = -pitch;
            return roll > 150.0f && pitch < 30.0f;
        }
        if (s_imu_diag.dev_orient_events > 0) {
            return s_imu_diag.dev_orient_value == 2;
        }
    }
#endif
#endif
    return false;
}

// Tracks whether the three BHI260 virtual sensors are currently configured.
// Guards against double-register and against unregistering what was never
// registered (P4.22).
static bool s_imu_registered = false;

bool hw_imu_is_registered() { return s_imu_registered; }

void hw_register_imu_process()
{
    if (s_imu_registered) return; // guard: already registered
    hw_imu_ensure_started(); // P6.2: first IMU use uploads the BHI260 firmware
#if defined(ARDUINO)
#if defined(USING_BHI260_SENSOR)
    s_imu_diag.bhi260_online = (hw_get_device_online() & HW_BHI260AP_ONLINE) != 0;
    if (s_imu_diag.bhi260_online) {
        core::ScopedI2cLock i2c;  // P7.45 (after hw_imu_ensure_started, which takes its own mutex first)
        // Power: the only consumers of the accel + game-rotation streams are
        // hw_is_face_down() (a slow physical gesture) and the IMU debug page,
        // which reads the cached values at 5 Hz — nothing needs 100 Hz. Run the
        // BHI260 at 25 Hz and let it batch ~200 ms of samples in its own FIFO
        // before interrupting the host: that collapses ~200 host I2C wakeups/s
        // (100 Hz x accel+GRV) to ~10/s while keeping face-down latency well
        // under a frame. A future high-rate consumer (e.g. tilt-to-wake) should
        // raise these back up. DEVICE_ORIENTATION already runs at 5 Hz below.
        float sample_rate = 25.0f;
        uint32_t report_latency_ms = 200;

        // Force-configure all three regardless of what the info table says
        // — the GPIO firmware variant on the Pager has been observed to
        // misreport availability. configure() returns false harmlessly when
        // the firmware genuinely lacks the sensor, so this is safe.
        s_imu_diag.accel_configured = instance.sensor.configure(
            SensorBHI260AP::ACCEL_PASSTHROUGH, sample_rate, report_latency_ms);
        if (s_imu_diag.accel_configured) {
            instance.sensor.onResultEvent(
                SensorBHI260AP::ACCEL_PASSTHROUGH, imu_accel_process);
        }

        s_imu_diag.grv_configured = instance.sensor.configure(
            SensorBHI260AP::GAME_ROTATION_VECTOR, sample_rate,
            report_latency_ms);
        if (s_imu_diag.grv_configured) {
            instance.sensor.onResultEvent(
                SensorBHI260AP::GAME_ROTATION_VECTOR, imu_data_process);
        }

        s_imu_diag.dev_orient_configured = instance.sensor.configure(
            SensorBHI260AP::DEVICE_ORIENTATION, 5.0f, report_latency_ms);
        if (s_imu_diag.dev_orient_configured) {
            instance.sensor.onResultEvent(
                SensorBHI260AP::DEVICE_ORIENTATION, imu_dev_orient_process);
        }
    }
#elif defined(USING_BMA423_SENSOR)
    s_imu_diag.bma423_online = (hw_get_device_online() & HW_BMA423_ONLINE) != 0;
    if (s_imu_diag.bma423_online) {
        core::ScopedI2cLock i2c;  // P7.45 (after hw_imu_ensure_started, which takes its own mutex first)
        instance.sensor.configAccelerometer();
        instance.sensor.enableAccelerometer();
    }
#endif // SENSOR
#endif // ARDUINO
    s_imu_registered = true;
}

// hw_probe_imu_info() — heavy diagnostic path (P4.6).
// Dumps the BHI260 firmware's virtual-sensor table to Serial and counts
// available sensors into s_imu_diag.sensor_count. Separated from
// hw_register_imu_process() so it only runs when the IMU debug page
// explicitly requests it, not on every wake.
void hw_probe_imu_info()
{
#if defined(ARDUINO) && defined(USING_BHI260_SENSOR)
    if (!(hw_get_device_online() & HW_BHI260AP_ONLINE)) return;
    core::ScopedI2cLock i2c;  // P7.45
    BoschSensorInfo info = instance.sensor.getSensorInfo();
    // Pinned SensorLib version lacks getAvailableSensorCount(), so walk
    // the BHY2 sensor table directly via the C helper to count what the
    // firmware actually exposes.
    uint16_t count = 0;
    if (info.dev) {
        for (uint8_t id = 1; id < BHY2_SENSOR_ID_MAX; ++id) {
            if (bhy2_is_sensor_available(id, info.dev)) count++;
        }
    }
    s_imu_diag.sensor_count = count;
    log_v("BHI260 probe: %u virtual sensors available", (unsigned)count);
#endif
}

void hw_get_imu_diag(imu_diag_t &out)
{
    out = s_imu_diag;
}

void hw_unregister_imu_process()
{
    if (!s_imu_registered) return; // guard: nothing registered, nothing to undo
#if defined(ARDUINO)
#if defined(USING_BHI260_SENSOR)
    if (hw_get_device_online() & HW_BHI260AP_ONLINE) {
        core::ScopedI2cLock i2c;  // P7.45
        // Disable all three virtual sensors that hw_register_imu_process() enables —
        // they must mirror each other exactly.  Previously only GAME_ROTATION_VECTOR
        // was disabled here, leaving ACCEL_PASSTHROUGH and DEVICE_ORIENTATION running.
        instance.sensor.configure(SensorBHI260AP::ACCEL_PASSTHROUGH, 0, 0);
        instance.sensor.configure(SensorBHI260AP::GAME_ROTATION_VECTOR, 0, 0);
        instance.sensor.configure(SensorBHI260AP::DEVICE_ORIENTATION, 0, 0);
    }
#elif defined(USING_BMA423_SENSOR)
    if (hw_get_device_online() & HW_BMA423_ONLINE) {
        core::ScopedI2cLock i2c;  // P7.45
        instance.sensor.disableAccelerometer();
    }
#endif // SENSOR
#endif // ARDUINO
    s_imu_registered = false;
}
