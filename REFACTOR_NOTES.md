# Refactor Notes — Production-Hardening Pass

> Handoff document. Records what has been done in the `core::` / `hal/` /
> `apps/` reorganization so far, and what remains so a fresh Claude Code
> session can pick up without re-learning the terrain.

## Current baseline

- **Hardware build** (`pio run -e tlora_pager`): last verified SUCCESS,
  RAM 23.5%, Flash 65.1%.
- **Emulator build** (`pio run -e emulator_lora_pager`): last verified
  SUCCESS.
- Git: on `master`, recent commits `4d73fa4`, `0c12dd5`, `aabd43f`,
  `6bd1307` are the refactor commits.

After each change in this arc, both builds were rebuilt and verified green
before moving on. Any future split should do the same.

---

## What has been done

### 1. Cross-module architecture (`src/core/`)

| File | Purpose |
|---|---|
| `core/app.h` | `core::App` abstract base — `onStart` / `onStop`. |
| `core/app_manager.{h,cpp}` | Singleton registry + `switchApp(name, parent)` / `queueSwitchApp`. |
| `core/system.{h,cpp}` | Singleton owning the global UI skeleton, status bar, menu/app panels; drives the main loop via `loop()` → `AppManager::update()`. |
| `core/scoped_lock.h` | `core::ScopedInstanceLock` — RAII wrapper around our own top-level task-coordination mutex. |
| `core/instance_lock.cpp` | Backing storage for the mutex. `core::instance_lock_init()` is called once from `factory.ino::setup()`. Emulator build compiles a no-op branch. |
| `core/spi_lock.{h,cpp}` | `core::ScopedSpiLock` — RAII wrapper around the **vendor's** `instance.lockSPI()/unlockSPI()` (distinct from the mutex above; this one guards the shared SPI bus on T-LoRa-Pager / T-Watch-Ultra, no-op on T-Watch-S3). |
| `core/system_hooks.h` | **Canonical** cross-module entry points: `ui_is_fake_sleep`, `ui_pause/resume_timers`, `ui_lock/unlock`, `ui_request_editor_switch`, `editor_auto_edit`, `menu_show/hidden`, `isinMenu`, `instance_lock_init`. LVGL-free so HAL TUs can include it cheaply. Replaced the scattered `extern bool ui_is_fake_sleep();` declarations that used to drift across `factory.ino`, `hal/lvgl_task.cpp`, and `hal/nfc_task.cpp`. |
| `core/input_focus.{h,cpp}` | Existed; not modified. |

### 2. HAL split (`src/hal/`)

- Domain headers (`types.h`, `system.h`, `display.h`, `power.h`,
  `storage.h`, `audio.h`, `wireless.h`, `radio.h`, `sensors.h`,
  `peripherals.h`, `board_config.h`) replaced the old monolithic
  `hal_interface.h` (kept as umbrella for back-compat).
- **`hal/radio/`** — per-chip drivers moved here from `src/` root:
  `sx1262.cpp`, `sx1280.cpp`, `cc1101.cpp`, `lr1121.cpp`, `nrf2401.cpp`.
  Each gated by its `ARDUINO_LILYGO_LORA_<chip>` define and implements
  the `radio_chip::` API declared in `hal/radio_chip.h`.
- **`hal/nfc_reader.{cpp,h}`** — NFC NDEF parser + ST25R3916 reader.
  Was `src/app_nfc.{cpp,h}`; renamed because it's a driver, not an app.
- `hal/internal.h` gained `hw_radio_begin()` and `hw_nrf24_begin()` — these
  were previously loose `extern void` declarations inside
  `hal/system.cpp`.
- `hal_interface.h` **no longer** has `using std::string; using std::vector;`
  at the bottom — that used to leak into every consumer of the umbrella.
  Files that still use unqualified `string`/`vector` now have file-local
  `using std::string;` declarations: `ui_msg.cpp`, `ui_text_editor.cpp`,
  `ui_settings.cpp`, `hal/system.cpp`. `ui_tasks.cpp` already had
  `using namespace std;` so no change needed there.
- `hal/storage.cpp` — the `extern bool is_usb_msc_{reading,writing,mounted}();`
  block now has a comment explaining these hook into vendor
  `lib/LilyGoLib/src/USB_MSC.cpp` which exposes no header.

### 3. Apps directory (`src/apps/`)

- `apps/app_registry.{h,cpp}` — `make_*_app()` factories + `register_all()`
  helper. Called from both `factory.ino` and `main.cpp`.
  Includes `APP_FACTORY(fn, cls)` macro that every ui_*.cpp uses to
  declare its factory in one line.
- `apps/menu_app.{h,cpp}` — home screen.
- **Every `core::App` implementation lives here.** The ui_*.cpp files
  that define app subclasses were moved out of `src/` root:
  `ui_audio_notes.cpp`, `ui_file_browser.cpp`, `ui_journal.cpp`,
  `ui_media_remote.cpp`, `ui_notes.cpp`,
  `ui_notes_sync.cpp`, `ui_settings.cpp`, `ui_tasks.cpp`,
  `ui_telegram.cpp`, `ui_text_editor.cpp`, `ui_weather.cpp`.
  Shared UI infrastructure (ui_main, ui_theme, ui_tools, ui_power,
  ui_msg, ui_lock, ui_wifi, ui_time_sync, ui_nfc_test,
  ui_list_picker) stays in `src/` — these aren't `core::App`
  subclasses.
- **Settings app split** (see section 4 below).

### 4. `ui_settings.cpp` decomposition

Originally 2829 lines. After 10 extractions: **624 lines** (**78% smaller**).
One private header shared across the split TUs.

| Extracted file | Lines | Namespace | Notes |
|---|---|---|---|
| `apps/settings_internal.h` | 160 | — | Private header. See section 5. |
| `apps/settings_weather.cpp` | 138 | `weather_cfg` | City picker + open-meteo geocoding. |
| `apps/settings_telegram.cpp` | 285 | `telegram_cfg` | URL/token config + Favorites subpage. |
| `apps/settings_notes_sync.cpp` | 167 | `notes_sync_cfg` | GitHub repo/branch/PAT config. |
| `apps/settings_fonts.cpp` | 223 | `fonts_cfg` | 14 dropdowns (7 contexts × face/size). |
| `apps/settings_datetime.cpp` | 374 | `datetime_cfg` | Spinbox manual entry + IANA TZ picker + NTP sync. |
| `apps/settings_info.cpp` | 205 | `info_cfg` | Read-only status rows + 1 Hz refresh timer. |
| `apps/settings_connectivity.cpp` | 252 | `connectivity_cfg` | WiFi/BT/Radio/NFC/GPS/Speaker/Haptic toggles + WiFi Networks / Test Internet / NFC Test buttons whose visibility follows their toggle. |
| `apps/settings_display.cpp` | 338 | `display_cfg` | Display & Backlight + Charger + Performance bundled — three `build_*` entry points, no cached state, no `reset_state`. |
| `apps/settings_storage.cpp` | 413 | `storage_cfg` + `notes_sec_cfg` | Storage subpage + Notes Security subpage bundled — both share the `storage_loader` popup triad for long-running fs ops. notes_sec rebuilds its page in place via `unregister_subpage_items_for` after each passphrase flow. |

Other cleanup done alongside: removed `get_ip_id` (declared, never read).

### 5. `apps/settings_internal.h` contract

**Private** — do not include from anywhere except
`ui_settings.cpp` and the `settings_*.cpp` split files.

Shared helpers (implemented in `ui_settings.cpp`, external linkage):
- `register_subpage_group_obj(page, obj)` — append a widget to the
  subpage's focus-group tracking.
- `unregister_subpage_items_for(page)` — remove all focus-group entries
  for `page`. Called by notes_sec before rebuilding its page in place.
- `activate_subpage_group(page)` — re-populate `menu_g` with the subpage's
  registered widgets, skipping hidden ones.
- `create_toggle_btn_row(parent, txt, initial, cb)` — themed toggle row.
- `toggle_child_focus_cb(e)` — paints the parent row as FOCUSED when an
  inner button gets focus. Used by storage's custom (non-toggle-row) buttons.
- `invert_scroll_key_cb(e)` — maps encoder/keyboard scroll keys onto
  spinbox/slider inc-dec. Used by datetime, backlight, charger.
- `settings_return_to_main_page()` — pops the menu back to the root.

Shared state:
- `extern user_setting_params_t local_param` — the working-copy settings
  blob. Populated by `ui_sys_enter` from NVS, written back by
  `ui_sys_exit`. Split subpages mutate fields on this struct and call
  `hw_set_user_setting(local_param)` to persist eagerly (crash-safety).

Cross-TU forward decls:
- `weather_*` + `weather_city_match` — defined in `ui_weather.cpp`.
- `timezone_*` — defined in `ui_time_sync.cpp`.

Per-subpage entry points, every namespace exposes:
- `void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);` (stored as
  `lv_obj`'s user_data, invoked by `settings_page_changed_cb`)
- `void reset_state();` (called from `ui_sys_exit` to null cached LVGL
  pointers before the menu is destroyed)
- `void set_sub_page(lv_obj_t *page);` (where the subpage needs to track
  its own page object — not all of them do, e.g. fonts/info don't)

### 6. Other hygiene

- Moved the instance-mutex state out of `factory.ino` — the `.ino` file is
  now thin Arduino-shell glue, real state lives in `core/instance_lock.cpp`.
- `main.cpp` (emulator) no longer defines no-op `instanceLockTake/Give`
  stubs locally; the `#ifdef ARDUINO` branch in `core/instance_lock.cpp`
  handles it.
- Removed redundant `extern void hw_init();` from `main.cpp` — already
  declared in `hal/system.h`.
- Removed the empty `src/ui/common/` + `src/ui/` directories.
- Converted 4 `instance.lockSPI()/unlockSPI()` sites in
  `ui_audio_notes.cpp` to `core::ScopedSpiLock`. Dozens more across
  `hal/storage.cpp` and `hal/audio.cpp` were left as-is because they use
  a lock-flag pattern that needs signature-level changes to convert
  safely.
- Collapsed the 12 identical `make_*_app()` factories in `ui_*.cpp` to
  a single `APP_FACTORY(fn, cls)` macro in `apps/app_registry.h`. The
  4 files that didn't already include the registry header now do.

---

## Hardening pass — 2026-05-16

The deferred items from the original audit (and a few more identified
during recommendation) have now been addressed. Summary:

### Completed

1. **Vendor SPI lock RAII** — every `instance.lockSPI()` /
   `unlockSPI()` pair (113 sites across storage/audio/wireless/
   notes_crypto/radio_common/radio/nrf2401) converted to
   `core::ScopedSpiLock` (unconditional) or `core::MaybeSpiLock`
   (conditional, replacing the `bool lock = false` patterns).
   `core/spi_lock.{h,cpp}` gained `MaybeSpiLock` for the
   "lock only on the SD branch" idiom in storage and notes_crypto.

2. **Native unit tests** — `[env:native_test]` added to
   `platformio.ini`. Tests live under `test/`. Pure-logic helpers
   extracted into `hal/notes_path.{h,cpp}` (path protection rules,
   Salted__ magic detection) so they compile without Arduino/mbedtls
   and can be exercised by Unity. Also covers `hal_error_string()`
   for completeness across HalError variants.
   Run with: `pio test -e native_test`.

3. **Large-file splits** —
   - `apps/menu_app.cpp` (1191 → 922): extracted ~270-line glance
     overlay to `apps/menu_glance.{h,cpp}`.
   - `hal/storage.cpp` (1558 → 1342): extracted ~180-line bulk-copy
     functions (`hw_copy_all_notes_to_hub`, `hw_copy_internal_to_sd`)
     to `hal/storage_bulk.cpp`. Clean cut — only uses public storage.h
     + hub.h APIs, no static helpers.
   - **Remaining splits to do** (still 1k+):
     * `apps/ui_telegram.cpp` (1577) — file statics tightly coupled,
       needs an internal header to share UI state across split TUs.
     * `apps/ui_weather.cpp` (1333) — fetch/HTTP layer is a coherent
       chunk that could extract.
     * `apps/ui_ssh.cpp` (1252) — `SshBackend` + `LibSshBackend` +
       `AnsiFilter` could form `ssh_backend.{h,cpp}` (~340 lines).
     * `hal/storage.cpp` further: the "preferred" file family
       (~300 lines) could extract but needs `storage_internal.h` for
       `FileInfo`/`list_files`/`encode_for_write`.

4. **Narrowed `ui_define.h` transitive includes** — dropped `WiFi.h`
   (now arrives via `hal/types.h`) and `esp_mac.h` (only `hal/system.cpp`
   needed it; moved there). Also split the file into focused sub-headers
   under `src/ui/`:
   - `ui/theme.h` — color tokens + theme_init
   - `ui/fonts.h` — per-context font getters
   - `ui/widgets.h` — themed lv_*_create wrappers
   - `ui/modals.h` — popups, message boxes, loading, prompts
   - `ui/back_button.h` — shared status-bar back button
   `ui_define.h` is now a thin aggregator that re-exports them all for
   back-compat. New code should `#include` the focused header instead.

5. **MSG_ macros → enum class** — the unused `MSG_MENU_NAME_CHANGED`,
   `MSG_LABEL_PARAM_CHANGE_*`, `MSG_TITLE_NAME_CHANGE`,
   `MSG_BLE_SEND_DATA_*`, `MSG_MUSIC_TIME_*`, `MSG_FFT_ID` defines
   were dead code; deleted. `LV_MENU_ITEM_BUILDER_VARIANT_*` is now an
   `enum class lv_menu_builder_variant_t : uint8_t { V1, V2 }` with
   the old upper-case names retained as `constexpr` aliases so the
   ~10 call sites don't churn.

6. **Modal context RAII** — `ChangeCtx`, `PickerCtx`, `LockCtx` now
   use `std::unique_ptr`. The teardown path moves ownership into a
   local so `g_ctx` is null while the user callback runs — same
   re-entry safety as the original `delete` ordering but exception-
   safe and self-documenting. `ChangeCtx` rides the void* user-data
   slot via release()/reset() across the prompt-chain callbacks.

7. **App lifecycle invariants documented** — every `apps/ui_*.cpp`
   has a "State reset on onStop" comment block at the top listing
   cached `lv_obj_t*`, `lv_timer_t*`, and FreeRTOS tasks that the
   exit path must release. If you add a new cached pointer or task,
   list it in the block AND extend the exit function — the comment
   block is the source of truth.

8. **HalError typed result type** — `hal/result.{h,cpp}` declares
   `enum class HalError` (storage / wireless / hub failure
   categories) plus a `HalResult<T>` value-or-error wrapper. The
   first migrated call site is `hal::hub_upload_note` — its old
   bool-with-string-out-param signature is now
   `HalError hub_upload_note(...)`, and its three callers
   (`ui_notes_sync.cpp`, `hal/storage.cpp` x2 → now in
   `storage_bulk.cpp`) render errors via `hal_error_string(err)`.
   New code should use the typed API; existing bool-returning
   functions can migrate incrementally — adding a typed variant
   alongside is the path of least breakage.

---

## Pre-session checklist for the next Claude Code run

1. `git status --short` to see the current uncommitted state.
2. Run `pio run -e tlora_pager -e emulator_lora_pager` to confirm a
   green baseline before making changes.
3. Read `REFACTOR_NOTES.md` (this file) + the auto-memory at
   `~/.claude/projects/-home-dns-tmp-t-lora-pager/memory/project_architecture_direction.md`.
4. For any split work: verify line-number boundaries with `grep -n`
   before `sed -i`, then rebuild after every extraction.
5. Do not revert user-applied changes flagged by system-reminder notes
   (e.g. the ones made to `factory.ino` and `ui_define.h` about
   `ui_device_lock_enforce` + WiFi auto-reconnect on hard reboot).
