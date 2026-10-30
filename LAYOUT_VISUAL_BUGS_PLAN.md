# Layout and visual bugs: audit and plan

Written **2026-10-25** against **`4e83da5`** ("fix: close the unlock loader after the canary
and prewarm journal/secret keys in the background"). The working tree was clean.

Items are numbered **`VB.x`**. This series is separate from `P2.x`…`P7.x`, `PB.x` and `IS.x`.

> **Status (2026-10-25): VB.1–VB.20 fixed** in the working tree and checked against the emulator
> sweep for all three form factors. Notes on how each was resolved:
> - VB.1: `AppManager::createAppRoot` gives every non-persistent app a fresh full-size child
>   (4 px pad, transparent, borderless), so a solo `UI_SHOTS_ONLY=hotel-light` sweep is now
>   pixel-identical to the same combo in a full sweep (status bar excluded).
> - VB.3: glyphs replaced with ASCII. `make lint-glyphs` (`tools/check_ui_glyphs.py`) rejects
>   new ones.
> - VB.6/9/10/11: `ui_is_narrow()` (< 320 px) drives the watch layouts: compact counter with
>   the clock hidden, a one-column Settings grid, stacked label-over-control rows, and the Files
>   strip above the list.
> - VB.7: the floating button is a filled accent disc, and Settings pages, Files, Tasks and
>   Recorder reserve bottom padding for it. The six apps without one still rely on the
>   status-bar back button, which is tappable on the watches.
> - VB.20 was a real bug: the tile's focus halo was clipped by the grid on the top/left. The grid
>   now always has `OVERFLOW_VISIBLE`.

## 1. Method

The emulator's screenshot sweep (`src/emu_shots.cpp`) was run headless for all three form
factors. It covers every app, every Settings subpage, the shared msgbox and the loader, in
4 styles × light/dark:

```bash
pio run -e emulator_lora_pager -e emulator_twatchs3 -e emulator_watch_ultra
for e in emulator_lora_pager emulator_twatchs3 emulator_watch_ultra; do
  SDL_VIDEODRIVER=offscreen UI_SHOTS_DIR=/tmp/shots/$e .pio/build/$e/program
done
```

(`xvfb-run` isn't installed on this box. `SDL_VIDEODRIVER=offscreen` works instead.)

That produced 768 frames: pager 480×222, T-Watch-S3 240×240, T-Watch-Ultra 502×410. They
were reviewed as per-page contact sheets, and suspect regions were cropped and upscaled.
Root causes were then confirmed in the source. For VB.1, a temporary probe in the sweep was
used and then reverted.

> **Limits.** This is emulator-only. It runs LVGL 9.2.2, while the hardware runs 9.5/9.6.
> The sweep only shows each page at rest. Open dropdowns, on-screen keyboards, the lock
> screen, Wi-Fi/list pickers and populated lists (notes, journal, chats) were **not** covered.
> Battery % and "…" values differ between frames because of emulator randomness and the
> 300 ms pump, not because of bugs.

## 2. Findings, ranked

### High

#### VB.1: Apps leak layout styles onto the shared app panel (state-dependent layout everywhere)
Every app gets the same persistent `_appPanel` tile (`src/core/system.cpp:99`).
`AppManager::switchApp` only `lv_obj_clean()`s it (`src/core/app_manager.cpp:49-53`). It
never resets styles or flags. Five apps restyle that shared panel and never undo it:

| App | Lines | What it sets on `parent` |
|---|---|---|
| Chat | `src/apps/ui_chat.cpp:902-909` | pad 6, pad_row 4, flex column, not scrollable |
| SSH | `src/apps/ui_ssh.cpp:889-896` | pad 6, pad_row 4, flex column, not scrollable |
| Notes Sync | `src/apps/ui_notes_sync.cpp:593-600` | pad 6, pad_row 4, flex column, not scrollable |
| Remote | `src/apps/ui_media_remote.cpp:288-295` | pad 4, pad_row 2, flex column, not scrollable |
| Weather | `src/apps/ui_weather.cpp:1015-1031` | pad 4, pad_row 1, flex column, **scrollable + scrollbar AUTO + CLICKABLE** |

As a result, the apps that don't set panel styles render differently depending on which of
these five ran last since boot: Editor, Tasks, Settings, Files, Journal, Recorder,
Recordings and Telegram. After a cold boot they sit flush against the screen edge, with
0 px padding. In the hotel style the first row even touches the title seam. After the user
opens Chat they gain 6 px padding and become a flex column. After Weather they also become a
clickable, scrollable panel.

- **Proof:** a probe that re-entered Editor showed it flush as the first app after boot and
  padded after the sweep had passed through Chat. Running `UI_SHOTS_ONLY=<one combo>` gives
  frames that differ from the same combo in a full sweep (AE 5.7k–12.9k px on Settings/Files).
- **The sweep hides the bug.** Every combo after the first is shot in the "after Chat"
  state, so a regression here won't show up in diffs.
- **Fix:** restore a baseline before `onStart` (or from `App::onStop`), or give each app its
  own full-size child root that it may style freely. The second option is cleaner:
  `AppManager` creates `lv_obj_create(parent)` with zero pad/border and transparent bg, and
  passes *that* as `parent`. Then decide the intended padding once (probably 4–6 px) and
  apply it explicitly to the apps that relied on the leaked padding.
- **Verify:** add a sweep mode that cold-boots per app (or that re-shoots Editor after
  every app) and confirm the frames are identical.

#### VB.2: Home tile grid overflows: top row clipped under the toggle strip, bottom row cut off
In `src/apps/menu_app.cpp:844-847`, `tile_h = (grid_h - (rows-1)*gap) / rows` is clamped up
to **50**. On the pager, 7 tiles give 3 rows: `grid_h` ≈ 142 → 42 → 50. Three rows then need
166 px in a 142 px grid. The grid is `LV_FLEX_ALIGN_CENTER` and **not scrollable**
(`:792-795`), so the 24 px overflow is split. The icons of the top row (Notes/Tasks/Recorder)
disappear under the Wi-Fi/BT/settings strip, and the SSH tile is half off-screen and can't
be scrolled into view. This happens on the pager and the T-Watch-S3, in all styles. The
Ultra fits.
- **Fix:** make the grid scrollable with `scroll_on_focus` and align it to START when it
  overflows, or derive the column count from the available height (4 columns on the pager
  gives 2 rows). The icon font is already picked from `tile_h` (`pick_icon_font`), so a
  shorter tile is fine.

#### VB.3: Missing glyphs render as tofu boxes (□)
The UI fonts don't contain `»` U+00BB, `—` U+2014 or `→` U+2192. Weather already strips the
`°` sign for the same reason (`ui_weather.cpp:174`). Visible in the sweep:
- Chat: "hub disabled. Settings □ Local Hub." (`ui_chat.cpp:1000`)
- Notes Sync: "Settings □ Notes Sync." and "Private repos work □ create…" (`ui_notes_sync.cpp:384,612-613`)
- Telegram: "open Settings □ Telegram…" (`ui_telegram.cpp:1650`)

These were not on screen in the sweep, but the same glyphs appear in user-visible strings:
- `ui_telegram.cpp:1531` (`→`) and `:908`
- `ui_notes_sync.cpp:244`
- `settings_hub.cpp:51-52,67,70`
- `settings_notes_sync.cpp:47,50`
- `settings_telegram.cpp:60,301,358`
- `ui_settings.cpp:506`
- `nfc_provision.cpp:116`
- `ui_nfc_test.cpp:160`
- `settings_imu_debug.cpp:157`
- **Fix:** replace them with `LV_SYMBOL_RIGHT` / ASCII `-` / `>`, or add the three code
  points to the generated font ranges. Add a unit or lint check that rejects non-ASCII,
  non-`LV_SYMBOL_*` code points in string literals under `src/apps/`.

#### VB.4: Classic style: sliders are invisible
Classic passes a **black** primary to `lv_theme_default_init` (`src/ui_theme.cpp:113`,
`:750-754`), and classic adds no slider styles (`k_theme_styles[CLASSIC] = {}`). The
indicator, knob and track are therefore black or near-black on the black page. In
Settings › Display (Screen / Keyboard / Timeout / Sleep Mode) and Settings › Charger
(Current), only the label and the value text show, and there is no visible control. This
happens on all devices.
- **Fix:** give classic slider/bar indicator + knob styles in `UI_COLOR_ACCENT` (orange) with
  a `surface` track, or pass the accent as primary and keep classic's buttons black locally.

#### VB.5: Settings › Devices: device names collapse to "..."
`src/apps/ui_settings.cpp:253-255` sets `LV_LABEL_LONG_DOT` plus `max_width = LV_PCT(65)`
on the name label. Every name longer than ~3 characters renders as just "..." (radio,
Haptic Drive, Power management, Real-time clock, PSRAM, …). Only "GPS" survives. This
happens on all devices and styles. The percentage is likely resolved while the lazily built
page still has no width, and the dotted text is then baked in.
- **Fix:** use `flex_grow(1)` on the name with `LONG_DOT` and a `min_width` of 0, instead of
  a percentage max width, or set the long mode after the first layout.

#### VB.6: T-Watch-S3: Editor word counter overlaps the clock
"0 words | 0 chars" (`ui_text_editor.cpp:547`) sits left of the centred
"25/10/2026 16:40", and on 240 px they draw over each other. This happens in all styles.
- **Fix:** on narrow screens, hide the clock while the counter is shown, or shorten the
  counter ("0w 0c"), or move the counter into the editor area.

#### VB.7: Watch floating back button covers content
`create_floating_button` (`src/ui_tools.cpp:782-793`) is screen-level, `BOTTOM_MID −30`, and
no app reserves space for it. On the S3 it covers:
- the Telegram settings "Banner on new msg" row
- Date & Time "Apply" and "Timezone"
- the Fonts page dropdowns
- Settings grid rows
- Info/IMU rows

On the Ultra it covers Date & Time › "Sync from GPS". In **classic** the button has no
background, so it shows as a bare `<` glyph drawn over the text underneath.
- **Fix:** add bottom padding ≥ `FLOAT_BUTTON_HEIGHT + 30` to scrollable pages on watch
  builds (or move it to a corner), and give the classic variant a filled circle.
- **Related:** only 6 apps create it (Files, Journal, Settings, Editor, Recorder, Tasks).
  Remote, Weather, Telegram, Notes Sync, SSH and Chat have none on the watches. Decide
  whether that's intended.

### Medium

#### VB.8: Slider value labels collide with the knob/track
The value label is appended right after a `flex_grow(1)` slider with no gap
(`settings_display.cpp:221-223`, `:306-311`). The knob overhangs the track, so it overlaps
"100%" (Keyboard) and "1000mA" (Charger Current). This happens in pixel, hotel and beos, on
every device.
- **Fix:** give the row a `pad_column`, or give the value label a fixed `min_width` sized for
  "100%"/"1000mA" plus `text_align RIGHT`, and add a horizontal margin equal to the knob
  overhang.

#### VB.9: Settings main grid: badges under the scrollbar; mid-word wraps on 240 px
- **Pager, all styles but classic:** the right column's shortcut badges (F, C, V, L, G, E…)
  sit under the page scrollbar.
- **T-Watch-S3:** the `LV_PCT(48)` two-column grid (`ui_settings.cpp:582-596`) leaves ~100 px
  per tile. Labels break mid-word ("Connecti/vity", "Display &/Backligh/t") and badges
  crowd the text.
- **Fix:** add right padding equal to the scrollbar width on the main page, and use a single
  column below ~320 px width.

#### VB.10: T-Watch-S3: Settings key/value rows too narrow
- **Fonts:** labels wrap mid-word ("Syste/m", "Journa/l", "Weath/er", "Telegra/m") and
  "Montserrat" is clipped by the chevron.
- **Performance:** "240 MHz (Max)" is clipped under the chevron.
- **Display:** "Sleep Mode" runs into the slider knob in hotel.
- **Fix:** stack label above control below ~320 px, or shrink the dropdown and let the label
  take its natural width.

#### VB.11: T-Watch-S3: Files side panel buttons too narrow
Storage and filter buttons wrap by character ("Int/ern/al", "./txt/onl/y", "No/.txt"), and
the panel runs off the bottom. This happens in all styles.
- **Fix:** on narrow screens use icon-only buttons, or move the filter into a dropdown/header.

#### VB.12: T-Watch-S3: Home tile labels wrap mid-word
"Record/er", "Telegra/m", "Weathe/r" wrap mid-word. This happens in all styles.
- **Fix:** use `LV_LABEL_LONG_DOT` with one line, or a smaller label font when `tile_w` is
  below ~80 px.

#### VB.13: T-Watch-S3: Tasks row clipped, with a horizontal scrollbar over the text
"• Dummy content for simulation" is cut at the right edge. A horizontal scrollbar (orange in
pixel, grey in hotel/beos) is drawn across the text's descenders. The labels use
`LV_LABEL_LONG_CLIP` (`ui_tasks.cpp:135,337`) inside a container that scrolls horizontally.
- **Fix:** use `LV_LABEL_LONG_WRAP` (or `DOT`) with width 100%, and `scroll_dir VER` on the
  container.

#### VB.14: Hotel light: focus ring on the status-bar back button is invisible
`.focus` = `.titlebar` = `0x1e7295` (`src/ui_theme.cpp:144,149`). The focused back button on
every Settings subpage / Journal / Recorder looks unfocused in hotel-light. Hotel-dark is
fine.
- **Fix:** for widgets on the title bar, use `title_fg` (white) as the focus border colour,
  or add a title-bar-specific focus token.

#### VB.15: SSH: pill focus outline clipped into broken white arcs
`style_pill` (`ui_ssh.cpp:639-650`) draws a 2 px white outline with 2 px pad. The parent row
clips it, so the focused "Connect" shows white crescents left and right of its accent
border. This happens in all styles and on all devices. The home toggle strip already solves
this with `LV_OBJ_FLAG_OVERFLOW_VISIBLE` plus side padding (`menu_app.cpp:686-691`).
- **Fix:** do the same on the SSH header row, or drop the outline and rely on the theme
  focus border.

#### VB.16: Chat: mic button is a thin, too-tall sliver
`make_btn` (`ui_chat.cpp:969-980`) fixes height 40 with a content width. In pixel, hotel
and beos it renders as a ~16 px-wide pill that is taller than the one-line input next to it.
- **Fix:** use a square size (e.g. the input's height), or `lv_obj_set_size(b, 40, 40)` and
  match the textarea height.

### Low

#### VB.17: Icon glued to label text in action rows
In "⌨Set hub URL", "✕Clear URL", "⌨Set city", "📁Set repo", "⌨Set passphrase" and similar
rows, `create_row` (`src/ui_tools.cpp:689-705`) adds the icon image and label with no column
gap. The main Settings grid does space them. This happens in all styles.
- **Fix:** set `pad_column` 4–6 on the `lv_menu_cont` in `create_row`.

#### VB.18: Date & Time: separators and value alignment
- The `:` between Hour and Min sits halfway between the label row and the value row. It
  should align with the values.
- In classic and pixel, Year/Mon/Day values are left-aligned while their labels are
  centred, so "2026" sits left of "Year".
- On the S3 (pixel/hotel) the "Apply" button is shorter than its text, and the label is
  clipped at the bottom.

#### VB.19: Editor: cursor sits on the textarea border
The textarea has no inner left/top padding, so the block cursor overlaps the frame's top-left
corner. This happens in all styles.
- **Fix:** give the textarea `pad_left/pad_top` ≥ 2.

#### VB.20: Classic home tile focus shows an offset second edge
The focused tile (Notes) shows an extra orange edge offset down-left from its focus border.
It looks like the `outline_pad` plus a shadow from the focus style rather than a deliberate
iso shadow (which classic doesn't have). Check intent before fixing.

## 3. Checked and *not* bugs

- Classic light and classic dark look identical. This is by design: classic and pixel share
  one palette row (`ui_theme.cpp:63-65`).
- The msgbox has no title in classic/pixel. This is deliberate (`ui_tools.cpp:457-460`).
- System Info / IMU Debug values show "…". They are filled asynchronously after the
  sweep's 300 ms pump.
- The sweep's loader covers the status bar and shows the detail line instead of the
  subtitle. This is the existing `ui_loading_t` design.
- An apparent white gap in "Copy Notes -> Phone" (beos-light Storage) was a montage
  artefact. It isn't present in the full-resolution frame.

## 4. Suggested order

1. **VB.1** first. It changes the padding of half the apps, so fix it before tuning any
   spacing. Re-take the baseline sweep afterwards.
2. **VB.3, VB.4, VB.5.** These are cheap and visible on every device.
3. **VB.2, VB.14, VB.15, VB.8.** Pager polish.
4. **VB.6, VB.7, VB.9–VB.13.** One narrow-screen pass for the 240 px watch. Consider a
   single `ui_is_narrow()` (width < 320) switch instead of per-app tweaks.
5. **VB.16–VB.20.**

## 5. Verification for every step

- Rebuild the hardware target and its emulator (`REFACTOR_NOTES.md` discipline).
- Re-run the sweep for all three emulators and compare against the 2026-10-25 baseline. Once
  VB.1 is fixed, a combo shot alone (`UI_SHOTS_ONLY=…`) must match the same combo inside a
  full sweep.
- Extend the sweep so these states are covered too: open dropdown, keyboard/textarea
  focused, populated notes/journal/chat lists, lock screen, Wi-Fi and list pickers.
- Spot-check on the pager hardware (LVGL 9.5), especially VB.4, VB.5 and VB.8, where theme
  internals differ between 9.2 and 9.5.
