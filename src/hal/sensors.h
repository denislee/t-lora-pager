/**
 * @file      sensors.h
 * @brief     GPS power toggle, IMU.
 */
#pragma once

#include "types.h"

// --- GPS ---
// The GPS module is wired to power-rail control + a UART. No app consumes
// continuous GPS data (the parsing/PPS plumbing has been dropped), so there
// is no persisted on/off setting — this just drives the power rail for
// hw_start_time_sync_gps()'s transient use (see hal/gps_time_sync.cpp).
void hw_set_gps_powered(bool on);

// --- IMU ---
// Register/unregister the three BHI260 virtual sensors (or BMA423 accel).
// hw_register_imu_process() is idempotent — double-calls are safe.
// hw_unregister_imu_process() is idempotent — calling when nothing is
// registered is a no-op. Both update the shared s_imu_registered flag.
// P6.2 / BT.2: the BHI260AP firmware (123.7 KB over I2C) is no longer uploaded
// at boot on the pager (factory.ino passes NO_HW_SENSOR). hw_imu_ensure_started()
// runs instance.initSensor() once, on first IMU use; hw_register_imu_process()
// calls it, so every consumer goes through it. Safe against concurrent first
// use (own mutex). Callers already hold the instance lock (LVGL task), as the
// IMU paths always did. No-op on the watches and the emulator.
void hw_imu_ensure_started();
void hw_register_imu_process();
void hw_unregister_imu_process();

// Returns true if the IMU pipeline is currently registered (i.e. sensors are
// configured and streaming). Used by system.cpp to decide whether to
// suspend/resume the pipeline across fake sleep cycles.
bool hw_imu_is_registered();

// Heavy diagnostic probe (P4.6): dumps the BHI260 firmware's virtual-sensor
// table and updates imu_diag_t::sensor_count. Call lazily from the IMU debug
// page, not on every wake — this walks all 255 BHY2 sensor IDs.
void hw_probe_imu_info();
void hw_get_imu_params(imu_params_t &params);

// True if the device is lying roughly flat with the screen pointing down
// (e.g. set face-down on a table). Always false on the emulator.
bool hw_is_face_down();

// IMU diagnostic snapshot. Lets the status-bar debug overlay (and any future
// caller) see what the IMU stack reports without having to know which sensor
// IC is on the board. All counters/booleans are 0 / false on the emulator.
struct imu_diag_t {
    bool     bhi260_online;        // BHI260AP detected at boot
    bool     bma423_online;        // BMA423 detected at boot
    uint16_t sensor_count;         // virtual sensors the firmware reports as present
    bool     grv_configured;       // configure(GAME_ROTATION_VECTOR, ...) returned true
    bool     dev_orient_configured;// configure(DEVICE_ORIENTATION, ...) returned true
    bool     accel_configured;     // configure(ACCEL_PASSTHROUGH, ...) returned true
    uint32_t grv_events;           // GAME_ROTATION_VECTOR callback fire count
    uint32_t dev_orient_events;    // DEVICE_ORIENTATION callback fire count
    uint32_t accel_events;         // ACCEL_PASSTHROUGH callback fire count
    uint8_t  dev_orient_value;     // last DEVICE_ORIENTATION byte received
    float    accel_g_x;            // last accel reading in g (≈ ±1.0 at rest)
    float    accel_g_y;
    float    accel_g_z;
};

void hw_get_imu_diag(imu_diag_t &out);
