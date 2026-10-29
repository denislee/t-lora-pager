/**
 * @file      ui_companion.h
 * @brief     The pager's side of phone messaging (PROTOCOL.md §6.5): banners,
 *            typing phone text, "find pager", and the pager → phone calls.
 *
 * Shared UI infrastructure, not an app (like ui_msg.cpp). The link handlers
 * (hal/link/handlers_notify.cpp) call the "any task" functions from the link
 * task; each one only flags or posts a closure to the LVGL thread
 * (core::ui_post), so none of them blocks, takes the instance lock or touches
 * LVGL directly — taking the instance lock from the link task would deadlock
 * against the LVGL thread calling into the link service.
 *
 * LVGL-free header.
 */
#pragma once

#include <functional>
#include <string>

#include "hal/companion_msg.h"

/* --- any task ------------------------------------------------------------ */

/** Show `e` per `d` (banner + haptic, waking the display first if asked). */
void ui_companion_deliver(const hal::msg::InboxEntry &e, const hal::msg::Delivery &d);

/** True while the note editor has registered its text sink (it is open). */
bool ui_companion_editor_open();

/** True when a text field had keypad focus at the last UI loop pass (a copy
 * kept by ui_companion_refresh_focus(), so the link task can read it). */
bool ui_companion_text_input_focused();

/** Type `text` into the open editor (Editor) or the focused text field (Kb).
 * Posted; a target that went away in between drops the text with a banner. */
void ui_companion_type(hal::msg::Target target, const std::string &text);

/** pager.find: wake, sticky banner and a buzz every 2 s for 30 s (start), or
 * stop the buzzing early. */
void ui_companion_find_pager(bool start);

/** Whether this board can wake its display from fake sleep for a message. */
bool ui_companion_can_wake();

/** screen.capture (ui_screen_capture.cpp): snapshot on the LVGL thread, then
 * screen.data + DATA from a worker. False — nothing started — while a capture
 * is still running. */
bool ui_screen_capture_start(const std::string &job);

/* --- LVGL thread --------------------------------------------------------- */

/** The editor registers where phone text goes while it is open; an empty
 * function unregisters (editor exit). */
void ui_companion_set_editor_sink(std::function<void(const std::string &)> sink);

/** Called once per UI loop pass (core::System::loop) to refresh the focus copy. */
void ui_companion_refresh_focus();

/** clip.push the given text to the phone on a worker; the outcome is a banner. */
void ui_companion_send_clip(const std::string &text);

/** phone.find on a worker; the outcome is a banner. */
void ui_companion_find_phone();

/** True while a phone→pager-initiated call is running (buttons show busy). */
bool ui_companion_call_busy();
