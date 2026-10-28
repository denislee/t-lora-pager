# Pager Companion (iOS)

The iPhone half of **PagerLink v1**, the framed RPC link between the
T‑LoRa‑Pager and the phone. It lives in this repository next to the firmware
(`src/`) and lilyhub (`server/`) so protocol changes touch firmware, app and the
golden vectors in one commit (see `IOS_COMPANION_PLAN.md` §3.2).

The app target is `PagerCompanion`. `PagerKit` is a plain Swift package that
holds the framing codec, the RPC session, the transports and the phone-side
services; it has no UIKit/SwiftUI imports and builds (and tests) without Xcode.

## Prerequisites

- **Xcode 16 or newer** (Swift 6 toolchain) — for the app target.
- **XcodeGen** (`brew install xcodegen`) — `project.yml` is the source of
  truth; the `.xcodeproj` is generated and not committed.
- **iOS 16+** for the app (kept at 16 so an iPhone 8 can run it; the session
  is an `ObservableObject`, not `@Observable`). The Simulator is
  enough for the emulator path; a device needs a signing team.
- **Docker** (optional) — to run the `PagerKit` tests on Linux.
- **Swift 6 toolchain** on Linux/CI for `make test-linux`.

## Layout

```
ios/
  README.md                       this file
  project.yml                     XcodeGen spec (source of truth)
  Makefile                        gen / test / test-linux / notes-e2e / settings-e2e / sim / device* / clean*
  .swift-format                   swift-format style config
  Config/
    Base.xcconfig                 shared build settings
    Local.xcconfig.example        template for per-developer signing
  Resources/Info.plist            app Info.plist (build-setting variables)
  App/
    PagerCompanionApp.swift       @main, injects PagerSession (+ its NotesModel, TasksModel, SettingsModel) + TelegramAccount
    Session/PagerSession.swift    @MainActor ObservableObject link state
    Notes/NotesModel.swift        Notes tab state: list, unlock, edit/save/delete, move to GitHub
    Tasks/TasksModel.swift        Tasks tab state: the pager's /tasks.txt, saved on every change
    Settings/SettingsModel.swift  Settings tab state: binds PagerKit's SettingsStore to the link
    Notes/KeychainSecretStore.swift  the GitHub PAT in the Keychain (this device only)
    Session/LocationProvider.swift  CoreLocation fix + city for weather "auto"
    Telegram/…                    TDLib JSON client + the phone's Telegram login
    Features/RootView.swift       Dashboard / Notes / Tasks / Telegram / Settings / Diagnostics tabs
    Features/Dashboard/…          connection + sys.info
    Features/Notes/…              note list + detail/editor, unlock, sync sheets, GitHub settings
    Features/Tasks/…              task list: check off, add, rename, reorder, delete
    Features/Telegram/…           api keys, phone/code/2FA login, log out
    Features/Settings/…           the pager's settings: generic form + Wi-Fi, time zone,
                                  weather location, Telegram favorites screens
    Features/Diagnostics/…        ping + session log
  PagerKit/                       Swift package (no UIKit/SwiftUI)
    Sources/PagerKit/…            Transport (BLE, TCP) / Link / RPC / Model /
                                  Services (sys, weather, tg + TDLib, chat, notes
                                  sync + move engine, fs) / Notes (indexer,
                                  cache, library, NoteCrypto, GitBlob, account) /
                                  Tasks (TasksFile format, TasksService) /
                                  settings (SettingsService, SettingsStore)
    Tests/PagerKitTests/…         XCTest (reads ../../protocol/vectors)
  NotesE2E/                       notes viewer + move checks against the emulator
  SettingsE2E/                    every pager setting: set, read back, kill -9, read back
```

## Generate and open

```sh
cd ios
make gen && open PagerCompanion.xcodeproj
```

`make gen` runs `xcodegen generate --spec project.yml`. Re-run it after editing
`project.yml` or adding/removing source files; the generated project is
git-ignored.

## Signing

The team id and bundle id prefix are **not** committed. Copy the template and
fill it in:

```sh
cd ios
cp Config/Local.xcconfig.example Config/Local.xcconfig
# edit Config/Local.xcconfig: DEVELOPMENT_TEAM, optional BUNDLE_ID_PREFIX
```

`Config/Base.xcconfig` pulls the local file in with
`#include? "Local.xcconfig"`; the `?` keeps the include optional, so a clean
checkout still generates and builds (unsigned, Simulator-only).
`Config/Local.xcconfig` is untracked.

## Tests

`PagerKit`'s XCTest suite runs against the shared golden vectors in
`../../protocol/vectors/*.json`.

- On macOS (Xcode toolchain):

  ```sh
  cd ios
  make test          # cd PagerKit && swift test
  ```

- On Linux (the same box that builds the firmware), via Docker:

  ```sh
  cd ios
  make test-linux    # docker run … swift:6.0-jammy swift test
  ```

  `test-linux` mounts the repo root, so it sees the vectors the same way the
  macOS run does. PagerKit depends on `apple/swift-crypto` 3.x (`Crypto` +
  `_CryptoExtras`) for notes decryption and git blob hashing, so the first run
  fetches and builds it.

- Against the running emulator (Linux, Docker):

  ```sh
  make -C ios notes-e2e   # ios/NotesE2E: listing order, both sync triggers, edit during verify
  ```

- Every pager setting end to end (Linux via Docker, or macOS). Starts the
  emulator itself on a fresh state root, so port 7600 must be free:

  ```sh
  make -C ios settings-e2e   # ios/SettingsE2E/run.sh: set + read back every writable
                             # setting, kill -9 the emulator, restart, read back again
  ```

## Simulator ↔ emulator

The iOS Simulator has no Bluetooth radio, so there the app talks to the
**emulator** over TCP. The emulator speaks the same PagerLink packets, length-prefixed
(`protocol/PROTOCOL.md` §5).

1. Build and run the emulator from the repo root:

   ```sh
   pio run -e emulator_lora_pager
   .pio/build/emulator_lora_pager/program
   ```

   It listens on `127.0.0.1:7600`. `PAGERLINK_PORT` overrides the port;
   `PAGERLINK_PORT=0` keeps the listener off.

2. Run the app in the Simulator (`make gen`, open the project, pick an
   iPhone Simulator, Run).

3. In the app's **Dashboard** tab, open the **Emulator** disclosure group,
   confirm host `127.0.0.1` / port `7600`, and tap **Connect to emulator**
   (just below the group). The state goes
   to *Connected*, `sys.hello` fills firmware/board, the clock is synced, and
   `sys.info` fills battery, storage and Wi‑Fi.

The CLI check against the same listener (from `tools/pagerlink/`):

```sh
cd tools/pagerlink
go run ./cmd/pagerlink smoke
```

`smoke` walks every Phase‑1 method and error path over the TCP transport.

## Notes

The **Notes** tab (shown when the pager advertises `"fs"`, or offline when a
cached list exists) lists every `/notes/*.txt` on internal flash and the SD
card in the pager Journal's order (internal wins a name clash, name
descending). Plain notes show their text; encrypted ones (`Salted__`) stay
locked until **Unlock notes** takes the notes passphrase, which is held in
memory only and dropped on **Lock** or when the app leaves the foreground.
Decryption happens on the phone (`NoteCrypto`, OpenSSL `enc` /
PBKDF2-SHA256 10000); plaintext never crosses the link. The list and the raw
bytes are cached under `Caches/notes` (ciphertext stays ciphertext) for
offline reading; **Clear cached notes** in the GitHub settings wipes it.

**Editing.** A note's **⋯** menu has **Edit** (the text becomes an editor;
**Save** writes it back with `fs.write`) and **Delete from pager** (also a
swipe on the list). A save first re-reads the note and refuses with "changed
on the pager" if its bytes are no longer the ones the edit started from, so a
pager-side edit is never overwritten silently. Like the pager's own save it
rewrites every copy of the name (internal and SD), and an encrypted note is
encrypted again on the phone under a fresh salt (so it needs the passphrase).
A delete removes every copy, conditionally (`fs.delete.if`) when the pager
supports it. Editing needs the link; offline notes are read-only.

## Tasks

The **Tasks** tab shows the pager's Tasks list (`/tasks.txt`, the format of
`src/apps/ui_tasks.cpp`: `[ ] ` / `[x] ` lines plus plain lines kept
verbatim). Tap to check off, swipe to delete, **Edit** to reorder, long-press
to rename, the bottom field to add, **Clear done** to drop finished tasks.
Every change is written at once to internal and, with a card in, SD, as the
pager does; a save made after the pager changed the file is refused and the
list reloads. Nothing is cached: offline, the last list read is read-only.

**Sync moves notes to GitHub.** Set the repo (`owner/name`), branch and a
fine-grained token with `contents:write` under the Notes tab's gear (the token
goes to the Keychain, this device only, and is never sent to the pager; a repo
configured on the pager before has to be entered here once). **Sync to
GitHub**, and the pager's own Notes Sync button while the phone is connected,
upload every note in one commit, read the tree of that commit back, and delete
from the pager exactly the notes GitHub shows with the git blob SHA of the
bytes pulled, using a conditional `fs.delete` (size + CRC, cap
`fs.delete.if`) so a note edited meanwhile stays. Nothing on GitHub is
overwritten: a name already there with other content goes up as
`<stem>~<sha7>.txt`. Encrypted notes reach GitHub as their ciphertext. A
pager without `fs.delete.if` gets uploads only.

## Settings

The **Settings** tab (shown when the pager's `sys.hello` caps include
`"settings"`) reads and writes every setting the pager's own Settings app has
(`IOS_SETTINGS_PLAN.md` §3, `protocol/PROTOCOL.md` §6.4). The pager owns the
catalog: `settings.schema` lists only what the connected board has, with its
ranges, and the tab renders it generically, one section per group in the
pager's tile order. Wi-Fi networks (scan, join, forget, internet test), time
zone, weather location (the phone looks the city up) and Telegram favorites
(picked from the phone's Telegram login when there is one) have their own
screens. Secrets (Telegram token, GitHub PAT, SSH password) are write-only:
the tab shows Set / Not set, and the pager refuses them unless its notes
encryption is on and unlocked. **Copy from this phone's GitHub settings**
writes the Notes tab's repo, branch and token to the pager's Notes Sync.

Every write carries `if_rev`; a change made on the pager meanwhile is picked
up (`evt.settings.changed`) and a stale write is retried once unless the
pager changed that same setting. Turning Bluetooth off asks first (it drops
this link), and Power off is confirmed on the pager's own screen. Offline, the
last values read this run stay visible, read-only. The session log shows
setting ids, never values.

## Telegram

The app can hold its own Telegram login (TDLib, via the `TDLibFramework`
package, a prebuilt ~300 MB xcframework fetched on first build), so the
pager's Telegram app works through the phone with no tg-bridge server. In the
Telegram tab enter an `api_id`/`api_hash` from my.telegram.org (API
development tools), then your phone number, the login code and any 2FA
password. While logged in, `TelegramService` answers `tg.*` from that session
(`PagerKit/…/TDTelegram.swift` maps TDLib to the pager's shapes); otherwise it
falls back to the bridge the pager sends, if any.

## Status

- **Phase 1, in progress.** The emulator/TCP path is wired end to end: the
  Dashboard connects, shows live battery/storage/Wi‑Fi, sets the clock and
  zone on connect, and Diagnostics pings and logs.
- **BLE is the main connect path.** `BLETransport` (a CoreBluetooth central)
  backs **Connect via Bluetooth** and the auto-connect toggle (retries with
  backoff). It shares the link iOS already holds for the pager as a keyboard,
  falls back to the last pager used and then a scan, and lets iOS pair on the
  encrypted rx/tx characteristics. State restoration is not wired yet, and
  hardware acceptance is still open (`HUB_REMOVAL_PLAN.md` W15). See
  `IOS_COMPANION_PLAN.md` §7.
- **Notes editing and Tasks tab:** PagerKit parts unit-tested
  (`NotesEditTests`: `fs.write`, re-encryption, conflict checks, tasks
  format); the app builds for the Simulator but the screens have not been
  driven against the emulator or hardware yet. The pager's Tasks app picks up
  a phone edit within a second while it is open (not mid-edit).
- **Notes tab (viewer + move to GitHub):** PagerKit parts tested on Linux
  (`make test-linux`, `make notes-e2e` against the emulator); the SwiftUI
  views, `NotesModel` wiring and Keychain store have not been built with Xcode
  or run in the Simulator / on hardware yet (`IOS_NOTES_VIEWER_PLAN.md` W8,
  W9, W15).
- **Settings tab:** PagerKit parts (`SettingsService`, `SettingsStore`)
  tested on Linux (`make test-linux`) and end to end against the emulator
  (`make settings-e2e`); `SettingsModel`, `SettingsView` and the custom
  screens were written on Linux and have not been built with Xcode or run in
  the Simulator yet (`IOS_SETTINGS_PLAN.md` W10, W11).
- **The app target builds on macOS** (`make gen && make sim`, Xcode 26, no
  warnings under strict concurrency). `TCPTransportTests` drives
  `TCPTransport` + `LinkSession` against a loopback-socket pager: connect
  readiness, refused connects and peer close.
