/**
 * @file      ui_theme.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-04-23
 * 
 */

#include "ui_define.h"
#include "hal/nvs.h"

// --- Style persistence ----------------------------------------------------
// Own NVS keys on purpose so the user_setting_params_t blob schema (and its
// version guard) is untouched. The style name is stored as a string; the
// legacy "pixel_iso" bool is still consulted when the string key is absent so
// a device upgraded from the two-style build keeps its choice.
static int8_t s_style = -1;   // -1 = not yet loaded from NVS

ui_style_t ui_style_get()
{
    if (s_style < 0) {
        std::string name = hal::nvs_get_str("ui", "style", "");
        if (name.empty()) {
            s_style = hal::nvs_get_bool("ui", "pixel_iso", false) ? 1 : 0;
        } else if (name == "pixel") {
            s_style = UI_STYLE_PIXEL_ISO;
        } else if (name == "hotel") {
            s_style = UI_STYLE_HOTEL;
        } else if (name == "beos") {
            s_style = UI_STYLE_BEOS;
        } else {
            s_style = UI_STYLE_CLASSIC;   // "classic" or an unknown value
        }
    }
    return (ui_style_t)s_style;
}

bool ui_style_is_pixel()
{
    return ui_style_get() == UI_STYLE_PIXEL_ISO;
}

bool ui_style_is_hotel()
{
    return ui_style_get() == UI_STYLE_HOTEL;
}

bool ui_style_is_beos()
{
    return ui_style_get() == UI_STYLE_BEOS;
}

// Hotel and BeOS share the outlined / bevelled widget chrome (as opposed to the
// flat Classic and extruded Pixel Iso pages), in both their light and dark
// palettes.
bool ui_style_has_chrome()
{
    return ui_style_is_hotel() || ui_style_is_beos();
}

// --- Dark mode ------------------------------------------------------------
// Orthogonal to the style: every style has a dark palette. Classic and Pixel
// Iso were designed dark, so their palette is the same either way; Hotel and
// BeOS swap to their night variants. Own NVS key, like the style.
static int8_t s_dark = -1;    // -1 = not yet loaded from NVS

bool ui_dark_mode_get()
{
    if (s_dark < 0) s_dark = hal::nvs_get_bool("ui", "dark", false) ? 1 : 0;
    return s_dark == 1;
}

void ui_dark_mode_set(bool dark)
{
    s_dark = dark ? 1 : 0;
    hal::nvs_set_bool("ui", "dark", dark);
    theme_init();
}

bool ui_style_is_dark()
{
    return !ui_style_has_chrome() || ui_dark_mode_get();
}

void ui_style_set(ui_style_t style)
{
    s_style = style;
    const char *name = style == UI_STYLE_PIXEL_ISO ? "pixel"
                     : style == UI_STYLE_HOTEL     ? "hotel"
                     : style == UI_STYLE_BEOS      ? "beos"
                                                   : "classic";
    hal::nvs_set_str("ui", "style", name);
    theme_init();
}

// --- Palettes -------------------------------------------------------------
// One ui_palette_t per style and mode; k_palettes indexes them [style][dark].
// Classic and Pixel Iso were designed dark, so both modes share one row. The
// night rows deepen the title bar but keep each style's identity colour.
static constexpr lv_color_t pal_hex(uint32_t c)
{
    return lv_color_t{(uint8_t)(c & 0xff), (uint8_t)((c >> 8) & 0xff), (uint8_t)((c >> 16) & 0xff)};
}

// Classic: flat, rounded, orange on black (LVGL's orange / grey palette mains).
static const ui_palette_t k_classic = {
    .accent = pal_hex(0xff9800), .muted = pal_hex(0x9e9e9e),
    .bg = pal_hex(0x000000), .fg = pal_hex(0xffffff), .surface = pal_hex(0x15171d),
    .outline = pal_hex(0x9e9e9e), .field = pal_hex(0x000000),
    .titlebar = pal_hex(0x000000), .title_fg = pal_hex(0xffffff),
    .title_icon = pal_hex(0xff9800), .title_muted = pal_hex(0x9e9e9e),
    .title_seam = pal_hex(0x000000), .primary = pal_hex(0x000000),
    .frame = pal_hex(0xff9800), .btn_face = pal_hex(0x000000),
    .backdrop = pal_hex(0x000000), .on_backdrop = pal_hex(0x9e9e9e),
    .focus = pal_hex(0xff9800), .accent_well = pal_hex(0x2a1a00),
    .radius = 8, .pill_radius = LV_RADIUS_CIRCLE, .keyline_w = 1,
    .frame_w = 2, .frame_radius = 8, .seam_w = 0,
};

// Pixel Iso: Sweetie-16, squared tiles, the title bar is the page itself.
static const ui_palette_t k_pixel = {
    .accent = pal_hex(0xef7d57), .muted = pal_hex(0x94b0c2),
    .bg = pal_hex(0x1a1c2c), .fg = pal_hex(0xf4f4f4), .surface = pal_hex(0x333c57),
    .outline = pal_hex(0x94b0c2), .field = pal_hex(0x1a1c2c),
    .titlebar = pal_hex(0x1a1c2c), .title_fg = pal_hex(0xf4f4f4),
    .title_icon = pal_hex(0xef7d57), .title_muted = pal_hex(0x94b0c2),
    .title_seam = pal_hex(0xef7d57), .primary = pal_hex(0xef7d57),
    .frame = pal_hex(0xef7d57), .btn_face = pal_hex(0x1a1c2c),
    .backdrop = pal_hex(0x1a1c2c), .on_backdrop = pal_hex(0x94b0c2),
    .focus = pal_hex(0xef7d57), .accent_well = pal_hex(0x2a1a00),
    .highlight = pal_hex(0x566c86), .depth = pal_hex(0x0b0c18), .grid = pal_hex(0x262b44),
    .radius = 0, .pill_radius = 0, .keyline_w = 1,
    .frame_w = 2, .frame_radius = 0, .seam_w = 2,
};

// Hotel: Habbo blue title bar with a yellow icon, black-ringed white faces on
// a beige room floor. Night swaps the beige room for a dim slate one. The seam
// is the title bar darkened by LV_OPA_40.
static const ui_palette_t k_hotel_day = {
    .accent = pal_hex(0x1e7295), .muted = pal_hex(0x6f6e66),
    .bg = pal_hex(0xe9e8e1), .fg = pal_hex(0x1c1c1c), .surface = pal_hex(0xf7f6f1),
    .outline = pal_hex(0x1c1c1c), .field = pal_hex(0xffffff),
    .titlebar = pal_hex(0x1e7295), .title_fg = pal_hex(0xffffff),
    .title_icon = pal_hex(0xffd34e), .title_muted = pal_hex(0x9cc3d6),
    .title_seam = pal_hex(0x124459), .primary = pal_hex(0x1e7295),
    .frame = pal_hex(0x1c1c1c), .btn_face = pal_hex(0xf7f6f1),
    .backdrop = pal_hex(0xe9e8e1), .on_backdrop = pal_hex(0x6f6e66),
    .focus = pal_hex(0x1e7295), .accent_well = pal_hex(0x2a1a00),
    .btn_bottom = pal_hex(0xd2d0c3), .select = pal_hex(0xc4e2f0),
    .scroll = pal_hex(0xbdbbb0), .divider = pal_hex(0xcfcdc2), .switch_on = pal_hex(0x3e9a2d),
    .floor = pal_hex(0x989865), .floor_line = pal_hex(0x7d7d50),
    .radius = 3, .pill_radius = LV_RADIUS_CIRCLE, .keyline_w = 2,
    .frame_w = 2, .frame_radius = 3, .seam_w = 2, .grad_darken = LV_OPA_10,
};

static const ui_palette_t k_hotel_night = {
    .accent = pal_hex(0x3a9cc6), .muted = pal_hex(0x9a988f),
    .bg = pal_hex(0x1f2326), .fg = pal_hex(0xeceae2), .surface = pal_hex(0x353a40),
    .outline = pal_hex(0x0a0b0c), .field = pal_hex(0x171a1d),
    .titlebar = pal_hex(0x0f4f6b), .title_fg = pal_hex(0xffffff),
    .title_icon = pal_hex(0xffd34e), .title_muted = pal_hex(0x8fb4c6),
    .title_seam = pal_hex(0x092f40), .primary = pal_hex(0x3a9cc6),
    .frame = pal_hex(0x0a0b0c), .btn_face = pal_hex(0x353a40),
    .backdrop = pal_hex(0x1f2326), .on_backdrop = pal_hex(0x9a988f),
    .focus = pal_hex(0x3a9cc6), .accent_well = pal_hex(0x2a1a00),
    .btn_bottom = pal_hex(0x272b30), .select = pal_hex(0x1d4a5e),
    .scroll = pal_hex(0x50555b), .divider = pal_hex(0x2c3136), .switch_on = pal_hex(0x3e9a2d),
    .floor = pal_hex(0x34352a), .floor_line = pal_hex(0x26271e),
    .radius = 3, .pill_radius = LV_RADIUS_CIRCLE, .keyline_w = 2,
    .frame_w = 2, .frame_radius = 3, .seam_w = 2, .grad_darken = LV_OPA_10,
};

// BeOS: grey bevelled R5 chrome, yellow window tab with a gold seam, blue
// keyboard-navigation keyline on the blue desktop. Night is Haiku-style dark
// grey with the yellow tab kept as the identity mark.
static const ui_palette_t k_beos_day = {
    .accent = pal_hex(0x336698), .muted = pal_hex(0x6b6b6b),
    .bg = pal_hex(0xd8d8d8), .fg = pal_hex(0x000000), .surface = pal_hex(0xe8e8e8),
    .outline = pal_hex(0x5a5a5a), .field = pal_hex(0xffffff),
    .titlebar = pal_hex(0xffcb00), .title_fg = pal_hex(0x000000),
    .title_icon = pal_hex(0x1d3f66), .title_muted = pal_hex(0x7a6200),
    .title_seam = pal_hex(0xb38f00), .primary = pal_hex(0x336698),
    .frame = pal_hex(0x5a5a5a), .btn_face = pal_hex(0xe8e8e8),
    .backdrop = pal_hex(0x336698), .on_backdrop = pal_hex(0xffffff),
    .focus = pal_hex(0x0000e5), .accent_well = pal_hex(0x2a1a00),
    .btn_top = pal_hex(0xf4f4f4), .btn_bottom = pal_hex(0xc8c8c8),
    .shade = pal_hex(0x989898), .select = pal_hex(0xc6c6c6),
    .track = pal_hex(0x989898), .scroll = pal_hex(0xb8b8b8),
    .tab = pal_hex(0xffcb00), .tab_edge = pal_hex(0xb38f00),
    .radius = 3, .pill_radius = LV_RADIUS_CIRCLE, .keyline_w = 1,
    .frame_w = 1, .frame_radius = 0, .seam_w = 1, .grad_darken = LV_OPA_20,
};

static const ui_palette_t k_beos_night = {
    .accent = pal_hex(0x6b9bd8), .muted = pal_hex(0x9a9a9a),
    .bg = pal_hex(0x2b2b2b), .fg = pal_hex(0xe8e8e8), .surface = pal_hex(0x3c3c3c),
    .outline = pal_hex(0x111111), .field = pal_hex(0x1e1e1e),
    .titlebar = pal_hex(0xe0b000), .title_fg = pal_hex(0x000000),
    .title_icon = pal_hex(0x1d3f66), .title_muted = pal_hex(0x5c4a00),
    .title_seam = pal_hex(0xb38f00), .primary = pal_hex(0x6b9bd8),
    .frame = pal_hex(0x111111), .btn_face = pal_hex(0x3c3c3c),
    .backdrop = pal_hex(0x1b3350), .on_backdrop = pal_hex(0xffffff),
    .focus = pal_hex(0x7f9cff), .accent_well = pal_hex(0x2a1a00),
    .btn_top = pal_hex(0x4c4c4c), .btn_bottom = pal_hex(0x343434),
    .shade = pal_hex(0x000000), .select = pal_hex(0x4a5466),
    .track = pal_hex(0x222222), .scroll = pal_hex(0x5a5a5a),
    .tab = pal_hex(0xe0b000), .tab_edge = pal_hex(0x8a6e00),
    .radius = 3, .pill_radius = LV_RADIUS_CIRCLE, .keyline_w = 1,
    .frame_w = 1, .frame_radius = 0, .seam_w = 1, .grad_darken = LV_OPA_20,
};

static const ui_palette_t *const k_palettes[4][2] = {
    /* UI_STYLE_CLASSIC   */ {&k_classic,   &k_classic},
    /* UI_STYLE_PIXEL_ISO */ {&k_pixel,     &k_pixel},
    /* UI_STYLE_HOTEL     */ {&k_hotel_day, &k_hotel_night},
    /* UI_STYLE_BEOS      */ {&k_beos_day,  &k_beos_night},
};

static const ui_palette_t *s_pal = nullptr;   // active row, re-selected by theme_init

static const ui_palette_t &palette_for(ui_style_t style)
{
    return *k_palettes[style][ui_dark_mode_get() ? 1 : 0];
}

const ui_palette_t &ui_palette()
{
    if (!s_pal) s_pal = &palette_for(ui_style_get());
    return *s_pal;
}

lv_color_t ui_color_accent()      { return ui_palette().accent; }
lv_color_t ui_color_muted()       { return ui_palette().muted; }
lv_color_t ui_color_bg()          { return ui_palette().bg; }
lv_color_t ui_color_fg()          { return ui_palette().fg; }
lv_color_t ui_color_surface()     { return ui_palette().surface; }
// Keyline drawn round chrome widgets: Hotel's black ring, BeOS's dark frame;
// the flat styles fall back to the muted grey.
lv_color_t ui_color_outline()     { return ui_palette().outline; }
// Page colour of text fields and list rows — white paper by day, a recessed
// well darker than the panel by night; the flat styles use the page bg.
lv_color_t ui_color_field()       { return ui_palette().field; }
lv_color_t ui_color_titlebar()    { return ui_palette().titlebar; }
lv_color_t ui_color_title_fg()    { return ui_palette().title_fg; }
lv_color_t ui_color_title_icon()  { return ui_palette().title_icon; }
lv_color_t ui_color_title_muted() { return ui_palette().title_muted; }
int32_t ui_radius()               { return ui_palette().radius; }

// White in every style: the saturated fills it sits on (red badge, green
// switch, blue accent) are dark enough that the light theme still needs white.
lv_color_t ui_color_on_accent()
{
    return lv_color_white();
}

// --- Styles ---------------------------------------------------------------
// All at file scope so a style switch updates the very objects already
// referencing them (see lv_obj_report_style_change in theme_init).
static lv_style_t s_list_bg;
static lv_style_t s_default_border;
static lv_style_t s_focus;
static lv_style_t s_px_square;
static lv_style_t s_px_block;
static lv_style_t s_px_pressed;
static lv_style_t s_px_row;
static lv_style_t s_px_track;
static lv_style_t s_px_indicator;
static lv_style_t s_px_knob;
static lv_style_t s_px_scrollbar;
static lv_style_t s_ht_btn;
static lv_style_t s_ht_btn_pressed;
static lv_style_t s_ht_row;
static lv_style_t s_ht_field;
static lv_style_t s_ht_track;
static lv_style_t s_ht_indicator;
static lv_style_t s_ht_switch_on;
static lv_style_t s_ht_knob;
static lv_style_t s_ht_scrollbar;
static lv_style_t s_ht_focus_fill;
static lv_style_t s_be_btn;
static lv_style_t s_be_btn_pressed;
static lv_style_t s_be_row;
static lv_style_t s_be_field;
static lv_style_t s_be_track;
static lv_style_t s_be_indicator;
static lv_style_t s_be_knob;
static lv_style_t s_be_scrollbar;
static lv_style_t s_be_select;
static bool s_styles_built = false;

static void build_styles()
{
    const ui_palette_t &p = ui_palette();
    // Every family's styles are rebuilt on each switch; the family-only tokens
    // come from that family's own row (in the current mode), the shared ones
    // from the active row.
    const ui_palette_t &px = palette_for(UI_STYLE_PIXEL_ISO);
    const ui_palette_t &ht = palette_for(UI_STYLE_HOTEL);
    const ui_palette_t &be = palette_for(UI_STYLE_BEOS);

    // Reset first when re-building so props dropped on a switch (e.g. classic
    // dropping the iso shadow) don't linger from the previous palette.
    if (s_styles_built) {
        lv_style_reset(&s_list_bg);
        lv_style_reset(&s_default_border);
        lv_style_reset(&s_focus);
        lv_style_reset(&s_px_square);
        lv_style_reset(&s_px_block);
        lv_style_reset(&s_px_pressed);
        lv_style_reset(&s_px_row);
        lv_style_reset(&s_px_track);
        lv_style_reset(&s_px_indicator);
        lv_style_reset(&s_px_knob);
        lv_style_reset(&s_px_scrollbar);
        lv_style_reset(&s_ht_btn);
        lv_style_reset(&s_ht_btn_pressed);
        lv_style_reset(&s_ht_row);
        lv_style_reset(&s_ht_field);
        lv_style_reset(&s_ht_track);
        lv_style_reset(&s_ht_indicator);
        lv_style_reset(&s_ht_switch_on);
        lv_style_reset(&s_ht_knob);
        lv_style_reset(&s_ht_scrollbar);
        lv_style_reset(&s_ht_focus_fill);
        lv_style_reset(&s_be_btn);
        lv_style_reset(&s_be_btn_pressed);
        lv_style_reset(&s_be_row);
        lv_style_reset(&s_be_field);
        lv_style_reset(&s_be_track);
        lv_style_reset(&s_be_indicator);
        lv_style_reset(&s_be_knob);
        lv_style_reset(&s_be_scrollbar);
        lv_style_reset(&s_be_select);
    }

    lv_style_init(&s_list_bg);
    lv_style_set_bg_color(&s_list_bg, p.bg);
    lv_style_set_text_color(&s_list_bg, p.fg);
    lv_style_set_bg_opa(&s_list_bg, LV_OPA_COVER);
    lv_style_set_border_width(&s_list_bg, 0);
    if (ui_style_has_chrome()) {
        // White (night: recessed) dropdown/list panel ringed by the chrome
        // keyline: Hotel's 2px black ring, BeOS's hard square 1px frame.
        lv_style_set_bg_color(&s_list_bg, p.field);
        lv_style_set_border_width(&s_list_bg, p.keyline_w);
        lv_style_set_border_color(&s_list_bg, p.outline);
        lv_style_set_border_opa(&s_list_bg, LV_OPA_COVER);
        lv_style_set_radius(&s_list_bg, p.frame_radius);
    }

    // Reserve border space in the default state (transparent) so that
    // toggling focus only swaps the border color — the layout size is stable.
    lv_style_init(&s_default_border);
    lv_style_set_border_width(&s_default_border, UI_BORDER_W);
    lv_style_set_border_opa(&s_default_border, LV_OPA_TRANSP);

    lv_style_init(&s_focus);
    lv_style_set_border_width(&s_focus, UI_BORDER_W);
    lv_style_set_border_color(&s_focus, p.focus);
    lv_style_set_border_opa(&s_focus, LV_OPA_COVER);
    lv_style_set_outline_width(&s_focus, 0);
    if (ui_style_is_pixel()) {
        lv_style_set_border_side(&s_focus, LV_BORDER_SIDE_FULL);
        lv_style_set_shadow_color(&s_focus,
                                  lv_color_darken(p.accent, LV_OPA_50));
        lv_style_set_translate_x(&s_focus, -1);
        lv_style_set_translate_y(&s_focus, -1);
        lv_style_set_shadow_offset_x(&s_focus, UI_ISO_DEPTH_X + 1);
        lv_style_set_shadow_offset_y(&s_focus, UI_ISO_DEPTH_Y + 1);
    } else if (ui_style_has_chrome()) {
        lv_style_set_border_side(&s_focus, LV_BORDER_SIDE_FULL);
    }

    // Hotel selection fill — only for buttons and list rows; text fields keep
    // their white page and just take the blue keyline.
    lv_style_init(&s_ht_focus_fill);
    lv_style_set_bg_color(&s_ht_focus_fill, ht.select);
    lv_style_set_bg_opa(&s_ht_focus_fill, LV_OPA_COVER);
    lv_style_set_bg_grad_dir(&s_ht_focus_fill, LV_GRAD_DIR_NONE);

    lv_style_init(&s_px_square);
    lv_style_set_radius(&s_px_square, 0);

    lv_style_init(&s_px_block);
    lv_style_set_bg_color(&s_px_block, p.surface);
    lv_style_set_bg_opa(&s_px_block, LV_OPA_COVER);
    lv_style_set_border_width(&s_px_block, 2);
    lv_style_set_border_side(&s_px_block,
                             (lv_border_side_t)(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT));
    lv_style_set_border_color(&s_px_block, px.highlight);
    lv_style_set_border_opa(&s_px_block, LV_OPA_COVER);
    lv_style_set_shadow_width(&s_px_block, 1);
    lv_style_set_shadow_spread(&s_px_block, 0);
    lv_style_set_shadow_offset_x(&s_px_block, UI_ISO_DEPTH_X);
    lv_style_set_shadow_offset_y(&s_px_block, UI_ISO_DEPTH_Y);
    lv_style_set_shadow_color(&s_px_block, px.depth);
    lv_style_set_shadow_opa(&s_px_block, LV_OPA_COVER);

    lv_style_init(&s_px_pressed);
    lv_style_set_translate_x(&s_px_pressed, UI_ISO_DEPTH_X);
    lv_style_set_translate_y(&s_px_pressed, UI_ISO_DEPTH_Y);
    lv_style_set_shadow_offset_x(&s_px_pressed, 0);
    lv_style_set_shadow_offset_y(&s_px_pressed, 0);
    lv_style_set_shadow_opa(&s_px_pressed, LV_OPA_TRANSP);
    lv_style_set_bg_color(&s_px_pressed,
                          lv_color_darken(p.surface, LV_OPA_20));

    lv_style_init(&s_px_row);
    lv_style_set_border_width(&s_px_row, 2);
    lv_style_set_border_side(&s_px_row, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&s_px_row, px.grid);
    lv_style_set_border_opa(&s_px_row, LV_OPA_COVER);
    lv_style_set_radius(&s_px_row, 0);

    lv_style_init(&s_px_track);
    lv_style_set_bg_color(&s_px_track, px.grid);
    lv_style_set_bg_opa(&s_px_track, LV_OPA_COVER);
    lv_style_set_radius(&s_px_track, 0);

    lv_style_init(&s_px_indicator);
    lv_style_set_bg_color(&s_px_indicator, p.accent);
    lv_style_set_bg_opa(&s_px_indicator, LV_OPA_COVER);
    lv_style_set_radius(&s_px_indicator, 0);

    lv_style_init(&s_px_knob);
    lv_style_set_radius(&s_px_knob, 0);
    lv_style_set_bg_color(&s_px_knob, p.fg);
    lv_style_set_shadow_width(&s_px_knob, 1);
    lv_style_set_shadow_spread(&s_px_knob, 0);
    lv_style_set_shadow_offset_x(&s_px_knob, 2);
    lv_style_set_shadow_offset_y(&s_px_knob, 1);
    lv_style_set_shadow_color(&s_px_knob, px.depth);
    lv_style_set_shadow_opa(&s_px_knob, LV_OPA_COVER);

    lv_style_init(&s_px_scrollbar);
    lv_style_set_radius(&s_px_scrollbar, 0);
    lv_style_set_bg_color(&s_px_scrollbar, p.accent);
    lv_style_set_bg_opa(&s_px_scrollbar, LV_OPA_COVER);
    lv_style_set_width(&s_px_scrollbar, 4);

    // Hotel — Habbo-style chunky chrome: white faces ringed in black, a soft
    // top-lit gradient on interactive widgets, and a hard drop shadow.
    lv_style_init(&s_ht_btn);
    lv_style_set_bg_color(&s_ht_btn, p.surface);
    lv_style_set_bg_grad_color(&s_ht_btn, ht.btn_bottom);
    lv_style_set_bg_grad_dir(&s_ht_btn, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_ht_btn, LV_OPA_COVER);
    lv_style_set_border_width(&s_ht_btn, 2);
    lv_style_set_border_color(&s_ht_btn, p.outline);
    lv_style_set_border_opa(&s_ht_btn, LV_OPA_COVER);
    lv_style_set_border_side(&s_ht_btn, LV_BORDER_SIDE_FULL);
    lv_style_set_radius(&s_ht_btn, 3);
    lv_style_set_text_color(&s_ht_btn, p.fg);
    lv_style_set_shadow_width(&s_ht_btn, 1);
    lv_style_set_shadow_spread(&s_ht_btn, 0);
    lv_style_set_shadow_offset_x(&s_ht_btn, 0);
    lv_style_set_shadow_offset_y(&s_ht_btn, 2);
    lv_style_set_shadow_color(&s_ht_btn, lv_color_black());
    lv_style_set_shadow_opa(&s_ht_btn, LV_OPA_60);

    // Pressed: gradient flipped and nudged down so it sinks into the drop.
    lv_style_init(&s_ht_btn_pressed);
    lv_style_set_bg_color(&s_ht_btn_pressed, ht.btn_bottom);
    lv_style_set_bg_grad_color(&s_ht_btn_pressed, p.surface);
    lv_style_set_translate_y(&s_ht_btn_pressed, 2);
    lv_style_set_shadow_offset_y(&s_ht_btn_pressed, 0);
    lv_style_set_shadow_opa(&s_ht_btn_pressed, LV_OPA_TRANSP);

    lv_style_init(&s_ht_row);
    lv_style_set_bg_color(&s_ht_row, p.field);
    lv_style_set_bg_opa(&s_ht_row, LV_OPA_COVER);
    lv_style_set_border_width(&s_ht_row, 1);
    lv_style_set_border_side(&s_ht_row, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&s_ht_row, ht.divider);
    lv_style_set_border_opa(&s_ht_row, LV_OPA_COVER);
    lv_style_set_radius(&s_ht_row, 0);
    lv_style_set_text_color(&s_ht_row, p.fg);

    lv_style_init(&s_ht_field);
    lv_style_set_bg_color(&s_ht_field, p.field);
    lv_style_set_bg_opa(&s_ht_field, LV_OPA_COVER);
    lv_style_set_border_width(&s_ht_field, 2);
    lv_style_set_border_color(&s_ht_field, p.outline);
    lv_style_set_border_opa(&s_ht_field, LV_OPA_COVER);
    lv_style_set_radius(&s_ht_field, 3);
    lv_style_set_text_color(&s_ht_field, p.fg);

    lv_style_init(&s_ht_track);
    // Recessed grey groove: stays visible even where a caller zeroes the border.
    lv_style_set_bg_color(&s_ht_track, ht.btn_bottom);
    lv_style_set_bg_opa(&s_ht_track, LV_OPA_COVER);
    lv_style_set_border_width(&s_ht_track, 2);
    lv_style_set_border_color(&s_ht_track, p.outline);
    lv_style_set_border_opa(&s_ht_track, LV_OPA_COVER);
    lv_style_set_radius(&s_ht_track, 3);

    lv_style_init(&s_ht_indicator);
    lv_style_set_bg_color(&s_ht_indicator, p.accent);
    lv_style_set_bg_opa(&s_ht_indicator, LV_OPA_COVER);
    lv_style_set_radius(&s_ht_indicator, 2);

    lv_style_init(&s_ht_switch_on);
    lv_style_set_bg_color(&s_ht_switch_on, ht.switch_on);
    lv_style_set_bg_opa(&s_ht_switch_on, LV_OPA_COVER);
    lv_style_set_radius(&s_ht_switch_on, 2);

    lv_style_init(&s_ht_knob);
    lv_style_set_bg_color(&s_ht_knob, p.surface);
    lv_style_set_bg_grad_color(&s_ht_knob, ht.btn_bottom);
    lv_style_set_bg_grad_dir(&s_ht_knob, LV_GRAD_DIR_VER);
    lv_style_set_border_width(&s_ht_knob, 2);
    lv_style_set_border_color(&s_ht_knob, p.outline);
    lv_style_set_border_opa(&s_ht_knob, LV_OPA_COVER);
    lv_style_set_radius(&s_ht_knob, 3);

    lv_style_init(&s_ht_scrollbar);
    lv_style_set_radius(&s_ht_scrollbar, 2);
    lv_style_set_bg_color(&s_ht_scrollbar, ht.scroll);
    lv_style_set_bg_opa(&s_ht_scrollbar, LV_OPA_COVER);
    lv_style_set_width(&s_ht_scrollbar, 6);

    // BeOS — R5 chrome: grey bevelled buttons lit from the top with a dark
    // bottom-right shade, hard 1px frames, flat white fields and lists.
    lv_style_init(&s_be_btn);
    lv_style_set_bg_color(&s_be_btn, be.btn_top);
    lv_style_set_bg_grad_color(&s_be_btn, be.btn_bottom);
    lv_style_set_bg_grad_dir(&s_be_btn, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_be_btn, LV_OPA_COVER);
    lv_style_set_border_width(&s_be_btn, 1);
    lv_style_set_border_side(&s_be_btn, LV_BORDER_SIDE_FULL);
    lv_style_set_border_color(&s_be_btn, p.outline);
    lv_style_set_border_opa(&s_be_btn, LV_OPA_COVER);
    lv_style_set_radius(&s_be_btn, 3);
    lv_style_set_text_color(&s_be_btn, p.fg);
    lv_style_set_shadow_width(&s_be_btn, 1);
    lv_style_set_shadow_spread(&s_be_btn, 0);
    lv_style_set_shadow_offset_x(&s_be_btn, 1);
    lv_style_set_shadow_offset_y(&s_be_btn, 1);
    lv_style_set_shadow_color(&s_be_btn, be.shade);
    lv_style_set_shadow_opa(&s_be_btn, LV_OPA_COVER);

    // Pressed: gradient flipped and nudged into the shade.
    lv_style_init(&s_be_btn_pressed);
    lv_style_set_bg_color(&s_be_btn_pressed, be.btn_bottom);
    lv_style_set_bg_grad_color(&s_be_btn_pressed, be.btn_top);
    lv_style_set_translate_x(&s_be_btn_pressed, 1);
    lv_style_set_translate_y(&s_be_btn_pressed, 1);
    lv_style_set_shadow_opa(&s_be_btn_pressed, LV_OPA_TRANSP);

    lv_style_init(&s_be_row);
    lv_style_set_bg_color(&s_be_row, p.field);
    lv_style_set_bg_opa(&s_be_row, LV_OPA_COVER);
    lv_style_set_border_width(&s_be_row, 1);
    lv_style_set_border_side(&s_be_row, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&s_be_row, be.btn_bottom);
    lv_style_set_border_opa(&s_be_row, LV_OPA_COVER);
    lv_style_set_radius(&s_be_row, 0);
    lv_style_set_text_color(&s_be_row, p.fg);

    lv_style_init(&s_be_field);
    lv_style_set_bg_color(&s_be_field, p.field);
    lv_style_set_bg_opa(&s_be_field, LV_OPA_COVER);
    lv_style_set_border_width(&s_be_field, 1);
    lv_style_set_border_color(&s_be_field, p.outline);
    lv_style_set_border_opa(&s_be_field, LV_OPA_COVER);
    lv_style_set_border_side(&s_be_field, LV_BORDER_SIDE_FULL);
    lv_style_set_radius(&s_be_field, 0);
    lv_style_set_text_color(&s_be_field, p.fg);

    lv_style_init(&s_be_track);
    lv_style_set_bg_color(&s_be_track, be.track);
    lv_style_set_bg_opa(&s_be_track, LV_OPA_COVER);
    lv_style_set_radius(&s_be_track, 2);
    lv_style_set_border_width(&s_be_track, 1);
    lv_style_set_border_color(&s_be_track, p.outline);
    lv_style_set_border_opa(&s_be_track, LV_OPA_COVER);

    lv_style_init(&s_be_indicator);
    lv_style_set_bg_color(&s_be_indicator, p.accent);
    lv_style_set_bg_opa(&s_be_indicator, LV_OPA_COVER);
    lv_style_set_radius(&s_be_indicator, 2);

    lv_style_init(&s_be_knob);
    lv_style_set_bg_color(&s_be_knob, be.btn_top);
    lv_style_set_bg_grad_color(&s_be_knob, be.btn_bottom);
    lv_style_set_bg_grad_dir(&s_be_knob, LV_GRAD_DIR_VER);
    lv_style_set_border_width(&s_be_knob, 1);
    lv_style_set_border_color(&s_be_knob, p.outline);
    lv_style_set_border_opa(&s_be_knob, LV_OPA_COVER);
    lv_style_set_radius(&s_be_knob, 2);

    lv_style_init(&s_be_scrollbar);
    lv_style_set_radius(&s_be_scrollbar, 0);
    lv_style_set_bg_color(&s_be_scrollbar, be.scroll);
    lv_style_set_bg_opa(&s_be_scrollbar, LV_OPA_COVER);
    lv_style_set_border_width(&s_be_scrollbar, 1);
    lv_style_set_border_color(&s_be_scrollbar, p.outline);
    lv_style_set_border_opa(&s_be_scrollbar, LV_OPA_COVER);
    lv_style_set_width(&s_be_scrollbar, 8);

    // Selection fill — only for buttons, dropdowns and list rows; text fields
    // keep their white page and just take the blue navigation keyline.
    lv_style_init(&s_be_select);
    lv_style_set_bg_color(&s_be_select, be.select);
    lv_style_set_bg_opa(&s_be_select, LV_OPA_COVER);
    lv_style_set_bg_grad_dir(&s_be_select, LV_GRAD_DIR_NONE);

    s_styles_built = true;
}

// The focusable widget classes that get border/focus treatment across styles.
static bool is_focusable_class(lv_obj_t *obj)
{
    return lv_obj_check_type(obj, &lv_button_class) ||
           lv_obj_check_type(obj, &lv_list_button_class) ||
           lv_obj_check_type(obj, &lv_menu_cont_class) ||
           lv_obj_check_type(obj, &lv_slider_class) ||
           lv_obj_check_type(obj, &lv_switch_class) ||
           lv_obj_check_type(obj, &lv_spinbox_class) ||
           lv_obj_check_type(obj, &lv_textarea_class) ||
           lv_obj_check_type(obj, &lv_dropdown_class) ||
           lv_obj_check_type(obj, &lv_checkbox_class);
}

// Per-style style sets the apply callback attaches; NULL where a style leaves
// that widget part to the base theme. Indexed by ui_style_t, cached in
// theme_init.
struct theme_styles_t {
    lv_style_t *base;         // every object's MAIN part
    lv_style_t *scrollbar;
    lv_style_t *btn;          // buttons and closed dropdowns
    lv_style_t *btn_pressed;
    lv_style_t *row;          // menu rows and list buttons
    lv_style_t *field;        // text areas and spinboxes
    lv_style_t *track;        // slider / bar / switch groove
    lv_style_t *indicator;    // slider / bar fill
    lv_style_t *switch_on;    // checked switch fill
    lv_style_t *knob;
    lv_style_t *select;       // focus fill on buttons and rows
    lv_style_t *list_select;  // current option of an open dropdown
};

static const theme_styles_t k_theme_styles[4] = {
    /* UI_STYLE_CLASSIC: reserve borders, accent on focus, nothing else. */
    {},
    /* UI_STYLE_PIXEL_ISO: squared everything, extruded blocks. */
    {&s_px_square, &s_px_scrollbar, &s_px_block, &s_px_pressed, &s_px_row, nullptr,
     &s_px_track, &s_px_indicator, &s_px_indicator, &s_px_knob, nullptr, nullptr},
    /* UI_STYLE_HOTEL: black-ringed faces, blue selection fill. */
    {nullptr, &s_ht_scrollbar, &s_ht_btn, &s_ht_btn_pressed, &s_ht_row, &s_ht_field,
     &s_ht_track, &s_ht_indicator, &s_ht_switch_on, &s_ht_knob, &s_ht_focus_fill, nullptr},
    /* UI_STYLE_BEOS: bevelled grey buttons, flat white fields, grey selection
       bar, which also marks the current option of an open dropdown. */
    {nullptr, &s_be_scrollbar, &s_be_btn, &s_be_btn_pressed, &s_be_row, &s_be_field,
     &s_be_track, &s_be_indicator, &s_be_indicator, &s_be_knob, &s_be_select, &s_be_select},
};
static const theme_styles_t *s_theme_styles = &k_theme_styles[UI_STYLE_CLASSIC];

/*Will be called when the styles of the base theme are already added
  to add new styles*/
static void new_theme_apply_cb(lv_theme_t * th, lv_obj_t * obj)
{
    (void)th;
    const theme_styles_t &st = *s_theme_styles;
    const bool focusable = is_focusable_class(obj);
    const bool button = lv_obj_check_type(obj, &lv_button_class) ||
                        lv_obj_check_type(obj, &lv_dropdown_class);
    const bool row = lv_obj_check_type(obj, &lv_menu_cont_class) ||
                     lv_obj_check_type(obj, &lv_list_button_class);
    const bool slider = lv_obj_check_type(obj, &lv_slider_class);

    if (st.base) lv_obj_add_style(obj, st.base, LV_PART_MAIN);
    if (st.scrollbar) lv_obj_add_style(obj, st.scrollbar, LV_PART_SCROLLBAR);

    // The transparent default border goes in before the row divider so the
    // divider shows at rest; focus still wins because it is state-specific.
    if (focusable) lv_obj_add_style(obj, &s_default_border, LV_PART_MAIN);

    if (button && st.btn) {
        lv_obj_add_style(obj, st.btn, LV_PART_MAIN);
        lv_obj_add_style(obj, st.btn_pressed, LV_STATE_PRESSED);
    }

    if (row && st.row) lv_obj_add_style(obj, st.row, LV_PART_MAIN);

    if (st.field && (lv_obj_check_type(obj, &lv_textarea_class) ||
                     lv_obj_check_type(obj, &lv_spinbox_class))) {
        lv_obj_add_style(obj, st.field, LV_PART_MAIN);
    }

    if (st.track && (slider || lv_obj_check_type(obj, &lv_bar_class))) {
        lv_obj_add_style(obj, st.track, LV_PART_MAIN);
        lv_obj_add_style(obj, st.indicator, LV_PART_INDICATOR);
        if (slider) lv_obj_add_style(obj, st.knob, LV_PART_KNOB);
    }

    if (st.track && lv_obj_check_type(obj, &lv_switch_class)) {
        lv_obj_add_style(obj, st.track, LV_PART_MAIN);
        lv_obj_add_style(obj, st.switch_on,
                         (lv_style_selector_t)(LV_PART_INDICATOR | LV_STATE_CHECKED));
        lv_obj_add_style(obj, st.knob, LV_PART_KNOB);
    }

    if (focusable) {
        lv_obj_add_style(obj, &s_focus, LV_STATE_FOCUSED);
        lv_obj_add_style(obj, &s_focus, LV_STATE_FOCUS_KEY);
    }

    // Selection fill only on buttons and rows; text fields keep their page
    // and just take the focus keyline.
    if ((button || row) && st.select) {
        lv_obj_add_style(obj, st.select, LV_STATE_FOCUSED);
        lv_obj_add_style(obj, st.select, LV_STATE_FOCUS_KEY);
    }

    if (lv_obj_check_type(obj, &lv_list_class) ||
        lv_obj_check_type(obj, &lv_dropdownlist_class)) {
        lv_obj_add_style(obj, &s_list_bg, LV_PART_MAIN);
    }
    if (st.list_select && lv_obj_check_type(obj, &lv_dropdownlist_class)) {
        lv_obj_add_style(obj, st.list_select,
                         (lv_style_selector_t)(LV_PART_SELECTED | LV_STATE_CHECKED));
        lv_obj_add_style(obj, st.list_select,
                         (lv_style_selector_t)(LV_PART_SELECTED | LV_STATE_PRESSED));
    }
}

void theme_init()
{
    lv_display_t * disp = lv_display_get_default();
    const lv_font_t *font = get_system_font();
    if (!font) font = MAIN_FONT;
    bool had_styles = s_styles_built;
    s_pal = &palette_for(ui_style_get());
    s_theme_styles = &k_theme_styles[ui_style_get()];
    // Classic uses a black primary; Pixel Iso, Hotel and BeOS take the accent.
    lv_theme_t * th = lv_theme_default_init(disp, s_pal->primary,
                                            lv_palette_main(LV_PALETTE_GREY),
                                            ui_style_is_dark(),
                                            font);
    build_styles();
    lv_theme_set_apply_cb(th, new_theme_apply_cb);
    lv_display_set_theme(disp, th);
    // Styles existed before this call (a live style switch): ask every object
    // to re-apply them so the new palette takes effect without a rebuild.
    if (had_styles) lv_obj_report_style_change(NULL);
}

// --- Raised blocks + backdrop ---------------------------------------------

// Generic raised-block helper. Classic is a no-op (flat chrome); Pixel Iso
// extrudes an iso tile; Hotel draws the black-outlined, top-lit Habbo button;
// BeOS draws the grey bevelled R5 button.
void ui_block(lv_obj_t *obj, lv_color_t face)
{
    if (ui_style_is_pixel()) {
        // The block reads as a raised iso tile — lit top/left edge plus a hard
        // 2:1 extrusion; pressing pushes it into the ground, focus lifts it.
        lv_obj_set_style_radius(obj, 0, 0);
        lv_obj_set_style_bg_color(obj, face, 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(obj, 2, 0);
        lv_obj_set_style_border_side(obj,
                                     (lv_border_side_t)(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT), 0);
        lv_obj_set_style_border_color(obj, lv_color_lighten(face, LV_OPA_30), 0);
        lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_outline_width(obj, 0, 0);
        lv_obj_set_style_shadow_width(obj, 1, 0);
        lv_obj_set_style_shadow_spread(obj, 0, 0);
        lv_obj_set_style_shadow_offset_x(obj, UI_ISO_DEPTH_X, 0);
        lv_obj_set_style_shadow_offset_y(obj, UI_ISO_DEPTH_Y, 0);
        lv_obj_set_style_shadow_color(obj, ui_palette().depth, 0);
        lv_obj_set_style_shadow_opa(obj, LV_OPA_COVER, 0);

        lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_FULL, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(obj, ui_color_accent(), LV_STATE_FOCUSED);
        lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(obj, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUSED);
        lv_obj_set_style_translate_x(obj, -1, LV_STATE_FOCUSED);
        lv_obj_set_style_translate_y(obj, -1, LV_STATE_FOCUSED);
        lv_obj_set_style_shadow_offset_x(obj, UI_ISO_DEPTH_X + 1, LV_STATE_FOCUSED);
        lv_obj_set_style_shadow_offset_y(obj, UI_ISO_DEPTH_Y + 1, LV_STATE_FOCUSED);
        lv_obj_set_style_shadow_color(obj,
                                      lv_color_darken(ui_color_accent(), LV_OPA_50),
                                      LV_STATE_FOCUSED);

        lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_FULL, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_color(obj, ui_color_accent(), LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_width(obj, 2, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_translate_x(obj, -1, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_translate_y(obj, -1, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_shadow_offset_x(obj, UI_ISO_DEPTH_X + 1, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_shadow_offset_y(obj, UI_ISO_DEPTH_Y + 1, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_shadow_color(obj,
                                      lv_color_darken(ui_color_accent(), LV_OPA_50),
                                      LV_STATE_FOCUS_KEY);

        lv_obj_set_style_translate_x(obj, UI_ISO_DEPTH_X, LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(obj, UI_ISO_DEPTH_Y, LV_STATE_PRESSED);
        lv_obj_set_style_shadow_offset_x(obj, 0, LV_STATE_PRESSED);
        lv_obj_set_style_shadow_offset_y(obj, 0, LV_STATE_PRESSED);
        lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(obj, lv_color_darken(face, LV_OPA_20), LV_STATE_PRESSED);
        return;
    }

    if (ui_style_is_beos()) {
        // BeOS bevel button — 1px frame, top-lit gradient, dark bottom-right
        // shade; pressing sinks it and flips the gradient, focus draws the blue
        // navigation keyline.
        lv_obj_set_style_radius(obj, 3, 0);
        lv_obj_set_style_bg_color(obj, face, 0);
        lv_obj_set_style_bg_grad_color(obj, lv_color_darken(face, LV_OPA_20), 0);
        lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_style_border_color(obj, ui_palette().outline, 0);
        lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_outline_width(obj, 0, 0);
        lv_obj_set_style_shadow_width(obj, 1, 0);
        lv_obj_set_style_shadow_spread(obj, 0, 0);
        lv_obj_set_style_shadow_offset_x(obj, 2, 0);
        lv_obj_set_style_shadow_offset_y(obj, 2, 0);
        lv_obj_set_style_shadow_color(obj, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(obj, LV_OPA_50, 0);

        lv_obj_set_style_border_width(obj, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(obj, ui_palette().focus, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUSED);

        lv_obj_set_style_border_width(obj, 2, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_color(obj, ui_palette().focus, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUS_KEY);

        lv_obj_set_style_translate_x(obj, 1, LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(obj, 1, LV_STATE_PRESSED);
        lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(obj, lv_color_darken(face, LV_OPA_20), LV_STATE_PRESSED);
        lv_obj_set_style_bg_grad_color(obj, face, LV_STATE_PRESSED);
        return;
    }

    if (!ui_style_is_hotel()) return;   // Classic — flat chrome, nothing to add.

    // Habbo-style chunky button — black outline, soft top-lit gradient, hard
    // drop under it.
    lv_obj_set_style_radius(obj, 3, 0);
    lv_obj_set_style_bg_color(obj, face, 0);
    lv_obj_set_style_bg_grad_color(obj, lv_color_darken(face, LV_OPA_10), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_border_color(obj, ui_palette().outline, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 1, 0);
    lv_obj_set_style_shadow_spread(obj, 0, 0);
    lv_obj_set_style_shadow_offset_x(obj, 0, 0);
    lv_obj_set_style_shadow_offset_y(obj, 3, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_60, 0);

    lv_obj_set_style_border_color(obj, ui_color_accent(), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(obj, ui_color_field(), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_grad_color(obj, ui_palette().select, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_translate_y(obj, -1, LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_offset_y(obj, 4, LV_STATE_FOCUSED);

    lv_obj_set_style_border_color(obj, ui_color_accent(), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(obj, ui_color_field(), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_grad_color(obj, ui_palette().select, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_translate_y(obj, -1, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_shadow_offset_y(obj, 4, LV_STATE_FOCUS_KEY);

    lv_obj_set_style_translate_y(obj, 2, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(obj, lv_color_darken(face, LV_OPA_20), LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_color(obj, face, LV_STATE_PRESSED);
}

static uint16_t s_iso_grid_buf[32 * 16];
static lv_image_dsc_t s_iso_grid;
static bool s_iso_grid_built = false;

static void build_iso_grid()
{
    if (s_iso_grid_built) return;
    // 2-across-1-down lines tile into a diamond lattice at 32x16.
    for (int i = 0; i < 32 * 16; i++) {
        s_iso_grid_buf[i] = lv_color_to_u16(ui_color_bg());
    }
    for (int x = 0; x < 32; x++) {
        s_iso_grid_buf[(x / 2) * 32 + x] = lv_color_to_u16(ui_palette().grid);
        s_iso_grid_buf[(15 - x / 2) * 32 + x] = lv_color_to_u16(ui_palette().grid);
    }
    // s_iso_grid is a zero-initialised static; the fields set below define it.
    s_iso_grid.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_iso_grid.header.cf = LV_COLOR_FORMAT_RGB565;
    s_iso_grid.header.w = 32;
    s_iso_grid.header.h = 16;
    s_iso_grid.header.stride = 32 * 2;
    s_iso_grid.data_size = sizeof(s_iso_grid_buf);
    s_iso_grid.data = (const uint8_t *)s_iso_grid_buf;
    s_iso_grid_built = true;
}

// Hotel room floor — same 32x16 diamond lattice as the iso grid, but filled
// with the Habbo olive and its darker grout line, built lazily on first use
// and rebuilt when dark mode flips the floor between day and night.
static uint16_t s_floor_buf[32 * 16];
static lv_image_dsc_t s_floor;
static int8_t s_floor_dark = -1;   // dark-mode state the buffer was built for

static void build_floor_grid()
{
    int8_t dark = ui_dark_mode_get() ? 1 : 0;
    if (s_floor_dark == dark) return;
    for (int i = 0; i < 32 * 16; i++) {
        s_floor_buf[i] = lv_color_to_u16(ui_palette().floor);
    }
    for (int x = 0; x < 32; x++) {
        s_floor_buf[(x / 2) * 32 + x] = lv_color_to_u16(ui_palette().floor_line);
        s_floor_buf[(15 - x / 2) * 32 + x] = lv_color_to_u16(ui_palette().floor_line);
    }
    s_floor.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_floor.header.cf = LV_COLOR_FORMAT_RGB565;
    s_floor.header.w = 32;
    s_floor.header.h = 16;
    s_floor.header.stride = 32 * 2;
    s_floor.data_size = sizeof(s_floor_buf);
    s_floor.data = (const uint8_t *)s_floor_buf;
    // A rebuild rewrites pixels behind an already-used descriptor.
    if (s_floor_dark >= 0) lv_image_cache_drop(&s_floor);
    s_floor_dark = dark;
}

void ui_apply_backdrop(lv_obj_t *obj)
{
    // BeOS paints the solid desktop blue; every other style its page bg, which
    // Pixel Iso and Hotel tile a pattern over.
    lv_obj_set_style_bg_color(obj, ui_palette().backdrop, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    if (ui_style_is_pixel()) {
        build_iso_grid();
        lv_obj_set_style_bg_image_src(obj, &s_iso_grid, 0);
        lv_obj_set_style_bg_image_tiled(obj, true, 0);
    } else if (ui_style_is_hotel()) {
        build_floor_grid();
        lv_obj_set_style_bg_image_src(obj, &s_floor, 0);
        lv_obj_set_style_bg_image_tiled(obj, true, 0);
    } else {
        lv_obj_set_style_bg_image_src(obj, NULL, 0);
    }
}

// BeOS window tab — the yellow R5 title tab, keylined along its top, left and
// right edges so the bottom merges into the window below it.
void ui_beos_tab(lv_obj_t *label)
{
    if (!ui_style_is_beos()) return;
    lv_obj_set_style_bg_color(label, ui_palette().tab, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(label, ui_palette().title_fg, 0);
    lv_obj_set_style_pad_hor(label, 10, 0);
    lv_obj_set_style_pad_ver(label, 3, 0);
    lv_obj_set_style_radius(label, 3, 0);
    lv_obj_set_style_border_width(label, 1, 0);
    lv_obj_set_style_border_color(label, ui_palette().tab_edge, 0);
    lv_obj_set_style_border_side(label,
                                 (lv_border_side_t)(LV_BORDER_SIDE_TOP |
                                                    LV_BORDER_SIDE_LEFT |
                                                    LV_BORDER_SIDE_RIGHT), 0);
    lv_obj_set_style_border_opa(label, LV_OPA_COVER, 0);
    // A BeOS tab hugs its title instead of spanning the window — the msgbox
    // header otherwise flex-grows its title label to the full width.
    lv_obj_set_flex_grow(label, 0);
    lv_obj_set_width(label, LV_SIZE_CONTENT);
}

ui_title_chrome_t ui_dress_title(lv_obj_t *bar, lv_obj_t *label, int32_t pad_hor)
{
    if (ui_style_is_hotel()) {
        if (bar) {
            lv_obj_set_style_bg_color(bar, ui_color_titlebar(), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(bar, ui_color_title_fg(), 0);
            lv_obj_set_style_pad_hor(bar, pad_hor, 0);
            lv_obj_set_style_pad_ver(bar, 4, 0);
            lv_obj_set_style_border_width(bar, 0, 0);
        }
        return UI_TITLE_BAR;
    }
    if (ui_style_is_beos()) {
        if (label) ui_beos_tab(label);
        return UI_TITLE_TAB;
    }
    return UI_TITLE_NONE;
}
