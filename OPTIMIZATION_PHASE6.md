# Optimization Phase 6 — Full-surface performance audit

Generated **2026-09-25** against a clean tree at **`55944a7`** ("chore: ignore graft's
local graph cache"). Items are numbered **`P6.x`**, continuing the
`P2.x`/`P3.x`/`P4.x`/`P5.x`/`PB.x` series.

**Scope.** Phases 2–5 and the PB batch went deep on fake-sleep power, task cadences and
dead code. This pass swept the **whole** app for performance, in five areas audited in
parallel: boot time, awake CPU and LVGL rendering, memory and heap, storage/SPI/SD I/O,
and network/TLS/lilyhub. Before being kept, every candidate was checked against all seven
earlier `OPTIMIZATION_*.md` docs. Where an item extends an older one, it names that item.

> **⚠️ Measurement discipline (inherited, unchanged).** Nothing here is current- or
> time-measured. Statements of *mechanism* (what the code does) were verified by reading the
> source, the vendored LVGL 9.5.0, and the arduino-esp32 2.0.17 framework. **Magnitudes
> (ms, KB, wake rates) are estimates** unless marked otherwise. The boot items (§A) should
> start with the `esp_timer_get_time()` markers in §G.1, which turn estimates into numbers
> without a meter.

> **Headline:** the three items with the biggest payoff are **not** power items:
> 1. **Boot:** probably 2–3 s of blocking vendor work before the first frame (§A).
> 2. **SD I/O:** SPI clocked at **4 MHz** on the bus it shares with the 80 MHz display, so
>    every SD access stalls display flushes (P6.19).
> 3. **Telegram through the hub is broken:** the proxy rejects every URL the firmware
>    sends (P6.28, a correctness regression).

---

## 0. Corrections to the record (read first)

| Claim in older docs | Reality (verified 2026-09-25) |
|---|---|
| P4.15 and the `wireless.cpp` comment: core is **arduino-esp32 2.0.14** | Wrong. `espressif32@6.10.0` resolves to `framework-arduinoespressif32 3.20017.241212` = **arduino-esp32 2.0.17 / ESP-IDF v4.4.7 / mbedTLS 2.28.7**. `firmware.bin` contains `v4.4.7`, and `firmware.map` has 0 refs to the 3.x or 2.0.14 packages, which are installed on the machine but not used by this project. |
| PHASE5 §3 and PB.21: core is **v3.1.3 / IDF 5.3.0** | Wrong, as above. The **conclusions still hold** on 2.0.17: `CONFIG_PM_ENABLE` is not set, and `WiFiClientSecure` has no session API. |
| `CLAUDE.md`: LVGL **9.4.0** (hw) | `lvgl/lvgl @ ^9.4.0` resolves to **9.5.0** in `.pio/libdeps/tlora_pager`. |
| P3.9 option (b): SSH stack in PSRAM | **Not available.** `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` is unset in the 2.0.17 sdkconfig, and `xTaskCreateWithCaps` needs IDF ≥ 5.1. |
| P4.19: NTP retry "drags an HTTP lookup with it" | Stale premise. `timezone_fetch_offset` is now a local POSIX-table computation with no HTTP. |
| P3.27: second OTA slot "breaks the OTA chain" | No OTA code exists: zero `Update.begin` / `esp_ota_*` / `ArduinoOTA` / `HTTPUpdate` in `src/` or `lib/LilyGoLib/src/`. See P6.42. |
| `notes_crypto.cpp` comment (~L118): PBKDF2 is "tens of ms per derivation" | Probably wrong by 10x or more. The 48-byte output needs **2 × 10k** HMAC-SHA256 iterations, and mbedTLS 2.28 re-keys HMAC every iteration. Estimated hundreds of ms to over 1 s. Measure (§G.1). |
| PB.8: LVGL task idles up to 200 ms on a static screen | **Never took effect.** See P6.6. |

---

## 1. Size snapshot (from `.pio/build/tlora_pager`, built 2026-09-21)

| Item | Bytes |
|---|---|
| `firmware.bin` | 2,855,264 (68.1 % of the 4 MiB app slot) |
| `.flash.text` / `.flash.rodata` | 1,577,375 / 1,155,792 |
| `.iram0` (text + vectors) | 88,134 |
| static internal DRAM (`.dram0.data` + `.bss`) | 33,552 + 56,408 = 89,960 |
| Top flash contributors | lvgl 611 K · `src/` 552 K (fonts, 55.6 K `kRootCaBundle`) · LilyGoLib 158 K (123.7 K BHI260 blob) · net80211 126 K · libc 108 K · lwip 98 K · LibSSH 95 K · mbedcrypto 92 K · NimBLE 88 K · **libsodium 70 K (only LibSSH)** · btdm 68 K · helix-mp3 53 K |
| Largest internal DRAM symbols | `ssh_ciphertab` 6.9 K (.data) · TinyUSB `_dfu_ctx` 4.1 K / `_mscd_buf` 4 K · `g_cnxMgr` 3.8 K · `ndef` 2.8 K · `ftm_initiator` 2.7 K |

Relevant sdkconfig facts (2.0.17, `qio_qspi`):
- **Flash and CPU:** QIO flash at 80 MHz, quad PSRAM at 80 MHz, 240 MHz CPU, 1 kHz FreeRTOS tick, 16 KB I-cache.
- **Allocation:** `SPIRAM_MALLOC_ALWAYSINTERNAL=4096`, `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y`, `SSL_MAX_CONTENT_LEN=16384` (symmetric).
- **Crypto acceleration:** hardware AES, SHA and MPI are on.

---

## 2. Executive summary

| ID | Item | Area | Value | Risk | Kind |
|---|---|---|---|---|---|
| P6.1 | 32 kHz crystal calibration busy-waits ~0.8 s every boot | Boot | **HIGH** | LOW-MED | perf |
| P6.2 | BHI260 123.7 KB firmware uploaded over I2C before the display (~1–2 s) | Boot | **HIGH** | LOW | perf |
| P6.3 | 8 MB PSRAM self-test on every boot (~0.3–0.5 s) | Boot | MED-HIGH | LOW | perf |
| P6.4 | Vendor `setupMSC()` walks and prints the whole FFat tree on boot | Boot | MED | LOW | perf |
| P6.5 | Boot screen clear allocates twice the buffer it needs (426 KB) | Boot | LOW | LOW | mem |
| P6.6 | LVGL task never idles (indev timers + `vTaskDelay` ignores the wake notify) | CPU | **HIGH** | MED | perf/power |
| P6.7 | Status-bar colour setters invalidate and redraw every second | Render | MED | LOW | perf/power |
| P6.8 | Blinking textarea cursors keep LVGL at 30 Hz | Render | MED | LOW | perf/power |
| P6.9 | Per-flush CPU byte-swap; render straight to RGB565_SWAPPED | Render | MED | LOW-MED | perf |
| P6.10 | Second full-screen draw buffer buys nothing (synchronous flush) | Render | MED | LOW | mem |
| P6.11 | First input after 2 s idle is handled at 80 MHz | CPU | MED | LOW-MED | latency |
| P6.12 | Opaque full-screen overlays re-render the hidden app every frame | Render | MED-LOW | LOW-MED | perf |
| P6.13 | Home menu rebuilt with ~300 local-style calls on every app exit; style cache off | Render | MED-LOW | LOW | latency |
| P6.14 | SSH terminal: O(n) re-measure per append, trim with no hysteresis, 256 B/50 ms read loop | Render/CPU | MED (while used) | LOW | perf |
| P6.15 | Recording screen animates a shadowed dot at 30 fps for the whole recording | Render | MED-LOW | LOW | perf/power |
| P6.16 | Tasks list: full rebuild per mutation, `LV_EVENT_ALL` per item | Render | LOW-MED | LOW-MED | perf |
| P6.17 | Animated scroll-on-focus renders 6–12 frames per encoder step | Render | MED | LOW | product call |
| P6.18 | Small awake-CPU and render items (bundle) | CPU/Render | LOW | LOW | perf |
| P6.19 | **SD card at 4 MHz on the SPI bus shared with the 80 MHz display** | Storage | **HIGH** | MED | perf |
| P6.20 | `openNextFile()` listing is O(n²) FAT lookups with the SPI lock held for the whole walk | Storage | **HIGH** | LOW-MED | perf |
| P6.21 | Every listing returns mtime 0, so journal previews never refresh (bug) | Storage | MED | LOW | **bug**/perf |
| P6.22 | PBKDF2 re-runs on the UI thread: prewarm never re-armed; secrets evicted from the key cache | Crypto | **HIGH** | LOW | perf |
| P6.23 | Every save mirrors to SD synchronously on the UI thread | Storage | MED-HIGH | LOW-MED | latency |
| P6.24 | `hw_read_file` reads SD first: slower, and can open a stale copy (bug) | Storage | MED | LOW | **bug**/perf |
| P6.25 | Journal snippet decrypts the whole file; CBC prefix decrypt suffices | Crypto | MED | LOW | perf |
| P6.26 | Audio SD I/O: 14 KB MP3 refill under one lock; recorder 1 KB-per-lock, never flushed | Storage | MED | LOW | perf/robustness |
| P6.27 | Small storage and crypto items (bundle, includes one bug) | Storage | LOW | LOW | perf/bug |
| P6.28 | **Hub Telegram proxy rejects every firmware URL; bg poll bypasses the hub** | Network | **HIGH** | LOW-MED | **bug**/perf |
| P6.29 | CA bundle index freed and re-allocated per request: use-after-free race | Network | MED-HIGH | LOW | **bug**/perf |
| P6.30 | mbedTLS allocates ~35–45 KB per session from internal SRAM only | Memory | MED-HIGH | MED | mem |
| P6.31 | HTTP keep-alive works on 2.0.17; per-host connection reuse (extends P4.15) | Network | **HIGH** | MED | perf/power |
| P6.32 | 5 s time-to-first-header aborts hub long operations server-side | Network | MED-HIGH | LOW | **bug**/perf |
| P6.33 | Notes sync uploads every note twice and re-sends notes the remote already has | Network | MED-HIGH | MED | perf |
| P6.34 | Weather hub fallback ignores reachability; hub verdict stale after boot and wake | Network | MED | LOW | latency |
| P6.35 | Weather downloads 168 hourly points and renders 8 | Network | MED | LOW | perf |
| P6.36 | Voice memo base64 body fully built in PSRAM; memos over ~3 min can't be sent | Network | MED | MED | perf/**bug** |
| P6.37 | TLS handshake timeout is the 120 s default | Network | LOW-MED | LOW | robustness |
| P6.38 | WiFi reconnect: no BSSID/channel reuse, only saved slot 0 | Network | MED | LOW-MED | latency/power |
| P6.39 | `listen_interval` left at IDF default under fake-sleep MAX_MODEM | Network | LOW-MED | LOW-MED | power |
| P6.40 | BLE advertises at 30–60 ms forever while unconnected (+ two side bugs) | BLE | LOW-MED | MED | power |
| P6.41 | Server proxy responses lack Content-Length / Cache-Control (extends D14) | Server | LOW | LOW | perf |
| P6.42 | Second OTA slot is 4 MiB of never-written flash (extends P3.27) | Flash | MED | MED | capacity |
| P6.43 | LibSSH costs 10 KB static DRAM + 165 KB flash even when unused | Flash/RAM | LOW | MED | product call |
| P6.44 | Unused software-blend colour formats still enabled (extends P3.19) | Flash | LOW | LOW | flash |
| P6.45 | Investigate: `-O2`/IRAM for LVGL draw, LVGL multi-core draw units | CPU | ? | MED | investigate |

---

## 3. Detailed findings

### A. Boot time — none of this was covered by earlier phases

Everything in this section runs inside the vendor `instance.begin()` **before LVGL starts**,
so it adds directly to time-to-first-frame. The estimated total for P6.1–P6.4 is **~2–3 s**.

#### P6.1 — 32 kHz crystal calibration busy-waits ~0.8 s on every boot
- **Where:**
  - `lib/LilyGoLib/src/LilyGoGeneral.cpp` → `calibrate_one` / `esp_enable_slow_crystal`.
  - Called from `LilyGo_LoRa_Pager.cpp` `begin()` (~L388).
- **Code:**
  ```cpp
  for (int i = 0; i < 5; ++i) { cali_val = rtc_clk_cal(cal_clk, cal_count); } // cal_count = 1000
  ```
- **What it costs:**
  - `esp_enable_slow_crystal` calls `CALIBRATE_ONE` 6 times. Each call runs 5 × 1000 slow-clock cycles.
  - Once the slow clock is switched to the 32.768 kHz crystal, 5 of those 6 calls run on it at ~153 ms each. That is **≈ 0.8 s of busy-wait**, computed from the code.
  - Only `cal_32k` is ever read, so the other passes are pure delay.
  - Nothing calls `esp_clk_slowclk_cal_set()`, which may also be a correctness gap for light-sleep and RTC time compensation. Check it.
- **Fix:**
  - Run one short calibration (e.g. 100 cycles, one pass) to confirm the crystal started.
  - Switch the clock and set the cal value once.
  - Alternatively, run the whole routine in a low-priority task after the first frame.
- **Risk:** LOW-MED. It is a vendor patch that touches the RTC; mark it `[LOCAL PATCH P6.1]` like P4.11/P5.1.

#### P6.2 — BHI260 firmware (123.7 KB) is uploaded over I2C on every boot, before the display (extends P3.25/P4.22)
- **Where:** `LilyGo_LoRa_Pager.cpp` `begin()` (~L350–364) → `initSensor()` (~L1088–1111):
  ```cpp
  Wire.setClock(1000000UL); sensor.setBootFromFlash(false); sensor.begin(Wire)
  ```
- **What it costs:**
  - SensorLib uses 64-byte I2C writes on ESP32, so the upload is ≈ 1,930 transactions.
  - Estimated **1–2 s**.
  - Since P4.22 the IMU is on-demand, and its only consumer is the IMU debug settings page.
- **Fix:**
  - Pass `NO_HW_SENSOR` in `factory.ino` `setup()`'s `disable_flags`.
  - Call `instance.initSensor()` lazily from `settings_imu_debug.cpp` `build_subpage`; `try_begin_cb` already shows the pattern.
  - `hw_is_face_down`, `hw_get_imu_params` and `hw_unregister_imu_process` already gate on `HW_BHI260AP_ONLINE`.
- **Risk:** LOW. The only visible change is a one-time ~1–2 s load when the IMU page first opens.

#### P6.3 — 8 MB PSRAM memory test runs on every boot
- **Where:** framework `cores/esp32/esp32-hal-psram.c` L42–82.
  - `__attribute__((weak)) bool testSPIRAM()` calls `esp_spiram_test()`, which walks all 8 MB twice.
  - It is not overridden anywhere in `src/`.
- **Estimate:** ~0.3–0.5 s.
- **Fix:** add `extern "C" bool testSPIRAM(void) { return true; }` in `src/`.
- **Risk:** LOW. You lose boot-time PSRAM defect detection, which is rarely useful on a shipped device.

#### P6.4 — Vendor `setupMSC()` walks and prints the whole FFat tree on every boot
- **Where:** `lib/LilyGoLib/src/USB_MSC.cpp` `setupMSC()` → `__listDir(FFat, "/", 3)` (~L218).
- **What it costs:**
  - Each entry does `openNextFile` + `getLastWrite()` (a stat) + `localtime` + `Serial.printf`.
  - `/notes` lives on FFat, so **boot time grows with the note count**.
  - This is not the `src/` `listDir` that phase 1 removed.
- **Fix:** delete the call, or gate it on `CORE_DEBUG_LEVEL > 0`.
- **Risk:** LOW.

#### P6.5 — Boot screen clear allocates twice the needed buffer
- **Where:** `lib/LilyGoLib/src/LilyGoDispInterface.cpp` (~L702):
  ```cpp
  std::vector<uint16_t> draw_buf(_width * _height * 2, 0);
  ```
- **Problem:** that allocates and zeroes 426 KB but pushes 213 KB.
- **Fix:** size it `_width * _height`, or push rows from a small buffer.
- **Risk:** LOW.

---

### B. Awake CPU and LVGL rendering

#### P6.6 — The LVGL task never idles: PB.8's 200 ms cap is unreachable and the input wake is ignored (extends PB.8)
- **Where:**
  - `src/hal/lvgl_task.cpp` `lvgl_task_fn` (awake branch).
  - `lib/LilyGoLib/src/LV_Helper_v9.cpp` `beginLvglHelper` (indev creation ~L366–395).
- **Code:**
  ```cpp
  next = lv_timer_handler();  ...  if (next > kMaxTickMs) next = kMaxTickMs;  vTaskDelay(pdMS_TO_TICKS(next));
  ```
- **Two defects, both verified:**
  1. **The handler always returns ≤ 33 ms.**
     - Each indev (encoder, keypad) owns a read timer at `LV_DEF_REFR_PERIOD` = 33 ms (`lvgl/src/indev/lv_indev.c:132`), and nothing pauses it.
     - Nothing in the repo calls `lv_indev_set_mode`.
     - Only the display refresh timer pauses when idle, so `lv_timer_handler()` never returns more than 33 ms.
     - Result: **~30 wakes/s forever while the screen is on**, each taking the instance mutex (contended with the keyboard, NFC and charge tasks) and running both indev read callbacks.
  2. **The input wake does nothing while awake.**
     - `vTaskDelay` ignores task notifications, so the `hw_lvgl_task_notify_wake()` that `keyboard_task`/`rotary_task` `enqueue_event` sends has no effect.
     - Only the fake-sleep branch consumes it.
     - Input latency is currently bounded only *because* of defect 1.
- **Fix (both halves must land together):**
  - Replace `vTaskDelay(next)` with `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(next))`.
  - Put the keypad/encoder indevs in `LV_INDEV_MODE_EVENT` and call `lv_indev_read()` for both at the top of each loop.
  - Alternatively, keep timer mode and `lv_timer_ready()` the read timers on wake.
  - While the last read state is PRESSED, cap `next` at 33 ms so long-press and auto-repeat still work.
  - Fixing only the indev mode would raise input lag to 200 ms.
- **Risk:** MED. It is the input path; do a hardware feel test (scroll, typing, long-press). This is the biggest remaining awake-idle CPU and power lever.

#### P6.7 — Status-bar colour setters invalidate every second (extends P4.10)
- **Where:** `src/core/system.cpp` 1 Hz lambda in `setupGlobalUI`. It calls `lv_obj_set_style_text_color` unconditionally for:
  - SD, when no card is present
  - BT, when enabled
  - WiFi, when enabled
  - Hub, when enabled
  - USB, when mounted
- **Why it costs:**
  - P4.10 guarded `lv_label_set_text*` only.
  - In LVGL 9.5, `lv_obj_set_local_style_prop` → `lv_obj_refresh_style` → `lv_obj_invalidate` runs **unconditionally**, even when the value is unchanged (`lv_obj_style.c` ~L353–378, verified).
  - So with WiFi enabled, or no SD card, the screen is never static: one render + SPI flush per second, forever.
- **Fix:** cache the last colour and visibility per icon, and call setters only on change.
- **Risk:** LOW.
- **Dead code:** `menu_glance.cpp` `glance_refresh` has the same pattern (5 labels + a colour every second, inside a 180° transformed layer). **`glance_show()` has zero callers** (graft-verified), so it is dead and not a runtime cost. Guard it only if it is ever revived, and prefer hardware rotation over `transform_rotation`.

#### P6.8 — Blinking textarea cursors keep LVGL at 30 Hz
- **Mechanism:**
  - The default theme gives `LV_PART_CURSOR | LV_STATE_FOCUSED` a 400 ms `anim_duration` (`lv_theme_default.c`).
  - A focused textarea therefore runs an infinite blink animation, which keeps LVGL's animation timer at 33 ms and invalidates the cursor 2.5×/s.
- **Affected screens:**
  - SSH `term_` (its cursor is already transparent but still blinks) and `input_`
  - Telegram and Chat composers
  - Tasks `add_ta`
  - the `ui_lock` passphrase field
  - settings text fields
- **Precedent:** the editor already disables the blink (`ui_text_editor.cpp` ~L409).
- **Fix:** one static style in `src/ui_theme.cpp` `new_theme_apply_cb`, with `anim_duration` 0 on `LV_PART_CURSOR | LV_STATE_FOCUSED` for `lv_textarea_class`.
- **Risk:** LOW (steady cursor). Once P6.6 lands, this is the main thing still waking LVGL on text screens.

#### P6.9 — Per-flush CPU byte-swap: render straight to RGB565_SWAPPED
- **Where:** `LV_Helper_v9.cpp` `disp_flush`:
  ```cpp
  #ifdef _SWAP_COLORS
  lv_draw_sw_rgb565_swap(color_p, len);
  #endif
  ```
  The display format is set to `LV_COLOR_FORMAT_RGB565`.
- **What it costs:** a full read-modify-write of the dirty area in PSRAM. For a full frame that is ~426 KB of traffic, estimated ~4–8 ms or 20–30 % of flush time.
- **Why it's avoidable:** LVGL 9.5 can render natively to `LV_COLOR_FORMAT_RGB565_SWAPPED`, and `LV_DRAW_SW_SUPPORT_RGB565_SWAPPED` defaults to 1.
- **Fix:** under `_SWAP_COLORS`, set the display to RGB565_SWAPPED and drop the swap call.
- **Risk:** LOW-MED. It is a vendor file, and the emulator doesn't exercise it, so check colours on hardware. It is distinct from P4.20, which covers buffer location and DMA.

#### P6.10 — The second full-screen draw buffer buys nothing (extends P4.20)
- **Where:** `LV_Helper_v9.cpp` allocates two 213 KB PSRAM buffers.
- **Why it buys nothing:** `disp_flush` is synchronous and calls `lv_display_flush_ready` inline, so double-buffering gives no render/flush overlap.
- **Fix:** use a single buffer. That saves 213 KB PSRAM now, and halves the internal-RAM cost if P4.20 ever moves the buffers internal for DMA.
- **Risk:** LOW. Decide it together with P4.20.

#### P6.11 — First input after 2 s idle is handled at 80 MHz
- **Where:**
  - `factory.ino` `loop()` drops to 80 MHz when `inactive_time > 2000`.
  - Only `loop()` restores the speed, on its `delay(50)` tick.
- **Why it's slow:**
  - Input wakes the LVGL task (prio 8), which preempts `loopTask` (prio 1) on the same core.
  - The first click handler therefore runs at a third of the speed for up to 50 ms: `switchApp` → `onStart` widget build → first full render.
  - That adds roughly 30 ms to nearly every navigation.
- **Fix:**
  - Raise to the active frequency inside `enqueue_event()` (`hal/keyboard_task.cpp`, `hal/rotary_task.cpp`).
  - Make `s_cpu_freq_mhz` atomic.
  - This also lets awake `loop()` slow from 20 Hz to ~4 Hz (see P6.18).
- **Risk:** LOW-MED.

#### P6.12 — Opaque full-screen overlays re-render the hidden app underneath every frame
- **Mechanism:** `lv_refr.c` `refr_area_part` always draws the active screen, then `lv_layer_top()` on top of it, with no occlusion check across layers.
- **Affected overlays:**
  - `ui_wifi_networks_open`
  - `ui_list_picker_open`
  - `ui_nfc_test_open`
  - `ui_popup_create` / `ui_loading_open`, including the 30 fps spinner
  - the USB warning box
- **Cost:** up to 2× render per frame; for example, scrolling the WiFi list also redraws Settings beneath it.
- **Fix:** while an opaque full-screen overlay is open, set `LV_OBJ_FLAG_HIDDEN` on `_mainScreen` and `_statusBar`. Make it ref-counted and restore it on every close path.
- **Risk:** LOW-MED.

#### P6.13 — Home menu rebuilt from scratch on every app exit; style cache off
- **Where:**
  - `core/app.h` base `onStop()` cleans the root.
  - `AppManager::switchApp` calls `MenuApp::onStop()` whenever an app opens.
  - `showMenu()` then re-runs `MenuApp::onStart`.
- **What the rebuild costs:**
  - Pills and 8 tiles, about 45 objects, set ~20–25 **local** style properties each: ~300 `lv_obj_set_style_*` calls.
  - Each call is a realloc plus a style refresh.
  - The outline properties also trigger an ext-draw-size refresh.
  - There are two forced `lv_obj_update_layout` passes.
- **Same pattern elsewhere:** Telegram `render_msgs` and Weather `make_cell` set `text_font` on every label. It inherits, so setting it once on the container is enough.
- **Fix:**
  - Use static shared `lv_style_t` for tiles and pills (MAIN/FOCUSED/PRESSED).
  - Enable `LV_OBJ_STYLE_CACHE 1` (`lv_conf.h` ~L381, 8 B per object, in PSRAM).
  - Optionally keep the menu tree alive and refresh only its dynamic parts.
- **Risk:** LOW. The cost is a few ms at 240 MHz, and three times that at 80 MHz (see P6.11).

#### P6.14 — SSH terminal (extends P3.4/§2.9/P3.9)
- **(a) Re-measure per append.** `ui_ssh.cpp` `append_terminal` calls `lv_textarea_add_text`, which runs:
  - `lv_label_ins_text`: realloc plus a full word-wrap re-measure.
  - `lv_textarea_set_cursor_pos`: `lv_obj_update_layout` plus scroll-to-cursor.

  That is O(buffer) per 100 ms chunk, with up to 8 KB of text.
- **(b) Trim with no hysteresis.** The trim cuts to just under 8000 bytes, so in steady state *every* append re-trims. Each re-trim costs `strlen` + an ~8 KB `std::string` copy + `lv_textarea_set_text` (free, alloc, full re-layout).
- **(c) Slow read loop.** `run_session` reads one 256 B buffer per stream, then sleeps `vTaskDelay(50)`:
  - throughput is capped at ~5–10 KB/s
  - it wakes 20×/s even when idle
  - it builds a temporary `std::string` per chunk
- **(d) Leftover debug print.** The P3.9 "TEMPORARY" `[stackwm]` `Serial.printf` is still live (`run_session` ~L338), the same situation P4.14 cleaned up for `ble_kb_ka`.
- **Fix:**
  - Trim to ~6000 when over 8000.
  - Render output into a plain `lv_label`, so there is no cursor to track.
  - Drain reads in a loop with a 1–2 KB buffer until empty, and replace the fixed sleep with `ssh_channel_poll_timeout` or `select` on `ssh_get_fd`.
  - Take `push_output(const char*, size_t)`.
  - Remove the `[stackwm]` print once P3.9 is measured.
- **Risk:** LOW, or MED for the widget swap.

#### P6.15 — Recording screen animates at 30 fps for the entire recording
- **Where:** `ui_audio_notes.cpp`:
  - `build_record_view` creates a red dot with `shadow_width 10` and an infinite 600 ms opacity pulse. With `LV_DRAW_SW_SHADOW_CACHE_SIZE 0`, the blurred shadow is recomputed every frame.
  - `rec_tick_cb` rewrites the montserrat_48 time label and the size label every 200 ms, although the time only changes once a second.
- **Why it matters:** these flushes compete for the shared SPI bus with the WAV writes (P6.26).
- **Fix:**
  - Blink the dot at 1 Hz from the existing tick.
  - Drop the shadow.
  - Write the labels only when their values change.
- **Risk:** LOW.

#### P6.16 — Tasks list: full rebuild per add/edit/delete, `LV_EVENT_ALL` per item
- **Where:** `ui_tasks.cpp` `rebuild_task_list`.
- **Problems:**
  - It runs `lv_obj_clean` and recreates every row (checkbox + label + ~12 local style properties) on every add, edit or delete. There is no row cap.
  - Each row registers `task_event_cb` with `LV_EVENT_ALL`, so the callback runs for every draw, cover-check and layout event of every row on every frame.
  - The toggle path is already incremental.
- **Fix:**
  - Insert and delete incrementally (`lv_obj_move_to_index`).
  - Subscribe only to the events actually used.
  - Share one static item style.
- **Risk:** LOW-MED.

#### P6.17 — Animated scroll-on-focus renders 6–12 frames per encoder step (product call)
- **Mechanism:**
  - `SCROLL_ON_FOCUS` scrolls with `LV_ANIM_ON` over 200–400 ms.
  - `ui_journal.cpp` also calls `lv_obj_scroll_to_view(…, LV_ANIM_ON)` itself.
- **Cost:** each frame of a ~460×190 area is ~17 ms of SPI wire time alone, so these animations likely run below 30 fps and saturate core 1 while scrolling.
- **Option:**
  - Clear `SCROLL_ON_FOCUS` in the theme apply callback.
  - Scroll with `LV_ANIM_OFF` from a FOCUSED handler.
- **Trade-off:** smoothness versus responsiveness and CPU. **Needs a user decision.**

#### P6.18 — Small awake-CPU and render items (bundle, all LOW risk)
- **Awake wake-ups:**
  - `rotary_reader` wakes ~20×/s awake, because vendor `getRotary()` uses a 50 ms queue timeout (`LilyGo_LoRa_Pager.cpp` ~L1224).
  - `loopTask` also wakes 20×/s awake (`delay(50)`), taking the instance mutex each time.
  - Fix: add a blocking vendor accessor; after P6.11, awake `loop()` can run at ~250 ms.
- **`notify::post()` doesn't wake LVGL:** a banner posted from another task waits up to `kMaxTickMs` = 200 ms once P6.6 lands. Add `hw_lvgl_task_notify_wake()` to `core/notify.cpp` `post()`.
- **Journal cards `clip_corner`:** each card's children render three times (corner-strip ARGB8888 layers plus a mask) on every redraw, and the children are transparent. Drop it and pad by at least 4 px.
- **Theme press animations:** `LV_THEME_DEFAULT_GROW 1` and `TRANSITION_TIME 80` cost ~150 ms of grow and transition frames per press. Set both to 0; this is cosmetic.
- **Chat "thinking" status:** `ui_chat.cpp` `poll_timer_cb` rewrites the same text 4 out of 5 times, every 200 ms for up to 20 s. Cache the last elapsed-seconds value.
- **Heap readout:** `hw_get_heap_info` (`hal/system.cpp`) calls `ESP.getHeapSize()`, which walks every heap block under the heap lock every second when the memory readout is on. The `total` value is unused, so call `heap_caps_get_free_size` only.
- **Dead battery history:** `hal/power.cpp` `battery_history` is write-only (no reader anywhere) and does `erase(begin())` each minute. Remove the vector and keep the charge-limit logic.

---

### C. Storage, SPI bus and crypto

Shared context for this section:
- On the pager, SD, display, LoRa and NFC share SCK/MISO/MOSI (`variants/lilygo_tlora_pager/pins_arduino.h`).
- The bus mutex is non-recursive, and display pushes take it.
- So **every `core::ScopedSpiLock` held during SD I/O freezes LVGL flushes**.
- FatFs itself is reentrant (`FF_FS_REENTRANT`).

#### P6.19 — The SD card runs at 4 MHz on a bus shared with the 80 MHz display
- **Where:** `lib/LilyGoLib/src/LilyGo_LoRa_Pager.cpp` `installSD()` (~L551, verified), plus the same line in `LilyGoWatchUltra.cpp` (~L644):
  ```cpp
  if (!SD.begin(SD_CS, SPI, 4000000U, "/sd")) {
  ```
  `src/` never remounts it faster. The only mention in the docs is a 25 MHz remount sketch inside the unimplemented PB.5 plan.
- **What it costs:**
  - A 512 B sector takes ~1.0 ms of clock time at 4 MHz, versus ~0.2 ms at 20 MHz.
  - A 14 KB MP3 refill holds the bus ~35–40 ms.
  - A 64 KB note read holds it ~200 ms.
  - Every SD item below gets ~5× cheaper from this one change.
- **Also in `sd_diskio.cpp`:**
  - CMD13 SEND_STATUS on every `disk_status`.
  - Software CRC16 per sector (`supports_crc=true`).
- **Fix:**
  - Mount at 20 MHz (or 25 MHz), falling back 10 → 4 MHz if `SD.begin` or a test sector read fails.
  - This can be done from `hw_mount_sd` (unmount and remount) without touching the vendor code.
- **Risk:** MED. MISO timing through the GPIO matrix and card compatibility both need hardware validation with several cards.

#### P6.20 — `openNextFile()` listings are O(n²) FAT lookups, with the SPI lock held for the whole walk (extends P2.7/P3.3/P2.9)
- **Mechanism (framework `FS/src/vfs_api.cpp`):**
  - `openNextFile` does `readdir`, then constructs a `VFSFileImpl`.
  - That constructor calls `stat()` and then `fopen()`/`opendir()`, **each of which walks the directory from its start**.
  - It also allocates a 4 KB stdio buffer per entry.
  - On SD, N entries cost ~N²/5 sector reads plus several CMD13s each.
  - Estimate: 30 notes ≈ 0.25 s, 100 ≈ 2.5 s, 200 = several seconds, all at 4 MHz under one `ScopedSpiLock`.
- **Affected loops:**
  - `storage.cpp` `list_files`, `list_entries`, `hw_count_internal_files`, `delete_path_recursive`.
  - `notes_crypto.cpp` `collect_sd_protected`.
  - `storage_bulk.cpp`.
  - SD callers hold the lock across the entire loop: `hw_list_sd_entries`, `hw_get_sd_txt_files`, `hw_get_preferred_txt_files_info`.
  - P3.3 moved the audio-notes scan off-thread, **but the display still freezes because the worker holds the bus lock**.
- **Fix:**
  - (a) Where only names are needed, use `root.getNextFileName(&isDir)` (available in 2.0.17, `FS.h:84`). It is a pure `readdir`, O(N).
  - (b) Where sizes or dates are needed (`list_entries`), use FatFs `f_opendir`/`f_readdir` directly. `FILINFO` includes `fsize`, `fdate` and `ftime` for free, which also fixes P6.21.
  - (c) Take the SPI lock per iteration, not across the whole walk.
- **Risk:** LOW for (a) and (c); MED for (b), because it needs the FatFs drive prefix.

#### P6.21 — Every listing returns mtime 0, so journal previews never refresh after an edit (bug caused by a perf shortcut)
- **Where:**
  - `storage.cpp` `list_files` does `list.push_back({filename.c_str(), 0});`. `getLastWrite` was dropped to avoid a re-stat.
  - `list_entries` does the same.
  - `ui_journal.cpp` `refresh_journal_entries` checks `if (old_e.mtime == mtime)`, which is always true for known names.
- **Effect:**
  - An edited note's preview stays stale forever, even after `r` refresh, and the stale snippet is persisted to `/journal_idx.bin` across reboots.
  - Each reconcile still does a full FFat walk plus an SD walk (P6.20) but can only detect adds and removes.
  - "mtime-desc" sorting in the file browser and audio notes is effectively by name.
- **Fix:**
  - Cheapest: store `file.size()` in the u32 slot. It is already free from the constructor's stat, and the index format doesn't change.
  - Proper: take `fdate`/`ftime` from `f_readdir` (P6.20b).
  - Alternative: have `hw_save_preferred_file` record changed paths, so the journal rebuilds only those snippets.
- **Risk:** LOW.

#### P6.22 — PBKDF2 re-runs on the UI thread (extends §2.3)
- **Where:**
  - `notes_crypto.cpp` `encrypt_with_pw`: `if (!consumed) { esp_fill_random(salt…); derive_key_iv(...) }`.
  - `notes_crypto_prewarm()` pre-derives **one** (salt, key, IV) triple. Its only caller is `ui_text_editor.cpp` `editor_build_ui`.
- **(a) Encrypt path.**
  - The first save of an editor session is free.
  - **Every later save pays the full derivation synchronously on the LVGL thread.** That includes save-on-exit, and every journal `save_index_file`, which runs during the LVGL-thread reconcile.
- **(b) Decrypt path for NVS secrets.**
  - `secrets.cpp` `secret_load` runs:
    - on every Telegram bg poll (`tg_bg_task`)
    - on Telegram open (`reload_config`, LVGL thread)
    - in the settings token-display getters
  - Its fixed salt shares the 6-entry LRU (`NC_KEYCACHE_N`) with every note, journal index and SSH decrypt.
  - Opening 6 or more encrypted notes, or one journal reconcile, evicts it, so the next poll or app open re-derives.
- **Cost:** see §0. `derive_key_iv` asks for 48 bytes, which is 2 PBKDF2 blocks × 10k iterations. Probably hundreds of ms or more each time.
- **Fix:**
  - (a) Re-arm the slot: call `notes_crypto_prewarm()` after a consumed encrypt, and after `notes_crypto_unlock`. It reuses the existing mutex and task. Never reuse salts.
  - (b) Add a small pinned cache tier for NVS-secret salts, zeroised in `notes_crypto_lock()` like the LRU.
  - Optional: a midstate-precomputed PBKDF2, ~2× fewer SHA compressions and byte-compatible with OpenSSL. Pin it with RFC test vectors in `native_test`.
  - Measure first (§G.1).
- **Risk:** LOW for (a) and (b); MED for the custom PBKDF2.

#### P6.23 — Every save mirrors to SD synchronously on the UI thread
- **Where:**
  - `storage.cpp` `hw_save_preferred_file`:
    ```cpp
    core::ScopedSpiLock lock; File f = SD.open(str, "w"); if (f) { … }
    ```
    The result is discarded.
  - Same pattern in `hw_save_preferred_bytes` and in `hw_delete_preferred_file`.
  - `ensure_notes_dir()` runs `FFat.exists` + `SD.exists` on every notes save.
- **What it costs:**
  - `SD.open("w")` does 3–4 directory walks (`VFSImpl::open` stat, constructor stat, `fopen`), plus truncate, data, dir-entry and FAT writes, all at 4 MHz under the lock.
  - Estimated tens to hundreds of ms per editor, tasks or journal-index save, growing with the size of `/notes`.
- **Fix:**
  - Move the best-effort SD mirror (writes and deletes) onto a single low-priority FIFO worker, which keeps per-path ordering.
  - Cache "notes dir ensured" in a static flag, reset on remount.
  - Optionally use POSIX `fopen("/sd"+path)` directly to cut 3 walks to 1.
- **Related:** `hw_prune_internal_storage` rewrites SD copies that the dual-write already created.
- **Risk:** LOW-MED. Pair it with P6.24.

#### P6.24 — `hw_read_file` reads SD first: slower, and can open a stale copy (bug)
- **Where:** `storage.cpp` `hw_read_file`:
  ```cpp
  if (is_sd) { lock.acquire(); f = SD.open(str, FILE_READ); … } if (!f) f = FFat.open(…)
  ```
- **Callers:** the text-editor open and tasks (`hw_read_preferred_file`).
- **Inconsistency:** `hw_read_preferred_bytes`, `…_snippet` and the list merge all treat FFat as authoritative ("Internal wins").
- **Cost:** a whole-file read over the 4 MHz bus under the lock, when an identical copy sits on 80 MHz QIO flash.
- **Correctness:** a note saved while the card was out leaves a stale SD copy. The editor opens that copy, and the next save overwrites the newer FFat version.
- **Fix:** read FFat first and fall back to SD, the same order as `hw_read_preferred_bytes`.
- **Risk:** LOW.

#### P6.25 — The journal snippet decrypts the whole file, but CBC allows a prefix decrypt
- **Where:** `storage.cpp` `hw_read_preferred_file_snippet`. Its comment says "CBC ciphertext can't be partially decrypted", then the code reads and decrypts the full file and truncates.
- **Why that's wrong:** CBC decryption is random-access. Block *i* needs only ciphertext blocks *i−1* and *i*, and block 0 uses the derived IV. Only the padding check needs the last block.
- **Fix:**
  - Read the 16-byte header (magic + salt) plus `ceil(kSnippetBytes/16)` blocks.
  - Decrypt that prefix without the padding check, via a new small `notes_crypto_decrypt_prefix` API.
- **Savings:** the full-file read, painful on SD, and three full-size heap copies per changed note during the reconcile.
- **Risk:** LOW.

#### P6.26 — Audio SD I/O
- **MP3 refill:** `audio.cpp` `mp3_fill_file` does `core::MaybeSpiLock lock(...); return fc->f->read(dst, maxlen);`, where `maxlen` can reach ~14 KB.
  - That is a ~35–40 ms bus hold at 4 MHz about once a second at 128 kbps: a periodic 2-frame UI hitch.
  - Fix: loop in ≤ 4 KB lock scopes, as the WAV player already does.
- **Recorder:** it takes the lock per 1 KB write (`frame_samples = 512`).
  - Three of every four writes are only a memcpy into the 4 KB stdio buffer, yet each still contends the bus mutex.
  - Because of the 44-byte WAV header, every flush splits into a 1-sector write plus a 7-sector write.
  - **There is no `flush()` until close**, so a crash or power loss during a recording of up to 5 minutes loses the whole file (the dir-entry size is never updated).
  - Fix: accumulate 8–16 KB in PSRAM and write it once per lock, and `flush()` every ~10 s.
  - Optionally pad the header to 512 B with a JUNK chunk. That needs `playWAV_sd`'s fixed `seek(44)` updated, which is MED risk for LOW gain.
- **Risk:** LOW.

#### P6.27 — Small storage and crypto items (bundle)
- **`notes_crypto_is_enabled()` reads NVS on every save:** Preferences begin, blob length and bytes, and a vector allocation, even for unprotected paths such as `tasks.txt`.
  - It is called from `storage.cpp` `encode_for_write` and 12 other sites.
  - Fix: short-circuit `protect && enabled`, and cache it in a static flag updated by `set_passphrase`, `change` and `disable`.
- **Bulk encrypt/disable reads whole files to check an 8-byte magic** (`notes_crypto.cpp`, around `raw_read`/`has_magic`). Probe 8 bytes first.
- **Status-bar note count:** `core/system.cpp` re-walks FFat every 60 s on the LVGL thread even when nothing changed. Use a filesystem generation counter bumped by save/delete, and `getNextFileName`.
- **Bug — "Copy Internal → SD" (`storage_bulk.cpp`) walks the FFat root twice and never copies notes.** Directories are skipped and notes live in `/notes`, so only `tasks.txt` and `journal_idx.bin` are copied. Recurse into `/notes` and drop the separate count pass.
- **Crypto buffer copies:**
  - `encrypt_with_pw` keeps `body` + `ct` + `out` (3× plaintext).
  - `decrypt_with_pw` keeps `plain` + `out`.
  - `ui_journal.cpp` `load_index_file` peaks at ~4× the index size.
  - Fix: encrypt in place inside `out` after reserving the header (mbedTLS CBC supports in-place), and decrypt straight into `out`.

---

### D. Network, TLS and lilyhub

#### P6.28 — The hub Telegram proxy rejects every URL the firmware sends, and the background poll bypasses the hub (bug + perf)
- **Where:**
  - `server/internal/telegram/telegram.go` `allowedTelegramURL` only accepts `https://api.telegram.org` (verified). Anything else gets a 400.
  - The firmware talks to a **user-configured bridge** REST API (`<bridge>/v1/chats…`), not the Bot API.
  - `ui_telegram.cpp` `tg_http_request` returns the proxy result as-is and never falls back to direct.
- **Effect:** with the hub enabled and reachable, every foreground Telegram op fails with HTTP 400: chat list, messages, mark-read, send and Settings favourites.
- **Background poll:** `tg_bg_task` calls `hw_http_request` directly (verified), so the most frequent periodic request always pays a direct TLS handshake.
- **Why it matters for performance:** device → hub is plain LAN HTTP, and the hub's Go client keeps a TLS keep-alive connection to the bridge. That is the cheapest way to stop the device doing a full TLS handshake (~0.5–1.5 s of radio time, ~40 KB internal heap) on every 5 s chat poll or 10 s list poll.
- **Fix:**
  - Server: replace the hard-coded host with a **configured bridge-host allowlist** (env var). Keep the SSRF guard, which was a deliberate security fix: the proxy attaches the bearer token.
  - Device: fall back to direct on proxy 4xx/5xx.
  - Route `tg_bg_task` through `tg_http_request`.
  - Optional: a hub `/unread` endpoint, so the background poll gets one number instead of 30 chats of JSON. Or at least `strstr` for `"unread":` instead of building a cJSON tree.
- **Risk:** LOW-MED (it touches a security guard; review it).

#### P6.29 — CA bundle index freed and re-allocated on every request: use-after-free race
- **Where:**
  - `wireless.cpp` `setup_secure_for_url` → `sec.setCACertBundle(kRootCaBundle)`.
  - It runs unconditionally from `http_open` and `hw_http_request`, **including plain `http://` hub calls**.
- **Mechanism:** `arduino_esp_crt_bundle_set` does `free(s_crt_bundle.crts)` + `calloc(121, 4)` on a **process-global** array, and re-walks 121 headers.
- **The race:** `esp_crt_verify_callback` binary-searches that same array during any concurrent handshake. Workers that can overlap: `tg_bg`, `tg_fg`, `wx_bg`, `wx_city`, `tg_favs`, `nsync_bg`. So a use-after-free is possible.
- **Fix:**
  - Set the bundle once at boot, and use a tiny `WiFiClientSecure` subclass that sets the protected `_use_ca_bundle = true`.
  - Skip TLS setup entirely for `http://`.
  - Optionally put one global mutex around `hw_http_*`, which also caps concurrent TLS sessions at one and bounds P6.30's internal-heap peak.
- **Note:** there is **no** full-bundle parse per request. Per request it builds a 484 B pointer index; per handshake it parses one root DER.
- **Risk:** LOW.

#### P6.30 — mbedTLS allocates only from internal SRAM: ~35–45 KB per live session
- **Evidence:**
  - `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y` + `SSL_MAX_CONTENT_LEN=16384`, symmetric: ~16.7 KB in + ~16.7 KB out record buffers, plus handshake and x509 state, per session.
  - `MBEDTLS_PLATFORM_MEMORY` is defined, and `mbedtls_platform_set_calloc_free` is exported by the prebuilt `libmbedcrypto.a` (verified with nm). No caller exists in the repo.
- **Fix:**
  - Call it once in `hw_init()` next to the P2.4 cJSON hooks.
  - Use a PSRAM-first calloc for allocations of ~4 KB or more (keep small ones internal), with an internal fallback.
  - This is the runtime equivalent of IDF's supported `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`.
- **Risk:** MED.
  - It is global: it also affects notes_crypto and LibSSH.
  - The S3 DMA-AES path must bounce external-RAM buffers.
  - Bench handshake time, and hardware-test TLS to Telegram, GitHub, open-meteo and SSH.
- **Relative size:** this is a bigger internal-RAM lever than P3.24 (NimBLE).

#### P6.31 — HTTP keep-alive is available on 2.0.17 (extends P4.15)
- **Current code:** `HttpClients c;` is a stack local in `hw_http_request` and `hw_http_get_string`, destroyed after every request.
- **Why reuse works:** in 2.0.17, `HTTPClient::connect()` returns "already connected, reusing connection" when `_reuse` is set. Session *resumption* is still out of reach, but connection *reuse* is not.
- **Fix:**
  - Keep a small per-host cache of `{WiFiClientSecure, HTTPClient}`, keyed by scheme+host+port. The key is required because `connect()` doesn't check the host.
  - Close idle connections after ~20–30 s: a held session pins ~40 KB of internal RAM, or PSRAM after P6.30.
  - Retry once on `SEND_HEADER_FAILED` / `CONNECTION_LOST` for sockets the server has closed.
- **Payoff:**
  - A Telegram chat view drops from ~12 handshakes/min to 1 per session.
  - The fetch→mark-read and send→refetch pairs share one connection.
- **Risk:** MED. Do P6.29 first, and decide the concurrency model at the same time.

#### P6.32 — The 5 s time-to-first-header limit aborts hub long operations server-side
- **Where:** `wireless.cpp` `c.http.setTimeout(5000)` in `hw_http_request` and `http_open`. `HTTPClient::handleHeaderResponse` returns `READ_TIMEOUT` after 5 s with no data.
- **Affected paths:**
  - `/api/notes/sync` (list + blobs + tree + commit + ref on GitHub), routinely more than 5 s on a first sync.
  - The Telegram proxy's upstream timeout is 15 s.
  - The weather proxy's cache miss can take up to 8 s.
- **Consequence:** the device closes the socket, and Go cancels `r.Context()`. The sync aborts partway, or the cache fill is lost, and the device retries the whole thing.
- **Fix:**
  - Add a per-call timeout argument: ~60–90 s for sync, ~12 s for hub proxies.
  - Keep server upstream timeouts below the device's.
  - Optionally detach cache fills from `r.Context()`.
- **Risk:** LOW.

#### P6.33 — Notes sync uploads every note twice and re-sends notes the remote already has (extends P2.10/D5)
- **Where:**
  - `ui_notes_sync.cpp` `run_sync_bg` makes one `hub_upload_note` POST per note: N connects and N base64 bodies.
  - `try_hub_sync` then re-reads, re-base64s and re-sends **all** notes in one body.
  - The server's `runSync` discards everything already on the remote ("additive-only").
- **Also:**
  - `hub_upload_note` builds `b64` and then copies it into `body`.
  - `body.reserve(256 + 1024*n)` under-reserves when notes average more than ~750 B.
- **Fix:**
  - (1) `/api/notes/sync` also writes the received files into the hub's `notesDir`; drop the per-note loop. That roughly halves the bytes and removes N connects.
  - (2) Make it two-phase: the device sends a manifest (name + size/CRC), the hub replies with the names it needs, and only those are sent. That is ≈ 0 bytes in steady state.
  - (3) Encode base64 directly into `body`.
- **Risk:** MED (protocol change on both sides).

#### P6.34 — Weather hub fallback ignores reachability; the hub verdict is stale after boot and wake (extends PB.10)
- **(a) Fallback ignores reachability.**
  - `ui_weather.cpp` `fetch_with_hub_fallback` checks only `hal::hub_get_url()`.
  - With the hub configured but down, each call waits out the 5 s connect timeout before going direct: +5–10 s per refresh (geo + forecast).
  - Fix: gate on `hal::hub_last_reachable()` as Telegram does, and/or use a ~1 s connect timeout for LAN URLs.
- **(b) Stale verdict after boot and wake.**
  - The status-bar probe runs every 30 ticks, and `hub_last_reachable` also has a 30 s max age.
  - lv_timers freeze in fake sleep, and nothing re-kicks the probe on wake, boot, GOT_IP or a hub-setting change.
  - Result: for up to 30 s after every wake, Telegram treats the hub as down and uses direct TLS.
  - Fix: kick a probe from `ui_resume_timers` and on GOT_IP, and raise max age to 45–60 s.
- **Risk:** LOW.

#### P6.35 — Weather downloads 168 hourly points and renders 8
- **Where:** `ui_weather.cpp` `fetch_weather` requests `&hourly=…&forecast_days=7`, while `build_hourly_cache` reads indices 0–21 in steps of 3.
- **Cost:** ~4.5–5 KB of the ~6–6.5 KB body is unused, which means ~700 extra cJSON nodes (tiny PSRAM allocations), longer TLS receive, and more parse CPU.
- **Fix:** add `&forecast_hours=24` (or `past_hours` + `forecast_hours` to keep today's 00–23).
  - The hub's forecast proxy passes the query through verbatim, so no server change is needed.
  - Confirm the anchoring against the open-meteo docs; the renderer parses the hour from the time string.
- **Risk:** LOW. After this trim, gzip toward the device isn't worth pursuing.

#### P6.36 — Voice memo: the whole base64 body is built in PSRAM, and long memos can't be sent (extends §2.4)
- **Where:** `ui_chat.cpp` `send_task` streams the SD file into an in-memory `ctx->body`.
- **Why long memos fail:**
  - At 16 kHz, 16-bit mono, one minute is ~2.56 MB of base64.
  - `HW_REC_MAX_MS` = 5 min gives ~12.8 MB, which exceeds 8 MB PSRAM and the server's `maxRequestBytes = 8<<20`.
  - Memos longer than ~3 min fail outright.
- **Fix:**
  - Stream SD → socket inside `chat_post` with a precomputed Content-Length (prefix + 4·⌈n/3⌉ + suffix).
  - Better: send the WAV raw (octet-stream/multipart). That is 25 % less airtime, and the server skips the base64 decode.
- **Risk:** MED.

#### P6.37 — TLS handshake timeout is the 120 s default
- **Mechanism:**
  - The `WiFiClientSecure` constructor sets `handshake_timeout = 120000`.
  - `setConnectTimeout(5000)` bounds only the TCP connect.
  - The handshake loop polls with `vTaskDelay(2)`.
- **Effect:** a stalled handshake can hold a single-slot worker (e.g. Telegram's) and the radio for up to 2 minutes.
- **Fix:** `c.secure.setHandshakeTimeout(10)`.
- **Risk:** LOW.

#### P6.38 — WiFi reconnect: no BSSID/channel reuse, only saved slot 0 (extends P4.17)
- **Where:** `wireless.cpp` `hw_wifi_reconnect_saved` calls `WiFi.begin(ssid, pw)` with no channel or BSSID, and only ever tries saved slot 0.
- **Effect:**
  - Every reconnect runs a full channel sweep.
  - Where only saved SSID #2 is in range, the supervisor spends 5 attempts over ~16 min, then turns WiFi off for 15 min, and **never connects**.
- **Fix:**
  - Cache the BSSID and channel of the last good association and pass them to `begin()`, falling back to a plain `begin()`.
  - On a supervisor retry, run one async scan and connect to the strongest saved SSID.
- **Risk:** LOW-MED.

#### P6.39 — `listen_interval` left at the IDF default under fake-sleep MAX_MODEM (extends PB.2)
- **Mechanism:** `WiFi.begin()` in 2.0.17 never sets it, so the default of 3 applies.
- **Fix:**
  - Set ~10 at association time: `begin(…, connect=false)` → `esp_wifi_set_config` → `esp_wifi_connect`.
  - No effect while awake (MIN_MODEM wakes on DTIM). In fake sleep it skips ~7 of every 10 beacons.
- **Risk:** LOW-MED. Bench it, and check the associated-link stability (P5 smoke step 4).

#### P6.40 — BLE advertising at 30–60 ms forever while unconnected (extends PB.12)
- **Where:** the vendored `ESP32 BLE Keyboard Fork` `BleKeyboard::begin()` sets no advertising interval, so the NimBLE default of 30–60 ms applies, including during fake sleep.
- **Fix:** advertise fast for 30 s, then at ~1 s (Apple's recommended pattern). This differs from PB.12 lever 2 (stop advertising): advertising stays up, just slower.
- **Side issue — conflicting connection params:** `setPreferredParams(160,320)` is in 1.25 ms units (200–400 ms) and conflicts with the 30–50 ms requested after connect. Drop it.
- **Side bug to verify — stray keys:**
  - `ui_media_remote.cpp` `forward_key_cb` calls `hw_set_ble_kb_char(&c)` on a single `static char`.
  - That ends in `bleKeyboard.print(const char*)`, which `strlen`s past the byte and can send stray keys.
  - Use `bleKeyboard.write((uint8_t)c)`.
- **Risk:** MED (vendor fork patch).

#### P6.41 — Server proxy responses lack Content-Length / Cache-Control (extends D14)
- **Where:** `httpx.Proxy` and the Telegram proxy write buffered bodies without `Content-Length`.
- **Effect:** anything over 2 KB goes out chunked, and the device's `HTTPClient::getString` can't `reserve()` (it reserves only when `_size > 0`).
- **Fix:** set `Content-Length`, and on a cache HIT emit `Cache-Control: max-age=<remaining TTL>`.
- **Risk:** LOW.

---

### E. Flash and partitions

#### P6.42 — The second OTA slot is 4 MiB of never-written flash (extends P3.27)
- **Where:** `lib/LilyGoLib/examples/factory/partitions.csv`:
  ```
  app0 ota_0 0x10000  0x400000
  app1 ota_1 0x410000 0x400000
  ffat fat   0x810000 0x7E0000
  ```
- **Why `app1` is unused:** there is no OTA code anywhere (see §0), so it is never written.
- **Fix:** use a single 4 MiB app partition, which grows FFat from ~7.9 MiB to ~11.9 MiB.
- **Risk:** MED, operational. Repartitioning **wipes FFat (notes)**, so it needs a backup/migration step (e.g. "Copy Internal → SD" once P6.27's bug is fixed, or notes sync). It also closes the door on future OTA, so it is a product call.

#### P6.43 — LibSSH costs static DRAM and flash even when SSH is never opened (product call)
- **Cost:**
  - 10.1 KB internal static DRAM: `ssh_ciphertab` 6.9 KB (.data), DH group values ~2.3 KB.
  - 165 KB flash: LibSSH 95 K + libsodium 70 K.
- **Options:** a library patch to make the tables `const`, or dropping SSH. LOW priority.

#### P6.44 — Unused software-blend colour formats are still enabled (extends P3.19)
- **Where:** `lv_conf.h` ~L146–153 enables the RGB888, XRGB8888, L8 and RGB565A8 blend paths, and `src/` ships no image assets.
- **Fix:** trim them using P3.19's verification procedure (build, compare map, check emulator). Flash only.

#### P6.45 — Investigate only
- **(a)** `-O2` for LVGL `draw/sw`, and/or `LV_ATTRIBUTE_FAST_MEM = IRAM_ATTR` (currently empty). Everything is `-Os`, and the 16 KB I-cache runs the blend loops from flash. IRAM competes with the internal-RAM budget.
- **(b)** `LV_USE_OS = LV_OS_FREERTOS` + `LV_DRAW_SW_DRAW_UNIT_CNT 2`, so rendering uses both cores.
- **Evaluate together:** both have uncertain gains while the draw buffers are in PSRAM. Benchmark alongside P4.20/P4.25/P6.9/P6.10.

---

## 4. Verified clean this pass (don't re-sweep)

- **Build:**
  - QIO flash at 80 MHz, quad PSRAM at 80 MHz, 240 MHz CPU; P3.16 guard in place.
  - `-Os` with `-ffunction-sections -fdata-sections --gc-sections`.
  - `CORE_DEBUG_LEVEL=0`, bootloader logging off.
  - WiFi/lwIP already try PSRAM first; hardware AES/SHA/MPI enabled.
- **Core loop and apps:**
  - The instance mutex is a standard priority-inheriting mutex.
  - `register_all` is only constructors plus one NVS purge and one `lv_timer`.
  - `queueSwitchApp` is only called from LVGL callbacks.
  - Default `onUpdate` is empty.
  - `hw_wifi_supervise` is cheap, and `hw_update_battery_history` is RAM-only (but see P6.18).
  - Boot order is LVGL before connectivity.
- **Styles:**
  - No `lv_style_init` in loops, and no leaked local `lv_style_t`.
  - Theme and msgbox styles are static and initialised once.
  - Dark theme, so buttons have no shadows.
  - `lv_obj_clear_flag(HIDDEN)` and `lv_obj_set_x` compare before writing.
- **Timers and redraws:**
  - Status-bar label text (P4.10), menu badges, media visibility, WiFi-gated tiles, media-remote status and the editor WiFi-pill timer are all change-gated.
  - Settings subpages are built lazily, once each.
  - `lv_refr_now` sites are one-shot or throttled to ≥ 100 ms.
  - Lists are bounded: journal pages of 5, file browser capped (P2.7), Telegram 20 messages plus signature skip.
- **Fonts and layers:**
  - Custom fonts are uncompressed 4 bpp; there are no scrolling or circular labels.
  - No forced layers in `src/` (no `opa_layered`, `blend_mode`, `bitmap_mask` or gradients).
  - `LV_USE_SYSMON 0`, so the refresh timer can pause.
- **Memory:**
  - P2.4 cJSON PSRAM hooks are live.
  - §2.15 MP3, recorder, WAV and NFC buffers are in PSRAM.
  - The LVGL heap is `ps_malloc`.
  - §2.3 key cache and prewarm are implemented and zeroised on lock and rotation.
  - §2.8, §2.9, §2.11, §2.6 and P3.4 are in place.
  - `base64_encode` pre-sizes; journal `save_index_file` reserves.
  - Static DRAM is ~90 KB, and the largest items are vendor-owned.
- **Storage:**
  - No byte-at-a-time I/O anywhere.
  - `hw_read_sd_stream` (3 KB chunks, lock released between chunks) is **the model pattern**.
  - WAV playback uses 4 KB PSRAM chunks with a lock per chunk.
  - FFat uses a 4 KB wear-levelling sector and prefers external RAM for FatFs allocations.
  - The prune scan is throttled and off-thread.
  - Settings sliders persist once, on exit.
- **Network:**
  - Hub-first calls already use plain HTTP on the LAN.
  - DNS is cached by lwIP.
  - WiFi scans are user-initiated and async.
  - The Telegram cadence is adaptive (P4.16), with a single worker slot and the render-signature skip.
  - The timezone lookup is local.
  - NimBLE roles are trimmed; latency 10 and 0 dBm TX are in place (PB.12).
- **Go server:**
  - Upstream clients are reused with keep-alive, and bodies are closed.
  - Caches and bodies are size-capped.
  - Chat sets Content-Length.
  - No goroutine leaks.
  - Still open from earlier phases (not re-reported): D10, D14, B1/D2a singleflight.

---

## 5. Suggested execution order

Ordered by value per unit of risk. Convention per item: one `<code>` commit plus one
`<docs>` commit, rebuild **both** `tlora_pager` and `emulator_lora_pager`, and run `native_test`.

1. **Instrument first, with no meter needed (§G.1).** Add boot-stage and PBKDF2 timing markers. This turns P6.1–P6.4 and P6.22 from estimates into numbers in one flash.
2. **Correctness bugs, independent of perf:**
   - P6.28 (Telegram hub proxy)
   - P6.29 (CA-bundle race)
   - P6.21 (stale journal previews)
   - P6.24 (stale SD read)
   - P6.27 (copy-to-SD misses notes)
   - P6.32 (hub long-op aborts)
3. **Cheap, high-value, LOW risk:**
   - P6.2 (lazy BHI260)
   - P6.3 (skip PSRAM test)
   - P6.4 (drop boot `__listDir`)
   - P6.22a (re-arm prewarm)
   - P6.7 (status-bar colour guard)
   - P6.8 (cursor blink)
   - P6.35 (weather hourly trim)
   - P6.37 (handshake timeout)
   - P6.34 (weather reachability + wake probe)
4. **Storage pass:**
   - P6.20a/c (`getNextFileName` + per-entry lock), then P6.20b with P6.21's real mtime
   - P6.23 (async SD mirror)
   - P6.25 (CBC prefix)
   - P6.26 (audio lock scopes + recorder flush)
   - P6.27 (remaining small items)
5. **Needs hardware and a feel test:**
   - P6.6 (LVGL idle + wake notify: biggest awake-idle lever)
   - P6.11 (freq bump on input)
   - P6.1 (crystal calibration, vendor patch)
   - P6.19 (SD clock: validate on several cards)
   - P6.9/P6.10 (swapped render + single buffer, check colours)
6. **Network structure:**
   - P6.30 (mbedTLS → PSRAM)
   - P6.31 (keep-alive; after P6.29)
   - P6.33 (sync manifest, server + device)
   - P6.36 (streamed voice memo)
   - P6.38 (fast reconnect)
7. **Rendering polish:** P6.12, P6.13, P6.14, P6.15, P6.16, P6.18.
8. **Product calls to surface to the user:**
   - P6.17 (scroll animation)
   - P6.42 (drop the OTA slot, wipes FFat)
   - P6.43 (SSH footprint)
   - P6.40 (BLE adv cadence)
   - P6.39 (listen interval, bench)
9. **Investigate only:** P6.44, P6.45.

---

## 6. Additions to the bench appendix

### G.1 Timing instrumentation (no meter needed)
Log `esp_timer_get_time()` deltas at each of these points, then remove the markers once
the numbers are recorded here:
- Around each `instance.begin()` stage: PSRAM test (log from `setup()` entry vs `esp_timer` at 0), `esp_enable_slow_crystal`, `initSensor`, `installSD`, `setupMSC`.
- At the first `disp_flush`.
- Around `derive_key_iv`.
- Around `lv_timer_handler` (for P6.6/P6.9/P6.13).

| Marker | Before (ms) | After (ms) |
|---|---|---|
| reset → `setup()` entry | | |
| `esp_enable_slow_crystal` | | |
| `initSensor` (BHI260 upload) | | |
| `installSD` | | |
| `setupMSC` (incl. `__listDir`) | | |
| reset → first `disp_flush` | | |
| `derive_key_iv` (one call) | | |
| SD read 64 KB @ 4 MHz vs 20 MHz | | |
| `list_entries` on SD, 100 notes | | |

### G.2 Power rows (meter)
| Row | Setup |
|---|---|
| Awake idle, static home screen, WiFi on — before P6.6/P6.7 | baseline |
| Same, after P6.6 + P6.7 | isolates LVGL idle + status-bar invalidation |
| Awake idle in the Telegram composer — before/after P6.8 | cursor blink |
| Telegram chat open 5 min, hub on — before/after P6.28 + P6.31 | TLS handshakes/min |

### G.3 Functional smoke test for this batch
1. Cold boot: time to first frame (stopwatch is fine); IMU debug page still works (P6.2).
2. Scroll lists, type fast, long-press keys (P6.6, P6.11).
3. Edit a journal note and return: its preview updates (P6.21).
4. Save repeatedly in an encrypted note: no hitch after the first save (P6.22).
5. Play an MP3 from SD while scrolling: no periodic hitch (P6.19, P6.26).
6. Record 5 min, then pull power at 4 min and check the file survives (P6.26).
7. Telegram with the hub on: list, open, send, mark-read (P6.28).
8. Parallel network: weather refresh while a Telegram poll runs, repeated 20×, no crash (P6.29).
9. First notes sync of 50+ notes through the hub completes (P6.32, P6.33).
10. Colours on the pager panel are unchanged (P6.9).
