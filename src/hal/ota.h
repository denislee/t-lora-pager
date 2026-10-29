/**
 * @file      ota.h
 * @brief     Firmware update slots and rollback (PROTOCOL.md §6.7).
 *
 * Hardware: the inactive app0/app1 slot of partitions.csv (4 MB each) through
 * esp_ota_*, and the pending/boot-count bookkeeping of hal/ota_state.h in NVS
 * namespace "ota". Emulator: the "slot" is the file
 * $PAGERLINK_FS_ROOT/ota/installed.bin and nothing reboots, so
 * `pagerlink smoke` can drive ota.apply end to end.
 *
 * The slot writer is single-use and single-caller: handlers_ota.cpp runs one
 * apply job at a time on its own worker.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct HwOtaStatus {
    std::string running;  // "app0" / "app1" ("emu" on the emulator)
    std::string next;     // the slot the next ota.apply writes
    bool pending = false;  // running image applied by OTA and not yet confirmed
    uint8_t boots = 0;
    uint32_t max_size = 0;  // bytes the next slot holds
};

/** First thing in setup(): count a pending boot, roll back after
 * hal::kOtaMaxBoots, and arm the self-confirm after hal::kOtaConfirmMs. */
void hw_ota_boot_check();

HwOtaStatus hw_ota_status();

/** Start writing an image of `size` bytes to the next slot. */
bool hw_ota_slot_begin(uint32_t size, std::string &err);
/** Append the next chunk (in order). */
bool hw_ota_slot_write(const uint8_t *data, size_t len, std::string &err);
/** Close the slot (the IDF validates the image), make it the boot slot and
 * mark it pending. */
bool hw_ota_slot_finish(std::string &err);
/** Drop a slot write that will not be finished. Safe when none is open. */
void hw_ota_slot_abort();

/** Restart into the new image (emulator: no-op). */
void hw_ota_reboot();
