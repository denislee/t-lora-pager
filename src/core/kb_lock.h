/**
 * @file      kb_lock.h
 * @brief     RAII wrapper around the bus-wide I2C (Wire) mutex (kb_lock.cpp).
 *
 * P7.45: this started as a keyboard-only lock (INPUT_STUTTER_PLAN.md IS.1)
 * and is now the lock for EVERY user of the shared Wire bus: the TCA8418
 * keyboard (`instance.kb`), the XL9555 expander behind `instance.powerControl()`
 * and `instance.io`, the BQ25896 charger (`instance.ppm`) / AXP PMU, the
 * BQ27220 gauge, the RTC, the audio codec's control port, and the IMU.
 * The file keeps its old name so its includers did not have to change.
 *
 * Why every user needs it, writes included: arduino-esp32's HAL lock covers
 * each Wire transaction, but a read's bytes are then taken from Wire's shared
 * rxBuffer with no lock held. A transaction from another task landing between
 * requestFrom() and read() leaves that read short; readBytes() then waits out
 * its timeout (20 ms, hw_init) and SensorLib's readRegister() returns -1.
 * SensorLib's setRegisterBit()/clrRegisterBit() store that -1 in a uint8_t
 * unchecked and write it back, so a colliding XL9555 digitalWrite() drives
 * every other output on the port high (AMP, LoRa, GPS, NFC rails). Read-
 * modify-write writes are therefore NOT safe without this lock.
 *
 * The keyboard reader (kb_reader, priority MAX-2) still keeps reading during a
 * long LVGL frame: the LVGL task holds the instance mutex for a whole frame,
 * but takes this lock only around its own short I2C calls.
 *
 * The underlying mutex is RECURSIVE: hw_enable_keyboard() holds the lock and
 * then calls hw_set_kb_backlight(), which takes it again; hw_low_power_loop()
 * holds it across the vendor lightSleep()/sleep(), which call powerControl()
 * and kb.end()/begin().
 *
 * Lock order: `core::ScopedInstanceLock` -> `core::ScopedSpiLock` -> this
 * lock. A caller may hold either of the others and then take this one; never
 * the reverse. Nothing under this lock may take the instance lock or the SPI
 * lock, or wait on a task that might (keep the scope to the I2C calls).
 *
 * Declared here without pulling in FreeRTOS so the header is cheap to include
 * from HAL and UI TUs; the implementation lives in kb_lock.cpp.
 */
#pragma once

namespace core {

// Create the bus mutex. Call once from setup() before any task starts.
// Idempotent; safe to call again.
void kb_lock_init();

class ScopedI2cLock {
public:
    ScopedI2cLock();
    ~ScopedI2cLock();
    ScopedI2cLock(const ScopedI2cLock &) = delete;
    ScopedI2cLock &operator=(const ScopedI2cLock &) = delete;
    ScopedI2cLock(ScopedI2cLock &&) = delete;
    ScopedI2cLock &operator=(ScopedI2cLock &&) = delete;
};

// Pre-P7.45 name; the same lock.

} // namespace core
