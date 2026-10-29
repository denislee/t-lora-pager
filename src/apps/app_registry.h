/**
 * @file      app_registry.h
 * @brief     Factory functions for every app the System should know about.
 *
 * Each ui_*.cpp file defines a `core::App` subclass and exposes it via one of
 * the `make_*` factories declared here. `register_all()` installs them into
 * `core::AppManager` — both `factory.ino` (hardware) and `main.cpp` (emulator)
 * call it during startup so the two entry points stay in sync.
 */
#pragma once

#include "../core/app.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Expand inside an existing `namespace apps { ... }` block. Replaces the
// boilerplate `std::shared_ptr<core::App> make_X_app() { return std::make_shared<XApp>(); }`
// trailer every ui_*.cpp used to carry.
#define APP_FACTORY(fn, cls) \
    std::shared_ptr<core::App> fn() { return std::make_shared<cls>(); }

namespace apps {

std::shared_ptr<core::App> make_text_editor_app();
std::shared_ptr<core::App> make_tasks_app();
std::shared_ptr<core::App> make_sys_app();
std::shared_ptr<core::App> make_file_browser_app();
std::shared_ptr<core::App> make_journal_app();
std::shared_ptr<core::App> make_audio_notes_app();
std::shared_ptr<core::App> make_audio_recordings_app();
std::shared_ptr<core::App> make_media_remote_app();
std::shared_ptr<core::App> make_weather_app();
std::shared_ptr<core::App> make_telegram_app();
std::shared_ptr<core::App> make_notes_sync_app();
std::shared_ptr<core::App> make_ssh_app();
std::shared_ptr<core::App> make_chat_app();
std::shared_ptr<core::App> make_inbox_app();

// Unread phone messages (hal/companion.h) for the Inbox home tile badge.
int inbox_unread_count();

// Starts the persistent Telegram unread-count poll (see ui_telegram.cpp).
// Safe to call multiple times. Runs on the emulator too (phone path).
void tg_begin_background_poll();

// Total unread messages across all chats as of the last successful poll.
// Returns 0 when unconfigured, offline, or never polled. The home-menu tile
// reads this to decide whether to render a badge.
int tg_get_unread_count();

// Telegram bridge config, driven by the Settings app (Settings → Telegram).
// Reads and writes go through NVS; a running Telegram app is never on screen
// at the same time, and re-reads the config in its onStart.
std::string tg_cfg_get_url();
void        tg_cfg_set_url(const char *url);
// Masked display string ("(not set)", "(locked)", or "********abcd").
std::string tg_cfg_get_token_display();
// Cheap form for the UI thread; false = needs a decrypt (hal::secret_mask_peek).
bool tg_cfg_peek_token_display(std::string &out);
// Saves the bearer AES-wrapped under the notes passphrase. Refuses to
// persist a non-empty token unless notes crypto is enabled AND the
// session is unlocked; on refusal returns false with `*err` populated.
// Passing an empty token always clears both slots.
bool        tg_cfg_set_token(const char *tok, std::string *err = nullptr);
bool        tg_cfg_token_is_encrypted();

// Favorite chat IDs. The Telegram app's chat list filters to favorites only;
// the Settings → Telegram → Favorites subpage drives what gets stored.
bool tg_cfg_is_favorite(long long id);
// `title` is persisted alongside the id so the Telegram app can skip the
// chat list and jump straight into the favorited chat when there is only
// one. Pass nullptr/"" if no title is known; existing title is preserved.
void tg_cfg_set_favorite(long long id, const char *title, bool on);
// Every favorite id with its title ("" when unknown), ascending id.
void tg_cfg_get_favorites(std::vector<std::pair<long long, std::string>> &out);
// Replaces the whole favorites set (ids != 0) and titles, then persists.
void tg_cfg_set_favorites(const std::vector<std::pair<long long, std::string>> &favs);

// Notification channel toggles for new-message alerts raised by the
// background poll (see tg_begin_background_poll). Default ON.
bool tg_cfg_get_notif_vibrate();
void tg_cfg_set_notif_vibrate(bool on);
bool tg_cfg_get_notif_banner();
void tg_cfg_set_notif_banner(bool on);

// Notes editor: typing two spaces after a word replaces them with ". "
// (the iOS "." shortcut). NVS namespace "editor", key "dbl_space"; default ON.
bool editor_cfg_get_double_space_period();
void editor_cfg_set_double_space_period(bool on);

// Blocking fetch of the full chat list (via the phone when connected, else
// direct to the bridge), for the Favorites subpage to render; worker thread
// only. Returns false with *err populated when there is no uplink, the
// bridge is unconfigured, the token is locked, or the network call fails.
bool tg_cfg_fetch_all_chats(std::vector<std::pair<long long, std::string>> &out,
                            std::string *err = nullptr);

// GitHub notes-sync config, driven by the Settings app (Settings » Notes
// Sync). Storage lives in the "notesync" NVS namespace and mirrors the
// Telegram token pattern: the PAT is only ever stored AES-wrapped with the
// notes passphrase (hal::secret_store refuses plaintext). Default branch is
// "main".
std::string nsync_cfg_get_repo();
void        nsync_cfg_set_repo(const char *v);
std::string nsync_cfg_get_branch();
void        nsync_cfg_set_branch(const char *v);
std::string nsync_cfg_get_token_display();
// Cheap form for the UI thread; false = needs a decrypt (hal::secret_mask_peek).
bool nsync_cfg_peek_token_display(std::string &out);
// Saves the PAT AES-wrapped under the notes passphrase. Refuses to persist
// a non-empty token unless notes crypto is enabled AND the session is
// unlocked; on refusal returns false with `*err` populated. Passing an
// empty token always clears both slots.
bool        nsync_cfg_set_token(const char *tok, std::string *err = nullptr);
bool        nsync_cfg_token_is_encrypted();

void register_all();

} // namespace apps
