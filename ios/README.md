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
  Makefile                        gen / test / test-linux / notes-e2e / sim / device* / clean*
  .swift-format                   swift-format style config
  Config/
    Base.xcconfig                 shared build settings
    Local.xcconfig.example        template for per-developer signing
  Resources/Info.plist            app Info.plist (build-setting variables)
  App/
    PagerCompanionApp.swift       @main, injects PagerSession (+ its NotesModel) + TelegramAccount
    Session/PagerSession.swift    @MainActor ObservableObject link state
    Notes/NotesModel.swift        Notes tab state: list, unlock, move to GitHub
    Notes/KeychainSecretStore.swift  the GitHub PAT in the Keychain (this device only)
    Session/LocationProvider.swift  CoreLocation fix + city for weather "auto"
    Telegram/…                    TDLib JSON client + the phone's Telegram login
    Features/RootView.swift       Dashboard / Notes / Telegram / Diagnostics tabs
    Features/Dashboard/…          connection + sys.info
    Features/Notes/…              note list + detail, unlock, sync sheets, GitHub settings
    Features/Telegram/…           api keys, phone/code/2FA login, log out
    Features/Diagnostics/…        ping + session log
  PagerKit/                       Swift package (no UIKit/SwiftUI)
    Sources/PagerKit/…            Transport (BLE, TCP) / Link / RPC / Model /
                                  Services (sys, weather, tg + TDLib, chat, notes
                                  sync + move engine, fs) / Notes (indexer,
                                  cache, library, NoteCrypto, GitBlob, account)
    Tests/PagerKitTests/…         XCTest (reads ../../protocol/vectors)
  NotesE2E/                       notes viewer + move checks against the emulator
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
- **Notes tab (viewer + move to GitHub):** PagerKit parts tested on Linux
  (`make test-linux`, `make notes-e2e` against the emulator); the SwiftUI
  views, `NotesModel` wiring and Keychain store have not been built with Xcode
  or run in the Simulator / on hardware yet (`IOS_NOTES_VIEWER_PLAN.md` W8,
  W9, W15).
- **The app target builds on macOS** (`make gen && make sim`, Xcode 26, no
  warnings under strict concurrency). `TCPTransportTests` drives
  `TCPTransport` + `LinkSession` against a loopback-socket pager: connect
  readiness, refused connects and peer close.
