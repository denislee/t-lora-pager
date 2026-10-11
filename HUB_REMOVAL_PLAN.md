# Lilyhub Removal — Plan
<!-- plan-doc: v2 -->
**Progress:** 34% — 9/26
**Status:** 🟡 H0, H1, H3, H5.1 landed; iOS Track C item 7 (file service) landed as cadd656. Next: H2.1 hw_link_call. H2.3 waits on Track C item 9; H4 on BLE hardware
**Last updated:** 2026-10-11 — /next: ran iOS-plan Track C item 7 (cadd656) to unblock H2.1/H2.2
**Beads:** git-ne9bg

## Handoff

Next action: W7 — H2.1 `hw_link_call(method, params, timeout_ms, cb)` + the DATA transfer-out helper in src/hal/link.h, on top of link_xfer (cadd656)
In play: /home/dns/tmp/t-lora-pager, branch master, 11 local commits not pushed (8fe85df…c0526f3); baseline shots in /tmp/shots-before
Not in this doc: H2.3 needs IOS_COMPANION_PLAN.md Track C item 9 (hal/weather_cache + handlers_weather); `make protocol-test` leaves the emulator binary missing, so rebuild before a smoke run

## Work
- [x] **W1** H0.1 Land the uncommitted hub work — ev: 8fe85df (server) · 7038cb7 (libhelix) · fd95e6e (firmware DC pass) · 0f018bb (docs); gate pager/emu/native/go all exit 0 · was: H0.1 · src: L270 · bd: git-ne9bg.1
- [x] **W2** H0.2 Record decisions D-H1…D-H8 — ev: all accepted as recommended by the user 2026-10-11 (see Current state) · was: H0.2 · src: L278 · bd: git-ne9bg.2
- [x] **W3** H0.3 Behaviour baseline — ev: go test -race -cover green (pagerlink 78.7 %); 256 shots in /tmp/shots-before, exit 0 · was: H0.3 · src: L279 · bd: git-ne9bg.3
- [x] **W4** H1.1 Telegram: drop the hub proxy branch — ev: 39c4e3f · was: H1.1 · src: L286 · bd: git-ne9bg.4
- [x] **W5** H1.2 Eviction: drop the hub mirror, SD copy is the only gate — ev: 39c4e3f; test_evict 3/3; negative control (remove on failure) fails `Expected 0 Was 1` · was: H1.2 · src: L290 · bd: git-ne9bg.5
- [x] **W6** H1.3 Weather direct-only — ev: 39c4e3f; shots match baseline below the status bar (only the clock spinbox differs) · was: H1.3 · src: L295 · bd: git-ne9bg.6
- [x] **W13** H3.1 Protocol: drop hub codes + sys.info hub, add LinkOffline — ev: 557a188; make protocol-test exit 0; pagerlink smoke all steps passed · was: H3.1 · src: L377 · bd: git-ne9bg.7
- [x] **W14** H3.2 Update IOS_COMPANION_PLAN.md — ev: d0e8e64 · was: H3.2 · src: L383 · bd: git-ne9bg.8
- [x] **W21** H5.1 Relocate Go PagerLink tooling to tools/pagerlink — ev: 7e68e64; go test -race -cover internal/pagerlink 78.7 % (= baseline); smoke passed · was: H5.1 · src: L422 · bd: git-ne9bg.9
- [ ] **W7** H2.1 Pager-initiated RPC API (`hw_link_call`) — left: Track C item 7 landed (cadd656: link_xfer engine, Rpc::defer, fs.* on emulator); build hw_link_call on top · was: H2.1 · src: L305 · bd: git-ne9bg.10
- [ ] **W8** H2.2 `pagerlink serve` mock phone — left: fs.read client side exists in tools/pagerlink (ReadFile); add the responder role · after: W7 · was: H2.2 · src: L311 · bd: git-ne9bg.11
- [!] **W9** H2.3 Weather via phone — blocked: iOS plan Track C item 9 (hal/weather_cache, handlers_weather) not landed · after: W8 · was: H2.3 · src: L315 · bd: git-ne9bg.12
- [!] **W10** H2.4 Chat via phone — blocked: needs W7/W8 (DATA upload + mock phone) · after: W8 · was: H2.4 · src: L320 · bd: git-ne9bg.13
- [!] **W11** H2.5 Notes sync via phone — blocked: needs W7 + W8 (fs.read is in, cadd656) · after: W8 · was: H2.5 · src: L334 · bd: git-ne9bg.14
- [!] **W12** H2.6 Telegram via phone — blocked: needs W7/W8 (and bulk DATA for >4096 B replies) · after: W8 · was: H2.6 · src: L353 · bd: git-ne9bg.15
- [!] **W15** H4.1 Hardware acceptance, pager + iPhone, Wi-Fi off — blocked: D-H1 gate, BLE transport (iOS plan Track A step 5) not on hardware · after: W9, W10, W11, W12 · was: H4.1 · src: L390 · bd: git-ne9bg.16
- [ ] **W16** H4.2 Delete the firmware hub — after: W15 · was: H4.2 · src: L398 · bd: git-ne9bg.17
- [ ] **W17** H4.3 Status bar: link indicator replaces hub_probe — after: W15 · was: H4.3 · src: L403 · bd: git-ne9bg.18
- [ ] **W18** H4.4 One-shot NVS cleanup (hub ns, weather/hub_url, PAT) — after: W15 · was: H4.4 · src: L408 · bd: git-ne9bg.19
- [ ] **W19** H4.5 Screenshot sweep vs /tmp/shots-before — after: W16, W17, W18 · was: H4.5 · src: L412 · bd: git-ne9bg.20
- [ ] **W20** H4.6 Build all six envs — after: W19 · was: H4.6 · src: L416 · bd: git-ne9bg.21
- [ ] **W23** H5.3 Decommission any installed hub (before W22) — after: W20 · was: H5.3 · src: L431 · bd: git-ne9bg.22
- [ ] **W22** H5.2 Delete server/ and the /server/bin/ ignore line — after: W23 · was: H5.2 · src: L429 · bd: git-ne9bg.23
- [ ] **W24** H6.1 CLAUDE.md lilyhub sweep — after: W22 · was: H6.1 · src: L440 · bd: git-ne9bg.24
- [ ] **W25** H6.2 REFACTOR_NOTES.md entry + README pointer — after: W22 · was: H6.2 · src: L445 · bd: git-ne9bg.25
- [ ] **W26** H6.3 Zero-reference grep (negative control already run 2026-10-11: live hits found) — after: W24, W25 · was: H6.3 · src: L447 · bd: git-ne9bg.26

## Blocked / needs a human

### W7 — H2.1 Pager-initiated RPC API
Blocker lives in another plan: IOS_COMPANION_PLAN.md › Track C item 7 (Phase 2 file service). Run it, then item 9, then resume here.
```
# OPEN
/next IOS_COMPANION_PLAN.md
```

### W15 — H4.1 Hardware acceptance
Gate D-H1: needs `link_transport_ble.cpp` (iOS plan Track A step 5) passing on a real pager + iPhone, then W9–W12. Not runnable from here.

## Current state
- Decisions D-H1…D-H8 (§3) accepted as recommended on 2026-10-11: gate H4 on BLE; PAT on the phone; Groq Whisper from the phone; tools/pagerlink module; one-shot NVS cleanup; direct-first for weather and Telegram; bridge creds sent per tg.* call.
- Firmware still carries the hub for chat and notes sync (hal/hub.*, settings_hub, ui_chat, ui_notes_sync, the system.cpp probe). HalError::HubDisabled/HubUnreachable remain but are off the wire.
- server/ still holds lilyhub; the PagerLink Go tooling lives in tools/pagerlink.

## Log

### 2026-10-11 — /next — outside item: IOS_COMPANION_PLAN.md Track C item 7
- User chose to unblock H2 by running the iOS plan's Track C item 7. Landed as cadd656 (+ c0526f3 log in the iOS plan): fs.* handlers, link_xfer credit-window engine + CRC-32, fs_volume (real dirs on the emulator), sandbox_path, Rpc::defer, evt.fs.changed, Go ReadFile/WriteFile + CLI.
- Evidence: pagerlink smoke 24/24 against the emulator (1 MiB round trip, dropped upload leaves no file or .part, `..` rejected, bad CRC rejected); native 113/113; Go 80.7 %; all six envs build; shots unchanged below the status bar. Negative controls: `..` allowed → test_sandbox_rejects FAILS; .part kept on drop → 2 smoke steps FAIL.
- Effect here: W7/W8 reopened (their DATA dependency exists now); W9 still waits on Track C item 9.

### 2026-10-11 — /next — W1–W6, W13, W14, W21
- Env: local only (/home/dns/tmp/t-lora-pager, master). No device, no remote; nothing pushed. Every item LOW (source edits + local commits), no rollback beyond `git revert <sha>`.
- W1 H0.1 ✅ gate on the dirty tree: `pio run -e tlora_pager` → 0, `-e emulator_lora_pager` → 0, `pio test -e native_test` → 92/92, `go test -race -cover ./...` (server) → ok. Committed by area: 8fe85df server, 7038cb7 libhelix-mp3, fd95e6e firmware DC pass, 0f018bb docs.
- W2 H0.2 ✅ user accepted D-H1…D-H8 as recommended.
- W3 H0.3 ✅ Go coverage: cache 65.9, chat 57.5, httpx 82.7, notessync 82.1, pagerlink 78.7, telegram 22.5, weather 56.1 %. `UI_SHOTS_DIR=/tmp/shots-before` → exit 0, 256 PPM in 8 style×mode dirs.
- W4–W6 H1 ✅ 39c4e3f. H1.2's eviction is `#ifdef ARDUINO`, so the emulator can't run it; the per-file move became `hal::evict_one` (src/hal/evict.h) with test/test_evict. Negative control: mutating it to remove on a failed write → `test_kept_when_copy_fails: Expected 0 Was 1` [FAILED]; restored → 3/3. Gate: pager 0, emu 0, native 95/95; flash 2,795,605 → 2,781,813 B. Shot diff vs baseline: 248/256 differ, every diff confined to rows ≤ 30 (status-bar clock) except `05-settings_page02` (the clock-seeded minute spinbox, known run-to-run variance).
- W13 H3.1 ✅ 557a188. `make protocol-test` → 0 (45 Unity, Go ok, 13 XCTest); `go run ./cmd/pagerlink smoke` vs emulator → "all steps passed", sys.info has no "hub" key. iOS app target not compiled (Linux; Track B).
- W14 H3.2 ✅ d0e8e64.
- W21 H5.1 ✅ 7e68e64. tools/pagerlink: `go test -race -cover` → internal/pagerlink 78.7 % (= baseline); vectors path needed one more `..`; server still `go vet` + `go test -race` green without it; protocol-test 0; smoke passed from the new dir.
- H6.3 negative control (pre-H4) ✅: the zero-reference grep still finds live hits in src/hal/hub.*, settings_hub.cpp, ui_chat.cpp, ui_notes_sync.cpp, core/system.cpp, server/.
- Parked: W7–W12 (iOS plan Track C 7/9 not landed: no handlers_fs.cpp, no hal/weather_cache), W15 (D-H1 BLE hardware gate); W16–W26 wait behind them.
- Observation: `make protocol-test` leaves .pio/build/emulator_lora_pager/program missing (seen twice); rebuild before a smoke run.

### 2026-10-11 — migrated to plan-doc v2
Everything below this entry is the pre-v2 body, kept verbatim. The live plan is above `## Log`.

### Pre-v2 body


> **Status:** proposed, 2026-10-11. Nothing executed yet.
> **Goal:** remove lilyhub completely: the Go service under `server/`, its
> systemd and cross-compile tooling, and every firmware path that talks to it.
> The features that use the hub today move to the **iOS companion app** over
> PagerLink (`IOS_COMPANION_PLAN.md`, `protocol/PROTOCOL.md`).
> **Supersedes:** the `IOS_COMPANION_PLAN.md` non-goal "Replacing lilyhub. The
> phone is an extra uplink alongside the hub, not a substitute for it." After
> this plan, the phone is the only uplink besides the pager's own Wi-Fi.
>
> Facts below were read from the working tree on top of `1f05e0b`, including
> its **uncommitted** edits (see H0.1). References use file + symbol, not line
> numbers.

---

## 1. Hub inventory

### 1.1 Features that use the hub

| Feature | Firmware entry point | Hub endpoint | Fallback today | Replacement |
|---|---|---|---|---|
| **Weather** | `src/apps/ui_weather.cpp`: `fetch_with_hub_fallback`, called by `fetch_geo_ip`, `fetch_geo_city`, `fetch_weather` (`HUB_PATH_GEO_IP`, `HUB_PATH_GEO_SEARCH`, `HUB_PATH_FORECAST`) | `/api/weather/geo/ip`, `/geo/search`, `/forecast` | direct ip-api / Open-Meteo over Wi-Fi | `weather.get` (D→P) + `weather.push` (P→D). This is already Phase 3 of the iOS plan |
| **Chat** (Groq LLM + Whisper STT) | `src/apps/ui_chat.cpp`: `kick_off_send`, `chat_post` (raw `WiFiClient` POST), `enter` (hub banner) | `/api/chat` | **none**: no Groq key on the device | `chat.ask` (D→P). The phone holds the Groq key and history |
| **Notes sync** to GitHub | `src/apps/ui_notes_sync.cpp`: `run_sync_bg`, `try_hub_sync` | `/api/notes/upload` + `/api/notes/sync` | **none**: `run_sync_bg` refuses to run when the hub is off | `notes.sync` (D→P). The phone pulls notes over `fs.*` and pushes to GitHub |
| **Notes mirror** ("Copy Notes -> Hub") | `src/apps/settings_storage.cpp`: `storage_copy_to_hub_cb` → `src/hal/storage_bulk.cpp`: `hw_copy_all_notes_to_hub` | `/api/notes/upload` | none | `notes.sync` with target `phone`: the phone stores the notes in its Files-app folder |
| **Eviction mirror** | `src/hal/storage.cpp`: `hw_prune_internal_storage` (`hub_on` / `hub_ok` branch) | `/api/notes/upload` | SD copy (always made) | **Dropped.** The SD copy is the safety net, as it is when the hub is off today |
| **Telegram** (proxy + uplink) | `src/apps/ui_telegram.cpp`: `tg_http_request` (`hub_last_reachable` branch) | `/api/telegram/proxy` | direct HTTPS to the user's **tg-bridge** (bridge URL + bearer in NVS `tgbridge`) | Hub proxy **dropped**: `allowedTelegramURL` only forwards `https://api.telegram.org`, so it already answers 4xx for every bridge call (P6.28) and the request falls through to direct. **New phone fallback:** `tg.*` (D→P) when the pager has no internet (§2.2) |

### 1.2 Hub plumbing (deleted outright)

| Where | What |
|---|---|
| `src/hal/hub.{h,cpp}` | NVS namespace `hub` (`url`, `enabled`), legacy `weather/hub_url` migration, `hub_is_reachable` TCP probe, reachability cache, `hub_upload_note` |
| `src/apps/settings_hub.cpp` + `settings_internal.h` `namespace hub_cfg` | Settings » Local Hub subpage |
| `src/apps/ui_settings.cpp` | the `"Local Hub"` row in the subpage table; `hub_cfg::reset_state()` in `ui_sys_exit` |
| `src/core/system.cpp` | `_statHubLabel` status-bar icon and the `hub_probe` FreeRTOS task, which does a TCP connect every ~30 s while the hub is enabled (`setupGlobalUI`) |
| `src/apps/menu_app.cpp` | stray `#include "../hal/hub.h"` |
| `src/hal/result.{h,cpp}` | `HalError::HubDisabled`, `HalError::HubUnreachable` and their strings |
| `src/hal/link/link_rpc.cpp` | `hal_error_code` → `"hub_disabled"` / `"hub_unreachable"` |
| `src/hal/link/handlers_sys.cpp` | `sys.info` `"hub":{"on","reachable"}` object |
| `src/hal/str_encode.{h,cpp}` | `parse_url`, which is dead once `hub_is_reachable` and `chat_post` are gone (its only two callers) |
| `src/hal/storage.h` | `hw_copy_all_notes_to_hub` declaration + comment |
| `test/test_hal_result`, `test/test_link_rpc` (`test_hal_error_code`), `test/test_str_encode` (`parse_url` cases) | assertions on the removed symbols |

### 1.3 Server side (deleted)

`server/cmd/lilyhub/`, `server/internal/{cache,httpx,weather,chat,notessync,telegram}/`,
`server/deploy/lilyhub.service`, the lilyhub targets in `server/Makefile`
(`build`, `linux-*`, `install`, `upgrade`, `uninstall`, `set-groq-key`, …),
`server/README.md`, `server/bin/` (the arm64 binary is already staged as
deleted), and the `/server/bin/` entry in `.gitignore`.

**Kept (relocated):** `server/cmd/pagerlink/` and `server/internal/pagerlink/`.
They are PagerLink tooling, not hub code: the Go codec tested against the
golden vectors, plus the emulator integration CLI that `make protocol-test` and
the `pagerlink smoke` run rely on. They currently live in the
`github.com/lilygo/lilyhub` Go module, so they move before `server/` is
deleted (H5, decision D-H4).

### 1.4 Other references

- iOS: `ios/App/Features/Dashboard/DashboardView.swift` (`"Hub"` row,
  `hubText`), `ios/App/Session/PagerSession.swift` (doc comment).
- Spec: `protocol/PROTOCOL.md` §3.4 (`hub_disabled`, `hub_unreachable`), §6
  (`sys.info` `hub` field).
- Docs: `CLAUDE.md` (overview, layout tree, "Lilyhub (`server/`)" section, the
  `hub_upload_note` example under HAL, the settings-split list),
  `IOS_COMPANION_PLAN.md` (goal 3, non-goals, §2, §3, §5.5, Phases 5/6),
  `REFACTOR_NOTES.md` (historical mentions; append a note, don't rewrite),
  `Makefile` `protocol-test` (`cd server && go test ./internal/pagerlink/`).
- `OPTIMIZATION_*.md` and other historical reports: leave as written.

---

## 2. Target architecture

```
                 before                                   after
pager ──Wi-Fi──▶ lilyhub ──▶ internet          pager ──Wi-Fi──────────────▶ internet  (weather, Telegram)
      ──Wi-Fi──────────────▶ internet          pager ──PagerLink (BLE)──▶ iPhone ──▶ internet
                                                                          (weather, chat, notes → GitHub / Files,
                                                                           Telegram → tg-bridge)
```

| Feature | New chain on the pager |
|---|---|
| Weather | Wi-Fi up → direct. No Wi-Fi and link connected → `weather.get` to the phone. Otherwise → cached `ForecastCache` with the stale badge. The phone also sends `weather.push` on connect and on every `BGAppRefreshTask`, so the cache is usually fresh before anyone asks |
| Chat | link connected → `chat.ask`. Otherwise a clean "Connect your phone" error. No direct path, same as today without the hub |
| Notes sync | link connected → `notes.sync`. Otherwise "Connect your phone" |
| Telegram | internet up on Wi-Fi → direct to tg-bridge. Otherwise, link connected → `tg.*` to the phone, which calls the bridge. Otherwise → "Offline" status, as today's "No internet" |

Secrets move off the LAN box and, for notes sync, off the pager (D-H2):

| Secret | Before | After |
|---|---|---|
| Groq API key | `/etc/lilyhub/env` on the hub | iOS Keychain, set in the app's Settings |
| GitHub PAT (notes) | pager secret store (`nsync_cfg_set_token`), sent to the hub per request | iOS Keychain (recommended), scrubbed from the pager |

### 2.1 New PagerLink methods

These are added to `protocol/PROTOCOL.md` §6 when each one lands. JSON
payloads only, so the framing vectors don't change. Each method gets
`pagerlink` CLI coverage.

| Method / event | Dir | Params → result |
|---|---|---|
| `weather.get` | D→P | `{key:"auto"\|<city>}` → `ForecastPayload` (iOS plan §5.5) |
| `weather.push` | P→D | `ForecastPayload` → `{}`; saved via `hal/weather_cache` |
| `loc.get` | D→P | → `{lat, lon, acc_m, city?}` |
| `chat.audio.begin` | D→P | `{size, crc32, fmt:"wav"}` → `{xfer}`. The phone is the receiver, so it allocates the transfer id (PROTOCOL §4). The pager then streams DATA |
| `chat.ask` | D→P | `{text?, audio_xfer?, reset?}` → `{transcript?, reply}`. Named timeout of 60 s, like `fs.write.commit` |
| `notes.sync` | D→P | `{targets:["github"\|"phone"], files:[{name, size, mtime, crc32}]}` → `{job}`. The phone answers immediately, then pulls only the files it needs with `fs.read` (Phase 2 file service) |
| `evt.notes.sync` | P→D | `{job, phase, uploaded, already, errors[], done}`. Progress and the final summary, rendered in the Notes Sync log |
| `tg.chats` | D→P | `{bridge, token, limit}` → `{chats:[{id,title,unread}]}`. The phone calls `GET <bridge>/v1/chats?limit=` |
| `tg.messages` | D→P | `{bridge, token, chat_id, limit}` → `{msgs:[{id,text,from,out,media?:{type,w,h}}]}` (`GET /v1/chats/{id}/messages`) |
| `tg.read` | D→P | `{bridge, token, chat_id, up_to}` → `{}` (`POST /v1/chats/{id}/read`) |
| `tg.send` | D→P | `{bridge, token, chat_id, text}` → `{}` (`POST /v1/chats/{id}/messages`) |

Error codes: `HubDisabled` / `HubUnreachable` are replaced by one
`HalError::LinkOffline` ("Phone not connected"). It maps to the existing
local-only `disconnected` code and is never sent on the wire. PagerLink v1 has
not shipped to any user, so removing `hub_disabled` / `hub_unreachable` and the
`sys.info` `hub` field doesn't bump `proto`. Firmware, Swift and Go change in
the same commit (PROTOCOL.md rule).

### 2.2 Telegram via the phone

Today the Telegram app (`src/apps/ui_telegram.cpp`) polls the user's
tg-bridge: a Go service on a Pi or VPS, outside this repo, that holds the
MTProto session. The pager makes four calls, all through `tg_http_request`:

| Pager caller | Bridge call |
|---|---|
| `fetch_chats` (foreground list, `tg_bg_task` unread notifier, `tg_cfg_fetch_all_chats` favourites picker) | `GET /v1/chats?limit=30` |
| `tg_fg_task` `FG_FETCH_MSGS` | `GET /v1/chats/{id}/messages?limit=20` |
| `tg_fg_task` mark-read | `POST /v1/chats/{id}/read {"up_to":n}` |
| `tg_fg_task` `FG_SEND` | `POST /v1/chats/{id}/messages {"text":…}` |

Design:

- **Typed methods, not a generic HTTP proxy.** Each `tg.*` method builds its
  own fixed `/v1/...` path from typed params (`chat_id` and `limit` are
  integers). The phone never fetches a path or URL the pager makes up, so the
  link can't be used as an open relay. Only the base URL comes from the
  pager.
- **Credentials travel with each call** (D-H7). The pager stays the only
  place the bridge URL and bearer are configured (Settings » Telegram, NVS
  `tgbridge`, `token_enc`), since the direct Wi-Fi path needs them anyway.
  The link is encrypted and bonded (iOS plan §8). The phone uses the token
  for that one request and never logs or stores it. The pager scrubs its copy
  after the call (`hal::secret_scrub`), as it does today.
- **Response size.** A RES payload is capped at 4096 B (PROTOCOL §3), and 20
  messages can exceed that. The phone trims each reply to the fields the
  pager parses (`id`, `title`, `unread` / `id`, `text`, `from`, `out`,
  `media{type,w,h}`). If the trimmed reply still doesn't fit, it returns
  `{xfer, size, crc32}` and streams the JSON as DATA, the same mechanism
  `fs.read` uses (Phase 2). The pager feeds the reassembled body to the
  unchanged parsers (`fetch_chats` parsing, `tg_parse_msgs`).
- **Uplink choice.** Replace the `internet_available()` gate (Wi-Fi + 1.1.1.1:53
  probe) with a `tg_uplink()` verdict: `Direct` when Wi-Fi is up and the probe
  passed, `Phone` when `hw_link_connected()`, `None` otherwise. `poll_tick`,
  `tg_bg_tick` / `tg_bg_task` and `tg_cfg_fetch_all_chats` all use it.
  `tg_http_request` keeps its signature. The transport is picked inside it,
  so the four callers don't change.
- **Background unread notifier.** `tg_bg_task` keeps its 60 s / 5 min cadence
  over the link when there's no Wi-Fi. A pager-initiated BLE request wakes a
  suspended iOS app through `bluetooth-central` background mode. That's more
  reliable than having the phone poll the bridge on `BGAppRefreshTask`, which
  iOS schedules at its own discretion. The cost is one small RPC per minute
  while the user is active: measure it in bench row B1.
- **Errors.** The bridge's HTTP status is passed through as `{status}` in an
  ERR `http_error`, and 401/403 map to `unauthorized`. The pager shows the
  same status line it shows for the direct path. A dropped link mid-request
  shows "Phone disconnected" and the next poll retries.
- **Emulator.** The phone path is compiled for the emulator too (today the
  whole Telegram network layer is `#ifdef ARDUINO`), so `pagerlink serve`
  can run Telegram end to end against a stub bridge. The direct path stays
  ARDUINO-only.

---

## 3. Decisions

| ID | Decision | Recommendation |
|---|---|---|
| D-H1 | **Sequencing.** Chat and notes sync have no non-hub path. Delete the hub before the BLE transport works on hardware, and both features are dead on the device until it does | **Gate on the BLE transport.** Run H1–H3 now: they cost no hardware feature and everything is testable on the emulator. Run H4 (cut-over + firmware deletion) only after iOS-plan Track A step 5 (`link_transport_ble.cpp`) passes on a real pager + iPhone. The alternative is to accept the gap and do everything now. That's faster, but chat and notes sync are unusable on hardware until BLE lands |
| D-H2 | Where the GitHub PAT, repo and branch live | **Phone (Keychain + app settings).** Repo, branch and token are configured in the app. The pager's `settings_notes_sync.cpp` shrinks to a read-only "configured on phone" row, and the stored PAT is scrubbed from the pager's secret store. Alternative: keep the config on the pager and send the token in `notes.sync`. That puts the secret on the BLE link, but the link is encrypted and bonded |
| D-H3 | Speech-to-text for voice chat | **Groq Whisper, called from the phone** (parity with `server/internal/chat`, including the `STT_LANG=pt` default and the vocabulary prompt). Apple `SFSpeechRecognizer` can be an option later, but the audio still has to cross the link either way |
| D-H4 | Home of the Go PagerLink tooling | **`tools/pagerlink/`** as its own Go module (`github.com/lilygo/pagerlink`), holding `cmd/pagerlink` and `internal/pagerlink` unchanged apart from import paths. `tools/` already holds the developer tools |
| D-H5 | Orphaned NVS (`hub` namespace, `weather/hub_url`) | **One-shot cleanup** at boot in HAL init: erase both if present, guarded by a flag key so it runs once. It's a few bytes, but leaving dead namespaces around contradicts "remove all support" |
| D-H6 | Weather order when Wi-Fi *and* the link are both up | **Direct first.** It matches today's behaviour and keeps working when the app is suspended. The proactive `weather.push` keeps the cache fresh, so the order rarely matters |
| D-H7 | Telegram bridge URL + bearer for the phone path | **Send them with each `tg.*` call.** The pager remains the single place they're configured, and the direct path needs them anyway. Alternative: configure them again in the iOS app (Keychain). That keeps the token off the link but creates two copies that can disagree |
| D-H8 | Telegram order when Wi-Fi *and* the link are both up | **Direct first**, as in D-H6. The phone is used only when the direct path is unavailable |

---

## 4. Work items

Every item ends with these checks: `pio run -e tlora_pager` and
`pio run -e emulator_lora_pager` green, plus `pio test -e native_test` green
(house rule, `REFACTOR_NOTES.md`). Protocol items also need `make
protocol-test`. The final item builds all six envs.

### H0 — Preconditions

- [ ] **H0.1 Land or park the uncommitted hub work.** The working tree carries
  about 1,300 lines of uncommitted edits in `server/internal/{chat,notessync,telegram,weather,cache}`,
  `server/cmd/lilyhub/main.go`, `server/README.md`, `src/hal/hub.cpp`, and the
  four consumer apps (`ui_chat`, `ui_notes_sync`, `ui_telegram`,
  `ui_weather`). Commit them first, so the final lilyhub behaviour is in
  history and can serve as the reference for the Swift ports in H2. The
  alternative is to discard them explicitly. Don't delete code whose latest
  version exists only in the working tree.
- [ ] **H0.2 Record the decisions** above (D-H1 … D-H8) in this file.
- [ ] **H0.3 Behaviour baseline.** Before anything changes, capture the
  reference outputs the ports must match: `go test ./...` in `server/`
  (green, with coverage numbers), and the UI screenshot sweep
  (`UI_SHOTS_DIR=/tmp/shots-before …`) for diffing in H4.

### H1 — Drop the hub where a direct path already exists (no feature loss)

- [ ] **H1.1 Telegram:** delete the `hal::hub_last_reachable()` branch of
  `tg_http_request`, so only the direct `hw_http_request` path to the
  tg-bridge remains. Remove the P6.28 proxy comments. This changes nothing
  for users: the hub's allowlist already rejected bridge URLs.
- [ ] **H1.2 Eviction:** delete the `hub_on`/`hub_ok` mirror in
  `hw_prune_internal_storage`. A note is removed from FFat only after the SD
  copy succeeds (`if (copied)`). **Regression test + negative control:** this
  is the data-safety edge. Verify on the emulator that an eviction with SD
  write forced to fail keeps the FFat file.
- [ ] **H1.3 Weather direct-only:** replace `fetch_with_hub_fallback` with a
  plain direct fetch and delete the `HUB_PATH_*` constants. Screenshot diff
  of the Weather app shows no change.

### H2 — Build the phone-side replacements (emulator + `pagerlink`, no hardware)

These depend on iOS-plan Track C items 7 (`fs.*` file service) and 9
(`weather_cache` + `handlers_weather`). Do those first; this list only adds
what is hub-specific.

- [ ] **H2.1 Pager-initiated RPC API.** Add `hw_link_call(method, params,
  timeout_ms, cb)` (and the transfer-out helper for DATA) to `src/hal/link.h`.
  Completion runs on the link task and is posted to LVGL through the existing
  async-job path. **(verify)** that `link_service` already routes incoming EVT
  messages to a subscriber; add the routing if it doesn't.
- [ ] **H2.2 `pagerlink` mock phone.** Give the Go client a responder role:
  `pagerlink serve` answers D→P requests (`weather.get`, `loc.get`,
  `chat.audio.begin`, `chat.ask`, `notes.sync`) with canned or recorded
  results, and drives `fs.read` for notes. This is the emulator test harness
  for H2.3–H2.5 and for H4's end-to-end checks.
- [ ] **H2.3 Weather via phone (iOS Phase 3).** Add the `weather.get` step to
  the fetch chain (§2 order, D-H6) and a "via phone" source label. iOS side:
  Open-Meteo provider (same fields the firmware asks for), CoreLocation for
  `auto`, a geocoding search trimmed like `trimGeoSearch`, and the hub's cache
  TTLs (forecast 10 min, geo-search 24 h, geo-ip 30 min) in the app.
- [ ] **H2.4 Chat via phone.**
  - Firmware: rewrite `kick_off_send` / `send_task` to call `chat.audio.begin`
    + DATA (voice) and then `chat.ask`. Delete `chat_post` and the raw
    `WiFiClient` code. "No Wi-Fi" is no longer an error; "no phone" is. Chat
    now works in the **emulator** too (it used to be "chat requires hardware
    build").
  - Recording size: **(verify)** the format `hw_rec_*` produces. Cap the
    voice memo so the upload stays under ~10 s at the throughput measured in
    S3. 16 kHz/16-bit mono is about 32 KB/s of audio.
  - iOS: `ChatService` ports `server/internal/chat` (after H0.1): Groq chat
    completions, Whisper STT with the `STT_LANG` / `STT_PROMPT` defaults, and
    per-pager history (last 20 messages, `reset`). The key is set in Settings
    and stored in the Keychain.
  - Tests: port `chat_test.go` cases to XCTest with a `URLProtocol` stub.
- [ ] **H2.5 Notes sync via phone.**
  - Firmware: `run_sync_bg` sends `notes.sync` with the internal-FFat
    manifest (name, size, mtime, CRC-32). It no longer reads bytes itself:
    the phone pulls each file it needs with `fs.read`, as raw ciphertext for
    encrypted notes (iOS plan §8, `protected:true`). The log renders
    `evt.notes.sync`. Delete `try_hub_sync` and the hub-upload loop.
  - "Copy Notes -> Hub" becomes **"Copy Notes -> Phone"**:
    `hw_copy_all_notes_to_hub` becomes a `notes.sync {targets:["phone"]}`
    call that includes SD notes, keeping the existing internal-wins-on-conflict
    rule.
  - iOS: `NotesSyncService` ports `server/internal/notessync`: additive push
    through the Git Data API (list `notes/`, parallel blob creates, one tree +
    commit + ref update, skip names already present). The "phone" target
    writes into the app's Documents folder, visible in the Files app. Repo,
    branch and PAT live in app settings + Keychain (D-H2).
  - Tests: port `notessync_test.go` cases. Byte-for-byte round trip of an
    encrypted note: the GitHub blob equals the pager's raw `Salted__` bytes.
    Negative control: a plaintext read path must fail the test.

- [ ] **H2.6 Telegram via phone** (design in §2.2).
  - Firmware: add `tg_uplink()` and route `tg_http_request` to the four
    `tg.*` calls when the verdict is `Phone`. Accept a DATA-delivered body
    for large replies. Gate `poll_tick`, `tg_bg_task` and
    `tg_cfg_fetch_all_chats` on `tg_uplink() != None` instead of Wi-Fi only.
    Show "via phone" in the status line, matching the weather source label.
    Keep the "State reset on onStop" block current if the worker gains
    link-call state.
  - iOS: `TelegramService` implements the four handlers with `URLSession`.
    It builds paths from typed params, trims replies to the parsed fields,
    switches to bulk transfer above 4096 B, and never stores the token.
    **(verify)** App Transport Security for an `http://` LAN bridge: it needs
    `NSAllowsLocalNetworking` plus the local-network permission prompt
    (`NSLocalNetworkUsageDescription`). An HTTPS bridge needs neither.
  - `pagerlink serve`: a stub tg-bridge behind the four handlers, so the
    emulator runs list → open chat → send → mark-read over the link.
  - Tests: XCTest for path building (a non-integer `chat_id` is rejected),
    trimming, the >4096 B bulk switch, and 401 → `unauthorized`.
    **Regression test + negative control:** on the emulator, with Wi-Fi
    "down" and the link up, the unread notifier fires once for a new message.
    With the link also down, it stays silent and the status reads "Offline".

### H3 — Protocol and app cleanup that doesn't need hardware

- [ ] **H3.1** In one commit: remove `hub_disabled` / `hub_unreachable`
  from `PROTOCOL.md` §3.4 and `link_rpc.cpp`, and add `HalError::LinkOffline`
  ↔ local `disconnected`. Drop the `sys.info` `hub` object from
  `handlers_sys.cpp`, PROTOCOL §6, the Go `pagerlink smoke` expectations, and
  the iOS Dashboard (`hubText` + row). Update `test_link_rpc`
  `test_hal_error_code` and `test_hal_result`.
- [ ] **H3.2** Update `IOS_COMPANION_PLAN.md`: replace the "Replacing
  lilyhub" non-goal with a pointer to this plan; revise goal 3, §5.5 (new
  chain) and Phase 5/6 wording (notes and chat are no longer "next to
  lilyhub").

### H4 — Cut-over and firmware deletion (gated by D-H1 on BLE transport)

- [ ] **H4.1 Hardware acceptance** on a real pager + iPhone, with Wi-Fi off on
  the pager: Weather shows a phone-fetched forecast; a typed and a spoken chat
  question get replies; Notes Sync pushes a new note to GitHub; "Copy Notes ->
  Phone" lands files in the Files app. Telegram, with Wi-Fi off, lists chats,
  opens one, sends a message and marks it read through the phone, and a new
  message buzzes the pager while the iOS app is in the background. With Wi-Fi
  back on, Telegram goes direct to the bridge (no link traffic). Record the
  evidence in §6.
- [ ] **H4.2 Delete the firmware hub:** `src/hal/hub.{h,cpp}`,
  `src/apps/settings_hub.cpp`, `hub_cfg` in `settings_internal.h`, the `Local
  Hub` row + `reset_state` call in `ui_settings.cpp`, the include in
  `menu_app.cpp`, `HubDisabled` / `HubUnreachable` in `result.{h,cpp}`, and
  `parse_url` + its tests in `str_encode` / `test_str_encode`.
- [ ] **H4.3 Status bar:** replace `_statHubLabel` and the `hub_probe` task in
  `System::setupGlobalUI` with a link indicator driven by
  `hw_link_connected()`. That read is non-blocking, so no probe task or timer
  is needed. This removes a periodic Wi-Fi TCP connect: note it as a
  power-positive change and re-run bench row B1.
- [ ] **H4.4 NVS cleanup** (D-H5): one-shot erase of namespace `hub` and
  `weather/hub_url`. If D-H2 = phone, also scrub the notes-sync PAT from the
  pager's secret store. Emulator test: seed the keys, boot, and confirm
  they're gone and a second boot is a no-op.
- [ ] **H4.5 Screenshot sweep** in every style × dark mode. Diff against the
  H0.3 baseline. Expected changes: Settings has no Local Hub page, Storage
  shows "Copy Notes -> Phone", and the status bar shows the link icon. Nothing
  else should change.
- [ ] **H4.6 Build all six envs:** `tlora_pager`, `twatchs3`, `twatch_ultra`
  and their three emulators. Hub code compiled on every board, so every board
  has to build without it.

### H5 — Delete lilyhub (server side)

- [ ] **H5.1 Relocate the PagerLink Go tooling** (D-H4): move
  `server/cmd/pagerlink` and `server/internal/pagerlink` to `tools/pagerlink/`
  with a new `go.mod`, and rewrite the import paths. Point the root
  `Makefile` `protocol-test` at it and update the `CLAUDE.md` build line
  (`cd tools/pagerlink && go run ./cmd/pagerlink smoke`). Run `go test -race
  -cover ./...` there; coverage must not drop below today's 78.7 % for
  `internal/pagerlink`.
- [ ] **H5.2 Delete `server/`** entirely (§1.3), and the `/server/bin/` line
  plus its comment in `.gitignore`.
- [ ] **H5.3 Decommission note** for anyone running a hub: `sudo systemctl
  disable --now lilyhub && sudo rm -rf /opt/lilyhub /etc/lilyhub
  /etc/systemd/system/lilyhub.service && sudo userdel lilyhub`. Put it in the
  commit body and the README. The repo can no longer run `make uninstall`
  once `server/` is gone, so do this before H5.2 on any box that has a hub
  installed.

### H6 — Docs and final sweep

- [ ] **H6.1 `CLAUDE.md`:** remove the lilyhub sentence in the overview, the
  `server/` line in the layout tree (add `tools/pagerlink/`), the "Lilyhub
  (`server/`)" section and `settings_hub.cpp` from the settings-split list.
  Change the HAL example from `hub_upload_note` to a surviving `HalResult`
  call site.
- [ ] **H6.2** `REFACTOR_NOTES.md`: append a dated entry. Historical lines
  stay as written. `README.md`: one-line pointer to the companion app.
- [ ] **H6.3 Zero-reference check.**
  `git grep -n -i -E 'lilyhub|hal/hub\.h|hub_(get|set|is|last|note|upload)|Hub(Disabled|Unreachable)|Local Hub|/api/(chat|notes|weather|telegram)'`
  must return hits only in historical docs (`OPTIMIZATION_*.md`,
  `REFACTOR_NOTES.md` history, this plan). Negative control: run it once
  before H4 and confirm it does find the live references.

---

## 5. Risks

| # | Risk | Mitigation |
|---|---|---|
| RH1 | Chat and notes sync go dark on hardware if H4 runs before BLE works | D-H1 gate. H1–H3 are safe to ship on their own |
| RH2 | Voice upload is too slow over GATT | Cap the recording length (H2.4). L2CAP CoC (iOS plan R3). Show upload progress in the chat status line |
| RH3 | The iOS app is suspended when the pager asks (`chat.ask`, `notes.sync`) | `bluetooth-central` background mode + state restoration (iOS plan R6). Network calls from a background BLE wake are time-limited: **(verify)** that a Groq round trip fits. If not, return `busy` and ask the user to open the app |
| RH4 | Users with a configured hub lose the Telegram proxy | No real loss: the proxy only forwarded `api.telegram.org`, so every tg-bridge call already went direct (P6.28). Check the direct path on hardware in H4.1 |
| RH8 | The Telegram background poll over BLE costs power on both devices | Same 60 s / 5 min cadence as Wi-Fi, small JSON, and only when Wi-Fi is down. Measure in bench row B1. If it's too costly, slow the phone-path cadence or switch to a phone-pushed `evt.tg.unread` |
| RH9 | The phone can't reach a LAN-only tg-bridge (phone off the home network, or ATS blocks `http://`) | Status line shows the bridge error. Document that the phone fallback needs a bridge the phone can reach (HTTPS/VPS, or the same LAN with local-network permission) |
| RH5 | PAT migration (D-H2): the user has to re-enter the GitHub token on the phone | One-time prompt in the Notes Sync app ("configure on your phone") and onboarding copy in the iOS app |
| RH6 | Behaviour drift in the Swift ports of chat and notes sync | Port the Go tests first (H2.4/H2.5) and use the H0.1-committed Go code as the reference |
| RH7 | Wi-Fi/BLE coexistence while the phone fetches weather and the pager is on Wi-Fi | Unchanged from iOS plan R4. Direct-first ordering (D-H6) avoids BLE traffic when Wi-Fi is up |

---

## 6. Progress log

_(append dated entries with evidence; never rewrite earlier ones)_
