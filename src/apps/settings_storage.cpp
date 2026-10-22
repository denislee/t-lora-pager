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
 *
 * The Notes Security bulk operations run on `g_sec_job`, a OneShotJob worker,
 * and report progress through `storage_progress_post` -> `g_progress_timer`
 * (the older `storage_progress_cb` touches LVGL and stays for the synchronous
 * Storage-subpage operations). `g_sec_job` is deliberately not torn down by
 * `reset_state()` so its result popup still lands, and `sec_done` only
 * navigates while `g_sub_page` is set.
 */
#include "../ui_define.h"
#include "../hal/notes_crypto.h"
#include "../hal/hub.h"
#include "../hal/link.h"
#include "../hal/notes_link.h"
#include "../hal/secrets.h"
#include "../hal/storage.h"
#include "../core/one_shot_job.h"
#include "settings_internal.h"

#include <functional>
#include <memory>
#include <mutex>
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

// Worker-safe progress mailbox (P7.7). The Notes Security bulk operations run
// on core::OneShotJob workers, which must never touch LVGL; they post here and
// the LVGL thread applies the latest value from g_progress_timer.
struct StorageProgress {
    std::mutex mu;
    int cur = 0;
    int total = 0;
    std::string name;
    bool dirty = false;
};

StorageProgress g_storage_progress;
lv_timer_t *g_progress_timer = nullptr;

// Worker-safe (P7.7): records the latest progress; the LVGL thread applies it
// from g_progress_timer. Never touches LVGL.
void storage_progress_post(int cur, int total, const char *name)
{
    std::lock_guard<std::mutex> lock(g_storage_progress.mu);
    g_storage_progress.cur = cur;
    g_storage_progress.total = total;
    g_storage_progress.name = name ? name : "";
    g_storage_progress.dirty = true;
}

void storage_progress_drain(lv_timer_t *)
{
    int cur, total;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(g_storage_progress.mu);
        if (!g_storage_progress.dirty) return;
        cur = g_storage_progress.cur;
        total = g_storage_progress.total;
        name = g_storage_progress.name;
        g_storage_progress.dirty = false;
    }
    if (!storage_loader.overlay) return;
    if (total > 0) {
        ui_loading_set_progress(&storage_loader, cur, total, name.c_str());
    } else {
        ui_loading_set_indeterminate(&storage_loader,
                                     name.empty() ? "Working..." : name.c_str());
    }
}

void storage_progress_start()
{
    {
        std::lock_guard<std::mutex> lock(g_storage_progress.mu);
        g_storage_progress.dirty = false;
        g_storage_progress.name.clear();
        g_storage_progress.cur = 0;
        g_storage_progress.total = 0;
    }
    if (!g_progress_timer) {
        g_progress_timer = lv_timer_create(storage_progress_drain, 100, nullptr);
    }
}

void storage_progress_stop()
{
    if (g_progress_timer) {
        lv_timer_delete(g_progress_timer);
        g_progress_timer = nullptr;
    }
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

struct CopyNotesResult { const char *title; std::string msg; };

// Runs the copy off the LVGL thread — the phone path can wait up to 120 s per
// batch, the hub path does one HTTP request per note (P7.20).
static core::OneShotJob<CopyNotesResult> g_copy_job;

static void copy_notes_done(CopyNotesResult &r) {
    hide_storage_loader();
    ui_msg_pop_up(r.title, r.msg.c_str());
}

// "Copy Notes -> Phone": phone first (notes.sync target "phone", internal +
// SD notes, internal wins a shared name); without a phone the existing hub
// upload (deleted in H4.2, D-H1); otherwise "Connect your phone".
// Runs on g_copy_job so the UI stays live; the hub path shows an indeterminate
// loader (no per-file progress off the LVGL thread).
void storage_copy_notes_cb(lv_event_t *) {
    // A copy is already running — its own completion pops the result.
    if (g_copy_job.busy()) return;

    // Decide the path on the UI thread: the worker must not query the link or
    // the hub config itself.
    bool phone = hw_link_connected();
    if (!phone && !hal::hub_is_enabled()) {
        ui_msg_pop_up("Phone",
                      "Connect your phone\n"
                      "(or set Settings " "\xC2\xBB" " Local Hub).");
        return;
    }

    show_storage_loader(phone ? "Copy Notes to Phone" : "Upload Notes via Hub");
    g_copy_job.on_done(copy_notes_done);

    core::OneShotJob<CopyNotesResult>::Start st = g_copy_job.start(
        [phone]() -> CopyNotesResult * {
            char msg[160];
            if (phone) {
                std::string err;
                HalResult<NotesSyncSummary> r = notes_link_copy_to_phone(err);
                if (r) {
                    snprintf(msg, sizeof(msg), "Copied %d note(s) to phone.%s",
                             r.value().uploaded + r.value().already,
                             r.value().errors.empty() ? "" : "\nSome failed.");
                } else {
                    snprintf(msg, sizeof(msg), "Copy failed: %s",
                             err.empty() ? hal_error_string(r.error()) : err.c_str());
                }
                return new CopyNotesResult{"Phone", msg};
            }

            int copied = 0, failed = 0;
            std::string err;
            // nullptr, not storage_progress_cb: that cb touches LVGL and this
            // runs on the worker.
            bool ok = hw_copy_all_notes_to_hub(&copied, &failed, &err, nullptr);

            if (ok) {
                snprintf(msg, sizeof(msg),
                         "Uploaded %d note(s) via hub.%s",
                         copied, failed > 0 ? "\nSome failed." : "");
            } else if (!err.empty()) {
                snprintf(msg, sizeof(msg), "Upload failed: %s\nUploaded: %d, failed: %d.",
                         err.c_str(), copied, failed);
            } else {
                snprintf(msg, sizeof(msg), "Upload failed.\nUploaded: %d, failed: %d.",
                         copied, failed);
            }
            return new CopyNotesResult{"Hub", msg};
        },
        8192, "copy_notes", 2);

    if (st == core::OneShotJob<CopyNotesResult>::Start::Failed) {
        hide_storage_loader();
        ui_msg_pop_up("Phone", "Could not start the copy.");
    }
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
    // Copy all internal+SD notes to the phone (its Files-app folder), or via
    // the hub when no phone is connected (hardware only, until H4.2).
    settings_action_row(sub_page, "Copy Notes -> Phone", LV_SYMBOL_UPLOAD, storage_copy_notes_cb);
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

// Result handed from a Notes Security worker back to the LVGL thread.
struct SecResult {
    enum Kind { Message, AskNewPassphrase } kind = Message;
    std::string msg;
    bool to_main = true;     // pop back to the settings root afterwards
    std::string old_pw;      // AskNewPassphrase: the verified current passphrase
    ~SecResult() { hal::secret_scrub(old_pw); }
};

// Runs every Notes Security bulk operation (set/change/disable passphrase,
// encrypt SD) off the LVGL thread (P7.7). Not torn down on settings exit: the
// result popup must still land.
static core::OneShotJob<SecResult> g_sec_job;

void change_new_cb(const char *pw, void *ud);   // forward declaration

void sec_done(SecResult &r)
{
    storage_progress_stop();
    hide_storage_loader();
    if (r.kind == SecResult::AskNewPassphrase) {
        auto ctx = std::make_unique<ChangeCtx>();
        ctx->old_pw = r.old_pw;
        ui_passphrase_prompt("New passphrase",
                             "Enter a new passphrase for your notes.",
                             /*confirm=*/true, change_new_cb, ctx.release());
        return;
    }
    if (!r.msg.empty()) ui_msg_pop_up("Notes Security", r.msg.c_str());
    if (r.to_main && g_sub_page) return_to_main();
}

bool start_sec_job(const char *title, std::function<SecResult *()> work)
{
    if (g_sec_job.busy()) {
        ui_msg_pop_up("Notes Security", "Another operation is still running.");
        return false;
    }
    show_storage_loader(title);
    storage_progress_start();
    g_sec_job.on_done(sec_done);
    auto st = g_sec_job.start(std::move(work), 10240, "notes_sec", 1);
    if (st == core::OneShotJob<SecResult>::Start::Failed) {
        storage_progress_stop();
        hide_storage_loader();
        ui_msg_pop_up("Notes Security", "Could not start the operation.");
        return false;
    }
    return true;
}

// --- Set passphrase flow ---
void set_pw_cb(const char *pw, void *)
{
    if (!pw) return;
    std::string *p = new std::string(pw);
    bool started = start_sec_job("Encrypting notes", [p]() -> SecResult * {
        bool ok = notes_crypto_set_passphrase(p->c_str());
        if (ok) notes_crypto_encrypt_existing(storage_progress_post);
        hal::secret_scrub(*p);
        delete p;
        auto *r = new SecResult();
        r->msg = ok ? "Passphrase set. Notes are now encrypted."
                    : "Could not set passphrase.";
        r->to_main = ok;
        return r;
    });
    if (!started) {
        hal::secret_scrub(*p);
        delete p;
    }
}

// --- Change passphrase flow: ask old, then ask new (with confirm). ---
void change_new_cb(const char *pw, void *ud)
{
    /* Take ownership of the heap-allocated ChangeCtx that change_old_cb
     * passed via ud. The unique_ptr release()/reset() pair across this
     * pair of callbacks is the safe equivalent of new/delete — both
     * cancel and OK paths drop the allocation when this scope exits. */
    std::unique_ptr<ChangeCtx> ctx((ChangeCtx *)ud);
    if (!pw) {
        hal::secret_scrub(ctx->old_pw);
        return;
    }
    std::string *o = new std::string(ctx->old_pw);
    std::string *n = new std::string(pw);
    hal::secret_scrub(ctx->old_pw);
    bool started = start_sec_job("Re-encrypting notes", [o, n]() -> SecResult * {
        bool ok = notes_crypto_change_passphrase(o->c_str(), n->c_str(),
                                                 storage_progress_post);
        hal::secret_scrub(*o);
        delete o;
        hal::secret_scrub(*n);
        delete n;
        auto *r = new SecResult();
        r->msg = ok ? "Passphrase changed." : "Passphrase change failed.";
        r->to_main = true;
        return r;
    });
    if (!started) {
        hal::secret_scrub(*o);
        delete o;
        hal::secret_scrub(*n);
        delete n;
    }
}

void change_old_cb(const char *pw, void *)
{
    if (!pw) return;
    // Verify old passphrase up front so the user doesn't type a whole new
    // one first only to find out the old was wrong. The verify (one PBKDF2,
    // plus finishing an interrupted change) now runs on the worker.
    std::string *p = new std::string(pw);
    bool started = start_sec_job("Checking passphrase", [p]() -> SecResult * {
        bool ok = notes_crypto_unlock(p->c_str());
        auto *r = new SecResult();
        if (ok) {
            r->kind = SecResult::AskNewPassphrase;
            r->old_pw = *p;
        } else {
            r->msg = "Wrong current passphrase.";
            r->to_main = false;
        }
        hal::secret_scrub(*p);
        delete p;
        return r;
    });
    if (!started) {
        hal::secret_scrub(*p);
        delete p;
    }
}

// --- Disable flow ---
void disable_pw_cb(const char *pw, void *)
{
    if (!pw) return;
    std::string *p = new std::string(pw);
    bool started = start_sec_job("Decrypting notes", [p]() -> SecResult * {
        NotesDisableResult res = notes_crypto_disable(p->c_str(), storage_progress_post);
        hal::secret_scrub(*p);
        delete p;
        auto *r = new SecResult();
        switch (res) {
        case NotesDisableResult::Ok:
            r->msg = "Encryption disabled. Notes are plaintext again.\nSaved tokens were erased.";
            break;
        case NotesDisableResult::Pending:
            r->msg = "An interrupted passphrase change needs the SD card.\nInsert it, unlock, then retry.";
            break;
        case NotesDisableResult::WriteFailed:
            r->msg = "Some notes could not be written.\nEncryption stays on; free space and retry.";
            break;
        case NotesDisableResult::WrongPassphrase:
        default:
            r->msg = "Wrong passphrase.";
            break;
        }
        r->to_main = true;
        return r;
    });
    if (!started) {
        hal::secret_scrub(*p);
        delete p;
    }
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
                         "Enter passphrase to decrypt all notes. Saved tokens (Telegram, GitHub, SSH) will be erased.",
                         /*confirm=*/false, disable_pw_cb, NULL);
}

void btn_lock_cb(lv_event_t *) {
    notes_crypto_lock();
    ui_msg_pop_up("Notes Security", "Notes locked.");
    return_to_main();
}

void btn_encrypt_sd_cb(lv_event_t *) {
    start_sec_job("Encrypting SD notes", []() -> SecResult * {
        int scanned = 0, enc_count = 0;
        bool ok = notes_crypto_encrypt_sd(&scanned, &enc_count,
                                          storage_progress_post);
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
        auto *r = new SecResult();
        r->msg = msg;
        r->to_main = true;
        return r;
    });
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
