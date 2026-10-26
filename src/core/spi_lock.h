/**
 * @file      spi_lock.h
 * @brief     RAII wrappers around the vendor's `instance.lockSPI()` /
 *            `instance.unlockSPI()` pair.
 *
 * Distinct from `core::ScopedInstanceLock` (scoped_lock.h):
 *  - `ScopedInstanceLock`  → our own top-level mutex, coordinating the LVGL
 *                            task, NFC task, link task and main loop (the
 *                            keyboard reader takes the I2C lock instead, IS.1).
 *  - `ScopedSpiLock`       → the vendor-internal SPI-bus lock. The vendor
 *                            uses it to serialize display / radio / SD
 *                            transactions on the shared SPI. On T-Watch-S3
 *                            it's a no-op; on T-LoRa-Pager and T-Watch-Ultra
 *                            it's a real mutex.
 *
 * Two flavors:
 *  - `ScopedSpiLock`   → always locks in ctor, always unlocks in dtor.
 *  - `MaybeSpiLock`    → conditional. Either ctor-arg or later `acquire()`
 *                        locks; dtor unlocks only if held. Replaces the
 *                        `bool lock = false; if (...) lockSPI()` patterns
 *                        scattered through hal/storage.cpp.
 *
 * Declared here without including LilyGoLib so the header is cheap to
 * include from HAL and UI TUs; the implementation lives in spi_lock.cpp.
 */
#pragma once

namespace core {

class ScopedSpiLock {
public:
    ScopedSpiLock();
    ~ScopedSpiLock();
    ScopedSpiLock(const ScopedSpiLock &) = delete;
    ScopedSpiLock &operator=(const ScopedSpiLock &) = delete;
    ScopedSpiLock(ScopedSpiLock &&) = delete;
    ScopedSpiLock &operator=(ScopedSpiLock &&) = delete;
};

/* Conditional RAII SPI lock. Construct unarmed (default) or pre-armed
 * (`MaybeSpiLock m(true)`); call `acquire()` later to arm. Dtor unlocks
 * only if currently held. Safe to call `acquire()` / `release()`
 * idempotently. */
class MaybeSpiLock {
public:
    MaybeSpiLock();
    explicit MaybeSpiLock(bool acquire_now);
    ~MaybeSpiLock();
    void acquire();
    void release();
    bool held() const { return held_; }
    MaybeSpiLock(const MaybeSpiLock &) = delete;
    MaybeSpiLock &operator=(const MaybeSpiLock &) = delete;
    MaybeSpiLock(MaybeSpiLock &&) = delete;
    MaybeSpiLock &operator=(MaybeSpiLock &&) = delete;
private:
    bool held_;
};

} // namespace core
