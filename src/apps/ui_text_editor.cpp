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
 *   unlock:  s_unlock_gate              — nulled, so an unlock prompt that
 *                                          completes after exit builds nothing
 *   display: clip_invalidate_cb         — removed from the default display
 *   timers:  sync_pill_timer            — lv_timer_del + null
 *            word_count_debounce_timer  — lv_timer_del + null
 *   save:    dirty buffer handed to hw_save_preferred_file_async (IS.2)
 *   state:   s_pending_open_path        — consumed by editor_build_ui
 *            s_last_space_pos           — reset (iOS "." shortcut tracker)
 * If you add a new cached LVGL pointer or timer, list it above AND
 * extend editor_exit() — the debounce timer in particular will fire
 * into a destroyed textarea if leaked.
 */
#include "../ui_define.h"
#include "../core/system.h"
#include "../core/notify.h"
#include "../hal/link.h"
#include "../hal/notes_crypto.h"
#include "../hal/nvs.h"
#include "../hal/storage.h"
#include "../hal/text_shortcuts.h"
#include "../hal/wireless.h"
#include "app_registry.h"

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
static lv_timer_t *sync_pill_timer = NULL;
static int sync_pill_last_state = -1;
static string current_file_path = "";
static bool content_dirty = false;
static string s_pending_open_path;

// iOS "." shortcut state (see the LV_EVENT_INSERT branch in
// text_area_event_cb): the cached setting and the cursor position right after
// the last space typed, so the next INSERT can tell a deliberate double space
// from two spaces typed far apart.
static int s_dbl_space_cache = -1;  // -1 = not loaded
static uint32_t s_last_space_pos = UINT32_MAX;  // cursor after the last typed space

// Word count is recomputed lazily through this debounce timer instead of
// inline on every LV_EVENT_VALUE_CHANGED. Counting words walks the entire
// textarea buffer and refreshes a label — cheap once, but on every keystroke
// it stacks up behind LVGL's own redraw of the textarea and produces visible
// typing stutter as a note grows past a few hundred chars.
static lv_timer_t *word_count_debounce_timer = NULL;

static void ui_text_editor_exit();
static void ui_text_editor_open_file(const char *path);
static void update_word_count();
static void schedule_word_count_update();

// Runs on the save worker: post a banner instead of a popup (LVGL is not
// ours to touch from there).
static void save_failed_cb(const char *path, const std::string &err)
{
    (void)path;
    core::notify::Notification n;
    n.title    = "Save failed";
    n.body     = err.empty() ? std::string("Note was not saved.") : err;
    n.icon     = LV_SYMBOL_WARNING;
    n.severity = core::notify::Severity::Error;
    n.duration_ms = 0;
    n.haptic   = true;
    core::notify::post(n);
}

// IS.2: leaving the editor snapshots the text and saves it on a worker, so
// the switch back to the menu does not wait for encode + FFat + SD. Same
// skip rules as save_content().
static void save_content_async()
{
    if (!text_area || !content_dirty) return;
    const char *txt = lv_textarea_get_text(text_area);
    bool only_whitespace = true;
    for (const char *p = txt; p && *p; ++p) {
        if (!isspace((unsigned char)*p)) { only_whitespace = false; break; }
    }
    if (only_whitespace) return;
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
    content_dirty = false;
    current_file_path = target_path;
    hw_save_preferred_file_async(target_path.c_str(), string(txt), save_failed_cb);
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

// Typing redraw clip. Every insert/delete makes the textarea's label
// invalidate itself whole, so each keystroke repainted (and pushed over SPI)
// the entire visible note. An edit at the cursor can only change the line
// above it (a delete can pull a word up) and everything below, so while the
// textarea's own key handler runs, invalidated areas are clipped to start at
// the line above the cursor. If the edit scrolled or changed the label
// height (new or removed line: scrollbar moves), the whole textarea is
// invalidated afterwards instead.
static bool s_clip_armed = false;
static int32_t s_clip_y1 = 0;
static int32_t s_clip_scroll_y = 0;
static int32_t s_clip_label_h = 0;

static bool is_edit_key(uint32_t key)
{
    // Printable chars and LV_KEY_DEL (127) are >= 0x20; navigation/ESC are not.
    return key == LV_KEY_BACKSPACE || key == LV_KEY_ENTER || key == '\r' || key >= 0x20;
}

static void clip_invalidate_cb(lv_event_t *e)
{
    if (!s_clip_armed) return;
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    if (area->y2 < s_clip_y1) {
        area->y1 = area->y2;  // nothing below the clip line; keep a sliver
    } else if (area->y1 < s_clip_y1) {
        area->y1 = s_clip_y1;
    } else {
        return;
    }
    area->y1 &= ~1;  // the pager's flush rounder wants an even y1
}

static void clip_arm_cb(lv_event_t *e)
{
    s_clip_armed = false;
    if (!text_area || !is_edit_key(lv_event_get_key(e))) return;
    lv_obj_t *label = lv_textarea_get_label(text_area);
    lv_point_t p;
    lv_label_get_letter_pos(label, lv_textarea_get_cursor_pos(text_area), &p);
    lv_area_t lc;
    lv_obj_get_coords(label, &lc);
    const lv_font_t *font = lv_obj_get_style_text_font(text_area, LV_PART_MAIN);
    int32_t line_h = lv_font_get_line_height(font) +
                     lv_obj_get_style_text_line_space(text_area, LV_PART_MAIN);
    s_clip_y1 = lc.y1 + p.y - line_h - 2;
    s_clip_scroll_y = lv_obj_get_scroll_y(text_area);
    s_clip_label_h = lv_obj_get_height(label);
    s_clip_armed = true;
}

// Encoder click toggles editing. Runs as a PREPROCESS handler so the
// textarea's own KEY handler never sees the ENTER: it used to insert a '\n'
// that was deleted again afterwards, and at the bottom of a note that pair
// started an animated scroll down and back (full-note redraws) right as the
// user began rotating to the back button.
static void encoder_enter_cb(lv_event_t *e)
{
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_ENCODER) return;
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_group_t *g = (lv_group_t *)lv_obj_get_group(obj);
    if (g) lv_group_set_editing(g, !lv_group_get_editing(g));
    lv_event_stop_processing(e);
}

static void clip_disarm()
{
    if (!s_clip_armed) return;
    s_clip_armed = false;
    if (!text_area) return;
    if (lv_obj_get_scroll_y(text_area) != s_clip_scroll_y ||
        lv_obj_get_height(lv_textarea_get_label(text_area)) != s_clip_label_h) {
        lv_obj_invalidate(text_area);
    }
}

static void do_exit()
{
    save_content_async();
    menu_show_deferred();  // switchApp → onStop → ui_text_editor_exit
}

static void exit_btn_cb(lv_event_t *)
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
    } else if (code == LV_EVENT_INSERT) {
        // iOS "." shortcut. LVGL raises INSERT before the character reaches
        // the buffer, so the cursor still sits right after the previous one.
        // The parameter is the text about to be inserted (LVGL hands '\x7F'
        // for deletes and the delete_char() call below goes through here
        // again — ignored, so it cannot disturb the tracking).
        const char *ins = (const char *)lv_event_get_param(e);
        if (!ins) return;
        uint32_t cur = lv_textarea_get_cursor_pos(obj);
        if (strcmp(ins, " ") != 0) {
            if (ins[0] != 0x7F) s_last_space_pos = UINT32_MAX;
            return;
        }
        bool consecutive = (cur == s_last_space_pos);
        s_last_space_pos = cur + 1;
        if (!consecutive || !apps::editor_cfg_get_double_space_period()) return;
        const char *txt = lv_textarea_get_text(obj);
        uint32_t byte = lv_text_encoded_get_byte_id(txt, cur);
        if (!hal::double_space_period(txt, byte)) return;
        s_last_space_pos = UINT32_MAX;          // a third space stays a space
        // Drop the first space, then make LVGL insert ". " in place of this
        // one. The order is load-bearing: insert_handler() resets
        // ta_insert_replace before each nested INSERT and only reads it after
        // our handler returns, so set_insert_replace must come after
        // delete_char — the other way round, delete_char's nested INSERT would
        // clear the replacement before it is ever consulted.
        lv_textarea_delete_char(obj);
        lv_textarea_set_insert_replace(obj, ". ");
    } else if (code == LV_EVENT_KEY) {
        clip_disarm();  // the textarea's own handler has run (clip_arm_cb)
        uint32_t key = lv_event_get_key(e);
        bool editing = lv_group_get_editing(g);

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
    // Deferred (IS.5) and through enterApp so the next app starts with an
    // empty, non-editing focus group (IS.4).
    core::System::getInstance().queueEnterApp(app_name);
}

/* Notes Sync needs a route out: WiFi (GitHub / the hub) or the phone over
 * PagerLink (H2.5). The pill is gated on that live state. Disabled state
 * blocks input and makes encoder nav skip the pill, matching the behaviour of
 * the gated tiles on the home screen. */
static bool sync_route_up()
{
    return hw_get_wifi_connected() || hw_link_connected();
}

static void apply_sync_pill_state(bool connected)
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

static void sync_pill_tick(lv_timer_t *t)
{
    (void)t;
    int now = sync_route_up() ? 1 : 0;
    if (now != sync_pill_last_state) {
        apply_sync_pill_state(now != 0);
        sync_pill_last_state = now;
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

static ui_unlock_gate_t s_unlock_gate = { NULL, editor_build_ui };

static void ui_text_editor_enter(lv_obj_t *parent)
{
    ui_build_when_unlocked(&s_unlock_gate, parent);
}

static void editor_build_ui(lv_obj_t *parent)
{
    // If it was already partially active or pointers are stale, clean up
    if (menu != NULL) {
        // If we are already here, don't do anything
        if (lv_obj_get_parent(menu) == parent) {
            return;
        }
        ui_text_editor_exit();
    }
    enable_keyboard();

    content_dirty = false;

    menu = create_menu(parent);

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
    /* Same border width in both states: border_width is a layout property, so
     * changing it on focus/defocus re-wrapped the whole note and made the
     * first scroll off the textarea (e.g. to the back button) lag. Only
     * color/opa (draw-only) change with focus. */
    lv_obj_set_style_border_color(text_area, UI_COLOR_MUTED, 0);
    lv_obj_set_style_border_width(text_area, UI_BORDER_W, 0);
    lv_obj_set_style_border_opa(text_area, LV_OPA_40, 0);
    lv_obj_set_style_border_color(text_area, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(text_area, LV_OPA_COVER, LV_STATE_FOCUSED);

    /* Cursor style */
    lv_obj_set_style_bg_color(text_area, UI_COLOR_FG, (lv_style_selector_t)LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(text_area, LV_OPA_COVER, (lv_style_selector_t)LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_width(text_area, 8, (lv_style_selector_t)LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(text_area, 0, (lv_style_selector_t)LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(text_area, 0, (lv_style_selector_t)LV_PART_CURSOR | LV_STATE_FOCUSED);

    lv_obj_add_event_cb(text_area, encoder_enter_cb,
                        (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), NULL);
    lv_obj_add_event_cb(text_area, clip_arm_cb,
                        (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), NULL);
    lv_obj_add_event_cb(text_area, text_area_event_cb, LV_EVENT_ALL, NULL);
    if (lv_display_t *disp = lv_display_get_default()) {
        lv_display_add_event_cb(disp, clip_invalidate_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }
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
    /* The pill icon is the first child of the button — cache it so the
     * route gate can recolour it without walking the tree on every tick. */
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

    /* Apply the initial route-gated state for the Sync pill and start a
     * lightweight tick that re-evaluates it — the user can flip WiFi from
     * the home screen toggles (or the phone can connect) and come back to
     * the editor without the pill going stale. */
    sync_pill_last_state = sync_route_up() ? 1 : 0;
    apply_sync_pill_state(sync_pill_last_state != 0);
    if (!sync_pill_timer) {
        sync_pill_timer = lv_timer_create(sync_pill_tick, 1000, NULL);
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

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button(exit_btn_cb, NULL);
#endif

    if (!s_pending_open_path.empty()) {
        string p;
        p.swap(s_pending_open_path);
        ui_text_editor_open_file(p.c_str());
    }

    // Pre-derive the note-encryption key for the save that fires on exit, so
    // that save only pays for the AES pass and not the PBKDF2. Called after the
    // open+decrypt above (P7.5) so the background derive doesn't compete with
    // the foreground one for the CPU. Self-guards to a no-op when notes crypto
    // is off/locked or a triple is already primed.
    notes_crypto_prewarm();
}

static void ui_text_editor_open_file(const char *path)
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

void ui_text_editor_open_file_deferred(const char *path)
{
    s_pending_open_path = path ? path : "";
    core::System::getInstance().queueEnterApp("Editor");
}

static void ui_text_editor_exit()
{
    // Persist on every teardown path (app switch, sleep, any switchApp that
    // bypasses the back button); the save runs on a worker (IS.2) so teardown
    // returns at once; a failure surfaces as a notify banner; sleep/power-off
    // paths call hw_save_flush().
    save_content_async();

    ui_hide_back_button();

    s_clip_armed = false;
    if (lv_display_t *disp = lv_display_get_default()) {
        lv_display_remove_event_cb_with_user_data(disp, clip_invalidate_cb, NULL);
    }

    if (word_count_label) {
        lv_obj_del(word_count_label);
        word_count_label = NULL;
    }

    if (sync_pill_timer) {
        lv_timer_del(sync_pill_timer);
        sync_pill_timer = NULL;
    }
    sync_pill_last_state = -1;
    ui_unlock_gate_cancel(&s_unlock_gate);

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

    ui_app_widgets_del(&menu, &quit_btn);
    text_area = NULL;
    disable_keyboard();
    current_file_path = "";
    s_last_space_pos = UINT32_MAX;  // no cross-entry double-space match
}

namespace {
class TextEditorApp : public core::App {
public:
    TextEditorApp() : core::App("Editor") {}
    void onStart(lv_obj_t *parent) override { ui_text_editor_enter(parent); }
    void onStop() override {
        ui_text_editor_exit();
        core::App::onStop();
    }
};
} // namespace

namespace apps {

// Notes editor settings, read through the settings registry (catalog id
// editor.double_space_period -> settings_table.cpp bind_editor). The NVS read
// happens once; every writer goes through the setter below, so the cache
// cannot go stale.
bool editor_cfg_get_double_space_period()
{
    if (s_dbl_space_cache < 0) {
        s_dbl_space_cache = hal::nvs_get_bool("editor", "dbl_space", true) ? 1 : 0;
    }
    return s_dbl_space_cache != 0;
}

void editor_cfg_set_double_space_period(bool on)
{
    hal::nvs_set_bool("editor", "dbl_space", on);
    s_dbl_space_cache = on ? 1 : 0;
}

APP_FACTORY(make_text_editor_app, TextEditorApp)
} // namespace apps
