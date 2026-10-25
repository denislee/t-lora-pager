/**
 * @file      peripherals.h
 * @brief     NFC discovery and the NFC enable toggle.
 */
#pragma once

#include "types.h"

// --- NFC ---
bool hw_get_nfc_enable();
void hw_set_nfc_enable(bool en);
bool hw_start_nfc_discovery();

/* True iff the last hw_start_nfc_discovery() call succeeded and we haven't
 * since stopped. Distinguishes "chip powered" from "RFAL polling live". */
bool hw_nfc_discovery_active();

/* Diagnostic hook invoked on every NFC event, from the nfc_reader task
 * (hal/nfc_task.cpp) with the instance lock held.
 * `kind` values:
 *   0 = raw detection (before NDEF parse)
 *   1 = RTD Text
 *   2 = RTD URI
 *   3 = Media WiFi
 *   4 = Other / unhandled NDEF type
 * `text` / `len` carry a short preview payload or NULL/0. Pass NULL to clear. */
typedef void (*nfc_diag_cb_t)(int kind, const char *text, unsigned len);
void hw_set_nfc_diag_hook(nfc_diag_cb_t cb);
