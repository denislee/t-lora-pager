# iOS Companion App — Feature Suggestions (2026-10-29)

> Follow-on to `IOS_COMPANION_PLAN.md` (the architecture and phase plan) and
> `protocol/PROTOCOL.md` (PagerLink v1). This doc lists what to build next in
> the iOS app (`ios/`), in a suggested order, using what exists as of `dd840b4`.

## 1. What exists today

**App screens** (`ios/App/Features/`): Dashboard, Diagnostics, Notes (view,
edit, delete, move to GitHub), Tasks, Telegram (TDLib runs on the phone), and
Settings (built from the pager's `settings.schema`).

**Background services** (`ios/PagerKit/Sources/PagerKit/Services/`):
`ChatService` (`chat.ask` + `chat.audio.begin`), `WeatherService`
(`weather.get` / `weather.push`), `NotesSyncService` (`notes.sync`),
`TelegramService` / `TDTelegram` (`tg.*`), `SettingsService`, `SysService`
(including automatic clock resync), and `FileService` (`fs.*`).

**Not built yet.** These methods have no code in `src/` or `ios/`:
`notify.post`, `clip.push`, `phone.find`, `text.send`, `loc.get` (only a
mock in tests), `ota.*`, `screen.capture`, `lora.send` / `evt.lora.rx`.
`IOS_COMPANION_PLAN.md` §4.5 reserves them for Phases 4 and 6.

**Built but with no screen:** `FileService` (the full `fs.*` API) and
`WeatherService`.

## 2. Suggested order

| # | Item | Firmware change? | Size |
|---|---|---|---|
| S1 | Files screen | No | M |
| S2 | Weather screen | No | S |
| S3 | Dashboard quick actions | No | S |
| S4 | Send text to pager (`notify.post`) + share extension | Yes | M |
| S5 | Shared clipboard (`clip.push`) | Yes | S |
| S6 | Phone as keyboard (`text.send`) | Yes | S |
| S7 | Find my phone / find my pager (`phone.find`) | Yes | S |
| S8 | Voice memo pull + transcription | No (uses `fs.*`) | M |
| S9 | Journal tab | No (uses `fs.*`) | M |
| S10 | SSH host manager | Yes (settings rows or file format) | M |
| S11 | Chat history | No | S |
| S12 | Firmware update over Bluetooth (OTA) | Yes | L |
| S13 | Screen capture (`screen.capture`) | Yes | M |
| S14 | LoRa messenger (`lora.send` / `evt.lora.rx`) | Yes | L |
| S15 | iPhone notifications on the pager (ANCS) | Yes, large | L |
| S16 | Widgets + Live Activity | No | M |
| S17 | Siri Shortcuts / App Intents | No (after S4–S7) | S |
| S18 | Battery history chart | No | S |
| S19 | Diagnostics export | No | S |

Start with S1–S3 (app-only, nothing to flash), then S4–S7 (one firmware +
app round), then S12 (OTA, which also gets day-to-day flashing away from the
RTS/download-mode quirk).

## 3. Quick wins: the code exists, only a screen is missing

### S1 — Files screen

`FileService` and the firmware's `fs.list/stat/read/write.*/delete/mkdir/rename`
are done and tested, but no tab uses them. `IOS_COMPANION_PLAN.md` §6.1
already describes the screen.

- A switch between Internal and SD, breadcrumb navigation, a list with size and
  mtime, folders first (the order `fs.list` already returns).
- Pull to refresh. Drop cached listings on `evt.fs.changed`.
- Swipe to delete or rename, plus a "New folder" button.
- Tap to download with progress, then open in Quick Look or a text preview,
  with a share sheet.
- Upload from the Files app or Photos with `fs.write.begin` → DATA →
  `fs.write.commit`, resuming from `offset` after a disconnect.
- A lock icon on `protected` (encrypted) files. Never decrypt them here; Notes
  owns decryption.

**Done when:** a file round-trips byte-for-byte (CRC32 matches) between
iPhone and SD in both directions against the emulator (`127.0.0.1:7600`), and
an editor save on the pager refreshes an open listing.

### S2 — Weather screen

`WeatherService` already answers `weather.get` and sends `weather.push`, but
nothing shows it.

- The location source (GPS / city / `weather.location` setting), the last
  fetch time, and the last push to the pager.
- A "Push now" button.
- A preview drawn like the pager draws it: the same WMO codes, 8 hourly
  slots, and 7 days (`PROTOCOL.md` §6.3 field limits).

**Done when:** "Push now" updates an open Weather app on the emulator, and the
preview matches the pager screenshot (`UI_SHOTS_DIR` sweep).

### S3 — Dashboard quick actions

- "Sync time now" sends `sys.time.set` (the automatic resync from `dd840b4`
  already does this; the button just exposes it).
- "Refresh weather" (S2) and "Open pager settings."
- After S4–S7: "Send text" and "Find pager."

## 4. Phase 4: phone ↔ pager messaging

These need new firmware handlers next to the existing `link_*` RPC modules,
golden vectors from `protocol/tools/mkvectors.py`, and Unity + Go + PagerKit
codec tests (`make protocol-test`). Each one adds a `caps` entry so older
firmware hides the feature.

### S4 — Send text to the pager (`notify.post`)

- `{id, title, body, src, ts, prio}` → the pager shows it with the shared
  `ui_popup_create` / `ui_result_show` helpers and saves it in a small inbox
  ring in FFat.
- An iOS **share extension** so any app can "Share → Pager."
- A notification policy for fake sleep: wake the display or queue silently,
  depending on `prio`.

**Done when:** a message typed on the phone shows on the pager within 1 s,
including during fake sleep (`IOS_COMPANION_PLAN.md` Phase 4).

### S5 — Shared clipboard (`clip.push`, both ways)

- Pager → phone: put the text on the iOS pasteboard in the foreground, or
  post a local notification in the background.
- Phone → pager: paste into the text editor's buffer.

### S6 — Phone as keyboard (`text.send`)

- `{text, target:"editor"|"kb"}` types into the pager's text editor or into the
  focused input (SSH session, Telegram reply).

### S7 — Find my phone / find my pager (`phone.find`)

- From the pager: ring a locked iPhone with a time-sensitive local
  notification and a sound.
- The reverse (phone → pager): beep and flash the pager.

**Done when:** "Find phone" from the pager rings a locked iPhone.

## 5. Phone counterparts for apps that only run on the pager

### S8 — Voice memos

`ui_audio_notes` records on the pager. The phone pulls the recordings over
`fs.read`, transcribes them with the Groq/Whisper path `ChatService` already
uses for `chat.ask`, and saves the text as a note or journal entry. Offer
"delete from pager after import" behind a toggle.

### S9 — Journal tab

`ui_journal` already reads rewritten notes (`aa3d4ce`), but the phone has no
Journal tab. Add a calendar view of entries, open an entry, and an "append to
today" action that writes through `fs.write.*`, the same way `NotesLibrary`
does.

### S10 — SSH host manager

`ui_ssh` is pager-only, and typing a private key on the pager keyboard is
painful. Edit hosts (name, host, port, user) and keys on the phone, store keys
in the Keychain (`KeychainSecretStore`), and push them to the pager as
settings-registry rows (`secret` type for the key, which never travels pager →
phone) or as a protected file.

### S11 — Chat history

Every `chat.ask` passes through the phone, so the phone can keep the full
conversation (searchable, copyable, exportable), while the pager keeps only
the last few turns.

## 6. Phase 6: bigger items

### S12 — Firmware update over Bluetooth (OTA)

- `ota.begin` / DATA / `ota.commit` writes the inactive `app0`/`app1` slot
  (4 MB each in `lib/LilyGoLib/examples/factory/partitions.csv`, so no
  partition change is needed).
- The app picks a GitHub release asset, checks its SHA-256, streams it, sets
  the boot partition and reboots. The firmware rolls back if the new image
  doesn't confirm itself within N boots.
- Measure GATT throughput first. If 4 MB is too slow, do the L2CAP CoC
  transport (`IOS_COMPANION_PLAN.md` §4.1) before OTA.

### S13 — Screen capture (`screen.capture`)

An RLE-compressed framebuffer → PNG on the phone, to attach to bug reports.
Reuse the encoding from the emulator's screenshot sweep so captures from the
pager and the emulator compare one to one.

### S14 — LoRa messenger

`lora.send` / `evt.lora.rx` relay text through the existing radio path
(`src/hal/radio/`). The phone gets a proper chat UI (threads, timestamps,
RSSI/SNR per message), and the pager acts as the radio.

### S15 — iPhone notifications on the pager (ANCS)

Shows iPhone notifications on the pager even when the companion app isn't
running. It's the biggest win for a pager, but it needs the GATT client role:
drop `CONFIG_BT_NIMBLE_ROLE_CENTRAL_DISABLED` and add an ANCS client module.
**Measure the flash/RAM cost first**, and check it against the boot-time and
battery work (`BOOT_TIME_PLAN_2026-10-24.md`, `OPTIMIZATION_*`).

## 7. iOS polish

### S16 — Widgets + Live Activity

Pager battery and connection status on a Lock Screen / Home Screen widget, fed
by `evt.battery`. Notes-sync progress as a Live Activity / Dynamic Island,
fed by `evt.notes.sync`.

### S17 — Siri Shortcuts / App Intents

"Send to pager," "Push weather," "Sync notes," "Find pager." Cheap once
S4–S7 exist, and they make the link scriptable from Shortcuts automations.

### S18 — Battery history chart

Log `evt.battery` (and `sys.info` on connect) on the phone and chart it over
days, to catch power regressions from firmware changes.

### S19 — Diagnostics export

Put the Diagnostics frame log, `sys.info`, the firmware hash from `sys.hello`
and the app version in one file, shared from the share sheet. Attach an S13
screenshot when it exists.

## 8. Testing rules (same as the existing work)

- Every new RPC: a golden vector in `protocol/`, then codec tests in Unity, Go
  and PagerKit (`make protocol-test`).
- End-to-end against the emulator in Docker, like `make -C ios notes-e2e` /
  `settings-e2e`: add `files-e2e` (S1) and `notify-e2e` (S4–S7).
- After any firmware change, rebuild both `tlora_pager` and
  `emulator_lora_pager`, one env at a time (`protocol-test` wipes other
  envs' build dirs).
- Power: no periodic timers in the link (`IOS_COMPANION_PLAN.md` R7).
  Measure fake-sleep current before and after S4 and S15.

## 9. Status (2026-10-29, after the implementation pass)

Committed as `19ab133`; the follow-ups below (§10) are not committed yet. Verified on this Linux box: `make protocol-test`
(vectors fresh, Unity, Go, PagerKit 311 tests), `pio test -e native_test`
(242 cases), `pagerlink smoke` (77 steps against the emulator),
`make -C ios settings-e2e`, `notes-e2e` and `files-e2e`, and builds of
`tlora_pager`, `twatchs3`, `twatch_ultra` and `emulator_lora_pager`. No
SwiftUI, extension or intent code has been compiled (no Xcode here), and no
hardware was attached.

| # | State | Where / notes |
|---|---|---|
| S1 | Done (app uncompiled) | `ios/App/Features/Files`, `PagerKit/Files/FileBrowser.swift`, `ios/FilesE2E` (passes) |
| S2 | Done (app uncompiled) | `ios/App/Features/Weather`, `PagerKit/Weather` |
| S3 | Done (app uncompiled) | Dashboard quick actions incl. Send text / Find pager |
| S4 | Done | `notify.post` (PROTOCOL.md §6.5), `src/hal/companion*.cpp`, pager Inbox app (`src/apps/ui_inbox.cpp`), share extension `ios/ShareExtension` (queues in the App Group outbox). Fake-sleep wake only on the T-LoRa-Pager; fake-sleep current not measured (no device) |
| S5 | Done | `clip.push` both ways; editor "to phone" pill |
| S6 | Done | `text.send` editor / focused field |
| S7 | Done | `phone.find` (Inbox app button) and `pager.find` (haptic + banner; there is no tone API, only WAV playback) |
| S8 | Done (app uncompiled) | `PagerKit/Voice`, recordings are `/mental_notes/*.wav` on SD |
| S9 | Done (app uncompiled) | `PagerKit/Journal`, Journal tab |
| S10 | Done, single host | `ssh.key` secret setting (pinned secret slot `ssh/key_enc`, `NC_PIN_N` 5), libssh pubkey auth before the password; phone SSH screen. Multi-host list: `ssh.hosts` setting (`name=user@host:port` entries separated by `;`, max 16, shared key/password; `src/hal/ssh_hosts.cpp`, `PagerKit/SSH/SSHHostList.swift`), host picker on the pager's Connect, editor on the phone. Key login not tested on hardware |
| S11 | Done (app uncompiled) | `PagerKit/Chat/ChatHistory.swift`; ChatService was never registered in the app before, now is |
| S12 | Done, untested on hardware | §6.7: upload with `fs.write`, `ota.apply` checks SHA-256 + 0xE9, writes the next slot; app-level rollback after 3 unconfirmed boots (no bootloader rollback). Emulator path verified by smoke. GATT throughput not measured |
| S13 | Done | §6.6 `screen.capture` → `screen.data` (`rgb565-rle`); `LV_USE_SNAPSHOT` on in both builds; 480x222 capture = 13.5 KB on the emulator |
| S14 | **Blocked** | There is no radio TX/RX path: `src/hal/radio_chip.h` only configures and parks the chip (P4.18). Needs a new driver path, an RX duty-cycle/battery decision and two radios to test |
| S15 | Measured only | Enabling the NimBLE central/GATT-client role: +11,216 B flash, +8 B static RAM (heap and the ANCS module itself not included). Decide against boot-time/battery work before building ANCS |
| S16 | Done (uncompiled) | `ios/Widgets`, App Group `group.$(BUNDLE_ID_PREFIX).pagercompanion.shared`, needs provisioning |
| S17 | Done (uncompiled) | `ios/App/Intents/PagerIntents.swift` |
| S18 | Done (app uncompiled) | `PagerKit/Battery/BatteryHistory.swift`, Swift Charts |
| S19 | Done (app uncompiled) | `PagerKit/Diagnostics/DiagnosticsReport.swift`, includes the S13 screenshot |

Found on the way: the emulator aborts on SIGTERM ("terminate called without
an active exception") at `dd840b4` already. Fixed in §10.

## 10. Follow-up pass (2026-10-29)

- **Emulator SIGTERM abort**: the PagerLink TCP listener was a joinable global
  `std::thread` (`src/hal/link/link_transport_tcp.cpp`); `exit()` destroyed it
  and called `std::terminate` (exit 134). It is detached now. Before: exit 134;
  after: exit 0, also after a full `pagerlink smoke`.
- **S10 multi-host**: done (see the S10 row). The value is one line because the
  settings registry refuses control characters in strings; the C++ and Swift
  parsers give the same `entry N: <reason>` errors (`test/test_ssh_hosts`,
  `SSHHostListTests`). `settings-e2e` writes, reads back and restarts with it.
  The pager picker is compiled but not driven in a UI test.
- **S14, S15**: still open, waiting on a decision (radio RX duty cycle / two
  radios for S14; flash/RAM vs boot-time and battery for S15).

Verified: `make protocol-test` (PagerKit 331 tests), `pio test -e native_test`
(260 cases), `make -C ios settings-e2e`, `pagerlink smoke`, builds of
`emulator_lora_pager`, `tlora_pager`, `twatchs3`, `twatch_ultra`.
