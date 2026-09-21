# Optimization Phase 5 — Fake-sleep residual drain

Generated **2026-09-20** against a clean tree at **`8405a2a`** ("docs: add
OPTIMIZATION_PHASE4.md"). Items are numbered **`P5.x`**, continuing the
`P2.x`/`P3.x`/`P4.x` series.

**Scope.** Phase 4 and the `PB.x` batch took the fake-sleep *state* apart — rails,
radio, ADC, IMU, WiFi power-save, task parking. What they did not finish is the
*residual periodic work* that keeps running once the device is in that state and
nothing is happening. This pass is exactly that: what still wakes a core, and how
often, during a fake sleep that nobody is interacting with.

> **⚠️ Measurement discipline (inherited, unchanged).** Nothing in this codebase has
> ever been current-measured, and this batch is no exception. Every claim below that
> is stated as fact is a **wake-rate / work-eliminated** claim, verifiable by reading
> the code. No mA figure is claimed for any item. `OPTIMIZATION_BATTERY.md`'s
> **Bench appendix** is still the highest-value unblocked action in this project.

> **🔴 BATCH LANDED UNMEASURED — 2026-09-20.** P5.1–P5.4 are implemented in tree.
> `tlora_pager`, `emulator_lora_pager` and `native_test` (28/28) all pass. No current
> measurements were taken before or after.
>
> **✅ CLOSED OUT — 2026-09-21.** Every item re-verified against the working tree
> rather than trusted from this doc: P5.1 in `LilyGo_LoRa_Pager.cpp` (ISR armed in
> `rotaryTask`, `ulTaskNotifyTake` guard, `lilygo_request_fake_sleep_toggle()` kick),
> P5.2 `delay(... ? 2000 : 50)` in `factory.ino`, P5.3 `kDormantPhaseMs`/
> `kPollDormantMs` in `charge_task.cpp`, P5.4 `FAKE_SLEEP_IDLE_FREQ_MHZ = 20` behind
> the single-owner `hw_fake_sleep_target_freq()`. All three gates re-run green on
> this date (`tlora_pager` RAM 27.5 % / Flash 68.1 %, `emulator_lora_pager`,
> `native_test` 28/28). §4's rows and smoke test are now transcribed into
> `OPTIMIZATION_BATTERY.md`'s Bench appendix. **The only work this batch still has
> open is the bench session itself, which needs hardware and a meter.**

---

## 1. The state being optimised

Steady-state fake sleep, radios down, nobody touching the device. Before this pass,
these are the things still periodically waking a core:

| Task | Prio | Cadence | Work done per wake |
|---|---|---|---|
| vendor `rotaryTask` | 10 | **10 Hz** | `digitalRead(ROTARY_C)` + `rotary.process()` (result discarded) |
| Arduino `loopTask` | 1 | **2 Hz** | instance mutex + `instance.loop()` + freq check |
| `charge_ind` | 3 | **0.2 Hz** | instance mutex + I2C VBUS read |
| `lvgl` / `keyboard` / `nfc` / `ble_kb_ka` | — | parked | blocked on notify (P4/PB — correct already) |

≈ **12.2 task wake-ups/s**, at a 40 MHz core clock, to observe a device where by
construction nothing can happen except a cable plug or a wheel press.

After P5.1–P5.4: ≈ **1.07 wake-ups/s** at a 20 MHz core clock — about an **11x**
reduction in wake rate at half the clock.

---

## 2. Items

### P5.1 ✅ — `rotaryTask` polls ROTARY_C at 10 Hz for the entire sleep

**File:** `lib/LilyGoLib/src/LilyGo_LoRa_Pager.cpp` (vendor file, local patch —
joins the existing `[LOCAL PATCH P4.11]` markers).

The highest-priority task in the system (prio 10, above LVGL's 8) spent the whole
of fake sleep in `delay(100)`, waking ten times a second to read one GPIO and to run
the quadrature state machine on two more — whose result was then *unconditionally
discarded* three lines later, because scroll is ignored while the display is off.
The only event it could act on is the >1000 ms centre-button hold.

ROTARY_C is GPIO 7, is RTC-capable, and has no other owner: the encoder polls
ROTARY_A/ROTARY_B and attaches no ISR anywhere in the vendor tree (verified against
every `attachInterrupt` call site in `lib/LilyGoLib/src/`).

**Change:** hang a `CHANGE` interrupt on ROTARY_C that notifies the task, and have
the task block on `ulTaskNotifyTake` whenever the display is off *and* no press is
in flight. While a press **is** in flight it keeps the 2 ms cadence, so the 1 s hold
threshold is timed at the same resolution as when awake. Also skip
`instance.rotary.process()` entirely while the display is off.

**This is a latency improvement, not a trade.** The press is now timestamped at the
interrupt edge rather than up to 100 ms late, so hold-to-wake responds *better* than
before. The 2000 ms timeout on the take is a stranding guard, not a hold detector —
deliberately so: if the ISR ever failed to fire, a 1 s hold could fall entirely
inside *any* polling window and be missed regardless, so the timeout is sized to cut
idle wake-ups (20x) rather than to back up the edge.

`lilygo_request_fake_sleep_toggle()` now also kicks the task, so an explicit toggle
raised from another task (the `l` keyboard shortcut, factory.ino's auto-sleep) is
serviced immediately instead of waiting out the guard.

**Verified transitions** (traced against the state machine, all four correct):
hold-to-sleep, hold-to-wake, short-press-while-asleep (correctly ignored, no
spurious click), and release-after-toggle (no spurious click on either edge).

**Cost:** button contact bounce now generates a short burst of interrupts per edge.
Harmless — each is a notify-give to an already-running task, and the counter
saturates.

### P5.2 ✅ — `loopTask` runs at 2 Hz during sleep to service nothing

**File:** `src/factory.ino`

The fake-sleep branch backed off to 2 Hz (PB pass). Everything it still drives in
that state is either self-throttled on a much longer clock or idempotent:

- NTP re-trigger — backoff floor is 30 s, capped at 15 min / 10 attempts (P4.19).
- `hw_wifi_supervise()` — self-throttles at 30 s minimum (P4.17).
- `hw_set_cpu_freq()` — no-ops when the frequency is unchanged (PB.17).
- `instance.loop()` — dispatches only `HW_IRQ_RTC` and `HW_IRQ_SENSOR`. The IMU is
  unregistered on sleep entry (P4.22/PB.6) so no sensor interrupt can arrive, and
  **`RTC_EVENT_INTERRUPT` has no registered consumer anywhere in `src/`** (grepped).

So the loop was taking the instance mutex twice a second to dispatch events that
cannot arrive to handlers that do not exist. **2 Hz → 0.5 Hz.** The added latency is
latency on work that does not exist.

### P5.3 ✅ — `charge_ind` polls VBUS at 0.2 Hz forever

**File:** `src/hal/charge_task.cpp`

P4.8 added a fast→medium→slow back-off that terminates at 5 s and stays there for
the rest of the sleep, making this the last periodic I2C consumer on the bus.

**Change:** a fourth stage — past **10 min** of uninterrupted fake sleep, drop to
15 s. Someone plugging a cable into a device that has been asleep for ten minutes is
not watching for the overlay in the first few seconds. Any VBUS edge resets
`last_reset_ms` and drops straight back to the 500 ms fast phase, so the *second*
plug event in a session is as responsive as ever.

**Interaction checked:** the 60 s battery-cap tick (P4.5) still fires, now with 15 s
granularity — fine for an 80 % cap. The 5 min notes-lock grace period (P4.29) is
unaffected because it expires at 5 min, before this stage is ever reached.

### P5.4 ⚠️ — 40 MHz fake-sleep floor → 20 MHz

**Files:** `src/hal/system.{h,cpp}`, `src/factory.ino`

On the ESP32-S3 any frequency below 80 MHz already runs the core off the 40 MHz XTAL
with the PLL down. 20 MHz is simply XTAL/2 and halves core dynamic power again on
top of that. Selected only when **neither BLE nor WiFi is associated**, so the ≥80 MHz
requirement of both stacks is never violated — and by that point every other consumer
is parked (LVGL, keyboard, rotary blocked; radio asleep; display off).

**Consequence, bounded:** APB tracks the CPU when the PLL is off, so peripheral
clocks divide with it — a bus configured for 400 kHz I2C runs near 100 kHz here.
The charge task's VBUS read is the only periodic bus traffic left in that state, and
`hw_power_up_all()` restores the user frequency before anything throughput-sensitive
runs again. Note the firmware **already** shipped a 40 MHz floor, which halves those
same clocks; this doubles an existing, working trade rather than introducing a new one.

Also fixes a latent duplication: the `hold_80 ? 80 : 40` rule was copy-pasted in
`hw_power_down_all()` and in factory.ino's `loop()` and could drift. Both now call
the single owner, `hw_fake_sleep_target_freq()`.

> **⚠️ This is the one item in the batch that most wants a meter.** It is the only
> P5 item that changes a hardware operating point rather than removing provably-dead
> work. If the measured delta from 40 MHz is noise, or anything misbehaves at XTAL/2,
> revert by setting `FAKE_SLEEP_IDLE_FREQ_MHZ` in `src/hal/system.cpp` back to `40`
> — nothing else needs to change.

---

## 3. What was considered and *not* done

- **PB.21 / tickless idle / `esp_pm_configure`** — still blocked. `CONFIG_PM_ENABLE`
  is off in the pinned Arduino-ESP32 v3.1.3 / IDF v5.3.0 core; enabling it needs a
  custom IDF rebuild. Re-confirmed, disposition unchanged. Worth noting that P5.1 and
  P5.2 remove two of the three obstacles that made tickless idle pointless even if it
  *were* available — the remaining periodic waker is the charge task.
- **10 MHz instead of 20** — marginal saving over 20 MHz is small while the
  peripheral-clock divide doubles again. Not worth the risk without bench data.
- **PMU IRQ instead of polling VBUS (PB.13)** — still not implementable: the pager
  does not route the BQ25896 `/INT` pin. Unchanged from `OPTIMIZATION_BATTERY.md`.
- **SD rail down during sleep (PB.5)** — unchanged disposition: unverifiable without
  a meter, and filesystem corruption is the failure mode. Still the largest single
  *unclaimed* fake-sleep item.

---

## 4. Bench checklist for this batch ✅ transcribed

**Landed in `OPTIMIZATION_BATTERY.md` → §Bench appendix → "P5 rows" (2026-09-21).**
That file is the one to fill in on the next hardware session; the copy below is
kept here as the rationale for the rows. Rows that isolate P5:

| Row | Setup |
|---|---|
| Fake sleep, radios off, **at `8405a2a`** | baseline for this batch |
| Fake sleep, radios off, P5.1+P5.2+P5.3 only (`FAKE_SLEEP_IDLE_FREQ_MHZ = 40`) | isolates the wake-rate work |
| Fake sleep, radios off, full P5 (20 MHz) | isolates P5.4 specifically |
| Fake sleep, WiFi associated, full P5 | confirms the 80 MHz hold still engages |

**Functional smoke test (no meter needed), in this order:**
1. Long-press wheel → sleeps. Long-press again → wakes. Repeat 5x.
2. Short-press while asleep → **nothing happens** (no wake, no click landing in the UI on the next wake).
3. Sleep, wait >10 min, plug cable → charging overlay appears (within ~15 s), then re-sleeps.
4. Sleep with WiFi associated → confirm the link survives (P5.4's 80 MHz hold).
5. Auto-sleep via `disp_timeout_second` → confirm it still fires (P5.1 changed the notify path `lilygo_request_fake_sleep_toggle()` takes).
6. Wake and scroll immediately → confirm no lost or phantom detents (P5.1 skips
   `rotary.process()` while asleep, so the state machine is not sampled across the
   sleep).

**On step 6 — analysed, and it should be safe.** `lib/LilyGoLib/src/rotary/Rotary.cpp`
is the canonical Ben Buxton full-step table. An emit is only ever produced on a
transition out of `R_CW_FINAL`/`R_CCW_FINAL`, and those states are only reachable
from `R_START` via a proper partial sequence. The `11` column (both pins high — the
detent rest position, pins are pulled up by `rotary.begin()`) maps to `R_START` from
every row, so whatever the last sample before sleep was, a settled wheel leaves
`state == R_START`. The worst case on wake is therefore **one swallowed or delayed
detent, never a phantom scroll**, and only if the wheel is left parked mid-detent.
Note the same discontinuity already existed pre-P5.1: 10 Hz sampling could not track
real rotation either. Verify on hardware anyway — this is a code-reading argument.
