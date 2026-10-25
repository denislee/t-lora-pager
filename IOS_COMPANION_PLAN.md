# iOS Companion App — Plan

> **Status:** in progress, 2026-10-11. §12 steps 1, 2, 5, the
> hardware-independent half of Phase 1 (`165067a`) and the Phase 2 file
> service on the emulator (Track C item 7, `cadd656`) have landed; Phase 0
> spikes S1–S5 are open and need a pager + iPhone. See §13 for the log.
> **Scope:** a native iOS app ("Pager Companion") that pairs with the
> T-LoRa-Pager over Bluetooth Low Energy, moves data both ways, and lets
> each device use the other's capabilities: browse/transfer pager files from
> the phone, have the pager pull weather/time/location from the phone, push
> notifications and text, and later sync notes, proxy chat, and update
> firmware.
>
> **Repository:** the iOS app's native codebase lives **in this repository**,
> under `ios/`, next to the firmware (`src/`) and lilyhub (`server/`). It is
> not a separate repo. §3.2 has the layout, build and repo hygiene.
>
> Firmware facts below were read from the tree at `4761042`. Items marked
> **(verify)** are assumptions that a Phase 0 spike has to confirm before
> work that depends on them starts.

---

## 1. Goals and non-goals

### Goals

1. **One link, many features.** One BLE GATT service plus one framed,
   bidirectional RPC protocol. Every feature (files, weather, notifications,
   …) is a set of messages on that link. No feature-specific GATT
   characteristics.
2. **Both sides can start a request.** The phone calls `fs.list` on the
   pager, and the pager calls `weather.get` on the phone. The protocol is
   symmetric.
3. **Works with no Wi-Fi.** When the pager has no Wi-Fi, the phone becomes
   its uplink for weather, time, location, chat, notes sync and Telegram.
   It replaces lilyhub, which is being removed (`HUB_REMOVAL_PLAN.md`).
4. **Coexists with the BLE HID keyboard / media remote.** The existing
   `Remote` app and keyboard forwarding (`src/apps/ui_media_remote.cpp`,
   `src/hal/wireless.cpp`) keep working on the same radio and the same
   iPhone.
5. **No regression in power.** The P4/P5/P6 power passes (fake-sleep
   residual drain, BLE keepalive parking, Wi-Fi supervisor) stay intact.
   The link costs nothing measurable when it is off and little when it is
   idle and connected.
6. **Developable without hardware.** The emulator exposes the same protocol
   over TCP, so the iOS app can be built against the emulator from the iOS
   Simulator (which has no CoreBluetooth).

### Non-goals (v1)

- Android client. The protocol is platform-neutral, so one can be added
  later.
- Reading other iOS apps' notifications from the companion app. iOS doesn't
  allow this. The only route is ANCS, which is a separate, optional
  firmware feature (Phase 6).
- ~~Replacing lilyhub~~. Superseded on 2026-10-11 by `HUB_REMOVAL_PLAN.md`:
  lilyhub is removed, and the phone is the only uplink besides the pager's
  own Wi-Fi.
- Mesh/LoRa protocol design. The LoRa bridge in Phase 6 only relays text to
  the existing radio path.

---

## 2. What exists today (firmware)

| Area | Where | Relevance |
|---|---|---|
| BLE stack | NimBLE-Arduino `^2.2.3`, `platformio.ini` `[env_arduino]` | Peripheral only: `-D CONFIG_BT_NIMBLE_ROLE_CENTRAL_DISABLED`, `-D CONFIG_BT_NIMBLE_ROLE_OBSERVER_DISABLED`. A GATT **server** is all v1 needs. |
| BLE HID keyboard | `ESP32-BLE-Keyboard-fork` (`BleKeyboard`), wrapped in `src/hal/wireless.cpp` (`hw_set_ble_kb_enable/disable`, keepalive task, iOS conn-param tuning) | **Owns the whole NimBLE stack today.** See risk R1. |
| BT toggle | `hw_set_bt_enable()` → `hw_set_ble_kb_enable()`; `user_setting.bt_enable`; used by `menu_app.cpp`, `settings_connectivity.cpp`, `ui_media_remote.cpp`, `hal/system.cpp::hw_connectivity_init` | "Bluetooth on" currently means "keyboard on". That meaning has to be split. |
| Storage | `src/hal/storage.h`: `hw_list_internal_entries` / `hw_list_sd_entries` (`HwDirEntry`), `hw_read_sd_stream` (chunked, releases SPI between chunks), `hw_read_internal_bytes_raw` / `hw_read_sd_bytes_raw`, `hw_save_preferred_bytes`, `hw_delete_path`, `hw_get_storage_info`, `hw_get_filesystem_dirty` | Covers almost everything the file service needs. Missing: chunked **write**, rename, mkdir (to add). |
| File browser | `src/apps/ui_file_browser.cpp` (internal/SD source, filter, delete, passphrase gate on protected paths) | Reference for path rules and the encrypted-note gate. |
| Weather | `src/apps/ui_weather.cpp`: fetch chain `fetch_with_hub_fallback` (hub → direct), Open-Meteo WMO codes, compact `ForecastCache` (8 hourly + 7 daily, about 190 B) in NVS `WEATHER_PREFS_NS`, keyed by city or `"auto"` | The phone can produce exactly this struct, so it plugs straight into the cache. |
| Time zone | `src/ui_time_sync.cpp`: `timezone_set_user_tz(iana)`, POSIX table | The phone sends epoch + IANA zone. |
| Hub | `src/hal/hub.h` (NVS ns `hub`, reachability cache) | Pattern to copy for the companion settings namespace. |
| Notes crypto | `src/hal/notes_crypto.*`, raw-byte readers | Encrypted notes can be shipped as ciphertext. Policy: see §8. |
| Partitions | `lib/LilyGoLib/examples/factory/partitions.csv`: `app0`/`app1` 4 MB OTA slots + `otadata` | BLE OTA (Phase 6) is possible with no partition change. |
| Budgets | `REFACTOR_NOTES.md`: RAM 23.5 %, flash 65.1 % (pager) | Plenty of room. NimBLE is already linked. |

Findings from reading the vendored `BleKeyboard.cpp`:

- `begin()` calls `BLEDevice::init(name)` and `BLEDevice::createServer()`,
  installs its own `ServerCallbacks`, and starts advertising.
- `end()` calls `BLEDevice::deinit(true)`, which tears down the **whole**
  stack.
- Its `onConnect` sets a file-global `connected = true` for **any** central.
  If the companion app connects, `hw_get_ble_kb_connected()` would report
  true, and the keepalive task would start sending HID reports to a link
  that isn't a keyboard.
- The library itself warns that repeated `end()`/`begin()` cycles leak
  memory.

---

## 3. Architecture

```
┌──────────────────────── iPhone ────────────────────────┐        ┌──────────────────── T-LoRa-Pager ────────────────────┐
│ SwiftUI app                                            │        │ apps/ui_companion.cpp  (pair, status, transfers)     │
│  Dashboard · Files · Weather · Inbox · Settings · Diag │        │ settings_connectivity (link toggle, trusted phones)  │
│        │                                               │        │        ▲ lv_async / OneShotJob (never from BLE task)  │
│ PagerKit (Swift package)                               │        │ hal/link/  link_rpc  (dispatcher, both directions)   │
│  Services: Sys, Files, Weather, Notify, Notes, OTA     │        │            handlers: sys · fs · weather · notify …   │
│  RPC client/server (request ids, timeouts, events)     │◀─────▶│            link_frame (SAR + message codec, pure C++)│
│  Framing codec (same golden vectors as firmware)       │  BLE   │            link_transport_ble.cpp  | _tcp.cpp (emu)  │
│  Transport: BLETransport | TCPTransport (simulator)    │  GATT  │ hal/ble_stack  (owns NimBLE: init, server, adv, sec) │
│  CoreBluetooth central + state restoration             │ (+L2CAP│ hal/ble_hid    (keyboard/media, on shared server)    │
└────────────────────────────────────────────────────────┘  later)└──────────────────────────────────────────────────────┘
```

### 3.1 Firmware layout (new and changed files)

Follows the `hal/` domain split and the `hal/radio/` sub-directory pattern.

```
src/hal/ble_stack.{h,cpp}        NEW  single owner of NimBLEDevice: init once, server,
                                      advertising data, security/IOCap, conn-param policy,
                                      per-connection callback fan-out (HID vs link)
src/hal/ble_hid.cpp              NEW  keyboard + media keys moved out of wireless.cpp,
                                      on the shared server (see R1 for the library decision)
src/hal/wireless.{h,cpp}         CHG  hw_set_bt_enable → ble_stack on/off; keyboard becomes a
                                      sub-feature; hw_get_ble_kb_connected = "HID-subscribed peer"
src/hal/link/link_frame.{h,cpp}  NEW  segmentation/reassembly + header codec — pure, no Arduino
src/hal/link/link_rpc.{h,cpp}    NEW  request table, id allocation, timeouts, event routing
src/hal/link/link_transport_ble.cpp NEW GATT service; #ifdef ARDUINO
src/hal/link/link_transport_tcp.cpp NEW emulator TCP listener; #ifndef ARDUINO
src/hal/link/handlers_sys.cpp    NEW  hello, info, ping, time
src/hal/link/handlers_fs.cpp     NEW  list/stat/read/write/delete/mkdir/rename
src/hal/link/handlers_weather.cpp NEW weather.push in; weather.get out
src/hal/link/handlers_notify.cpp NEW  notify.post, clip.*, phone.find
src/hal/link.h                   NEW  public HAL API: hw_link_enable(), hw_link_connected(),
                                      hw_link_peer_name(), hw_link_request_weather(cb), …
src/hal/weather_cache.{h,cpp}    NEW  ForecastCache/GeoData load/save extracted from
                                      apps/ui_weather.cpp so the link handler can write it
src/hal/storage.{h,cpp}          CHG  chunked write (open/append/commit with temp file + rename),
                                      hw_rename_path, hw_mkdir_path
src/apps/ui_companion.cpp        NEW  core::App "Phone": pairing code, peer, transfer progress,
                                      trusted-device list, "find my phone"
src/apps/settings_connectivity.cpp CHG Companion toggle row + "forget phones"
src/hal/board_config.h           CHG  USING_COMPANION_LINK (all three boards; it's board-agnostic)
test/test_link_frame/            NEW  Unity: SAR, header codec, golden vectors
test/test_link_rpc/              NEW  Unity: id allocation, timeouts, dup/late responses
```

The "State reset on onStop" block convention applies to `ui_companion.cpp`.
UI code calls only `hal/link.h`, never NimBLE.

### 3.2 iOS codebase — in this repository (`ios/`)

The iOS app is developed **in this repository**, not a separate repo. The
pager firmware, lilyhub and the iOS companion form one product that talks
over one protocol, so they share a single history.

Why one repo:

- **Atomic protocol changes.** A new message or a changed field touches the
  firmware handler, the Swift codec and the golden vectors in **one
  commit**. Neither side can ship against a protocol the other doesn't
  speak.
- **Shared test vectors.** `protocol/vectors/*.json` is read by both the
  Unity tests (`test/`) and the XCTest suite (`ios/PagerKit/Tests`), so it
  has to live in one tree.
- **Shared tooling.** The emulator (`pio run -e emulator_lora_pager`), the
  Go integration CLI (`tools/pagerlink/cmd/pagerlink`) and the iOS Simulator app all
  run from one checkout. The graft/graphify indexes cover firmware and app
  together, so "who sends `weather.get`" resolves across both.
- **One versioning story.** `sys.hello` reports the firmware git hash, and
  the app embeds its own build's hash. Both come from the same history, so
  "which app goes with which firmware" is answered by `git log`.

Top-level layout after the change (new entries marked `+`):

```
t-lora-pager/
├── src/                 firmware (PlatformIO, Arduino + LVGL)
├── test/                native Unity tests (+ test_link_frame, test_link_rpc)
├── tools/pagerlink/     Go PagerLink codec + integration CLI (own module)
├── + protocol/          PagerLink spec + golden vectors — shared by firmware and iOS
├── + ios/               native iOS companion app (Swift / SwiftUI)
├── boards/ variants/ lib/ support/ tools/
├── platformio.ini  Makefile  CLAUDE.md  REFACTOR_NOTES.md
└── IOS_COMPANION_PLAN.md
```

Inside `ios/`:

```
ios/
  README.md                        prerequisites, build/run, simulator ↔ emulator setup
  project.yml                      XcodeGen spec — the source of truth for the Xcode project
  Makefile                         `make gen` (xcodegen), `make test`, `make sim`
  .swift-format                    formatting config
  PagerKit/                        Swift package — no UIKit/SwiftUI imports
    Sources/PagerKit/
      Transport/ BLETransport.swift  TCPTransport.swift  Transport.swift (protocol)
      Link/      Frame.swift  Segmenter.swift  Reassembler.swift
      RPC/       RPCClient.swift (async/await)  RPCServer.swift (pager-initiated requests)
      Services/  SysService  FileService  WeatherService  NotifyService  NotesService  OTAService
      Model/     Codable message types
    Tests/PagerKitTests/           XCTest; consumes ../../protocol/vectors/*.json
  App/
    PagerCompanionApp.swift        @main, scene, background tasks registration
    Features/ Onboarding/ Dashboard/ Files/ Weather/ Inbox/ Settings/ Diagnostics/
    Providers/ WeatherProvider (Open-Meteo, optional WeatherKit)  LocationProvider (CoreLocation)
  Resources/ Info.plist entitlements
protocol/                          (repo root, not inside ios/ — shared by both sides)
  PROTOCOL.md                      normative spec (generated from §4 once stable)
  vectors/*.json                   golden frames used by Unity tests and XCTest
```

Repo hygiene for the iOS tree:

- **The generated Xcode project isn't committed.** `ios/project.yml` is the
  source, and `make -C ios gen` regenerates
  `ios/PagerCompanion.xcodeproj`, which avoids `.pbxproj` merge conflicts.
  `ios/PagerKit/` is a plain Swift package and builds with
  `swift build` / `swift test` without Xcode-project generation.
- **`.gitignore` additions** (in the existing root `.gitignore`, alongside
  the `/server/bin/` entry):
  ```
  # iOS companion — generated project, build products, per-user state.
  /ios/*.xcodeproj/
  /ios/**/xcuserdata/
  /ios/**/.build/
  /ios/**/DerivedData/
  /ios/**/*.xcresult
  ```
  The root `.gitignore` already ignores `build` everywhere, which also
  covers `ios/build/`.
- **Signing:** the development team ID and bundle id prefix come from an
  untracked `ios/Config/Local.xcconfig` (template committed as
  `Local.xcconfig.example`). No signing identities or provisioning
  profiles go in git.
- **Firmware builds are unaffected:** `platformio.ini` sets `src_dir = src`
  and the native tests use `test/`, so PlatformIO never looks at `ios/` or
  `protocol/`. The firmware needs nothing from `ios/` to build. Only
  `protocol/vectors/` is read, and only by `pio test -e native_test`.
- **Root `Makefile`:** add `ios-gen`, `ios-test` and `ios-sim` targets that
  delegate to `ios/Makefile`, plus `protocol-test`, which runs both the
  Unity and XCTest vector suites. The firmware targets stay as they are.
- **Toolchain:** Xcode 16+ on macOS for the app. `ios/PagerKit` tests also
  run on Linux with the Swift toolchain (no CoreBluetooth there; the BLE
  transport is `#if canImport(CoreBluetooth)`), so the codec and RPC tests
  can run on the same Linux box that builds the firmware.
- **Docs:** when Phase 1 lands, add `ios/` and `protocol/` to the
  "Layered layout" tree and the Build/Run section of `CLAUDE.md`, and add a
  short pointer in `README.md`.
- **Commits:** changes that cross the protocol touch firmware, iOS and
  vectors in the same commit. iOS-only commits use an `ios` scope, as in
  `feat(ios): …`, to match the existing `perf(power): …` style.

- **Target:** iOS 17+ (Observation `@Observable`, Swift concurrency, SwiftUI
  `NavigationStack`). Swift 6 language mode with strict concurrency.
  `PagerKit` puts all CoreBluetooth access behind an `actor`.
- **Background:** `UIBackgroundModes = bluetooth-central`. CoreBluetooth
  state restoration (`CBCentralManagerOptionRestoreIdentifierKey`) so iOS
  relaunches the app on pager events. `BGAppRefreshTask` refreshes the
  weather snapshot so it's ready when the pager asks.
- **Discovery:** the app scans for the link service UUID, which must be in
  the advertising packet or scan response so background scanning works. If
  iOS already holds the pager connected as a keyboard, the app gets it from
  `retrieveConnectedPeripherals(withServices: [linkUUID])` instead of
  scanning (**verify**, R2).

---

## 4. Protocol — "PagerLink v1"

### 4.1 GATT surface

| Item | UUID | Properties | Notes |
|---|---|---|---|
| Service `PagerLink` | 128-bit custom (generate once, freeze) | primary | Advertised (scan response) so iOS can scan for it in the background |
| `rx` (phone → pager) | custom | Write, Write Without Response; **encrypted + authenticated** | All link packets from the phone |
| `tx` (pager → phone) | custom | Notify; **encrypted + authenticated** | All link packets from the pager |
| `info` | custom | Read (unencrypted) | `{proto:1, fw:"<hash>", board:"pager", l2cap_psm:0}`. Lets the app show "update needed" before pairing |

The keyboard keeps its standard HID service (0x1812). iOS hides 0x1812 from
apps, so there's no clash.

**L2CAP CoC (Phase 6 or earlier if throughput needs it):** the vendored
NimBLE-Arduino ships `NimBLEL2CAPServer` (needs
`-D CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM=1`). iOS supports
`CBPeripheral.openL2CAPChannel(psm)`. The PSM is published in `info`. The
same frame stream runs over the channel, so only the transport changes.

### 4.2 Link layer (segmentation)

One ATT write or notification carries one **packet**:

```
byte 0     : flags  bit7 START, bit6 END, bits5..0 rolling seq (mod 64)
byte 1..N  : message bytes
```

- Max packet = negotiated `ATT_MTU − 3`. The firmware requests MTU 247 and
  iOS typically answers 185–247 (**verify**). Data Length Extension is
  used if the controller negotiates it.
- The receiver reassembles START…END into one message. A seq gap or a
  START in the middle of a message drops the partial message and raises a
  `link.reset` event. BLE's link layer is reliable, so gaps only come from
  app-level buffer overflow (bugs), and both sides log them.
- Over TCP (emulator) the same packets are length-prefixed with a `u16`.

### 4.3 Message layer

```
u8   ver     = 1
u8   type    0 REQ · 1 RES · 2 ERR · 3 EVT · 4 DATA · 5 ACK
u16  id      request id (REQ/RES/ERR), transfer id (DATA/ACK), 0 for EVT
u16  len     payload length (≤ 4096 for JSON; DATA payload ≤ 1 packet budget × K)
...  payload REQ/RES/ERR/EVT: UTF-8 JSON (cJSON on pager, Codable on iOS)
             DATA: u32 offset + raw bytes
             ACK:  u32 next_expected_offset + u16 credits
```

- **Ids:** each side allocates its own REQ ids. The phone uses even ids
  and the pager odd, so they never collide.
- **Timeouts:** 10 s default per request, longer for named operations
  (`fs.write.commit`, `ota.*`).
- **Errors:** `ERR` payload `{code, msg}`. Codes map one-to-one onto
  `hal::HalError` (`hal/result.h`), so firmware handlers return
  `HalResult<T>` and the dispatcher serializes it.
- **Why JSON for control:** cJSON is already linked and used. Payloads are
  small. It can be debugged from a log. Bulk bytes never go through JSON.

### 4.4 Bulk transfer and flow control

- A transfer is started by an RPC (`fs.read` / `fs.write.begin`), which
  returns a transfer id, total size and CRC32 (read) or accepts them
  (write).
- **Credit-based window:** the receiver grants credits in `ACK`, initially
  16 packets. The sender never has more than `credits` DATA packets
  outstanding.
  - Pager → phone (`notify`): if NimBLE runs out of mbufs, `notify()`
    fails; the sender backs off 5–20 ms and retries the same packet.
  - Phone → pager (write-without-response): iOS gates on
    `canSendWriteWithoutResponse` / `peripheralIsReady(toSendWriteWithoutResponse:)`.
    The pager only grants new credits after the bytes are flushed to
    FFat/SD, because SD writes are the slow path.
- **Integrity:** CRC32 over the whole file is checked on completion. Writes
  go to `<path>.part` and are renamed on a matching commit, so a dropped
  link never leaves a half-written note.
- **Resume:** `fs.read {offset}` / `fs.write.begin {resume:true}` restart
  from the last acked offset after a reconnect.
- **Expected throughput:** plan for **20–40 KB/s** over GATT with a 15–30 ms
  connection interval, less while Wi-Fi is active (radio coexistence).
  Measure in Spike S3. L2CAP is the lever if this isn't enough (target
  > 80 KB/s).

### 4.5 Message catalog (v1 + reserved)

Direction: **P→D** = phone calls device, **D→P** = device calls phone.

| Method / event | Dir | Phase | Payload → result |
|---|---|---|---|
| `sys.hello` | P→D | 1 | `{proto, app, caps[]}` → `{proto, fw, board, name, caps[], mtu}`. Version gate: major mismatch → app shows "update firmware/app" |
| `sys.info` | P→D | 1 | → `{batt_pct, charging, vbus, uptime_s, wifi:{on,ssid,rssi}, storage:{int:{t,u}, sd:{t,u,mounted}}}` |
| `sys.ping` | both | 1 | `{t}` → `{t}` (RTT shown in Diagnostics) |
| `sys.time.set` | P→D | 1 | `{epoch, tz:"America/Sao_Paulo"}` → sets RTC + `timezone_set_user_tz`. Auto-sent on every connect |
| `evt.battery` | D→P | 1 | `{pct, charging}` on change ≥ 5 % or charge-state flip |
| `fs.list` | P→D | 2 | `{vol:"int"\|"sd", path, cursor?, limit≤64}` → `{entries:[{name,dir,size,mtime}], cursor?}` (paged; folders first, same sort as the file browser) |
| `fs.stat` | P→D | 2 | `{vol, path}` → `{dir,size,mtime,protected}` |
| `fs.read` | P→D | 2 | `{vol, path, offset?}` → `{xfer, size, crc32}` then DATA… |
| `fs.write.begin` / `.commit` / `.abort` | P→D | 2 | `{vol, path, size, crc32, overwrite?, resume?}` → `{xfer, offset}`; DATA…; `commit` → renames `.part` |
| `fs.delete` / `fs.mkdir` / `fs.rename` | P→D | 2 | Paths validated with the same rules as `notes_path` (no `..`, no outside-root) |
| `evt.fs.changed` | D→P | 2 | `{vol, path?}` driven by `hw_get_filesystem_dirty()`; the app invalidates its cache |
| `weather.get` | D→P | 3 | `{key:"auto"\|city}` → `ForecastPayload` (see §6.3) |
| `weather.push` | P→D | 3 | `ForecastPayload` → saved to the weather cache; the Weather app re-renders if open |
| `loc.get` | D→P | 3 | → `{lat, lon, acc_m, city?}` (also feeds the "auto" weather key) |
| `notify.post` | P→D | 4 | `{id, title, body, src, ts, prio}` → shown via the shared popup/toast + stored in a small inbox ring |
| `clip.push` | both | 4 | `{text}`. Pager→phone puts it on the iOS pasteboard (foreground) or a local notification (background) |
| `phone.find` | D→P | 4 | → the phone plays a sound / posts a time-sensitive local notification |
| `text.send` | P→D | 4 | `{text, target:"editor"\|"kb"}` types into the pager editor (phone as keyboard) |
| `notes.index` / `notes.pull` / `notes.push` | P→D | 5 | Notes-aware sync on top of `fs.*` (mtime-based, conflict = keep both) |
| `chat.ask` | D→P | 6 | Phone runs the pager's chat request against Groq (LLM + Whisper), replacing lilyhub's `/api/chat` (`HUB_REMOVAL_PLAN.md` H2.4) |
| `lora.send` / `evt.lora.rx` | both | 6 | Relay text to and from the existing radio path |
| `ota.begin` / DATA / `ota.commit` | P→D | 6 | Writes the inactive `app0`/`app1` slot, verifies, sets boot partition |
| `screen.capture` | P→D | 6 | RLE-compressed framebuffer for bug reports / docs |

Unknown methods return `ERR {code:"unsupported"}`. `caps[]` in `sys.hello`
tells each side what the other supports, so the app hides features the
firmware doesn't have.

---

## 5. Firmware design details

### 5.1 BLE stack ownership (blocker for everything else)

1. `ble_stack` calls `NimBLEDevice::init()` once when Bluetooth is turned
   on. It is never `deinit`ed and re-`init`ed in a loop. "BT off" stops
   advertising and disconnects peers. A full deinit only happens on an
   explicit "radio off" (and stays rare, because of the leak).
2. One `NimBLEServerCallbacks` owned by `ble_stack`. It fans out to the HID
   and link modules per `connHandle`. A connection counts as a **keyboard**
   when the peer subscribes to the HID input report CCCD, and as a
   **companion** when it subscribes to `tx`. One iPhone can be both on the
   same connection.
3. `hw_get_ble_kb_connected()` becomes "a peer is subscribed to HID input",
   not "any connection", so the keepalive task stops treating companion
   connections as keyboards.
4. **Advertising:** name `lilygo-pager` (keep it, because existing bonds
   use it), appearance = keyboard, HID UUID in the advertising data,
   PagerLink UUID in the scan response. Fast interval (30–60 ms) for 30 s
   after enabling or waking, then slow (1–2 s). Advertising stops when the
   connection limit is reached (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS`,
   currently default, **verify**; 1 iPhone = 1 connection).
5. **Security:** LE Secure Connections + bonding, as today
   (`setSecurityAuth(true, …, true)`). Link characteristics require
   encryption + authentication. IO capability moves from `NO_INPUT_OUTPUT`
   (Just Works) to `DISPLAY_ONLY`: the pager shows a 6-digit passkey in
   `ui_companion` and the user types it on the iPhone. This is a
   device-global setting, so the keyboard will also use passkey pairing,
   which is acceptable and safer. Existing bonds stay valid. Decision D4.
6. **Trusted phones:** NVS namespace `companion`, holding a list of bonded
   peer identity addresses + a user-given name. "Forget" removes the bond
   (`NimBLEDevice::deleteBond`) and the entry.
7. **Connection-parameter policy** in one place, extending
   `ble_kb_apply_ios_conn_params`:
   - idle: 30–50 ms, latency 10, timeout 6.72 s (today's Apple-compliant
     band);
   - transfer active: 15–30 ms, latency 0;
   - back to idle 2 s after the last DATA packet.

### 5.2 Threading

- NimBLE host-task callbacks only copy packets into a FreeRTOS
  `StreamBuffer` (rx) and return. They never take the SPI lock or touch
  LVGL.
- `link_task` (low priority, 6 KB stack, PSRAM message buffers) does
  reassembly, dispatch and handler execution. File handlers take
  `core::ScopedSpiLock` per chunk (via `hw_read_sd_stream` for reads) so
  the UI never stalls behind a transfer.
- UI notifications go through the existing async-job path
  (`core::OneShotJob` / LVGL async call under the UI lock), the same way
  `ui_weather.cpp` hands fetch results back.
- Outgoing requests (`weather.get`) are `link_rpc` calls with a completion
  callback. The callback runs on `link_task`, and the app posts it to the
  UI.

### 5.3 Power and fake sleep

- Link off: zero cost. It's not advertised, and no task runs (the task is
  created on enable).
- Link idle-connected: `link_task` blocks on the stream buffer, with no
  periodic wake. The keepalive stays HID-only. Measure idle current with
  the phone connected vs. HID-only (bench row in §9).
- **Fake sleep:** the link stays connected (the radio isn't on the cut
  keyboard rail). Small RPCs (`notify.post`, `weather.push`, `sys.time.set`)
  are served. Bulk transfers hold a "busy" vote that keeps the CPU at full
  speed (the mechanism from `4761042`, P6.11) and defers fake-sleep entry
  while a transfer is active. Push the vote through the same hook rather
  than adding a new one.
- `ui_resume_timers()` already wakes the HID keepalive. Add the
  link-advertising "fast burst after wake" to the same hook.

### 5.4 Emulator

- `link_transport_tcp.cpp` listens on `127.0.0.1:7600` (env
  `PAGERLINK_PORT`) when the emulator starts. Same packets, `u16`
  length-prefixed.
- Storage handlers work unchanged against the emulator's filesystem
  backing. The weather handler writes the emulator's NVS shim.
- Add a `UI_SHOTS` state for `ui_companion` (pairing code, connected,
  transfer in progress) so the screenshot sweep covers it in every style
  and dark mode.

### 5.5 Weather integration (the motivating example)

Current chain in `ui_weather.cpp`: direct internet (the hub step was removed
in `HUB_REMOVAL_PLAN.md` H1.3). New chain (D-H6, direct first):

```
direct (if Wi-Fi)  →  phone (if link connected)  →  cached (stale badge)
```

- `weather_cache.{h,cpp}` takes `ForecastCache`, `GeoData`, `load_/save_*`
  and the cache key logic out of `ui_weather.cpp` unchanged, so behavior
  is identical.
- The phone fetches **Open-Meteo** with the same fields the firmware asks
  for. The WMO codes and units match, so no code mapping is needed and
  `wmo_to_icon` works as-is.
- `ForecastPayload` (JSON) mirrors `ForecastCache`:
  `{key, ts, hourly:[{hr,code,temp,pop}×8], daily:[{date,code,hi,lo,pop}×7], city, lat, lon}`.
  The firmware validates ranges (`temp` fits `int8_t`, counts ≤ 8/7),
  packs it and calls `save_forecast_cache`.
- The phone also pushes `weather.push` proactively on connect and every
  `BGAppRefreshTask` cycle, so opening Weather on the pager is usually
  instant, even with no Wi-Fi at all.
- With `key == "auto"`, the phone uses its own CoreLocation fix instead of
  ip-api geolocation, which is more accurate than IP lookup.

---

## 6. iOS app design

### 6.1 Screens

| Screen | Content |
|---|---|
| Onboarding | Explain Bluetooth use → scan → pick "lilygo-pager" → system passkey prompt (code shown on pager) → name the device |
| Dashboard | Connection state, RTT, battery, storage bars (internal / SD), Wi-Fi status, last weather push, quick actions (Find pager, Send text, Sync time) |
| Files | Volume switch (Internal / SD), breadcrumb navigation, list with size/mtime, pull-to-refresh, swipe delete/rename, new folder; tap → download with progress → Quick Look / text preview; share sheet; import from Files app / Photos (upload); protected (encrypted) files shown with a lock badge |
| Weather | What the phone will serve to the pager: location source (GPS / city), last fetch, "push now", a preview rendered like the pager does |
| Inbox | Texts/clipboard the pager sent; compose a message to the pager (`notify.post` / `text.send`) |
| Settings | Auto-push weather, auto time sync, background refresh, units, trusted device rename/forget, transport (BLE / TCP for emulator, debug builds only) |
| Diagnostics | Live frame log (hex + decoded JSON), MTU, conn interval, throughput test (`fs.read` of a generated file), export logs |

### 6.2 Data and state

- `PagerSession` (`@Observable`, `@MainActor`) is what the UI sees: state,
  device info, active transfers.
- `LinkActor` (actor) owns `CBCentralManager`/`CBPeripheral`, segmentation
  and the RPC tables. It exposes `AsyncStream` for events and `async`
  methods for calls.
- `RPCServer` registers handlers for pager-initiated methods
  (`weather.get`, `loc.get`, `clip.push`, `phone.find`, `chat.ask`).
- Downloads are cached in the app's Caches directory, keyed by
  `vol:path:mtime:size`, and invalidated by `evt.fs.changed`.

### 6.3 External services and entitlements

| Need | Choice | Notes |
|---|---|---|
| Weather | **Open-Meteo** (default) | Free, no key, same WMO codes as firmware. WeatherKit optional later (needs a paid developer account + condition→WMO mapping) |
| Location | CoreLocation, "When In Use" + significant-change for background | Only used for weather "auto" and `loc.get` |
| Background | `bluetooth-central`, `BGTaskScheduler` (`fetch`) | State restoration identifier `pager.link.central` |
| Notifications | `UNUserNotificationCenter` (local only) | `phone.find`, background `clip.push` |
| Signing | Apple Developer account | A free personal team works for on-device dev (7-day profiles); TestFlight needs the paid program |

---

## 7. Phased delivery

Each phase ends with both the hardware and emulator builds green (house
rule from `REFACTOR_NOTES.md`), `pio test -e native_test` green, and the
iOS package tests green.

### Phase 0 — Spikes (de-risk; ~3–5 days)

| # | Question | Exit criterion |
|---|---|---|
| S1 | Can HID + a custom GATT service live on one NimBLE server we own? Port keyboard + media keys to `NimBLEHIDDevice` (ships with NimBLE-Arduino) on a `ble_stack`-owned server vs. patching the fork to accept an external server | Keyboard typing + media keys work on iPhone after the port; decision recorded (D1) |
| S2 | Does iOS let the app use PagerLink while the system holds the same peripheral as a keyboard? | Minimal Swift test app finds it via `retrieveConnectedPeripherals`, subscribes to `tx`, round-trips a ping while typing still works |
| S3 | Real throughput: MTU iOS grants, DLE, notify/write-w/o-resp KB/s at 15 ms, with Wi-Fi off/on | Numbers in this doc; go/no-go for L2CAP in v1 |
| S4 | Memory: NimBLE mbuf/heap headroom with HID + link + Wi-Fi + TLS fetch concurrently | No heap failures in a 30 min soak; free-heap low-water mark recorded |
| S5 | Passkey pairing (`DISPLAY_ONLY`) with an already-bonded iPhone | Re-pair flow documented; old Just-Works bond either survives or the migration step is known |

### Phase 1 — Link foundation (~1.5 weeks)

Firmware: `ble_stack`, `ble_hid` (from S1), `link_frame`, `link_rpc`, BLE +
TCP transports, `handlers_sys`, `ui_companion` (pairing code, status),
connectivity toggle, trusted-phone list, `evt.battery`, auto
`sys.time.set` on connect.
Repo: create `ios/` and `protocol/` in this repository (§3.2 layout),
`.gitignore` entries, `ios/Makefile` + root `ios-*` targets, `ios/README.md`,
and the `CLAUDE.md` layout update.
iOS: Xcode project via XcodeGen, `PagerKit` transport/framing/RPC,
onboarding + dashboard + diagnostics, state restoration.
**Done when:** the phone pairs with the passkey, shows live battery/storage,
sets time + zone on connect, reconnects automatically after the pager
reboots or the phone app is killed, and the keyboard/media remote still
works. The emulator + iOS Simulator path works end to end.

### Phase 2 — File browser and transfer (~1.5 weeks)

Firmware: `handlers_fs`, chunked write + `.part` commit, rename/mkdir in
`hal/storage`, `evt.fs.changed`, busy vote during transfers.
iOS: Files screen, download/upload with progress, Quick Look, share sheet,
document picker import, resume after disconnect.
**Done when:** a 1 MB SD file downloads with matching CRC32, upload of a
`.txt` lands and opens in the pager editor, pulling the battery mid-upload
leaves no partial file, and paths with `..` are rejected (unit test +
negative control).

### Phase 3 — Weather, location and time via phone (~1 week)

Firmware: `weather_cache` extraction (behavior-neutral commit first),
`handlers_weather`, phone step in the fetch chain, "via phone" source
label in the Weather app.
iOS: Open-Meteo provider, CoreLocation, `weather.get`/`loc.get` handlers,
proactive push on connect + background refresh.
**Done when:** with Wi-Fi off on the pager, opening Weather shows a fresh
forecast fetched by the phone, rendered the same way as the direct path
(screenshot diff), and the "auto" location uses the phone's GPS.

### Phase 4 — Messages, clipboard, find-my (~1 week)

`notify.post` (pager toast + small inbox ring persisted in FFat),
`clip.push` both ways, `phone.find`, `text.send` into the editor.
**Done when:** a message typed on the phone appears on the pager within
1 s, including during fake sleep (it wakes the display per notification
policy), and "Find phone" from the pager rings a locked iPhone.

### Phase 5 — Notes / tasks / journal sync (~1.5 weeks)

Phone becomes the notes sync target, replacing lilyhub's `/api/notes/sync`
(`HUB_REMOVAL_PLAN.md` H2.5, `notes.sync`): mtime
index, pull/push, conflict = keep both (`name (phone).txt`). Encrypted
notes travel as ciphertext (§8). iOS shows decrypted text only if the user
enters the passphrase (requires a Swift port of `notes_crypto`, decision D5).
**Done when:** editing a note on either side syncs on the next connect, the
same note survives a round trip byte-for-byte, and an encrypted note never
leaves the pager in plaintext.

### Phase 6 — Advanced (pick by value)

- **BLE OTA:** stream a firmware `.bin` from the phone into the inactive OTA
  slot, verify the SHA-256 the app sends, set the boot partition, reboot,
  and roll back automatically if the new image doesn't confirm itself
  within N boots.
- **L2CAP CoC transport** if S3 shows GATT is too slow for OTA or large
  files.
- **Chat proxy:** the pager chat app always uses `chat.ask`; the phone is its
  only path once lilyhub is gone (`HUB_REMOVAL_PLAN.md` H2.4).
- **LoRa bridge:** compose and read LoRa messages on the phone.
- **ANCS** (iPhone notifications on the pager with no app running): needs
  GATT-client role on the peripheral connection. That means dropping
  `CONFIG_BT_NIMBLE_ROLE_CENTRAL_DISABLED` (flash/RAM cost to measure)
  and an ANCS client module.
- **Screen capture** for bug reports.
- T-Watch-S3 / Ultra enablement. The link is board-agnostic; only the UI
  layout of `ui_companion` and bench power numbers need per-board work.

---

## 8. Security and privacy

- Link characteristics require an encrypted, authenticated (MITM-protected)
  bond. An unbonded phone can only read `info`.
- Per-session authorization: the first `fs.*` call from a phone not on the
  trusted list triggers a pager-side confirm prompt (the shared
  `ui_popup_create` confirm). Trusted phones skip it.
- Path sandbox: handlers only accept paths under the volume root, normalized
  with the `notes_path` rules (already unit-tested in `test/test_notes_path`).
- **Encrypted notes:** by default the file service returns raw ciphertext
  (`hw_read_internal_bytes_raw` / `hw_read_sd_bytes_raw`) and marks
  `protected:true`. It never decrypts on behalf of the phone. An option to
  serve plaintext while the pager is unlocked is decision D5.
- The phone stores no pager files outside its sandbox caches. "Forget
  device" clears caches and the bond.
- Location is only sent to the pager on request (`loc.get`) or inside a
  weather payload, never logged on the pager.

---

## 9. Testing strategy

| Layer | How |
|---|---|
| Frame / SAR codec | Unity `test/test_link_frame` + XCTest on the **same** `protocol/vectors/*.json` (header encode/decode, segmentation at MTU 23/185/247, seq gap → reset, oversized length → reject) |
| RPC | Unity `test/test_link_rpc`: id parity, timeout, late response dropped, duplicate response, unknown method → `unsupported` |
| Handlers | Emulator integration via a small Go CLI (`tools/pagerlink/cmd/pagerlink`, its own Go module) that speaks the TCP transport: list/read/write/delete, CRC, `.part` cleanup, path traversal negative controls |
| iOS UI | XCTest + SwiftUI previews against a `MockTransport`; UI tests against the emulator over TCP from the Simulator |
| Hardware bench | Matrix below, results appended to this doc |
| Screenshots | `UI_SHOTS_DIR=… xvfb-run -a .pio/build/emulator_lora_pager/program` covers `ui_companion` states |

Bench matrix (pager + real iPhone):

| Row | Measure |
|---|---|
| B1 | Idle current: BT off / HID only / HID + link connected / link connected in fake sleep |
| B2 | Throughput down/up, Wi-Fi off and on |
| B3 | 1 h soak: reconnect count, heap low-water, no HID drops |
| B4 | Reconnect after pager reboot, app kill, iPhone Bluetooth toggle, out of range 5 min |
| B5 | Keyboard typing + media keys while a file transfer runs |

---

## 10. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | `BleKeyboard` owns init/deinit/server callbacks, and its `connected` flag is true for any central | Spike S1. Own the stack in `ble_stack`. Prefer porting HID onto `NimBLEHIDDevice` (removes the fork dependency and its begin/end leak) |
| R2 | iOS behavior with a peripheral that is both a system keyboard and an app accessory | Spike S2 before any UI work. Fallback: a separate advertising set is not available on all controllers, so the fallback is "companion mode" where HID is off |
| R3 | GATT throughput too low for OTA / big files | Credit window tuning, 15 ms interval during transfers, L2CAP CoC (`NimBLEL2CAPServer` is already vendored) |
| R4 | Wi-Fi/BLE coexistence on the single 2.4 GHz radio slows both | Pause hub/weather Wi-Fi fetches while a bulk BLE transfer runs; measure in B2 |
| R5 | Heap pressure (NimBLE + TLS + LVGL) | PSRAM for link buffers, fixed-size pools, S4 soak; cap concurrent transfers to 1 |
| R6 | iOS background limits (app suspended, no scanning without a service UUID) | Service UUID in the scan response, state restoration, proactive weather push so pager-initiated calls aren't time-critical |
| R7 | Power regression against the P4–P6 work | No periodic timers in the link; B1 numbers gate Phase 1 sign-off |
| R8 | Pairing change (`DISPLAY_ONLY`) breaks existing keyboard bonds | S5; if needed, a one-time "re-pair" prompt in `ui_companion` |

---

## 11. Decisions to make

| ID | Decision | Recommendation |
|---|---|---|
| D1 | Keep the BleKeyboard fork (patched) or port HID to `NimBLEHIDDevice` | Port. It removes the stack-ownership and leak problems at the root |
| D2 | Where the iOS codebase lives | **Decided:** in this repository, under `ios/`, with the shared spec and vectors in `protocol/` (§3.2) |
| D3 | Minimum iOS version | iOS 17 (Observation, modern concurrency); covers iPhone XS and later |
| D4 | Pairing: passkey (`DISPLAY_ONLY`) vs Just Works + app-level code | Passkey: OS-level MITM protection, nothing custom to get wrong |
| D5 | Encrypted notes: ciphertext only, or plaintext while unlocked; port `notes_crypto` to Swift? | Ciphertext only in v1; Swift port in Phase 5 if wanted |
| D6 | Weather source on phone | Open-Meteo (code parity with firmware); WeatherKit optional |
| D7 | Boards for v1 | Pager first; S3/Ultra get the link for free but UI/bench work later |

---

## 12. First concrete steps

1. Scaffold `ios/` (XcodeGen `project.yml`, empty `PagerKit` package,
   app target) and `protocol/` in this repository, with the `.gitignore`
   entries from §3.2.
2. Generate and freeze the PagerLink service + characteristic UUIDs, and
   commit them as constants in `src/hal/link/link_uuids.h` and
   `ios/PagerKit/Sources/PagerKit/Link/UUIDs.swift`.
3. Spike S1 on a branch: `ble_stack` + HID on `NimBLEHIDDevice`, keyboard
   and media keys verified on iPhone, both builds green.
4. Spike S2/S3 with a throwaway SwiftUI app (ping + throughput) against
   that branch, and record the numbers in §4.4.
5. Write `protocol/PROTOCOL.md` + the first golden vectors, then
   `link_frame` with its Unity tests, then the matching Swift codec against
   the same vectors.
6. Start Phase 1.

---

## 13. Progress log

### 2026-10-10 — protocol foundation + emulator path (`165067a`, pushed to `master`)

Done:

- **§12.1 scaffold:** `protocol/` (spec, vectors, oracle) and `ios/`
  (`PagerKit` package, XcodeGen `project.yml`, `Config/*.xcconfig`,
  `Resources/Info.plist`, `App/` with Dashboard + Diagnostics over TCP,
  `Makefile`, `README.md`, `.swift-format`). `.gitignore` entries from §3.2
  (+ `ios/Config/Local.xcconfig`, + the generated vector header). Root
  `Makefile`: `ios-gen`, `ios-test`, `ios-sim`, `protocol-test`. `CLAUDE.md`
  layout + build section updated.
- **§12.2 UUIDs frozen:** service `5BFF3035-4B08-4B35-BEEB-6A426D451D47`,
  rx `02855CCC-F4A4-4AA7-AFDC-237158247D4D`, tx
  `A975E68C-192B-4364-BE4E-6BB414A20346`, info
  `D7B0CF7E-0B2B-4CAC-BB23-4C0E20464B42` — `src/hal/link/link_uuids.h`,
  `ios/PagerKit/Sources/PagerKit/Link/UUIDs.swift`, `protocol/PROTOCOL.md` §1.
- **§12.5 spec + vectors + codecs:** `protocol/PROTOCOL.md` is normative.
  Decisions made while writing it: little-endian; per-direction seq starts
  at 0 per connection; after a `seq_gap`/`oversize` the receiver enters a
  *discard* state (one reset per fault, not one per remaining packet); REQ
  payload `{"m","p"}`, EVT `{"e","p"}`; transfer ids follow the receiver's
  parity; local-only error codes `timeout`, `disconnected`; `sys.info` drops
  the `vbus` field (no HAL source) and adds `wifi.connected`.
  `protocol/tools/mkvectors.py` is the oracle that writes `vectors/*.json`;
  three codecs are tested against it: C++ (`src/hal/link/link_frame.*`),
  Swift (`ios/PagerKit`), Go (`server/internal/pagerlink`).
- **Phase 1, hardware-independent part:** `link_json` (cJSON isn't in the
  emulator build), `link_rpc` (id parity/wrap, timeouts, late/dup drops,
  unknown → `unsupported`), `link_service` (transport-agnostic core,
  `evt.battery`), `handlers_sys` (`sys.hello/info/ping/time.set`),
  `link_transport_tcp.cpp` (emulator, `127.0.0.1:7600`, `PAGERLINK_PORT`),
  `src/hal/link.h` public API; emulator `main.cpp` starts the link.
  `server/cmd/pagerlink` CLI (`hello|info|ping|time|call|smoke`).

Evidence:

- `pio test -e native_test` → 92/92 (new: `test_link_frame` 10,
  `test_link_json` 13, `test_link_rpc` 22).
- `make -C ios test-linux` (swift:6.0-jammy) → 13/13 XCTests.
- `go test -race ./internal/pagerlink/` → ok, 78.7 % coverage.
- Negative controls: corrupting one expected reset in `reassemble.json` fails
  both the Unity and the XCTest vector suites (`gap_mid_message_then_continue`);
  restored, `mkvectors.py --check` clean.
- `pio run -e emulator_lora_pager` and `pio run -e tlora_pager` → SUCCESS.
- End to end: emulator under `xvfb-run` + `go run ./cmd/pagerlink smoke` →
  11/11 PASS (hello, info, ping RTT ≈ 80 µs, time.set, unknown method →
  `unsupported`, bad epoch / bad tz → `invalid_argument`, proto 2 →
  `unsupported`, injected wrong-seq packet → `seq_gap` reset, ping after
  reset OK).

Not verified here (no macOS): the app target and `TCPTransport`
(`Network.framework`, `#if canImport(Network)`) have never been compiled.
First action on a Mac: `make ios-gen && make ios-sim`, then connect the
Simulator to the emulator.

### Next steps (as of 2026-10-11)

The critical path is S1 → S2/S3 → BLE transport, and it needs a pager plus
an iPhone. Tracks B and C need neither and can run in parallel; they are
built and tested against the emulator over TCP with `pagerlink`.

**Track A — hardware (critical path; blocks every BLE item)**

1. **S1** — `src/hal/ble_stack.{h,cpp}` owns `NimBLEDevice` (init once,
   shared server, per-connection fan-out); keyboard + media keys ported to
   `NimBLEHIDDevice` in `src/hal/ble_hid.cpp`; `hw_get_ble_kb_connected()` =
   "peer subscribed to HID input". Code can be written and compiled on a
   branch without hardware. **Exit:** typing + media keys work on the
   iPhone; record decision D1 in §11.
2. **S2** — throwaway Swift app finds the pager via
   `retrieveConnectedPeripherals(withServices: [PagerLink])`, subscribes to
   `tx`, round-trips `sys.ping` while typing still works. Decides R2.
3. **S3** — granted MTU, DLE, notify / write-without-response KB/s at 15 ms
   with Wi-Fi off and on. Record in §4.4; go/no-go for L2CAP in v1.
4. **S4** (30 min soak with HID + link + Wi-Fi + TLS, heap low-water) and
   **S5** (`DISPLAY_ONLY` passkey with an already-bonded iPhone).
5. Rest of Phase 1 BLE: `link_transport_ble.cpp` (NimBLE callbacks →
   StreamBuffer → `link_task` → `link_service`), replace the ARDUINO stubs in
   `link_transport_tcp.cpp`, advertising/scan-response, passkey, trusted
   phones (NVS `companion`), conn-param policy (§5.1.7), iOS `BLETransport`
   + onboarding + state restoration, bench row B1.

**Track B — Mac (quick, unblocks the app)**

6. `make ios-gen && make ios-sim`; fix whatever the first compile of the app
   target and `TCPTransport` turns up (both written on Linux, never
   compiled). Then run the emulator and connect from the Simulator
   (Dashboard → Emulator) — Phase 1's "emulator + iOS Simulator path works
   end to end" criterion.

**Track C — no hardware (emulator over TCP)**

7. **Done 2026-10-11 (`cadd656`, firmware + Go; see the log entry below).**
   **Phase 2 file service:** `handlers_fs.cpp`
   (`fs.list/stat/read/write.begin/commit/abort/delete/mkdir/rename`,
   `evt.fs.changed`), credit-window bulk transfer (§4 of `PROTOCOL.md`),
   chunked write to `<path>.part` + rename on CRC-matching commit,
   `hw_rename_path` / `hw_mkdir_path` in `hal/storage`, path sandbox via the
   `notes_path` rules, `protected:true` + ciphertext for encrypted notes,
   busy vote during transfers. Move the `fs.*` rows into `PROTOCOL.md` §6,
   add bulk vectors, extend `pagerlink smoke`. **Exit (emulator):** 1 MB
   file round-trips with matching CRC32; a connection dropped mid-upload
   leaves no partial file; `..` / outside-root paths rejected (unit test +
   negative control). iOS Files screen follows once Track B is done.
8. **`ui_companion` screen** (`core::App` "Phone": pairing code, peer,
   transfer progress, trusted list, "find my phone"), Companion toggle in
   `settings_connectivity.cpp`, `UI_SHOTS` states (pairing / connected /
   transfer) in every style × dark mode. Binds to `hal/link.h` only; the
   BLE-specific parts light up after Track A.
9. **Done 2026-10-11 (`668cbef` + `bf12208`; consumer in `e489487`, see the log entry below).**
   **Phase 3 groundwork:** extract `ForecastCache`/`GeoData` load/save into
   `hal/weather_cache.{h,cpp}` as a behaviour-neutral commit first, then
   `handlers_weather.cpp` (`weather.push`, and the pager-initiated
   `weather.get` step in the fetch chain) tested from `pagerlink`.

Recommended order: 1 (start the code now) and 6 whenever a Mac is at hand;
otherwise 7 → 8 → 9 while hardware is unavailable.

### 2026-10-11 — Track A: Spike S1 code + firmware BLE transport (uncommitted, needs bench)

Done (firmware only; untested on hardware):

- `hal/ble_stack.{h,cpp}` owns NimBLE: init once on first BT enable, never
  deinit ("BT off" = stop advertising + disconnect). One server; HID services
  first (keeps bonded handles), PagerLink appended. Adv: flags, appearance
  keyboard, 0x1812, name `lilygo-pager`; scan response: PagerLink UUID.
  ATT_MTU 247. Security unchanged (bond + SC, Just Works).
- `hal/ble_hid.{h,cpp}`: keyboard + media keys on `NimBLEHIDDevice`, report
  map / PnP ids / ASCII map copied from the BleKeyboard fork, which is dropped
  from `lib_deps` (D1 = port, pending the bench check). Keyboard = peer
  subscribed to a HID input report; reports go only to those peers.
- `hal/link/link_transport_ble.{h,cpp}`: rx (write / write-nr, encrypted),
  tx (notify), info (read JSON). Host callbacks queue into a PSRAM queue; the
  `link` task (8 KB, prio 1, core 0) drives `link_service`, blocks while no
  companion, ticks at 1 Hz while one is. Companion = first `tx` subscriber,
  P = MTU − 3. Replaces the ARDUINO stubs in `link_transport_tcp.cpp`.
- NimBLE resolves to 2.5.1 (`^2.2.3`). Its handle-less `notify()` reaches every
  connected peer, subscribed or not, so HID addresses peers explicitly.
- Evidence: tlora_pager, twatchs3, twatch_ultra, emulator_lora_pager build;
  native 133/133.
- Open: S1 exit (typing + media keys on iPhone), whether bonded phones need
  "Forget This Device" to see the new service, and whether a restored `tx`
  CCCD on reconnect marks a phone as companion while the app isn't running
  (S2). The iOS `BLETransport` is still missing (since added:
  `ios/PagerKit/Sources/PagerKit/Transport/BLETransport.swift`).

### 2026-10-11 — Track C item 9: weather cache + `weather.push` (`668cbef`, `bf12208`, local, not pushed)

Done (firmware + Go; the iOS Open-Meteo provider and proactive push wait for Track B):

- `hal/weather_cache.{h,cpp}`: ForecastCache/GeoData load/save moved out of
  `ui_weather.cpp` unchanged on hardware (`sizeof(ForecastCache) == 144`
  asserted); RAM-backed on the emulator.
- `hal/weather_payload` (pure ForecastPayload codec, integer fields, ranges
  rejected), `handlers_weather.cpp` (`weather.push`), `"weather"` cap,
  `PROTOCOL.md` §6.3 (`weather.push`, `weather.get`), `pagerlink` mock + smoke.
- The pager-initiated `weather.get` step landed as `HUB_REMOVAL_PLAN.md` H2.3
  (`e489487`): direct → phone → stale cache, "via phone" label.
- Evidence: native 133/133, `make protocol-test` 0, smoke 45/45, all six envs
  build. Negative controls: temp range check removed → native + smoke FAIL;
  reply-key check removed → smoke step FAIL.

### 2026-10-11 — Track C item 7: Phase 2 file service on the emulator (`cadd656`, local, not pushed)

Done (firmware + Go; the iOS Files screen waits for Track B):

- `fs.list/stat/read/write.begin/commit/abort/delete/mkdir/rename` and
  `evt.fs.changed` (`src/hal/link/handlers_fs.cpp`), specified in
  `PROTOCOL.md` §6.1. `sys.hello` caps now include `"fs"`.
- Credit-window bulk transfer as a pure engine (`link_xfer.{h,cpp}`:
  credits count DATA messages, go-back-N on a no-progress ACK) plus
  CRC-32/ISO-HDLC, with CRC vectors added to `bulk.json` (zlib oracle).
- Writes land in `<path>.part` and are renamed only on a size + CRC-matching
  commit; abort, a 30 s stall and a link drop delete the `.part`. Max two
  transfers. `fs.read` ids are allocated by the phone (even), `fs.write` ids
  by the pager (odd), per §4's receiver rule.
- Path sandbox: `sandbox_path()` in `notes_path` (`..`, backslash, control
  bytes rejected). Encrypted notes are served raw with `protected:true`.
- Storage goes through `fs_volume.{h,cpp}` rather than new `hw_rename_path`
  / `hw_mkdir_path` in `hal/storage`: on the emulator it is a real directory
  tree (`$PAGERLINK_FS_ROOT`, default `/tmp/pagerlink-fs`), separate from the
  canned listings the UI and screenshot sweep render.
- `Rpc::defer()` (first DATA after the RES); `hw_get_filesystem_generation()`
  drives `evt.fs.changed` without clearing the apps' dirty flag; transfers
  tap the P6.11 CPU boost (`hw_cpu_boost_for_input`).
- Go: `Client.ReadFile/WriteFile`, CLI `ls|stat|get|put|rm|mkdir|mv`.

Evidence:

- Exit criteria, `go run ./cmd/pagerlink smoke` against the emulator → 24/24
  PASS: 1 MiB round trip with matching CRC-32; a connection dropped after
  8 KiB of a 64 KiB upload leaves neither the file nor its `.part`; `..`
  paths rejected on stat/list/read/mkdir; bad CRC → `invalid_argument` and
  no `.part`; `evt.fs.changed` received.
- Negative controls: letting `..` through fails `test_sandbox_rejects`;
  keeping the `.part` on drop fails the smoke's bad-CRC and dropped-upload
  steps.
- `pio test -e native_test` → 113/113 (new `test_link_xfer` 11, sandbox 4,
  CRC vectors 1, `Rpc::defer` 2). `make protocol-test` green. Go
  `internal/pagerlink` coverage 78.7 → 80.7 %.
- All six envs build. Screenshot sweep unchanged below the status bar.

Not done in item 7: resume (`resume:true` → `unsupported`), the
trusted-phone confirm prompt (§8, needs Track A's trusted list), deferring
fake sleep during a transfer (needs the BLE transport on hardware), the iOS
Files screen (Track B). `make protocol-test` leaves
`.pio/build/emulator_lora_pager/program` missing; rebuild the emulator
before a smoke run.
