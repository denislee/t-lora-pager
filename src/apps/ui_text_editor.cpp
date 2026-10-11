/**
 * @file      ui_text_editor.cpp
 * @author    LilyGo CLI Agent
 * @license   MIT
 * @date      2026-04-10
 *
 * State reset on onStop (editor_exit):
 *   widgets: menu, text_area, quit_btn, word_count_label, float_bar,
 *            float_sync_btn, float_sync_icon, float_journal_btn
 *                                       — destroyed via lv_obj_del then nulled
 *   widgets: pending_editor_parent      — nulled (was deferred-build pointer)
 *   timers:  sync_pill_wifi_timer       — lv_timer_del + null
 *            word_count_debounce_timer  — lv_timer_del + null
 * If you add a new cached LVGL pointer or timer, list it above AND
 * extend editor_exit() — the debounce timer in particular will fire
 * into a destroyed textarea if leaked.
 */
#include "../ui_define.h"
#include "../core/app_manager.h"
#include "../core/system.h"
#include "../hal/notes_crypto.h"
#include "../hal/storage.h"
#include "../hal/wireless.h"

using std::string;

LV_FONT_DECLARE(lv_font_montserrat_12);
LV_FONT_DECLARE(lv_font_montserrat_14);

static lv_obj_t *menu = NULL;
static lv_obj_t *text_area = NULL;
static lv_obj_t *quit_btn = NULL;
static lv_obj_t *word_count_label = NULL;
static lv_obj_t *float_bar = NULL;
static lv_obj_t *float_sync_btn = NULL;
static lv_obj_t *float_sync_icon = NULL;
static lv_obj_t *float_journal_btn = NULL;
static lv_timer_t *sync_pill_wifi_timer = NULL;
static int sync_pill_wifi_last_state = -1;
static string current_file_path = "";
static bool content_dirty = false;

// Word count is recomputed lazily through this debounce timer instead of
// inline on every LV_EVENT_VALUE_CHANGED. Counting words walks the entire
// textarea buffer and refreshes a label — cheap once, but on every keystroke
// it stacks up behind LVGL's own redraw of the textarea and produces visible
// typing stutter as a note grows past a few hundred chars.
static lv_timer_t *word_count_debounce_timer = NULL;

static void ui_text_editor_exit(lv_obj_t *parent);
static void update_word_count();
static void schedule_word_count_update();

// `allow_prune` forwards to hw_save_preferred_file: the exit/teardown path
// passes false so returning to the menu is never delayed by the internal-storage
// eviction sweep, which runs on the next ordinary save instead.
static bool save_content(bool show_error_popup, bool allow_prune = true)
{
    if (!text_area) return true;
    if (!content_dirty) return true;

    const char *txt = lv_textarea_get_text(text_area);

    bool only_whitespace = true;
    size_t len = lv_strlen(txt);
    for (size_t i = 0; i < len; i++) {
        if (!isspace((unsigned char)txt[i])) {
            only_whitespace = false;
            break;
        }
    }
    if (only_whitespace) return true;

    string target_path = current_file_path;
    if (target_path.empty()) {
        struct tm timeinfo;
        hw_get_date_time(timeinfo);
        char filename[64];
        snprintf(filename, sizeof(filename), "/notes/%04d%02d%02d_%02d%02d%02d.txt",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        target_path = filename;
    }

    string err;
    bool success = hw_save_preferred_file(target_path.c_str(), txt, &err, allow_prune);
    if (success) {
        content_dirty = false;
        if (current_file_path.empty()) current_file_path = target_path;
    } else if (show_error_popup) {
        string msg = "Failed to save file: " + err;
        ui_msg_pop_up("Save", msg.c_str());
    }
    return success;
}

static void update_word_count()
{
    if (!text_area || !word_count_label) return;
    
    const char *txt = lv_textarea_get_text(text_area);
    size_t chars = lv_strlen(txt);
    size_t words = 0;
    bool in_word = false;
    
    for (size_t i = 0; i < chars; i++) {
        if (isspace((unsigned char)txt[i])) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            words++;
        }
    }
    
    lv_label_set_text_fmt(word_count_label, "%zu words | %zu chars", words, chars);
}

static void word_count_debounce_cb(lv_timer_t *t)
{
    (void)t;
    word_count_debounce_timer = NULL;  // one-shot consumed itself below
    update_word_count();
}

// Coalesce bursts of keystrokes into a single word-count refresh. The first
// keystroke arms a 250 ms one-shot; subsequent keystrokes just push the
// deadline forward, so the label updates ~250 ms after the user stops typing
// instead of on every char.
static void schedule_word_count_update()
{
    if (!word_count_label) return;
    if (word_count_debounce_timer) {
        lv_timer_reset(word_count_debounce_timer);
        return;
    }
    word_count_debounce_timer = lv_timer_create(word_count_debounce_cb, 250, NULL);
    lv_timer_set_repeat_count(word_count_debounce_timer, 1);
}

static void do_exit()
{
    save_content(true);
    // menu_show will trigger AppManager::switchApp which calls ui_text_editor_exit
    menu_show();
}

static void exit_btn_cb(lv_event_t *e)
{
    do_exit();
}

static void text_area_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_group_t *g = (lv_group_t *)lv_obj_get_group(obj);

    if (code == LV_EVENT_VALUE_CHANGED) {
        content_dirty = true;
        schedule_word_count_update();
    } else if (code == LV_EVENT_CLICKED) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev && lv_indev_get_type(indev) != LV_INDEV_TYPE_ENCODER) {
            // Only toggle on click if it's NOT the encoder (e.g., touchscreen)
            // Encoder toggle is handled in LV_EVENT_KEY to prevent newline insertion
            bool editing = lv_group_get_editing(g);
            lv_group_set_editing(g, !editing);
        }
    } else if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);
        lv_indev_t *indev = lv_indev_get_act();
        bool editing = lv_group_get_editing(g);

        if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER && key == LV_KEY_ENTER) {
            lv_group_set_editing(g, !editing);
            if (editing) {
                // Remove the newline character inserted by the default textarea handler
                lv_textarea_delete_char((lv_obj_t *)lv_event_get_target(e));
            }
            lv_event_stop_processing(e);
            return;
        }

        if (key == 0x1E) {
            lv_event_stop_processing(e);
            return;
        }
        if (key == LV_KEY_ESC) {
            if (editing) {
                lv_group_set_editing(g, false);
            } else {
                /* ESC when not editing -> exit the editor */
                do_exit();
            }
            lv_event_stop_processing(e);
        }
    }
}

static void editor_build_ui(lv_obj_t *parent);

// Floating pills (bottom-right) that link out to Sync and Journal. They were
// previously sibling tiles in a Notes submenu; pulling them inside the editor
// keeps the home screen single-tap to a new note while leaving the two
// follow-up actions a thumb-reach away. The pills render invisible (opa=0)
// until the encoder focuses one, so they don't crowd the editing surface —
// they fade in only while the user is parked on them.
static void float_btn_click_cb(lv_event_t *e)
{
    const char *app_name = (const char *)lv_event_get_user_data(e);
    if (!app_name) return;
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    if (btn && lv_obj_has_state(btn, LV_STATE_DISABLED)) {
        hw_feedback();
        return;
    }
    hw_feedback();
    core::AppManager::getInstance().switchApp(app_name,
        core::System::getInstance().getAppPanel());
}

/* Sync needs WiFi to talk to GitHub, so the pill is gated on the live link
 * state. Disabled state blocks input and makes encoder nav skip the pill,
 * matching the behaviour of WiFi-gated tiles on the home screen. */
static void apply_sync_pill_wifi_state(bool connected)
{
    if (!float_sync_btn) return;
    lv_color_t accent = lv_palette_main(LV_PALETTE_GREEN);
    if (connected) {
        lv_obj_remove_state(float_sync_btn, LV_STATE_DISABLED);
        lv_obj_set_style_border_color(float_sync_btn, accent, 0);
        lv_obj_set_style_border_opa(float_sync_btn, LV_OPA_60, 0);
        if (float_sync_icon)
            lv_obj_set_style_text_color(float_sync_icon, accent, 0);
    } else {
        lv_obj_add_state(float_sync_btn, LV_STATE_DISABLED);
        lv_obj_set_style_border_color(float_sync_btn, UI_COLOR_MUTED, 0);
        lv_obj_set_style_border_opa(float_sync_btn, LV_OPA_30, 0);
        if (float_sync_icon)
            lv_obj_set_style_text_color(float_sync_icon, UI_COLOR_MUTED, 0);
        lv_group_t *g = lv_obj_get_group(float_sync_btn);
        if (g && lv_group_get_focused(g) == float_sync_btn) {
            lv_group_focus_next(g);
        }
    }
}

static void sync_pill_wifi_tick(lv_timer_t *t)
{
    (void)t;
    int now = hw_get_wifi_connected() ? 1 : 0;
    if (now != sync_pill_wifi_last_state) {
        apply_sync_pill_wifi_state(now != 0);
        sync_pill_wifi_last_state = now;
    }
}

/* Reveal both pills together so the user always sees the full action group
 * when their focus is parked on either one — keeps Journal / Sync side-by-side
 * rather than letting only the focused one peek out. */
static void pill_focused_cb(lv_event_t *e)
{
    (void)e;
    if (float_sync_btn)    lv_obj_set_style_opa(float_sync_btn,    LV_OPA_COVER, 0);
    if (float_journal_btn) lv_obj_set_style_opa(float_journal_btn, LV_OPA_COVER, 0);
}

/* Re-evaluate visibility once LVGL has finished moving focus. We defer via
 * lv_async_call because LV_EVENT_DEFOCUSED fires *before* the group's focus
 * pointer is updated — checking it inside the event would always return the
 * pill we're leaving, never the new target, so the pills would never hide
 * once revealed. */
static void pill_recheck_async(void *unused)
{
    (void)unused;
    if (!float_sync_btn || !float_journal_btn) return;
    lv_group_t *g = lv_obj_get_group(float_sync_btn);
    lv_obj_t *focused = g ? lv_group_get_focused(g) : NULL;
    bool show = (focused == float_sync_btn || focused == float_journal_btn);
    lv_opa_t target = show ? LV_OPA_COVER : LV_OPA_TRANSP;
    lv_obj_set_style_opa(float_sync_btn,    target, 0);
    lv_obj_set_style_opa(float_journal_btn, target, 0);
}

static void pill_defocused_cb(lv_event_t *e)
{
    (void)e;
    lv_async_call(pill_recheck_async, NULL);
}

static lv_obj_t *make_editor_pill(lv_obj_t *bar, const char *symbol,
                                  lv_palette_t palette)
{
    const int32_t pill_h = 26;
    lv_color_t accent = lv_palette_main(palette);

    lv_obj_t *btn = lv_btn_create(bar);
    lv_obj_set_size(btn, pill_h + 14, pill_h);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(btn, pill_h / 2, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, accent, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_60, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);

    /* Hidden-by-default visibility: object opacity multiplies through to all
     * children, so opa=0 erases the bg, border, and icon together while the
     * pill remains in the focus group and fully clickable. Visibility is
     * driven manually by FOCUSED/DEFOCUSED handlers in editor_build_ui — the
     * pair fades in/out together so the user always sees both actions. */
    lv_obj_set_style_opa(btn, LV_OPA_TRANSP, 0);

    lv_obj_set_style_outline_width(btn, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(btn, UI_COLOR_FG, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(btn, 2, LV_STATE_FOCUSED);

    lv_obj_set_style_bg_color(btn, lv_palette_darken(palette, 3),
                              LV_STATE_PRESSED);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(icon, accent, 0);
    lv_obj_center(icon);
    return btn;
}

static lv_obj_t *pending_editor_parent = NULL;
static void editor_unlock_result_cb(bool ok, void *ud)
{
    lv_obj_t *parent = (lv_obj_t *)ud;
    if (!ok) {
        pending_editor_parent = NULL;
        menu_show();
        return;
    }
    if (pending_editor_parent == parent) {
        pending_editor_parent = NULL;
        editor_build_ui(parent);
    }
}

static void ui_text_editor_enter(lv_obj_t *parent)
{
    if (notes_crypto_is_enabled() && !notes_crypto_is_unlocked()) {
        pending_editor_parent = parent;
        ui_passphrase_unlock(editor_unlock_result_cb, parent);
        return;
    }
    editor_build_ui(parent);
}

static void editor_build_ui(lv_obj_t *parent)
{
    // If it was already partially active or pointers are stale, clean up
    if (menu != NULL) {
        // If we are already here, don't do anything
        if (lv_obj_get_parent(menu) == parent) {
            return;
        }
        ui_text_editor_exit(NULL);
    }
    enable_keyboard();

    content_dirty = false;

    menu = create_menu(parent, nullptr);

    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);

    /* Back button lives on the top status bar. */
    lv_obj_t *back_btn = ui_show_back_button(exit_btn_cb);

    /* Textarea fills the whole page. The word-count readout sits on the status
     * bar just to the right of the back button (same row as '<'). */
    text_area = lv_textarea_create(main_page);
    lv_obj_set_width(text_area, LV_PCT(100));
    lv_obj_set_flex_grow(text_area, 1);
    lv_textarea_set_placeholder_text(text_area, "");
    lv_obj_set_style_text_font(text_area, get_editor_font(), 0);
    lv_obj_set_style_bg_color(text_area, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(text_area, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(text_area, UI_RADIUS, 0);
    lv_obj_set_style_border_color(text_area, UI_COLOR_MUTED, 0);
    lv_obj_set_style_border_width(text_area, 1, 0);
    lv_obj_set_style_border_opa(text_area, LV_OPA_60, 0);
    lv_obj_set_style_border_color(text_area, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(text_area, UI_BORDER_W, LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(text_area, LV_OPA_COVER, LV_STATE_FOCUSED);

    /* Cursor style */
    lv_obj_set_style_bg_color(text_area, UI_COLOR_FG, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(text_area, LV_OPA_COVER, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_width(text_area, 8, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(text_area, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(text_area, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);

    lv_obj_add_event_cb(text_area, text_area_event_cb, LV_EVENT_ALL, NULL);
    lv_group_add_obj(lv_group_get_default(), text_area);

    /* Floating Sync / Journal pills, anchored bottom-right of the app panel.
     * The pills sit invisibly in the focus group; encoder rotation past the
     * textarea lands on them and fades them in via LV_STATE_FOCUSED. Rotating
     * away returns them to opa=0. */
    float_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(float_bar);
    lv_obj_add_flag(float_bar, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(float_bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(float_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(float_bar, 0, 0);
    lv_obj_set_style_pad_all(float_bar, 0, 0);
    lv_obj_set_style_pad_column(float_bar, 6, 0);
    lv_obj_set_flex_flow(float_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(float_bar, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(float_bar, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_remove_flag(float_bar, LV_OBJ_FLAG_SCROLLABLE);

    float_journal_btn = make_editor_pill(float_bar, LV_SYMBOL_BARS,
                                         LV_PALETTE_CYAN);
    lv_obj_add_event_cb(float_journal_btn, float_btn_click_cb,
                        LV_EVENT_CLICKED, (void *)"Journal");
    lv_obj_add_event_cb(float_journal_btn, pill_focused_cb,
                        LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(float_journal_btn, pill_defocused_cb,
                        LV_EVENT_DEFOCUSED, NULL);

    float_sync_btn = make_editor_pill(float_bar, LV_SYMBOL_REFRESH,
                                      LV_PALETTE_GREEN);
    /* The pill icon is the first child of the button — cache it so the WiFi
     * gate can recolour it without walking the tree on every tick. */
    float_sync_icon = lv_obj_get_child(float_sync_btn, 0);
    lv_obj_add_event_cb(float_sync_btn, float_btn_click_cb,
                        LV_EVENT_CLICKED, (void *)"Notes Sync");
    lv_obj_add_event_cb(float_sync_btn, pill_focused_cb,
                        LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(float_sync_btn, pill_defocused_cb,
                        LV_EVENT_DEFOCUSED, NULL);

    /* Pills join the default group so the encoder can rotate to them past
     * the textarea. They start invisible and fade in only on focus. */
    if (lv_group_t *def_grp = lv_group_get_default()) {
        lv_group_add_obj(def_grp, float_journal_btn);
        lv_group_add_obj(def_grp, float_sync_btn);
    }

    /* Apply the initial WiFi-gated state for the Sync pill and start a
     * lightweight tick that re-evaluates it — the user can flip WiFi from
     * the home screen toggles and come back to the editor without the pill
     * going stale. */
    sync_pill_wifi_last_state = -1;
    apply_sync_pill_wifi_state(hw_get_wifi_connected());
    sync_pill_wifi_last_state = hw_get_wifi_connected() ? 1 : 0;
    if (!sync_pill_wifi_timer) {
        sync_pill_wifi_timer = lv_timer_create(sync_pill_wifi_tick, 1000, NULL);
    }

    word_count_label = lv_label_create(lv_obj_get_parent(back_btn));
    lv_label_set_text(word_count_label, "0 words | 0 chars");
    lv_obj_set_style_text_color(word_count_label, ui_color_title_muted()  /* lives in the status bar */, 0);
    lv_obj_set_style_text_font(word_count_label, &lv_font_montserrat_12, 0);
    lv_obj_align_to(word_count_label, back_btn, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    lv_menu_set_page(menu, main_page);

    // Focus the textarea so encoder/keyboard nav lands there first, but
    // don't enter editing mode — the user presses ENTER (encoder click) to
    // start typing.
    lv_group_t *g = lv_obj_get_group(text_area);
    if (g) {
        lv_group_focus_obj(text_area);
    }
    update_word_count();

    // Pre-derive the note-encryption key now, while the user is just opening
    // the editor, so the save that fires on exit only pays for the AES pass and
    // not the ~10k-iteration PBKDF2. Self-guards to a no-op when notes crypto
    // is off/locked.
    notes_crypto_prewarm();

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button([](lv_event_t *e) {
        do_exit();
    }, NULL);
#endif
}

void ui_text_editor_open_file(const char *path)
{
    if (text_area == NULL) return;
    current_file_path = path;
    string content;
    if (hw_read_file(path, content)) {
        lv_textarea_set_text(text_area, content.c_str());
        content_dirty = false;
        update_word_count();
    }
}

static void ui_text_editor_exit(lv_obj_t *parent)
{
    // Persist the buffer on every teardown path, not just do_exit(): the
    // AppManager can stop us from app-switching, sleep, or any code that
    // calls switchApp() without routing through our back button. Save
    // silently here — the UI is already going away so a popup would race
    // with the destruction. Skip the eviction sweep (allow_prune=false): the
    // teardown must return promptly, and the sweep will run on the next save.
    save_content(false, /*allow_prune=*/false);

    ui_hide_back_button();

    if (word_count_label) {
        lv_obj_del(word_count_label);
        word_count_label = NULL;
    }

    if (sync_pill_wifi_timer) {
        lv_timer_del(sync_pill_wifi_timer);
        sync_pill_wifi_timer = NULL;
    }
    sync_pill_wifi_last_state = -1;

    if (word_count_debounce_timer) {
        lv_timer_del(word_count_debounce_timer);
        word_count_debounce_timer = NULL;
    }

    if (float_bar) {
        lv_obj_del(float_bar);
        float_bar = NULL;
    }
    float_sync_btn = NULL;
    float_sync_icon = NULL;
    float_journal_btn = NULL;

    if (menu) {
        lv_obj_clean(menu);
        lv_obj_del(menu);
        menu = NULL;
    }
    if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
    text_area = NULL;
    disable_keyboard();
    current_file_path = "";
}

#include "app_registry.h"

namespace {
class TextEditorApp : public core::App {
public:
    TextEditorApp() : core::App("Editor") {}
    void onStart(lv_obj_t *parent) override { ui_text_editor_enter(parent); }
    void onStop() override {
        ui_text_editor_exit(getRoot());
        core::App::onStop();
    }
};
} // namespace

namespace apps {
APP_FACTORY(make_text_editor_app, TextEditorApp)
} // namespace apps
