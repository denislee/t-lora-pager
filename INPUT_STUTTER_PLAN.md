# Input stutter after screen changes: plan

Written **2026-10-12** against **`d619ff9`** (working tree: only iOS files modified).
Items are numbered **`IS.x`**. Where an item overlaps an older `OPTIMIZATION_*.md`
item, it names that item.

**Status (2026-10-12):** IS.0 instrumentation, IS.1, IS.2 (async save; the SD clock
is not changed), IS.3, IS.4, IS.5 and IS.6 are implemented on `master`
(`d1abbae`..`88299dd`), and the builds, native tests and emulator checks pass.
**Still open:** the IS.0 hardware measurement table, the hardware feel test
(section 6), the SD clock raise (P6.19, needs a CRC read-back on the device), and
IS.7, which waits for the hardware numbers. Each item below has its own status line.

## 1. Symptom

After moving between screens, the pager's keyboard and rotary encoder feel dead or
laggy for a while. This happens most often when leaving the Notes editor after an
edit, going back to the home screen, or opening Settings. Presses either arrive late
or seem to be lost.

"Change the screen" is read here as *navigating between apps*. If it also means a
display setting (theme, font, rotation), add those to the repro matrix in IS.0. A
font change re-lays out the whole status bar (`core/system.cpp` status timer,
`applied_header_font` branch).

## 2. How input reaches the UI today

```
TCA8418 ISR ──notify──▶ kb_reader task (core 0, prio MAX-2)
                          └─ takes core::ScopedInstanceLock ◀── the problem
                          └─ instance.kb.getKey() over I2C
                          └─ queue (32, drop-oldest) ─▶ hw_input_event_wake()
rotary vendor queue ───▶ rotary_reader task (core 0) ─▶ queue (16), NO instance lock

lvgl task (core 1, prio 8)
  └─ ScopedInstanceLock {
         lv_timer_handler()   ← render + SPI flush + every timer + every event cb
                                (app teardown/build, saves, menu rebuild all run here)
         System::loop()       ← AppManager::update() (queued switches), notify::pump()
     }
  └─ ulTaskNotifyTake(next)  ← woken by input (P6.6)
```

Key point: **the keyboard reader needs the same mutex that the LVGL task holds for the
whole of every frame.** Anything slow that runs inside an event callback or a timer
(saving a note, rebuilding the home grid, building Settings, a flush that waits on the
SPI bus) also blocks the keyboard. The rotary path was already decoupled for this exact
reason (`hal/rotary_task.cpp` header comment). The keyboard path was only half-decoupled.

## 3. Root-cause candidates (ranked)

Mechanisms below were checked by reading the code. **Magnitudes are not measured yet.**
IS.0 is there to measure them before any fix lands.

| # | Candidate | Evidence | Confidence |
|---|---|---|---|
| A | **Keyboard I2C read is gated on the UI mutex.** Every long LVGL-thread operation freezes key reads. | `hal/keyboard_task.cpp` `keyboard_task_fn`: `core::ScopedInstanceLock lock;` around `instance.kb.getKey()`. `hal/lvgl_task.cpp` holds the same lock around `lv_timer_handler()` + `System::loop()`. | High (mechanism) |
| B | **Leaving the editor does synchronous storage I/O on the LVGL thread.** The save encodes (AES when notes crypto is on), writes FFat, then writes SD at **4 MHz**, all under the instance mutex. | `apps/ui_text_editor.cpp` `do_exit()` → `save_content(true)` → `hal/storage.cpp` `hw_save_preferred_file` (`fs_write_all(FFat…)`, then `ScopedSpiLock` + `fs_write_all(SD…)`). `ui_text_editor_exit` calls `save_content` again (a no-op if already clean). SD clock: `lib/LilyGoLib/src/LilyGo_LoRa_Pager.cpp` `SD.begin(SD_CS, SPI, 4000000U…)` (= P6.19, still open). | High (mechanism) |
| C | **Returning home rebuilds the whole home screen**, then repaints it in full, all inside one locked frame. | `core/system.cpp` `System::showMenu` → `switchApp("MainMenu", _menuPanel)` → `lv_obj_clean` + `MenuApp::onStart` (`apps/menu_app.cpp`, ~340 lines: pills, grid, `lv_obj_update_layout`, badges, shortcut cbs on every group member). | High (mechanism) |
| D | **SPI-bus chain.** Background tasks touch the SD card at 4 MHz while holding the SPI lock. The display flush, running inside the LVGL frame, then waits for that lock while still holding the instance mutex, so the keyboard reader is starved too. | Background SD users: journal background refresh (`apps/ui_journal.cpp` `cancel_bg_refresh`/refresh job), `notes_prune` (`hal/storage.cpp` `prune_task`), `nsync_bg` (`apps/ui_notes_sync.cpp`). Flush: `LV_Helper_v9.cpp` `disp_flush` → `pushColors` (synchronous, no DMA, takes the vendor SPI lock). | Medium-high |
| E | **One shared focus group for the home screen and every app.** Home tiles are not cleaned when an app opens (only `_appPanel` is cleaned), so they stay in `_menuGroup` next to the app's widgets. Encoder/Tab navigation can then land focus on invisible, off-tile home widgets, which feels like dead keys. Separately, `System::showBackButton` rebuilds the group from a snapshot capped at **32** members (`if (n > 32) n = 32;`) and silently drops the rest. Home tiles + Settings items + subpage widgets can go over that cap. | `core/system.cpp` `setupGlobalUI` (single `_menuGroup`), `showBackButton`. `core/app_manager.cpp` `switchApp` cleans only the target parent. `apps/menu_app.cpp` `tile_click_cb`/`launch_app_from_home` (hideMenu + switchApp, no group swap). | Medium (verify with group dump) |
| F | **The app is torn down from inside its own event callback.** ESC in the editor's `LV_EVENT_KEY` handler calls `do_exit()` → `menu_show()` → `switchApp`, which deletes the textarea that is mid-dispatch. The back button does the same through `back_btn_wait_release_cb` + `lv_indev_wait_release`. LVGL's indev reset/wait-release after a delete can swallow the first press on the new screen. | `apps/ui_text_editor.cpp` `text_area_event_cb` (ESC branch), `exit_btn_cb`. `core/system.cpp` `back_btn_wait_release_cb`. `AppManager::queueSwitchApp` exists but these paths don't use it. | Medium-low |
| G | **Stale keys burst into the next screen.** `disable_keyboard()` only disables the LVGL indev. The reader keeps filling its 32-deep queue, and `enable_keyboard()` replays the backlog into whatever screen is now focused. | `ui_tools.cpp` `disable_keyboard`/`enable_keyboard`; `hal/keyboard_task.cpp` queue is never flushed. | Low |
| H | **Group editing flag survives teardown.** `lv_group_remove_obj` (LVGL 9.5 `lv_group.c`) does not clear `group->editing`. The home screen recovers because `lv_group_focus_obj` clears it, but direct app→app switches (editor pills → Journal / Notes Sync) never pass through it. | `apps/ui_text_editor.cpp` `float_btn_click_cb` → `switchApp` directly. | Low |

**Re-rank after IS.0 (emulator group dump, 2026-10-12).** The first half of **E** is
wrong. Leaving home runs `MenuApp::onStop` → `core::App::onStop`, which
`lv_obj_clean`s the menu panel, so the home tiles were *deleted* on every app launch,
not left behind in `_menuGroup`. Every app's group held only its own widgets plus
the back button (Settings root: 22 members, under the 32 cap). What is left of **E**
is the 32-member cap. **C** is confirmed and is worse than described: home was torn
down on every launch *and* rebuilt on every return. **H** stands, because every
screen shared one group object. No hardware numbers yet, so A/B/D keep their
mechanism-only ranking.

Likely combination: **A** is the amplifier. **B + C** (plus **D** whenever a background
task is touching SD) are the long frames that A turns into "keys don't work". **E/F**
can make a key that *was* delivered appear to do nothing.

## 4. Plan

### IS.0: Measure and reproduce first (no behaviour change)

**Status: instrumentation DONE `d1abbae`; hardware table OPEN.** `core/input_profile.{h,cpp}`
is built by `pio run -e tlora_pager_profile` (`-D INPUT_PROFILE`, `[prof]` lines on
serial). It reports the lock hold histogram and holder, the kb_reader lock wait, key
enqueue→dequeue p50/p95/max, frame time, spans, and a group dump on each switch.
With no device attached, only the emulator group dump has been run (section 7).

Repo rule: nothing is time-measured yet. Get numbers first, so each fix is proven by a
before/after comparison.

1. **Instance-lock hold tracker** (debug build flag `-D INPUT_PROFILE`):
   - In `core/instance_lock.cpp` `instanceLockTake`/`Give`, record `esp_timer_get_time()`
     and `pcTaskGetName(NULL)`.
   - Keep the max hold time and the holder name, plus a coarse histogram (<5, <16, <33,
     <100, <300, ≥300 ms).
   - Log any hold ≥ 50 ms right away, with the holder name.
   - Also record how long `kb_reader` **waited** to acquire the lock. This is the
     number that matches the user's symptom.
2. **Span timers** (same flag) around:
   - `AppManager::switchApp` (split into onStop / `lv_obj_clean` / onStart)
   - `save_content`
   - `hw_save_preferred_file` (split into encode / FFat / SD)
   - `MenuApp::onStart`
   - `ui_sys_enter`
   - `lv_timer_handler` (per-frame duration, worst frame)
3. **Key latency:** stamp each `KeyEvent` with `esp_timer_get_time()` at enqueue. In
   `kb_read_cb`, log dequeue − enqueue. Report p50/p95/max per 10 s window.
4. **Group dump** (emulator + device): on every `switchApp`, log
   `lv_group_get_obj_count(default)` and, for each member, its class, whether it is
   on-screen (`lv_obj_is_visible`), and its owning panel (menu or app). This confirms
   or rules out E.
5. **Repro matrix.** Record the numbers above for each case:
   - Home → Notes → type 50 chars → ESC/back → home → press keys at once.
   - Same, with notes encryption ON (AES + PBKDF2 prewarm path).
   - Same, with the SD card in and out.
   - Home → Settings → subpage → back → back.
   - Journal → open note → edit → back (journal background refresh after dirty).
   - Editor pill → Journal / Notes Sync (app→app without passing through home).
   - Display setting change (theme / font), if that is part of "change the screen".
6. **Exit criteria for IS.0:** a table in this doc that gives, for each scenario, the
   worst instance-lock hold (with holder), the worst kb_reader wait, the key-latency
   p95/max, and the group size. Re-rank section 3 from those numbers before fixing.

### IS.1: Decouple the keyboard reader from the UI mutex (fixes A)

**Status: DONE `3fd1de6`.** `core::ScopedKbLock` (`core/kb_lock.{h,cpp}`) is a recursive
keyboard-only mutex. It is taken by the reader, `setNotifyTask`, `hw_set_kb_backlight`,
`hw_enable_keyboard`, `hw_disable_keyboard`, `hw_set_keyboard_read_callback`, and
around `lightSleep()`/`hw_sleep()` in `hw_low_power_loop`. `graft grep "instance.kb"`
found no other sites in `src/`. `CONFIG_DISABLE_HAL_LOCKS` is not set in the
arduino-esp32 SDK config, so Wire's per-transaction lock is on.
`isAltKeyPressed()` returns the cached `alt_key_pressed` bool, so no I2C. Lock order
is instance → kb, and nothing takes the instance lock under the kb lock.
Open: soak test on hardware (WiFi scan, NFC, charging, sleep/wake).

- Replace the instance-lock hold in `keyboard_task_fn` with a **dedicated keyboard/I2C
  lock**, using a new `core::ScopedKbLock` that follows the `spi_lock` pattern.
- Take it at every other site that touches `instance.kb`:
  - the sleep/wake path (`hw_power_down_all` / `hw_power_up_all`, `instance.kb.end()/begin()`)
  - `setNotifyTask` in `hw_keyboard_task_start`
  - any brightness/backlight call on the keyboard
  - Find all of them first with `graft grep "instance.kb"`.
- Check that arduino-esp32 2.0.17 `Wire` has its HAL bus lock enabled
  (`CONFIG_DISABLE_HAL_LOCKS` unset). If so, other I2C users on the same bus (PMIC,
  RTC, sensors) are already serialised per transaction and do not need the instance
  mutex for bus safety.
- Keep `instance.isAltKeyPressed()` in `rotary_read_cb` safe: it reads cached state,
  but confirm that.
- **Expected effect:** keys are read and queued even while the UI is busy. They are
  then applied in one burst when the frame ends (`continue_reading`), exactly like the
  rotary path today. This does not make long frames shorter, but it removes "lost" and
  "dead" keys.
- **Risk:** MED. This changes I2C concurrency. Soak test with typing during WiFi scan,
  NFC polling, charging, and sleep/wake cycles.

### IS.2: Take the note save off the exit path (fixes B)

**Status: DONE `6e14ff3`, except the SD clock.** `hw_save_preferred_file_async()`
(`hal/storage`) saves on a `note_save` task (core 0, prio 2, 10 KB stack). It allows
one save in flight, and any later call waits behind it. Every public reader, lister,
deleter and bulk copy in `storage.cpp`/`storage_bulk.cpp`, plus the sync
`hw_save_preferred_file`, waits for a pending save first (the ordering rule). A
failure posts an Error banner through `core::notify` from the editor's callback.
`ui_pause_timers`, `hw_low_power_loop` and `hw_shutdown` call `hw_save_flush()`. Any
PBKDF2 derive runs on the worker. The worker never takes the instance lock, so the
LVGL thread can wait on it safely. Not done:
- **SD clock (P6.19):** no hardware here to run the CRC read-back, so the vendor
  `SD.begin(…, 4000000U)` is unchanged.
- **Native ordering test:** the ordering lives in FreeRTOS code, not pure logic, so it
  has no native test.
- **Pull-power durability test:** still to do on hardware.

- `do_exit` / `ui_text_editor_exit`:
  - Snapshot the textarea text into a heap `std::string`.
  - Hand it to a `core::OneShotJob` (the same pattern as `menu_app.cpp` `s_inet_job`)
    that runs `hw_save_preferred_file` on a worker task (core 0, low priority).
  - Return to the menu immediately.
- **Correctness rules (must hold):**
  - **Ordering:** a re-open of the same path, Journal listing, or Notes Sync must wait
    for, or see, the pending save. Keep a "pending save path" flag in `hal/storage`,
    and have `hw_read_file`/sync block briefly on it, or queue behind it.
  - **Failure surfacing:** the save worker posts failures via `notify::post()`, so the
    user still sees "Save failed".
  - **Sleep/power-off during a pending save:** `ui_pause_timers` and the
    power-off/reboot paths must flush (join) the save first.
  - **Encryption:** the key must already be cached (prewarm). If it isn't, do PBKDF2
    in the worker, never on the LVGL thread.
- **SD clock (P6.19):** raise the pager/ultra `SD.begin` frequency (try 20 MHz, then
  40 MHz). Verify with a read-back CRC of a large file and the display under load. This
  shortens every SD hold on the shared SPI bus (helps B and D).
- **Risk:** MED (durability). Add a native unit test for the pending-save ordering
  logic if it is factored as pure logic. Hardware test: pull power 1 s after exit and
  confirm the note survived.

### IS.3: Stop rebuilding the home screen on every return (fixes C)

**Status: DONE `88299dd`.** `MenuApp::persistentUi()` makes AppManager skip the clean.
`onStop` pauses the timers and remembers focus. `onStart` resumes unless the layout
key changes (style, dark mode, home font, visible-tile mask, panel size). This
replaces the planned dirty flag/`applyChromeStyle` hook: `ui_sys_exit` calls
`applyChromeStyle` on every Settings exit, so that hook would have rebuilt home
after every Settings visit. Emulator: the full screenshot sweep is pixel-identical to
the pre-change sweep apart from the clock (8 style × mode combos × 32 pages, exit 0).
Home before and after a Tasks round trip differs by 63 px (the clock), with focus
restored to the same tile.

- Build `MenuApp` once and keep it alive. On return:
  - refresh badges, WiFi gating, media visibility and the toggle states (all already
    timer-driven)
  - re-order tiles only when the activity order or visibility actually changed (dirty
    flag set by `home_apps_set_visible` / theme change)
  - restore focus to the last-used tile
- `AppManager::switchApp` needs a "persistent app" notion: do not `lv_obj_clean`
  `_menuPanel` when the target is `MainMenu` and it is already built.
- **Theme/font change** must still force a rebuild. `applyChromeStyle` is the hook.
- **Risk:** LOW-MED. The screenshot sweep (`UI_SHOTS_DIR=…`) covers the visual side in
  every style × dark mode.

### IS.4: Separate focus groups per screen (fixes E, H)

**Status: DONE `88299dd`.** It was needed once IS.3 kept the home widgets alive.
`System::enterApp()` is the single entry point: it empties `_appGroup`, clears
editing, makes it the default, then switches. `showMenu()` restores `_menuGroup`
with editing cleared. Home tiles/shortcuts/Settings go through `queueEnterApp`, as do
the editor pills, the file-browser open and the screenshot sweep. `showBackButton`
snapshots the whole group into a `std::vector`. Emulator group dump after the
change: Tasks 4, Editor 5, Journal 2 (back button + that app's own widgets only).

- Give the app panel its own group:
  - On `hideMenu()` + app launch, `set_default_group(_appGroup)` after
    `lv_group_remove_all_objs(_appGroup)` and `lv_group_set_editing(_appGroup, false)`.
  - On `showMenu()`, switch back to `_menuGroup`.
  - Do this in **one** place: `AppManager::switchApp`, or a `System::enterApp(name)`
    helper that `tile_click_cb`, `launch_app_from_home`, `settings_click_cb` and the
    editor pills all call. No more scattered `hideMenu()+switchApp()` pairs.
- `showBackButton`: remove the fixed `members[32]` cap. Either use
  `lv_group_swap_obj`/insert-at-front semantics, or size the snapshot from
  `lv_group_get_obj_count`. At minimum, `LV_LOG_WARN` when the cap is hit.
- Overlays (`ui_overlay_open/close`, `create_msgbox`/`destroy_msgbox`) already swap
  groups, so they need no change. Re-verify they restore the *app* group after IS.4.
- **Risk:** MED. Focus behaviour changes on every screen. Use the group dump from IS.0
  as the regression check: an app's group must contain only that app's widgets plus
  the back button.

### IS.5: Never tear down from inside the dying widget's event (fixes F)

**Status: DONE `a841aa1`** (the file-browser → editor open followed in `88299dd`).
`menu_show_deferred()` / `System::queueShowMenu()` / `queueEnterApp()` run from
`System::loop()` after `lv_timer_handler()`. Every `menu_show()` in `src/apps/` became
the deferred form. `do_exit` paths no longer call their `ui_*_exit` before the switch
(`onStop` does it). While a transition is queued, the keypad and encoder read
callbacks leave input queued (`ui_transition_pending()`), so keys typed during the
switch reach the new screen. `back_btn_wait_release_cb` is removed.
Emulator xdotool flows give the same switch sequence as the pre-change build: home →
`n` → type → ESC×2 → `s`; tile Enter/Tab paths; editor pill → Journal.

- Route every "leave this app" path through `AppManager::queueSwitchApp`, applied in
  `System::loop()` after `lv_timer_handler` returns. That covers the editor ESC branch,
  the back-button callbacks, `float_btn_click_cb` and `editor_unlock_result_cb`'s
  `menu_show()`.
- Add a `menu_show_deferred()` (queued `showMenu`) and use it from event callbacks.
  Keep the synchronous `menu_show()` for non-event callers.
- Then re-check whether `back_btn_wait_release_cb` is still needed. With deferred
  teardown, the triggering key is fully released before the new screen exists.
- **Risk:** LOW-MED. Watch for apps that relied on the synchronous switch to happen
  before they continue, e.g. code after `menu_show()` that touches now-deleted objects.
  That code is actually a latent bug the deferred switch makes safe.

### IS.6: Drop stale input across transitions (fixes G)

**Status: DONE `9bf377d`, keyboard only.** `hw_keyboard_flush_queue()` runs from
`enable_keyboard()` only when the indev was disabled for more than 300 ms. An
unconditional flush would discard the keys IS.1/IS.5 now preserve during a
transition (`disable_keyboard` and `enable_keyboard` run in the same frame on an app
switch). The rotary queue is not flushed on app switch, for the same reason. The
IS.5 hold already delivers in-transition scrolls to the new screen.

- In `enable_keyboard()`, or a new `hw_keyboard_flush_queue()` called from it, drain
  `s_event_queue` and clear `s_held_char`. Do the same for the rotary queue on app
  switch.
- Alternative: stop toggling the LVGL indev per app at all. With IS.4, each screen
  owns its group, so the per-app disable has little value.
- **Risk:** LOW.

### IS.7: Shrink what's left of the transition frame (only if IS.0 numbers still say so)

**Status: OPEN, waiting for the IS.0 hardware numbers** (`pio run -e tlora_pager_profile`).

- **Spread big builds over several frames:** build Settings' main grid in two passes
  (first visible row now, the rest via `lv_async_call`), and Journal list rows in
  chunks. The goal is no single locked frame over ~33 ms.
- **CPU frequency:** confirm P6.11 (commit `4761042`) also raises the clock on app
  switches started from a timer or async path, not only on key enqueue.
- **P6.12** (opaque overlays re-render the hidden app): hide `_mainScreen` while a
  full-screen overlay is open.
- **Flush:** `disp_flush` is synchronous `pushColors` with no DMA. Async DMA flush with
  two partial buffers lets LVGL render the next chunk while SPI sends the previous one.
  This is the biggest structural render win but needs vendor-driver work. Plan it
  separately if IS.0 shows flush dominates frame time.

## 5. Order of work

1. **IS.0.** Instrument and fill the measurement table. Re-rank section 3.
2. **IS.1** (keyboard lock). Biggest perceived win for the least surface change.
3. **IS.5 + IS.6** (deferred teardown, flush stale keys). Small and low-risk.
4. **IS.2** (async save + SD clock). Needs the durability test.
5. **IS.4** (per-screen groups), then **IS.3** (persistent home).
6. **IS.7** only for what the IS.0 numbers still flag.

Re-run the IS.0 matrix after each step and append the numbers to section 7. Never
overwrite earlier rows.

## 6. Verification for every step

- Build **both** the hardware target and its emulator (`REFACTOR_NOTES.md` discipline):
  `pio run -e tlora_pager` and `pio run -e emulator_lora_pager`. Also build
  `twatchs3` / `twatch_ultra` when touching shared HAL (`keyboard_task`, `storage`,
  `system.cpp`).
- `pio test -e native_test` (plus any new pure-logic tests, e.g. pending-save ordering).
- Screenshot sweep:
  `UI_SHOTS_DIR=/tmp/shots xvfb-run -a .pio/build/emulator_lora_pager/program`
  (exit code = failures).
- **Hardware feel test** (pager):
  - fast typing in the editor
  - ESC out immediately after typing, then type on home at once
  - home → Settings → subpage → back × 10
  - scroll flicks during a Journal background refresh
  - hold-to-repeat
  - Alt+Backspace
  - sleep/wake mid-typing
- **Targets** (to confirm against IS.0 baselines):
  - kb_reader lock wait max < 50 ms during navigation (after IS.1, ~0)
  - key-to-dequeue p95 < 50 ms
  - no LVGL frame > 100 ms on any navigation in the matrix
  - zero lost keys over a scripted 200-key burst during exit-from-editor
- Update each `apps/ui_*.cpp` "State reset on onStop" block when teardown changes
  (IS.2/IS.3/IS.5).

## 7. Measurements log

_Append-only. One row per run: date, commit, scenario, worst lock hold (holder), worst
kb wait, key latency p95/max, worst frame, group size._

| Date | Commit | Scenario | Worst hold (holder) | kb wait max | key p95 / max | worst frame | group size |
|---|---|---|---|---|---|---|---|
| | | | | | | | |
| 2026-10-12 | d1abbae | emulator sweep, group dump only (no lock/latency on emulator) | — | — | — | — | MainMenu 13, Editor 5, Tasks 4, Settings 22, Files 10, SSH 12, Remote 8, Chat 5, others 1–2 |
| 2026-10-12 | 88299dd | emulator xdotool flow, group dump after IS.3/IS.4 | — | — | — | — | MainMenu 13, Tasks 4, Editor 5, Journal 2 (app group holds only the app + back) |
