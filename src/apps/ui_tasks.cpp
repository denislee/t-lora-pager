/**
 * @file      ui_tasks.cpp
 * @author    LilyGo CLI Agent
 * @license   MIT
 * @date      2026-04-10
 *
 * State reset on onStop (ui_tasks_exit):
 *   widgets: menu, add_ta, task_container, quit_btn
 *                                       — destroyed via lv_obj_del then nulled
 *   timers:  save_debounce_timer        — flushed (pending write forced
 *                                          through) then lv_timer_del + null
 * No tasks or hw sessions. If you add any more timers, list them above AND
 * extend the exit path.
 */
#include "../ui_define.h"
#include <vector>
#include <string>
#include <algorithm>

using namespace std;

struct TaskItem {
    bool is_task;
    bool checked;
    string text;
    lv_obj_t *obj;
};

static lv_obj_t *menu = NULL;
static lv_obj_t *add_ta = NULL;
static lv_obj_t *task_container = NULL;
static lv_obj_t *quit_btn = NULL;
static int32_t add_ta_expanded_h = 0;
static vector<TaskItem> tasks;
static int32_t editing_idx = -1;

static const char *tasks_file_path = "/tasks.txt";

static void ui_tasks_exit();
static void do_exit();

static void save_tasks() {
    string content = "";
    for (auto &t : tasks) {
        if (t.is_task) {
            content += t.checked ? "[x] " : "[ ] ";
            content += t.text + "\n";
        } else {
            content += t.text + "\n";
        }
    }
    hw_save_preferred_file(tasks_file_path, content.c_str());
}

// Writing the whole list to flash/SD on every mutation (toggle, add, edit,
// delete) blocks the LVGL thread for 10-200 ms per event. The in-memory
// `tasks` vector is already authoritative intra-session, so the actual write
// is coalesced behind a 300 ms one-shot timer; ui_tasks_exit flushes any
// pending write synchronously so an abrupt exit never drops the last edit.
static lv_timer_t *save_debounce_timer = NULL;

static void save_debounce_cb(lv_timer_t *t) {
    (void)t;
    save_debounce_timer = NULL;
    save_tasks();
}

static void schedule_save() {
    if (save_debounce_timer) {
        lv_timer_reset(save_debounce_timer);
        return;
    }
    save_debounce_timer = lv_timer_create(save_debounce_cb, 300, NULL);
    lv_timer_set_repeat_count(save_debounce_timer, 1);
}

static void flush_pending_save() {
    if (save_debounce_timer) {
        lv_timer_del(save_debounce_timer);
        save_debounce_timer = NULL;
        save_tasks();
    }
}

static void ui_tasks_refresh();
static void rebuild_task_list();

static void task_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);

    if (code == LV_EVENT_SHORT_CLICKED) {
        // Short-click opens the task for editing. SHORT_CLICKED (not CLICKED)
        // is required because LVGL still fires CLICKED on release after a
        // long-press — using CLICKED would toggle complete and immediately
        // open the editor on the same gesture.
        for (size_t i = 0; i < tasks.size(); ++i) {
            if (tasks[i].obj == obj && tasks[i].is_task) {
                editing_idx = (int32_t)i;
                if (add_ta) {
                    lv_textarea_set_text(add_ta, tasks[i].text.c_str());
                    lv_group_focus_obj(add_ta);
                }
                break;
            }
        }
    } else if (code == LV_EVENT_LONG_PRESSED) {
        // Long-press toggles completion; checkbox auto-toggle is disabled.
        for (auto &t : tasks) {
            if (t.obj == obj && t.is_task) {
                t.checked = !t.checked;
                if (t.checked) lv_obj_add_state(obj, LV_STATE_CHECKED);
                else lv_obj_remove_state(obj, LV_STATE_CHECKED);
                string icon = t.checked ? LV_SYMBOL_OK : LV_SYMBOL_MINUS;
                string display_text = icon + "  " + t.text;
                lv_obj_t * label = lv_obj_get_child(obj, 0);
                if (label) lv_label_set_text(label, display_text.c_str());
                schedule_save();
                break;
            }
        }
    } else if (code == LV_EVENT_FOCUSED || code == LV_EVENT_DEFOCUSED) {
        lv_obj_t * label = NULL;
        if (lv_obj_has_class(obj, &lv_checkbox_class)) {
            label = lv_obj_get_child(obj, 0);
        } else if (lv_obj_has_class(obj, &lv_label_class)) {
            label = obj;
        }

        if (label) {
            if (code == LV_EVENT_FOCUSED) {
                lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            } else {
                lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
            }
        }
    } else if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);
        if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
            // Find the task and remove it
            size_t focus_idx = 0;
            bool found = false;
            for (auto it = tasks.begin(); it != tasks.end(); ++it) {
                if (it->obj == obj) {
                    tasks.erase(it);
                    found = true;
                    break;
                }
                focus_idx++;
            }
            if (found) {
                schedule_save();
                rebuild_task_list();
                if (!tasks.empty()) {
                    if (focus_idx >= tasks.size()) {
                        focus_idx = tasks.size() - 1;
                    }
                    if (tasks[focus_idx].obj) {
                        lv_group_focus_obj(tasks[focus_idx].obj);
                    }
                } else if (add_ta) {
                    lv_group_focus_obj(add_ta);
                }
            }
        }
    }
}

static void set_add_ta_collapsed(lv_obj_t *ta, bool collapsed) {
    if (!ta) return;
    if (collapsed) {
        if (add_ta_expanded_h == 0) {
            lv_obj_update_layout(ta);
            add_ta_expanded_h = lv_obj_get_height(ta);
        }
        lv_obj_set_height(ta, 0);
        lv_obj_set_style_pad_ver(ta, 0, 0);
        lv_obj_set_style_border_width(ta, 0, 0);
        lv_obj_set_style_opa(ta, LV_OPA_TRANSP, 0);
    } else {
        lv_obj_set_height(ta, add_ta_expanded_h > 0 ? add_ta_expanded_h : LV_SIZE_CONTENT);
        lv_obj_remove_local_style_prop(ta, LV_STYLE_PAD_TOP, 0);
        lv_obj_remove_local_style_prop(ta, LV_STYLE_PAD_BOTTOM, 0);
        lv_obj_remove_local_style_prop(ta, LV_STYLE_BORDER_WIDTH, 0);
        lv_obj_remove_local_style_prop(ta, LV_STYLE_OPA, 0);
    }
}

static void add_ta_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    lv_group_t *g = (lv_group_t *)lv_obj_get_group(ta);

    if (code == LV_EVENT_FOCUSED) {
        set_add_ta_collapsed(ta, false);
        // Match the Telegram composer's pattern: clear editing on focus so the
        // physical keyboard delivers characters straight to the textarea
        // (e.g. focus coming back from a task row that was left in edit mode).
        // App entry already starts non-editing (IS.4).
        if (g) lv_group_set_editing(g, false);
    } else if (code == LV_EVENT_DEFOCUSED) {
        set_add_ta_collapsed(ta, true);
    } else if (code == LV_EVENT_CLICKED) {
        // Ensure focus but explicitly stay out of editing mode
        lv_group_focus_obj(ta);
        lv_group_set_editing(g, false);
    } else if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);

        if (key == LV_KEY_ENTER) {
            const char *txt = lv_textarea_get_text(ta);
            if (editing_idx >= 0 && editing_idx < (int32_t)tasks.size()) {
                if (strlen(txt) > 0) {
                    tasks[editing_idx].text = txt;
                }
                size_t focus_idx = (size_t)editing_idx;
                editing_idx = -1;
                schedule_save();
                lv_textarea_set_text(ta, "");
                rebuild_task_list();
                if (focus_idx < tasks.size() && tasks[focus_idx].obj) {
                    lv_group_focus_obj(tasks[focus_idx].obj);
                }
            } else if (strlen(txt) > 0) {
                // Add new task
                TaskItem t;
                t.is_task = true;
                t.checked = false;
                t.text = txt;
                t.obj = NULL;
                tasks.insert(tasks.begin(), t); // Add at the top
                schedule_save();
                lv_textarea_set_text(ta, "");
                rebuild_task_list();
            }
            // Always stop Enter from reaching the default handler to prevent "entering" editing mode
            lv_event_stop_processing(e);
            return;
        }

        if (key == LV_KEY_ESC) {
            if (editing_idx >= 0) {
                size_t focus_idx = (size_t)editing_idx;
                editing_idx = -1;
                lv_textarea_set_text(ta, "");
                if (focus_idx < tasks.size() && tasks[focus_idx].obj) {
                    lv_group_focus_obj(tasks[focus_idx].obj);
                }
                lv_event_stop_processing(e);
                return;
            }
            lv_event_stop_processing(e);
            do_exit();
            return;
        }

        // For all other keys (printable chars, backspace, navigation), 
        // let the default textarea handler deal with them. 
        // Since we stay in "editing: false" state, LVGL handles character insertion 
        // from physical keyboards automatically without entering visual editing mode.
    }
}

// Single exit path for ESC, the status-bar back button and the touch quit
// button.
static void do_exit() {
    menu_show_deferred();  // onStop runs ui_tasks_exit
}

static void tasks_back_cb(lv_event_t *e) {
    do_exit();
}

// "[ ] text" → open task, "[x] text" / "[X] text" → done task, anything
// else → a plain (non-task) line kept verbatim.
static TaskItem parse_line(const string &line) {
    TaskItem t;
    t.is_task = false;
    t.checked = false;
    t.obj = NULL;
    if (line.rfind("[ ] ", 0) == 0) {
        t.is_task = true;
        t.text = line.substr(4);
    } else if (line.rfind("[x] ", 0) == 0 || line.rfind("[X] ", 0) == 0) {
        t.is_task = true;
        t.checked = true;
        t.text = line.substr(4);
    } else {
        t.text = line;
    }
    return t;
}

static void ui_tasks_refresh() {
    if (task_container == NULL) return;
    tasks.clear();

    string content;
    if (!hw_read_file(tasks_file_path, content)) {
        content = "";
    }

    // One item per line ("\n" or "\r\n"; the last line may lack the
    // terminator). Blank lines are dropped.
    size_t start = 0;
    while (start < content.size()) {
        size_t end = content.find('\n', start);
        if (end == string::npos) end = content.size();
        string line = content.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) tasks.push_back(parse_line(line));
    }

    rebuild_task_list();
}

// Rebuilds the on-screen list from the in-memory `tasks` vector only — no
// file I/O. Used both by ui_tasks_refresh (after a cold-start file parse)
// and directly by mutation handlers (toggle/add/edit/delete), which already
// keep `tasks` authoritative and would otherwise force a redundant
// write-then-reread of the file they just wrote.
static void rebuild_task_list() {
    if (task_container == NULL) return;
    lv_obj_clean(task_container);

    for (auto &t : tasks) {
        if (t.is_task) {
            t.obj = lv_checkbox_create(task_container);
            lv_obj_set_width(t.obj, LV_PCT(100));
            lv_checkbox_set_text(t.obj, ""); // Clear built-in text
            
            // Create a separate label for the text to support scrolling
            lv_obj_t * label = lv_label_create(t.obj);
            lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP); // Default to CLIP
            lv_obj_set_width(label, LV_PCT(100));
            lv_obj_set_align(label, LV_ALIGN_LEFT_MID);

            // Hide the default checkbox indicator box to make it a clean list
            lv_obj_set_style_opa(t.obj, LV_OPA_TRANSP, LV_PART_INDICATOR);
            lv_obj_set_style_width(t.obj, 0, LV_PART_INDICATOR);
            lv_obj_set_style_pad_all(t.obj, 0, LV_PART_INDICATOR);
            lv_obj_set_style_margin_all(t.obj, 0, LV_PART_INDICATOR);
            lv_obj_set_style_pad_left(t.obj, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_column(t.obj, 0, LV_PART_MAIN);

            string icon = t.checked ? LV_SYMBOL_OK : LV_SYMBOL_MINUS;
            string display_text = icon + "  " + t.text;
            lv_label_set_text(label, display_text.c_str());
            
            lv_obj_set_style_bg_color(t.obj, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
            lv_obj_set_style_bg_opa(t.obj, LV_OPA_20, LV_STATE_FOCUSED);

            // Visual feedback when the task is done (checked state)
            lv_obj_set_style_text_decor(label, LV_TEXT_DECOR_STRIKETHROUGH, LV_STATE_CHECKED);
            lv_obj_set_style_text_color(label, UI_COLOR_MUTED, LV_STATE_CHECKED);

            if (t.checked) {
                lv_obj_add_state(t.obj, LV_STATE_CHECKED);
            }
            // Disable the built-in click-to-toggle. Click opens the editor;
            // long-press is the explicit gesture for marking complete.
            lv_obj_remove_flag(t.obj, LV_OBJ_FLAG_CHECKABLE);
            lv_obj_add_event_cb(t.obj, task_event_cb, LV_EVENT_ALL, NULL);
            lv_group_add_obj(lv_group_get_default(), t.obj);
        } else {
            t.obj = lv_label_create(task_container);
            lv_obj_set_width(t.obj, LV_PCT(100));
            lv_label_set_long_mode(t.obj, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_pad_left(t.obj, 0, LV_PART_MAIN);
            
            string display_text = string(LV_SYMBOL_BULLET) + " " + t.text;
            lv_label_set_text(t.obj, display_text.c_str());
            lv_obj_set_style_text_color(t.obj, UI_COLOR_MUTED, 0);

            // Make non-task items focusable so they can scroll on highlight
            lv_obj_add_flag(t.obj, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_border_color(t.obj, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
            lv_obj_set_style_border_width(t.obj, UI_BORDER_W, LV_STATE_FOCUSED);
            lv_obj_set_style_bg_color(t.obj, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
            lv_obj_set_style_bg_opa(t.obj, LV_OPA_20, LV_STATE_FOCUSED);
            
            lv_obj_add_event_cb(t.obj, task_event_cb, LV_EVENT_ALL, NULL);
            lv_group_add_obj(lv_group_get_default(), t.obj);
        }
    }
}

static void ui_tasks_enter(lv_obj_t *parent) {
    if (menu != NULL) return;
    enable_keyboard();

    menu = create_menu(parent);
    ui_show_back_button(tasks_back_cb);

    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(main_page, 0, 0); // Remove padding to flush left

    add_ta = lv_textarea_create(main_page);
    lv_textarea_set_one_line(add_ta, true);
    lv_textarea_set_placeholder_text(add_ta, "[new task]");
    lv_obj_set_width(add_ta, LV_PCT(100));

    lv_obj_set_style_text_color(add_ta, UI_COLOR_MUTED, LV_PART_TEXTAREA_PLACEHOLDER);

    lv_obj_add_event_cb(add_ta, add_ta_event_cb, LV_EVENT_ALL, NULL);
    lv_group_add_obj(lv_group_get_default(), add_ta);

    task_container = lv_obj_create(main_page);
    lv_obj_set_width(task_container, LV_PCT(100));
    lv_obj_set_flex_grow(task_container, 1);
    lv_obj_set_flex_flow(task_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(task_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(task_container, 0, 0);
    lv_obj_set_style_pad_all(task_container, 0, 0);

    ui_tasks_refresh();

    lv_menu_set_page(menu, main_page);
    // enterApp hands us an empty, non-editing group (IS.4), and add_ta's
    // FOCUSED handler clears editing again, so the first key reaches it.
    lv_group_focus_obj(add_ta);

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button(tasks_back_cb, NULL);
#endif
}

static void ui_tasks_exit() {
    ui_hide_back_button();
    disable_keyboard();
    // save_tasks() serializes the `tasks` model (not widget state); flush
    // before tasks.clear() below.
    flush_pending_save();
    if (menu) {
        lv_obj_clean(menu);
        lv_obj_del(menu);
        menu = NULL;
    }
    if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
    add_ta = NULL;
    task_container = NULL;
    add_ta_expanded_h = 0;
    editing_idx = -1;
    tasks.clear();
}

#include "app_registry.h"

namespace {
class TasksApp : public core::App {
public:
    TasksApp() : core::App("Tasks") {}
    void onStart(lv_obj_t *parent) override { ui_tasks_enter(parent); }
    void onStop() override {
        ui_tasks_exit();
        core::App::onStop();
    }
};
} // namespace

namespace apps {
APP_FACTORY(make_tasks_app, TasksApp)
} // namespace apps

