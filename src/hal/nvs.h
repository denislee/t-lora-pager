#pragma once

// Single-key NVS (Arduino Preferences) accessors. The begin/get/put/end dance
// — plus its `#ifdef ARDUINO` / emulator twin — used to be copy-pasted as
// `load_pref`/`save_pref` triplets in ui_telegram.cpp and ui_notes_sync.cpp
// (OPTIMIZATION_REPORT §2.18). Consolidated here so each app keeps only a thin
// namespace-binding wrapper. Multi-key configs (ui_ssh, ui_weather) deliberately
// keep their own batched begin/end — folding them into single-key calls would
// multiply NVS open/close cycles.
//
// Lives in HAL because it wraps a hardware/platform facility; on the desktop
// emulator the entries persist as files under $PAGERLINK_NVS_DIR (default
// $PAGERLINK_FS_ROOT/nvs, itself defaulting to /tmp/pagerlink-fs/nvs), one
// file per <namespace>/<key>, so settings survive an emulator restart.
//
// The emulator layout is intentionally dumb (raw bytes, decimal text for the
// integer/bool kinds) — it only has to round-trip what the apps write.

#include <cstddef>
#include <cstdint>
#include <string>

namespace hal {

// Read a string entry from `ns`/`key`, returning `dflt` when absent (or when
// the entry cannot be read). Opens the namespace read-only.
std::string nvs_get_str(const char *ns, const char *key, const char *dflt = "");

// Write a string entry. An empty/null `value` removes the key (matching the app
// configs' "empty means unset" convention).
void nvs_set_str(const char *ns, const char *key, const char *value);

// Write a string entry literally — an empty value is stored (as an empty entry)
// instead of removing the key. Needed where "present but empty" carries meaning
// (the weather city's "auto" sentinel, for instance).
void nvs_put_str(const char *ns, const char *key, const std::string &value);

// Read/write a bool entry, same namespace semantics as the string variants.
bool nvs_get_bool(const char *ns, const char *key, bool dflt);
void nvs_set_bool(const char *ns, const char *key, bool value);

// Read/write a uint16_t entry (stored as decimal text on the emulator).
uint16_t nvs_get_u16(const char *ns, const char *key, uint16_t dflt);
void nvs_set_u16(const char *ns, const char *key, uint16_t value);

// Read a binary entry into `out`. Returns true when the entry is present (the
// data is then copied verbatim, and `out` may legitimately be empty); false —
// leaving `out` untouched — when it is absent.
bool nvs_get_blob(const char *ns, const char *key, std::string &out);

// Write a binary entry. `len == 0` removes the key.
void nvs_set_blob(const char *ns, const char *key, const void *data, size_t len);

// True when the entry exists at all, regardless of its contents.
bool nvs_has(const char *ns, const char *key);

} // namespace hal
