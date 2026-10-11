# Dead code & simplification audit

Generated **2026-10-09** against **`4761042`** plus the uncommitted working tree as it stood that day
(28 modified files, `emu_shots.{h,cpp}` untracked). Line numbers refer to that working tree.
Items are numbered **`DC.x`** so they can be cited alongside the `P2.x`…`P6.x` / `PB.x` series.

**Scope.** `src/` (firmware + emulator), `server/` (lilyhub), `test/`, build config. Vendored
`lib/` was excluded except to confirm it does not reference the symbols called dead here.

**Read-only.** Nothing in the tree was edited. Builds went to `/tmp/deadscan/build`
(`PLATFORMIO_BUILD_DIR`), so `.pio/` is untouched.

---

## How this was verified

Four evidence sources were used. Every item names the one it rests on.

| Tag | Method |
|---|---|
| **[C]** compiler | `-Wunused-function -Wunused-variable -Wunused-but-set-variable -Wunused-const-variable=1 -Wunused-macros`. Run on **`emulator_lora_pager`** (gcc 16) and **`tlora_pager`** (xtensa gcc 8.4). A static symbol counts as dead only if it is flagged in **both** builds. A symbol flagged only in the emulator is used behind `#ifdef ARDUINO` (see §2.3). |
| **[L]** linker | `-ffunction-sections -Wl,--gc-sections,--print-gc-sections` on the same two builds. A function with external linkage that the **pager** link removes is unreachable on hardware. Each one was then grep-confirmed to have no other caller. |
| **[R]** review | Five parallel read-only reviewers, one per area: network apps, content apps, settings/UI/core, HAL, Go server. Each claim was checked by exhaustive grep (including `factory.ino`, `main.cpp`, `test/`) and against both sides of every `#ifdef`. Items already open in `OPTIMIZATION_*.md` are cited by ID, not re-described. |
| **[S]** spot-check | I re-read the source myself for the high-impact `[R]` claims: weather auto-IP, NFC secret logging, MP3 path, UTC+0 time zone, settings registry, watch build breakage. |

Go: `go vet ./...` is clean. `golang.org/x/tools/cmd/deadcode@latest` reports **no unreachable
functions**, with or without tests. (The installed `staticcheck` cannot read Go 1.27 export
data, so it was not used.)

---

## 0. Headline — bugs found while hunting dead code (fix these first)

These are not cleanups. They are defects that showed up as dead or unreachable code.

| ID | Problem | Where | Evidence |
|---|---|---|---|
| **DC.0.1** | **Both watch targets fail to compile at HEAD.** `twatch_ultra`: `ui_ssh.cpp:53` includes `libssh_esp32.h`, but LibSSH is only in the `tlora_pager` `lib_deps`. `twatchs3`: `settings_imu_debug.cpp:151,229` use `BoschSensorInfo`, which is BHI260-only (S3 has a BMA423). Neither break comes from the uncommitted edits. | `ui_ssh.cpp`, `settings_imu_debug.cpp` | **[C]** `pio run -e twatch_ultra` / `-e twatchs3` → `fatal error` / `not declared`. Because of this, no watch-only code path is compile-checked today. Fix: gate SSH on `ARDUINO_T_LORA_PAGER` (or a `USING_LIBSSH` capability in `board_config.h`) and the IMU firmware-info block on `USING_BHI260_SENSOR`. *Product question:* if the watches are no longer targets, dropping the two envs and `USING_TOUCHPAD` paths is the bigger simplification. |
| **DC.0.2** | **Weather "auto (IP)" mode can never run.** Choosing auto calls `p.remove("user_city")`. Both readers then return `getString("user_city", WEATHER_DEFAULT_CITY)`, which is "Sao Paulo", so `user_city.empty()` is never true. As a result `fetch_geo_ip`, `parse_geo`, `GEO_URL`, `HUB_PATH_GEO_IP` and the `"auto"` cache key are unreachable. Settings shows "(auto from IP)" only for a moment. | `ui_weather.cpp:537`, `:1170`, `:1186` | **[R][S]** Fix: store `""` as the explicit auto value (`putString("user_city","")`), or delete auto mode (~45 lines). Also collapse the duplicate `load_user_city` (:533) into `weather_get_user_city` (:1165). |
| **DC.0.3** | **NFC provisioning secret is leaked to serial and the NFC test screen.** For a Text record, `Serial.printf("…Sentence: %s")` and `diag_emit(1, …)` run **before** `nfc_provision_maybe_handle()` intercepts `lilygo+<slot>:<secret>`. The comment beside it says the payload is "never rendered". `:106` also prints the Wi-Fi password from a Wi-Fi NDEF record. | `hal/peripherals.cpp:66-76`, `:106` | **[R][S]** Move the provisioning check first and drop the password from the `printf`. |
| **DC.0.4** | **UTC+0 users get +8 h after NTP.** `cached_tz_offset_sec != 0` treats a legitimately zero offset (UTC, London and Lisbon in winter) as "unknown". It then calls `hw_start_time_sync_ntp()` with the `GMT_OFFSET_SECOND` default. | `factory.ino:213` | **[R][S]** Test `tz_offset_valid` only. A better fix is `configTzTime(timezone_get_user_posix(), …)`, which also keeps DST rules. That removes the offset cache and the "+1 h DST" approximation (~90 lines, see DC.3.9). |
| **DC.0.5** | **OTG toggle shown on watches but always fails there.** `hw_has_otg_function()` returns `true` on both `#ifdef` branches. | `hal/power.cpp:122` | **[R]** `return defined(USING_PPM_MANAGE)`. |
| **DC.0.6** | **NTP backoff doesn't match P4.19 (marked done).** Plain doubling gives 30/60/120/240/480/900 s, not 30 s→1 m→5 m→15 m. The clamp `ntp_backoff_ms < 60000 && attempts >= 2` can never be true. | `factory.ino:221-223` | **[R][S]** Use a 4-entry step table. |
| **DC.0.7** | **Watch WAV playback is a silent no-op.** On boards without `USING_AUDIO_CODEC`, a `.wav` reaches `if (!isMP3) return;`. | `hal/audio.cpp` `hw_sd_play` | **[R]** Fixed for free by DC.3.1. |
| **DC.0.8** | **Secrets left in memory.** SSH `connect_with` copies the password into `SshApp::cfg_` and never scrubs it (also not on backend error returns). The notes-sync token is not scrubbed on the Wi-Fi-offline return. The Telegram token is not trimmed on save, so a pasted trailing newline is stored. | `ui_ssh.cpp:685`, `ui_notes_sync.cpp:347-350`, `ui_telegram.cpp` `save_token` | **[R]** |
| **DC.0.9** | **A task's checked state can be lost on save.** `save_tasks` reads checkbox *widget* state instead of `t.checked`, so an item without a widget saves as unchecked, and the flush-before-teardown order is load-bearing. | `apps/ui_tasks.cpp:47` | **[R]** Read `t.checked`. |

---

## 1. Summary

| Area | Confirmed dead (lines, approx.) | Simplification (lines, approx.) | Notes |
|---|---|---|---|
| Settings / UI infra / core | ~250 | ~1,000 | Tile table, theme palette table, settings row helpers, fonts callbacks |
| Content apps (weather, journal, audio notes, tasks, files, editor, menu) | ~260 | ~350 | Dead audio "main" view, unreachable back handlers, worker pattern ×8 |
| Network apps (telegram, chat, notes sync, ssh, hub) | ~100 | ~330 | `/v1/chats` fetched 4×, secret plumbing duplicated |
| HAL | ~450 (incl. MP3 path, NFC debug dumpers) | ~250 | Task boilerplate ×5, storage I/O ×7 |
| Go server | ~10 (+5.9 MB tracked binary) | ~150 (+90 test) | Retry/backoff copy, GitHub POST helpers |
| **Total** | **~1,050** | **~2,100** | Plus `lib/libhelix-mp3/` (22 files) |

The largest low-risk wins are: DC.3.1 (MP3 path), DC.3.3 (NFC `DEBUG_NDEF`), DC.3.4 (audio
notes main view), DC.4.1 (Settings tile table), DC.4.3/4.4 (settings rows and fonts callbacks),
and DC.4.13 (lilyhub retry helper).

---

## 2. Mechanically confirmed dead code ([C] and [L] on hardware + emulator)

### 2.1 Static symbols unused on **both** the pager and the emulator build [C]

Delete outright. None is referenced anywhere in `src/` or `test/`.

| Symbol | Where | Note |
|---|---|---|
| `internet_check_click_cb` | `apps/menu_app.cpp:351` | Pill removed; also drop the "Internet check pill" header comment and `:529`. `run_internet_check`'s `btn` param is always `nullptr`. |
| `log_appendf` | `apps/ui_notes_sync.cpp:177` | `bg_log_appendf` is the live one. |
| `SshApp::update_top_bar` | `apps/ui_ssh.cpp:542` (decl), `:1207` | Comment at `:759` still points to it; the logic lives in `focus_changed_cb`. |
| `set_icon_kind`, `deg_to_compass` | `apps/ui_weather.cpp:340`, `:369` | Wind is not even requested from the API. |
| `storage_should_use_sd` | `hal/storage.cpp:679` | See DC.3.6: the whole "prefer SD" toggle is gone. |
| `float_button_event_cb` | `ui_tools.cpp:657` | `create_floating_button` itself is **live** on watches (`USING_TOUCHPAD`). |
| local `obj` | `ui_msg.cpp:15` | Unused variable. |

### 2.2 Static symbols dead on the pager but referenced on the emulator [C]

| Symbol | Where | Note |
|---|---|---|
| `fetch_chat_title` → `tg_get` → `make_url` | `apps/ui_telegram.cpp:1442-1469`, `:538-549`, `:348-355` | 0 callers on hardware; `show_default_view` now falls back to "Chat". Delete the chain plus the comments at `:1437-1441` and `:1471-1474` (~55 lines). |
| `json_nested_value`, `json_num`, `clear_forecast_cache_nvs` | `apps/ui_weather.cpp:462`, `:471`, `:710` | Also `GeoData::have_coords` (:483) and static `root` (:108) are write-only. A second copy of `struct weather_city_match` sits at `:1240`; use the one in `settings_internal.h:72`. |
| `ndefBuffer8Print`, `ndefBufferDump`, `ndefTNFNames`, `err`, `res` | `hal/nfc_reader.cpp:183`, `:240`, `:332`, `:343`, `:658` | Only reachable from `#ifdef DEBUG_NDEF`, which nothing defines. See DC.3.3. |

### 2.3 External-linkage functions removed by the **pager** linker [L], grep-confirmed

| Symbol | Where | Note |
|---|---|---|
| `core::register_settings_entry` | `core/settings_registry.cpp:20` | **0 registrations anywhere.** So `settings_entries()` is always empty, and the loop at `ui_settings.cpp:759`, `reset_settings_entries()` (`:824`) and `create_registered_entry` (`:630`) never do anything. Either delete the registry (~110 lines incl. header) or use it as the static tile table in DC.4.1. The header also says plug-ins render *before* the hand-wired tiles; the code renders them after. |
| `apps::menu::glance_show` + helper chain | `apps/menu_glance.{h,cpp}` (280 lines) | Already known (P6, PB.6). The whole TU is dead; also drop `#include "menu_glance.h"` (`menu_app.cpp:17`) and the glance comments at `:357`, `:726`, `:737`. |
| `is_screen_small` | `ui_tools.cpp:745`, `hal/display.h:21` | Only the vendor's uncompiled `examples/factory` calls it. |
| `menu_hidden` | `ui_main.cpp:101`, `core/system_hooks.h:70` | As above. |
| `hw_get_wifi_status` | `hal/wireless.cpp:198`, `wireless.h:11` | As above. |
| `hw_get_cpu_freq` | `hal/system.cpp:75`, `system.h:85` | |
| `ui_style_is_light` | `ui_theme.cpp:87`, `ui/theme.h:29` | Also `DEFAULT_OPA` (`theme.h:19`) has 0 users. |
| `core::App::onUpdate` (base) | `core/app.h` | Never overridden in practice; `AppManager::update` (`:70`) makes a no-op virtual call per tick. Low value; drop the hook or leave it. |

### 2.4 False positives: do **not** delete

Most of the emulator-only warning list is **used on hardware** inside `#ifdef ARDUINO`. Two
other cases also look dead but are live:
- **Weather:** the entire forecast-grid renderer (`render_cache` → `make_cell` →
  `make_icon_widget` → `draw_*`, `wmo_to_icon`, `day_short`, `sanitize_ascii`, `HUB_PATH_*`,
  `WEATHER_*`). Deleting it would blank the weather screen on hardware.
- **Telegram:** `render_msgs`, `poll_tick`, `start_timer`, `fire_notifications`,
  `json_text_body`, `s_*_sig`, `s_bg_timer`, `on_unlocked`, `purge_legacy_plaintext_token`.
- **Elsewhere:** `menu_app` (`kHomeAppsNs`, `make_vis_key`), `kick_notes_reload`,
  `chat current_device_id`, `CHAT_DIR`, `SEND_WATCHDOG_MS`, `ssh pw_save`/`SSH_NS`,
  `hub NS`/`hub_last_reachable`/`hub_upload_note`, `notes_crypto NC_*`, `system.cpp`
  `SETTINGS_*` etc., `gps_time_sync` transient-power helpers.
- **`ssh_session_is_active`:** called from `factory.ino:343`, which the emulator does not link.
- **`create_floating_button`:** used on watches (`USING_TOUCHPAD`).

**Simplification:** ~100 of the emulator's `-Wunused` warnings come from this pattern. Moving
those definitions inside the same `#ifdef ARDUINO` as their callers (or marking them
`[[maybe_unused]]`) would make the emulator build warning-clean. Unused-warnings could then
be turned on permanently to catch new dead code.

---

## 3. Dead code found by review (unreachable features, dead branches, write-only state)

### HAL

**DC.3.1 MP3 playback path cannot be reached [R][S]**
- **Where:** `hal/audio.cpp`: `play_mp3_with_filler`, `mp3_fill_file`, the FFat branch of `hw_sd_play`, `mp3dec.h`, and `lib/libhelix-mp3/`.
- **Evidence:** the only producer is `hw_set_sd_music_play`, and its one caller (`ui_audio_notes.cpp:660`) always passes `AUDIO_SOURCE_SDCARD` and a `.wav` file. `AUDIO_SOURCE_FATFS` and `APP_EVENT_RECOVER` are never sent.
- **Fix:** delete the decoder path, the `source_type` parameter and libhelix (~170 lines plus ~53 KB flash per P6 §1). Replace the 3 `Serial.println` lines in `hw_set_sd_music_play` with `log_d`. This also makes the MP3 part of P6.26 moot.
- **Risk:** LOW. Re-test WAV playback on the pager.

**DC.3.2 `monitor_params_t` fields [R]**
- **Where:** `hal/types.h:177-197`, `hal/power.cpp:282-310`, `hal/charge_task.cpp:228-262`.
- **Orphaned (0 references):** the 9 fields PB.14 stopped reading (`remainingCapacity` … `timeToFull`).
- **Write-only:** `type`, `temperature`, `charge_state`, `sys_voltage`, `usb_voltage`, `ntc_state`.
- **Stale justification:** the P4.7 `enableMeasure`/`disableMeasure` + `hw_invalidate_monitor_cache()` dance exists "for usb/sys voltage on the overlay/Info page", but nothing reads those values.
- **Fix:** delete the orphaned fields now (zero risk), then drop the write-only fields with their PPM ADC reads (MED; check on hardware). This supersedes the deferred §1.5 note.

**DC.3.3 NFC `DEBUG_NDEF` is never defined [C][R][S]**
- **Where:** `hal/nfc_reader.cpp`.
- **Dead blocks:** 6 blocks totalling 99 lines can never compile (`:364-407`, `:428-456`, `:473-479`, …), and the dump helpers in §2.2 are only reachable from them.
- **Constant parameter:** `verbose` is always `false` (the only call is `:96`).
- **Smaller items:** a duplicate forward declaration of `ndefMediaVCardTranslate` (`:33-34`), and a non-static global `interval` (`:129`).
- **Print-only code:** the vCard dump (~110 lines) only prints to Serial.
- **Saves:** ~180–250 lines. **Risk:** LOW (Serial output only).

**DC.3.4 GPS transient-power cluster is half-removed [R]**
- **Where:** `hal/gps_time_sync.cpp`.
- **Evidence:** since P2.5 the rail is off at boot and only `hw_start_time_sync_gps` powers it, so `s_auto_enabled` is always true. The comment at `:43-44` refers to a user setting that no longer exists.
- **Fix:** power on at start and off at finish/stop. Drop `s_auto_enabled`, the helper and possibly `hw_get_gps_powered`/`s_gps_powered` (~15 lines).

**DC.3.5 Redundant BLE keyboard guards [R]**
- **Where:** `hal/wireless.cpp:22`, `:751-861`.
- **Evidence:** `CONFIG_BLE_KEYBOARD` is `#define`d unconditionally, so 6 nested `#ifdef` blocks are no-ops. `USING_BLE_KEYBOARD` is defined for every board. The comment at `:867` ("pager builds with it unset") describes an impossible case. The name `"lilygo-pager"` is duplicated (`:746`/`:753`); use `hw_get_ble_kb_name()`.
- **Saves:** ~20 lines.

**DC.3.6 The "prefer SD" storage toggle is gone, but its shell remains [C][R]**
- **Where:** `hal/storage.cpp:679`, `hal/types.h:218`, `hal/storage.h:76,107`.
- **Evidence:** `user_setting.storage_prefer_sd` has no setter, and its one reader is the dead `storage_should_use_sd`. The header comments still say "honours the internal/SD toggle".
- **Aliases:** `hw_delete_preferred_file` duplicates `hw_delete_file`, and `hw_read_preferred_file` is a 1-line alias of `hw_read_file`.
- **Fix:** drop the field (bump `SETTINGS_VERSION`, as was done for v11/v12) and merge the aliases (~30 lines).

**DC.3.7 Write-only state and unused types [R]**
- **Write-only:** `dev_conts_var.max/min_charge_current` (`hal/internal.h:14-15`).
- **Radio params:** `radio_params_t.isRunning` is only written, and `.interval` is only read by `RADIO_LOG`, which compiles to nothing (`DEBUG_RADIO` is never defined). Related to P4.18.
- **Unused types:** `radio_tx_params_t`, `radio_rx_params_t`, `AudioParams_t` and the `RadioMode` name.
- **Identical `#ifdef` branches:** `system.cpp:323-327` sets `mic_gain` to 10 on both branches.
- **Unused includes:** `wireless.cpp` includes `SD.h`, `FFat.h` and `storage.h`.
- **Ignored argument:** `setup_secure_for_url` ignores `url`.
- **Double lock:** `hw_low_power_loop` calls `notes_crypto_lock()` twice (directly and via `hw_sleep`).
- **Constant arguments:** `hw_ping_internet` is called with `("1.1.1.1", 53, 3000)` at all 6 sites; give it defaults.

**DC.3.8 `HalResult` migration has stalled [R]**
- **Evidence:** `HalResult<T>` is used only in `test/`. Only `hub_upload_note` returns `HalError`, and 14 of the 22 non-Ok values are never produced. 8 bool + `std::string *error` APIs remain (3 in `wireless.h`, 4 in `storage.h`, 1 in `secrets.h`).
- **Choice:** either migrate the 4 storage write/copy functions, or trim the enum. §1.6 recorded the decision to keep it; this is a nudge to act on it.

### Settings / UI / core

**DC.3.9 Time zone dead branches [R][S]**
- **Where:** `ui_time_sync.cpp`, `settings_datetime.cpp`, `factory.ino`.
- **Never empty:** `timezone_get_user_tz()` falls back to `"America/Sao_Paulo"`, so `"(device default)"` (`settings_datetime.cpp:107`) and the `if (!tz.empty())` checks (`:183`, `factory.ino:204`) are dead.
- **Never fails:** `timezone_fetch_list` cannot fail, so the error branch at `:271` is dead.
- **Stale comments:** worldtimeapi references (`:34`, `:176`), and "falls back to `GMT_OFFSET_SECOND`" (`:178`, `:381`, `clear_tz_cb` text).
- **Unused constants:** `ntpServer1/2`, `gmtOffset_sec` and `daylightOffset_sec` (`factory.ino:37-40`).
- **Related bug:** see DC.0.4.

**DC.3.10 Leftover LVGL 8 branches and shims [R][S]**
- **Where:** `ui_tools.cpp:28,88,233,311,345,680`, `ui_settings.cpp:660`, `ui_journal.cpp:511`. Both environments are LVGL 9 (9.5.0 hw / 9.2.2 emu).
- **Size:** `create_msgbox`'s `#else` alone is 47 lines (`ui_tools.cpp:493-539`).
- **`ui_define.h`:** `LV_IMG_CF_ALPHA_8BIT`, `lv_point_t` and `M_PI` shims have 0 users. The other shims have 10 call sites that could move to the LVGL 9 names. The header also has no `#pragma once`.
- **Saves:** ~75 lines. (`OPTIMIZATION_PROGRESS.md` left these "NOT touched"; they are not small.)

**DC.3.11 Write-only state and unused hooks [R]**
- `System::_appGroup` is created (`system.cpp:49`) and never used.
- `editor_auto_edit` is only ever set to false (`ui_main.cpp:22/26`, `ui_text_editor.cpp:486`, `system_hooks.h:66`). Its "Editor deep-link" comment is stale.
- `LockCtx::ok_btn`/`cancel_btn` and imu `lbl.i2c_scan` are never read.
- `build()`'s `hide_cancel`/`hide_ok` are always passed the same value.
- `Notification::source` is never read.
- `g_sub_page` is write-only in `settings_hub`, `settings_telegram` and `settings_notes_sync`. Drop those three `set_sub_page`s, their declarations (`settings_internal.h:95/101/107`) and the calls (`ui_settings.cpp:572/583/594`).

**DC.3.12 Widget-factory parameters that are always the same [R]**
- `create_text` always gets `icon=NULL`, so the V2 swap branch (`ui_tools.cpp:578`) can never run and `builder_variant` has no effect.
- `create_slider` always gets `icon=NULL` and `VALUE_CHANGED`; `create_dropdown` always gets `icon=NULL`.
- `child_focus_cb` (`ui_tools.cpp:545`) is byte-identical to `toggle_child_focus_cb` (`ui_settings.cpp:487`).
- **Saves:** ~35 lines.

### Content apps

**DC.3.13 Audio notes: `VIEW_MAIN` is unreachable [R]**
- **Evidence:** startup only enters `VIEW_RECORD` (quick mode) or `VIEW_LIST`. Non-quick recording can only be started from `VIEW_MAIN` itself.
- **Dead:** `build_main_view`, `MainTile`/`kMainTiles`, `record_btn_cb`, `browse_btn_cb`, `enter_record`, `enter_main`, the `!s_quick_record` branches (`:497`, `:531`), and the `#else` of `kick_notes_reload` (`:253-255`).
- **Write-only:** `parent_obj` (`:61`). The statics `rec_status_lbl`, `rec_dot` and `pb_vol_slider` could be locals.
- **Saves:** ~130 lines.

**DC.3.14 `back_event_handler` can never fire in 4 apps [R]**
- **Where:** `ui_journal.cpp:457`, `ui_file_browser.cpp:365`, `ui_audio_notes.cpp:899`, `ui_text_editor.cpp:148`.
- **Evidence:** `create_menu` sets `LV_MENU_ROOT_BACK_BUTTON_DISABLED` and each app shows one page, so the header back button is hidden and never clickable (`lv_menu.c:292`).
- **Fix:** let `create_menu` take `nullptr`. Give each app one `do_exit()` used by ESC, the back button and the touch quit button. Tasks (`ui_tasks.cpp:429-442,254-257,277-281,477-485`) exits by sending CLICKED to a 0×0 root back button; replace that with `do_exit()`.
- **Saves:** ~60 lines.

**DC.3.15 Smaller items in the content apps [R]**
- **Tasks:** `parent_obj` (:29) is write-only, `child_label` (:386) is the same object as `label`, and `if (tasks.empty()) return;` (:354) is redundant.
- **Journal:** both branches at `:810-814` assign the same value. The second sort (`:271`) is redundant. `:88` re-implements `content_has_salted_magic` from `hal/notes_path.h`. `bg_progress_cb` (`:400`) exists only because `report()` yields only when given a callback.
- **Text editor:** `update_word_count()` after save is redundant, `word_count_label` is nulled twice (`:529`/`:561`), and `ui_text_editor_*`/`ui_tasks_*` should be `static`.
- **Media remote:** `kb_forward_on` is redundant.

### Network apps

**DC.3.16 Unreachable "app is open" branches in the Telegram setters [R]**
- **Where:** `ui_telegram.cpp:1769`, `:1788`, `:1831-1834`.
- **Evidence:** their only callers run inside the Settings app, after `TelegramApp::onStop` has already set `s_view = V_NONE`.

**DC.3.17 Settings token label: misleading branch and false comment [R]**
- **Where:** `settings_telegram.cpp:61-68`, `settings_notes_sync.cpp:47-54`.
- **Evidence:** a non-empty token can only come from `token_enc`, so `"[plaintext]"` only ever appears next to "(not set)". The comment's claim that the probe is "shared with the display" is false.

**DC.3.18 Chat and notes-sync write-only state [R]**
- `ui_chat.cpp` `s_send_task` is only written; pass `nullptr` to `xTaskCreatePinnedToCore`.
- `s_root` is write-only in both `ui_chat.cpp` and `ui_notes_sync.cpp`.
- `LocalNote::internal` is always `true` (`ui_notes_sync.cpp:374`), so the `hw_read_sd_bytes_raw` branch (`:265`) is dead.
- `sync_btn_cb` is never attached to a button; rename it `start_sync()`.

**DC.3.19 Legacy plaintext-token purge [R]**
- **Evidence:** this is one boot-time NVS RW open per namespace, run forever. `save_token` already purges on every write.
- **Fix:** retire it, or replace the per-app wrappers plus `nsync_purge_legacy_token_slot` (`app_registry.h:105`, `.cpp:27`) with two `hal::secret_purge_legacy` calls in `register_all()`.

### Go server

**DC.3.20 Dead or stale items in `server/` [R]**
- **Tracked binary:** `server/bin/lilyhub-linux-arm64` (5.9 MB) has been committed since `fd3dcd2` and never updated. `make clean` deletes the directory. Fix: `git rm --cached` it and add `server/bin/` to `.gitignore`.
- **Unread response:** the device never parses the `/api/notes/upload` response (`notessync.go:636-640` → `hub.cpp:206-238` checks status only). Reply `204` and drop `UploadResponse`, or keep it for curl debugging.
- **`safeName`:** always returns its input unchanged, so return only `bool`. Its `"."`/`".."` checks are already covered by `HasPrefix(".")`. `ensureNotesDir` is a one-line wrapper with one caller.

---

## 4. Simplification opportunities

### Settings / UI infra

**DC.4.1 Settings grid → one tile table [R] (~190 lines, LOW)**
- **Evidence:** 14 `create_subpage_*` builders (`ui_settings.cpp:238-626`) and 3 `*_click_cb`s (`:355-410`) differ only in icon, label, builder, `set_sub_page` and app name. `create_registered_entry` already renders a `SettingsEntry` generically.
- **Fix:** add `char key` and `const char *app` to `SettingsEntry`. Build one `static const SettingsEntry kTiles[]` with one generic click callback. Delete the runtime registry (DC §2.3).

**DC.4.2 Theme palette table [R] (~250 lines, MED)**
- **Spread of style checks:** `ui_style_is_beos` ×28, `is_hotel` ×25, `is_pixel` ×19, `has_chrome` ×10. 15 colour getters in `ui_theme.cpp:109-244` each run their own beos→hotel→pixel chain.
- **Copies:** `new_theme_apply_cb` (`:619-809`) is three ~60-line near-copies. `HT()` and `BE()` (`:106-107`) are the same macro.
- **Hard-coded per-style values outside `ui_theme.cpp`:** `ui_tools.cpp:441`, `:57-75`, `:379-409`, and `system.cpp:512-532` (with a literal `0xb38f00`).
- **Fix:** a `ui_palette_t[style][dark]` table plus per-style style-pointer tables, cached in `theme_init()`, and one `ui_dress_title()` helper.
- **Check:** the `UI_SHOTS_DIR` screenshot sweep covers every style.

**DC.4.3 Settings row and toggle helpers [R] (~260 lines, LOW)**
- **Copies:** key/value rows ×3 (`ui_settings.cpp:304`, `settings_info.cpp:175`, `settings_imu_debug.cpp:65`). Action rows ×6 (`settings_storage.cpp:195-262`, `settings_imu_debug.cpp:387-431`).
- **Re-implementation:** the MSC "SD/Int" toggle re-implements `create_toggle_btn_row`.
- **Repeated code:** the `" On " : " Off "` label update appears 17×, and the 6 connectivity toggles (`settings_connectivity.cpp:40-135`) share one body. `settings_home_apps.cpp:43` re-adds a callback just to attach user_data.
- **Fix:** add `settings_kv_row` and `settings_action_row`. Make `create_toggle_btn_row` take on/off text and user_data, update its own label and register its own group object (`register_subpage_group_obj` follows 64 factory calls today).

**DC.4.4 Fonts page: 18 callbacks → 2 [R] (~110 lines, LOW)**
- **Where:** `settings_fonts.cpp:36-176`.
- **Fix:** pass a pointer to the `local_param` field as user_data.
- **While there:** the two Telegram callbacks skip `lv_event_stop_processing`, and the "14 dropdowns / 7 contexts" comments are wrong (there are 18 dropdowns over 9 contexts).

**DC.4.5 Overlay open/close helper [R] (~100 lines, LOW-MED)**
- **Evidence:** `ui_wifi`, `ui_list_picker`, `ui_nfc_test` and `ui_lock` each save the group and back button, create a group, set the default group, enable the keyboard, and reverse it all on close.
- **Status-bar height:** the formula appears 5× (`system.cpp:58,206`, `ui_wifi.cpp:383`, `ui_list_picker.cpp:88`, `ui_nfc_test.cpp:270`).
- **Modal buttons:** modal-button styling is duplicated (`ui_wifi.cpp:336`, `ui_lock.cpp:288`), and the `ui_lock` copy hard-codes `0x2a1a00`, which ignores the theme.
- **USB warning:** the box (`system.cpp:392`) is a hand-built `ui_popup_create`.
- **Fix:** `ui_overlay_open/close()` and `ui_status_bar_height()` in `ui/modals.h`.

### Content apps

**DC.4.6 Background worker + 100 ms drain timer, copied 8 times [R] (~120+ lines, MED)**
- **Copies:**
  - `ui_weather`
  - `settings_weather.cpp:89-197`
  - `ui_audio_notes.cpp:178-256` (+`:988-1001`)
  - `menu_app.cpp:265-341`
  - `settings_connectivity`, `ui_telegram`, `settings_telegram`, `ui_notes_sync`
- **Shared shape:** each has a task handle, a timer, a `volatile done` flag, a heap result swapped under the instance lock, re-attach-if-running logic, and a teardown.
- **Fix:** a `core::OneShotJob<T>` (`start(fn, stack)`, `on_done(cb)`, `teardown()`).

**DC.4.7 Yes/No confirm dialog [R] (~40 lines, LOW)**
- **Where:** hand-built twice (`ui_journal.cpp:507-562`, `ui_file_browser.cpp:427-463`).
- **Evidence:** both identify the pressed button by its label text, then walk to the parent's parent to close.
- **Fix:** `ui_confirm(title, msg, cb(bool, void*), ud)` in `modals.h`. `emu_shots.cpp:183` can use it too.

**DC.4.8 Phone volume-key handler, copied twice [R] (~35 lines, LOW)**
- **Where:** `menu_app.cpp:377-398, 489-522` and `ui_media_remote.cpp:135-194`. The comment admits it "mirrors" the other.
- **Fix:** a shared `ble_volume_btn_attach()`.
- **Also:** `ui_media_remote` repeats the same style block in `make_key_button`/`volume_btn`.

**DC.4.9 Tasks / journal / file browser / menu [R] (~120 lines, LOW)**
- **Tasks:** the line parser is copied for the last line (`:305-339`). Split once and use one `parse_line()`.
- **File browser:**
  - `struct Entry` (:56) duplicates `HwDirEntry`; use it directly.
  - `file_list_key_cb` (:478) rebuilds every path to find the focused entry. Store the index in `user_data` instead.
  - The `tasks.txt` filter is duplicated with `ui_journal.cpp:204`.
- **Menu:**
  - `onStop`/`onStart` repeat the same reset; make one `reset_state()`.
  - `tile_rest_bg` == `pill_rest_bg`.
  - The three parallel badge vectors could be one struct.
  - `home_apps_set_visible` should use `hal::nvs_set_bool`.
- **List rows:** the icon + label + muted-meta row is built twice (`ui_audio_notes.cpp:428-450`, `ui_file_browser.cpp:287-318`). Add `ui_list_add_meta_row()` in `widgets.h`.

### Network apps

**DC.4.10 Telegram fetches and parses `/v1/chats` in 4 places [R] (~60 lines, MED)**
- **Copies:** `tg_fg_task` + `tg_parse_chats`, `tg_bg_task` (`:1648-1720`), `tg_cfg_fetch_all_chats` (`:1837-1891`), and the dead DC §2.2 chain.
- **Copied pieces:** `tg_cfg_fetch_all_chats` copies `require_internet()` verbatim. `tg_bg_task` calls the UI-thread-only `internet_available()` from the worker (`:1658`) and has 4 copies of its `vTaskDelete` exit.
- **Fix:** one worker-safe `fetch_chats()` through `tg_http_request`. This also closes **P6.28** ("route `tg_bg_task` through `tg_http_request`").
- **Test:** the background notifier on hardware.

**DC.4.11 Secret/token plumbing duplicated in telegram and notes_sync [R] (~70 lines, LOW)**
- **Copies:** `token_is_encrypted`, `load_token_plain`, `save_token` and `scrub_string`. The `*_cfg_get_token_display` mask is byte-identical (`ui_telegram.cpp:1775-1783` vs `ui_notes_sync.cpp:660-668`), and the `refresh_labels` token blocks are copied.
- **Fix:** add `hal::secret_mask_display(ns, key)` plus trim-on-save in `secret_store`.

**DC.4.12 Small network-app duplicates [R] (~110 lines, LOW)**
- **Telegram:**
  - `json_text_body` (`:402-423`) is a 4th JSON escaper missed by §2.18; use `hal::json_escape`. Note that `\r` is then escaped rather than dropped.
  - The floating pill is built 3× (`:1267`, `:1355`, `:1369`); add a `make_pill()` helper.
- **SSH:**
  - The `style_pill` lambda is copied inline at `:1297-1305`.
  - Saved-password auto-connect appears twice (`:640`, `:721`).
  - `run_session` repeats its 4-step error cleanup 4 times.
  - The `pw_load`/`pw_save` `#ifdef` wrappers duplicate the hal stubs.
- **Chat:**
  - `exit_cb` (`:877-884`) is `cancel_inflight()`.
  - The `send_task` exit is duplicated (`:418`, `:485`).
  - There is a nested redundant `#ifdef ARDUINO` at `:602`.
- **HAL helpers:**
  - The same trim loop exists ×5 (`hub.cpp trim_url`, `settings_hub.cpp:66`, `ui_chat.cpp:636`, `ui_notes_sync.cpp:96`, `settings_weather`). Add `hal::trim()` in `str_encode`.
  - Host/port URL parsing is duplicated (`hub_is_reachable`, `chat_post`).
  - `hub_get_url()` takes the mutex 3× where one locked read is enough.
  - `secret_store` opens NVS read-write before validating, then needs 3 `p.end()` branches.
- **Clamped encoder scroll:** appears ×4 (telegram `:1187`, journal, ssh, chat).

### HAL

**DC.4.13 `*_task.cpp` boilerplate [R] (~85 lines, LOW)**
- **Repeated code:**
  - 6 identical notify bodies.
  - 5 copies of create-pinned/`log_e`/null (~9 lines each).
  - 5 copies of the fake-sleep park, with the 1000 ms constant redefined 5×.
  - Doubled `#else` stub pairs in keyboard, rotary and nfc.
- **Keyboard and rotary also share:** `get_byte_pos` (13 lines), the drop-oldest enqueue, and the boost+wake pair.
- **Fix:** a `hal/task_util.h` containing:
  - `PinnedTask{start, notify}`
  - `park_if_fake_sleep()`
  - `queue_send_drop_oldest<T>()`
  - `hw_input_event_wake()`
  - `utf8_byte_pos()`

  Also collapse each file's guard to `#if defined(ARDUINO) && defined(USING_X)`.
- **Test:** input wake latency on hardware.

**DC.4.14 Storage I/O helpers [R] (~80 lines, LOW-MED)**
- **Read-all ×7:** open → size → resize → read → close appears in `hw_read_file`, `hw_read_internal_bytes_raw`, `hw_read_sd_bytes_raw`, `hw_read_preferred_bytes`, prune, copy-to-SD, and `notes_crypto raw_read`.
- **Write-all ×5:** open("w") → write → length check.
- **Two path normalisers:** `normalize_path` vs the `String` ternary it was meant to replace (5 uses in `storage.cpp`, plus `storage_bulk.cpp:155` and `notes_crypto.cpp:449,473`).
- **Hard-coded path:** `notes_crypto.cpp:500,512` hard-code `"/notes"`.
- **Re-implementation:** `collect_sd_protected` re-implements `list_files`.
- **Fix:** `fs_read_all` and `fs_write_all`, plus a shared `NOTES_DIR`. Fix **P6.27** (Copy Internal→SD skips `/notes`) while there.

**DC.4.15 Small HAL tables [R] (~25 lines, LOW)**
- The charge-current table is copied 4× (`power.cpp:181,195,219,234`); make it one `static constexpr`.
- `hw_is_usb_msc_*` (`storage.cpp:152-186`) can be one `#if defined(ARDUINO) && !ARDUINO_USB_MODE`.

### Go server (lilyhub)

**DC.4.16 Retry/backoff is copied in chat and notessync [R] (~55 + ~80 test lines, LOW-MED)**
- **Where:** `chat.go:401-463` and `notessync.go:41-101`. The notessync comment says so outright.
- **Fix:** `httpx.DoWithRetry(...)` plus `httpx.Truncate` (also replacing `chat.go:550`/`notessync.go:618` `truncate` and `chat.go:543` `clip`).
- **While there:** read `limit+1` and reject oversized bodies, as `httpx.Proxy` already does. Today both copies silently truncate at 1 MiB and then fail with a misleading "parse:" error (open A2).
- **Tests:** the two `fastBackoff` helpers and the duplicated 429/5xx/4xx tests collapse into one set.

**DC.4.17 Smaller lilyhub simplifications [R] (~85 lines, LOW)**
- **GitHub POST helpers:** `createBlob`/`createTree`/`createCommit` (`notessync.go:492-589`) become one `postForSHA()` (~45 lines).
- **Hand-built request:** `listRemote` (`:373-379`) should use `newGitHubRequest`.
- **Method routing:** `go.mod` is `go 1.22`, so register `"POST /api/chat"`, `"GET /api/weather/..."` etc. That drops 4 hand-written 405 checks, and weather currently accepts any method. Add `httpx.DecodeJSON` for the repeated LimitReader/DisallowUnknownFields/"bad json" blocks. `TestProxyRejectsNonPost` then needs to go through the mux.
- **JSON writer:** move chat's `writeJSON` (Content-Length + `Connection: close`, required by the ESP32 `HTTPClient`) to `httpx.WriteJSON` and use it in notessync. That is the same defect class as **P6.41**.
- **Test fixture:** `notessync_test.go` builds `&Handler{client:…, maxParallel:4}` 6 times; use one fixture.
- **Errors:** `fmt.Errorf` with no verbs → `errors.New` (`weather.go:34`, `chat.go:387`, notessync "missing sha").

---

## 5. Stale comments and docs (fix opportunistically; all [R])

**`src/`**
- `settings_internal.h:1-22`:
  - It says there are 3 split files and warns of "3,000 lines".
  - The `invert_scroll_key_cb` note (`:40`) describes keys the function doesn't handle.
- `ui/fonts.h` says the implementations are in `ui_theme.cpp`; they are in `ui_tools.cpp`.
- "see ui_define.h" should point to `modals.h`/`back_button.h` (`ui_tools.cpp:83`, `ui_lock.cpp:332`, `system.cpp:653`).
- `scoped_lock.h`, `instance_lock.cpp` and `ui_main.cpp:91` call the instance mutex the "SPI bus" lock. That contradicts `spi_lock.h` and `CLAUDE.md`.
- `ui_telegram.cpp`:
  - `:735-738` says `ascii_safe` runs on the UI thread. It actually runs on the worker (`:727`), including the font glyph lookup, so check its thread-safety on hardware.
  - `:1759` (and `ui_notes_sync.cpp:647`) say "called from ui_settings.cpp".
- `ui_notes_sync.cpp`:
  - `:30-35` describes a "stop flag, then joined" that doesn't exist.
  - `:222-227` mentions a "direct fallback" that doesn't exist.
  - `:553` mentions PUT/DELETE/download log lines that no longer exist.
- `ui_chat.cpp:110-114` justifies the watchdog with HTTPClient hangs, but `chat_post` uses a raw client.
- `ui_ssh.cpp:98` says "replace with libssh2-backed class"; that is already done.
- `ui_weather.cpp`:
  - The header (`:16-23`) and `:402` mention focusable cells and a current row that don't exist.
  - `:1208` says "see line ~707".
- `ui_audio_notes.cpp` `:3`, `:74`, `:877` and `:1044` describe the dead main view.
- `menu_app.cpp`:
  - The shortcut list (`:526-531`) is incomplete.
  - `:975` lists an `l` shortcut that doesn't exist.
  - `:999` cites "Recordings", which is not in `kItems`.
- `ui_journal.cpp`:
  - `:152` says "first 4KB"; the snippet is 1536 bytes.
  - The header claims state is reset on exit when it isn't.
- HAL:
  - `peripherals.cpp:3` mentions IR, which is gone.
  - `peripherals.cpp:167` says "from the main loop"; it now runs in `nfc_task`.
  - `nfc_task.cpp:46` says "50×/s"; the poll is 10 Hz.
  - `keyboard_task.cpp:270` mentions a stop function "if added later".
  - `storage.h:120` says "SD card root"; the code lists `/notes`.
- `settings_info.cpp:244` assumes the page is rebuilt; builders run only once.

**`server/`**
- `README.md`:
  - The Layout and Endpoints sections omit chat, notessync and telegram, plus `/api/notes/upload` and `/api/telegram/proxy`.
  - "Byte-identical weather responses" is contradicted by `trimGeoSearch`.
  - Only `GROQ_API_KEY` is documented. The README does not mention `STT_LANG*`, `STT_PROMPT`, `LILYHUB_NOTES_DIR` or `LILYHUB_ADDR`.
- Go comments:
  - `main.go:6-7` says a shared client lives there.
  - `cache.go:3` says "no eviction".
  - `notessync.go:44` says sync is "serial".
  - `safeName`'s doc comment sits above `validRepo` (`:642-650`).
  - `storeMu` cites an fsync that nothing calls.
  - `UploadRequest` claims to be the "source of truth" for sync, but that is still only a proposal (P6.33).

**`CLAUDE.md`**
- It says `radio_chip` can be switched to `CC1101`/`SX1280`/`SI4432`. But `platformio.ini` sets `RADIOLIB_EXCLUDE_CC1101`/`SX128X`/`SI443X`, so those choices don't build without editing the excludes. There is also no `si4432.cpp` driver in `src/hal/radio/`.
- It says LVGL 9.4.0; P6 §0 records 9.5.0.

---

## 6. Build config & repo hygiene

| ID | Item | Evidence |
|---|---|---|
| DC.6.1 | `twatchs3` pulls `IRremoteESP8266`, but nothing in `src/` or `lib/LilyGoLib/src` uses it. The IR code was removed in §1.4; only the vendor's `examples/IRsendDemo` references it. | **[R]** grep. Drop it from `lib_deps` once DC.0.1 makes the env build again, then confirm by building. |
| DC.6.2 | `platformio.ini:13-118` holds ~100 commented-out `src_dir = …/examples/…` lines. | Replace them with one example line plus a pointer to `lib/LilyGoLib/examples/`. Pure readability. |
| DC.6.3 | `test/test_desktop/` actually tests `str_encode`. | Rename it to `test_str_encode`. |
| DC.6.4 | `server/bin/lilyhub-linux-arm64` is a tracked 5.9 MB binary. | See DC.3.20. |
| DC.6.5 | Emulator `-Wunused` noise (~100 warnings, all `#ifdef ARDUINO` placement). | See §2.4. Once it's clean, add `-Wunused-function -Wunused-variable` to `[env_emulator]` so new dead code fails visibly. |

---

## 7. Already-tracked items re-confirmed still open (not re-described)

| ID | Item |
|---|---|
| P4.18 | The radio toggle is a no-op (enable/disable both end in `radio.sleep()`), and the `hw_radio_begin`/`hw_nrf24_begin` event groups do nothing. This is the largest remaining dead subsystem and still needs a product decision. |
| P6 / PB.6 | `menu_glance.{h,cpp}` is dead (DC §2.3). |
| P6.18 | `battery_history` is write-only (`power.cpp:20`). `hw_get_heap_info` still calls `ESP.getHeapSize()`. |
| P6.21 | `list_files` pushes mtime 0, so the `a.time != b.time` branch in 3 sort lambdas is dead and the "oldest first" prune is really alphabetical. |
| P6.24 | `hw_read_file` reads SD first; the snippet and bytes readers read internal first. |
| P6.27 | "Copy Internal → SD" skips `/notes` (`storage_bulk.cpp:128`). |
| P6.28 | Telegram's hub proxy rejects bridge URLs (DC.4.10 helps). |
| P6.33 | Notes are uploaded twice. |
| P6.41 | No Content-Length on notessync responses (DC.4.17). |
| A2 (Phase 2) | Silent 1 MiB truncation (DC.4.16). `go.mod` has no `toolchain` line. |

---

## 8. Suggested order

1. **Correctness first:** DC.0.1 (restores watch builds, which also makes the watch-only branches checkable), DC.0.3, DC.0.2, DC.0.4, DC.0.5, DC.0.9.
2. **Pure deletions (LOW risk, verified by compiler/linker):** everything in §2.1–§2.3, DC.3.3, DC.3.13, DC.3.14, DC.3.10, DC.3.11, DC.3.12. After each batch, rebuild **all three hardware envs and the emulator**, as `REFACTOR_NOTES.md` requires.
3. **Deletions that need a hardware smoke test:** DC.3.1 (WAV playback), DC.3.2 (charger monitor), DC.3.4 (GPS sync).
4. **Mechanical refactors:** DC.4.1, DC.4.3, DC.4.4, DC.4.13, DC.4.16, DC.4.17. Then DC.4.2 with a `UI_SHOTS_DIR` screenshot diff.
5. **Behavioural consolidations:** DC.4.6, DC.4.10, DC.4.14, which need hardware tests of the background workers.

---

## Appendix — reproduce

```bash
# compiler + linker dead-code signal (separate build dir; .pio untouched)
FLAGS="-Wunused-function -Wunused-variable -Wunused-but-set-variable -Wunused-const-variable=1 \
       -Wunused-macros -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--print-gc-sections"
for e in emulator_lora_pager tlora_pager twatchs3 twatch_ultra; do
  PLATFORMIO_BUILD_DIR=/tmp/deadscan/build PLATFORMIO_BUILD_FLAGS="$FLAGS" \
    pio run -e $e > /tmp/deadscan/$e.log 2>&1; echo "$e exit=$?"
done
grep 'warning:' /tmp/deadscan/tlora_pager.log | grep '^src/' | sort -u           # static dead on hw
grep 'removing unused section' /tmp/deadscan/tlora_pager.log \
  | grep 'build/tlora_pager/src/' | grep -E "'\.(text|literal)\." | c++filt         # extern dead on hw

# Go
cd server && go vet ./... && go run golang.org/x/tools/cmd/deadcode@latest -test ./...
```

---

## 9. Execution status (2026-10-09)

Executed on top of `4761042` + the uncommitted tree, in 7 gated batches (Go server; firmware 1, 2a, 2b, 3a, 3b, 4, 5, 6).
Every batch gate: all six envs build (`emulator_lora_pager`, `emulator_twatchs3`, `emulator_watch_ultra`,
`tlora_pager`, `twatchs3`, `twatch_ultra`), native tests pass, and the `UI_SHOTS_DIR` sweep matches the
previous batch below the status bar (only the clock-seeded `05-settings_page02` minute spinbox varies run to run).
Nothing committed.

**Result:** both watch envs build again. Pager flash 2,869,657 → 2,792,585 B (−77 KB), RAM 92,400 → 92,120 B.
Native tests 28 → 47. Emulator builds now compile `src/` with `-Wunused-*` (via `build_src_flags`) and have 0
unused warnings. Go: `go vet`, `go test -race`, `deadcode` clean. About 14k lines deleted across the tree
(this includes `lib/libhelix-mp3/`).

| Item | Status | Note |
|---|---|---|
| DC.0.1–0.9 | DONE | Watches kept (SSH gated on `USING_LIBSSH`, IMU info on `USING_BHI260_SENSOR`); weather auto stores `""`; TZ via `configTzTime(timezone_get_user_posix())`; NTP 4-step table; WAV plays on watches via the PCM amp (DC.0.7) |
| §2.1–2.4 | DONE | Settings registry and `menu_glance` deleted; §2.4 symbols moved under `#ifdef ARDUINO` |
| DC.3.1–3.14, 3.16–3.19 | DONE | DC.3.6 bumps `SETTINGS_VERSION` 13→14 (one settings reset on upgrade); DC.3.8 = enum trimmed; DC.3.19 = two `secret_purge_legacy` calls |
| DC.3.15 | PARTIAL | `bg_progress_cb` kept (it controls when `report()` yields to the watchdog) |
| DC.3.20 | DONE | Binary untracked (`git rm --cached`, staged), `/server/bin/` ignored; `UploadResponse` + 200 kept on purpose (older firmware) |
| DC.4.1–4.5, 4.8–4.17 | DONE | `ui_lock`/NFC-test focus fill now theme-derived (was hard-coded 0x2a1a00) |
| DC.4.6 | PARTIAL | 6 of 9 workers on `core::OneShotJob`; `ui_telegram` tg_fg/tg_inet/tg_bg and `ui_notes_sync` don't have the one-result shape. Also removes a latent UI-thread deadlock in 3 Settings callbacks (re-taking the non-recursive instance mutex) |
| DC.4.7 | DONE | except `emu_shots.cpp` (needs the msgbox handle) |
| §5, §6 | DONE | `test/test_desktop` → `test/test_str_encode` (plain `mv`; git rename not recorded yet) |
| §7 P6.27, P6.41, PB.6 | DONE | P6.28 device side done (proxy 4xx/transport → direct fallback); **hub allowlist for bridge hosts still open** |
| §7 P4.18, P6.18, P6.21, P6.24, P6.33, A2 | OPEN | out of scope (tracked elsewhere / need a product decision) |

**Still open / found during execution**
- `server/internal/telegram/telegram.go` still silently truncates proxied bodies at 1 MiB (same class as A2).
- LVGL `^9.4.0` resolves to 9.5.0 (pager, S3) but 9.6.0 (ultra); 388 deprecation warnings on ultra. Pin it?
- Auto fake-sleep on display timeout is pager-only (the vendor symbol only exists in the pager driver).
- BQ25896 ADC stays enabled while awake, but nothing in-tree reads it any more (possible power saving).

**Hardware smoke test required before release:** NTP in UTC+0 and DST zones; weather auto-IP; NFC
provisioning (nothing on serial) and tag reads; SSH connect, auth-fail and saved-password paths; WAV playback on
all three boards; charger/battery UI and the fake-sleep charge overlay; GPS time sync rail on/off; BLE keyboard,
keypad and rotary latency after the `task_util` change; every migrated OneShotJob (run, leave and re-enter,
leave and stay away) and the three ex-deadlock Settings actions; Telegram notifier with hub on and off; notes sync;
Copy Internal → SD including `/notes` and encrypted notes; notes encrypt/decrypt/change passphrase; the first boot
after flashing (settings reset because of v14).
