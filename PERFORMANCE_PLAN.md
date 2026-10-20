# Performance plan: full-app audit, phase 7

Written **2026-10-19** against **`c9c1a96`** ("docs: record on-device input-stutter
measurements and IS.8-IS.11"). The working tree differs from it only in
`ios/PagerKit/Sources/PagerKit/Services/TDTelegram.swift` and its test.

Items are numbered **`P7.x`**, continuing the `P2.x`…`P6.x` / `PB.x` / `IS.x` series.
Every finding was checked against the earlier docs before being kept (`OPTIMIZATION_*.md`,
`INPUT_STUTTER_PLAN.md`, `HUB_REMOVAL_PLAN.md`, `IOS_COMPANION_PLAN.md`). An item that
extends an older one names it.

**Scope.** Six areas were audited in parallel, each against current source:
1. Boot and task/timer cadences.
2. LVGL rendering and work on the UI thread.
3. Storage, SD and crypto.
4. Networking, PagerLink and BLE.
5. Memory, heap, flash and build configuration.
6. The companions: `server/` (lilyhub), `ios/PagerKit` and the iOS app, and `tools/pagerlink`.

Each audit did two things:
- Re-checked every item that an older doc still lists as open (§3).
- Swept its area for new problems (§4–§11).

> **⚠️ Measurement discipline (inherited, unchanged).** Statements of *mechanism* (what the
> code does) were verified by reading the source, the vendored LVGL 9.5.0, and the
> arduino-esp32 2.0.17 / IDF 4.4.7 framework. The only measured numbers are:
> - the IS.0 on-device rows (`INPUT_STUTTER_PLAN.md` §7);
> - the fresh build (§1);
> - the emulator gdb trace (P7.9, P7.11);
> - the host simulations of the transfer engine and `JSONValue` (P7.29, P7.55).
>
> **Every other magnitude is an estimate and is labelled as one.**
> Before investing in a MED/HIGH-risk item, get the number first (P7.9, P7.49, §G.1 of
> `OPTIMIZATION_PHASE6.md`).

**Build discipline (unchanged).** After every change, run:
```
pio run -e tlora_pager && pio run -e emulator_lora_pager && pio test -e native_test
```
Shared HAL changes also need `twatchs3` and `twatch_ultra` builds. PagerLink engine
changes also need `make protocol-test`. Anything touching tasks, ISRs, locks, rails, sleep
or the BLE stack is ⚠️ **hardware-test-required**.

---

## 0. Headline

1. **PBKDF2 is still the biggest measured freeze, and more code paths reach it than were
   recorded.** Each derivation costs 1.9–2.7 s at 240 MHz (measured, IS.10), and about 20
   user flows trigger one on the LVGL thread (§5 table).
   - **P7.1** is a few-line fix. Encryption never seeds the key cache, so every edit
     followed by a reopen costs a guaranteed ~2 s miss.
   - **P7.2** (midstate PBKDF2) speeds up every row of the table.
2. **PagerLink bulk transfers can stall or livelock on a single lost message (P7.29).**
   - The bug is in all three engines: firmware, Go and Swift.
   - A host simulation of the real engine with one dropped message:
     - 8 KB stalls until timeout;
     - 100 KB takes 2,113 sends instead of 26;
     - 256 KB takes about 904,000 sends.
   - **P7.30** makes loss likely in the first place. The BLE receive window is about 8×
     larger than the pager's packet queue, and the overflow drops packets.
3. **Frames are expensive per pixel, not over-invalidated.**
   - The measured home frame works out to about 2.5–3.2 µs/px (derived from IS.11).
   - An emulator gdb trace shows that **idle Settings does not redraw itself**: the only
     invalidations are about 44×17 px status-bar labels. The 200–336 ms "idle" Settings
     frames must come from somewhere else.
   - Measure render, swap and push separately first (**P7.9**). Then do P6.9 → P6.10 →
     P4.20.
   - The editor re-measures and repaints the whole textarea on every keystroke (**P7.10**).
4. **Boot is still about 3–4.5 s of blocking vendor work before the first frame
   (estimate).**
   - New since P6: the ST7796 init table sleeps **600 ms** (5 × 120 ms, calculated from
     code). About 360 ms of that is register writes that need no settle time (**P7.44**).
   - P6.1–P6.4 are all still open.
5. **The SD card still runs at 4 MHz (P6.19).** That cost compounds into P7.24, P7.34 and
   every SPI-lock stall behind a display flush.

---

## 1. Size snapshot

Fresh `pio run -e tlora_pager` on 2026-10-19 at `c9c1a96`.

| Item | 2026-09-21 (P6) | Now | Delta |
|---|---|---|---|
| `firmware.bin` | 2,855,264 (68.1 % of 4 MiB slot) | **2,864,352 (68.3 %)** | +9,088 |
| `.flash.text` / `.flash.rodata` | 1,577,375 / 1,155,792 | 1,584,387 / 1,157,824 | +7,012 / +2,032 |
| `.iram0` | 88,134 | 88,154 | +20 |
| static DRAM (`.dram0.data` + `.bss`) | 89,960 | **94,600** (33,568 + 61,032) | **+4,640** |

**Flash contributors:**
- **lvgl 610.7 K**, of which the built-in Montserrat fonts are **404 K** (size 48 alone is 96.7 K).
- **`src/` 628.8 K**: custom fonts 206 K, `kRootCaBundle` 55.6 K, `hal/link/` 54.1 K,
  C++ unwind tables ~53 K.
- **LilyGoLib 157.6 K**, of which the BHI260 blob is 123.7 K.
- Framework and libraries: net80211 126 K · libc 98 K · lwip 98 K · LibSSH 95 K ·
  mbedcrypto 92 K · NimBLE 87 K · libsodium 70 K.
- **Exception tables:** `.eh_frame` 105 K plus `.gcc_except_table` 16 K.

**What changed since P6:**
- Added about +85 K flash: PagerLink and the BLE-era modules.
- Removed about −53 K: helix-mp3 (`7038cb7`), plus the hub/dead-code removal.
- Static DRAM grew +4.6 K: link BSS 2.4 K, theme backdrop tiles 2 K, and the rest.

**sdkconfig facts not recorded before:**
- `CONFIG_HEAP_POISONING_LIGHT=y`. It is in the prebuilt framework, so the project cannot
  change it.
- The task watchdog checks **only the CPU0 idle task**.
- `SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` is unset.
- Wi-Fi already uses dynamic buffers (clean).

---

## 2. Corrections to the record

| Claim in an older doc | Reality (verified 2026-10-19) |
|---|---|
| `INPUT_STUTTER_PLAN.md` IS.8 / IS.9: "working tree, uncommitted" | Committed: `ee0bd75` (IS.8), `cf007a7` (IS.9). |
| P6.8: blinking textarea cursors keep LVGL at 30 Hz | **Premise stale.**<br>• `ui_theme.cpp` `theme_init` installs `new_theme_apply_cb` on the default theme object itself, so the default theme's `theme_apply` (which holds the 400 ms cursor anim) never runs on app widgets.<br>• Emulator trace: `cursor_blink_anim_cb` ran 0 times.<br>• Side effect (functional, not perf): only the editor styles `LV_PART_CURSOR`, so other textareas probably show **no cursor at all**. Confirm on a screenshot. |
| P6 §4 / P6.18: "no gradients, buttons have no shadows"; the theme GROW/TRANSITION cost | **Stale.**<br>• `ui_theme.cpp` `build_styles` gives Hotel/BeOS buttons `LV_GRAD_DIR_VER` plus a 1 px shadow, and `ui_block` adds a gradient and shadow.<br>• The default-theme transitions never apply (same cause as P6.8). |
| PB.14: removing the write-only gauge fields saves I2C reads | **Premise stale.** In SensorLib 0.3.3, every `GaugeBQ27220` getter reads the cached struct. All the I2C is in `refresh()`: one 54-byte read plus one 4-byte read. See P7.16. |
| PB.8: the LVGL task idles up to 200 ms | Still unreachable: the indev read timers run at 33 ms. P6.6 is only half done (§3). |
| P6.26: 14 KB MP3 refill under one lock | **Premise stale.** MP3 was removed (`7038cb7`); the player is WAV-only with 4 KB chunks. The recorder half is still open. |
| P6.28 / P6.34: hub Telegram proxy, weather hub fallback | **Premise stale.** H1 (`39c4e3f`) removed both firmware paths.<br>• `hal::hub_last_reachable` has zero readers.<br>• The only hub endpoints still called are `/api/chat` and `/api/notes/{upload,sync}`; both are fallbacks until W16. |
| P6.36: voice memos over ~3 min can't be sent | **Mitigated.** Voice is capped at 15 s (`CHAT_AUDIO_MAX_BYTES`), and the phone path sends raw WAV. |
| P3.24: NimBLE heap in internal RAM is 30–60 KB | Estimate looks high. The movable host pools come to about 10–15 KB (estimate). |
| `core/kb_lock.h`: "Writes are safe without it" | **Wrong** for read-modify-write register writes (P7.45). |
| `notes_crypto.cpp/.h`: PBKDF2 is "tens of ms" | Measured 1.9–2.7 s (IS.10). The comment is still uncorrected. |
| PB.20 / P5.4: the frequency floor keys on "BLE associated" | Since `f71fd9e`, `hw_get_ble_kb_connected()` means *HID subscribed*. A companion-only link gets 20 MHz (P7.35). |
| `IOS_COMPANION_PLAN.md` §5.2: "PSRAM message buffers" | Nearly every link buffer is ≤ 4096 B, the internal-first threshold, so they land in internal RAM. Only the 4102 B encoded message and the BLE queue reach PSRAM (P7.36). |
| `link_frame.h` / `PROTOCOL.md`: credits = "16 packets" | The engine counts 16 **messages** of up to 4092 B (P7.30). |

---

## 3. Status of the items older docs still list as open

Legend: ☐ still open · ◐ partially · ✅ fixed · ⊘ moot or stale.

### 3.1 Boot, cadence, render (P6 §A/§B, PB, IS)

| ID | Item | Status | Evidence |
|---|---|---|---|
| P6.1 | 32 kHz crystal calibration ~0.8 s | ☐ | `LilyGoGeneral.cpp` `calibrate_one` (5 × 1000 cycles) and `esp_enable_slow_crystal` are unchanged. |
| P6.2 | BHI260 123.7 KB upload before the display | ☐ | `factory.ino` `setup()` `disable_flags` has no `NO_HW_SENSOR`. |
| P6.3 | PSRAM self-test every boot | ☐ | There is no `testSPIRAM` override. |
| P6.4 | `setupMSC()` lists the whole FFat tree | ☐ | `USB_MSC.cpp` `setupMSC` → `__listDir(FFat,"/",3)`. |
| P6.5 | Boot clear allocates 2× the buffer (426 KB) | ☐ | `LilyGoDispArduinoSPI::init` `draw_buf(_width*_height*2)`. |
| P6.6 | LVGL task never idles | ◐ | Notify wait landed (`4761042`). Still missing: `lv_indev_set_mode`, so the indev timers run at 33 ms, about 30 wakes/s. |
| P6.7 | Status-bar colour setters invalidate every second | ☐ | `core/system.cpp` `setupGlobalUI` lambda: unconditional `lv_obj_set_style_text_color` for the SD/BT/WiFi/Hub/USB icons. Emulator: 2 invalidations per tick. |
| P6.9 | CPU byte-swap per flush | ☐ | `LV_Helper_v9.cpp` `disp_flush` → `lv_draw_sw_rgb565_swap`. `LV_DRAW_SW_SUPPORT_RGB565_SWAPPED` is available. |
| P6.10 | Second full-screen buffer is useless | ☐ | `beginLvglHelper` makes two 213 KB `ps_malloc`; `useDMA()` is false. |
| P6.11 | First input after idle runs at 80 MHz | ✅ | `hw_cpu_boost_for_input`. Gap for timer-triggered renders: P7.14. |
| P6.12 | Opaque overlays re-render the hidden app | ☐ | Nothing hides `_mainScreen`/`_statusBar` (`ui_tools.cpp`). |
| P6.13 | Home rebuilt on exit, local styles, style cache off | ◐ | The rebuild is gone (IS.3). Still present: ~20–25 local props per tile and `LV_OBJ_STYLE_CACHE 0`. New neighbour: P7.11. |
| P6.14 | SSH terminal re-measure, trim, read loop | ☐ | All four parts. Extended by P7.43. |
| P6.15 | Recording screen 30 fps shadowed dot | ☐ | `ui_audio_notes.cpp` `build_record_view`. |
| P6.16 | Tasks list full rebuild, `LV_EVENT_ALL` | ☐ | `ui_tasks.cpp` `rebuild_task_list`. |
| P6.17 | Animated scroll-on-focus | ☐ (product call) | `ui_journal.cpp` `post_focus_cb` `LV_ANIM_ON`; `lv_menu` `SCROLL_ON_FOCUS`. |
| P6.18 | Small bundle | ◐ | Still open:<br>• rotary `getRotary` 50 ms timeout (20 wakes/s);<br>• `loop()` `delay(50)`;<br>• `notify::post` doesn't wake LVGL;<br>• journal `clip_corner`;<br>• chat `set_status` every 200 ms;<br>• `ESP.getHeapSize()` heap walk;<br>• `battery_history` vector with no reader.<br>The theme part is stale (§2). |
| P6.45 / P4.25 | `-O2`/IRAM, multi-core draw, SIMD | ☐ | `-Os`, `LV_OS_NONE`, `DRAW_UNIT_CNT 1`, `LV_USE_DRAW_SW_ASM NONE`. |
| P4.20 | PSRAM render target, CPU-polled SPI | ☐ | `pushColors` → `_spi->writeBytes`. |
| PB.9 | Keyboard 100 Hz poll | ✅ (P4.12) | Leftover: the 100 ms idle fallback does 2 I2C reads (see P7.45). |
| PB.10 / PB.15 / PB.16 / PB.17 | Hub probe 30 s, gauge TTL, park 1 s, freq dedupe | ✅ | — |
| PB.12 | BLE cadences | ◐ | Advertising interval is never set (P6.40). Companion-only gap: P7.35. |
| PB.13 | Charge VBUS IRQ | ⊘ | There is no PMU interrupt pin. |
| PB.20 | 80 MHz fake-sleep floor | ◐ | Premise changed (P7.35). |
| PB.21 | ESP-IDF power management | ⊘ blocked | `CONFIG_PM_ENABLE` unset in 2.0.17. |
| P5.1–P5.4 | Fake-sleep residual drain | ✅ (P5.4 not checked on hardware) | — |
| IS.0 | Profiler | ◐ | Instrumentation is in. Missing: boot and wake markers, and per-stage frame times (P7.9). |
| IS.1–IS.6, IS.8, IS.9 | Input-stutter fixes | ✅ | IS.8 residual → P7.45. IS.9 bug → P7.23. |
| IS.7 / IS.11 | Transition and render-bound frames | ☐ | Broken down in P7.9–P7.13. |
| IS.10 | PBKDF2 ~2 s on the LVGL thread | ☐, wider than recorded | §5. |

### 3.2 Storage and crypto (P6 §C)

| ID | Item | Status | Evidence |
|---|---|---|---|
| P6.19 | SD at 4 MHz on the shared SPI bus | ☐ | `LilyGo_LoRa_Pager.cpp` and `LilyGoWatchUltra.cpp` `installSD()` → `SD.begin(…, 4000000U, …)`. |
| P6.20 | `openNextFile` listings O(n²), SPI lock held for the walk | ☐ | Every lister still uses it: `storage.cpp` `list_files`/`list_entries`/`hw_count_internal_files`/`delete_path_recursive`, `storage_bulk.cpp`, and `link/fs_volume.cpp` `vol_list`/`remove_recursive`. Measured 423 ms for 51 notes (IS.9). |
| P6.21 | Listings return mtime 0 (journal previews never refresh) | ☐ bug | `list_files`, `list_entries`, `vol_list`. Prune "oldest first" is really alphabetical. |
| P6.22 (a) | Prewarm never re-armed | ☐ | `notes_crypto_prewarm()` has one caller (`editor_build_ui`). |
| P6.22 (b) | Secrets evicted from the 6-entry LRU | ☐ | `NC_KEYCACHE_N = 6`, no pinning. |
| P6.23 | Synchronous SD mirror on the UI thread | ◐ | The editor exit is async (IS.2). Still synchronous: tasks `save_tasks`, journal `delete_confirm_cb`, file-browser and audio-note deletes, plus `ensure_notes_dir` `exists` ×2 on every save. |
| P6.24 | `hw_read_file` reads SD first (slow, can be stale) | ☐ bug | `hw_read_file`. |
| P6.25 | Snippet decrypts the whole file | ☐ | `hw_read_preferred_file_snippet`. |
| P6.26 (recorder) | 1 KB written per lock, never flushed | ☐ | `recorderTask`. |
| P6.27 | Bundle | ◐ | ✅ Copy→SD now includes `/notes`.<br>☐ `notes_crypto_is_enabled()` reads NVS on every call (13 sites, including each journal page flip).<br>☐ Magic probe reads the whole file.<br>☐ 3× plaintext copies on encrypt.<br>☐ Status-bar file count walk (off by default). |
| P2.7 item 3 | File-browser `load_entries` on a worker | ☐ | Still on the LVGL thread (P7.19). |

### 3.3 Network, BLE, memory, flash (P6 §D/§E/§F)

| ID | Item | Status | Evidence |
|---|---|---|---|
| P6.29 | CA bundle index freed and re-allocated per request (use-after-free race) | ☐ bug | `wireless.cpp` `setup_secure` runs unconditionally from `http_open`/`hw_http_request`, including for `http://`. Concurrent workers still exist. |
| P6.30 | mbedTLS 35–45 KB per session, internal only | ☐ | There is no `mbedtls_platform_set_calloc_free`. |
| P6.31 | No HTTP keep-alive | ☐ (less needed) | Telegram prefers the phone whenever the link is up. |
| P6.32 | 5 s time-to-first-header | ☐ narrower | Affects only the hub notes-sync fallback (moot at W16). |
| P6.33 | Notes uploaded twice | ◐ | The phone path fixed it by design. The hub fallback still does it. |
| P6.35 | Weather downloads 168 hourly points, renders 8 | ☐ | `ui_weather.cpp` `fetch_weather` `forecast_days=7`. The phone side is the same (P7.61). |
| P6.37 | TLS handshake timeout is the 120 s default | ☐ | There is no `setHandshakeTimeout`. It also delays the weather phone fallback (P7.39). |
| P6.38 | Wi-Fi reconnect: no BSSID/channel, slot 0 only | ☐ | `hw_wifi_reconnect_saved` → `wifi_load_saved(0)`. |
| P6.39 | `listen_interval` default | ☐ | — |
| P6.40 | BLE advertises at 30–60 ms forever, + stray-key bug | ☐ (code moved) | `ble_stack.cpp` `configure_advertising` sets no interval. **The stray-key bug is still there:** `ui_media_remote.cpp` `forward_key_cb` → `hw_set_ble_kb_char(&c)` → `ble_hid::type(const char*)` reads past a single `char`. |
| P6.41 | Proxy replies lack Content-Length / Cache-Control | ⊘ for firmware | — |
| P6.42 / P3.27 | Second OTA slot: 4 MiB never written | ☐ | `partitions.csv` is unchanged; there is zero OTA code. |
| P6.43 | LibSSH 10 KB DRAM + 165 KB flash | ☐ (product call) | — |
| P6.44 | Unused blend colour formats | ☐ | Pairs with P7.53. |
| P3.9 | SSH 32 KB internal stack | ☐ | The "TEMPORARY" `[stackwm]` `Serial.printf` every 3 s **ships in production** (`ui_ssh.cpp` `run_session`). |
| P3.14 | `ble_kb_ka` 3 KB stack never measured | ☐ | Needs P7.49. |
| P3.24 | NimBLE heap internal | ☐ | About 10–15 KB movable (estimate). |
| P3.25 / P6.2 | BHI260 blob 123.7 K | ☐ | — |
| P4.26 | `app/play` 8 KB stack | ☐, more oversized now | MP3 is gone; 2–3 KB is probably enough (measure first). |
| P2.4 | cJSON PSRAM hooks | ◐ regressed | Telegram moved off cJSON (P7.50). |

### 3.4 Server (`OPTIMIZATION_REPORT.md` §4, P2 §4)

The firmware now calls only `/api/chat` and `/api/notes/{upload,sync}`, and `server/` is
deleted at W22 (`HUB_REMOVAL_PLAN.md`). `go vet`, `go test -race` and `gofmt` are clean in
`server/` and `tools/pagerlink`.

- **Fixed:** A1–A3, B2–B6, D1–D9 (except D10), D11–D13, D2b.
- **Still open but moot** (the weather routes are no longer called): B1/D2a singleflight, D14, P6.41.
- **Still open and live until W16:** D10 (`createBlob` marshal copy) and P7.59 (1,000-file listing cap).
- **Unchanged:** the toolchain is still unpinned (`go 1.22`, no `toolchain` line).

---

## 4. Executive summary of new findings

Value = expected payoff. Risk = chance of regression. "HW" = needs a hardware test.

| ID | Item | Area | Value | Risk |
|---|---|---|---|---|
| P7.1 | Encrypt never seeds the key cache, so edit → reopen always costs a ~2 s PBKDF2 | Crypto | **HIGH** | LOW |
| P7.2 | Faster PBKDF2: HMAC midstates, software SHA, T1/T2 on both cores | Crypto | **HIGH** | MED |
| P7.3 | Status-label-only decrypts on the LVGL thread; the "deferred" refresh runs before the first paint | Crypto/UI | **HIGH** | LOW |
| P7.4 | Unlock derives on the LVGL thread then flushes the cache, so Journal pays a second derive (~4 s) | Crypto/UI | **HIGH** | MED |
| P7.5 | Prewarm competes with the foreground decrypt for the SHA engine | Crypto | MED | LOW |
| P7.6 | Journal first run / bad index: whole reconcile on the LVGL thread (N × 2 s) | Crypto/UI | MED | LOW-MED |
| P7.7 | Set/change/disable passphrase: one derive per *copy*, on the LVGL thread | Crypto | MED | MED |
| P7.8 | Telegram bg poll decrypts the token before checking for the phone path | Crypto | LOW-MED | LOW |
| P7.9 | No per-stage frame timing; idle Settings frames don't come from Settings | Render | **MED (do first)** | none |
| P7.10 | Editor keystroke re-measures and invalidates the whole textarea | Render | **HIGH** | MED, HW |
| P7.11 | Every Settings exit restyles and relays out the whole UI | Render | MED | LOW |
| P7.12 | Journal: `clip_corner` 3 passes per card, 1.5 KB labels, animated page scroll | Render | MED | LOW |
| P7.13 | Style draw cost (gradient + shadow on every button, translucent borders); shadow/circle caches off | Render | LOW-MED | LOW |
| P7.14 | Timer- and worker-triggered renders run at 80 MHz | CPU | LOW-MED | LOW-MED |
| P7.15 | Fake-sleep entry renders home at 20 MHz with the awake tasks re-enabled | CPU/Render | MED-LOW | MED, HW |
| P7.16 | Gauge `refresh()` (58 B of I2C, ~14 ms) inside the LVGL frame every 1–5 s | CPU | LOW-MED | LOW |
| P7.17 | Status-bar "typing" gate freezes the clock, icons and USB warning while typing | UI/bug | LOW-MED | LOW |
| P7.18 | Chat / Notes Sync log: whole 8 KB label re-set and forced layout per line | Render | LOW | LOW |
| P7.19 | `wait_pending_save` spin and SD listings inside LVGL callbacks | UI | LOW-MED | LOW-MED |
| P7.20 | "Copy Notes → Phone" and the hub copy run inside an LVGL event (seconds to minutes) | UI | **HIGH (UX)** | LOW |
| P7.21 | BLE HID `delay(7)` per report on the LVGL thread | UI | LOW | LOW |
| P7.22 | Small render items (IMU debug, msgbox shadow, LVGL builtin memcpy) | Render | LOW | LOW |
| P7.23 | IS.9 freshness check off by one, so every index rewrite triggers a second reconcile | Storage/bug | MED | LOW |
| P7.24 | `hw_read_file` holds the SPI lock through the ~2 s decrypt | Storage | MED | LOW |
| P7.25 | Journal snippets: ~77 KB internal DRAM, doubled by the IS.9 snapshot | Memory | MED | LOW-MED |
| P7.26 | `hw_mount_sd` never marks the card online and mounts without the SPI lock | Storage/bug | LOW | LOW-MED |
| P7.27 | NVS `fs/dirty` flag written twice per edit → journal cycle | Storage | LOW | LOW |
| P7.28 | `sys.info` may trigger a full-FAT free scan on `link_task` (verify) | Storage | LOW | LOW |
| P7.29 | **Transfer engine go-back-N: one lost message → stall or retransmit storm (all 3 engines)** | Link | **HIGH** | MED |
| P7.30 | BLE receive window ~8× the pager rx queue; `post()` blocks the NimBLE host 50 ms then drops | Link/BLE | **HIGH** | MED, HW |
| P7.31 | PagerLink bulk sends starve BLE HID (keys dropped or stuck) | BLE | MED-HIGH | LOW-MED, HW |
| P7.32 | Notify back-pressure: 2 ms poll, 200 ms budget, initial window pumped under `g_mutex` | Link | MED | MED, HW |
| P7.33 | No transfer connection params, MTU read once, no DLE/2M PHY | BLE | MED | MED, HW |
| P7.34 | Link file I/O reopens per 4 KB chunk; whole-file CRC passes; notes read ~3× per sync | Link/Storage | MED | MED |
| P7.35 | Companion-only connection: 20 MHz in fake sleep, no idle conn params, 1 Hz tick while asleep | Power/BLE | MED | MED, HW |
| P7.36 | Link buffers all in internal RAM; 4 copies + 1 alloc per BLE packet | Memory/Link | MED | LOW |
| P7.37 | `MAX_PACKET` 512 vs MTU 247, link stack 8 KB, NimBLE `MAX_CONNECTIONS` 3 | Memory | LOW-MED | LOW |
| P7.38 | CRC-32 table 1 KB DRAM with a lazy-init race; the ROM has `esp_rom_crc32_le` | Memory | LOW | LOW |
| P7.39 | Weather tries direct first with Wi-Fi but no internet (+5–120 s before the phone) | Network | LOW | LOW |
| P7.40 | Network probes nobody reads (inet probe with the phone up, hub probe) | Network | LOW | LOW |
| P7.41 | Hub chat: 15 s device read budget vs ~60 s server; byte-at-a-time read | Network | LOW-MED (moot at W16) | LOW |
| P7.42 | HTTP bodies held twice; chunked bodies grow by realloc every 1,460 B | Memory/Net | LOW | LOW |
| P7.43 | SSH loop: ~10 KB/s cap, ~150 ms echo latency, prod stack-watermark print | Network | LOW | LOW |
| P7.44 | **ST7796 init sleeps 600 ms; ~360 ms of it is not needed** | Boot | **MED-HIGH** | LOW-MED, HW |
| P7.45 | I2C bus not locked bus-wide; read-modify-write expander writes can drive every rail high | Bus/bug | MED-HIGH | MED, HW |
| P7.46 | Radio fully reconfigured on every wake even when disabled | Boot/Power | LOW | LOW |
| P7.47 | OneShotJob / weather results polled at 10 Hz | CPU | LOW | LOW |
| P7.48 | `heap_caps_malloc_extmem_enable` threshold never lowered | Memory | MED | MED, HW |
| P7.49 | No heap low-water / largest-block / stack watermark telemetry | Memory | **MED (unblocks)** | LOW |
| P7.50 | Telegram JSON moved off PSRAM-hooked cJSON onto internal-first `std::string` | Memory | MED | LOW-MED |
| P7.51 | Montserrat 48 costs 96.7 KB for a handful of glyphs | Flash | LOW-MED | LOW |
| P7.52 | `-fexceptions` keeps ~40–50 KB of `src/` unwind tables for two `try` blocks | Flash | LOW | MED |
| P7.53 | Four unused LVGL widgets compiled in (~10 KB) | Flash | LOW | LOW |
| P7.54 | `LV_ASSERT_HANDLER while(1)` hangs core 1 silently (WDT watches CPU0 only) | Robustness | LOW | LOW |
| P7.55 | iOS: TDLib decodes every update and hops to the main thread for each | iOS | MED | LOW |
| P7.56 | iOS: `JSONValue` decode is a `try?` cascade; new `JSONEncoder` per call | iOS | MED | LOW |
| P7.57 | iOS + fw: notes sync lists the repo and commits once per manifest batch | iOS | MED | LOW / MED |
| P7.58 | GitHub Contents listing caps at 1,000 files (iOS + server) | iOS/Server | LOW → MED | LOW |
| P7.59 | Chat deadline mismatch: phone retries up to ~6 min; pager watchdog shorter than begin+upload+ask | iOS/fw | LOW-MED | LOW |
| P7.60 | iOS weather: no cache, geocodes every time, 7 days hourly | iOS | LOW | LOW |
| P7.61 | iOS: Telegram chat list rebuilt with up to 30 serial `getChat` per poll | iOS | LOW | LOW |
| P7.62 | iOS `LocationProvider`: reverse geocode per request; "auto" path can exceed the pager's 15 s | iOS app | MED-LOW | LOW |
| P7.63 | iOS small items: oversize reply dropped silently, abandoned uploads kept, head-of-line queue, copies, log re-render | iOS | LOW | LOW |
| P7.64 | tools/pagerlink: one `conn.Write` per 244 B packet; `time.After` per loop | Tools | LOW | LOW |

---

## 5. PBKDF2 derivations per user flow (the IS.10 picture, complete)

- **Cost per derivation:** 1.9–2.7 s, measured at 240 MHz.
- **"Miss":** the salt isn't in the 6-entry LRU. That is always the case after boot or a
  lock: lock flushes the cache, and fake sleep locks after 5 min.

| Flow | Derivations | Thread | Notes |
|---|---|---|---|
| Unlock (passphrase OK) | 1 | **LVGL** | `ui_lock.cpp` `ok_event_cb` → `notes_crypto_unlock` (canary). Then the cache is flushed (P7.4). |
| Journal cold open (unlocked, encrypted index) | 1 | **LVGL** | `load_index_file`. Measured 2289 ms. |
| Journal open while locked | 2, serial | **LVGL** | ≈ 4–5 s (estimate), P7.4. |
| Journal open with FS dirty | +1 per new note + 1 index encrypt | worker | Edited notes are not re-read (P6.21); then one extra reconcile (P7.23). |
| Journal first run / no or undecryptable index | N + 1 | **LVGL** | P7.6. |
| Journal delete entry | 1 | **LVGL** | `delete_confirm_cb` → `save_index_file`. |
| Open an encrypted note | 1 foreground + 1 prewarm | **LVGL** + `notes_prewarm` | They contend (P7.5). The SPI lock is held throughout if the copy is on SD (P7.24). |
| …reopen the same note after editing | +1 | **LVGL** | The new salt was never cached (P7.1). |
| Open an encrypted note while locked | 3 | LVGL ×2 + bg | unlock + decrypt + prewarm. |
| Telegram open | 1 (2 if locked) | **LVGL** | `reload_config` → `secret_load`. Measured 1837 ms. |
| Telegram bg poll | 1 per poll on a miss | `tg_bg` | Also on the phone path (P7.8). |
| Settings › Telegram / › Notes Sync subpage | 1 on open, +1 after each URL/repo/branch edit; set token = 2 | **LVGL** | `refresh_labels` (P7.3); P7.1. |
| Notes Sync open | 1 | **LVGL** | `enter()` status label, even on the phone path (P7.3). |
| SSH open with a saved password | 1 | **LVGL** | `SshApp::onStart` hint (P7.3). |
| Set passphrase / disable | 1 + F | **LVGL** | F = FFat + SD protected copies (P7.7). |
| Change passphrase | 2F + 3 | **LVGL** | 50 notes on FFat + SD ≈ 207 derivations ≈ 6.5–9 min (estimate). |
| Encrypt SD notes | M | **LVGL** | `notes_crypto_encrypt_sd`. |
| Notes Sync run via phone | 0 | — | `run_phone_sync`. |

**Order of payoff:**
1. P7.2 shrinks every row.
2. P7.1 and P7.3 remove rows outright.
3. P6.22a/b + IS.10(b), the fixed-salt prewarm on a worker after unlock, takes the
   2289 ms and 1837 ms cold opens off the LVGL thread.
4. P7.4–P7.7 move the remaining derivations off the LVGL thread.

---

## 6. Crypto findings

### P7.1: Encryption never seeds the key cache (HIGH, LOW risk)
- **Where:** `hal/notes_crypto.cpp` `encrypt_with_pw` (both the consumed-prewarm and inline-derive branches).
- **Mechanism (verified):**
  - `keycache_store_locked` has exactly one caller, `decrypt_with_pw`.
  - An encrypt writes a fresh salt and zeroizes `key`/`iv` without caching them.
- **Consequence:** each of these pays a full derivation:
  - reopening a note just edited;
  - any journal snippet of a newly saved note;
  - the index read after a reconcile rewrote it;
  - `secret_store` followed by `refresh_labels`.
- **Fix:** when `g_unlocked && g_passphrase == pw`, call
  `keycache_store_locked(salt, key, iv)` under `PrewarmLock` before zeroizing. This is the
  same exposure as today's decrypt-side cache, and `notes_crypto_lock` still flushes it.
- **Verify:** `native_test` round trip. On hardware, confirm the hit with the profile build's `p` command.

### P7.2: Make PBKDF2 itself faster (HIGH, MED risk, security review)
- **Where:** `notes_crypto.cpp` `derive_key_iv` → `mbedtls_pkcs5_pbkdf2_hmac`.
- **Mechanism (verified from the linked library):**
  - On the S3, `libmbedcrypto.a` routes every 64-byte SHA block through `esp_sha_dma` under
    `esp_crypto_sha_aes_lock`.
  - mbedTLS PBKDF2 re-keys the HMAC every iteration: 4 compressions × 10k iterations × 2
    output blocks (48 B) = 80k hardware block ops.
  - The measured 1.9–2.7 s therefore works out to about 25–34 µs per block, mostly
    per-block setup (derived).
- **Fix:**
  - Custom PBKDF2-HMAC-SHA256 with a software compression function and precomputed
    ipad/opad midstates (2 compressions per iteration).
  - T1 and T2 are independent, so compute them in parallel on both cores.
  - Estimate: ~0.2–0.6 s (unmeasured; assumes a ~10–15 µs software block on the LX7).
  - Keep `NC_ITERATIONS`. Pin the output with RFC 7914 §11 vectors plus `openssl enc -pbkdf2` vectors in `native_test`.
- **Also:** correct the "tens of ms" comments in `notes_crypto.cpp/.h`.

### P7.3: Decrypts that only feed a status label (HIGH, LOW risk)
- **Where:**
  - `apps/ui_notes_sync.cpp` `enter()`: `load_token_plain()` is used only to pick the "unlocked/locked" text, even when the sync goes through the phone.
  - `apps/ui_ssh.cpp` `SshApp::onStart`: `secret_exists(...) && pw_load().empty()`, used only for a hint.
  - `apps/settings_telegram.cpp` and `settings_notes_sync.cpp` `refresh_labels` → `secret_mask_display` → `secret_load`. Called from `build_subpage` and after `set_url_cb`/`set_repo_cb`/`set_branch_cb`.
- **Mechanism (verified in LVGL 9.5 `lv_timer.c`):**
  - `build_subpage` runs `lv_timer_create(refresh_labels, 1)` to "let the page paint first".
  - The builder itself runs inside a timer (indev → `lv_menu` load). Creating a timer
    restarts the handler loop from the list head, where new timers sit.
  - So the 1 ms timer fires in the **same** `lv_timer_handler` pass, before
    `lv_display_refr_timer`. The page has not painted when the ~2 s decrypt runs.
  - The "menu slide-in" the comment mentions doesn't exist in LVGL 9.
- **Fix:**
  - Status text: use `secret_exists() && notes_crypto_is_unlocked()`.
  - Decrypt only where the plaintext is consumed (the sync worker, `try_saved_connect`).
  - Masked labels: show `********`, or compute the mask on a `core::OneShotJob` and set it in the done callback.

### P7.4: Unlock derives on the LVGL thread, then flushes the cache (HIGH, MED risk)
- **Where:** `ui_lock.cpp` `ok_event_cb` → `notes_crypto_unlock` (decrypts the canary, then `keycache_flush()`). Then `ui_journal.cpp` `journal_unlock_result_cb` → `journal_build_ui` → `load_index_file`.
- **Impact:** ~2 s + ~2.3 s back to back, inside one LVGL event (from IS.10 figures).
- **Fix:**
  - Run the unlock on a `OneShotJob` with an "Unlocking…" state.
  - On that worker, prewarm the fixed-salt keys right away: journal index and secret slots (IS.10 option b, P6.22a/b).
- **Risk:** security-sensitive ordering; review.

### P7.5: Prewarm and the foreground decrypt fight over the SHA engine (MED)
- **Where:** `apps/ui_text_editor.cpp` `editor_build_ui`. `notes_crypto_prewarm()` is called **before** `ui_text_editor_open_file` → `hw_read_file` → decrypt.
- **Mechanism:**
  - Both derivations run at once and are serialised block by block on `esp_crypto_sha_aes_lock`.
  - The same lock is shared with TLS (a `tg_bg` handshake).
- **Impact (estimate):** the foreground decrypt can take up to ~2×, about 4–5 s.
- **Fix:** prewarm after the read and decrypt, or move open+decrypt to a worker behind a loader.

### P7.6: Journal first-run reconcile still on the LVGL thread (MED, rare but extreme)
- **Where:** `ui_journal.cpp` `journal_build_ui`, the `show_loader` branch → `refresh_journal_entries(...)`.
- **When it runs:** the index is missing, empty, or fails to decrypt.
- **Cost:** one whole-file snippet decrypt per encrypted note (P6.25) plus an encrypted
  index save, all inside `lv_timer_handler`. 51 notes ≈ 100 s (estimate).
- **Fix:** always show the loader and run this pass on `s_bg_job`, applying the result through `bg_refresh_done`.

### P7.7: Bulk passphrase operations: one derive per copy, on the LVGL thread (MED, MED risk)
- **Where:**
  - `apps/settings_storage.cpp` `set_pw_cb`, `change_new_cb`, `disable_pw_cb`, `btn_encrypt_sd_cb`.
  - `hal/notes_crypto.cpp` `notes_crypto_encrypt_existing` / `_change_passphrase` / `_disable` / `enumerate_protected`.
- **Mechanism:**
  - The SD dual-write copy is byte-identical to the FFat copy (same salt), yet it is
    decrypted and re-encrypted separately.
  - By the time the SD pass runs, the 6-entry LRU has already evicted the FFat entries.
  - `storage_progress_cb` only yields between files.
- **Fix:**
  - Process by name: decrypt the FFat copy once; if the SD bytes match (size + CRC), write the same new ciphertext to both.
  - Probe 8 bytes before a full read (P6.27).
  - Run the loop on a worker.
- **Risk:** migration path; review crash safety and canary ordering. HW test.

### P7.8: Telegram bg poll decrypts the token before checking the phone path (LOW-MED)
- **Where:** `apps/ui_telegram.cpp` `tg_bg_task` → `load_bridge_auth`. `load_token_plain()` runs before the `hw_link_connected()` check.
- **Cost:** a 2 s derive on a miss, which also holds the shared crypto lock (P7.5).
- **Fix:** check `hw_link_connected()` and an empty URL first, and pin the token salt (P6.22b).

---

## 7. Rendering and UI-thread findings

### P7.9: Measure frame stages before any IS.7 work (MED, no risk, do first)
- **Where:** `lib/LilyGoLib/src/LV_Helper_v9.cpp` `disp_flush`; `src/core/input_profile.cpp`.
- **The gap:** the profiler times `lv_timer_handler` as one number. These stages are never separated:
  - render into PSRAM;
  - `lv_draw_sw_rgb565_swap` over PSRAM;
  - polled `SPI.writeBytes` from PSRAM.
- **Derived:** the IS.11 home frame comes to ~2.5–3.2 µs/px, far above what a 240 MHz S3 should need.
- **Emulator gdb trace** (breakpoint on `lv_inv_area`, 20 s idle on the Settings root):
  - Every invalidation came from the `setupGlobalUI` status lambda (x 430–478, y 7–23).
  - **Nothing came from Settings.**
  - So the IS.11 "Settings idle 200–336 ms" frames are one of: navigation frames
    mislabelled as idle, non-render time inside `lv_timer_handler`, or the flush waiting
    on the SPI lock. P7.9 tells which.
- **Fix (profile build only):**
  - Time the swap and `pushColors` inside `disp_flush`.
  - Add display `LV_EVENT_REFR_START` / `REFR_READY` for render time.
  - Sum invalidated pixels per frame.
  - Time each timer callback in the slow-frame census.
  - Log the CPU MHz with each slow frame.
- **Then, in order of payoff:**
  1. P6.9: render `RGB565_SWAPPED` and drop the swap.
  2. P6.10: single buffer.
  3. P4.20: internal-SRAM partial buffer of ~480×40, then DMA flush.
  4. P6.45: `-O2`/IRAM for the draw code.

### P7.10: Every editor keystroke re-measures the whole note and repaints the whole textarea (HIGH, MED risk)
- **Where:** `apps/ui_text_editor.cpp` `editor_build_ui` (one textarea holds the whole note). LVGL 9.5 `lv_label_ins_text` → `lv_label_set_text(obj, NULL)` → `lv_label_refr_text`.
- **Mechanism (verified in 9.5):**
  - `lv_label_refr_text` runs `lv_text_get_size_attributes` over the full text.
  - It resets `hint.line_start = -1`, so the next draw rescans from line 0.
  - It calls `lv_obj_invalidate(label)`, which covers nearly the whole app panel.
  - `lv_textarea_add_char` also forces a cursor `lv_obj_update_layout`.
- **Impact (estimate):**
  - 2 × O(note length) scans, about 1–5 ms for a 5 KB note.
  - A near-full-screen render per key, about 100–250 ms at the IS.11 per-pixel cost.
  - Keys aren't lost (IS.1) but appear in bursts.
- **Fix:**
  - Lower the per-pixel cost (P7.9 chain).
  - Patch `lv_label_ins_text` / `lv_label_cut_text` to invalidate from the insert line (`lv_label_get_letter_pos`) down to the bottom, so typing at the end repaints one or two lines.
  - Stopgap: cap the visible editor height.
- **Verify:** HW typing test.

### P7.11: Every Settings exit restyles and relays out the whole UI (MED, LOW risk)
- **Where:**
  - `apps/ui_settings.cpp` `ui_sys_exit` → `theme_init()` (`ui_theme.cpp`) → `build_styles()` (resets ~30 static styles) → `lv_obj_report_style_change(NULL)`.
  - Then `System::applyChromeStyle` (~15 local props).
  - Then `menu_app.cpp` `current_layout_key` → `lv_obj_update_layout`.
- **Measured** (emulator gdb, one exit, from `theme_init` to the next refresh):
  - 336 `lv_obj_refresh_style`
  - 298 `lv_label_refr_text`
  - 1,592 `lv_obj_mark_layout_as_dirty`
- **Why it's redundant:** `ui_style_set` and `ui_dark_mode_set` already call `theme_init`
  when the user changes them. Only a system-font change needs it at exit.
- **Fix:** in `theme_init`, return early when `{style, dark, get_system_font()}` match the
  last build. Call `applyChromeStyle` only on a palette change.
- **Verify:** screenshot sweep after a font change.

### P7.12: Journal scroll frames: `clip_corner`, 1.5 KB labels, animated page scroll (MED, LOW risk)
- **Where:** `apps/ui_journal.cpp` `journal_build_ui` and `post_focus_cb`.
  - Labels hold snippets of up to `kSnippetBytes` = 1536 with `LV_LABEL_LONG_WRAP`.
  - Cards have `clip_corner` on `post_cont` with `UI_RADIUS`.
  - `post_focus_cb` scrolls with `LV_ANIM_ON`.
- **Mechanism (verified in 9.5 `lv_refr.c` `lv_obj_redraw`):**
  - With `clip_corner` and radius > 0, the children are drawn **three times** per card:
    two ARGB8888 strip layers (PSRAM) plus a mask and blend, then the middle pass.
  - Each label draw scans line breaks from the top down to its clip.
  - Each encoder step is a 200–400 ms animation that invalidates the whole `lv_menu` page
    (~480×190) on every frame.
- **Fix:**
  - Drop `clip_corner` and pad by the radius (P6.18).
  - Store only what fits in the 125 px card (~300–400 B, which also helps P7.25), and load the full text on click.
  - `LV_ANIM_OFF` in `post_focus_cb` (P6.17, product call).

### P7.13: Style-dependent draw cost (LOW-MED, LOW risk)
- **Where:**
  - `ui_theme.cpp` `build_styles` / `new_theme_apply_cb`: Hotel/BeOS give every `lv_button`/dropdown a `bg_grad_dir VER` plus a 1 px shadow.
  - `ui_block`; `ui_apply_backdrop` (a 32×16 tile, ~210 tiles per full home frame in Pixel/Hotel).
  - `menu_app.cpp` `onStart` tiles: radius 14, `border_opa 30`, focus outline `opa 40`.
  - `lv_conf.h`: `LV_DRAW_SW_SHADOW_CACHE_SIZE 0`, `LV_DRAW_SW_CIRCLE_CACHE_SIZE 4`, `LV_CACHE_DEF_SIZE 0`.
- **Mechanism:**
  - Every shadow recomputes its corner mask on each draw.
  - More than 4 distinct radii are in use (14, 15, circle, 8, 4, 3), so the circle cache can thrash.
- **Fix:** measure per style first (P7.9). Then try:
  - shadow cache ~8;
  - circle cache 8;
  - opaque precomputed border colours (`lv_color_mix`) instead of `border_opa 30`.

### P7.14: Renders triggered by timers or workers run at 80 MHz (LOW-MED)
- **Where:** `factory.ino` `loop()` drops to 80 MHz once there has been no input for 2 s. Only `enqueue_event` calls `hw_cpu_boost_for_input`.
- **Affected:**
  - Telegram poll → `render_msgs`
  - Journal `bg_refresh_done` (full page rebuild)
  - Weather `gen_poll_cb` → `render_cache`
  - `ui_main.cpp` `deferred_switch_timer_cb`
- **Impact (estimate):** these renders are about 3× slower.
- **Fix:** boost in the `OneShotJob` drain callbacks, or on `LV_EVENT_INVALIDATE_AREA` in `lvgl_task`, without stamping the input time.

### P7.15: Entering fake sleep renders home at the 20 MHz sleep clock (MED-LOW, MED risk)
- **Where:** vendor `perform_fake_sleep_toggle` (`LilyGo_LoRa_Pager.cpp`); `ui_main.cpp` `ui_pause_timers`, `ui_request_editor_switch`, `deferred_switch_timer_cb`.
- **Mechanism (verified):**
  1. `ui_pause_timers()` → `hw_power_down_all()` drops the CPU to the sleep frequency and cuts the keyboard rail.
  2. `ui_request_editor_switch()` then clears `fake_sleep_active` and arms a 10 ms timer.
  3. In that window `lvgl`, `kb_reader` (polling an unpowered TCA8418) and `rotary_reader` all run as if awake.
  4. The callback runs `showMenu()`, and the full render plus flush can happen in the same pass, at 20 MHz under the instance lock.
- **Impact (estimate):** 267–336 ms at 240 MHz (IS.11) scales to about 3 s at 20 MHz. A long-press to wake during it blocks in `ui_lock()`.
- **Fix:** switch the menu and `lv_refr_now()` at full clock *before* `hw_power_down_all()`. Use a separate "entering" flag instead of clearing `fake_sleep_active`.
- **Also:** the whole enter/exit sequence runs on the vendor `rotaryTask`, which has a **2 KB stack**. Measure its watermark (P7.49).
- **Verify:** HW, 50 sleep/wake cycles including fast re-wakes.

### P7.16: Gauge sweep inside the LVGL frame (LOW-MED, LOW risk)
- **Where:** `core/system.cpp` status lambda → `hal/power.cpp` `hw_get_monitor_params` → SensorLib `GaugeBQ27220::refresh()`.
- **Mechanism:**
  - The TTL is 1 s while charging and 5 s on battery.
  - Each refresh reads 54 + 4 bytes; IS.8 measured ~14 ms.
  - The callers use only SOC, voltage and status.
  - The sweep runs under the instance lock and `ScopedKbLock`, so `kb_reader` waits for it.
  - The PB.14 premise is stale (§2).
- **Fix:**
  - Targeted reads only: 0x08–0x0B (voltage + status, 4 B) and 0x2C (SOC, 2 B).
  - Optionally refresh on a low-priority worker and have the status bar read the cache.
- **Verify:** HW check of % and the charge icon.

### P7.17: The status-bar "typing" gate rests on a reason that no longer holds (LOW-MED, functional)
- **Where:** `core/system.cpp` `setupGlobalUI` lambda: `if (core::isTextInputFocused()) return;`.
- **Mechanism:** the comment says the keyboard reader needs the instance mutex, which stopped being true with IS.1. The early return also skips:
  - the clock;
  - the Wi-Fi/BT icons;
  - the **"Unsafe to disconnect" USB MSC box**, so plugging into a PC while in the editor shows no warning.
- **Fix:** while typing, skip only the gauge sweep and the file count.

### P7.18: Chat / Notes Sync log rebuilds the whole label per line (LOW)
- **Where:** `apps/ui_chat.cpp` and `apps/ui_notes_sync.cpp` `log_append`: `lv_label_set_text` + `lv_obj_update_layout` + scroll.
- **Fix:**
  - Append with `lv_label_ins_text` at the end.
  - Trim with hysteresis (8000 → 6000).
  - Drop the forced layout (scroll to `LV_COORD_MAX` after the next layout).
- **Related:** P6.14.

### P7.19: Synchronous storage waits and SD listings inside LVGL callbacks (LOW-MED)
- **Where:**
  - `hal/storage.cpp` `wait_pending_save` (`while (s_save_pending) vTaskDelay(5)`) is reached from every reader, lister and deleter, including `tasks.txt`.
  - `apps/ui_file_browser.cpp` `load_entries` (from `folder_click_cb`, `source_click_cb` and enter) → `hw_list_sd_entries` under `ScopedSpiLock` at 4 MHz, O(n²) (P6.20).
- **Impact:** right after leaving the editor, the next screen stalls for the rest of the
  save. That can include a full PBKDF2 when the prewarm slot was consumed.
- **Fix:**
  - Wait only when the path matches, or for note listings.
  - Replace polling with a binary semaphore.
  - List on a `OneShotJob`.
  - Guarantee a primed slot (P6.22a).

### P7.20: "Copy Notes → Phone" and the hub copy run inside an LVGL event (HIGH UX, LOW risk)
- **Where:** `apps/settings_storage.cpp` `storage_copy_notes_cb`.
  - Phone path: `notes_link_copy_to_phone`, which CRCs the manifest of every internal and SD note, makes the `hw_link_call`, then waits up to 120 s per batch for `evt.notes.sync`.
  - Hub path: `hw_copy_all_notes_to_hub`, one `hw_http_request` per note (`storage_bulk.cpp`).
- **Impact:** input and rendering freeze for the whole sync, ≥120 s if the phone stalls.
  `HUB_REMOVAL_PLAN.md` recorded this as an unnumbered follow-up.
- **Fix:** run it on a `core::OneShotJob` with a progress drain, as `ui_notes_sync` and `g_fav_job` already do.

### P7.21: BLE HID `delay(7)` per report on the UI thread (LOW)
- **Where:** `hal/ble_hid.cpp` `send_report`. The media remote's `forward_key_cb` sends two reports per key, so that's 14 ms under the instance lock per key.
- **Fix:** queue reports to the keepalive task. Fold this in with P7.31.

### P7.22: Small render items (LOW)
- `settings_imu_debug.cpp` `render_tick` rewrites ~8 labels unconditionally at 5 Hz (debug page only).
- `ui_tools.cpp` `create_msgbox`: `shadow_width 18`, `opa 40`, with the shadow cache at 0, so the blur is recomputed on every nearby redraw.
- `lv_conf.h` `LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN`: LVGL's own memcpy/memset are used instead of newlib's ROM versions. Evaluate with P6.45.
- **Verified clean:**
  - No software rotation (MADCTL hardware rotation, `LV_USE_MATRIX 0`).
  - No widget on idle Settings redraws on its own (P7.9).

---

## 8. Storage findings

### P7.23: The IS.9 freshness check is off by one (MED bug, LOW risk)
- **Where:** `apps/ui_journal.cpp` `bg_refresh_done`, `expect = r.fs_gen + (r.saved_index ? 1u : 0u)`.
- **Mechanism (verified):**
  - `hal/storage.cpp` `hw_save_preferred_bytes` skips `hw_set_filesystem_dirty(true)` for
    `_idx.bin`, so an index write never bumps `filesystem_generation`.
  - After every reconcile that saves the index, `fresh` is false, so:
    - `cache_valid = false` and the dirty clear is skipped;
    - either `schedule_bg_refresh()` runs a second full reconcile, or the next page flip does.
- **Impact:** one extra FFat + SD walk per index rewrite (measured 423 ms for 51 notes; the SD part runs under the SPI lock).
- **Fix:** `expect = r.fs_gen`. Do not bump the generation from the index write, because
  that would also send `evt.fs.changed` to the phone.

### P7.24: `hw_read_file` holds the SPI lock through the decrypt (MED, LOW risk)
- **Where:** `hal/storage.cpp` `hw_read_file`.
- **Mechanism:** the `MaybeSpiLock` taken for `SD.open` lives until return, and `decode_after_read` (PBKDF2 + AES) runs before return.
- **Impact:** every other bus user stalls for ~2 s plus the 4 MHz read:
  - the display flush;
  - `note_save` and `journal_bg` SD I/O;
  - NFC;
  - the recorder's per-1 KB locked writes (risk of an I2S overrun; inferred).
- **Fix:** `lock.release()` right after `hal::fs_read_all`. Better still, land P6.24 (FFat first).

### P7.25: Journal snippets in internal DRAM, doubled by the IS.9 snapshot (MED, LOW-MED risk)
- **Where:** `apps/ui_journal.cpp` `kSnippetBytes = 1536`; `bg_refresh_timer_cb` (`seed->entries = journal_entries; // copy`).
- **Mechanism:** with `SPIRAM_MALLOC_ALWAYSINTERNAL=4096`, every 1.5 KB `std::string` goes to internal RAM.
- **Impact (estimate):** ~77 KB pinned for 51 notes, ~150 KB at the snapshot peak. That competes with TLS/Wi-Fi allocations that must be internal (P6.30).
- **Fix:**
  - Hand the worker only (name, mtime).
  - Shorter snippets (P7.12).
  - Or a PSRAM arena, or P7.48.
- **Security side note (verified):** `notes_crypto_lock()` does not clear
  `journal_entries`. Decrypted snippets of encrypted notes stay in RAM through lock and
  sleep, which contradicts the `hw_low_power_loop` comment.

### P7.26: `hw_mount_sd` never marks the card online and mounts without the SPI lock (LOW)
- **Where:** `hal/storage.cpp` `hw_mount_sd`; vendor `LilyGoLoRaPager::installSD`.
- **Callers:** `hw_prune_internal_storage`, `enumerate_protected`, `notes_crypto_encrypt_sd`, `hw_copy_internal_to_sd`.
- **Mechanism:**
  - Only `begin()` sets `HW_SD_ONLINE`, so a card inserted after boot is never seen.
  - Prune retries the mount every 10th save once there are 50 or more notes.
  - `installSD` runs `SD.begin` without `lockSPI`, which can race display flushes.
- **Fix:** a wrapper that takes `ScopedSpiLock`, calls `installSD` and updates the probe mask (needs a small vendor accessor).

### P7.27: NVS `fs/dirty` flag written twice per edit → journal cycle (LOW)
- **Where:** `storage.cpp` `hw_set_filesystem_dirty`; `ui_journal.cpp` `journal_build_ui`.
- **Mechanism:**
  - The NVS partition is only 0x5000 (5 pages), so page reclaim recurs.
  - Every flash write disables the cache on both cores.
- **Fix:** keep the flag in RAM (the generation counter already serves live observers).
  Persist it only on the clean → dirty transition at sleep or shutdown.

### P7.28: `sys.info` may scan the whole FAT on `link_task` (LOW, verify)
- **Where:** `hal/link/handlers_sys.cpp` `sys.info` → `hw_get_storage_info` → `SD.usedBytes()` under `ScopedSpiLock`.
- **Mechanism:**
  - The code path is verified.
  - Whether `f_getfree` trusts FSINFO on this card is not.
- **Impact (estimate):** possibly seconds on the first call on a large card at 4 MHz.
- **Fix:** cache the usage and refresh it on the filesystem generation.

---

## 9. PagerLink and BLE findings

### P7.29: Transfer-engine go-back-N: stall and retransmit storm (HIGH, MED risk)
- **Where:**
  - `src/hal/link/link_xfer.cpp` `XferSender::on_ack` / `pump`, `XferReceiver::on_data`.
  - Ported line for line to `tools/pagerlink/internal/pagerlink/xfer.go` and `ios/PagerKit/Sources/PagerKit/Link/Xfer.swift`.
- **Mechanism (verified in all three):**
  - The receiver ACKs every 4 messages, and re-ACKs **every** DATA at an unexpected offset.
  - The sender rewinds on **every** no-progress ACK (`if (next == acked_ && next_ > acked_) next_ = acked_;`).
  - The sender has **no retransmit timer**.
  - **Stall:** if the last DATA in flight is lost, or only one message follows the loss, no
    no-progress ACK ever arrives. The transfer waits for the idle watchdog to fail it.
  - **Storm:** each later out-of-order DATA produces a duplicate ACK, and each duplicate
    ACK triggers another rewind of up to 16 messages. The writer queues are unbounded.
- **Measured** (host simulation of the real Go engine, one dropped message; scratch copy in `/tmp/xsim`):

  | Transfer | DATA sends | Lossless baseline |
  |---|---|---|
  | 8 KB | stalls until timeout | — |
  | 64 KB | 241 | 17 |
  | 100 KB | 2,113 | 26 |
  | 256 KB, early drop | ~904,000 (effectively a livelock; ACKs keep the idle timer alive) | — |

- **Exposure:**
  - Phone → pager: `tg.*` bulk replies (failures surface as 15 s `TelegramService.bulkTimeout`) and `fs.write`.
  - Pager → phone: `fs.read` and the chat voice upload (up to 480 KB).
  - Losses are real (P7.30).
- **Fix** (wire format unchanged; land it in all three engines together, with the firmware as the oracle):
  1. The sender rewinds at most once per `acked_` value, and ignores further duplicates until there is progress.
  2. The receiver sends one re-ACK per gap.
  3. A sender retransmit timeout: after ~1–2 s with no ACK and bytes in flight, rewind and pump.
  4. Add lossy-loopback tests (drop the k-th DATA) in all three and vectors in `bulk.json`.
     Today's tests cover only the lossless case and a single unit rewind.
- **Status (2026-10-19): ✅ code in all three engines, hardware smoke test owed.** Items 1, 3 and 4 landed as written. Item 2 became "at most two ACKs per gap": a single re-ACK can read as progress to the sender. One dropped DATA now costs 6 / 29 / 42 / 81 sends for 8 / 64 / 100 / 256 KB (was stall / 241 / 2,113 / ~904,000). The negative control reproduces the storm on the old engine. Details: §16 "§G.1 boot markers and P7.29"; numbers: §15. Open: a receiver deleted at Done can't re-ACK (phone side, see §16).

### P7.30: BLE receive window ~8× the pager's packet queue (HIGH, MED risk)
- **Where:** `hal/link/link_transport_ble.cpp` `post()` / `RxCallbacks::onWrite` / `QUEUE_DEPTH`; `handlers_fs.cpp` `handle_write_begin` (`XferReceiver(..., INITIAL_CREDITS, 4)`); `link_frame.h` `INITIAL_CREDITS`.
- **Mechanism (verified):**
  - Credits count messages of up to 4092 B. 16 credits ≈ 64 KB ≈ 270 BLE packets in flight.
  - The rx queue holds 32 events, about 7.8 KB of packets.
  - `post()` runs **on the NimBLE host task** and waits up to 50 ms in `xQueueSend`. Then it drops the packet (`g_dropped`).
  - The reassembler sees `seq_gap` and discards the whole message, which triggers P7.29.
  - `link_task` is priority 1 on core 0, under `kb_reader`, `rotary`, NFC, charge and Wi-Fi.
    It also holds `g_mutex` across 4 KB FFat/SD writes (P7.34).
  - While `post()` blocks, the host task also stalls HID reports and every GATT event.
- **Fix:**
  - Size the BLE grant to the queue: 1–2 messages, `ack_every=1`, or a byte window. This is protocol-compatible because the receiver chooses the window.
  - Grow the PSRAM queue (e.g. 128 deep) and size `Event` to the MTU (P7.37).
  - Never block the host task: drop-and-count, or use a lossless ring buffer.
  - Log `g_dropped` and `link_resets` per transfer (`hw_link_get_stats`).
- **Verify:** HW upload test.
- **Status (2026-10-19): ✅ code, HW upload test owed.** Queue sized to a whole window (304 events), packets never block the host task, grant sized to the queue, drops counted in `HwLinkStats::rx_dropped`. Details: §16 "P7.30".

### P7.31: PagerLink bulk sends starve BLE HID (MED-HIGH, LOW-MED risk)
- **Where:** `hal/ble_hid.cpp` `send_report` (ignores the `notify()` result); `link_transport_ble.cpp` `send_packet`; NimBLE `MSYS_1_BLOCK_COUNT` 12 × 256 B.
- **Mechanism:**
  - HID and PagerLink share one mbuf pool.
  - `send_packet` re-polls `notify()` every 2 ms and grabs every mbuf that frees up.
  - A HID notify that hits ENOMEM is dropped. **A lost key-up leaves a key stuck repeating on the iPhone.**
- **Fix:**
  - Bounded retry (~50 ms) for HID notifies.
  - Cap PagerLink's outstanding notifies (count them via `onStatus`) so 2–3 mbufs stay free.
  - Raise `CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT` to 24–32 (about +3–5 KB internal RAM).
- **Verify:** HW, typing while a voice memo uploads.

### P7.32: Notify back-pressure is a 2 ms poll with a 200 ms budget, under `g_mutex` (MED)
- **Where:** `link_transport_ble.cpp` `send_packet` (`NOTIFY_RETRIES=100`, `NOTIFY_RETRY_MS=2`); `link_service.cpp` `service_send_bytes`; `XferSender::pump`; `Segmenter::send`.
- **Mechanism:**
  - After 200 ms (4–6 connection events) the send gives up, so the whole transfer fails.
  - The first window (~270 notifies, ~2–3 s estimated) is pumped on the *caller's* worker while holding `g_mutex`. That blocks `link_task` from draining rx (feeding P7.30) and blocks every other `service_call`.
- **Fix:**
  - Event-driven flow control: a semaphore given from `onStatus`, or a counter of mbufs in flight.
  - A stall timeout of seconds, tied to the supervision timeout.
  - Pump only on `link_task`, and never wait for radio space while holding `g_mutex`.

### P7.33: No transfer connection parameters, stale MTU, no DLE/2M (MED, MED risk)
- **Where:**
  - `hal/ble_stack.cpp` `apply_idle_conn_params` (interval 0x18–0x28, latency 10), applied by `wireless.cpp` `ble_kb_keepalive_task` whenever HID is subscribed.
  - `link_transport_ble.cpp` `companion_up`, where `getPeerMTU` is read once.
  - There is no `onMTUChange`, no `setDataLen` and no PHY call.
- **Mechanism:**
  - `IOS_COMPANION_PLAN.md` §5.1 item 7 (15–30 ms with latency 0 during a transfer, back to idle 2 s after) is not implemented.
  - Phone → pager traffic can wait up to ~10 skipped events, about 0.5 s (estimate; controller behaviour unverified).
  - If CCCD restore beats the MTU exchange, the session runs at 20 B packets until reconnect.
- **Impact (estimate):**
  - The 480 KB voice upload takes ~12–24 s, against ~5–8 s at 15 ms.
  - begin (10 s) + upload + `chat.ask` (60 s) can exceed `ui_chat.cpp` `PHONE_WATCHDOG_MS` = 75 s.
- **Fix:**
  - Call `updateConnParams(conn, 12, 24, 0, timeout)` when a transfer starts, and restore idle 2 s after the last DATA.
  - Handle `onMTUChange` by updating `State::packet_size`.
  - Call `setDataLen(conn, 251)` and request 2M PHY in `companion_up`.
  - Size `PHONE_WATCHDOG_MS` to begin + upload + ask.
- **Verify:** HW throughput measurement (S3). Keep the iOS HID-compliant idle band.

### P7.34: Link file I/O: per-chunk reopen, whole-file CRC passes, notes read ~3× (MED, MED risk)
- **Where:** `hal/link/fs_volume.cpp` `vol_read_at` / `vol_write_at`; `handlers_fs.cpp` `file_crc`, `handle_read`, `handle_write_commit`; `hal/notes_link.cpp` `scan`.
- **Mechanism:**
  - Every 4092 B chunk does: open (VFS `stat` ×2 + `fopen` + a 4 KB stdio buffer), `seek`, read or write, close.
  - Writes use `FILE_APPEND` and close per chunk, so FatFS syncs the FAT and the directory entry every chunk. On FFat that is an extra wear-levelled 4 KB sector rewrite each time.
  - `handle_read` CRCs the whole file before the first DATA.
  - Commit re-reads the whole `.part`.
  - `notes_link` `scan` CRCs every note on every sync, and the phone's `fs.read` CRCs and streams each needed note again.
  - All of it runs on `link_task` under `g_mutex`; the SD chunks also hold the SPI lock at 4 MHz (P6.19).
- **Impact (estimate):**
  - 2–3× the needed FS operations on reads, and 2–3 extra sector writes per 4 KB on uploads.
  - ~12–25 ms of SPI hold per SD chunk.
  - ~2–3 s to first byte on a 1 MB SD `fs.read`.
- **Fix:**
  - Keep one open `File` per `Transfer` (take `VolLock` per chunk).
  - Accumulate the CRC inside `XferReceiver`.
  - Cache CRCs by (vol, path, size, generation).

### P7.35: Companion-only connection: wrong sleep clock, no idle params, tick runs in sleep (MED, MED risk)
- **Where:** `hal/system.cpp` `hw_fake_sleep_target_freq`; `hal/ble_hid.cpp` `connected()` ("HID subscribed"); `wireless.cpp` `ble_kb_keepalive_task`; `link_transport_ble.cpp` `link_task` (`TICK_MS=1000`); `link_service.cpp` `service_tick`.
- **Mechanism (verified):**
  - Since `f71fd9e`, a phone connected only through PagerLink makes `hw_get_ble_kb_connected()` false. Fake sleep then drops to **20 MHz** with a BLE link up, breaking the "≥ 80 MHz while a link is up" rule the code itself states. The same applies to BT advertising while unconnected, and to Wi-Fi enabled but not associated.
  - `apply_idle_conn_params` runs only for HID peers.
  - `link_task` never parks. Every second it runs `Rpc::tick`, `fs_tick`, stall scans and `hw_get_monitor_params` under `g_mutex`, the last one with an I2C sweep every 5 s.
- **Fix:**
  - Base the frequency floor on `hal::ble::connected_count() > 0 || hal::ble::enabled()` (and `wifi_enable`).
  - Apply the idle params in `companion_up` too.
  - Derive the queue timeout from the nearest RPC or transfer deadline, ~10 s when idle or asleep.
  - Drive `evt.fs.changed` from the generation bump.
  - Read the battery outside `g_mutex` (the IS.8 residual).
- **Also unguarded:** `sys.time.set` → `hw_set_date_time` (RTC I2C) runs on `link_task` without the kb lock (P7.45).
- **Verify:** HW soak, phone app connected without the keyboard, 30 min of fake sleep.

### P7.36: Link buffers are all internal RAM, and the copy chain is long (MED, LOW risk)
- **Where:** `hal/link/link_frame.cpp` (`Reassembler::partial_`, `decode_message`, `encode_message`, `Segmenter::send`'s per-packet `std::string`); `link_xfer.cpp` `XferSender::pump`; `link_json.cpp` `JsonObject::parse`, `json_array_items`.
- **Mechanism:**
  - Payload and chunk buffers are ≤ 4096 B (`MAX_PAYLOAD` 4096, `MAX_DATA_CHUNK` 4092), the internal-first threshold, so they go internal. Only the 4102 B encoded message lands in PSRAM.
  - **Outbound,** each byte is copied 4 times (read buf → DATA payload → message → packet string), with one heap allocation per ~245 B packet. That is ~2,000 small internal allocations per voice memo.
  - **Inbound,** `decode_message` copies again, and `json_array_items` copies the whole body about 3× plus once per item.
  - Only the BLE event queue is explicitly PSRAM.
- **Impact (estimate):**
  - CPU cost is small.
  - The risk is internal-heap fragmentation while TLS needs ~16.7 KB contiguous (P6.30). The S4 soak that was meant to measure this was never recorded.
- **Fix:**
  - Reused member buffers or a PSRAM `heap_caps_malloc`.
  - A stack `uint8_t pkt[MAX_PACKET]` in `Segmenter::send`.
  - `pump` reads straight into the `encode_data` output.
  - `decode_message` returns a view.
  - Or the global lever, P7.48.

### P7.37: Oversized BLE link constants (LOW-MED, LOW risk)
- **Where:** `link_transport_ble.cpp` `TASK_STACK = 8192` (the plan says 6 KB), `MAX_PACKET = 512`, the `Event` struct, `post`/`link_task` `static Event ev`, `create_queue`; `ble_stack.cpp` `setMTU(247)`.
- **Mechanism:**
  - Packets can never exceed 244 B, yet each `Event` is 518 B: the queue takes ~16.6 KB of PSRAM, and the two statics take 1,036 B of internal BSS.
  - NimBLE is never deinitialised, so the link task (8 KB), NimBLE host (4 KB) and `ble_kb_ka` (3 KB) stacks stay resident once BT has been on.
  - `MAX_CONNECTIONS` is 3 for one phone.
- **Fix:**
  - Derive `MAX_PACKET` from the MTU (247 − 3), with a `static_assert` tying it to `setMTU`.
  - Measure the link-task watermark (P7.49).
  - `-D CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`, after checking the `ble_hid` `MAX_PEERS` logic.

### P7.38: CRC-32 table in DRAM, with a lazy-init race (LOW)
- **Where:** `link_xfer.cpp` `crc32_update`, `static uint32_t table[256]`, built lazily with no synchronisation.
- **Fix:**
  - On `ARDUINO`, use `esp_rom_crc32_le`; confirm the init/xor convention against the vectors.
  - Use a `constexpr` rodata table for `native_test`.

---

## 10. Network findings

### P7.39: Weather tries direct first with Wi-Fi but no internet (LOW)
- **Where:** `apps/ui_weather.cpp` `weather_fetch` → `direct_fetch`. `try_direct` is gated only on Wi-Fi association.
- **Impact:** +5 s (connect timeout) to +120 s (handshake timeout, P6.37) before "via phone".
- **Fix:**
  - Gate direct on a cached internet verdict, as Telegram's `s_inet_ok` does.
  - Go phone-first when the link is up and the verdict is unknown.
  - Land P6.37.

### P7.40: Network probes whose results are never used (LOW)
- **Telegram:** `apps/ui_telegram.cpp` `tg_uplink` → `internet_available` → `kick_inet_probe` refreshes even when `hw_link_connected()`, though the phone always wins. Each probe is a task plus a TCP connect to 1.1.1.1:53 every 30 s.
- **Hub:** `core/system.cpp` spawns a 3 KB `hub_probe` task every 30 s, even with Wi-Fi down. `hub_last_reachable` has no reader any more, and the task writes a `static bool` across tasks with no atomic.
- **Fix:**
  - Skip the inet probe when the link is up.
  - Probe the hub only on GOT_IP, a settings change or wake (or drop it with H4).
  - Make the shared flag `std::atomic`.

### P7.41: Hub chat budget mismatch (LOW-MED, moot at W16)
- **Where:** `apps/ui_chat.cpp` `chat_post` (15 s deadline, `client.read()` one byte at a time, `vTaskDelay(10)`), `SEND_WATCHDOG_MS` 20 s. Server: `chat.go` `clientTimeout` 60 s × 3 attempts.
- **Impact:** voice turns slower than 15 s fail on the device while the server finishes anyway, so a user retry pays twice.
- **Fix:**
  - Device budget ~70 s and a watchdog just above it.
  - Buffered `read(buf, n)` and wait on `available()`.

### P7.42: HTTP bodies held twice; chunked bodies grow by realloc (LOW)
- **Where:** `hal/wireless.cpp` `hw_http_request` / `hw_http_get_string`: `String rbody = c.http.getString(); out.append(...)`.
- **Mechanism (framework 2.0.17):**
  - With chunked transfer, `StreamString::write` reallocs to the exact size once per 1,460 B read.
  - The body is then copied into the caller's `std::string`, so the peak is about 2× the body.
- **Fix:** stream into the `std::string` through a small `Print` adapter (`reserve(getSize())`, geometric growth) with `writeToStream`.

### P7.43: SSH read loop (LOW; extends P6.14 / P3.9)
- **Where:** `apps/ui_ssh.cpp` `LibSshBackend` session loop: `char buf[256]`, two `ssh_channel_read_nonblocking`, `vTaskDelay(50)`. The UI `poll_tick_cb` runs every 100 ms.
- **Impact:**
  - Throughput tops out around 10 KB/s.
  - Echo latency up to ~150 ms.
  - 20 Hz wakes while idle.
  - The "TEMPORARY" `[stackwm]` `Serial.printf` fires every 3 s in production.
- **Fix:**
  - `ssh_channel_poll_timeout` / `ssh_event_dopoll`, plus a task notify on input.
  - A 2–4 KB buffer, drained until empty.
  - Remove the watermark print.

---

## 11. Boot, bus and cadence findings

### P7.44: The ST7796 init table sleeps 600 ms (MED-HIGH, LOW-MED risk, vendor patch)
- **Where:** `lib/LilyGoLib/src/LilyGo_LoRa_Pager.cpp` `st7796_init_list`, run by `LilyGoDispInterface.cpp` `LilyGoDispArduinoSPI::init` (`if (t[i].len & 0x80) delay(120);`).
- **Mechanism (verified):** five entries carry the 0x80 flag, so 5 × 120 ms = **600 ms** inside `instance.begin()`, before LVGL starts:

  | Entry | What it is | Delay needed? |
  |---|---|---|
  | `0x01` SWRESET | reset | yes, keep |
  | `0x11` SLPOUT | sleep out | ~5 ms is the usual need before the next command (the 120 ms rule applies before SLPIN); check the datasheet |
  | `0xC5` VCOM (`0x81`) | plain register write | no |
  | `0xE1` gamma (`0x8F`) | plain register write | no |
  | `0xF0 0x69` command lock (`0x81`) | plain register write | no |

- **Impact:** −360 ms boot (calculated). Up to −475 ms if SLPOUT drops to 5–10 ms (estimate; datasheet check needed).
- **Fix:** clear 0x80 on the three register writes, marked `[LOCAL PATCH]`. Keep SWRESET.
- **Verify:** HW, 20 cold boots; check for glitches, colours and gamma.

**Boot budget before the first frame.** None of this is measured; add the §G.1 markers from `OPTIMIZATION_PHASE6.md` first.

| Step | Time | Basis |
|---|---|---|
| PSRAM self-test (P6.3) | 0.3–0.5 s | estimate |
| BHI260 upload (P6.2) | 1–2 s | estimate |
| 32 kHz crystal calibration (P6.1) | 0.80 s | calculated from code |
| ST7796 init delays (P7.44) | 0.60 s | calculated from code |
| FFat tree listing (P6.4) | grows with notes (51-note walk = 423 ms in IS.9) | estimate |
| SD mount at 4 MHz, LoRa begin, home build, first render | — | not measured |

### P7.45: The I2C bus is not locked bus-wide; read-modify-write expander writes are unsafe (MED-HIGH, MED risk)
- **Where:** the claim in `core/kb_lock.h`. Unguarded sites:
  - **Read-modify-write writes:**
    - `instance.powerControl()` → XL9555 `digitalWrite` → SensorLib `setRegisterBit`/`clrRegisterBit`. Callers: `hw_power_down_all`/`hw_power_up_all`, `hw_set_haptic_enable`, `hw_start/stop_nfc_discovery`, `hw_set_gps_powered`, the audio paths and the codec PA callback.
    - XPowers `enableCharge`/`disableCharge`.
  - **Reads:**
    - `hw_get_charge_enable()` from `hw_update_battery_history` (LVGL, 60 s, outside the kb lock).
    - `factory.ino` `loop()` → `instance.loop()` → `sensor.update()`.
    - Boot `hw_connectivity_init` NFC `powerControl` while `kb_reader` is already running.
    - `sys.time.set` RTC write (P7.35).
- **Mechanism (verified):**
  - A short read makes SensorLib `readRegister` return −1.
  - `setRegisterBit` stores that in a `uint8_t` unchecked, then writes 0xFF|bit. That **drives every other output on the port high**: the AMP, LoRa, GPS and NFC rails.
  - The colliding reader is `kb_reader` (priority MAX-2, core 0). Its 100 ms idle fallback does two I2C reads.
- **Impact:** rare, but silent. Rails stay powered (mA drain) or the charger state flips until the next write. Each collision also blocks for the 20 ms Wire timeout.
- **Fix:**
  - Promote `ScopedKbLock` to a bus-wide `core::ScopedI2cLock` (the same recursive mutex).
  - Take it in `hal::` wrappers around `powerControl`, the charger get/set, `hw_set_charger_current`, codec open/close/volume, the RTC writes, the sensor paths, and `instance.loop()`.
  - Fix the `kb_lock.h` comment.
  - Raise `keyboard_task.cpp` `kFallbackMs` from 100 to ~500 ms (the ISR is the real wake path).
  - Lock order: instance → i2c only.
- **Verify:** HW soak with Wi-Fi, NFC, charging, audio and sleep/wake.

### P7.46: Radio fully reconfigured on wake even when disabled (LOW)
- **Where:** `hal/system.cpp` `hw_power_up_all` → `hw_set_radio_enable` → `hal/radio/sx1262.cpp` `configure`. `radio_enable` defaults to 0.
- **Mechanism:** each wake still runs standby + `setFrequency` (image calibration) + modulation setup, then sleep. About 5–20 ms per wake (estimate).
- **Fix:** skip the reconfigure when `!user_setting.radio_enable`; the chip is already asleep.

### P7.47: Worker results polled at 10 Hz (LOW)
- **Where:** `core/one_shot_job.h` `drain_tick`; `apps/ui_weather.cpp` `gen_poll_cb`.
- **Impact:** up to 100 ms of completion latency. It becomes a real wake source once P6.6 lands.
- **Fix:** the job trampoline calls `hw_lvgl_task_notify_wake()` + `lv_timer_ready(timer_)`. Also give `notify::post` the wake (P6.18).

---

## 12. Memory and build findings

### P7.48: Lower the internal-first allocation threshold (MED, MED risk, HW soak)
- **Where:** `hal/system.cpp` `hw_init()`. The framework sets the value in `esp32-hal-psram.c` via `heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL)` = 4096.
- **Mechanism:**
  - Calling `heap_caps_malloc_extmem_enable(1024)` (or 512) after PSRAM init sends every general 0.5–4 KB `malloc`/`new` to PSRAM. That covers P7.25, P7.36, P7.50, `std::function` blobs and `String` growth.
  - Unaffected: explicit `MALLOC_CAP_INTERNAL/DMA`, FreeRTOS stacks and queues, and mbedTLS internal calloc.
- **Impact (estimate):** several to tens of KB of peak internal pressure removed under Wi-Fi + BLE + TLS.
- **Before landing:** audit for any plain `malloc` buffer handed to DMA, used from an ISR, or used with the cache disabled. Bench with P7.49 telemetry before and after.

### P7.49: No heap or stack telemetry (MED, LOW risk, unblocks the rest)
- **Where:** `hal/system.cpp` `hw_get_heap_info` returns only `ESP.getHeapSize()`/`getFreeHeap()`. There are no callers of `heap_caps_get_minimum_free_size`, `heap_caps_get_largest_free_block`, `getMinFreeHeap` or `getMaxAllocHeap` in `src/`.
- **Impact:** P6.30, P3.24, P7.36, P7.48, P3.9, P3.14, P4.26, P7.15 (rotary 2 KB stack) and P7.37 all stay estimates. Fragmentation is invisible.
- **Fix:**
  - Internal min-free and largest-block on the Info page and in the profile build's `[prof]` report.
  - A `uxTaskGetStackHighWaterMark` sweep of named tasks in the profile build.

### P7.50: Telegram JSON moved off the PSRAM-hooked cJSON (MED, LOW-MED risk)
- **Where:** `apps/ui_telegram.cpp` `fetch_chats`, `tg_parse_msgs` → `hal/link/link_json.cpp` `json_array_items`, `JsonObject::parse`/`get_object` (since `8af40f4`, H2.6).
- **Mechanism:**
  - The body is copied ~4×, and each element is reparsed. Per-item strings and retained `Message` fields land in internal RAM.
  - This partially undoes P2.4.
- **Impact (estimate):** 10–20 KB transient and 5–15 KB long-lived internal RAM per 20-message page.
- **Fix:** P7.48 short term. `json_array_items` returning `(offset, len)` spans, and `JsonObject` over a borrowed view.

### P7.51: Montserrat 48 costs 96.7 KB of flash (LOW-MED, LOW risk)
- **Where:** `lv_conf.h` `LV_FONT_MONTSERRAT_48 1`.
- **Uses:**
  - `ui_audio_notes.cpp` `rec_time_lbl` (digits and `:`).
  - `hal/charge_task.cpp` (`LV_SYMBOL_CHARGE`, `%d%%`).
  - `menu_app.cpp` `pick_icon_font` (only when `tile_h >= 140`).
- **Fix:**
  - Generate a `lv_font_conv` subset (digits, `:%- `, the few symbols used): ~5–10 KB, saving ~85–90 KB (estimate).
  - Set `LV_FONT_MONTSERRAT_48 0` and keep the emulator define in sync (the P3.23 lesson).
  - Sizes 22–32 are user-selectable, so they stay.
  - This mainly buys headroom for P6.42 / P3.27 (a 3 MiB slot).

### P7.52: `-fexceptions` for `src/` (LOW, MED risk)
- **Mechanism:**
  - `.eh_frame` is 105 K (`src/` 40 K) and `.gcc_except_table` is 16 K (`src/` 13 K).
  - `src/` has only two `try` blocks: `ui_notes_sync.cpp` `start_sync` and `ui_chat.cpp` `spawn_send_task`, both `catch (...)`.
- **Fix:**
  - Replace both with `std::nothrow` and explicit checks.
  - Add `-fno-exceptions` to `build_src_flags` only, which saves ~40–50 KB (estimate).
  - Compare the map before and after.
- **Risk:** any throwing template instantiated in `src/` becomes an abort.

### P7.53: Four unused LVGL widgets (LOW, LOW risk)
- **Where:** `lv_conf.h` `LV_USE_TABLE`, `LV_USE_TABVIEW`, `LV_USE_KEYBOARD`, `LV_USE_BUTTONMATRIX` (buttonmatrix only after keyboard is off).
- **Mechanism:** none is referenced in `src/`. They cost 10.3 KB of linked flash (measured per object).
- **Fix:** turn them off with the P3.17 procedure and bundle with P6.44 for ~18–25 KB (estimate).

### P7.54: An LVGL allocation failure hangs core 1 silently (LOW, LOW risk)
- **Where:** `lv_conf.h` `LV_USE_ASSERT_MALLOC 1`, `LV_ASSERT_HANDLER while(1);`. The LVGL task runs on core 1, and the task watchdog checks only CPU0's idle task.
- **Fix:** `LV_ASSERT_HANDLER abort();`, so the device writes a coredump to flash and reboots.

---

## 13. Companion findings (iOS PagerKit, iOS app, server, tools)

Transfer-engine flow control (P7.29) is shared with the firmware and listed in §9.

### P7.55: TDLib client decodes every update and sends each to the main actor (MED, LOW risk)
- **Where:** `ios/App/Telegram/TDJSONClient.swift` `receiveLoop`; `TelegramAccount.init` `onUpdate` → `handle`.
- **Mechanism:**
  - Every `td_receive` result goes `String` → `Array(utf8)` → `JSONValue.decode`.
  - Every update without `@extra` becomes a `Task { @MainActor … }`.
  - `handle` keeps only `updateAuthorizationState`.
  - The client starts at app launch, with or without a pager.
- **Measured:** `JSONValue.decode` takes 115 µs per KB against 19 µs for `JSONSerialization` (Swift 6.1 release on Linux; Darwin will differ).
- **Fix:** decode only replies with `"@extra"` and updates starting with
  `{"@type":"updateAuthorizationState"`. Drop the rest before they reach the main actor.

### P7.56: `JSONValue` encode/decode cost on every RPC (MED, LOW risk)
- **Where:** `Model/JSONValue.swift` `init(from:)` and `encoded()`.
  - `init(from:)` is a `try?` cascade Bool → Double → String → array → object, so up to 4 thrown errors per node.
  - `encoded()` builds a new `JSONEncoder` with `.sortedKeys` on every call (58 µs/KB).
- **Hot paths:** `LinkSession` `handleResponse`/`handleEvent`/`handleRequest`; `NotesSyncService.existingNames`; TDLib and Groq replies.
- **Double encode:** `TelegramService.reply` encodes once to measure, then `handleRequest` encodes again.
- **Fix:**
  - Build from `JSONSerialization` output.
  - Keep `.sortedKeys` only where the golden vectors need it.
  - Let `reply` hand back the bytes it already encoded.

### P7.57: Notes sync: one repo listing and one commit per manifest batch (MED; LOW / MED risk)
- **Where:** `ios/PagerKit/.../Services/NotesSyncService.swift` `run`; firmware `hal/notes_link.cpp` `notes_link_sync`, `notes_manifest_batches` (≤ 4096 B per batch, ~50 entries).
- **Impact (estimate):** 500 notes means 10 Contents listings of ~350 KB and 10 commits instead of 1. That is +10–30 s and a noisy git history.
- **Fix:**
  - A short-TTL (~2 min) remote-name cache across jobs: LOW risk.
  - Coalesce commits, debounced or via a `more`/`last` flag in `notes.sync`: protocol change, MED risk.

### P7.58: GitHub Contents listing stops at 1,000 files (LOW, rising to MED past 1,000 notes)
- **Where:** `NotesSyncService.swift` `GitHubNotesBackend.existingNames`; `server/internal/notessync/notessync.go` `listRemote` (live until W16).
- **Mechanism:** notes past the cap look missing, so every sync pulls them again over BLE, re-uploads the blobs and makes a new commit.
- **Fix:** use the Git Trees API on the `notes` tree, and check `truncated`.

### P7.59: Chat deadlines don't line up across phone and pager (LOW-MED, LOW risk)
- **Where:** `ios/PagerKit/.../Services/ChatService.swift` `GroqChatBackend` (`requestTimeout` 60 s × `maxAttempts` 3, for both STT and LLM); firmware `chat_link.cpp` `CHAT_ASK_TIMEOUT_MS` 60 s and `ui_chat.cpp` `PHONE_WATCHDOG_MS` 75 s.
- **Mechanism:** after the pager gives up, the phone retries for up to ~6 min. It spends Groq quota and stores a reply the pager never showed, so the histories drift.
- **Fix:** one ~55 s deadline per `ask`, each attempt using the remaining time. Size the pager watchdog to begin + upload + ask (P7.33).

### P7.60: iOS weather has no cache and fetches 7 days of hourly data (LOW)
- **Where:** `Services/WeatherService.swift` `payload`, `geocode`, `forecastQuery`.
- **Fix:**
  - Cache the geocode per key.
  - Cache the forecast for ~10 min.
  - Add `forecast_hours=24`. The firmware direct path should get the same (P6.35).

### P7.61: iOS Telegram chat list rebuilt on every poll (LOW)
- **Where:** `Services/TDTelegram.swift` `chats(limit:)`. This file has uncommitted local edits; it was only read.
- **Mechanism:** up to 30 sequential `getChat` calls per `tg.chats`, and serial `getUser` in `messages`. About 30 ms CPU per poll (estimate).
- **Fix:** maintain the list from TDLib updates once P7.55's filter forwards them. Otherwise run the calls in a task group.

### P7.62: iOS `LocationProvider`: reverse geocode every request; "auto" can exceed the pager's 15 s (MED-LOW)
- **Where:** `ios/App/Session/LocationProvider.swift` `place()`, `fix()`.
- **Mechanism:**
  - The fix is cached for 15 min, but `CLGeocoder().reverseGeocodeLocation` runs on every call.
  - Time budget against the pager's `WEATHER_GET_TIMEOUT_MS` of 15 s:
    - normal path: fix up to 8 s + reverse geocode (no timeout) + forecast 5 s;
    - fix-failure path: 8 s + IP lookup 5 s + forecast 5 s = 18 s.
  - The 8 s timeout task is never cancelled, so a stale timer can fail a later request.
- **Fix:**
  - Cache the city together with the fix.
  - Cap the locator at ~4 s and put a timeout on the geocode.
  - Tie each timer to its own request.

### P7.63: iOS small items (LOW)
- `LinkSession.handleRequest` / `sendMessage` drop a reply over 4096 B (`try?`), so the pager waits out a 10–15 s timeout. Send an `internal` "reply too large" error instead.
- `ChatService.uploads` keeps abandoned WAVs for the service's lifetime. Expire them, or keep at most 1–2.
- `NotesSyncService.deliver` (latent "phone" target) re-pulls files the phone already has. Compare size and CRC32 first.
- `LinkSession` has one unbounded writer queue, so up to 64 KB of DATA can sit ahead of a reply or ACK (~1–2 s on BLE, estimate). Add a control-message priority lane.
- ~5 copies per byte on send; `Inbound.data` is not reserved. Add `reserveCapacity(size)` in `arm()`.
- `PagerSession.appendLog` re-renders the Dashboard and Diagnostics views:
  - a new `DateComponentsFormatter` on each render;
  - index-identified rows (`ForEach(indices, id: \.self)`) that all shift once the 200-line cap rotates.

  Fix: a separate log store, a static formatter and stable IDs.

### P7.64: `tools/pagerlink` (LOW)
- `client.go` `writeRecord`: one allocation and one `conn.Write` per 244 B packet. Use a `bufio.Writer` flushed per message.
- `responder.go` `AcceptBytes`, `files.go` and `Client.Call`: a new `time.After` per loop iteration. Under `go 1.22`, each timer stays alive until it fires. Use one `time.NewTimer` + `Reset`.

### Checked and clean
- **Firmware:**
  - LoRa: no polling, and disabled by default.
  - Phone requests do no LVGL-thread work.
  - `hw_link_connected` is lock-free.
  - NTP has backoff.
  - The Wi-Fi scan is async.
  - Advertising stops while connected.
  - Wi-Fi uses dynamic buffers.
  - There is no duplicate CA bundle.
  - Rotation is done in hardware.
- **iOS:**
  - BLE write pacing (`canSendWriteWithoutResponse`).
  - `pendingWrites` holds at most one entry.
  - No polling timers apart from the reconnect backoff.
- **Server:**
  - Shared default transport.
  - Bodies closed and limited.
  - No goroutine leaks.
  - `-race` clean.

---

## 14. Order of work

Each step is gated by the build discipline in the header. Append measurements to §15 and
never overwrite rows.

**Phase A: measure, no behaviour change (unblocks everything)**
1. **P7.49** heap and stack telemetry.
2. **P7.9** per-stage frame timing.
3. §G.1 boot markers (`OPTIMIZATION_PHASE6.md`).

   One `tlora_pager_profile` hardware session then fills the boot, frame and heap rows.

**Phase B: cheap, low-risk fixes with big wins**
1. **P7.1** encrypt seeds the key cache.
2. **P7.3** status-only decrypts.
3. **P7.23** journal freshness off-by-one.
4. **P7.24** release the SPI lock before decrypt.
5. **P7.8** Telegram token before link check.
6. **P7.20** Copy Notes → Phone on a worker.
7. **P7.17** typing gate.
8. **P7.11** `theme_init` early-out.
9. **P7.40** probes.
10. **P7.39** weather gate.
11. **P7.54** assert handler.
12. **P7.43** remove the prod stack print.

**Phase C: transfer reliability** (needs `make protocol-test` plus hardware)
1. **P7.29** engine fix in all three implementations, with lossy tests. ✅ code 2026-10-19 (§16); HW smoke test owed.
2. **P7.30** BLE grant and queue. ✅ code 2026-10-19 (§16); HW upload test owed.
3. **P7.32** event-driven notify.
4. **P7.31** HID starvation.
5. **P7.33** connection params, MTU change, DLE.
6. **P7.34** file I/O.

**Phase D: crypto off the UI thread** (security review)
1. **P7.2** fast PBKDF2 with vectors.
2. **P7.4** unlock on a worker, plus fixed-salt prewarm (P6.22a/b).
3. **P7.5**, **P7.6**, **P7.7**.

**Phase E: render** (only what the Phase A numbers flag)
1. P6.9.
2. P6.10.
3. **P7.10** editor invalidation.
4. **P7.12** journal.
5. P4.20 partial internal buffer, then DMA.
6. **P7.13** caches.
7. P6.12.
8. **P7.14** / **P7.15** clock.

**Phase F: boot** (hardware, vendor patches)
1. **P7.44** display delays.
2. P6.2 BHI260 deferred.
3. P6.1 crystal.
4. P6.3 PSRAM test.
5. P6.4 MSC listing.
6. P6.19 SD clock (needs the CRC read-back test from IS.2).

**Phase G: bus and power** (hardware soak)
1. **P7.45** bus-wide I2C lock.
2. **P7.35** companion-only floor and tick.
3. **P7.16** targeted gauge reads.
4. **P7.46**, **P7.47**.

**Phase H: memory and flash**
1. **P7.48** threshold (after P7.49 baselines).
2. **P7.36** link buffers.
3. **P7.50** JSON spans.
4. **P7.25**, **P7.37**, **P7.38**.
5. **P7.51**, **P7.53** with P6.44.
6. **P7.52**.

**Phase I: companions**
1. **P7.55**, **P7.56**, **P7.57**, **P7.62**, **P7.59**.
2. Then the LOW items.
3. Server items only if W16/W22 slip.

### Product calls needed (not engineering decisions)
- P6.17: keep the animated scroll-on-focus?
- P7.12: how many snippet lines should a journal card show?
- P6.43: is LibSSH worth 165 KB of flash and 10 KB of DRAM?
- P6.42 / P3.27: will there ever be OTA? If not, reclaim the 4 MiB slot.
- P7.57: is a protocol flag for commit coalescing acceptable?

### Hardware smoke tests added by this plan
Add these to the `OPTIMIZATION_PROGRESS.md` checklist:
- 20 cold boots after P7.44: no panel glitch, colours and gamma correct.
- Type in Notes while a voice memo uploads to the phone: no dropped or stuck keys (P7.31).
- Upload 256 KB to the phone with induced loss: completes, and the send count is ≤ 2× lossless (P7.29/P7.30).
- Phone connected without the keyboard, 30 min of fake sleep: the link survives and the wake count drops (P7.35).
- 50 sleep/wake cycles including fast re-wake: no hang in `ui_lock` (P7.15).
- I2C soak with Wi-Fi, NFC, charging, audio and sleep/wake: no stray rails, measured with a meter (P7.45).
- Edit a note → reopen: no ~2 s freeze (P7.1). Settings › Telegram opens painted first (P7.3).

---

## 15. Measurements log

_Append-only. One row per run: date, commit, scenario, what was measured, result._

| Date | Commit | Scenario | Measured | Result |
|---|---|---|---|---|
| 2026-10-19 | c9c1a96 | `pio run -e tlora_pager` | image / static DRAM | 2,864,352 B (68.3 %) / 94,600 B |
| 2026-10-19 | c9c1a96 | emulator, gdb on `lv_inv_area`, Settings root idle 20 s | invalidation sources | only status-bar labels (~44×17 px); nothing from Settings |
| 2026-10-19 | c9c1a96 | emulator, gdb, one Settings exit | `lv_obj_refresh_style` / `lv_label_refr_text` / `mark_layout_as_dirty` | 336 / 298 / 1,592 |
| 2026-10-19 | c9c1a96 | emulator, full screenshot sweep | `theme_apply` / `new_theme_apply_cb` / `cursor_blink_anim_cb` | 4 / 859 / 0 |
| 2026-10-19 | c9c1a96 | host sim of `xfer.go`, one dropped DATA | DATA sends: 8 / 64 / 100 / 256 KB | stall / 241 (vs 17) / 2,113 (vs 26) / ~904,000 |
| 2026-10-19 | — | Swift 6.1 release (Linux), 1 KB TDLib update | `JSONValue.decode` vs `JSONSerialization` | 115 µs vs 19 µs |
| 2026-10-19 | c9c1a96 + §16 working tree | `pio run -e tlora_pager` | image / static DRAM | 2,869,104 B (68.4 %, +4,752) / 94,712 B (33,584 + 61,128, +112) |
| 2026-10-19 | c9c1a96 + §16 (P7.29) | Go engine, lossy harness, one dropped DATA (interleaved delivery unless noted) | DATA sends / ACKs / timer firings: 8 KB last DATA (batched) · 64 KB #5 · 100 KB #5 · 256 KB #0 · 256 KB #5 | 6/3/1 · 29/7/0 · 42/9/0 · 81/18/0 · 81/19/0 (was stall · 241 · 2,113 · ~904,000) |
| 2026-10-19 | c9c1a96 + §16 (P7.29) | negative control: new Go tests against the `c9c1a96` `xfer.go` | interleaved 100 KB #5 / 256 KB #0 | 1,285 sends (limit 52) / livelock: 1,117,506 sends in 200k steps |
| 2026-10-19 | c9c1a96 + §16 (P7.29) | `pio run -e tlora_pager` | image / static DRAM | 2,869,113 B (68.4 %) / 94,712 B |
| 2026-10-19 | 57c44e2 + P7.30 working tree | `pio run -e tlora_pager` | image / static DRAM | 2,869,904 B (68.4 %) / 94,192 B (−520: the two static `Event`s shrink with `MAX_PACKET` 512 → 244) |

Scratch artifacts from this audit (not in the repo, may be cleaned by the OS):
- `/tmp/xsim` (transfer simulation)
- `/tmp/jbench` (JSON bench)
- `/tmp/emu_gdb` (emulator gdb scripts and logs)

---

## 16. Progress log

_Append-only. Working tree on top of `c9c1a96`, uncommitted. Hardware checks marked ☐ are still owed._

### 2026-10-19: Phase B and Phase A instrumentation

| ID | Status | What changed | Verified | HW owed |
|---|---|---|---|---|
| P7.1 | ✅ code | `notes_crypto.cpp` `encrypt_with_pw` seeds `keycache_store_locked` under `PrewarmLock` when `g_unlocked && g_passphrase == pw` (same gate as decrypt). Corrected the "tens of ms" comments in `notes_crypto.cpp/.h`. | builds | ☐ edit → reopen, no ~2 s freeze (`p`/span) |
| P7.3 | ✅ code | New `hal::secret_mask_peek` (secrets.h/.cpp): answers "(not set)" / "(locked)" / a session-remembered mask without PBKDF2. The mask (`********`+last 4) is remembered by `secret_store` and `secret_mask_display`, and forgotten on clear or when seen locked. Settings › Telegram and › Notes Sync: `refresh_labels` never decrypts on the LVGL thread; on a miss it shows `********` and runs the decrypt on a `OneShotJob` (`tg_mask` / `nsync_mask`), torn down in `reset_state`. The 1 ms "paint first" timer and its placeholders are gone. `ui_notes_sync.cpp` `enter()` and `ui_ssh.cpp` `onStart` use `secret_exists` + `notes_crypto_is_unlocked()`. | builds, emulator sweep | ☐ Settings › Telegram opens painted |
| P7.23 | ✅ code | `ui_journal.cpp` `bg_refresh_done`: `fresh = gen == r.fs_gen` (no +1; verified `hw_save_preferred_bytes` never bumps the generation for `_idx.bin`). | builds | ☐ `j`/`f` console: one reconcile per dirty open |
| P7.24 | ✅ code | `storage.cpp` `hw_read_file`: `f.close(); lock.release();` before `decode_after_read`. | builds | — |
| P7.8 | ◐ narrowed | **Plan premise partly wrong:** with the link up, `tg_phone_request` still forwards `bridge` + `token` so the phone can fall back to the bridge. So the decrypt is skipped only when the link is up **and** no bridge URL is set; otherwise behaviour is unchanged (phone-only when the token is locked/unset). | builds | — |
| P7.20 | ✅ code | `settings_storage.cpp` `storage_copy_notes_cb` runs on `core::OneShotJob<CopyNotesResult>` (`copy_notes`, 8 KB, prio 2); path chosen on the UI thread; double-click ignored while busy. The hub path loses per-file progress (indeterminate loader), since `storage_progress_cb` touches LVGL. | builds | ☐ copy with phone; UI stays live |
| P7.17 | ✅ code | `core/system.cpp` status lambda: typing skips only the gauge sweep and the file-count walk; clock, icons and the USB "unsafe to disconnect" box keep updating. | builds, sweep | ☐ USB plug while in editor |
| P6.7 | ✅ code (pulled forward) | Same lambda: icon colours written only on change (`set_icon_color`, file-scope caches reset by `applyChromeStyle`). This was the only idle invalidation source in the P7.9 gdb trace besides real text changes. | sweep pixel-identical | — |
| P7.11 | ✅ code | `theme_init()` now returns `bool` and early-outs when `{style, dark, system font}` match the last build. `ui_sys_exit` calls `applyChromeStyle` only when it rebuilt (style/dark callbacks already re-tint). | sweep pixel-identical | ☐ font change on device |
| P7.40 | ✅ code | Hub probe: not spawned while Wi-Fi is down; `hub_reachable`/`hub_probe_running` are `std::atomic<bool>`. Telegram: `tg_uplink()` skips `internet_available()` while the link is up. | builds | — |
| P7.39 | ✅ code | `ui_weather.cpp` `weather_fetch`: with the phone up, a 1.5 s TCP probe (1.1.1.1:53) gates the direct path; without the phone, direct as before. P6.37 (handshake timeout) still open. | builds | — |
| P7.54 | ✅ code | `lib/LilyGoLib/src/lv_conf.h` `[LOCAL PATCH]`: `LV_ASSERT_HANDLER abort();`. | builds | — |
| P7.43 | ◐ | Only the `[stackwm]` print: now `#ifdef INPUT_PROFILE` (kept for P3.9). The read-loop rework is still open. | builds | — |
| P7.49 | ✅ code | `hal::hw_get_heap_stats` (internal free / min-free / largest block, PSRAM free / min-free). Settings › Info gains **RAM** and **PSRAM** rows (1 Hz, written only on change). Profile build prints `[prof] heap: …` and `[prof] stack free: …` (by-name `xTaskGetHandle` sweep: the 2.0.17 SDK has `CONFIG_FREERTOS_USE_TRACE_FACILITY` off) every 10 s, even while idle. | builds | ☐ fill §15 rows |
| P7.9 | ◐ code | Profile build: vendor `disp_flush` `[LOCAL PATCH]` weak hook `lvh_prof_flush` times the byte-swap and `pushColors`; display `REFR_START/READY` events time the refresh. Slow frames print `refr (render, swap, push; px, flushes), timers/other` and the CPU MHz; the window line adds refresh max and average px per frame. **Not done:** per-timer-callback time (LVGL 9.5 has no timer hook; the census still names the timers). Pixels are flushed px (≈ invalidated area after joining). | builds | ☐ fill §15 rows |
| §G.1 boot markers | ☐ | Not started. | — | — |

**Emulator screenshot sweep** (`UI_SHOTS_DIR`, 8 style × mode combos, 256 shots): exit 0, and pixel-diffed against a `c9c1a96` worktree
baseline. Every differing pixel lies in the status-bar clock row (y ≤ 21) or the Date/Time page's live minute field, apart from the
emulator's simulated battery %. No other visual change from P6.7, P7.11 or P7.3.

**Next open items**, in order:
1. One `tlora_pager_profile` hardware session: fill the §15 boot/frame/heap/stack rows, plus the ☐ checks above.
2. §G.1 boot markers.
3. Phase C (P7.29 first, all three engines plus lossy tests).

### 2026-10-19 (later): §G.1 boot markers and P7.29

| ID | Status | What changed | Verified | HW owed |
|---|---|---|---|---|
| §G.1 | ✅ code | Profile build only. `core::prof::boot_mark(name)` records `esp_timer` time into a 32-slot table, and `report_tick` prints it once after the first display flush as `[prof] boot: <name> <ms> (+<delta> ms)`. Markers: `setup`; vendor `LilyGoLoRaPager::begin` (`[LOCAL PATCH]`, weak `lvh_prof_boot_mark` + `BOOT_MARK`): begin, i2c pmu/xl9555, bhi260, display init, slow crystal, rtc, nfc, keyboard, haptic, lora, sd, msc, begin end; then `lvgl helper`, `hw_init`, `ui init`, `first flush` (from `lvh_prof_flush`), `connectivity`. Caveats: the time before `esp_timer` starts (ROM, bootloader, and probably the PSRAM test) is not covered. The `connectivity` mark (setup task) and `first flush` (LVGL task) can race on the slot counter; profile-only, accepted. `derive_key_iv` already measured (IS.10). Pager only (the profile env extends `tlora_pager`). | builds: `tlora_pager_profile`, `tlora_pager`, emulator | ☐ fill the P7.44 boot-budget table |
| P7.29 | ✅ code, all three engines | Same semantics in `link_xfer.cpp`, `xfer.go`, `Xfer.swift`; wire format unchanged. **Sender:** tracks `high` (highest offset sent); rewinds at most once per acked offset (`rewound` latch, cleared on progress); an ACK above the send point moves it forward; new `retransmit()`. **Receiver:** at most **two** ACKs per gap. If the sender has not been told `next` yet (`last_ack != next`), the first ACK is progress for it; the second is the duplicate that triggers the one rewind. A single re-ACK was not enough: when the batch ACK is below the gap, the re-ACK reads as progress and only the timer recovers (found by the Go harness). After Done it re-ACKs the final offset for any DATA (lost final ACK). **Owners,** retransmit timeout `XFER_RETRANSMIT_MS` / `RetransmitTimeout` / `Xfer.retransmitTimeout` = 2 s: `handlers_fs.cpp` `fs_tick` (reads) and `link_service.cpp` `service_tick` (outbound) via `rto_ms`, at 1 s tick granularity; Go `SendBytes` (one idle timer + one RTO timer, also drops the per-loop `time.After` from P7.64); Swift `LinkSession.outboundIdle` (the watchdog wakes at `min(timeout, 2 s)`). `PROTOCOL.md` §4 now documents the rules and says credits count DATA messages, not packets (also `link_frame.h`). `make protocol-test` now runs `test_link_xfer` (it didn't before). | Unity `test_link_xfer` 26/26; Go `-race` (82.8 % cover); PagerKit 113/113 (Docker swift); `make protocol-test` exit 0; all 5 fw envs build; native 145/145. Lossy tests (batched + interleaved, drop k-th DATA or ACK) in all three; negative control in §15. | ☐ 256 KB phone upload with induced loss (§14 smoke test) |

**Not done in P7.29, by choice:**
- `bulk.json` scenario vectors: the vectors cover the codec only, and each engine now has the same lossy cases.
- **A receiver that is deleted at Done still can't answer a retransmit.** Swift `finishIncoming` closes the inbound right away, so a final ACK lost on the way to the pager (the pager's rx queue overflow, P7.30) leaves the pager sender retransmitting until its stall window. Fixing that needs a short linger on the phone side. P7.30 (grant sized to the queue) removes most of this loss.

**Next open items**, in order:
1. The `tlora_pager_profile` hardware session (§G.1 boot rows now included), plus every ☐ above.
2. Phase C: P7.30 (BLE grant and queue), then P7.32, P7.31, P7.33, P7.34.

### 2026-10-19 (later): P7.30

| ID | Status | What changed | Verified | HW owed |
|---|---|---|---|---|
| P7.30 | ✅ code | **Queue holds a whole window:** `link_transport_ble.cpp` `QUEUE_DEPTH` = `INITIAL_CREDITS` × `packets_per_message(MAX_MESSAGE, MAX_PACKET)` + `RX_SLACK` (16 × 17 + 32 = 304 events, ~76 KB PSRAM; internal fallback 16). `MAX_PACKET` = `hal::ble::ATT_MTU` − 3 = 244 (new `ble_stack.h` constant, also used by `setMTU`; `static_assert` == `TCP_PACKET`), so `Event` is ~250 B (P7.37 part). **Host task never blocks on a packet:** `post()` waits 0 ticks for `Kind::Packet`, 50 ms only for control events; a drop costs one message, which P7.29 recovers. **Grant sized to the queue:** new `TransportOps::rx_queue_packets` (BLE: depth − slack; TCP: 0 = unbounded, so the emulator is unchanged) and `service_rx_window()` → pure `rx_credits_for()` / `rx_ack_every_for()` (`link_frame.h/.cpp`). Both receivers (`handlers_fs.cpp` `handle_write_begin`, `link_service.cpp` `service_accept_expect`) use it. At P = 244 it is still 16 credits / ack every 4, so nothing changes on the wire; at a small MTU (P = 20) the grant drops to 1 credit / ack every 1 after the first ACK. **Drops visible:** `HwLinkStats::rx_dropped` (cumulative, BLE only), the per-tick log prints the total, `companion_up` logs `rxq=`. `PROTOCOL.md` §4: a receiver may grant fewer than 16 and should then ACK often. | Unity `test_rx_window` (new), native 146/146; `make protocol-test` exit 0 (Unity 76, Go, PagerKit 113); `tlora_pager`, `twatchs3`, `twatch_ultra`, `emulator_lora_pager` build; emulator `pagerlink smoke` all steps passed. | ☐ phone → pager `fs.write` of 256 KB+ and a `tg.messages` bulk reply: `rx_dropped` stays 0, no `[link] dropped` lines |

**Not done in P7.30, by choice:**
- The **implicit first window** is still 16 messages; a receiver can only shrink it after its first ACK. At P = 244 the queue now covers it. At a tiny MTU it doesn't, but P7.29 recovers, and iOS always negotiates a large MTU. Shrinking the first window would need a `window` field in `fs.write.begin` / the tg.* params (a protocol change in all three engines).
- `link_task` priority (1, core 0) unchanged: the queue now absorbs its slow FFat writes. Revisit with P7.34 if `rx_dropped` shows up on hardware.

**Next open items**, in order:
1. The `tlora_pager_profile` hardware session plus every ☐ above (no device was attached in this session).
2. Phase C: P7.32 (event-driven notify, pump off the caller's worker), then P7.31, P7.33, P7.34.
