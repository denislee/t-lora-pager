/**
 * @file      ui/modals.h
 * @brief     Shared modal helpers (popups, message boxes, loading, prompts).
 *
 * Every long-running flow that needs to block the UI should use these
 * instead of hand-rolling lv_obj_t trees on lv_layer_top. They share the
 * theme tokens from ui/theme.h so the look is consistent.
 *
 * Implementations live in src/ui_tools.cpp (popup/loading/result, overlay,
 * modal button, confirm, status-bar height),
 * src/ui_lock.cpp (passphrase/text prompts + device-lock enforcement),
 * src/ui_wifi.cpp (WiFi picker), src/ui_nfc_test.cpp (NFC overlay).
 */
#pragma once

#include <lvgl.h>
#include <stddef.h>

/* MsgBox-style two-button modal. */
lv_obj_t *create_msgbox(lv_obj_t *parent, const char *title_txt,
                        const char *msg_txt, const char **btns,
                        lv_event_cb_t btns_event_cb, void *user_data);
void destroy_msgbox(lv_obj_t *msgbox);

/* Themed fullscreen modal overlay. Caller appends children (labels, bar,
 * buttons); destroy with ui_popup_destroy(). Used by ui_loading and
 * ui_result internally — match the create_msgbox() look. */
lv_obj_t *ui_popup_create(const char *title);
void ui_popup_destroy(lv_obj_t *popup);

/* Height of the global status bar: the header font's line height + 8 px,
 * never less than 30. Overlays that sit just below the bar use it. */
int32_t ui_status_bar_height(void);

/* Modal input context shared by the full-screen overlays (WiFi picker, list
 * picker, NFC test, passphrase prompts). ui_overlay_open() saves the
 * default group, installs a fresh one (`wrap` = lv_group_set_wrap) as the
 * default, re-points the status-bar back button at `back_cb` (skipped when
 * NULL, e.g. the passphrase modal, which owns no back button), enables the
 * keyboard, and returns a themed container on lv_layer_top(): full width,
 * flush below the status bar, BG fill, 8 px padding, column flex (start /
 * center / center) with 6 px row gap. The caller adds children and their
 * group entries via o->group. ui_overlay_close() reverses all of it; it is
 * safe on a zeroed struct. */
typedef struct {
    lv_obj_t     *overlay;
    lv_group_t   *group;
    lv_group_t   *prev_group;
    lv_event_cb_t prev_back_cb;
} ui_overlay_t;

lv_obj_t *ui_overlay_open(ui_overlay_t *o, lv_event_cb_t back_cb, bool wrap);
void ui_overlay_close(ui_overlay_t *o);

/* Themed button for modal overlays: SURFACE face, faint accent border that
 * goes solid (and the face accent-tinted) when focused, FG label centered.
 * `cb` (may be NULL) fires on LV_EVENT_CLICKED with `user_data`. */
lv_obj_t *ui_modal_btn_create(lv_obj_t *parent, const char *text,
                              lv_event_cb_t cb, void *user_data);

/* Yes/No confirmation dialog (create_msgbox look). `cb(true, ud)` on Yes,
 * `cb(false, ud)` on No; the dialog is closed before `cb` runs. */
typedef void (*ui_confirm_cb)(bool yes, void *ud);
void ui_confirm(const char *title, const char *msg, ui_confirm_cb cb, void *ud);
/* Same, with custom labels laid out {no_txt, yes_txt} (e.g. "Cancel" left,
 * "Save" right); `cb(true, ud)` on yes_txt. */
void ui_confirm(const char *title, const char *msg, const char *no_txt,
                const char *yes_txt, ui_confirm_cb cb, void *ud);

/* Unified loading / progress popup. Every long-running screen (sync,
 * download, index scan, storage ops) should use this instead of hand-
 * building lv_obj_t trees atop ui_popup_create() — produces a consistent
 * look: title at top, spinner OR progress bar centered, counts + detail
 * lines below.
 *
 * Typical usage:
 *   ui_loading_t ld;
 *   ui_loading_open(&ld, "Syncing", "Connecting...");
 *   ui_loading_set_progress(&ld, i, total, fname);
 *   ...
 *   ui_loading_close(&ld);
 *
 * After open() the popup is indeterminate (spinner + detail line). The
 * first set_progress() call swaps the spinner for a real progress bar
 * and shows "cur / total (pct%)" counts. set_indeterminate() swaps back. */
typedef struct {
    lv_obj_t *overlay;
    lv_obj_t *spinner;
    lv_obj_t *bar;
    lv_obj_t *counts;
    lv_obj_t *detail;
} ui_loading_t;

void ui_loading_open(ui_loading_t *l, const char *title, const char *detail);
void ui_loading_set_indeterminate(ui_loading_t *l, const char *detail);
void ui_loading_set_progress(ui_loading_t *l, int cur, int total, const char *detail);
void ui_loading_close(ui_loading_t *l);

/* Structured result popup. Same two-button modal as ui_msg_pop_up, but the
 * body is rendered as aligned "label: value" rows so numeric summaries
 * (files synced / failed / skipped) read as a table instead of a comma-
 * separated sentence. Pass nullptr/0 rows to fall back to a plain subtitle
 * body.
 *
 * Example:
 *   ui_summary_row_t rows[] = {
 *       {"Synced",  "3 / 10"},
 *       {"Failed",  "2"},
 *       {"Skipped", "5"},
 *   };
 *   ui_result_show("Sync", "Done.", rows, 3); */
typedef struct {
    const char *label;
    const char *value;
} ui_summary_row_t;

void ui_result_show(const char *title, const char *subtitle,
                    const ui_summary_row_t *rows, size_t n_rows);

/* Notes-encryption passphrase prompts (see ui_lock.cpp). */
typedef void (*ui_passphrase_result_cb)(const char *pw, void *ud);
typedef void (*ui_passphrase_unlock_cb)(bool ok, void *ud);

/* Runs the session-unlock flow. Fires `cb(true, ud)` immediately if crypto
 * is disabled or already unlocked; otherwise shows the passphrase modal
 * and calls `cb` with the outcome. */
void ui_passphrase_unlock(ui_passphrase_unlock_cb cb, void *ud);

/* Unlock-gated app build shared by the editor and the journal. Runs
 * `build(parent)` at once when the notes session is unlocked (or crypto is
 * off); otherwise shows the unlock modal and runs it on success, or returns
 * to the menu (menu_show_deferred) on cancel. `gate` is the app's static
 * state: ui_unlock_gate_cancel() on exit makes a still-open prompt a no-op. */
typedef struct {
    lv_obj_t *pending;              /* parent awaiting the unlock, or NULL */
    void (*build)(lv_obj_t *parent);
} ui_unlock_gate_t;
void ui_build_when_unlocked(ui_unlock_gate_t *gate, lv_obj_t *parent);
static inline void ui_unlock_gate_cancel(ui_unlock_gate_t *gate) { gate->pending = NULL; }

/* Low-level prompt. `cb` receives the typed passphrase (non-null, only valid
 * during the callback) on OK, or NULL on Cancel. With `confirm=true`, a second
 * confirmation field is shown and only matching entries proceed. */
void ui_passphrase_prompt(const char *title, const char *subtitle,
                          bool confirm,
                          ui_passphrase_result_cb cb, void *ud);

/* Plain-text modal prompt. Same look as ui_passphrase_prompt but with no
 * password masking. `initial` pre-fills the field (NULL = empty). `cb` fires
 * with the typed text on OK (only valid during the callback) or NULL on
 * Cancel. */
void ui_text_prompt(const char *title, const char *subtitle,
                    const char *initial,
                    ui_passphrase_result_cb cb, void *ud);

/* Device-level lock enforcement. If notes crypto is enabled and the session
 * is locked, puts the unlock modal on top of the UI until the correct
 * passphrase is entered — it has no title and no OK/Cancel buttons, so typing
 * the passphrase is the only way past it. No-op when crypto is disabled or the
 * session is already unlocked. */
void ui_device_lock_enforce();

/* True while any of the ui_passphrase_* / ui_text_prompt / ui_device_lock_enforce
 * modals are showing. The fake-sleep wake path checks this so it doesn't yank
 * the input group away from the modal on resume. */
bool ui_passphrase_is_active();

/* Modal WiFi picker: scan / list / connect / forget. Self-contained overlay
 * on lv_layer_top. See ui_wifi.cpp. */
void ui_wifi_networks_open();

/* Live NFC status + detection counters overlay. See ui_nfc_test.cpp. */
void ui_nfc_test_open();
