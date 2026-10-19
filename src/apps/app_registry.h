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

// Starts the persistent Telegram unread-count poll (see ui_telegram.cpp).
// Safe to call multiple times. A noop on the emulator build.
void tg_begin_background_poll();

// Total unread messages across all chats as of the last successful poll.
// Returns 0 when unconfigured, offline, or never polled. The home-menu tile
// reads this to decide whether to render a badge.
int tg_get_unread_count();

// Telegram bridge config, driven by the Settings app (Settings → Telegram).
// Reads and writes go through NVS; set_* mirrors the change into a running
// Telegram app if one is currently on-screen.
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

// Notification channel toggles for new-message alerts raised by the
// background poll (see tg_begin_background_poll). Default ON.
bool tg_cfg_get_notif_vibrate();
void tg_cfg_set_notif_vibrate(bool on);
bool tg_cfg_get_notif_banner();
void tg_cfg_set_notif_banner(bool on);

// Blocking HTTP fetch of the full chat list from the bridge, for the
// Favorites subpage to render. Returns false with *err populated when the
// bridge is unconfigured, the token is locked, or the network call fails.
// Only meaningful on hardware — returns false on the emulator.
bool tg_cfg_fetch_all_chats(std::vector<std::pair<long long, std::string>> &out,
                            std::string *err = nullptr);

// GitHub notes-sync config, driven by the Settings app (Settings » Notes
// Sync). Storage lives in the "notesync" NVS namespace and mirrors the
// Telegram token pattern: the PAT is AES-wrapped with the notes passphrase
// when crypto is enabled+unlocked, plaintext otherwise. Default branch is
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
