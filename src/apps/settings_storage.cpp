/**
 * @file      settings_storage.cpp
 * @brief     Settings » Storage + Notes Security subpages. Extracted from
 *            ui_settings.cpp; see settings_internal.h for the cross-TU
 *            contract.
 *
 * Bundled together because both share the storage_loader popup triad
 * (show_storage_loader / hide_storage_loader / storage_progress_cb) used
 * for long-running filesystem operations (copy-to-SD, prune, bulk
 * encrypt/decrypt). Keeping them in one TU lets the loader state stay
 * file-private.
 *
 * The Notes Security subpage is an async-prompt state machine driven by
 * `ui_passphrase_prompt` — each step heap-allocs a ChangeCtx and re-enters
 * the next prompt. After any state-mutating operation `mark_for_rebuild()`
 * drops the page's focus-group entries and resets user_data so LVGL
 * rebuilds it on next entry with the fresh enabled/unlocked status.
 */
#include "../ui_define.h"
#include "../hal/notes_crypto.h"
#include "../hal/hub.h"
#include "../hal/storage.h"
#include "settings_internal.h"

#include <memory>
#include <string>

namespace {

// Progress popup shared between the Storage subpage (copy-to-SD, manual
// prune) and every Notes Security flow (set/change/disable passphrase,
// encrypt SD). Only one can be visible at a time, so a file-local instance
// is enough.
ui_loading_t storage_loader = {};

void storage_progress_cb(int cur, int total, const char *name)
{
    if (!storage_loader.overlay) return;
    if (total > 0) {
        ui_loading_set_progress(&storage_loader, cur, total, name);
    } else {
        ui_loading_set_indeterminate(&storage_loader, name ? name : "Working...");
    }

    // Every bulk caller (copy-to-hub/SD, prune, encrypt/decrypt-all) runs the
    // whole-corpus loop synchronously on the LVGL task and calls this cb once
    // per file. On a large corpus that keeps IDLE0/IDLE1 off the CPU long
    // enough to panic the task watchdog — not just stutter. Throttle the screen
    // flush AND the IDLE yield to a >100 ms window, exactly like ui_journal.cpp's
    // report(): the widget values above already update every call, so the bar
    // stays accurate; vTaskDelay(1) costs one tick but guarantees IDLE runs and
    // feeds the watchdog.
    static uint32_t last_refr = 0;
    uint32_t now = lv_tick_get();
    if (lv_tick_elaps(last_refr) > 100) {
        lv_refr_now(NULL);
        last_refr = now;
#ifdef ARDUINO
        vTaskDelay(1);
#endif
    }
}

void show_storage_loader(const char *title)
{
    ui_loading_open(&storage_loader, title, "Preparing...");
}

void hide_storage_loader()
{
    ui_loading_close(&storage_loader);
}

} // anonymous namespace

// ============================================================================
// Storage subpage
// ============================================================================

namespace storage_cfg {

namespace {

void msc_target_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.msc_prefer_sd = en;
    hw_set_msc_prefer_sd(en);
}

void storage_prune_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.prune_internal = en;
}

void storage_prune_now_cb(lv_event_t *) {
    show_storage_loader("Pruning Internal");
    hw_prune_internal_storage(storage_progress_cb);
    hide_storage_loader();
    ui_msg_pop_up("Storage", "Internal storage pruned to\nkeep only the 50 newest files.");
}

void storage_copy_to_hub_cb(lv_event_t *) {
    if (!hal::hub_is_enabled()) {
        ui_msg_pop_up("Hub",
                      "Local Hub is disabled.\n"
                      "Enable it in Settings " "\xC2\xBB" " Local Hub first.");
        return;
    }

    show_storage_loader("Upload Notes to Hub");

    int copied = 0, failed = 0;
    std::string err;
    bool ok = hw_copy_all_notes_to_hub(&copied, &failed, &err, storage_progress_cb);

    hide_storage_loader();

    char msg[160];
    if (ok) {
        snprintf(msg, sizeof(msg),
                 "Uploaded %d note(s) to hub.%s",
                 copied, failed > 0 ? "\nSome failed." : "");
    } else if (!err.empty()) {
        snprintf(msg, sizeof(msg), "Upload failed: %s\nUploaded: %d, failed: %d.",
                 err.c_str(), copied, failed);
    } else {
        snprintf(msg, sizeof(msg), "Upload failed.\nUploaded: %d, failed: %d.",
                 copied, failed);
    }
    ui_msg_pop_up("Hub", msg);
}

void storage_copy_to_sd_cb(lv_event_t *) {
    show_storage_loader("Backup to SD");

    int copied = 0, failed = 0;
    std::string err;
    bool ok = hw_copy_internal_to_sd(&copied, &failed, &err, storage_progress_cb);

    hide_storage_loader();

    char msg[128];
    if (ok) {
        snprintf(msg, sizeof(msg), "Copied %d file(s) to SD.", copied);
    } else if (!err.empty()) {
        snprintf(msg, sizeof(msg), "Copy failed: %s\nCopied: %d, failed: %d.",
                 err.c_str(), copied, failed);
    } else {
        snprintf(msg, sizeof(msg), "Copy failed.\nCopied: %d, failed: %d.",
                 copied, failed);
    }
    ui_msg_pop_up("Storage", msg);
}

} // anonymous namespace

void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    // USB MSC target: checked = SD card, unchecked = internal flash.
    create_toggle_btn_row(sub_page, "USB MSC Target", local_param.msc_prefer_sd,
                          msc_target_cb, nullptr, " SD ", " Int ");
    create_toggle_btn_row(sub_page, "Limit 50 files", local_param.prune_internal, storage_prune_cb);

    settings_action_row(sub_page, "Copy Internal -> SD", LV_SYMBOL_COPY, storage_copy_to_sd_cb);
    // Copy all internal+SD notes to the hub. Mirrors the on-device store so
    // the hub becomes the source of truth for the new sync flow.
    settings_action_row(sub_page, "Copy Notes -> Hub", LV_SYMBOL_UPLOAD, storage_copy_to_hub_cb);
    settings_action_row(sub_page, "Prune Now (Keep 50)", LV_SYMBOL_TRASH, storage_prune_now_cb);
}

} // namespace storage_cfg

// ============================================================================
// Notes Security subpage — encryption-at-rest for *.txt notes
// ============================================================================

namespace notes_sec_cfg {

namespace {

// Tracked so we can rebuild with the fresh enabled/unlocked state after a
// passphrase operation — settings_page_changed_cb clears user_data after
// the first build, so without this a re-entry would still show the old
// "Status: OFF".
lv_obj_t *g_sub_page = nullptr;

struct ChangeCtx { std::string old_pw; };

void build_subpage_impl(lv_obj_t *menu, lv_obj_t *sub_page);

void mark_for_rebuild()
{
    if (!g_sub_page) return;
    // Drop stale subpage_items entries for this page before cleaning, so
    // activate_subpage_group won't re-add deleted widgets.
    unregister_subpage_items_for(g_sub_page);
    lv_obj_clean(g_sub_page);
    lv_obj_set_user_data(g_sub_page, (void*)&build_subpage_impl);
}

void return_to_main()
{
    // Pop back to the settings root so the sub-page is rebuilt on re-entry
    // with the updated state.
    mark_for_rebuild();
    settings_return_to_main_page();
}

// --- Set passphrase flow ---
void set_pw_cb(const char *pw, void *)
{
    if (!pw) return;
    if (!notes_crypto_set_passphrase(pw)) {
        ui_msg_pop_up("Notes Security", "Could not set passphrase.");
        return;
    }
    show_storage_loader("Encrypting notes");
    notes_crypto_encrypt_existing(storage_progress_cb);
    hide_storage_loader();
    ui_msg_pop_up("Notes Security", "Passphrase set. Notes are now encrypted.");
    return_to_main();
}

// --- Change passphrase flow: ask old, then ask new (with confirm). ---
void change_new_cb(const char *pw, void *ud)
{
    /* Take ownership of the heap-allocated ChangeCtx that change_old_cb
     * passed via ud. The unique_ptr release()/reset() pair across this
     * pair of callbacks is the safe equivalent of new/delete — both
     * cancel and OK paths drop the allocation when this scope exits. */
    std::unique_ptr<ChangeCtx> ctx((ChangeCtx *)ud);
    if (!pw) return;
    std::string new_pw = pw;
    show_storage_loader("Re-encrypting notes");
    bool ok = notes_crypto_change_passphrase(ctx->old_pw.c_str(),
                                             new_pw.c_str(),
                                             storage_progress_cb);
    hide_storage_loader();
    ui_msg_pop_up("Notes Security",
                  ok ? "Passphrase changed." : "Passphrase change failed.");
    return_to_main();
}

void change_old_cb(const char *pw, void *)
{
    if (!pw) return;
    // Verify old passphrase up front so the user doesn't type a whole new
    // one first only to find out the old was wrong.
    if (!notes_crypto_unlock(pw)) {
        ui_msg_pop_up("Notes Security", "Wrong current passphrase.");
        return;
    }
    auto ctx = std::make_unique<ChangeCtx>();
    ctx->old_pw = pw;
    /* Ownership of ctx travels to change_new_cb via the void* user-data
     * slot. release() detaches the unique_ptr; change_new_cb re-wraps. */
    ui_passphrase_prompt("New passphrase",
                         "Enter a new passphrase for your notes.",
                         /*confirm=*/true, change_new_cb, ctx.release());
}

// --- Disable flow ---
void disable_pw_cb(const char *pw, void *)
{
    if (!pw) return;
    show_storage_loader("Decrypting notes");
    bool ok = notes_crypto_disable(pw, storage_progress_cb);
    hide_storage_loader();
    ui_msg_pop_up("Notes Security",
                  ok ? "Encryption disabled. Notes are plaintext again."
                     : "Wrong passphrase.");
    return_to_main();
}

// --- Sub-page wiring ---

void btn_set_cb(lv_event_t *) {
    ui_passphrase_prompt("Set passphrase",
                         "Create a passphrase. You'll need it to read your notes.",
                         /*confirm=*/true, set_pw_cb, NULL);
}

void btn_change_cb(lv_event_t *) {
    ui_passphrase_prompt("Current passphrase",
                         "Enter your current passphrase.",
                         /*confirm=*/false, change_old_cb, NULL);
}

void btn_disable_cb(lv_event_t *) {
    ui_passphrase_prompt("Disable encryption",
                         "Enter passphrase to decrypt all notes.",
                         /*confirm=*/false, disable_pw_cb, NULL);
}

void btn_lock_cb(lv_event_t *) {
    notes_crypto_lock();
    ui_msg_pop_up("Notes Security", "Notes locked.");
    return_to_main();
}

void btn_encrypt_sd_cb(lv_event_t *) {
    show_storage_loader("Encrypting SD notes");
    int scanned = 0, enc_count = 0;
    bool ok = notes_crypto_encrypt_sd(&scanned, &enc_count, storage_progress_cb);
    hide_storage_loader();
    char msg[96];
    if (!ok) {
        snprintf(msg, sizeof(msg), "SD card unavailable or session locked.");
    } else if (scanned == 0) {
        snprintf(msg, sizeof(msg), "No protected notes found on SD.");
    } else {
        snprintf(msg, sizeof(msg),
                 "Scanned %d file(s).\nEncrypted %d new file(s).",
                 scanned, enc_count);
    }
    ui_msg_pop_up("Notes Security", msg);
    return_to_main();
}

void build_subpage_impl(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 4, 0);
    bool enabled  = notes_crypto_is_enabled();
    bool unlocked = notes_crypto_is_unlocked();

    lv_obj_t *status = lv_menu_cont_create(sub_page);
    lv_obj_t *lbl = lv_label_create(status);
    const char *state_txt = !enabled ? "Status: OFF"
                          : unlocked ? "Status: Unlocked"
                                     : "Status: Locked";
    lv_label_set_text(lbl, state_txt);
    lv_obj_set_style_text_color(lbl, UI_COLOR_MUTED, 0);

    if (!enabled) {
        lv_obj_t *b = create_button(sub_page, LV_SYMBOL_KEYBOARD,
                                    "Set passphrase", btn_set_cb);
        register_subpage_group_obj(sub_page, b);
    } else {
        lv_obj_t *b1 = create_button(sub_page, LV_SYMBOL_REFRESH,
                                     "Change passphrase", btn_change_cb);
        register_subpage_group_obj(sub_page, b1);

        if (unlocked) {
            lv_obj_t *b2 = create_button(sub_page, LV_SYMBOL_CLOSE,
                                         "Lock now", btn_lock_cb);
            register_subpage_group_obj(sub_page, b2);

            lv_obj_t *b_sd = create_button(sub_page, LV_SYMBOL_SD_CARD,
                                           "Encrypt SD notes", btn_encrypt_sd_cb);
            register_subpage_group_obj(sub_page, b_sd);
        }

        lv_obj_t *b3 = create_button(sub_page, LV_SYMBOL_TRASH,
                                     "Disable encryption", btn_disable_cb);
        register_subpage_group_obj(sub_page, b3);
    }
}

} // anonymous namespace

void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    build_subpage_impl(menu, sub_page);
}

void set_sub_page(lv_obj_t *page)
{
    g_sub_page = page;
}

void reset_state()
{
    g_sub_page = nullptr;
}

} // namespace notes_sec_cfg
