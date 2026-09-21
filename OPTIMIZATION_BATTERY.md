# Optimization — Battery / Power Analysis & Handoff

Generated **2026-07-21** on the post-phase-3 tree (working tree at `4ed512f` + the
uncommitted P3.9/P3.14 watermark instrumentation and the unrelated settings-shortcut
feature diff). Produced by a four-way sweep (sleep/power-rails, wireless, periodic
tasks/timers, display/sensors); **every headline claim below was re-verified against
current source** before being recorded (the standing lesson from phases 1–3: reports
go stale and agents mis-read — re-verify again before *editing*).

**Relationship to the other docs:**
- `OPTIMIZATION_PROGRESS.md` — phase-1 tracker; its **Methodology** and the
  **hardware smoke-test checklist** apply verbatim here. Several PB items below add
  to that checklist.
- `OPTIMIZATION_PHASE2.md` / `OPTIMIZATION_PHASE3.md` — phases 2–3. The battery items
  they already track are cross-referenced, not duplicated: **P2.12** (WiFi power-save)
  is promoted to PB.2, **P3.12** (SD rail) to PB.5, **§1.5** (write-only gauge fields)
  to PB.14 — those stay the canonical IDs; this file adds the battery framing and the
  new neighbours they compound with.
- This file — the battery-focused handoff, numbered `PB.x`.

**⚠️ Measurement discipline (this doc's own P3.7 rule):** every mA figure below is a
**datasheet/estimate**, not a bench reading. None of this codebase's power behaviour
has ever been current-measured. Before investing in the medium/high-effort items,
put the device on a USB power meter / PPK (battery path if possible) and record the
baseline table in §Bench appendix. Do not "fix" an item whose measured cost turns out
to be noise, and do not trust the estimates over the meter.

**Build discipline (unchanged):** after every change,
`pio run -e tlora_pager && pio run -e emulator_lora_pager && pio test -e native_test`.
Anything touching task loops / ISRs / rails / sleep paths is ⚠️
**hardware-test-required** — add it to the smoke-test checklist in
`OPTIMIZATION_PROGRESS.md`.

---

## 1. Current power model (verified 2026-07-21)

The firmware has **four** power states. Only the first two are reachable without a
button press; there is **no automatic transition into any of them** (see PB.1).

### 1.1 Active (screen on, recent input)
- CPU at `user_setting.cpu_freq_mhz` (default 240 MHz).
- LVGL task wakes at **~60 Hz even with static content** — `kMaxTickMs = 16`
  (`hal/lvgl_task.cpp:41,67`) caps the sleep below LVGL's own 33 ms refresh period
  (PB.8). Each wake takes the instance mutex.
- Keyboard task polls the TCA8418 over I2C at **100 Hz** (`hal/keyboard_task.cpp`,
  `kPollMs = 10`) (PB.9). Vendor rotary task polls GPIO at 500 Hz (cheap, vendor code,
  informational only — phase-3 note stands).
- `loopTask` at 20 Hz (`factory.ino` `delay(50)`); `instance.loop()` is an EventGroup
  read, no I2C on idle ticks.
- Status-bar 1 Hz lv_timer: wall-clock from sysclock (no I2C, §2.12 done), gauge sweep
  behind a 5 s TTL when discharging, FFat file count every 60 ticks, and a
  **hub TCP probe every 10 ticks** when the hub is enabled (PB.10).
- Telegram background poll: 60 s lv_timer, spawns an HTTPS fetch when its guards pass
  (PB.11).
- Backlight: AW9364, 17 levels; default `brightness_level = 50` is clamped to max
  (16) — i.e. **default is full brightness**, and there is no auto-dim of any kind.

### 1.2 Idle-awake (no input > 2 s, screen still on)
- CPU drops to 80 MHz (`factory.ino` inactivity check). **Nothing else changes** —
  full brightness, 60 Hz LVGL, 100 Hz keyboard, all timers.
- `user_setting.disp_timeout_second` (Settings → Display, slider 0–180 s) is
  **written and persisted but never read by any sleep/dim logic** — the timeout knob
  is dead (PB.1). A device left face-up never sleeps.

### 1.3 Fake sleep (rotary center-button ≥ 1 s → vendor `perform_fake_sleep_toggle()` → `ui_pause_timers()` → `hw_power_down_all()`)
Off / gated:
- Panel: `sleepDisplay()` sends ST7796 SLPIN; backlight 0. ✅
- Rails cut (`hal/system.cpp:662-679`): keyboard (+`kb.end()`), NFC, haptic, speaker. ✅
- GPS: permanently off since boot (P2.5 ✅).
- CPU: 40 MHz — **or 80 MHz if WiFi or BLE is connected** (`hold_80`,
  `system.cpp:675-677`) (PB.20).
- LVGL / keyboard / rotary / NFC tasks: notify-block on `ulTaskNotifyTake(…, 200 ms)`
  (P2.1/P2.2/P3.13 ✅). All lv_timers are de-facto frozen because `lv_timer_handler()`
  is never called. Note the 200 ms timeouts mean each of these four tasks still
  wakes 5×/s as a fallback (PB.16).

Still running / still powered — **this is the fake-sleep gap list**:
- **SX1262 in STANDBY** (~0.6–1.6 mA). `radio.sleep()` is never called anywhere in
  `src/`; even the settings "radio off" maps `RADIO_DISABLE` → `radio.standby()`
  (`hal/radio/sx1262.cpp:68`) (PB.3).
- **BQ25896 continuous ADC measurement stays enabled** (~0.5–1 mA); the vendor's own
  `lightSleep()` calls `ppm.disableMeasure()` — our fake sleep doesn't (PB.4).
- **SD rail on** (~0.5–1 mA), deliberate skip with comment (P3.12 / PB.5).
- **BHI260 keeps running 3 virtual sensors** (accel 25 Hz, rotation 25 Hz,
  orientation 5 Hz), filling a FIFO nobody drains during sleep (~0.5–1 mA) — and the
  feature they feed (`glance_show()`) has **zero call sites** (PB.6).
- **WiFi fully associated** at the Arduino default power-save (`WIFI_PS_MIN_MODEM`;
  no explicit `esp_wifi_set_ps`/`WiFi.setSleep` call exists) (PB.2), and it forces
  the 80 MHz CPU floor (PB.20).
- **BLE**: if the BLE keyboard is enabled, connection params force a wake per
  connection event (slave latency 0, 30–50 ms interval), advertising runs forever
  when unconnected, and the 1 Hz `ble_kb_ka` task is not fake-sleep gated (PB.12).
- Charge task polls `ppm.isVbusIn()` over I2C at 2 Hz (its awake state blocks until
  sleep entry — this task only costs during fake sleep) (PB.13). `loopTask` at 2 Hz;
  vendor rotary at 10 Hz (GPIO-only, needed for the wake long-press).

### 1.4 True light sleep / deep sleep (PMU button long-press → `hw_low_power_loop()`)
- `sleep_mode == 0`: vendor `lightSleep()` → `esp_light_sleep_start()`. The vendor
  path cuts **everything** our fake sleep leaves on: `ppm.disableMeasure()`,
  `radio.sleep()`, SD unmount + rail cut, all expander rails
  (`lib/LilyGoLib/src/LilyGo_LoRa_Pager.cpp:678-764`). Wake source: boot button only.
- `sleep_mode == 1`: `hw_sleep()` → deep sleep, notes passphrase locked first.
- **No ESP-IDF automatic light sleep / tickless idle / `esp_pm_configure` anywhere**
  (PB.21, investigate-only).

The one-line summary: **fake sleep is the state the device actually lives in, and it
was tuned for CPU/lock churn (phases 2–3), not for rail/chip power. The vendor's
`lightSleep()` is the reference for what fake sleep still leaves on.**

---

## 2. Already-tracked battery items — status (do not redo)

| ID | Item | Status |
|---|---|---|
| P2.1 | Keyboard task fake-sleep notify-block | ✅ done (HW smoke-test still owed) |
| P2.2 | NFC task gate + rail cut when disabled/sleeping | ✅ done (HW smoke-test still owed) |
| P2.5 | GPS rail transient-only, toggle removed | ✅ done |
| P3.13 | Rotary task fake-sleep notify-block | ✅ done (HW smoke-test still owed) |
| §2.12 | Status-bar sysclock + gauge 5 s TTL | ✅ done |
| P3.16 | Double `setCpuFrequencyMhz` at boot | ✅ done (but see PB.17 — the *transition* double-set is new and different) |
| P2.12 | WiFi power-save | ✅ done → **PB.2** |
| P3.12 | SD rail through fake sleep | ⏸ deferred pending bench → **PB.5** (remount unverifiable without HW) |
| §1.5 | Write-only `monitor_params_t` gauge fields | ✅ done → **PB.14** (9 removable register reads incl. the `.temperature` correction — landed 2026-07-21) |

---

## 3. Executive summary — new opportunities

Estimates assume the ~1000 mAh battery the P3.12 analysis cites. Fake-sleep
parasitics stack: closing PB.2–PB.6 is worth an estimated **3–8 mA continuous during
sleep** (more if WiFi stays connected), i.e. potentially days of standby.

**Status legend (updated 2026-07-21):** ✅ landed (code in tree, builds green, ⚠️ HW smoke-test still owed) · 🔶 partial (some parts landed, remainder deferred) · ⏸ deferred (documented disposition — bench/HW/product-gated; see the item's Resolved note).

| # | Finding | Est. impact | Risk / effort |
|---|---|---|---|
| PB.1 ✅ | `disp_timeout_second` is a dead knob — **no automatic sleep at all** | Largest real-world lever (display + everything stays on indefinitely) | Med effort, Low risk / ⚠️ HW |
| PB.2 ✅ | WiFi PS never configured; escalate to `WIFI_PS_MAX_MODEM` in fake sleep (= P2.12) | ~1–5 mA in sleep w/ WiFi | Low / product latency call |
| PB.3 ✅ | SX1262 never sleeps — STANDBY forever, even "disabled" | ~0.6–1.6 mA, 24/7 | Low code / ⚠️ HW (boot/ISR adjacency) |
| PB.4 ✅ | BQ25896 ADC continuous through fake sleep (vendor sleeps it) | ~0.5–1 mA in sleep | Very low / ⚠️ HW trivial |
| PB.5 ⏸ | SD rail through fake sleep (= P3.12) — deferred pending bench (remount unverifiable without HW) | ~0.5–1 mA in sleep | Med (remount) / ⚠️ HW |
| PB.6 ✅ | BHI260 3 virtual sensors 24/7 feeding a never-called feature; `hw_unregister_imu_process()` only disables 1 of 3 | ~0.5–1 mA, 24/7 | Low (option B) / product call (option A) |
| PB.7 🔶 | NFC runs full RF-field polling; ST25R3916 Wake-Up mode (~75 µA) unused — poll 20→100 landed; WU mode deferred (⚠️ HW) | 5–15 mA whenever NFC enabled | Med / ⚠️ HW |
| PB.8 ✅ | LVGL task wakes 60 Hz on static content (`kMaxTickMs=16`) | CPU churn + mutex contention, awake | Low |
| PB.9 ✅ | Keyboard 100 Hz I2C poll awake (`kPollMs` 10 → 20–30) | I2C/lock churn, awake | Low / ⚠️ HW feel-check |
| PB.10 ✅ | Hub TCP probe every 10 s (cache TTL is 30 s) | 1 TCP+task spawn/10 s | Trivial — **done 2026-07-21** |
| PB.11 ⏸ | Telegram bg poll every 60 s (TLS round-trip) — kept 60 s (product decision recorded) | WiFi airtime each minute | Product call |
| PB.12 🔶 | BLE: slave latency 0; advertising forever; `ble_kb_ka` 1 Hz un-gated; TX power default — latency/gating/TX-power landed; advertising-timeout deferred | ~1–2 mA connected; ~0.1–0.3 mA advertising | Low–Med / ⚠️ HW |
| PB.13 ⏸ | Charge task 2 Hz I2C VBUS poll in sleep → PMU VBUS-insert IRQ — **not implementable: pager doesn't route the BQ25896 /INT to a GPIO** | small I2C churn in sleep | Med (IRQ plumbing) / ⚠️ HW |
| PB.14 ✅ | Gauge sweep reads 9 write-only registers per sweep (= §1.5, +`.temperature` correction) | I2C churn each sweep | Low / ⚠️ HW pass |
| PB.15 ✅ | `hw_get_battery_voltage()` bypasses the TTL cache — full `gauge.refresh()` at 1 Hz on the info page | I2C churn while page open | Trivial — **done 2026-07-21** |
| PB.16 ✅ | Fake-sleep notify-blocks use 200 ms timeouts → 4 tasks × 5 wakes/s | scheduler churn in sleep | Low |
| PB.17 ✅ | CPU freq set twice on every fake-sleep entry/exit (`hw_power_*_all` + `loop()` both own it) | cosmetic (~0.5 ms PLL) | Trivial |
| PB.18 ✅ | `hw_light_sleep()` dead code; would skip `notes_crypto_lock()` if ever called | hygiene + latent security gap | Trivial — **done 2026-07-21** |
| PB.19 ⏸ | `speaker_enable=1` latches the amp rail on between sessions (default off; codec PA-callback self-gates playback) — flag has zero consumers; product-semantics call | 2–8 mA only if user enables toggle | Low |
| PB.20 ⏸ | 80 MHz fake-sleep CPU floor whenever WiFi/BLE connected — prereqs (PB.2/PB.17) landed; 40 MHz stability needs HW soak | ~20–30 mW in sleep | Med / ⚠️ HW, compound with PB.2 |
| PB.21 ⏸ | No ESP-IDF auto light sleep / tickless idle; no periodic true-light-sleep during fake sleep — `CONFIG_PM_ENABLE` off in the pinned core (needs custom IDF rebuild) | potentially the deepest saver | Investigate-only / high |

---

## 4. Detailed findings

### A. Structural — make sleep happen, then close its gaps

#### PB.1 — `disp_timeout_second` is a dead setting: the device never sleeps on its own (HIGHEST VALUE)

**Where:** `apps/settings_display.cpp:57-68,235-241` (slider writes + persists it),
`hal/types.h:184` (field), `hal/system.cpp:343,418` (default 0), and — the gap —
`factory.ino` reads `lv_display_get_inactive_time(NULL)` only to drop the CPU to
80 MHz at > 2 s; **nothing anywhere compares the inactivity clock to
`disp_timeout_second`**. Grep-verified: the only reads of the field are the settings
UI's own display of it.

**Cost today:** every mA in this document is conditional on the user remembering the
rotary long-press. A pager left on a desk runs full backlight (default = max
brightness — the default `brightness_level = 50` clamps to the AW9364 max of 16),
60 Hz LVGL, 100 Hz keyboard I2C, radio standby, IMU, forever. This dwarfs every
other item.

**Fix direction:** in `factory.ino::loop()` where `inactive_time` is already read:
when `!ui_is_fake_sleep() && disp_timeout_second > 0 && inactive_time >= timeout`,
enter fake sleep. Two plumbing notes:
1. The entry point is the vendor's `perform_fake_sleep_toggle()`
   (`LilyGo_LoRa_Pager.cpp:1301`), driven from its rotary task. Phase-1 removed the
   `lilygo_request_fake_sleep_toggle` extern as dead — **re-introduce that hook** (or
   mirror the toggle body: `setBrightness(0)` + `sleepDisplay()` + `ui_pause_timers()`)
   rather than duplicating the sequence ad-hoc. Keep entry/exit symmetric with the
   long-press path so wake behaves identically.
2. Guards: don't fire while audio is playing/recording (`playerEvent` bits /
   recorder running), while an SSH session is connected, or while text input is
   focused (`core::isTextInputFocused()` — same guard the telegram poll uses).
   Encoder/keyboard activity already resets the LVGL inactivity clock via the input
   drivers, so wake-then-idle re-arms correctly for free.

**Optional stage 2 (separate commit):** two-stage dim — at `timeout − 5 s` drop
brightness to ~2 as a warning (vendor `decrementBrightness()` exists; `hw_shutdown()`
already uses it), restore on interaction. UX polish, small extra state.

**Risk:** Low-Med. The mechanism is proven (it's the long-press path); the new part
is only the trigger + guards. ⚠️ HW: verify auto-entry never fires mid-playback /
mid-sync, and that wake latency matches the manual path (P2.1/P3.13 checklist items
already cover first-input-after-wake).

**✅ Landed 2026-07-21:** trigger wired in `factory.ino::loop()` (awake else-branch,
after the existing CPU-freq block). `disp_timeout_second` is cached in the same
once-per-second `hw_get_user_setting()` pass as `cpu_freq_mhz` — no extra struct copy
per tick. The trigger fires `lilygo_request_fake_sleep_toggle()` (re-introduced
`extern "C"` declared in `factory.ino` — the vendor rotary task consumes the flag and
runs `perform_fake_sleep_toggle()`, keeping entry/exit symmetric with the long-press
path) when: `!auto_sleep_pending && cached_disp_timeout_sec > 0 && inactive_time >=
(uint32_t)cached_disp_timeout_sec * 1000 && !hw_player_running() &&
!hw_rec_running() && !core::isTextInputFocused() && !ssh_session_is_active()`. The
SSH guard required a new accessor: `ssh_session_is_active()` (declared in
`src/core/system_hooks.h`, defined in `src/apps/ui_ssh.cpp`) backed by a file-scoped
`volatile bool s_ssh_session_active` set/cleared at the `State::Connected` /
teardown transitions in both `LibSshBackend::run_session()` (ARDUINO) and
`LoopbackBackend` (emulator stub) — so `emulator_lora_pager` links cleanly without
any separate fallback. Async latch: `static bool auto_sleep_pending` is set `true`
immediately after the `lilygo_request_fake_sleep_toggle()` call and cleared in the
`if (ui_is_fake_sleep())` branch once the rotary task has processed the request and
`ui_is_fake_sleep()` is confirmed — prevents loop() from re-firing the toggle on
every subsequent 50 ms tick before the state propagates. Stage-2 two-stage dim
intentionally deferred to a separate commit. ⚠️ HW smoke-test still owed: verify
auto-entry never fires mid-playback/mid-sync and that wake latency matches the manual
long-press path.

#### PB.2 — WiFi power-save (promotes P2.12): explicit MIN_MODEM + escalate to MAX_MODEM during fake sleep

**Where:** `hal/wireless.cpp:125-144` (`hw_set_wifi_enable` — `WiFi.mode(WIFI_STA)`
with no power-save call anywhere after it; grep-verified zero
`WiFi.setSleep`/`esp_wifi_set_ps` in `src/`). Fake-sleep entry/exit:
`hal/system.cpp` `hw_power_down_all()`/`hw_power_up_all()`.

**Cost today:** the stack runs at the Arduino-ESP32 *default* (`WIFI_PS_MIN_MODEM`
per the phase-2 analysis — worth confirming once on serial, since nothing in this
tree ever sets it). MIN keeps the RF front-end waking every DTIM beacon; MAX lets it
sleep for the listen interval, saving an estimated 1–5 mA while associated — exactly
the state fake sleep spends hours in. Auto-reconnect is already off
(`factory.ino:108`), so there is no reconnect churn to worry about.

**Fix direction:**
- On enable: `WiFi.setSleep(WIFI_PS_MIN_MODEM)` explicitly (determinism, not a
  behaviour change).
- In `hw_power_down_all()`: `esp_wifi_set_ps(WIFI_PS_MAX_MODEM)` when connected;
  restore MIN in `hw_power_up_all()`.

**Trade-off (the product decision phase 2 parked):** MAX_MODEM adds up to a few
hundred ms latency to *inbound* packets during sleep. The only background consumer
is the 60 s telegram poll — outbound-initiated, entirely tolerant. If in doubt, gate
behind a settings toggle ("Deeper WiFi sleep").

**Risk:** Low. ⚠️ HW: confirm telegram bg poll still completes during fake sleep
(it doesn't — lv_timers freeze — so really: confirm reconnect/first-fetch-after-wake
is unaffected) and that the hub probe TTL behaviour is unchanged after wake.

**✅ Landed 2026-07-21:** `wireless.cpp` gains `<esp_wifi.h>` (guarded inside the
existing `#ifdef ARDUINO` include block). `hw_set_wifi_enable()` now calls
`WiFi.setSleep(WIFI_PS_MIN_MODEM)` immediately after `WiFi.mode(WIFI_STA)` — making
the Arduino default explicit so the wake-restore baseline is always known. Two new
wrappers added to `wireless.cpp` + declared in `wireless.h`: `hw_wifi_powersave_sleep()`
(escalates to `WIFI_PS_MAX_MODEM` via `esp_wifi_set_ps` if connected, no-op otherwise)
and `hw_wifi_powersave_active()` (restores `WIFI_PS_MIN_MODEM` if connected). Both are
no-ops in the emulator build (`#ifdef ARDUINO` guarded with empty else path). Wired into
`system.cpp`: `hw_power_down_all()` calls `hw_wifi_powersave_sleep()` and
`hw_power_up_all()` calls `hw_wifi_powersave_active()` — `system.cpp` already reaches
`wireless.h` through `hal_interface.h`, no extra include needed. Settings toggle
deliberately skipped: LVGL timers (including the 60 s Telegram poll) are frozen for the
entire fake-sleep interval, so the inbound-latency trade-off of MAX_MODEM is
irrelevant — there is no background consumer that could be penalised. ⚠️ HW smoke-test
still owed: confirm reconnect and first-fetch-after-wake are unaffected, and that
hub-probe TTL behaviour is unchanged after wake.

#### PB.3 — SX1262 never leaves STANDBY: sleep it when disabled and on fake-sleep entry

**Where:** `hal/radio/sx1262.cpp:68` — `case RADIO_DISABLE: state = radio.standby();`
(and `hal/radio.cpp:22-26`: the settings "radio off" path ends there). No
`radio.sleep()` call exists in `src/` (grep-verified). `hw_power_down_all()` doesn't
touch the radio. Vendor `lightSleep()` *does* call `radio.sleep()`
(`LilyGo_LoRa_Pager.cpp:687`).

**Cost today:** SX1262 STANDBY_RC ≈ 0.6–1.6 mA vs < 1 µA in sleep — continuously,
in every state, regardless of the radio toggle. On a ~1000 mAh battery that is
~15–40 mAh/day for a chip this firmware never TX/RXes with (phase-1 established the
radio is configured-but-idle).

**Fix direction (two independent halves):**
1. `RADIO_DISABLE` → `radio.sleep()` in `configure()`. RadioLib requires a wake
   (standby/begin) before the next register write after sleep — put the wake at the
   top of `configure()` (or in `hw_set_radio_default()`), so every reconfigure path
   is self-healing.
2. `hw_power_down_all()`: `radio.sleep()` under `core::ScopedSpiLock`;
   `hw_power_up_all()`: re-apply `hw_set_radio_enable(user_setting.radio_enable)`.

**Risk:** Low in code, but ⚠️ **HW-test-required with extra caution**: this sits next
to the phase-1 radio dead-code passes that are *themselves* still awaiting the
hardware smoke-test (boot ISRs kept conservatively). Land PB.3 only after (or
together with) that checklist run: boot, toggle radio in settings, sleep/wake cycle,
no SPI errors on serial.

**✅ Landed 2026-07-21:** Two independent halves. **Half 1 (RADIO_DISABLE→sleep +
configure() self-heal):** In `src/hal/radio/sx1262.cpp`, added `radio.standby()` at
the very top of the `#ifdef ARDUINO` body in `configure()` (before the first
`radio.set*()` call) so every reconfigure path — TX, RX, and RADIO_DISABLE — is
self-healing after a prior `sleep()`; changed `case RADIO_DISABLE:` from
`radio.standby()` to `radio.sleep()`. **Half 2 (new `radio_chip::sleep()` API +
power-down/up wiring):** Added `int16_t sleep();` declaration to
`src/hal/radio_chip.h`; implemented it in all four standard chip drivers
(`sx1262.cpp`, `cc1101.cpp`, `sx1280.cpp`, `lr1121.cpp`) mirroring each file's
existing `#ifdef ARDUINO` / `return 0` stub convention. `nrf2401.cpp` was skipped —
it does not implement the `radio_chip::` interface (no `configure()` or
`default_params()`), and adding `sleep()` there alongside a primary chip driver
would cause an ODR violation. In `hw_power_down_all()` (`src/hal/system.cpp`): added
`radio_chip::sleep()` under a `core::ScopedSpiLock` with a comment tying it to
vendor `lightSleep()` parity (`LilyGo_LoRa_Pager.cpp:687`); added `#include
"radio_chip.h"` and `#include "../core/spi_lock.h"`. In `hw_power_up_all()`: added
`hw_set_radio_enable(user_setting.radio_enable)` — this restores the user-configured
radio state; `configure()`'s top-of-function `radio.standby()` self-heals the chip
from sleep before re-applying settings. **SPI-lock decision:** `hw_set_radio_params()`
in `radio_common.cpp:61` already acquires `core::ScopedSpiLock` before calling
`radio_chip::configure()`, so the re-enable path in `hw_power_up_all()` is
self-locking — no extra lock added there. The new `radio_chip::sleep()` in
`hw_power_down_all()` bypasses that path and needs its own lock, which was added.
`user_setting.radio_enable` is not touched in `hw_power_down_all()` — intent is
preserved across wake. All three builds green (tlora_pager, emulator_lora_pager,
native_test). ⚠️ HW smoke-test still owed (and must precede or accompany the
phase-1 radio dead-code checklist run): boot, toggle radio in settings, sleep/wake
cycle, no SPI errors on serial.

#### PB.4 — BQ25896: disable continuous ADC during fake sleep (vendor parity, near-free)

**Where:** `hw_power_down_all()` (`hal/system.cpp:662-679`) — no
`instance.ppm.disableMeasure()`. Vendor `lightSleep()` calls it on entry and
`enableMeasure()` on wake (`LilyGo_LoRa_Pager.cpp:685,756`).

**Cost today:** continuous ADC conversion ≈ 0.5–1 mA for data nobody consumes during
sleep — the LVGL task is blocked, so no gauge sweep runs; the charge task reads only
`isVbusIn()` (a status register, not ADC-dependent — verify this claim on hardware
once: plug-in detection while asleep is the one behaviour that must survive).

**Fix direction:** `disableMeasure()` in `hw_power_down_all()`, `enableMeasure()` in
`hw_power_up_all()`, mirroring the vendor. ⚠️ HW: charger-plug during fake sleep
still wakes the display (the charge task's VBUS edge), and charging state renders
correctly after wake.

**✅ Landed 2026-07-21:** Added `instance.ppm.disableMeasure()` at the top of
`hw_power_down_all()` (`src/hal/system.cpp:661`) and `instance.ppm.enableMeasure()`
at the end of `hw_power_up_all()` (`src/hal/system.cpp:695`), each guarded by
`#if defined(USING_PPM_MANAGE)` (T-LoRa-Pager only; Watch-S3/Ultra use PMU, emulator
defines neither). Directly mirrors vendor `lightSleep()` parity
(`LilyGo_LoRa_Pager.cpp:685,756`). All three builds green (tlora_pager, emulator_lora_pager,
native_test). ⚠️ HW smoke-test still owed: charger-plug during fake sleep must still
wake the display via the charge task's VBUS-status read (`isVbusIn()`, not
ADC-dependent), and charging must render correctly after wake.

#### PB.5 — SD rail through fake sleep (= P3.12, unchanged, restated for completeness)

**Where:** `hal/system.cpp:671` comment "SD Card is left on to avoid mount/unmount
overhead" (was :668 in the original write — upstream changes shifted it by three lines);
vendor `lightSleep()` does `uninstallSD()` (function defined at :562) + rail cut
(`LilyGo_LoRa_Pager.cpp:701–702`).

Still open, still the same shape: ~0.5–1 mA for hours of sleep, fix needs a quiesce
gate (no bulk/prune/sync in flight) + reliable re-`SD.begin()` on wake.
**Bench-measure the actual mA before paying the remount-complexity cost** — this is
the flagship example of an item the meter might demote. Tracked as P3.12; keep that ID.

**Resolved 2026-07-21 — deferred pending bench (P3.12):**

Re-verified line numbers (2026-07-21): SD-skip comment is `hal/system.cpp:671`
(function `hw_power_down_all()`); vendor `uninstallSD()` call + rail cut at
`LilyGo_LoRa_Pager.cpp:701–702` (function definition at :562); both stable.

**In-flight SD writer/consumer map:**

| Consumer | SD operation | PB.1 auto-sleep guard | Manual long-press |
|---|---|---|---|
| Audio recorder (`hal/audio.cpp:502`) | `SD.open(WAV, FILE_WRITE)` — active WAV write | `!hw_rec_running()` ✓ | None ✗ |
| Audio player (`hal/audio.cpp:202,243`) | `SD.open(track)` — streaming read | `!hw_player_running()` ✓ | None ✗ |
| Notes sync bg task (`apps/ui_notes_sync.cpp:487`) | `SD.open()` reads via `notes_crypto.cpp` + `storage.cpp` | None ✗ | None ✗ |
| Bulk copy Internal→SD (`hal/storage_bulk.cpp:170`) | `SD.open(dst, "w")` — synchronous on LVGL task | None ✗ | None ✗ |
| File browser (`apps/ui_file_browser.cpp`) | `SD.open()` directory listing | None (read-only, no persistent handle) | None |
| Journal / Audio Notes / Chat apps | `SD.open()` reads+writes via `storage.cpp` | None (active note edits) | None ✗ |

PB.1's guards (`!hw_player_running()`, `!hw_rec_running()`, `!core::isTextInputFocused()`,
`!ssh_session_is_active()` in `factory.ino:248–254`) protect only the **auto-sleep path**.
The **manual long-press path** (vendor `lilygo_request_fake_sleep_toggle()` via the rotary
task) carries **no SD quiesce gate at all**. Notes sync (`notes_sync_bg_task` FreeRTOS
task, `ui_notes_sync.cpp:487`) and bulk copy (LVGL-task synchronous,
`storage_bulk.cpp:170`) are unguarded on both paths.

**Quiesce-gate + remount sketch** (bench session pick-up):

1. Add `volatile bool g_sd_op_in_flight` (or `std::atomic<bool>`), exposed via
   `bool hw_sd_op_in_flight()` in `hal/storage.h`. Set/clear it:
   - in `notes_sync_bg_task` around the SD-read phase (`ui_notes_sync.cpp:~413`);
   - in `hw_copy_all_notes_to_sd()` before/after the `SD.open()` loop
     (`storage_bulk.cpp:~170`).
2. Extend the auto-sleep gate (`factory.ino:248`) to also check
   `!hw_sd_op_in_flight()`.
3. Add a symmetric gate on the manual long-press path: wherever the rotary task
   calls `lilygo_request_fake_sleep_toggle()`, refuse if `hw_sd_op_in_flight()`,
   `hw_rec_running()`, or `hw_player_running()`.
4. In `hw_power_down_all()` (after the quiesce checks pass): `instance.uninstallSD()`
   then `instance.powerControl(POWER_SD_CARD, false)` — mirrors `lightSleep()` at
   `LilyGo_LoRa_Pager.cpp:701–702`.
5. In `hw_power_up_all()`: `instance.powerControl(POWER_SD_CARD, true)` then, under
   `core::ScopedSpiLock`, `SD.begin(SD_CS, instance.getSPI(), 25000000)`. On success:
   `SD.mkdir(NOTES_DIR)` and `SD.mkdir(CHAT_DIR)` (mirrors `ui_audio_notes.cpp:125`
   and `ui_chat.cpp:666`). Treat mount failure as a recoverable error and log it;
   storage.h callers already tolerate a missing SD via `HW_SD_ONLINE` flag checks.
6. **Bench first:** measure the actual mA delta before writing any of this. The meter
   decides whether ~0.5–1 mA estimate holds and whether the remount complexity is
   worth paying — the whole point of §7 step 3.

**Reason for deferral:** no bench meter available to confirm the estimate (this doc's own
measurement-discipline rule, §⚠️). SD remount (`SD.begin()`) is **unverifiable without
hardware** — build-green does not mean safe; a failed silent remount corrupts the next
note write. The quiesce gate requires coordinating three separate SD-writing paths
(recorder task, notes-sync FreeRTOS task, bulk-copy on LVGL task) before sleep entry
is safe. This item joins the fake-sleep gap bench session (§7 step 3) when hardware is
available; the bench number then decides whether the remount complexity is justified.
Doc-only update; no code was changed.

#### PB.6 — BHI260 runs three virtual sensors 24/7 for a feature with zero call sites

**Where:** `apps/app_registry.cpp:32` registers the IMU pipeline at boot
(comment says it drives the glance overlay); `hal/sensors.cpp:175-201` configures
ACCEL_PASSTHROUGH @ 25 Hz, GAME_ROTATION_VECTOR @ 25 Hz, DEVICE_ORIENTATION @ 5 Hz.
**`apps::menu::glance_show()` (`menu_glance.cpp:174`) has zero call sites** — the
only reference is a comment in `menu_app.cpp:316`. The sole live consumer of IMU
data is the settings IMU-debug subpage (page-gated). During fake sleep the chip
keeps computing into a FIFO nobody drains (`sensor.update()` runs from the blocked
LVGL-task path).

**Prerequisite bug:** `hw_unregister_imu_process()` (`hal/sensors.cpp:219-232`)
disables **only** GAME_ROTATION_VECTOR — ACCEL and DEVICE_ORIENTATION keep running
after "unregister". Fix first (two more `configure(…, 0, 0)` calls), whatever else
is decided.

**Cost today:** ~0.5–1 mA continuously (datasheet, three virtual sensors active).

**Fix direction — a product fork:**
- **Option A (wire the feature):** implement the intended raise-to-glance: a modest
  lv_timer sampling `hw_is_face_down()` edges → `glance_show()`. Keeps the IMU on
  with a reason. (Then PB.6 reduces to "suspend sensors during fake sleep".)
- **Option B (power-first):** call the (fixed) `hw_unregister_imu_process()` from
  `hw_power_down_all()` and re-register in `hw_power_up_all()`; optionally don't
  register at boot at all until a consumer exists.

**Risk:** Low (option B). ⚠️ HW: IMU-debug subpage still shows live data after a
sleep/wake cycle; re-register I2C cost (~50 ms) doesn't hurt wake feel.

**✅ Landed 2026-07-21:** Two parts. **Part 1 (unregister bug fix):** `hw_unregister_imu_process()` (`src/hal/sensors.cpp:219`) previously disabled only `GAME_ROTATION_VECTOR`; added the two missing `configure(…, 0, 0)` calls for `ACCEL_PASSTHROUGH` and `DEVICE_ORIENTATION` so all three virtual sensors that `hw_register_imu_process()` enables are now properly disabled on unregister. A comment was added noting that the three calls must mirror `hw_register_imu_process()` exactly. **Part 2 (suspend/re-register wiring):** In `hw_power_down_all()` (`src/hal/system.cpp`): added `hw_unregister_imu_process()` after the radio sleep block with a comment explaining that during fake sleep the LVGL task is blocked so `sensor.update()` never runs and the BHI260 FIFO fills pointlessly (~0.5–1 mA waste). In `hw_power_up_all()`: added `hw_register_imu_process()` after `hw_set_radio_enable()`, restoring all three virtual sensors on wake. BHI260 is I2C, not SPI — no `ScopedSpiLock` needed at either call site. **Option A (wire the glance feature)** was deliberately left unimplemented — that is a product call, not a power call. The re-register path includes the full sensor-table dump in `hw_register_imu_process()` (~50 ms) — acceptable per the doc. The include path for both functions was already present: `system.cpp` includes `../hal_interface.h`, which includes `hal/sensors.h` where both are declared — no new include required. All three builds green (`tlora_pager`, `emulator_lora_pager`, `native_test`). ⚠️ HW smoke-test still owed: IMU-debug subpage shows live data after a sleep/wake cycle.

#### PB.7 — NFC: full RF-field polling instead of ST25R3916 Wake-Up mode

**Where:** `hal/nfc_reader.cpp:664-666` — `totalDuration = 1000U`,
`discover_params.wakeupEnabled = false`; poll loop at `kPollMs = 20`
(`hal/nfc_task.cpp:33`). The RFAL state for WU mode is already handled in
`demoNotif()` (`nfc_reader.cpp:107`).

**Cost today:** only when the user enables NFC (default off — P2.2 already made the
disabled case free): continuous field-on polling ≈ 5–15 mA. WU mode (chip wakes the
host on amplitude/phase change) ≈ 75 µA — two orders of magnitude.

**Fix direction:** flip `wakeupEnabled = true` and let RFAL manage
WU-detect → full poll → back to WU; independently, `kPollMs` 20 → 100 is a free 5×
cut in lock traffic with no perceptible tap latency.

**Risk:** Med for WU mode (tag-tech coverage — verify type A/B/F + the NDEF flows in
`ui_nfc_test`); Low for the poll-interval bump. ⚠️ HW both.

**✅ Landed 2026-07-21 (Part A) / deferred (Part B):** **Part A (`kPollMs` 20 → 100):**
`constexpr uint32_t kPollMs = 100` at `src/hal/nfc_task.cpp:33`; inline comment updated to
explain ~10 Hz cadence and the 5× cut in instance-lock traffic with no perceptible tap
latency (RFAL responds as soon as a tag is detected, not on this timer boundary).
⚠️ HW feel-check still owed: brief hands-on tap-latency verification with a real tag to
confirm 100 ms cadence is imperceptible in practice. **Part B (ST25R3916 Wake-Up mode):**
deferred — **do NOT enable blind.** Exact one-line change when ready:
`discover_params.wakeupEnabled = true` at `src/hal/nfc_reader.cpp:667` (re-verified against
current source; the doc previously cited :665 — lines shifted). `discover_params.totalDuration`
remains `1000U` at :666. `demoNotif()` (`nfc_reader.cpp:105`) already handles
`RFAL_NFC_STATE_WAKEUP_MODE` and `RFAL_NFC_STATE_POLL_TECHDETECT`, so the RFAL state-machine
path is already in place — only the one `wakeupEnabled` flag needs flipping. Cannot be
verified here (no NFC tags/reader; emulator does not exercise NFC); enabling blind risks
silently breaking all tag reads across tag technologies. HW verification checklist before
landing: (1) type A tag detection and NDEF read + write via `ui_nfc_test`; (2) type B and
type F tag detection; (3) confirm WU-detect → full-poll → back-to-WU cycle completes
correctly; (4) no regression in the full `ui_nfc_test` NDEF read/write flows. NFC is
default-off (P2.2), so the disabled-case cost is already zero — WU mode only matters when
the user enables NFC, and getting it wrong silently makes NFC non-functional. All three
builds green (tlora_pager SUCCESS, emulator_lora_pager SUCCESS, native_test 28/28 PASSED).

### B. Awake-state churn (CPU/I2C/network cadences)

#### PB.8 — LVGL task wakes at 60 Hz with static content

**Where:** `hal/lvgl_task.cpp:41,67` — `lv_timer_handler()` returns the real next
deadline (≥ 33 ms when idle, `LV_DEF_REFR_PERIOD 33`), then
`if (next > kMaxTickMs) next = kMaxTickMs;` clamps every sleep to 16 ms.

**Cost today:** ~60 instance-mutex acquisitions/s on core 1 for nothing, and it's
the dominant contender against the keyboard/NFC tasks. The 16 ms cap predates input
moving to its own tasks — there is no responsiveness argument left: input events
arrive via queues/notifies, and `hw_lvgl_task_notify_wake()` already exists to kick
the task out of its sleep early.

**Fix direction:** honor `lv_timer_handler()`'s return (cap at ~200 ms for safety,
e.g. `kMaxTickMs = 200`), and audit that every input path that needs an immediate
frame kicks `hw_lvgl_task_notify_wake()` (keyboard/rotary already feed LVGL indevs —
verify the kick, not assume it). ⚠️ HW: scroll/typing feel, animation smoothness.

**✅ Landed 2026-07-21:** `kMaxTickMs` raised 16 → 200 in `src/hal/lvgl_task.cpp:41`
(comment updated to explain the task now honors `lv_timer_handler()`'s own deadline —
active animations/timers return small deadlines and are unaffected; a fully static
screen sleeps up to 200 ms, cutting ~60 unnecessary mutex acquisitions/s). Input
immediacy preserved by adding `hw_lvgl_task_notify_wake()` to `enqueue_event()` in
`src/hal/keyboard_task.cpp:90` and `src/hal/rotary_task.cpp:66` — both call the wake
unconditionally after the enqueue (the xTaskNotifyGive is idempotent; multiple gives
before the task's ulTaskNotifyTake collapse to one). Input choke-point audit: all
key/release/auto-repeat/pingkey events in keyboard_task pass through the single
`enqueue_event()` at `keyboard_task.cpp:81`; all scroll/click events in rotary_task
pass through `enqueue_event()` at `rotary_task.cpp:57`. `lv_async_call` is used only
from within LVGL event handlers (on the LVGL task itself) so it needs no kick.
All three builds green (tlora_pager, emulator_lora_pager, native_test, 28/28 test
cases). ⚠️ HW feel-test still owed: scroll/typing responsiveness and animation
smoothness on device.

#### PB.9 — Keyboard awake-poll 100 Hz → 40–50 Hz

**Where:** `hal/keyboard_task.cpp:51` (`kPollMs = 10`; the in-code comment already
flags 20–30 ms as the open micro-item, carried from P2.1's "related" note).
Halves/thirds I2C + lock churn; no human-perceptible latency at 20–25 ms. ⚠️ HW:
fast-typing + auto-repeat feel. Tiny, separate commit.

**✅ Landed 2026-07-21:** Changed `kPollMs` from `10` to `20` in
`src/hal/keyboard_task.cpp:51`, dropping poll rate from 100 Hz to ~50 Hz. Halves
I2C + instance-lock churn while awake; 20 ms latency is imperceptible to human
typing. Auto-repeat (`kRepeatDelayMs`, `kRepeatIntervalMs`) is time-based via tick
comparison and is unaffected by the interval change — it is simply evaluated at
20 ms granularity instead of 10 ms. All three builds green (tlora_pager,
emulator_lora_pager, native_test). ⚠️ HW feel-test still owed: fast-typing
responsiveness and auto-repeat rate on device.

#### PB.10 — Hub reachability probe every 10 s; the cache it feeds is 30 s ✅ done 2026-07-21

**Where:** `core/system.cpp:292-296` — `++hub_tick >= 10` on the 1 Hz status-bar
tick; consumers read `hub_last_reachable(max_age_ms = 30000)` (`hal/hub.h:58`).
Probing 3× faster than the freshness the consumers demand buys nothing; each probe
is a task spawn + TCP SYN (radio wake). **Fix:** `>= 10` → `>= 30`. Status-bar hub
icon lags ≤ 30 s instead of ≤ 10 s. Trivial.

**✅ Landed 2026-07-21:** `hub_tick >= 30` in `core/system.cpp`, with a comment
tying the cadence to the consumers' 30 s `max_age`. No HW risk (network cadence
only); builds green (hw + emu + native).

#### PB.11 — Telegram background poll cadence (product decision)

**Where:** `apps/ui_telegram.cpp:1871` — global 60 s lv_timer (created at boot,
never deleted; guards: app closed, internet cached-available, no text input, one
worker). Each firing that passes guards is a TLS round-trip (~1 s WiFi airtime).
Frozen during fake sleep (lv_timers pause), so this is an *awake* cost only.
**Options:** 60 s → 120–300 s (notification latency trade), or leave — flagging for
the product pass alongside P2.11/P2.12. No code smell here; the guards are right.

**Resolved 2026-07-21 — product decision, kept as-is:**

Re-verified 2026-07-21. Timer creation confirmed at `apps/ui_telegram.cpp:1871`
(`s_bg_timer = lv_timer_create(tg_bg_tick, 60000, nullptr)` — no drift from doc).
Guard set in `tg_bg_tick` (lines 1708–1713) is exactly as described: (1) `s_view !=
V_NONE` → skip while app is open; (2) `!internet_available()` → skip when WiFi
unavailable; (3) `core::isTextInputFocused()` → skip while typing anywhere; (4)
`s_bg_task != nullptr` → skip if a worker is already in flight. All four guards
confirmed. The timer is created exactly once at boot (guarded by `if (s_bg_timer)
return;` at line 1867). The cost is awake-only (lv_timers pause during fake sleep).

Decision: **keep 60 s.** The 60 s cadence preserves acceptable notification latency;
the per-firing cost (~1 s TLS round-trip) is incurred only while the user is awake and
connected, and the guard set is correct — there is no code smell to fix. Bumping to
120–300 s is a deliberate notification-latency trade that belongs to a product-owner
decision, folded into the P2.11/P2.12 product-pass bucket. When that decision is made,
the single change is the `60000` literal in `apps/ui_telegram.cpp:1871` (replace with
`120000`–`300000`).

#### PB.12 — BLE keyboard: four small levers

**Where:** `hal/wireless.cpp` (BLE HID path). Verified state: full `deinit(true)` on
toggle-off ✅; keepalive task 1 Hz + 25 s HID heartbeat; iOS conn params
`updateConnParams(…, 0x18, 0x28, 0, 0x2A0)` (`wireless.cpp:44`).
1. **Slave latency 0 → ~10** (`wireless.cpp:44`): at a 30–50 ms interval the
   peripheral answers every connection event; latency 10 cuts radio wakes ~10× while
   idle-connected. Supervision timeout (6.72 s) comfortably covers it; Apple
   guidelines allow it. ⚠️ HW: keystroke latency + iOS reconnect stability.
2. **Advertising runs forever unconnected** (`hw_set_ble_kb_enable` →
   `bleKeyboard.begin()`): add a timeout (~5 min) + re-arm on keypress/toggle.
   Trade: host can't reconnect unprompted after the window — acceptable for HID.
3. **`ble_kb_ka` not fake-sleep gated** (`wireless.cpp:48-78`): 1 Hz wakes during
   sleep; mirror the notify-block pattern, kicked from `ui_resume_timers()`.
4. **TX power never set**: default +3 dBm; for a keyboard used at < 1 m,
   `NimBLEDevice::setPower()` down a step or two is worth a bench-check. (Also:
   the P3.14 TEMPORARY watermark print in this task is still live in the working
   tree — take the reading, land the stack trim, remove the print.)

**✅ Landed 2026-07-21:** three of the four levers landed; one deferred.

- **Lever 1 — slave latency 0 → 10 (landed).** `src/hal/wireless.cpp:45`:
  changed `updateConnParams(…, 0x18, 0x28, 0, 0x2A0)` to
  `updateConnParams(…, 0x18, 0x28, 10, 0x2A0)`. Updated the surrounding comment
  block to state latency 10 and the Apple-compliance check: max effective
  interval = 50 ms × (10+1) = 550 ms ≤ 2 s; supervision timeout 6.72 s ≥
  550 ms × 3 = 1.65 s ✓. Cuts idle-connected radio wakes ~10×.
  ⚠️ HW smoke-test owed: keystroke latency + iOS reconnect stability.

- **Lever 2 — advertising timeout (deferred).** Implementation sketch: track
  elapsed time without a connection in `ble_kb_keepalive_task`; call
  `NimBLEDevice::getAdvertising()->stop()` after 5 min unconnected; re-arm in
  `hw_set_ble_kb_enable()` and on keypress in `hw_set_ble_kb_char()` /
  `hw_set_ble_key()`. Reason for deferral: `BleKeyboard.cpp`'s `onDisconnect`
  callback calls `BLEDevice::startAdvertising()` to restart advertising after a
  disconnect — externally stopping advertising bypasses the library's internal
  state tracking, and correctness of externally calling
  `NimBLEDevice::getAdvertising()->start()` to re-arm cannot be verified without
  an iOS host + meter. Risk of a silent broken-reconnect failure is unacceptable
  without hardware validation. Deferring until a bench session can confirm the
  advertising lifecycle survives the external stop/restart.

- **Lever 3 — gate `ble_kb_ka` task during fake sleep (landed).** Mirrored the
  established notify-block pattern:
  - `src/core/system_hooks.h`: added `void hw_ble_kb_task_notify_wake();`
    declaration (alongside the other `hw_*_task_notify_wake` decls), with a
    clarifying comment.
  - `src/hal/wireless.cpp`: added `#include "core/system_hooks.h"`. Added
    fake-sleep block at the top of the `ble_kb_keepalive_task` loop:
    `if (ui_is_fake_sleep()) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)); was_connected = false; continue; }`.
    Added `hw_ble_kb_task_notify_wake()` implementation at the end of the
    file, guarded by `#if defined(ARDUINO) && defined(USING_BLE_KEYBOARD)`.
  - `src/ui_main.cpp`: added `hw_ble_kb_task_notify_wake()` call in
    `ui_resume_timers()` alongside the existing `hw_lvgl_task_notify_wake()` /
    `hw_keyboard_task_notify_wake()` / `hw_rotary_task_notify_wake()` kicks.
  The P3.14 stack-watermark `Serial.printf("[stackwm] ble_kb_ka …")` in the
  keepalive task was **intentionally left in place** (§6: removal deferred until
  the hardware reading is captured).
  ⚠️ HW smoke-test owed: advertising/reconnect after fake-sleep wake; confirm
  iOS does not drop the link during a long sleep.

- **Lever 4 — TX power +3 dBm → 0 dBm (landed).** `src/hal/wireless.cpp`
  (`hw_set_ble_kb_enable`): added `NimBLEDevice::setPower(0)` immediately after
  `bleKeyboard.begin()`. Signature confirmed from the vendored
  NimBLE-Arduino 2.2.3 header:
  `static bool setPower(int8_t dbm, NimBLETxPowerType type = NimBLETxPowerType::All)`.
  Marked bench-tunable in the comment.
  ⚠️ HW smoke-test owed: confirm link stays stable at 0 dBm across 1–2 m.

All three builds green: `tlora_pager` SUCCESS 29.5 s, `emulator_lora_pager`
SUCCESS 6.0 s, `native_test` 28/28 passed.

#### PB.13 — Charge task: poll → PMU IRQ (fake-sleep's last periodic I2C)

**Where:** `hal/charge_task.cpp:144` — 500 ms `isVbusIn()` I2C poll, only during
fake sleep (awake it blocks on a notify; good design). After PB.4/PB.16 this becomes
the largest remaining sleep-state wake source. The BQ25896 has a VBUS-insert IRQ;
if the pager routes the PMU IRQ line (check `pins_arduino.h` / vendor init), moving
plug-detection to the ISR + notify zeroes the periodic cost. Med effort — only worth
it once the bigger rails are done and the meter confirms it matters.

**Resolved 2026-07-21 — investigated, deferred (hardware blocker):** Re-verified
against current source: `charge_task.cpp:37` sets `kPollMs = 500`; line 131 gates on
`ui_is_fake_sleep()` (awake the task blocks on `ulTaskNotifyTake`); lines 144–150 run
`vTaskDelay(pdMS_TO_TICKS(kPollMs))` then call `poll_vbus_locked()` →
`instance.ppm.isVbusIn()` under `ScopedInstanceLock` — exactly as the doc describes.
PB.4 (PPM ADC off) and PB.16 (notify timeout raised) have now landed, confirming the
charge task is indeed the largest remaining periodic I2C wake source in fake sleep.

**PMU IRQ routing finding — NOT routed on the pager:** `variants/lilygo_tlora_pager/pins_arduino.h`
defines `RTC_INT (1)`, `NFC_INT (5)`, `SENSOR_INT (8)`, and `LORA_IRQ (14)` but
**no PMU/BQ25896/charger interrupt pin**. Confirmed in vendor init
`lib/LilyGoLib/src/LilyGo_LoRa_Pager.cpp`: `initPMU()` (line 514) calls
`ppm.init(Wire, SDA, SCL)` with no `attachInterrupt`; the only `attachInterrupt` calls
in this file are for `SENSOR_INT` (BHI260) and `RTC_INT` (PCF85063). The vendor does
define `HW_IRQ_POWER` (`LilyGoTypedef.h:63`) and the pager's `loop()` (line 1200)
contains a stub for it — but it is fully commented out. By contrast, the Watch-S3 and
Watch-Ultra *do* define `PMU_INT`, call `attachInterrupt(PMU_INT, ...)`, and handle
`HW_IRQ_POWER` live; the pager board simply does not break out the BQ25896 `/INT` pin
to any ESP32 GPIO. **PB.13 is therefore not implementable without a hardware wiring
change** — a stronger deferral than the doc originally anticipated. Closed as
investigate-only; no code was changed.

#### PB.14 — Gauge sweep trims (= §1.5, with one correction to the record)

**Where:** `hal/power.cpp:300-332` inside `hw_get_monitor_params`. Re-verified all
five callers: they read **only** `.battery_percent` / `.is_charging` (plus
`.battery_voltage` internally). **Correction to the phase-1 record: `.temperature`
(L309) is also write-only** — `settings_info` uses `hw_get_battery_voltage()`
directly, nothing reads `.temperature`. That makes **9** removable
`instance.gauge.*` register reads per sweep (current, remaining/full/design
capacity, standby current, average power, max-load current, temperature,
timeToEmpty/Full). Keep: `refresh()`, `getStateOfCharge()`, `getVoltage()`,
`getBatteryStatus()` (gates `is_charging`), and the ppm `getFaultStatus()`
clear-side-effect noted in `OPTIMIZATION_PROGRESS.md`. Rides the hardware pass as
before; battery-relevant because it is live I2C every sweep (5 s idle / 1 s
charging).

**✅ Landed 2026-07-21:** Grep-verification (re-run against full `src/` tree)
confirmed **zero readers** for all 10 checked fields — `instantaneousCurrent`,
`remainingCapacity`, `fullChargeCapacity`, `standbyCurrent`, `designCapacity`,
`averagePower`, `maxLoadCurrent`, `timeToEmpty`, `timeToFull`, and `.temperature`
(the correction the doc calls out; the PMU branch assigns it but no caller reads
it from `monitor_params_t` on hardware). The 9 gauge register reads were removed
from the `if (hw_get_device_online() & HW_GAUGE_ONLINE)` block in
`src/hal/power.cpp` (lines 304–334 before the edit): `getCurrent()`,
`getRemainingCapacity()`, `getFullChargeCapacity()`, `getStandbyCurrent()`,
`getTemperature()`, `getDesignCapacity()`, `getAveragePower()`,
`getMaxLoadCurrent()`, and the entire
`if (batteryStatus.isInDischargeMode()) { … timeToEmpty/timeToFull … }` block.
Kept intact: `refresh()`, `getStateOfCharge()`, `getVoltage()`,
`getBatteryStatus()` + the `isFullChargeDetected()` veto on `is_charging`.
The `monitor_params_t` struct (`hal/types.h`) was not modified — all fields
remain; the PMU branch still assigns `.temperature`. TTL comment updated from
"~12 I2C gauge/PMU register reads" to "~4 gauge reads (9 removed by PB.14) plus
~5 PPM reads". All three builds green: `tlora_pager` SUCCESS 22 s, `emulator_lora_pager`
SUCCESS 1.7 s, `native_test` 28/28 passed. ⚠️ HW pass still owed: info page and
status bar must still render battery % and charge state correctly on device.

#### PB.15 — `hw_get_battery_voltage()` bypasses the TTL cache ✅ done 2026-07-21

**Where:** `hal/power.cpp:79-83` — unconditional `gauge.refresh()` +
`getVoltage()`; called at 1 Hz by `apps/settings_info.cpp:57` while the info page is
open. `gauge.refresh()` is the *expensive* multi-register BQ28Z610 access the §2.12
TTL was added to throttle — this side door does it every second. **Fix:** route
through `hw_get_monitor_params()` and read `.battery_voltage` (≤ 5 s staleness on a
static info label is fine). Trivial.

**✅ Landed 2026-07-21:** the **caller** was fixed, not `hw_get_battery_voltage()`
itself — the function is also called from inside `hw_get_monitor_params()`
(`power.cpp:337`, gauge-offline fallback), so routing the function through the
monitor params would recurse. `settings_info.cpp:57` (the 1 Hz timer) now reads
`.battery_voltage` from the TTL-cached sweep and shares the status bar's cache. The
one-shot page-build read at `settings_info.cpp:185` was intentionally left on the
direct call (single read, freshest value at page open). Builds green.

### C. Micro / hygiene / adjacent correctness

#### PB.16 — Fake-sleep notify-blocks: 200 ms timeouts = 20 fallback wakes/s across 4 tasks

**Where:** `lvgl_task.cpp:56`, `keyboard_task.cpp:107`, `rotary_task.cpp:79`,
`nfc_task.cpp` — all `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200))`. The explicit
wake kicks (`hw_*_task_notify_wake()` from `ui_resume_timers()`) are the real wake
path; the 200 ms timeout is only a safety net. Raising to 1000 ms (or
`portMAX_DELAY` where the kick coverage is proven) cuts sleep-state scheduler wakes
4/5ths. Low risk — the kicks already exist and are on the smoke-test checklist; keep
the timeout non-infinite unless every wake path is audited.

**✅ Landed 2026-07-21:** raised all four `kFakeSleepIdleMs` / `kIdleMs`
constants 200 → 1000 ms in `hal/lvgl_task.cpp:40`, `hal/keyboard_task.cpp:55`,
`hal/rotary_task.cpp:50`, and `hal/nfc_task.cpp:34`. Timeouts kept non-infinite.
Wake-kick audit: `ui_resume_timers()` kicks `hw_lvgl_task_notify_wake()`,
`hw_keyboard_task_notify_wake()`, and `hw_rotary_task_notify_wake()` — those
three tasks have immediate wake coverage and see zero responsiveness impact.
The NFC task has NO kick from `ui_resume_timers()`; `hw_nfc_task_notify_wake()`
is called only from `hw_start_nfc_discovery()` (when the user enables NFC in
settings). Consequence: raising `kIdleMs` to 1000 ms adds up to ~800 ms latency
to NFC scan resumption after a fake-sleep wake when discovery was already active
before sleep — acceptable, as NFC re-enable is not latency-critical. Builds green
(tlora\_pager + emulator\_lora\_pager + native\_test, 28/28 tests passed). This is
a sleep-state scheduler-churn reduction; no HW smoke-test strictly required, but
it rides the same fake-sleep checklist (wake → keyboard, wake → LVGL, wake → rotary).

#### PB.17 — CPU frequency set twice per fake-sleep transition

**Where:** `hw_power_down_all()` (`system.cpp:677`) and `hw_power_up_all()`
(`system.cpp:685`) each call `setCpuFrequencyMhz(…)`, then `factory.ino::loop()`'s
`last_freq` state machine (which doesn't know about those calls) repeats the set on
its next tick. Distinct from the boot-time P3.16 (fixed). **Fix:** let `loop()` own
frequency exclusively (accept ≤ 500 ms lag on entry), or update `last_freq` via a
setter. Cosmetic (~0.5 ms PLL re-lock each), but it keeps the frequency policy in
one place — worth doing when touching PB.20.

**✅ Landed 2026-07-21:** Implemented option 2 (tracked setter) over option 1 (loop-ownership) specifically to preserve snappy wake: option 1 would leave a ≤ 500 ms window after wake where the CPU stays at 40/80 MHz while `loop()` sits in `delay(500)`. Added `hw_set_cpu_freq(uint32_t mhz)` and `hw_get_cpu_freq()` to `src/hal/system.h` and implemented them in `src/hal/system.cpp` with a file-static `s_cpu_freq_mhz = 0` tracker (0 = unknown, forces a real set on first call). The implementation guards `setCpuFrequencyMhz()` behind `#ifdef ARDUINO` so the emulator compiles cleanly while still tracking the value. Replaced the two raw `setCpuFrequencyMhz()` calls in `hw_power_down_all()` and `hw_power_up_all()` (`src/hal/system.cpp`) with `hw_set_cpu_freq()`. In `src/factory.ino`: converted both `setCpuFrequencyMhz()` calls in `setup()` to `hw_set_cpu_freq()`; deleted the `static uint32_t last_freq = 0` variable from `loop()` and replaced all three `if (last_freq != x) { setCpuFrequencyMhz(x); last_freq = x; }` guards with bare `hw_set_cpu_freq(x)` calls (the tracker self-dedupes). Exactly one call to `setCpuFrequencyMhz()` remains in the entire `src/` tree, inside `hw_set_cpu_freq()`. No frequency values or `hold_80` policy changed (that is PB.20). Cosmetic/no-HW-risk: freq policy consolidation only. All three builds green (`tlora_pager`, `emulator_lora_pager`, `native_test`).

#### PB.18 — `hw_light_sleep()` is dead code with a latent security gap ✅ done 2026-07-21

**Where:** `hal/system.cpp:654-660` (+ decl `hal/system.h:60`). Zero callers
(`hw_low_power_loop()` calls `instance.lightSleep()` directly). Unlike `hw_sleep()`
/ `hw_low_power_loop()`, it does **not** `notes_crypto_lock()` or deinit audio
before suspending — any future caller would sleep with the notes passphrase in RAM.
**Fix:** delete it (phase-1 grep discipline: check `lib/LilyGoLib/src/` for extern
callers first), or harden the body to match `hw_sleep()`. Trivial either way.

**✅ Landed 2026-07-21:** deleted — grep confirmed zero callers in `src/` **and**
`lib/LilyGoLib/src/` (only the definition + decl existed). Removed both the body
(`hal/system.cpp`) and the declaration (`hal/system.h:60`). Chose delete over harden
so no half-safe sleep path can be resurrected by accident. Builds green.

#### PB.19 — Speaker amp: settings toggle latches the rail; playback path is fine (correcting the sweep)

**Verified mechanics** (this corrects an initial sweep finding before it entered the
record): on the pager (`USING_AUDIO_CODEC`), the vendor wires
`codec.setPaPinCallback` → `EXPANDS_AMP_EN` (`LilyGo_LoRa_Pager.cpp:472-474`), so
`codec.open()/close()` gates the amp per playback session — there is **no**
"amp on 24/7 during playback idle" and **no** "silent audio after fake sleep" bug.
What *is* true: `hw_set_speaker_enable(true)` (`hal/audio.cpp:614` →
`powerControl(POWER_SPEAK, en)`) latches the rail on until the next fake sleep cuts
it, independent of playback (2–8 mA amp quiescent). Default `speaker_enable = 0`, so
out-of-box cost is zero. **Fix (small, optional):** make the toggle a pure
preference gate (persist the flag; don't drive the rail), letting the codec callback
own the pin. Verify what the toggle is *meant* to do (keyboard clicks?) before
changing semantics — product check.

**Resolved 2026-07-21 — investigated, deferred (product semantics):** Full consumer
trace of `user_setting.speaker_enable` / `hw_get_speaker_enable()` across the entire
`src/` tree found **zero functional consumers beyond the rail drive**. Precise
mechanics: `speaker_enable_cb` (`settings_connectivity.cpp:119-123`) → `local_param
.speaker_enable = en` + `hw_set_speaker_enable(en)` → `user_setting.speaker_enable =
en` + `powerControl(POWER_SPEAK, en)` (`audio.cpp:611-614`). That is the toggle's
complete effect — it latches the `POWER_SPEAK` rail. The player self-manages the same
rail: `powerControl(POWER_SPEAK, true)` at `audio.cpp:123` on playback start,
`powerControl(POWER_SPEAK, false)` at `audio.cpp:159` on playback end; fake sleep cuts
it at `system.cpp:686`. There is no keyboard-click, UI-beep, or notification-tone path
in `src/` that reads the flag or requires a pre-latched rail. `hw_get_speaker_enable()`
is called only from `settings_connectivity.cpp:347` to seed the toggle widget's initial
state. Default `speaker_enable = 0` (`system.cpp:390,442`) → zero out-of-box cost; opt-
in cost is 2–8 mA quiescent draw from enabling the toggle until the next playback-end
or fake sleep un-latches it. Three options for the product owner: **(a)** wire the flag
as a real master-audio gate (check it in the player before calling `open()` / before
enabling `POWER_SPEAK`) — makes the toggle meaningful as a true mute; **(b)** make it
preference-only (persist the flag, remove the `powerControl` call from `hw_set_speaker_
enable`) — the toggle becomes inert until some other path reads it; **(c)** remove the
toggle entirely if it serves no intended purpose. **Recommendation: do not change
behavior until product intent is confirmed.** Out-of-box cost is already zero; the
existing 2–8 mA exposure is user-opt-in only. Doc-only; no build needed.

#### PB.20 — The 80 MHz fake-sleep CPU floor (compound with PB.2)

**Where:** `system.cpp:675-677` + the same logic in `factory.ino` —
`hold_80 = ble_connected || wifi_connected`. Correct today (the RF stacks want
≥ 80 MHz). After PB.2 (MAX_MODEM) the question becomes whether 40 MHz + modem-sleep
is stable — an ESP-IDF-version-dependent behaviour that must be answered by soak
test, not docs. ⚠️ HW-only item; park until PB.2 has bench numbers.

**Resolved 2026-07-21 — unblocked, HW-gated:** Both prerequisites are now in tree. PB.2
(`hw_wifi_powersave_sleep()` / `WIFI_PS_MAX_MODEM`) landed: `src/hal/wireless.cpp:177`
defines the function and `src/hal/system.cpp:711` calls it from `hw_power_down_all()`.
PB.17 (freq consolidation) landed: `hw_set_cpu_freq()` is defined at
`src/hal/system.cpp:26` and is now the single owner of all `setCpuFrequencyMhz()` calls;
`factory.ino::loop()` drives the policy at line 205–206:
`bool hold_80 = hw_get_ble_kb_connected() || hw_get_wifi_connected(); hw_set_cpu_freq(hold_80 ? 80 : 40);`
(the old `static last_freq` dedup guard was deleted by PB.17; `hw_set_cpu_freq()` self-dedupes).
The experiment is now set up: the remaining work is a **hardware soak test** — lower the
fake-sleep floor from 80 → 40 MHz while WiFi is connected under `WIFI_PS_MAX_MODEM`
(and separately with BLE connected) and confirm the RF link survives over a multi-hour
soak on the exact ESP-IDF/Arduino-core version this repo pins. The code change is a
single `hold_80` policy edit in `factory.ino::loop()` (the mirror in `system.cpp:695`
inside `hw_power_down_all()` is a separate opt-in path; adjust both if the soak passes).
A wrong call silently drops the RF link during sleep — defer the code change until soak
results are in hand.

#### PB.21 — No ESP-IDF power management at all (investigate-only)

No `esp_pm_configure`, no tickless idle, no `CONFIG_PM_ENABLE` anywhere; the only
`esp_light_sleep_start()` path is the PMU-button one. Two investigation tracks, both
potentially bigger than everything above combined, both risky under Arduino:
1. **Auto light sleep** (`esp_pm_configure` + tickless): requires the Arduino
   framework's sdkconfig to have PM enabled (check before designing anything — the
   prebuilt core likely doesn't; a custom sdkconfig/IDF-component build is a big
   hammer).
2. **Periodic true light sleep during fake sleep:** from `factory.ino`'s fake-sleep
   branch, call the vendor `lightSleep()` with a timer + GPIO wake mask instead of
   `delay(500)` — the vendor already restores everything on wake. Wake sources are
   the crux (rotary center press, PMU IRQ, charger); the vendor path currently
   supports the boot button only, so this needs wake-source plumbing in vendored
   code. Sacrificial-device territory, like P3.25.

**Resolved 2026-07-21 — investigated, deferred:** Full investigation performed; no
code changed.

**Track 1 (auto light sleep / tickless idle):** Grepped both the active sdkconfig
(`~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/sdkconfig`, IDF
v5.3.0 libs paired with Arduino-ESP32 v3.1.3) and the framework's own sdkconfig
(`framework-arduinoespressif32/tools/sdk/esp32s3/sdkconfig`). Both have `CONFIG_PM_ENABLE`
**explicitly not set** (`# CONFIG_PM_ENABLE is not set`). No `CONFIG_FREERTOS_USE_TICKLESS_IDLE`,
no `CONFIG_PM_DFS_INIT_AUTO`, and no `CONFIG_PM_SLP_IROM_IN_RAM` appear anywhere in
either file. The only PM-adjacent flags present are
`CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=y` and `CONFIG_PM_POWER_DOWN_TAGMEM_IN_LIGHT_SLEEP=y`
— those govern what happens during a manually-triggered `esp_light_sleep_start()` call
and are irrelevant to the auto-PM / tickless path. Conclusion: calling
`esp_pm_configure()` with the prebuilt Arduino-ESP32 v3.1.3 / IDF v5.3.0 core will
silently do nothing at best, hard-fault at worst. Enabling tickless idle requires
rebuilding the entire IDF lib set with `CONFIG_PM_ENABLE=y` and
`CONFIG_FREERTOS_USE_TICKLESS_IDLE=1` — a full custom-core build, not a header flag.
That is a separate, large project. Confirmed via `grep -rn "esp_pm_configure\|esp_light_sleep_start\|tickless\|CONFIG_PM_ENABLE" src/` — zero hits in `src/`; the only `esp_light_sleep_start()` path in the firmware is the vendor `lightSleep()` reached from `hw_low_power_loop()` via the PMU-button (WAKEUP_SRC_BOOT_BUTTON only).

**Track 2 (periodic true light sleep during fake sleep):** Audited
`lib/LilyGoLib/src/LilyGo_LoRa_Pager.cpp:663–735`. `checkWakeupPins()` accepts only
`WAKEUP_SRC_ROTARY_BUTTON` (rotary center press, `ROTARY_C` GPIO) and
`WAKEUP_SRC_BOOT_BUTTON` (GPIO 0); any other bit is silently dropped. `lightSleep()`
calls `esp_sleep_enable_ext1_wakeup_io()` for the GPIO mask then
`esp_light_sleep_start()` — no `esp_sleep_enable_timer_wakeup()` call, so **timer
wakeup is absent from the pager's vendor `lightSleep()`**. To use periodic light sleep
from the fake-sleep `delay(500)` in `factory.ino.cpp:255`, timer wakeup would need to
be added to vendored code. The `lightSleep()` teardown sequence also performs a full
peripheral shutdown (disables keyboard, GPS, SD, haptic, NFC, radio, flushes serial,
delays 1 s) — appropriate for intentional sleep but far too heavy for a ~500 ms
wake-check nap. Additionally, PB.13 confirmed the PMU IRQ line is not routed to an
ESP32-S3 GPIO on the pager, so charger-detect wake during periodic light sleep is
impossible without a board hardware change. The useful wake sources for a periodic
fake-sleep nap would be timer + rotary centre press; both require vendored code
changes, and the 1 s pre-sleep `delay()` in `lightSleep()` would negate any power
benefit for a 500 ms nap interval. This is sacrificial-device territory: needs a
dedicated hardware bring-up effort with custom periodic-nap logic, not a code edit to
the existing path.

---

## 5. Verified clean this pass (don't re-sweep)

- Panel really sleeps (SLPIN) and backlight hits 0 in fake sleep; keyboard backlight
  rail cut. Haptic rail cut in sleep (DRV2605 idle cost only while awake, rail-gated
  by the setting).
- All lv_timers freeze during fake sleep (LVGL task blocks before
  `lv_timer_handler`); no app timer needs its own fake-sleep guard.
- Weather has **no** background poll (fetch on app open / tap; 15 min cache).
  Home-screen internet check is user-triggered. Menu badge/visibility timers are
  RAM-reads only and die with the menu.
- BLE fully deinits on toggle-off; audio player task blocks on its queue when idle
  (zero cost); recorder is on-demand; amp is codec-callback-gated (see PB.19).
- Charge task is notify-blocked while awake (only costs during sleep).
- GPS rail: force-off at boot, transient-only for GPS time sync (P2.5).
- Gauge sweep TTL (1 s charging / 5 s discharging) works as designed
  (`power.cpp:257-266`) — PB.15 is a bypass of it, not a flaw in it.

## 6. Worktree caveats at analysis time

- `src/hal/wireless.cpp` + `src/apps/ui_ssh.cpp` carry the **TEMPORARY P3.14/P3.9
  stack-watermark prints** — **now committed in `51c71a6`** (were uncommitted at
  analysis time), still mid-measurement from phase-3's 2026-07-16 session. The
  `ble_kb_ka` print itself wakes the serial path every 15 s; don't count it in any
  bench baseline, and finish those readings first. Removal is deferred until the
  P3.9/P3.14 stack readings are captured — the process is take-reading → land the
  stack trim → remove the print, so pulling the prints early forfeits both.
- `src/apps/ui_settings.cpp` carries the unrelated settings-shortcut feature diff
  (**also committed in `51c71a6`**) with a **known intermittent back-navigation
  crash** (flagged in P3.19's entry). Bench sessions that navigate settings should
  expect it.

## 7. Suggested execution order

**Session progress — 2026-07-21 (all items worked one-by-one, each build-verified
`tlora_pager` + `emulator_lora_pager` + `native_test` green):**
- **Landed (code in tree):** PB.1, PB.2, PB.3, PB.4, PB.6, PB.8, PB.9, PB.14, PB.16,
  PB.17 (fully); PB.7 (poll bump only); PB.12 (slave-latency + ka fake-sleep gating +
  TX-power; advertising-timeout deferred). Plus the earlier PB.10/PB.15/PB.18. **⚠️ The
  whole `PB.x` batch still owes the hardware smoke-test / bench baseline** — no line of
  it has been current-measured (see the §Bench appendix and the discipline banner up top).
- **Deferred with a documented disposition (no blind code):** PB.5 (SD remount —
  unverifiable without a meter; corruption is the failure mode), PB.7-WU (⚠️ HW tag-tech
  coverage), PB.11 (kept 60 s — product call), PB.12-advertising (library owns the
  lifecycle), PB.13 (**pager doesn't route the BQ25896 /INT pin — not implementable
  without a wiring change**), PB.19 (speaker flag has zero consumers — product-semantics
  call), PB.20 (40 MHz floor — prereqs landed, needs HW soak), PB.21 (`CONFIG_PM_ENABLE`
  off in the pinned Arduino core — auto light-sleep needs a custom IDF rebuild).
- Note: PB.17 (freq ownership) was landed this session rather than parked — it consolidated
  cleanly (`hw_set_cpu_freq()` single owner) and PB.20's prereq now sits in tree.

The original plan (below) is retained for provenance.

1. **Trivial, zero-HW-risk first:** ✅ PB.10 (hub probe 10→30 s), ✅ PB.15 (voltage
   cache path), ✅ PB.18 (delete dead `hw_light_sleep`) — **all landed 2026-07-21**.
   PB.17 (freq ownership — optional) ✅ **also landed 2026-07-21** (see session summary
   above); it consolidated frequency ownership behind `hw_set_cpu_freq()`.
2. **The flagship:** PB.1 auto fake-sleep on `disp_timeout_second` (+ optional dim
   stage). Everything else multiplies through it.
3. **Fake-sleep gap batch (one bench session):** PB.4 (PPM ADC), PB.3 (radio sleep),
   PB.6 (IMU off in sleep + unregister fix), PB.2 (WiFi MAX_MODEM), PB.16 (notify
   timeouts) — each is small; measure the sleep-state current before/after each on
   the meter, in that order, and record deltas here. PB.5/P3.12 (SD rail) joins this
   session if its bench number justifies the remount work.
4. **Awake churn:** PB.8 (LVGL cap), PB.9 (keyboard poll) — feel-test on device.
5. **Product decisions:** PB.11 (telegram cadence), PB.7 (NFC WU mode), PB.12
   (BLE levers), PB.19 (speaker toggle semantics) — fold into the existing
   P2.11/P2.12 product-pass bucket.
6. **Deferred/investigate:** PB.13 (PMU IRQ), PB.20 (40 MHz floor), PB.21 (true
   light sleep) — only with bench data and a sacrificial device.

Per repo convention: one `<code>` + one `<docs>` commit per item; update this file's
items with ✅/measured-delta notes as they land; `OPTIMIZATION_PROGRESS.md`'s
methodology stays in force — **re-verify every file:line here against current source
before editing; this file will go stale the same way its three predecessors did.**

---

## Bench appendix — the baseline table to fill on the next hardware session

Measure at the battery (or USB with charging complete/disabled), 30 s settle per row:

| State | Setup | mA (measured) |
|---|---|---|
| Active, menu, full backlight, WiFi on | default boot | |
| Active, menu, WiFi off, BLE off | toggles off | |
| Idle-awake > 2 s (80 MHz step) | hands off | |
| Fake sleep, WiFi connected | long-press | |
| Fake sleep, WiFi off, BLE off | | |
| Fake sleep after PB.4 / PB.3 / PB.6 / PB.2 (one row each, cumulative) | | |
| Vendor light sleep (PMU button) | reference floor | |
| NFC enabled, idle (field on) | for PB.7 | |

The "vendor light sleep" row is the target floor for the fake-sleep column: every mA
of gap is exactly the PB.2–PB.6 list.

### P5 rows — isolating the fake-sleep residual-drain batch

Added from `OPTIMIZATION_PHASE5.md` §4. These four rows sit on top of the table
above and exist to split the P5 batch into its wake-rate half and its
operating-point half, because only the latter can plausibly regress:

| Row | Setup | mA (measured) |
|---|---|---|
| Fake sleep, radios off, **at `8405a2a`** | baseline for this batch (pre-P5) | |
| Fake sleep, radios off, P5.1+P5.2+P5.3 only | build with `FAKE_SLEEP_IDLE_FREQ_MHZ = 40` in `src/hal/system.cpp` — isolates the wake-rate work (12.2 → 1.07 wake-ups/s) | |
| Fake sleep, radios off, full P5 | stock tree (20 MHz floor) — isolates P5.4 | |
| Fake sleep, WiFi associated, full P5 | confirms `hw_fake_sleep_target_freq()`'s 80 MHz hold still engages | |

If row 3 is not measurably below row 2, P5.4 is buying nothing: set
`FAKE_SLEEP_IDLE_FREQ_MHZ` back to `40` and keep the rest of the batch.

**Functional smoke test for the same session (no meter needed), in this order:**

1. Long-press wheel → sleeps. Long-press again → wakes. Repeat 5x.
2. Short-press while asleep → **nothing happens** (no wake, and no click landing
   in the UI on the next wake).
3. Sleep, wait >10 min, plug cable → charging overlay appears within ~15 s
   (P5.3's dormant stage), then re-sleeps.
4. Sleep with WiFi associated → confirm the link survives (P5.4's 80 MHz hold).
5. Auto-sleep via `disp_timeout_second` → confirm it still fires (P5.1 changed
   the notify path `lilygo_request_fake_sleep_toggle()` takes).
6. Wake and scroll immediately → confirm no lost or phantom detents (P5.1 skips
   `rotary.process()` while asleep). Worst case by code reading is one swallowed
   detent, never a phantom scroll; verify on hardware anyway.
