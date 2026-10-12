# Pager Companion (iOS)

The iPhone half of **PagerLink v1**, the framed RPC link between the
T‑LoRa‑Pager and the phone. It lives in this repository next to the firmware
(`src/`) and lilyhub (`server/`) so protocol changes touch firmware, app and the
golden vectors in one commit (see `IOS_COMPANION_PLAN.md` §3.2).

The app target is `PagerCompanion`. `PagerKit` is a plain Swift package that
holds the framing codec, the RPC session and the transports; it has no
UIKit/SwiftUI imports and builds (and tests) without Xcode.

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
  Makefile                        gen / test / test-linux / sim / clean
  .swift-format                   swift-format style config
  Config/
    Base.xcconfig                 shared build settings
    Local.xcconfig.example        template for per-developer signing
  Resources/Info.plist            app Info.plist (build-setting variables)
  App/
    PagerCompanionApp.swift       @main, injects PagerSession + TelegramAccount
    Session/PagerSession.swift    @Observable @MainActor link state
    Telegram/…                    TDLib JSON client + the phone's Telegram login
    Features/RootView.swift       Dashboard / Telegram / Diagnostics tabs
    Features/Dashboard/…          connection + sys.info
    Features/Telegram/…           api keys, phone/code/2FA login, log out
    Features/Diagnostics/…        ping + session log
  PagerKit/                       Swift package (no UIKit/SwiftUI)
    Sources/PagerKit/…            Transport / Link / RPC / Services / Model
    Tests/PagerKitTests/…         XCTest (reads ../../protocol/vectors)
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
  macOS run does.

## Simulator ↔ emulator

The iOS Simulator has no CoreBluetooth, so the app talks to the **emulator**
over TCP. The emulator speaks the same PagerLink packets, length-prefixed
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
   confirm host `127.0.0.1` / port `7600`, and tap **Connect**. The state goes
   to *Connected*, `sys.hello` fills firmware/board, the clock is synced, and
   `sys.info` fills battery, storage and Wi‑Fi.

The CLI check against the same listener (from `tools/pagerlink/`):

```sh
cd tools/pagerlink
go run ./cmd/pagerlink smoke
```

`smoke` walks every Phase‑1 method and error path over the TCP transport.

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
  Dashboard connects, shows live battery/storage/Wi‑Fi/hub, sets the clock and
  zone on connect, and Diagnostics pings and logs.
- **BLE transport pending Spikes S1/S2.** There is no `BLETransport` yet; the
  CoreBluetooth central, state restoration and passkey pairing land once S1
  (HID + a link service on one NimBLE server we own) and S2 (iOS letting the app
  use PagerLink while the system holds the pager as a keyboard) are confirmed.
  See `IOS_COMPANION_PLAN.md` §7.
- **The app target builds on macOS** (`make gen && make sim`, Xcode 26, no
  warnings under strict concurrency). `TCPTransportTests` drives
  `TCPTransport` + `LinkSession` against a loopback-socket pager: connect
  readiness, refused connects and peer close.
