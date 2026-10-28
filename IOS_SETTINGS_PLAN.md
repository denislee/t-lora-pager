# iOS companion — configure every pager setting from the phone

**Date:** 2026-10-28
**Status:** W1–W12 and W14 implemented and verified on Linux (2026-10-28, uncommitted); W13 (hardware pass) open; the app's SwiftUI layer (W10/W11) not yet built with Xcode
**Scope:** `protocol/` (new `settings.*` family, vectors), firmware (`src/hal/settings_registry.*`,
`src/hal/link/handlers_settings.cpp`, Settings app write-through), `ios/` (PagerKit
`SettingsService` + app Settings tab), `tools/pagerlink` (smoke)
**Related:** `IOS_COMPANION_PLAN.md` §4.5 / §6.1 / §8, `IOS_NOTES_VIEWER_PLAN.md` (process and
testing pattern this plan copies), `protocol/PROTOCOL.md` §3, §6

## 1. Goal

Every setting you can change on the pager's own **Settings** app (plus the few that live in
other apps) can be read and changed from the **Pager Companion** iOS app. Changes go both ways:
a change on the phone shows up on the pager right away, and a change on the pager shows up in the
app while it is connected.

**Done when:**

- Every row in the catalog (§3) marked **v1** can be read and written from the app's Settings tab.
  The pager applies the change exactly as it would if you made it on the pager (same HAL call,
  same side effects, same persistence), and the value survives a pager reboot.
- Changing a value on the pager while the app is connected updates the app within 1 s
  (`evt.settings.changed`).
- Changing a value from the phone while the pager's Settings app is open on the same page is
  **not** lost when the user leaves Settings (the clobber in §2.2 is fixed).
- Only settings that exist on the connected board are shown, with that board's ranges (for
  example brightness 0..16 on the pager and 0..255 on the watches).
- Secrets (tokens, passwords) are **write-only**: the app can set or clear them and see whether
  one is set, but the pager never sends a secret value over the link.
- An app talking to a firmware that lacks the `"settings"` capability hides the tab. A firmware
  talking to an older app is unaffected.

**Out of scope (v1):** LoRa radio parameters, BLE device name and home tile order. None of these
can be configured on the pager today (§2.4); adding them is a firmware feature first. Also out:
the IMU Debug page and the Devices list (diagnostics, not settings), and OTA.

## 2. What exists today

### 2.1 Link and app

- PagerLink v1 has `sys.*`, `fs.*`, `weather.*`, plus the pager-to-phone `chat.*`, `notes.sync`
  and `tg.*` calls (`PROTOCOL.md` §6). **There is no settings method.** The only setting the
  phone can change is the timezone, as a side effect of `sys.time.set` (`handlers_sys.cpp:174`).
- Handlers are registered per connection in `service_on_connect` (`link_service.cpp:363-368`) via
  `Rpc::handle(method, fn)`. `fn` has the signature
  `bool(const JsonObject&, JsonWriter&, RpcError&)` (`link_rpc.h:76`). `rpc.defer(fn)` runs work
  after the reply has been sent (`link_rpc.cpp:232-241`).
- `caps` in `sys.hello` is a hard-coded list `sys, fs, fs.delete.if, weather`
  (`handlers_sys.cpp:68-74`).
- The iOS app has Dashboard / Notes / Tasks / Telegram / Diagnostics tabs (`RootView.swift`).
  `PagerSession.bringUp` reads the hello caps and starts per-feature services
  (`PagerSession.swift:188-238`). That is where a `SettingsModel` hooks in.

### 2.2 How the pager stores settings (and the traps)

- Most settings are fields of one struct, `user_setting_params_t` (`src/hal/types.h:123-160`),
  saved as a single blob in NVS `pager/pager` with a versioned header
  (`SETTINGS_VERSION = 14`, `system.cpp:173-215`).
- The rest live in their own NVS namespaces: `ui/{style,dark}`, `timezone/tz`,
  `homeapps/v_<App>`, `hub/{enabled,url}`, `weather/user_city` (+ geo cache), `tgbridge/*`,
  `notesync/*`, `ssh/*`, `wifi_cred/*`.
- **T1: whole-blob clobber.** The Settings app copies the blob into `local_param` when it opens
  (`ui_settings.cpp:573`) and writes the whole copy back when it closes (`ui_settings.cpp:665`).
  Any value written by RPC while Settings is open is lost on exit.
- **T2: some setters don't persist.** `hw_set_{wifi,bt,radio,nfc,speaker,haptic}_enable` change
  `user_setting` but don't save NVS (`wireless.cpp:129`, `:782`, `radio.cpp:19`,
  `system.cpp:538`). Today they are only saved on Settings exit, so the same is true for
  brightness, keyboard backlight, timeout, sleep mode, charger current and `prune_internal`.
- **T3: Bluetooth Off kills the link.** `hw_set_bt_enable(false)` → `hal::ble::disable()`
  (`wireless.cpp:812-824`) drops every BLE peer, including the phone that asked.
- **T4: threading.** RPC handlers run on the link task under `LinkLock`
  (`link_service.cpp:332`). Theme, font, style and anything else that touches LVGL must be
  marshalled with `core::ui_post()` (`core/ui_post.h:27`).
- **T5: secrets need notes encryption.** The secret slots `tgbridge/token_enc`,
  `notesync/token_enc` and `ssh/pw_enc` (`secrets.cpp:63-67`) are AES-wrapped with the notes
  passphrase. Writing one is refused unless encryption is on **and unlocked**
  (`secrets.cpp:219-229`). Writing an empty value always clears the slot. Wi-Fi passwords
  (`wifi_cred/p<N>`) are stored in **plaintext**.
- **T6: timezone validation is loose.** `sys.time.set` accepts any `[A-Za-z0-9_/+-]{1,64}`, but
  only zones in `TZ_TABLE` (~80, `ui_time_sync.cpp:41-128`) are applied. An unknown name is
  stored and silently ignored.
- Some fields are only polled once a second (`factory.ino:264-272`: CPU clock, timeout,
  brightness, keyboard backlight). That is fine for the phone path.

### 2.3 Board differences that the app must respect

| Item | pager | T-Watch S3 | T-Watch Ultra |
|---|---|---|---|
| Brightness range | 0..16, default 8 | 0..255 | 0..255 |
| Keyboard backlight | yes | — | — |
| OTG output | yes | — | — |
| Charger current | 32 levels × 64 mA | 13-entry table, 100..1000 mA | same as S3 |
| NFC test action | yes | — | yes |
| SSH (real backend) | yes | — | — |
| Auto fake-sleep on timeout | yes | dims only | dims only |

### 2.4 Not configurable on the pager today

LoRa parameters (only `radio_enable`; `radio_chip::default_params` is fixed), GPS (owned by time
sync), the BLE name (compile-time `DEVICE_NAME`), home tile order (static `kItems`), the notes
auto-lock delay (constant, `system.cpp:67`). Listed so nobody looks for them; see §8.

## 3. Settings catalog

The ids below are the wire ids. Types: `bool`, `int{min,max,step}`, `enum[...]`, `string{max}`,
`secret` (write-only, read as `{"set":bool}`), `list`. **Apply** says when it takes effect:
`live` (immediately), `tick` (≤ 1 s, next loop poll), `theme` (theme rebuilt via `ui_post`;
screens already built keep the old font until reopened), `reboot` (the app says so).

### 3.1 Display & backlight — `settings_display.cpp`

| id | type | store | apply | boards | v1 |
|---|---|---|---|---|---|
| `display.brightness` | int 0..board max | blob `brightness_level` | live (`hw_set_disp_backlight`) | all | ✅ |
| `display.kb_backlight` | int 0..100 %, step 5 (→ 0..255) | blob `keyboard_bl_level` | live (`hw_set_kb_backlight`) | pager | ✅ |
| `display.timeout_s` | int 0..180, 0 = never | blob `disp_timeout_second` | tick | all | ✅ |
| `display.sleep_mode` | enum `light`,`deep` | blob `sleep_mode` | next sleep | all | ✅ |
| `display.ui_style` | enum `classic`,`pixel`,`hotel`,`beos` | NVS `ui/style` | theme (`ui_style_set` + `applyChromeStyle`) | all | ✅ |
| `display.dark` | bool | NVS `ui/dark` | theme (no effect on classic/pixel; the app greys it out) | all | ✅ |
| `statusbar.show_mem` | bool | blob `show_mem_usage` | live (1 Hz) | all | ✅ |
| `statusbar.show_files` | bool | blob `show_file_count` | live (1 Hz) | all | ✅ |

### 3.2 Charger & performance — `settings_display.cpp`

| id | type | store | apply | boards | v1 |
|---|---|---|---|---|---|
| `power.charging` | bool | blob `charger_enable` | live (`hw_set_charger` + `hw_apply_charge_limit`) | all | ✅ |
| `power.charge_current_ma` | enum of the board's levels | blob `charger_current` | live (`hw_set_charger_current_level`) | all | ✅ |
| `power.charge_limit_80` | bool | blob `charge_limit_en` | live | all | ✅ |
| `power.otg` | bool, **not persisted** (live hardware state) | — | live (`hw_set_otg`, may fail) | pager | ✅ |
| `perf.cpu_mhz` | enum 240, 160, 80 | blob `cpu_freq_mhz` | tick | all | ✅ |

### 3.3 Fonts — `settings_fonts.cpp`

Eighteen settings: `fonts.<ctx>.face` (enum of 7: `montserrat`, `unscii8`, `unscii16`,
`courier`, `inter`, `atkinson`, `jetbrains`) and `fonts.<ctx>.size` (int 10..32, step 2), for
`ctx` ∈ `system, home, notes, journal, weather, telegram, ssh, chat, header`. Store: blob
`<ctx>_font_{index,size}` (`notes` = `editor_*`). Apply: theme. All boards. ✅ v1.

The app's face picker notes that only Inter and Atkinson render accented Latin-1 (Montserrat is
ASCII only).

### 3.4 Date & time — `settings_datetime.cpp`, `ui_time_sync.cpp`

| id | type | store | apply | v1 |
|---|---|---|---|---|
| `time.tz` | enum of `TZ_TABLE` names, plus `""` = default (America/Sao_Paulo) | NVS `timezone/tz` | live (`timezone_set_user_tz`) | ✅ |
| clock | already `sys.time.set`; the app keeps "Sync time from phone" | RTC | live | ✅ (exists) |
| action `time.sync_ntp` | — | — | needs Wi-Fi; result arrives as an event | ✅ |
| action `time.sync_gps` | — | — | `unsupported` on builds without GPS | ✅ |

`sys.time.set` gets the same `TZ_TABLE` check (fixes T6): an unknown zone is rejected with
`invalid_argument` and is no longer stored.

### 3.5 Connectivity — `settings_connectivity.cpp`, `ui_wifi.cpp`

| id | type | store | apply | v1 |
|---|---|---|---|---|
| `conn.wifi` | bool | blob `wifi_enable` | live | ✅ |
| `conn.bt` | bool, **dangerous** (T3) | blob `bt_enable` | live, after the reply (`rpc.defer`) | ✅ with guard (§4.5) |
| `conn.radio` | bool | blob `radio_enable` | live; a config error comes back as `internal` with the radio's error code | ✅ |
| `conn.nfc` | bool | blob `nfc_enable` | live | ✅ |
| `conn.speaker` | bool | blob `speaker_enable` | live | ✅ |
| `conn.haptic` | bool | blob `haptic_enable` | live | ✅ |
| `wifi.saved` | list of SSIDs (most recent first, max 16); **no passwords** | NVS `wifi_cred` | — | ✅ |
| action `wifi.scan` | → `[{ssid, rssi, secure}]` | — | needs `conn.wifi` on | ✅ |
| action `wifi.add` | `{ssid, password}` → connect, save on success | `wifi_cred` | live | ✅ |
| action `wifi.forget` | `{ssid}` (new HAL: forget one network) or `{all:true}` | `wifi_cred` | live | ✅ |
| action `wifi.test` | → `{ok, ms}` (`hw_ping_internet`) | — | — | ✅ |

Wi-Fi passwords cross the link only phone → pager inside the encrypted, bonded BLE link, the
same way they are typed on the pager today. They are never sent back.

### 3.6 Home apps — `settings_home_apps.cpp`, `menu_app.cpp`

`home.visible.<App>` bool for `Editor` (shown as Notes), `Tasks`, `Recorder`, `Telegram`, `Chat`,
`Weather`, `SSH`. Store: NVS `homeapps/v_<App>`, default true. Apply: live (the home grid
rebuilds via `ui_post`). ✅ v1. The list comes from `kItems`, so a new tile appears in the app
without an app update.

### 3.7 Local hub — `settings_hub.cpp`, `hal/hub.cpp`

| id | type | store | v1 |
|---|---|---|---|
| `hub.enabled` | bool | NVS `hub/enabled` | ✅ |
| `hub.url` | string ≤ 128, `http(s)://…`; `""` clears | NVS `hub/url` (via `hal::hub_set_url`, which trims and invalidates the reachability cache) | ✅ |

### 3.8 Weather — `settings_weather.cpp`, `ui_weather.cpp`

| id | type | store | v1 |
|---|---|---|---|
| `weather.location` | `{"auto":true}` or `{"name","lat","lon"}` | NVS `weather/user_city` + geo cache, via `weather_set_user_location` (clears the cached forecast) | ✅ |

The phone geocodes the city itself (Open-Meteo geocoding; it already talks to Open-Meteo for
`weather.get`), so the pager does no search over the link. After a change the app pushes a fresh
forecast for the new key (`weather.push`).

### 3.9 Telegram — `settings_telegram.cpp`, `ui_telegram.cpp`

| id | type | store | v1 |
|---|---|---|---|
| `tg.url` | string ≤ 128 | NVS `tgbridge/url` | ✅ |
| `tg.token` | secret | `tgbridge/token_enc` (T5) | ✅ |
| `tg.notify_vibrate` | bool | `tgbridge/notif_vib` | ✅ |
| `tg.notify_banner` | bool | `tgbridge/notif_toast` | ✅ |
| `tg.favorites` | list `[{id, title}]` | `tgbridge/favs` + `fav_titles` | ✅ |

The favorites editor lists chats from the phone's own Telegram login when it has one (TDLib), or
from the bridge otherwise. When the phone is logged in, the pager's `tg.*` calls already go
through the phone, so `tg.url` / `tg.token` only matter for the direct Wi-Fi path; the app says so.

### 3.10 Notes sync — `settings_notes_sync.cpp`, `ui_notes_sync.cpp`

| id | type | store | v1 |
|---|---|---|---|
| `nsync.repo` | string `owner/name` | NVS `notesync/repo` | ✅ |
| `nsync.branch` | string, `""` = main | NVS `notesync/branch` | ✅ |
| `nsync.token` | secret | `notesync/token_enc` (T5) | ✅ |

The app offers **"Copy from this phone's GitHub settings"**, which writes repo, branch and PAT in
one `settings.set`. The phone still keeps its own PAT in its Keychain (`GitHubAccountStore`); the
pager copy is only for syncing without the phone.

### 3.11 Notes security — `settings_storage.cpp`, `hal/notes_crypto.cpp`

| id | type | v1 |
|---|---|---|
| `notes_sec.state` | read-only enum `off`, `locked`, `unlocked` | ✅ |
| action `notes_sec.lock` | lock now | ✅ |
| set / change / disable passphrase, encrypt SD | actions with passphrases | ❌ pager only (D3) |

### 3.12 Storage — `settings_storage.cpp`

| id | type | store | apply | v1 |
|---|---|---|---|---|
| `storage.msc_target` | enum `sd`, `int` | blob `msc_prefer_sd` + lib NVS `lilygo/msc_sd` (`hw_set_msc_prefer_sd`) | reboot | ✅ |
| `storage.prune_internal` | bool (keep the 50 newest on internal) | blob `prune_internal` | live | ✅ |
| action `storage.copy_int_to_sd` | — | — | worker (today it runs on the LVGL thread; move it) | ✅ |
| action `storage.prune_now` | — | — | worker | ✅ |

"Copy notes → phone" is not needed in the app: the Notes tab already does it.

### 3.13 SSH — `ui_ssh.cpp` (pager only, `USING_LIBSSH`)

| id | type | store | v1 |
|---|---|---|---|
| `ssh.host` | string ≤ 128 | NVS `ssh/host` | ✅ |
| `ssh.user` | string ≤ 64 | NVS `ssh/user` | ✅ |
| `ssh.port` | int 1..65535 | NVS `ssh/port` | ✅ |
| `ssh.password` | secret | `ssh/pw_enc` (T5) | ✅ |

### 3.14 System

| id | kind | v1 |
|---|---|---|
| action `sys.power_off` | the pager shows a confirm popup; the phone cannot power it off silently | ✅ |
| System Info extras (MAC, IP, chip id, fw hash, LVGL/core versions, build date, RAM/PSRAM) | extend `sys.info` with an `about` object; shown in the app's About section | ✅ |
| Devices list, IMU debug | diagnostics | ❌ |

## 4. Design

### 4.1 One registry on the pager, used by both UIs

New `src/hal/settings_registry.{h,cpp}`: a static table with one entry per setting id:
`{id, type, range/enum, board gate, store, getter, setter, apply kind, secret?}`. Getters and
setters call the HAL functions that exist today. The registry:

- validates the value (type, range, enum, string length and charset),
- writes it **field by field**: for blob fields, it updates `user_setting` under the instance lock
  and calls `save_user_setting_nvs()`; for the other namespaces it calls the existing setter
  (`hal::hub_set_url`, `timezone_set_user_tz`, `secret_store`, …),
- applies it (direct HAL call, or `core::ui_post` for `theme`/`live` work that touches LVGL, T4),
- bumps a RAM revision counter `settings_rev` and notifies observers with the changed ids.

The pager's Settings app moves onto the same registry (`W3`): each control writes through
`settings_set(id, value)` on change, and the whole-blob write-back on exit is removed. That fixes
T1 and T2 for both paths, by construction, and also makes every setting crash-safe (the rule in
`CLAUDE.md` "call `hw_set_user_setting` eagerly" becomes true for all of them). The Settings app
subscribes to the registry observer: when an id on the open subpage changes from the link, it
updates that widget via `ui_post`.

The registry is plain C++ with the HAL calls behind function pointers, so validation and the
schema encoder run in `native_test`.

### 4.2 Wire protocol — `settings.*` (cap `"settings"`)

All P→D unless noted. Added to `PROTOCOL.md` as §6.4 with golden vectors.

| Method / event | Params → result |
|---|---|
| `settings.schema` | `{"group"?,"cursor"?}` → `{"rev_schema":"<hash>","groups":[{"id","title"}],"items":[{"id","group","title","type","min"?,"max"?,"step"?,"enum"?:[{"v","label"}],"max_len"?,"apply","secret"?,"danger"?}],"cursor"?}`. Only the settings that exist on this board, with this board's ranges. Paged by `cursor` so each RES stays under 4096 B. `rev_schema` is a hash of the whole schema: the phone caches it and refetches only when the hash in `settings.get` changes |
| `settings.get` | `{"ids"?:[…]}` → `{"rev","rev_schema","values":{"<id>":<value>}}`. All ids when `ids` is absent. A secret reads as `{"set":true\|false}`, never its value. Fits one RES (~70 values, about 2.5 KB) |
| `settings.set` | `{"values":{"<id>":<value>},"if_rev"?}` → `{"rev","applied":[ids],"reboot":bool}`. **All or nothing**: every value is validated first; one bad value fails the whole call with `invalid_argument` and a `msg` naming the id (`"display.brightness: out of range"`); nothing is written. With `if_rev`, a stale revision fails with `invalid_argument` `"changed"` (same word as `fs.delete.if`) so the app reloads instead of overwriting a pager-side edit |
| `settings.action` | `{"id","p"?}` → per-action result. Long actions (`wifi.scan`, `time.sync_ntp`, `storage.copy_int_to_sd`) answer `{"job"}` at once and report `evt.settings.job {"job","done","ok","msg","result"?}` |
| `evt.settings.changed` | D→P `{"rev","ids":[…]}` after any change, from the pager UI or the link. The app refetches those ids with `settings.get` |

Error mapping: a secret write while notes are locked or encryption is off → `unauthorized`,
`msg` `"notes locked"` / `"encryption off"` (the app explains how to unlock on the pager).
Unknown id → `invalid_argument`. Board-gated id → `unsupported`.

Why schema-driven rather than hard-coding ids in Swift: board gating and per-board ranges stay on
the pager; a new pager setting appears in the app without an app release (generic rendering); the
app only needs custom screens for the few complex ones (§4.4).

### 4.3 Concurrency and revision

- `settings_rev` is a u32 in RAM that starts at a boot-random value, so a revision from before a
  reboot never matches.
- The app sends `if_rev` on every set. On `"changed"` it refetches, shows the new value, and
  re-applies the user's edit only if that same id was not the one that changed.
- Both UIs write through the registry under the instance lock, so the last writer wins per id and
  nothing is lost silently.

### 4.4 iOS

- **PagerKit** `Services/SettingsService.swift`: `schema()` (follows `cursor`, caches by
  `rev_schema`), `get(ids:)`, `set(_:ifRev:)`, `action(_:params:)`, plus a typed `SettingValue`
  enum (`bool/int/string/enum/secret(set:)/list`) and `SettingsSchema` model. Pure,
  Linux-testable, no UIKit.
- **App** `Settings/SettingsModel.swift` (`@MainActor ObservableObject`): schema + values,
  pending edits, `evt.settings.changed` handling, offline cache (the last values as read-only when
  disconnected). Started in `PagerSession.bringUp` when caps contain `"settings"`.
- **App** `Features/Settings/`: a generic `Form` renderer driven by the schema (`Toggle`,
  `Slider`/`Stepper`, `Picker`, `TextField`, `SecureField` with "Set"/"Clear" for secrets,
  "reboot needed" and "dangerous" badges), one section per group, in the pager's tile order.
  Custom screens only where a generic row isn't enough:
  - **Wi-Fi networks**: saved list, scan, add with password, forget.
  - **Timezone**: searchable list; defaults to the phone's current zone.
  - **Weather location**: city search on the phone, plus "Auto".
  - **Telegram favorites**: chat list with toggles.
  - **Fonts**: face + size per context, with a preview using the bundled font where we have it.
- Edits are sent on commit (toggle flip, slider release, text submit), not on every slider tick,
  to keep the link quiet.
- New tab **Settings** in `RootView`. The existing app-side settings (GitHub account, Telegram
  login) stay where they are and are linked from a "This phone" section.

### 4.5 Safety rules

- **Bluetooth off over BLE** (T3): the app shows "This will disconnect the app. Turn Bluetooth
  back on from the pager." and sends `conn.bt=false` only after the user confirms. The pager
  answers first and disables BLE in `rpc.defer`. Over TCP (emulator) there is no problem.
- **Wi-Fi off** while the pager is mid-sync with the hub: allowed, same as on the pager.
- **Power off** shows a confirm popup on the pager (`ui_popup_create`). The phone cannot power the
  pager off unattended.
- **Trusted phone**: `settings.set`, `settings.action` and secret writes go through the same
  trusted-phone gate as `fs.*` (`IOS_COMPANION_PLAN.md` §8). `settings.schema` / `settings.get`
  are allowed on any bonded link.
- **Logging**: neither side logs secret values. The pager logs `settings.set ids=[…]` only; the
  app's session log shows ids, never values of secret ids.
- **Reboot settings** (`storage.msc_target`): applied to NVS and reported with `reboot:true`; the
  app shows a banner. No automatic reboot.

## 5. Work items

### A. Firmware foundation

| ID | Work | Acceptance |
|---|---|---|
| W1 | **Registry core** `hal/settings_registry.{h,cpp}`: table, validate, typed get/set, `settings_rev`, observers, board gating, schema + values JSON encoders. Blob fields and the `ui`, `timezone`, `homeapps`, `hub` namespaces first. | `native_test` suite `test_settings_registry`: every id validates min/max/step/enum/len; a bad value leaves the store unchanged; a set bumps `rev` once; board gating hides `display.kb_backlight` and `power.otg` on watch builds; the schema page encoder keeps every page < 4096 B. |
| W2 | **Remaining stores**: `weather`, `tgbridge` (incl. favorites), `notesync`, `ssh`, secrets (T5 errors), `wifi_cred` (SSID list, add, new per-SSID forget), `storage`, `notes_sec.state`. Fix T6 in `sys.time.set`. | Unit tests for each store with a fake NVS; secret read returns only `{set}`; an unknown tz is rejected by both `settings.set` and `sys.time.set`. |
| W3 | **Settings app write-through**: every control calls `settings_set`; remove the exit write-back (`ui_settings.cpp:665`); observer refreshes open widgets via `ui_post`; `theme_init` still runs on exit for fonts and style. Keep the `State reset on onStop` comment blocks current. | Every hardware env and the matching emulator build; UI shots sweep (`UI_SHOTS_DIR=…`) is pixel-identical to before; on the emulator, a value changed in Settings survives a kill -9 without leaving Settings. |
| W4 | **Actions**: `wifi.scan/add/forget/test`, `time.sync_ntp/gps`, `notes_sec.lock`, `storage.copy_int_to_sd` (moved off the LVGL thread), `storage.prune_now`, `sys.power_off` (pager-side confirm); job ids and `evt.settings.job`. | Emulator smoke for each action that the emulator can run; the others are marked hardware-only in W12. |

### B. Protocol

| ID | Work | Acceptance |
|---|---|---|
| W5 | **Spec + vectors**: `PROTOCOL.md` §6.4, `protocol/tools/mkvectors.py` gains `settings.json` (schema page, get with a secret, set ok, set with one bad id, stale `if_rev`, event). `"settings"` added to hello caps. | `mkvectors.py --check` and `make protocol-test` green; Unity, Go and PagerKit read the same vectors. |
| W6 | **Handlers** `src/hal/link/handlers_settings.cpp` registered in `service_on_connect`; `evt.settings.changed` emitted from the registry observer (coalesced to one event per 250 ms); `conn.bt=false` deferred; trusted-phone gate. | `pagerlink` smoke: `settings schema` pages to the end; `settings set display.brightness=…` applies and `settings get` reads it back; a set with one bad id changes nothing; a stale `if_rev` answers `"changed"`; changing a value in the emulator UI produces `evt.settings.changed`. Negative control: with the all-or-nothing check disabled, the bad-id test fails. |
| W7 | **`pagerlink` CLI**: `settings schema|get|set|action` subcommands plus smoke steps. | `(cd tools/pagerlink && go run ./cmd/pagerlink smoke)` passes against the emulator. |

### C. iOS

| ID | Work | Acceptance |
|---|---|---|
| W8 | **PagerKit `SettingsService` + models** (§4.4). | XCTest against the vectors and an in-process pager `LinkSession` (as `NotesSyncServiceTests` does): schema paging, cache hit by `rev_schema`, secret decoding, `"changed"` → typed error, event decoding. `make test-linux` green. |
| W9 | **`SettingsModel` + session wiring**: start on `"settings"` cap, refetch on events, offline cache, edit-commit-on-release, conflict handling (§4.3). | Unit tests with a stub service: an event for an id the user is editing doesn't drop the edit; a stale-rev set refetches and retries once. |
| W10 | **Generic Settings UI** + tab in `RootView`; `make gen`. | Simulator ↔ emulator over TCP 7600: every v1 id in §3 is shown, and changing each one changes the emulator (spot-check with UI shots). |
| W11 | **Custom screens**: Wi-Fi, timezone, weather location, Telegram favorites, fonts, secrets ("Set" / "Clear" / "Copy from this phone's GitHub settings"), BT-off and power-off confirms. | Simulator ↔ emulator: add a Wi-Fi network, pick a timezone, set a city (the Weather app on the emulator shows it), toggle a favorite, set and clear `nsync.token` (with notes unlocked; locked → the "unlock on the pager" message). |
| W12 | **End-to-end script** `make -C ios settings-e2e` (like `notes-e2e`): drives PagerKit against the emulator through every v1 id: set, read back, restart the emulator, read back again. | Green on Linux (Docker swift) and macOS. |

### D. Close-out

| ID | Work | Acceptance |
|---|---|---|
| W13 | **Hardware pass** (pager + iPhone over BLE, then T-Watch S3 if available). | Each v1 id changed from the phone takes effect and survives a reboot; the watch shows only its own settings and ranges; BT-off disconnects cleanly and BT back on from the pager reconnects; no secret in the pager serial log or the app session log; idle current with the Settings tab open is unchanged (no polling). |
| W14 | **Docs**: `ios/README.md` (Settings tab, e2e), `IOS_COMPANION_PLAN.md` §4.5/§6.1 updated, `CLAUDE.md` (registry rule: new settings go through `settings_registry`, never straight to NVS). | Docs land with the last code commit. |

**Order:** W1 → W3 (fixes T1/T2 on the pager alone, useful on its own) → W5 → W6/W7 → W2/W4 →
W8 → W9 → W10 → W11 → W12 → W13 → W14. After every firmware item, rebuild all three hardware envs
and the matching emulator (`REFACTOR_NOTES.md` rule).

## 6. Tests and checks

| Layer | How |
|---|---|
| Validation, schema, paging | Unity `test_settings_registry` (`pio test -e native_test`) |
| Wire format | `protocol/vectors/settings.json`, consumed by Unity, Go and PagerKit (`make protocol-test`) |
| Handlers | `pagerlink smoke` against the emulator, including negative controls (bad id, stale rev, secret while locked, gated id) |
| Pager UI regression | `UI_SHOTS_DIR` sweep before/after W3, every style × dark mode |
| App logic | PagerKit XCTest + `SettingsModel` tests |
| End to end | `make -C ios settings-e2e`, then Simulator ↔ emulator |
| Hardware | W13 |

## 7. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | W3 touches every Settings subpage; a missed control regresses persistence | One subpage per commit; UI shots before/after; a test that walks the registry table and checks every id has a Settings control or is marked phone-only |
| R2 | Applying from the link task races LVGL | Every LVGL-touching apply goes through `ui_post` (T4); a debug assert in the registry when an LVGL apply runs off the UI thread |
| R3 | Schema too big for one RES | Paging with `cursor` (W1 test caps every page < 4096 B) |
| R4 | Secret writes fail confusingly when notes are locked | Clear `unauthorized` messages; the app shows lock state from `notes_sec.state` before the user types a token |
| R5 | Plaintext Wi-Fi passwords in NVS | Unchanged from today; noted for a follow-up (store them in a secret slot) |
| R6 | Font/theme changes don't reach screens already built | Same as on the pager today; the app says "applies to screens opened after the change" |
| R7 | NVS wear from slider spam | Commit-on-release in the app; the registry skips the write when the value is unchanged |

## 8. Open decisions

| ID | Decision | Recommendation |
|---|---|---|
| D1 | Schema-driven generic UI vs hard-coded Swift settings | **Schema-driven** (board gating + per-board ranges stay on the pager; new settings need no app release) |
| D2 | Allow `conn.bt=false` from the phone at all | Allow, with the confirm in §4.5 |
| D3 | Passphrase operations (set / change / disable notes encryption) from the phone | **Pager only in v1.** They re-encrypt every note and can erase secrets; the phone gets state + "Lock now" |
| D4 | Send pager-side secrets (Telegram token, GitHub PAT, SSH password) from the phone | Yes, write-only; they are needed only for the pager's direct Wi-Fi paths |
| D5 | Add the missing settings in §2.4 (LoRa params, BLE name, home order, auto-lock delay) | Separate plan, firmware first; the registry makes them one table row each on the link side |
| D6 | Move Wi-Fi passwords into encrypted secret slots | Follow-up; it would make Wi-Fi depend on unlocking the notes, which needs a design of its own |

## 9. Progress

| ID | State | Evidence |
|---|---|---|
| W1 | done | `src/hal/settings_registry.{h,cpp}` (pure registry: validate, all-or-nothing batch, `rev`, observers, paged schema/get/set JSON) + `src/hal/settings_catalog.{h,cpp}` (pure catalog, board profile) + `src/settings_table.cpp` (HAL bindings). `pio test -e native_test -f test_settings_registry`: 15/15, incl. every id's range/enum/length checks, watch gating of `display.kb_backlight`/`power.otg`/`ssh.*`, every schema page < 4096 B. Negative control: validation bypassed → `test_all_or_nothing` + `test_gating_watch` fail (restored, 15/15) |
| W2 | done | All §3 stores bound in `settings_table.cpp` (weather, tgbridge incl. favorites in its NVS format, notesync, ssh, secrets with `unauthorized` "encryption off"/"notes locked", `wifi_cred` list + new `hw_wifi_forget_ssid`, storage, `notes_sec.state`). T6: `sys.time.set` refuses a zone not in `TZ_TABLE` (`timezone_is_known`); smoke step "sys.time.set tz Mars/Olympus_Mons -> invalid_argument". Emulator stores (`hal::nvs_*`, blob, tz, hub, weather city, home tiles) now persist under `$PAGERLINK_NVS_DIR` |
| W3 | done | Every Settings control commits through the registry (`settings_commit*`, store-only before its own HAL apply; sliders debounced 400 ms); whole-blob write-back on exit removed; observer rebuilds the open stateless subpage on a phone change. UI shots before (HEAD worktree) vs after: 256/256 identical except the status-bar clock region (x 431–460, y 7–20) in 26 shots where the minute rolled over. All 3 hardware envs + emulator build. Kill -9 persistence verified through the registry path by `settings-e2e` (not by injecting key events into the Settings UI) |
| W4 | done | Actions in `settings_table.cpp`: `wifi.scan/add/forget/test`, `time.sync_ntp/gps`, `notes_sec.lock`, `storage.copy_int_to_sd`, `storage.prune_now` (workers + `evt.settings.job`), `sys.power_off` (pager-side `ui_confirm`). Emulator smoke: `wifi.test` job round trip. Others hardware-only (W13) |
| W5 | done | `PROTOCOL.md` §6.4 (+ §6.2 hook, §7 vectors); `mkvectors.py` emits `settings.json` (17 cases, 3 events); `"settings"` cap in `sys.hello`. Unity replays every case byte for byte (`test_golden_vectors`); Go and PagerKit decode the same file. `make protocol-test` green |
| W6 | done | `src/hal/link/handlers_settings.cpp`; coalesced `evt.settings.changed` (250 ms) from `service_tick`; hardware applies posted from `rpc.defer` (conn.bt off cannot cut the RES). Full `pagerlink smoke` green incl. 12 settings steps. Negative control: all-or-nothing disabled → "settings.set all-or-nothing" step FAILs (restored, green) |
| W7 | done | `pagerlink settings schema\|get\|set\|action` + `smokeSettings`; `go vet` / `go test ./...` green |
| W8 | done | PagerKit `SettingsService` (schema paging + `rev_schema` cache, typed `SettingValue`, `changed` → `SettingsError.changed`, job waits). `SettingsServiceTests` 9/9 against an in-process pager session serving the vectors |
| W9 | done | PagerKit `SettingsStore` (UI-free state, conflict rules §4.3) + app `SettingsModel` wired in `PagerSession` (cap-gated attach, events, detach keeps values read-only). `SettingsStoreTests` 9/9 (edited id keeps its draft on a pager change; stale rev retries once, refused when the same id changed). Full PagerKit suite 260/260 |
| W10 | written, not built | `App/Features/Settings/SettingsView.swift` (generic schema-driven form) + Settings tab in `RootView`. No Xcode on this host: needs `make gen && make sim` and the Simulator ↔ emulator pass |
| W11 | written, not built | `App/Features/Settings/SettingsScreens.swift` (Wi-Fi, time zone, weather location with phone geocoding — `WeatherService.searchCities`, tested — Telegram favorites), secrets Set/Clear, "Copy from this phone's GitHub settings", BT-off and power-off confirms. Fonts use the generic pickers; the font preview is not done |
| W12 | done | `make -C ios settings-e2e` (`ios/SettingsE2E`): 58 writable settings set + read back, emulator `kill -9`, restart, all read back; secret refused `unauthorized`; unknown id refused. Green on Linux (Docker); macOS path not run here. Negative control: emulator blob save disabled → 36 "after restart" FAILs (restored, green) |
| W13 | open | Needs the pager + an iPhone over BLE (and a watch) |
| W14 | done | `ios/README.md` (Settings tab, settings-e2e), `IOS_COMPANION_PLAN.md` §4.5/§6.1, `CLAUDE.md` (registry rule, emulator NVS, test list, settings-e2e) |

Deviations from the design above, all deliberate:

- There is no separate trusted-phone gate in the firmware (`fs.*` has none either); `settings.*` relies on the bonded, encrypted BLE link like `fs.*` (§4.5).
- Schema gained `"type":"object"` (`weather.location`) and an `actions` list on the first page, so the app can render action buttons generically.
- Hardware applies from the phone run on the LVGL thread after the reply, so `conn.radio` does not return the radio's error code (the toggle is stored; a config error shows only in the pager's log). `power.otg` applies synchronously and does return `internal` when refused.
- `home.visible.*` takes effect the next time the home screen opens (its layout key), not instantly.
- While Settings is open, a phone change rebuilds only the stateless subpages (Display, Fonts, Charger, Home Apps, Storage, Performance); the others show the new value when reopened (the stored value is right either way).
- `storage.copy_int_to_sd` from the phone runs on a worker; the pager's own Storage button still runs it on the LVGL thread.
