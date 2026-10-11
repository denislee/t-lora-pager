/**
 * @file      scoped_lock.h
 * @brief     RAII wrapper around instanceLockTake / instanceLockGive.
 *
 * The instance mutex is the project's top-level task-coordination lock
 * (LVGL task, NFC task, keyboard task, main loop). It is NOT the SPI-bus
 * lock — that is the vendor's lockSPI(), wrapped by core/spi_lock.h. Every
 * non-trivial return path must release it, so prefer the RAII
 * `ScopedInstanceLock` over manual take/give pairs.
 */
#pragma once

void instanceLockTake();
void instanceLockGive();

namespace core {

class ScopedInstanceLock {
public:
    ScopedInstanceLock() { instanceLockTake(); }
    ~ScopedInstanceLock() { instanceLockGive(); }
    ScopedInstanceLock(const ScopedInstanceLock &) = delete;
    ScopedInstanceLock &operator=(const ScopedInstanceLock &) = delete;
    ScopedInstanceLock(ScopedInstanceLock &&) = delete;
    ScopedInstanceLock &operator=(ScopedInstanceLock &&) = delete;
};

} // namespace core
