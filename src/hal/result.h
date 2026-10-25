/**
 * @file      result.h
 * @brief     Typed HAL error codes + helpers.
 *
 * Many HAL entry points return `bool` with an out-param `std::string` error
 * message. That works but the call sites bake user-facing strings into the
 * HAL ("Cannot open SD file for writing."), which makes the same failure
 * look different from each path and forces the UI layer to grep for
 * substrings if it wants to react to a specific cause.
 *
 * This header introduces an `enum class HalError` that names the failure
 * categories the UI actually distinguishes. A function that wants typed
 * errors returns `HalResult<T>` or just `HalError` and the caller can
 * `if (err == HalError::StorageFull) ...` instead of substring-matching.
 *
 * Intentional non-goals:
 *  - Not a wholesale replacement for `bool` returns. The vast majority of
 *    HAL calls (toggles, "is enabled" queries, fire-and-forget writes)
 *    are fine as bool. Migrate the ones where the *reason* matters —
 *    storage, wireless, hub — and leave the rest alone.
 *  - Not `std::expected`. We target older C++ runtimes; a tiny tagged
 *    union is enough here.
 */
#pragma once

#include <cstdint>
#include <string>

// Only values something produces (or a test exercises) are listed; add new
// ones together with the first producer.
enum class HalError : uint8_t {
    Ok = 0,

    /* Generic */
    InvalidArgument,
    NotSupported,
    InternalError,

    /* Storage */
    StorageFull,       /* fs.write out of space (link/handlers_fs.cpp) */

    /* Wireless / network */
    WifiOffline,
    HttpError,         /* non-2xx response */
    Unauthorized,      /* 401/403 */

    /* Link */
    LinkOffline,       /* phone not connected (the *_link.cpp services) */

    /* Hub */
    HubDisabled,
    HubUnreachable,
};

/* Stable, English, UI-safe message for a HalError. Suitable for ui_msg_pop_up
 * subtitles when the caller doesn't have a more specific message in hand.
 * Returns a static string — do not free, do not modify. */
const char *hal_error_string(HalError e);

/* Value-or-error wrapper for the common `T value, std::string err` pattern.
 *
 * Usage (e.g. the *_link.cpp services):
 *   HalResult<size_t> r = some_hal_call(path, content);
 *   if (!r) {
 *       ui_msg_pop_up("Save", hal_error_string(r.error()));
 *       return;
 *   }
 *   log_i("wrote %u bytes", (unsigned)r.value());
 */
template <typename T>
class HalResult {
public:
    HalResult(T v) : ok_(true), v_(v), e_(HalError::Ok) {}
    HalResult(HalError e) : ok_(false), v_(), e_(e) {}

    explicit operator bool() const { return ok_; }
    bool is_ok() const  { return ok_; }
    bool is_err() const { return !ok_; }

    /* Pre: is_ok(). */
    const T &value() const { return v_; }
    T &value()             { return v_; }

    /* Pre: is_err(). Returns HalError::Ok if value is held. */
    HalError error() const { return e_; }

private:
    bool     ok_;
    T        v_;
    HalError e_;
};
