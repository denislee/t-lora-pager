/**
 * @file      ui/theme.h
 * @brief     Runtime theme tokens + four selectable UI styles.
 *
 * Classic (flat, rounded, orange-on-black) is the default; Pixel Iso is a
 * near-isometric pixel-art look (squared tiles, hard 2:1 extrusion shadows,
 * Sweetie-16 palette); Hotel is a Habbo-Hotel-inspired light theme (light grey
 * panels, dark text, black outlines, blue title bars); BeOS is a BeOS R5 look
 * (grey bevelled chrome, yellow window tabs, blue keyboard-navigation focus).
 * Dark mode is orthogonal to the style: Hotel and BeOS swap to night palettes,
 * Classic and Pixel Iso are dark by design and stay as they are.
 * Use the token macros in every place that picks a color instead of
 * hard-coding — a style swap is a single-header change.
 */
#pragma once

#include <lvgl.h>

#define UI_BORDER_W          2

typedef enum { UI_STYLE_CLASSIC = 0, UI_STYLE_PIXEL_ISO = 1, UI_STYLE_HOTEL = 2, UI_STYLE_BEOS = 3 } ui_style_t;
ui_style_t ui_style_get();            // lazily loaded from NVS on first call
void ui_style_set(ui_style_t style);  // persist + rebuild theme (calls theme_init)
bool ui_style_is_pixel();             // true only for Pixel Iso
bool ui_style_has_chrome();           // Hotel or BeOS (outlined / bevelled widgets), day or night
bool ui_dark_mode_get();              // dark-mode preference, lazily loaded from NVS
void ui_dark_mode_set(bool dark);     // persist + rebuild theme (calls theme_init)
/* Every colour token and per-style metric of one style in one mode. The table
 * lives in ui_theme.cpp, indexed [style][dark]; ui_palette() returns the active
 * row. Chrome tokens a style has no widget for are left zero. */
typedef struct {
    // Shared tokens: every row sets these (the ui_color_* getters read them).
    lv_color_t accent, muted, bg, fg, surface, outline, field;
    lv_color_t titlebar, title_fg, title_icon, title_muted;
    lv_color_t title_seam;   // keyline under the status bar (see seam_w)
    lv_color_t primary;      // LVGL default-theme primary colour
    lv_color_t frame;        // keyline round modal boxes
    lv_color_t btn_face;     // face of modal footer buttons
    lv_color_t backdrop;     // screen fill under the backdrop pattern
    lv_color_t on_backdrop;  // secondary text drawn straight on the backdrop
    lv_color_t focus;        // keyboard-focus keyline
    lv_color_t accent_well;  // dark face behind a focused accent keyline
    // Chrome tokens (Hotel / BeOS / Pixel Iso widget parts).
    lv_color_t btn_top, btn_bottom, shade, select, track, scroll, divider, switch_on;
    lv_color_t tab, tab_edge;             // BeOS window tab
    lv_color_t floor, floor_line;         // Hotel room floor
    lv_color_t highlight, depth, grid;    // Pixel Iso lit edge, extrusion, backdrop grid
    // Metrics.
    int16_t  radius;        // ui_radius()
    int16_t  pill_radius;   // badges and other pills
    int8_t   keyline_w;     // chrome keyline / modal-button border
    int8_t   frame_w;       // modal frame
    int8_t   frame_radius;  // modal frame corners
    int8_t   seam_w;        // status-bar bottom seam; 0 = none
    lv_opa_t grad_darken;   // bottom of a top-lit chrome gradient
} ui_palette_t;
const ui_palette_t &ui_palette();     // active row; re-selected by theme_init

lv_color_t ui_color_accent();
lv_color_t ui_color_muted();
lv_color_t ui_color_bg();
lv_color_t ui_color_fg();
lv_color_t ui_color_surface();   // raised tile / pill face
lv_color_t ui_color_on_accent(); // text/icons on an accent or any saturated fill
lv_color_t ui_color_outline();   // keyline round chrome widgets (Hotel / BeOS)
lv_color_t ui_color_field();     // page of text fields and list rows
lv_color_t ui_color_titlebar();  // status-bar / title-bar fill
lv_color_t ui_color_title_fg();  // text on the title bar
lv_color_t ui_color_title_icon();// status icons on the title bar
lv_color_t ui_color_title_muted();// secondary title-bar text
int32_t ui_radius();
// Narrow display (< 320 px wide: the 240 px T-Watch-S3). Pages switch to
// single-column / stacked layouts and compact labels below this width.
bool ui_is_narrow();
#define UI_COLOR_ACCENT     ui_color_accent()
#define UI_COLOR_MUTED      ui_color_muted()
#define UI_COLOR_BG         ui_color_bg()
#define UI_COLOR_FG         ui_color_fg()
#define UI_COLOR_SURFACE    ui_color_surface()
#define UI_COLOR_ON_ACCENT  ui_color_on_accent()
#define UI_RADIUS           (ui_radius())
/* Isometric extrusion vector for Pixel Iso blocks — 2:1, the pixel-art iso ratio. */
#define UI_ISO_DEPTH_X   4
#define UI_ISO_DEPTH_Y   2
void ui_block(lv_obj_t *obj);  // raised block for Pixel Iso / Hotel / BeOS; no-op in Classic
void ui_apply_backdrop(lv_obj_t *obj);           // screen bg; Pixel Iso / Hotel tile a patterned backdrop
typedef enum { UI_TITLE_NONE = 0, UI_TITLE_BAR, UI_TITLE_TAB } ui_title_chrome_t;
// Title chrome per style: Hotel paints `bar` (may be the label itself) as the
// blue title bar, BeOS turns `label` into its window tab, Classic / Pixel Iso
// draw nothing. Either object may be NULL. Returns which chrome was drawn.
ui_title_chrome_t ui_dress_title(lv_obj_t *bar, lv_obj_t *label, int32_t pad_hor);

// Build (or rebuild) the LVGL theme for the current style, dark mode and
// system font. Returns false without touching anything when none of the three
// changed since the last build (P7.11).
bool theme_init();
