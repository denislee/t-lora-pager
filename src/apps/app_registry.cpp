/**
 * @file      app_registry.cpp
 */
#include "app_registry.h"
#include "../core/app_manager.h"
#include "../hal/secrets.h"

namespace apps {

void register_all() {
    auto &am = core::AppManager::getInstance();
    am.registerApp(make_text_editor_app());
    am.registerApp(make_tasks_app());
    am.registerApp(make_sys_app());
    am.registerApp(make_file_browser_app());
    am.registerApp(make_journal_app());
    am.registerApp(make_audio_notes_app());
    am.registerApp(make_audio_recordings_app());
    am.registerApp(make_media_remote_app());
    am.registerApp(make_weather_app());
    am.registerApp(make_telegram_app());
    am.registerApp(make_notes_sync_app());
    am.registerApp(make_ssh_app());
    am.registerApp(make_chat_app());
    am.registerApp(make_inbox_app());
    // Drop any plaintext token (GitHub PAT, Telegram bearer) left over from
    // a pre-encryption-required build; doing it here means it runs exactly
    // once per boot, before any UI can read the legacy slot. The namespaces
    // match NSYNC_PREFS_NS (ui_notes_sync.cpp) and TG_PREFS_NS (ui_telegram.cpp).
    hal::secret_purge_legacy("notesync", "token");
    hal::secret_purge_legacy("tgbridge", "token");
    // Start polling right after apps are registered so the badge can show
    // unread messages even before the user opens Telegram.
    tg_begin_background_poll();
    // P4.22: IMU pipeline is registered on demand by the IMU debug settings
    // subpage (build_subpage / reset_state) rather than unconditionally at boot.
    // hw_is_face_down() returns false gracefully when sensors are not streaming.
}

} // namespace apps
