/**
 * @file      settings.h
 * @brief     The device settings registry: every user setting, bound to its
 *            store and apply step (IOS_SETTINGS_PLAN.md §4.1).
 *
 * Both the pager's Settings app and the PagerLink settings.* handlers write
 * through here, so a value is validated, stored field by field and applied the
 * same way whichever UI changed it. New settings go through this registry,
 * never straight to NVS.
 *
 * The catalog (ids, types, ranges, board gates) is hal/settings_catalog.cpp;
 * the HAL bindings live in src/settings_table.cpp (it reaches into apps and
 * the theme, so it sits beside the other shared UI glue in src/).
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "settings_registry.h"

namespace settings {

/** The device registry, built and bound on first use. Any thread. */
Registry &registry();

/**
 * Validate, store and apply one value. `msg` gets the error message. With
 * `apply` false the value is stored (and observers told) but the deferred
 * hardware/theme apply is dropped: for a pager control that has already
 * applied the change itself. A setter that applies synchronously (power.otg)
 * still does.
 */
bool set(const char *id, const Value &v, std::string *msg = nullptr, bool apply = true);
inline bool set_bool(const char *id, bool v) { return set(id, Value::of_bool(v)); }
inline bool set_int(const char *id, int64_t v) { return set(id, Value::of_int(v)); }
inline bool set_str(const char *id, const std::string &v) { return set(id, Value::of_str(v)); }

/** Read one value (a default-constructed Value for an unknown id). */
Value get(const char *id);

/**
 * Hardware applies are run on the LVGL thread (core::ui_post), after the
 * value is stored. While a sink is installed on this thread they are queued
 * into it instead, so the link handler can post them only after its reply has
 * been sent (conn.bt=false must not cut the link before the answer, T3).
 */
void set_apply_sink(std::vector<std::function<void()>> *sink);

/** A store changed outside the registry (the Wi-Fi list after a connect). */
void note_changed(const char *id);

}  // namespace settings
