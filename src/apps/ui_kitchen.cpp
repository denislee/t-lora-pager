/**
 * @file      ui_kitchen.cpp
 * @brief     Kitchen: a vegetable-prep game. Stage 1 chops a cucumber.
 *            Turning the wheel rocks a chef's knife on its belly; every
 *            full stroke cuts a slice that tumbles onto the board, and the
 *            cucumber is pushed forward for the next one.
 *            Stage 2 peels a potato: the wheel drags a peeler down one
 *            meridian, the strip of skin curls off and drops away, and the
 *            potato turns to bring the next strip round.
 *
 * Rendering is a small hand-rolled 3D pass on one plain lv_obj
 * (LV_EVENT_DRAW_MAIN): an oblique projection of shaded low-poly cylinders
 * (cucumber, slices) plus outlined flat shapes (board, knife). The potato
 * is ray-cast per pixel into an image (a lumpy ellipsoid, flat colour
 * bands, antialiased edge); the peeler is a 3D slab drawn as line art.
 * Three things are buffered: the potato image (cast once per spin, then only
 * the strip being peeled is recoloured), the landed pile of slices (baked
 * when one lands) and the peeler's poses (each baked the first time it is
 * needed). The frame timer steps once per display refresh and invalidates
 * only the areas of the parts that moved (KITCHEN_FPS_PLAN.md).
 *
 * Input: wheel / arrow keys crank the knife (either direction chops),
 * touch drag does the same, click pauses (wheel then reaches Back) or,
 * once a stage is done, moves on to the next (the potato loops back to the
 * cucumber), ESC leaves.
 *
 * State reset on onStop (ui_kitchen_exit):
 *   widgets: stage (labels are its children), quit_btn — deleted, nulled
 *            s_pile.canvas (offscreen, parentless)      — pile_free(): lv_obj_delete
 *            s_py.canvas (offscreen, parentless)        — py_free(): lv_obj_delete
 *   timers:  frame_timer                               — lv_timer_del + null
 *            the display's refresh period, when KITCHEN_REFR_MS changes
 *            it                                        — set back to LV_DEF_REFR_PERIOD
 *            the probe's display REFR_START / REFR_READY (and emulator
 *            FLUSH_START) callbacks
 *            (KITCHEN_PROBE builds)                    — removed
 *   state:   s_game — rebuilt by new_game() on the next enter; the group's
 *            edit mode is switched back off so the next app starts in nav.
 *            s_pot / s_stage — rebuilt by start_potato() / new_game().
 *            s_dirty — invalidated (the next frame redraws the stage).
 *   memory:  s_pimg: buf (the potato image, internal RAM or a draw buf),
 *            px / list (its per-pixel cache)            — pot_free()
 *            s_pile.buf (the baked pile)                — pile_free(): lv_draw_buf_destroy
 *            s_py.slot[].mem (the peeler's poses, PSRAM) — py_free()
 * No tasks.
 */
#include "../ui_define.h"
#include "kitchen.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

// Frame probe (KITCHEN_FPS_PLAN.md KF.0) and bench (KF.1): compiled into the
// *_profile envs (INPUT_PROFILE), KITCHEN_BENCH builds and the emulator.
#if defined(INPUT_PROFILE) || defined(KITCHEN_BENCH) || !defined(ARDUINO)
#define KITCHEN_PROBE 1
#ifdef ARDUINO
#include <esp_timer.h>
#else
#include <chrono>
#endif
#endif

// lv_draw_triangle_dsc_t renamed bg_color -> color after 9.2 (the emulator
// pins 9.2.2, hardware 9.5.0).
#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR < 3
#define KT_TRI_COLOR(d) (d).bg_color
#else
#define KT_TRI_COLOR(d) (d).color
#endif

namespace {

// ---- Frame probe (KF.0) -----------------------------------------------------
// One `[kitchen]` line every 2 s while the game is open: refreshes that drew
// the stage per second, refresh time (display REFR_START -> REFR_READY:
// render + flush), stage_draw_cb time and runs per refresh (one per
// invalidated area), primitives issued to LVGL / skipped on the CPU, and the
// potato's ray-casts and strip recolours. The bench (KF.1) keeps the same
// totals per game phase. Each line also splits the refresh into flush (count,
// pixels, swap, push, MB/s) and render = refresh minus the time the flush held
// up the LVGL task (KF.24). Hardware numbers come from the vendor
// lvh_prof_flush hook, so they need INPUT_PROFILE.

#ifdef KITCHEN_PROBE
uint64_t kp_now()
{
#ifdef ARDUINO
    return (uint64_t)esp_timer_get_time();
#else
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
#endif
}

constexpr int KP_BINS = 400;   // refresh-time histogram, 0.25 ms bins

struct KpStats {
    uint32_t frames, draw_calls, tasks, culled, casts, recolours;
    uint64_t refr_us, refr_min_us, refr_max_us, draw_us, cast_us, recolour_us;
    uint32_t flushes;
    uint64_t flush_px, swap_us, push_us, blocked_us;
    uint16_t hist[KP_BINS];

    void frame(uint64_t us)
    {
        if (!frames || us < refr_min_us) refr_min_us = us;
        refr_max_us = std::max(refr_max_us, us);
        frames++;
        refr_us += us;
        uint32_t b = std::min<uint32_t>(KP_BINS - 1, (uint32_t)(us / 250));
        if (hist[b] < 0xffff) hist[b]++;
    }
    float p95_ms() const
    {
        uint32_t want = (frames * 95 + 99) / 100, n = 0;
        for (int b = 0; b < KP_BINS; b++)
            if ((n += hist[b]) >= want) return (b + 1) * 0.25f;
        return KP_BINS * 0.25f;
    }
    void print(const char *what) const
    {
        float f = frames ? (float)frames : 1.0f;
        uint32_t screen_px = (uint32_t)lv_display_get_horizontal_resolution(nullptr) *
                             (uint32_t)lv_display_get_vertical_resolution(nullptr);
        if (!screen_px) screen_px = 1;
        float flush_px_f = flush_px / f;
        float render_ms = ((float)refr_us - (float)blocked_us) / 1000.0f / f;
        if (render_ms < 0) render_ms = 0;
        float mbs = push_us ? flush_px * 2.0f / push_us : 0;
        printf("[kitchen] %s: %u frames | refr ms min %.1f avg %.1f p95 %.1f max %.1f | draw %.2f ms x%.2f/frame"
               " | tasks %.0f culled %.0f /frame | cast %u x %.1f ms | recolour %u x %.2f ms"
               " | flush x%.2f %.0f px (%.0f%% of screen) swap %.2f push %.2f ms | render %.2f ms | %.1f MB/s\n",
               what, (unsigned)frames, refr_min_us / 1000.0f, refr_us / 1000.0f / f, frames ? p95_ms() : 0.0f,
               refr_max_us / 1000.0f, draw_us / 1000.0f / f, draw_calls / f, tasks / f, culled / f, (unsigned)casts,
               casts ? cast_us / 1000.0f / casts : 0.0f, (unsigned)recolours,
               recolours ? recolour_us / 1000.0f / recolours : 0.0f, flushes / f, flush_px_f,
               100.0f * flush_px_f / (float)screen_px, swap_us / 1000.0f / f, push_us / 1000.0f / f, render_ms, mbs);
    }
};

// Bench phases (KF.1); -1 is everything in between.
enum { KB_EARLY_CHOP, KB_LATE_CHOP, KB_PEEL_STROKE, KB_PEEL_TURN, KB_N };
const char *const kBenchNames[KB_N] = {"early chop", "late chop", "peel stroke", "peel turn"};

// Counted during one refresh, then added to the stats at REFR_READY.
struct KpFrame {
    uint32_t draw_calls, tasks, culled;
    uint64_t draw_us;
    uint32_t flushes;
    uint64_t flush_px, swap_us, push_us, blocked_us;
};

struct KitchenProbe {
    KpStats win, bench[KB_N];
    KpFrame cur;
    uint32_t win_t0;
    uint64_t refr_t0;
    uint32_t casts_since_refr, multi_cast_refrs;
    bool bench_on, bench_finished, muted;
};
KitchenProbe s_kp;

int kp_bucket();   // the game phase now, for the bench (defined with the game)

// Apply f to the window and, while the bench runs, to the current phase.
template <typename F> void kp_add(F f)
{
    f(s_kp.win);
    if (s_kp.bench_on) {
        int b = kp_bucket();
        if (b >= 0) f(s_kp.bench[b]);
    }
}

void kp_refr_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_REFR_READY) {
        if (s_kp.casts_since_refr > 1) s_kp.multi_cast_refrs++;
        s_kp.casts_since_refr = 0;
    }
    if (s_kp.muted) return;
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        s_kp.refr_t0 = kp_now();
        s_kp.cur = KpFrame{};
    } else if (s_kp.cur.draw_calls) {   // REFR_READY of a refresh that drew the stage
        uint64_t us = kp_now() - s_kp.refr_t0;
        const KpFrame f = s_kp.cur;
        kp_add([&](KpStats &st) {
            st.frame(us);
            st.draw_calls += f.draw_calls;
            st.tasks += f.tasks;
            st.culled += f.culled;
            st.draw_us += f.draw_us;
            st.flushes += f.flushes;
            st.flush_px += f.flush_px;
            st.swap_us += f.swap_us;
            st.push_us += f.push_us;
            st.blocked_us += f.blocked_us;
        });
    }
}

#ifndef ARDUINO
// The SDL flush has no hook of its own: count the area here (swap/push read 0). Emulator times are -O0 host times with no flush cost; only ratios between phases mean anything (KF.25c).
void kp_flush_cb(lv_event_t *e)
{
    auto *a = (const lv_area_t *)lv_event_get_param(e);
    kitchen::note_flush((uint32_t)lv_area_get_size(a), 0, 0, 0);
}
#endif

// KF.14: what the game's internal-RAM image leaves for Wi-Fi / BLE.
void kp_print_heap()
{
#ifdef ARDUINO
    printf("[kitchen] min free internal heap: %u bytes\n", (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
#endif
}

void kp_report_tick()
{
    if (lv_tick_elaps(s_kp.win_t0) < 2000) return;
    float secs = lv_tick_elaps(s_kp.win_t0) / 1000.0f;
    if (s_kp.win.frames) {
        char what[32];
        snprintf(what, sizeof(what), "%.1f fps", s_kp.win.frames / secs);
        s_kp.win.print(what);
        kp_print_heap();
    }
    s_kp.win = KpStats{};
    s_kp.win_t0 = lv_tick_get();
}
#define KP(stmt) \
    do {         \
        stmt;    \
    } while (0)
#else
#define KP(stmt) ((void)0)
#endif

// Forced allocation failures, to check every bake/cache path fails soft
// (KITCHEN_FPS_PLAN_2026-11-02.md KF.13). Emulator only: KITCHEN_FAIL=pile_buf,pot_px
// fails the allocations named.
#ifdef ARDUINO
constexpr bool kf_fail(const char *) { return false; }
#else
bool kf_fail(const char *what)
{
    const char *s = getenv("KITCHEN_FAIL");
    return s && strstr(s, what);
}
#endif

// ---- World + projection ---------------------------------------------------
// x right, y up, z toward the viewer. The board is the plane y = 0; the
// cucumber lies along z with its cut end at the knife plane z = 0.

struct V3 {
    float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 norm(V3 a)
{
    float l = sqrtf(dot(a, a));
    return l > 1e-6f ? a * (1.0f / l) : a;
}

// Oblique projection: z slides down-right on screen. kView is the direction
// that projects to a single point, pointing at the viewer: a face is visible
// when its normal has a positive component along it.
constexpr float KX = 0.55f, KY = 0.30f;
const V3 kView = {-KX, KY, 1.0f};
const V3 kLight = norm({-0.5f, 0.9f, 0.55f});

// The slice of world space framed on screen (projected units).
constexpr float VIEW_X0 = -7.0f, VIEW_X1 = 7.5f;
constexpr float VIEW_Y0 = -6.7f, VIEW_Y1 = 2.6f;   // bottom leaves room for the table

// Scene dimensions (world units). A Japanese cucumber: long and slender.
constexpr float R = 0.7f;            // cucumber / slice radius
constexpr float T = 0.22f;           // slice thickness
constexpr float L_TOTAL = 10.5f;      // whole cucumber length
constexpr float STUB = 1.8f;         // end piece left uncut
constexpr int   MAX_SLICES = (int)((L_TOTAL - STUB) / T);
constexpr float TIP_X = -4.6f;       // knife rocking point on the board
constexpr float KNIFE_TIP = -1.6f;   // tip, ahead of the rocking point
constexpr float LB = 6.2f;           // blade length
constexpr float HB = 1.8f;           // blade height at the heel
constexpr float LH = 3.0f;           // handle length
constexpr float ALPHA_MAX = 33.0f * 3.14159265f / 180.0f;
constexpr float PERIOD = 6.0f;       // wheel notches per full chop
constexpr float BX0 = -6.5f, BX1 = 7.5f, BZ0 = -11.5f, BZ1 = 4.2f, BT = 0.45f;

// Japanese cucumber: deep green skin with pale prickles, pale crisp flesh.
const lv_color_t C_SKIN_DARK = lv_color_hex(0x072808);
const lv_color_t C_SKIN_LIGHT = lv_color_hex(0x4fae3c);
const lv_color_t C_FLESH = lv_color_hex(0xe6f6c4);
const lv_color_t C_RIM = lv_color_hex(0x2a7a24);
const lv_color_t C_SEED = lv_color_hex(0xc9e39a);
const lv_color_t C_DOT = lv_color_hex(0xe8f8d4);
const lv_color_t C_LINE = lv_color_hex(0xe6e6e6);

// A view onto the stage. With a layer it draws, skipping primitives wholly
// outside the layer's clip area before LVGL allocates a task for them
// (KF.6). Without one it is a measure pass: nothing is drawn, and bbox grows
// to cover what would have been, from the very same projection code.
struct Ctx {
    lv_layer_t *layer;
    float ox, oy, s;
    lv_area_t clip;
    lv_area_t *bbox;
};

constexpr lv_area_t NO_AREA = {0, 0, -1, -1};
bool area_empty(const lv_area_t &a) { return a.x1 > a.x2 || a.y1 > a.y2; }

void area_add(lv_area_t &d, const lv_area_t &a)
{
    if (area_empty(a)) return;
    if (area_empty(d)) {
        d = a;
        return;
    }
    d.x1 = std::min(d.x1, a.x1), d.y1 = std::min(d.y1, a.y1);
    d.x2 = std::max(d.x2, a.x2), d.y2 = std::max(d.y2, a.y2);
}

Ctx draw_ctx(const Ctx &view, lv_layer_t *layer)
{
    Ctx c = view;
    c.layer = layer;
    c.clip = layer->_clip_area;
    c.bbox = nullptr;
    return c;
}

Ctx measure_ctx(const Ctx &view, lv_area_t *bbox)
{
    Ctx c = view;
    c.layer = nullptr;
    c.bbox = bbox;
    return c;
}

// Every primitive passes its conservative screen bbox (antialiasing
// included) here first; true means draw it.
bool emit(const Ctx &c, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    lv_area_t a = {x1, y1, x2, y2};
    if (!c.layer) {
        area_add(*c.bbox, a);
        return false;
    }
    if (x2 < c.clip.x1 || x1 > c.clip.x2 || y2 < c.clip.y1 || y1 > c.clip.y2) {
        KP(s_kp.cur.culled++);
        return false;
    }
    KP(s_kp.cur.tasks++);
    return true;
}

lv_point_precise_t proj(const Ctx &c, V3 p)
{
    lv_point_precise_t o;
    o.x = (lv_value_precise_t)lroundf(c.ox + c.s * (p.x + KX * p.z));
    o.y = (lv_value_precise_t)lroundf(c.oy + c.s * (-p.y + KY * p.z));
    return o;
}

#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR < 3
// LVGL 9.2's lv_draw_sw_triangle takes any vertical side (two corners with
// the same x) as the triangle's top-to-bottom edge. When the third corner
// lies above or below that side instead, the edge masks clip the whole
// triangle away, leaving black holes (fixed in 9.3+, which re-sorts).
// Push one end of such a side 1 px away from the third corner.
void fix_vertical_side(lv_point_precise_t *p)
{
    for (int pass = 0; pass < 3; pass++) {
        bool changed = false;
        for (int i = 0; i < 3; i++) {
            for (int j = i + 1; j < 3; j++) {
                if ((int32_t)p[i].x != (int32_t)p[j].x) continue;
                int k = 3 - i - j;
                int32_t lo = std::min((int32_t)p[i].y, (int32_t)p[j].y);
                int32_t hi = std::max((int32_t)p[i].y, (int32_t)p[j].y);
                if ((int32_t)p[k].y >= lo && (int32_t)p[k].y <= hi) continue;
                // Move the end nearer the third corner: the side then leans
                // toward it and the shape only gains a sliver.
                int m = std::abs((int32_t)p[i].y - (int32_t)p[k].y) < std::abs((int32_t)p[j].y - (int32_t)p[k].y) ? i : j;
                p[m].x = (lv_value_precise_t)((int32_t)p[m].x + ((int32_t)p[k].x > (int32_t)p[m].x ? -1 : 1));
                changed = true;
            }
        }
        if (!changed) return;
    }
}
#endif

void tri2(const Ctx &c, lv_point_precise_t a, lv_point_precise_t b, lv_point_precise_t d, lv_color_t col)
{
    lv_point_precise_t p[3] = {a, b, d};
#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR < 3
    fix_vertical_side(p);   // before the bbox: it can move a corner several px
#endif
    if (!emit(c, (int32_t)std::min({p[0].x, p[1].x, p[2].x}) - 2, (int32_t)std::min({p[0].y, p[1].y, p[2].y}) - 2,
              (int32_t)std::max({p[0].x, p[1].x, p[2].x}) + 2, (int32_t)std::max({p[0].y, p[1].y, p[2].y}) + 2))
        return;
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.p[0] = p[0];
    t.p[1] = p[1];
    t.p[2] = p[2];
    KT_TRI_COLOR(t) = col;
    lv_draw_triangle(c.layer, &t);
}

// Solid-shaded surfaces: each triangle is pushed ~1 px out from its centroid
// so neighbours overlap. Otherwise the antialiased edges leave dark seams.
void tri_solid(const Ctx &c, lv_point_precise_t a, lv_point_precise_t b, lv_point_precise_t d, lv_color_t col)
{
    lv_point_precise_t p[3] = {a, b, d};
    float cx = ((float)a.x + (float)b.x + (float)d.x) / 3.0f;
    float cy = ((float)a.y + (float)b.y + (float)d.y) / 3.0f;
    for (auto &q : p) {
        float dx = (float)q.x - cx, dy = (float)q.y - cy;
        float l = sqrtf(dx * dx + dy * dy);
        if (l < 0.5f) continue;
        q.x = (lv_value_precise_t)lroundf((float)q.x + dx / l);
        q.y = (lv_value_precise_t)lroundf((float)q.y + dy / l);
    }
    tri2(c, p[0], p[1], p[2], col);
}

void line2(const Ctx &c, lv_point_precise_t a, lv_point_precise_t b, lv_color_t col, int w)
{
    if (!emit(c, (int32_t)std::min(a.x, b.x) - w - 1, (int32_t)std::min(a.y, b.y) - w - 1,
              (int32_t)std::max(a.x, b.x) + w + 1, (int32_t)std::max(a.y, b.y) + w + 1))
        return;
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.p1 = a;
    l.p2 = b;
    l.color = col;
    l.width = w;
    // Round caps cost a circle mask each; at 1-2 px they can't be seen (KF.9b).
    l.round_start = l.round_end = w > 2;
    lv_draw_line(c.layer, &l);
}

void line3(const Ctx &c, V3 a, V3 b, lv_color_t col, int w) { line2(c, proj(c, a), proj(c, b), col, w); }

void dot2(const Ctx &c, lv_point_precise_t p, int rad, lv_color_t col)
{
    lv_area_t a = {(int32_t)p.x - rad, (int32_t)p.y - rad, (int32_t)p.x + rad, (int32_t)p.y + rad};
    if (!emit(c, a.x1 - 1, a.y1 - 1, a.x2 + 1, a.y2 + 1)) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = col;
    // A 3x3 dot needs no circle mask (KF.9c).
    d.radius = rad > 1 ? LV_RADIUS_CIRCLE : 0;
    lv_draw_rect(c.layer, &d, &a);
}

int line_w(const Ctx &c, float k) { return std::max(1, (int)lroundf(c.s * k)); }

// Convex polygon, filled as a fan.
void fill_poly(const Ctx &c, const lv_point_precise_t *p, int n, lv_color_t col)
{
    for (int i = 1; i + 1 < n; i++) tri2(c, p[0], p[i], p[i + 1], col);
}

void outline_poly(const Ctx &c, const lv_point_precise_t *p, int n, lv_color_t col, int w)
{
    for (int i = 0; i < n; i++) line2(c, p[i], p[(i + 1) % n], col, w);
}

// ---- Cylinders --------------------------------------------------------------

constexpr int SEGS = 12;

lv_color_t skin_shade(V3 n)
{
    float i = 0.22f + 0.78f * std::max(0.0f, dot(n, kLight));
    return lv_color_mix(C_SKIN_LIGHT, C_SKIN_DARK, (uint8_t)(i * 255.0f));
}

// The cut face: flesh disc, darker rim ring, three seeds.
void draw_face(const Ctx &c, V3 center, V3 u, V3 v, float r, const lv_point_precise_t *ring)
{
    lv_point_precise_t ctr = proj(c, center);
    for (int i = 0; i < SEGS; i++) tri_solid(c, ctr, ring[i], ring[(i + 1) % SEGS], C_FLESH);
    int w = line_w(c, 0.09f);
    for (int i = 0; i < SEGS; i++) line2(c, ring[i], ring[(i + 1) % SEGS], C_RIM, w);
    int sr = std::max(1, (int)lroundf(c.s * 0.035f));
    for (int k = 0; k < 6; k++) {   // small seeds in three pairs
        float a = 0.5f + (k / 2) * 2.094f + (k & 1 ? 0.35f : -0.35f);
        dot2(c, proj(c, center + (u * cosf(a) + v * sinf(a)) * (0.36f * r)), sr, C_SEED);
    }
}

// A frustum from c0 (radius r0) to c1 (radius r1). Back faces are culled, so
// the visible facets of this convex solid never overlap and need no sorting.
void draw_cyl(const Ctx &c, V3 c0, V3 c1, float r0, float r1, bool face0, bool face1)
{
    V3 a = norm(c1 - c0);
    V3 helper = fabsf(a.y) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0};
    V3 u = norm(cross(helper, a));
    V3 v = cross(a, u);
    // Facet normals lean along the axis by the taper slope.
    float len = sqrtf(dot(c1 - c0, c1 - c0));
    float k = len > 1e-4f ? (r0 - r1) / len : 0.0f;
    lv_point_precise_t e0[SEGS], p0[SEGS], p1[SEGS];
    float cs[SEGS], sn[SEGS];
    for (int i = 0; i < SEGS; i++) {
        float th = i * 6.2831853f / SEGS;
        cs[i] = cosf(th);
        sn[i] = sinf(th);
        e0[i] = p0[i] = proj(c, c0 + (u * cs[i] + v * sn[i]) * r0);
    }
    bool vis[SEGS];
    lv_color_t col[SEGS];
    for (int i = 0; i < SEGS; i++) {
        int j = (i + 1) % SEGS;
        V3 n = norm(norm(u * (cs[i] + cs[j]) + v * (sn[i] + sn[j])) + a * k);
        vis[i] = dot(n, kView) > 0;
        if (!vis[i]) continue;
        col[i] = skin_shade(n);
        if (i & 1) col[i] = lv_color_darken(col[i], 26);   // fine ridges along the skin
    }
    // LVGL fills a triangle by masking its whole bounding box, which a long
    // facet lying along the screen diagonal mostly wastes: cut a long
    // frustum into pieces about two radii long (KF.9a).
    int pieces = 1;
    if (c.layer) {
        lv_point_precise_t q0 = proj(c, c0), q1 = proj(c, c1);
        float lpx = sqrtf((float)((q1.x - q0.x) * (q1.x - q0.x) + (q1.y - q0.y) * (q1.y - q0.y)));
        float rpx = c.s * std::max(r0, r1);
        if (rpx > 1 && lpx > 3 * rpx) pieces = std::min(8, (int)ceilf(lpx / (2 * rpx)));
    }
    for (int seg = 1; seg <= pieces; seg++) {
        float t = (float)seg / pieces;
        V3 cc = seg == pieces ? c1 : c0 + (c1 - c0) * t;
        float rr = seg == pieces ? r1 : r0 + (r1 - r0) * t;
        for (int i = 0; i < SEGS; i++) p1[i] = proj(c, cc + (u * cs[i] + v * sn[i]) * rr);
        for (int i = 0; i < SEGS; i++) {
            if (!vis[i]) continue;
            int j = (i + 1) % SEGS;
            tri_solid(c, p0[i], p0[j], p1[j], col[i]);
            tri_solid(c, p0[i], p1[j], p1[i], col[i]);
        }
        memcpy(p0, p1, sizeof(p0));
    }
    if (face1 && dot(a, kView) > 0) draw_face(c, c1, u, v, r1, p1);
    if (face0 && dot(a, kView) < 0) draw_face(c, c0, u, v, r0, e0);
}

// A flat disc across the solid; drawn first, it fills any gap the culled
// facets of a small piece leave, so the black background can't show through.
void fill_disc(const Ctx &c, V3 center, V3 axis, float r, lv_color_t col)
{
    V3 helper = fabsf(axis.y) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0};
    V3 u = norm(cross(helper, axis));
    V3 v = cross(axis, u);
    lv_point_precise_t ring[SEGS];
    for (int i = 0; i < SEGS; i++) {
        float th = i * 6.2831853f / SEGS;
        ring[i] = proj(c, center + (u * cosf(th) + v * sinf(th)) * r);
    }
    lv_point_precise_t ctr = proj(c, center);
    for (int i = 0; i < SEGS; i++) tri_solid(c, ctr, ring[i], ring[(i + 1) % SEGS], col);
}

// A rounded cucumber end on a base disc of radius r, bulging along `out`.
// Rings are drawn far-to-near so the nearer facets win.
void draw_round_end(const Ctx &c, V3 base, V3 out, float r)
{
    static const float prof[][2] = {   // {distance along out, radius} / r
        {0, 1.0f}, {0.3f, 0.86f}, {0.55f, 0.52f}, {0.7f, 0.0f},
    };
    constexpr int N = sizeof(prof) / sizeof(prof[0]);
    bool outward_first = dot(out, kView) > 0;
    for (int k = 0; k + 1 < N; k++) {
        int i = outward_first ? k : N - 2 - k;
        draw_cyl(c, base + out * (prof[i][0] * r), base + out * (prof[i + 1][0] * r),
                 prof[i][1] * r, std::max(0.02f, prof[i + 1][1] * r), false, false);
    }
}

// ---- Game state ---------------------------------------------------------------

struct Slice {
    V3 from, to;
    float yaw;       // spin about y while it tumbles
    float lean;      // final tilt off flat (radians)
    float t;         // tumble progress 0..1
    bool tip;        // the rounded front end, cut first; rolls onto its side
};

struct SkinDot {
    float th, u;     // angle around the axis, distance from the back end
};
constexpr int N_DOTS = 110;

struct Game {
    float phase_target, phase;   // crank position, in wheel notches
    float zf, zf_target;         // world z of the cucumber's front face
    float len;                   // remaining cucumber length
    int cuts;
    bool armed, pending_advance, done, paused;
    Slice slices[MAX_SLICES];
    SkinDot dots[N_DOTS];
    uint32_t rng;
};

Game s_game;
lv_obj_t *stage = nullptr;
lv_obj_t *quit_btn = nullptr;
lv_obj_t *lbl_title = nullptr;
lv_obj_t *lbl_count = nullptr;
lv_obj_t *lbl_hint = nullptr;
lv_timer_t *frame_timer = nullptr;
uint32_t s_last_tick = 0;
int s_stage = 0;   // 0 cucumber, 1 potato

// The game steps once per display refresh (KF.11): a frame timer beating
// against the refresh timer stepped some frames twice and others not at
// all. KITCHEN_REFR_MS (per env) also shortens the display's refresh
// period while the game is open; ui_kitchen_exit puts it back.
#ifndef KITCHEN_REFR_MS
#define KITCHEN_REFR_MS LV_DEF_REFR_PERIOD
#endif

// ---- Stage 2 state: the potato --------------------------------------------
// Model space: y up the potato's long axis, longitude ph around it (x = cos,
// z = sin), latitude th from the top (0) to the bottom (pi); the surface is
// D * u for u on the unit sphere, D = diag(PRX, PRY, PRZ). It spins about y,
// then is tilted toward the viewer and leant; the view looks down -z.
// Stroke k peels the strip centred on the meridian that sits at P_PEEL_W in
// front of the right limb, then the potato turns one strip so the peeled
// flesh rolls round to face the viewer.

constexpr int P_STRIPS = 16;
constexpr float P_PI = 3.14159265f;
constexpr float P_DELTA = 2 * P_PI / P_STRIPS;   // one strip, in longitude
constexpr float P_PEEL_W = P_DELTA;              // peel meridian, toward the viewer
constexpr float P_PERIOD = 6.0f;                 // wheel notches per strip
constexpr float P_PEEL_END = 0.8f;               // rest of the stroke: lift + turn
constexpr float P_TH0 = 0.14f * P_PI, P_TH1 = 0.78f * P_PI;  // peeler's stroke (the hidden bottom finishes with it)
constexpr float PRX = 2.0f, PRY = 2.35f, PRZ = 1.85f;         // semi-axes
constexpr float P_TILT = 0.3f, P_LEAN = -0.3f;
constexpr float PCX = -0.6f, PCY = -0.45f;
constexpr float PV_X0 = -4.5f, PV_X1 = 4.5f, PV_Y0 = -4.4f, PV_Y1 = 3.4f;
constexpr int SH_PTS = 16;     // points along a peel
constexpr int MAX_RIB_EYES = 6;
constexpr int MAX_SH = 4;      // peels in the air at once
constexpr int N_EYES = 34;

// Flat colours, light side round to the shadow (the art has no gradients).
const lv_color_t C_PSKIN[4] = {lv_color_hex(0xf6b46c), lv_color_hex(0xe8913f), lv_color_hex(0xc8702c),
                               lv_color_hex(0x9c5220)};
const lv_color_t C_PFLESH[4] = {lv_color_hex(0xfffbe8), lv_color_hex(0xfbe7aa), lv_color_hex(0xefcb78),
                                lv_color_hex(0xd2a254)};
const lv_color_t C_PRIM = lv_color_hex(0x8a4518);   // the skin's cut edge
const lv_color_t C_PEYE = lv_color_hex(0x2e1406);
const V3 kLightP = norm({-0.5f, 0.7f, 0.55f});

// A peel: its two cut edges, its skin side's normal, the eyes it carries.
struct Ribbon {
    V3 l[SH_PTS], r[SH_PTS], nrm[SH_PTS];
    V3 eye[MAX_RIB_EYES];
    int n_eye;
};

struct Shaving {
    Ribbon r;   // view space at release
    V3 vel, axis;
    float spin, t;
};

// A potato's own lumpiness: a radial offset on the unit sphere, made of a
// few broad low-order lobes (egg, kidney, flattened sides) and some shallow
// dents. Polynomials only, it is evaluated a few times per pixel.
constexpr int N_DENTS = 7;
struct PotShape {
    float c[7];
    V3 dent[N_DENTS];
    float depth[N_DENTS];
};

struct Potato {
    float phase, phase_target;   // crank position, in wheel notches
    int strips;                  // strips peeled off
    bool done;
    Shaving sh[MAX_SH];
    int n_sh;
    V3 eyes[N_EYES];             // on the unit sphere
    PotShape shape;
};

// The potato is ray-cast into an image once per spin (KF.3): the cast
// keeps, for every covered pixel, its height on the unit sphere, the strips
// under it and its three finished colours (flesh, skin, cut edge). While a
// strip is peeled the spin holds still, so a frame only re-picks the colour
// of the pixels that touch the strip whose peel line moved.
// The image is RGB565, pre-blended on the stage's background (KF.5): it is
// drawn first, over nothing else, so its edge can be blended at cast time
// and the blit is an opaque copy.
using PotPixel = uint16_t;
constexpr lv_color_format_t POT_CF = LV_COLOR_FORMAT_RGB565;

struct PotPx {
    uint32_t off;                  // byte offset in the image
    float uy;                      // unit-sphere height of the surface hit
    PotPixel flesh, skin, rim;     // the pixel peeled / on the skin / at a cut edge
    uint8_t s0, s1;                // strips: centre | above << 4, right | left << 4
};

struct PotImage {
    lv_draw_buf_t *buf;
#ifdef ARDUINO
    lv_draw_buf_t ram_buf;         // buf points here when its pixels live in internal RAM
    void *ram;
#endif
    bool valid;
    bool indexed;                  // px / list are complete: a strip can be recoloured
    float spin, s, ox, oy;
    int32_t x0, y0;
    float pc[P_STRIPS];            // the peel the pixels show
    PotPx *px;                     // covered pixels
    uint32_t n_px, cap_px;
    uint32_t *list, cap_list;      // pixel indices by strip: list[start[k] .. start[k + 1])
    uint32_t start[P_STRIPS + 1];
    lv_area_t sbox[P_STRIPS];      // screen area of each strip's pixels
};

Potato s_pot;
PotImage s_pimg = {};

// The landed pile only changes when a slice lands, and is drawn last of
// the static scene, so it is baked into an image then (KF.7): late in the
// game that is ~1,600 primitives a frame fewer. Drawn live until baked, or when it can't be.
struct PileImage {
    lv_draw_buf_t *buf;    // null: nothing has landed
    lv_obj_t *canvas;      // offscreen screen the bake draws through
    lv_area_t area;        // where the image goes on screen
    int landed;            // slices in it
    float s, ox, oy;       // the layout it was baked for
    bool valid;
    bool failed;           // this bake failed: drawn live until the next slice lands
};
PileImage s_pile = {};

// Dirty rectangles (KF.6 / KF.8): each moving part keeps the screen area it
// covered when last drawn.
enum { DP_KNIFE, DP_CUKE, DP_FLY, DP_PEELER, DP_SHAVINGS, DP_N };
struct Dirty {
    bool valid;   // false: the next frame redraws the whole stage
    int stage;
    float s, ox, oy;
    lv_area_t part[DP_N];
};
Dirty s_dirty = {};

float frand()
{
    s_game.rng = s_game.rng * 1664525u + 1013904223u;
    return (s_game.rng >> 8) * (1.0f / 16777216.0f);
}

float tri_wave(float p)
{
    float q = p / PERIOD;
    q -= floorf(q);
    return fabsf(1.0f - 2.0f * q);   // 1 = knife up, 0 = blade on the board
}

float knife_alpha() { return ALPHA_MAX * tri_wave(s_game.phase); }

void update_labels()
{
    if (!stage) return;
    const char *hint;
    if (s_stage == 0) {
        lv_label_set_text_fmt(lbl_count, "%d / %d", s_game.cuts, MAX_SLICES);
        if (s_game.done) hint = "All chopped! Click for the potato";
        else if (s_game.paused) hint = "Paused - click to resume";
        else if (s_game.cuts == 0) hint = "Turn the wheel to chop";
        else hint = "";
    } else {
        lv_label_set_text_fmt(lbl_count, "%d / %d", s_pot.strips, P_STRIPS);
        if (s_pot.done) hint = "All peeled! Click to start over";
        else if (s_game.paused) hint = "Paused - click to resume";
        else if (s_pot.phase_target <= 0) hint = "Turn the wheel to peel";
        else hint = "";
    }
    lv_label_set_text(lbl_hint, hint);
}

void pile_free();
void py_free();
void pot_free(PotImage &im);

void new_game()
{
    uint32_t seed = s_game.rng ? s_game.rng : lv_tick_get() | 1u;
    s_game = Game{};
    s_game.rng = seed;
    s_game.zf = s_game.zf_target = T;
    s_game.len = L_TOTAL;
    for (auto &d : s_game.dots) {
        d.th = frand() * 6.2831853f;
        d.u = 0.25f + frand() * (L_TOTAL - 0.3f);
    }
    s_stage = 0;
    s_pile.valid = false;
    s_pile.failed = false;
    pot_free(s_pimg);
    py_free();
    s_dirty.valid = false;
    if (lbl_title) lv_label_set_text(lbl_title, "Stage 1: Cucumber");
    update_labels();
    if (stage) lv_obj_invalidate(stage);
}

void start_potato()
{
    s_stage = 1;
    s_pot = Potato{};
    // Eyes: single specks scattered over the skin, kept off the poles.
    for (V3 &e : s_pot.eyes) {
        float th = (0.15f + frand() * 0.72f) * P_PI, ph = frand() * 2 * P_PI;
        e = {sinf(th) * cosf(ph), cosf(th), sinf(th) * sinf(ph)};
    }
    PotShape &sh = s_pot.shape;
    for (float &k : sh.c) k = (frand() - 0.5f) * 0.24f;
    sh.c[4] = 0.04f + frand() * 0.05f;   // one end fatter: always a bit of an egg
    for (int k = 0; k < N_DENTS; k++) {
        sh.dent[k] = norm({frand() - 0.5f, frand() - 0.5f, frand() - 0.5f});
        sh.depth[k] = 0.04f + frand() * 0.05f;
    }
    pile_free();
    s_pimg.valid = false;   // fresh skin: cast the image again
    s_dirty.valid = false;
    if (lbl_title) lv_label_set_text(lbl_title, "Stage 2: Potato");
    update_labels();
    if (stage) lv_obj_invalidate(stage);
}

bool stage_done() { return s_stage == 0 ? s_game.done : s_pot.done; }

void next_stage()
{
    if (s_stage == 0) start_potato();
    else new_game();
}

// Top of the pile under a flat slice centred at (x, z) on the board
// (base 0) or on the table in front of it (base -BT): every earlier slice on
// the same surface whose disc overlaps its footprint holds it up.
float pile_top(float x, float z, float base)
{
    float top = base;
    for (int i = 0; i < s_game.cuts; i++) {
        const Slice &o = s_game.slices[i];
        if (o.tip || (o.to.y < 0) != (base < 0)) continue;
        float dx = o.to.x - x, dz = o.to.z - z;
        if (dx * dx + dz * dz < (2 * R) * (2 * R)) top = std::max(top, o.to.y + T * 0.5f);
    }
    return top;
}

void spawn_slice()
{
    Slice &s = s_game.slices[s_game.cuts];
    s.from = {0, R, T * 0.5f};
    s.t = 0;
    s.lean = 0;
    s.tip = s_game.cuts == 0;
    if (s.tip) {
        // Rolls off to the left, in front of the blade and clear of the
        // pile, and lies on its side.
        s.to = {-4.0f, R, 2.4f + frand() * 0.5f};
        s.yaw = -(1.2f + frand() * 0.5f);   // cut face turned away, skin showing
        return;
    }
    // Slices land flat, resting on whatever they overlap. Of a few random
    // spots take the lowest, so the pile spreads before it climbs. Now and
    // then one flies clear of the board's front edge and drops to the table.
    bool off_board = frand() < 0.2f;
    float base = off_board ? -BT : 0.0f;
    float bx = 0, bz = 0, by = 1e9f;
    for (int k = 0; k < 10; k++) {
        float x = 0.3f + frand() * 4.6f;
        float z = off_board ? BZ1 + R + 0.1f + frand() * 0.5f : 1.0f + frand() * 2.4f;
        float y = pile_top(x, z, base);
        if (y < by) bx = x, bz = z, by = y;
    }
    s.to = {bx, by + T * 0.5f, bz};
    s.yaw = (frand() - 0.5f) * 1.2f;
}

void cut()
{
    spawn_slice();
    s_game.cuts++;
    s_game.len -= T;
    s_game.zf = s_game.zf_target = 0;
    s_game.armed = false;
    if (s_game.cuts >= MAX_SLICES) s_game.done = true;
    else s_game.pending_advance = true;
    hw_feedback();
    update_labels();
}

void slice_pose(const Slice &s, V3 &center, V3 &axis)
{
    float e = 1.0f - (1.0f - s.t) * (1.0f - s.t);   // ease out
    center = s.from + (s.to - s.from) * e;
    center.y += sinf(s.t * 3.14159265f) * 0.6f;
    if (s.tip) {
        // Swing the axis round to horizontal, dipping a little as it rolls.
        float psi = s.yaw * e;
        axis = norm({sinf(psi), -0.12f * e, cosf(psi)});
        return;
    }
    // Tip forward from upright (axis +z) to lying flat (axis ~+y).
    float phi = e * (1.5707963f - s.lean);
    V3 ax = {0, sinf(phi), cosf(phi)};
    float yaw = s.yaw * e;
    axis = {ax.z * sinf(yaw), ax.y, ax.z * cosf(yaw)};
}

// ---- Drawing ------------------------------------------------------------------

void draw_board(const Ctx &c)
{
    int w = line_w(c, 0.07f);
    V3 a = {BX0, 0, BZ0}, b = {BX1, 0, BZ0}, d = {BX1, 0, BZ1}, e = {BX0, 0, BZ1};
    V3 down = {0, -BT, 0};
    line3(c, a, b, C_LINE, w);
    line3(c, b, d, C_LINE, w);
    line3(c, d, e, C_LINE, w);
    line3(c, e, a, C_LINE, w);
    line3(c, a + down, e + down, C_LINE, w);
    line3(c, e + down, d + down, C_LINE, w);
    line3(c, a, a + down, C_LINE, w);
    line3(c, e, e + down, C_LINE, w);
    line3(c, d, d + down, C_LINE, w);
}

void draw_skin_dots(const Ctx &c, float z_back, float z0, float z1)
{
    int r = std::max(1, (int)lroundf(c.s * 0.028f));
    for (const auto &d : s_game.dots) {
        if (d.u > s_game.len - 0.12f) continue;
        float z = z_back + d.u;
        if (z < z0 || z > z1) continue;
        V3 n = {cosf(d.th), sinf(d.th), 0};
        if (dot(n, kView) < 0.2f) continue;
        dot2(c, proj(c, V3{0, R, z} + n * (R * 1.02f)), r, C_DOT);
    }
}

void draw_cucumber_back(const Ctx &c)
{
    float z_back = s_game.zf - s_game.len;
    float z_split = std::min(0.0f, s_game.zf);
    bool has_front = s_game.zf > 0.02f;
    // Rounded far end, then the body up to the knife plane.
    draw_round_end(c, {0, R, z_back}, {0, 0, -1}, R);
    draw_cyl(c, {0, R, z_back}, {0, R, z_split}, R, R, false, !has_front);
    draw_skin_dots(c, z_back, z_back, z_split);
}

void draw_cucumber_front(const Ctx &c)
{
    if (s_game.zf <= 0.02f) return;
    // Until the first cut the front still ends in its rounded tip.
    bool whole = s_game.cuts == 0;
    draw_cyl(c, {0, R, 0}, {0, R, s_game.zf}, R, R, false, !whole);
    if (whole) draw_round_end(c, {0, R, s_game.zf}, {0, 0, 1}, R);
    draw_skin_dots(c, s_game.zf - s_game.len, 0, s_game.zf);
}

// Knife-local (s along the flat of the edge, h up from the edge) to world.
// The knife lives in the plane z = 0 and rocks about s = 0, where the curved
// belly meets the flat edge; the tip sits ahead of it at negative s.
V3 knife_pt(float s, float h, float ca, float sa) { return {TIP_X + s * ca - h * sa, s * sa + h * ca, 0}; }

void knife_poly(const Ctx &c, const float (*pts)[2], int n, float ca, float sa, lv_point_precise_t *out)
{
    for (int i = 0; i < n; i++) out[i] = proj(c, knife_pt(pts[i][0], pts[i][1], ca, sa));
}

// A chef's knife: straight spine dipping to the tip, a belly curving up from
// the flat edge into it, a tall heel, and a narrower riveted handle flush
// with the spine.
void draw_knife(const Ctx &c)
{
    float al = knife_alpha();
    float ca = cosf(al), sa = sinf(al);
    int w = line_w(c, 0.07f);
    int thin = std::max(1, w / 2);
    const lv_color_t black = lv_color_black();

    static const float blade[][2] = {
        {KNIFE_TIP, HB * 0.62f},       // tip
        {KNIFE_TIP + 0.25f, HB * 0.36f},
        {KNIFE_TIP + 0.7f, HB * 0.15f},
        {KNIFE_TIP + 1.25f, HB * 0.04f},
        {0, 0},                        // belly meets the flat edge
        {LB, 0},                       // heel
        {LB, HB},                      // spine at the heel
    };
    constexpr int NB = sizeof(blade) / sizeof(blade[0]);
    lv_point_precise_t bp[NB];
    knife_poly(c, blade, NB, ca, sa, bp);
    fill_poly(c, bp, NB, black);
    outline_poly(c, bp, NB, C_LINE, w);

    // Edge bevel, following the belly toward the tip.
    static const float bevel[][2] = {
        {KNIFE_TIP + 0.55f, HB * 0.5f}, {KNIFE_TIP + 1.1f, HB * 0.3f}, {0.4f, HB * 0.24f}, {LB - 0.2f, HB * 0.24f},
    };
    constexpr int NV = sizeof(bevel) / sizeof(bevel[0]);
    lv_point_precise_t vp[NV];
    knife_poly(c, bevel, NV, ca, sa, vp);
    for (int i = 0; i + 1 < NV; i++) line2(c, vp[i], vp[i + 1], C_LINE, thin);

    // Bolster band, then the handle with a rounded butt.
    constexpr float HL = HB * 0.42f, HT = HB;   // handle bottom / top
    constexpr float BL = 0.28f;                 // bolster length
    static const float bolster[][2] = {{LB, HL - 0.06f}, {LB + BL, HL}, {LB + BL, HT}, {LB, HT}};
    lv_point_precise_t op[4];
    knife_poly(c, bolster, 4, ca, sa, op);
    fill_poly(c, op, 4, black);
    outline_poly(c, op, 4, C_LINE, w);

    static const float handle[][2] = {
        {LB + BL, HL}, {LB + LH - 0.35f, HL - 0.05f}, {LB + LH - 0.08f, HL + 0.06f}, {LB + LH, (HL + HT) * 0.5f},
        {LB + LH - 0.08f, HT - 0.04f}, {LB + LH - 0.35f, HT + 0.06f}, {LB + BL, HT},
    };
    constexpr int NH = sizeof(handle) / sizeof(handle[0]);
    lv_point_precise_t hp[NH];
    knife_poly(c, handle, NH, ca, sa, hp);
    fill_poly(c, hp, NH, black);
    outline_poly(c, hp, NH, C_LINE, w);

    int rr = std::max(2, (int)lroundf(c.s * 0.1f));
    for (int k = 0; k < 3; k++) {
        float s = LB + BL + (LH - BL) * (0.25f + k * 0.25f);
        lv_point_precise_t p = proj(c, knife_pt(s, (HL + HT) * 0.5f, ca, sa));
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_opa = LV_OPA_TRANSP;
        d.border_color = C_LINE;
        d.border_width = thin;
        d.radius = LV_RADIUS_CIRCLE;
        lv_area_t a = {(int32_t)p.x - rr, (int32_t)p.y - rr, (int32_t)p.x + rr, (int32_t)p.y + rr};
        if (emit(c, a.x1 - 1, a.y1 - 1, a.x2 + 1, a.y2 + 1)) lv_draw_rect(c.layer, &d, &a);
    }
}

void draw_one_slice(const Ctx &c, const Slice &s, V3 ctr, V3 ax)
{
    V3 h = ax * (T * 0.5f);
    if (!s.tip) {
        draw_cyl(c, ctr - h, ctr + h, R, R, true, true);
        return;
    }
    // Tip: cut face at -axis, rounded end at +axis; nearer part last.
    bool end_near = dot(ax, kView) > 0;
    fill_disc(c, ctr + h, ax, R * 0.97f, C_SKIN_DARK);
    if (!end_near) draw_round_end(c, ctr + h, ax, R);
    draw_cyl(c, ctr - h, ctr + h, R, R, true, false);
    if (end_near) draw_round_end(c, ctr + h, ax, R);
}

bool slice_landed(int i) { return s_game.slices[i].t >= 1.0f; }

int count_landed()
{
    int n = 0;
    for (int i = 0; i < s_game.cuts; i++) n += slice_landed(i);
    return n;
}

// Painter's order for the landed pile: a slice resting on another must come
// after it; otherwise the one nearer the viewer (on the board) goes last.
bool draw_landed(const Ctx &c, bool (*before_slice)(lv_layer_t *) = nullptr)
{
    const int n = s_game.cuts;
    V3 ctr[MAX_SLICES], ax[MAX_SLICES];
    bool placed[MAX_SLICES] = {};
    for (int i = 0; i < n; i++) slice_pose(s_game.slices[i], ctr[i], ax[i]);
    auto landed = slice_landed;
    auto ground_depth = [&](int i) { return -KX * ctr[i].x + ctr[i].z; };
    auto rests_on = [&](int a, int b) {   // a sits above b
        const Slice &sa = s_game.slices[a], &sb = s_game.slices[b];
        if (sa.tip || sb.tip || sa.to.y <= sb.to.y) return false;
        float dx = sa.to.x - sb.to.x, dz = sa.to.z - sb.to.z;
        return dx * dx + dz * dz < (2 * R) * (2 * R);
    };
    for (;;) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (placed[i] || !landed(i)) continue;
            bool blocked = false;
            for (int j = 0; j < n && !blocked; j++)
                blocked = j != i && !placed[j] && landed(j) && rests_on(i, j);
            if (!blocked && (best < 0 || ground_depth(i) < ground_depth(best))) best = i;
        }
        if (best < 0) break;
        if (before_slice && !before_slice(c.layer)) return false;
        placed[best] = true;
        draw_one_slice(c, s_game.slices[best], ctr[best], ax[best]);
    }
    return true;
}

// Slices still in the air are drawn on top of everything.
void draw_flying(const Ctx &c)
{
    for (int i = 0; i < s_game.cuts; i++) {
        if (slice_landed(i)) continue;
        V3 ctr, ax;
        slice_pose(s_game.slices[i], ctr, ax);
        draw_one_slice(c, s_game.slices[i], ctr, ax);
    }
}

bool pile_ready(const Ctx &c)
{
    return s_pile.valid && s_pile.landed == count_landed() && s_pile.s == c.s && s_pile.ox == c.ox &&
           s_pile.oy == c.oy;
}

void pile_free()
{
    if (s_pile.buf) {
        lv_image_cache_drop(s_pile.buf);
        lv_draw_buf_destroy(s_pile.buf);
    }
    if (s_pile.canvas) lv_obj_delete(s_pile.canvas);
    s_pile = PileImage{};
}

// A canvas layer queues its draw tasks until it is finished, and a late pile
// is thousands of them (the Ultra's used up the emulator's whole LVGL heap,
// and lv_draw_add_task doesn't survive a failed allocation). So the bake
// finishes the layer before every slice, leaving at most one slice's tasks
// queued, and stops if there isn't room for those.
constexpr size_t PILE_TASK_RESERVE = 32 * 1024;

bool pile_bake_step(lv_layer_t *layer)
{
    lv_canvas_finish_layer(s_pile.canvas, layer);
    lv_canvas_init_layer(s_pile.canvas, layer);
    if (kf_fail("pile_tasks")) return false;
    void *room = lv_malloc(PILE_TASK_RESERVE);
    if (!room) return false;
    lv_free(room);
    return true;
}

// The bake can't be done: free what it holds and leave the pile drawn live.
void pile_give_up()
{
    if (s_pile.buf) {
        lv_image_cache_drop(s_pile.buf);
        lv_draw_buf_destroy(s_pile.buf);
        s_pile.buf = nullptr;
    }
    s_pile.failed = true;
}

// Re-bake for view v if stale. Returns the screen area that changed.
lv_area_t pile_update(const Ctx &v)
{
    if (pile_ready(v)) return NO_AREA;
    // Failed for this pile already: it is drawn live, nothing changed. Try again when a slice lands.
    if (s_pile.failed && s_pile.landed == count_landed() && s_pile.s == v.s && s_pile.ox == v.ox && s_pile.oy == v.oy) return NO_AREA;
    s_pile.failed = false;
    lv_area_t changed = s_pile.valid && s_pile.buf ? s_pile.area : NO_AREA;
    lv_area_t box = NO_AREA;
    draw_landed(measure_ctx(v, &box));
    s_pile.valid = false;
    s_pile.landed = count_landed();
    s_pile.s = v.s, s_pile.ox = v.ox, s_pile.oy = v.oy;
    area_add(changed, box);
    if (area_empty(box)) {
        if (s_pile.buf) lv_image_cache_drop(s_pile.buf), lv_draw_buf_destroy(s_pile.buf);
        s_pile.buf = nullptr;
        s_pile.valid = true;
        return changed;
    }
    int32_t w = lv_area_get_width(&box), h = lv_area_get_height(&box);
    if (s_pile.buf && ((int32_t)s_pile.buf->header.w != w || (int32_t)s_pile.buf->header.h != h)) {
        lv_image_cache_drop(s_pile.buf);
        lv_draw_buf_destroy(s_pile.buf);
        s_pile.buf = nullptr;
    }
    if (!s_pile.buf && !kf_fail("pile_buf")) s_pile.buf = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0);
    if (!s_pile.canvas && !kf_fail("pile_canvas")) s_pile.canvas = lv_canvas_create(nullptr);
    if (!s_pile.buf || !s_pile.canvas) {
        pile_give_up();
        return changed;
    }
    lv_canvas_set_draw_buf(s_pile.canvas, s_pile.buf);
    lv_canvas_fill_bg(s_pile.canvas, lv_color_black(), LV_OPA_TRANSP);
    lv_layer_t layer;
    lv_canvas_init_layer(s_pile.canvas, &layer);
    Ctx c = draw_ctx(v, &layer);
    c.ox -= box.x1, c.oy -= box.y1;
    bool baked = draw_landed(c, pile_bake_step);
    lv_canvas_finish_layer(s_pile.canvas, &layer);
    if (!baked) {
        pile_give_up();
        return changed;
    }
    lv_image_cache_drop(s_pile.buf);
    s_pile.area = box;
    s_pile.valid = true;
    return changed;
}

void draw_pile(const Ctx &c)
{
    if (!pile_ready(c)) {
        draw_landed(c);
        return;
    }
    const lv_area_t &a = s_pile.area;
    if (!s_pile.buf || !emit(c, a.x1, a.y1, a.x2, a.y2)) return;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = s_pile.buf;
    lv_draw_image(c.layer, &d, &a);
}

// ---- Stage 2: potato ------------------------------------------------------------

lv_point_precise_t proj_o(const Ctx &c, V3 p)
{
    lv_point_precise_t o;
    o.x = (lv_value_precise_t)lroundf(c.ox + c.s * p.x);
    o.y = (lv_value_precise_t)lroundf(c.oy - c.s * p.y);
    return o;
}

float smooth01(float x) { return x * x * (3 - 2 * x); }

constexpr float DENT_S2 = 0.35f * 0.35f;   // dent radius, squared (unit sphere)

// Radial offset at unit direction u, and its gradient.
float pot_bump(V3 u, V3 &g)
{
    const float *c = s_pot.shape.c;
    float x = u.x, y = u.y, z = u.z;
    float b = c[0] * x * y + c[1] * y * z + c[2] * x * z + c[3] * (x * x - z * z) + c[4] * y * y * y +
              c[5] * x * y * y + c[6] * z * y * y;
    g = {c[0] * y + c[2] * z + 2 * c[3] * x + c[5] * y * y,
         c[0] * x + c[1] * z + 3 * c[4] * y * y + 2 * c[5] * x * y + 2 * c[6] * z * y,
         c[1] * y + c[2] * x - 2 * c[3] * z + c[6] * y * y};
    for (int k = 0; k < N_DENTS; k++) {
        V3 d = u - s_pot.shape.dent[k];
        float q = dot(d, d) / DENT_S2;
        if (q >= 1) continue;
        float w = 1 - q, a = s_pot.shape.depth[k];
        b -= a * w * w;
        g = g + d * (a * 4 * w / DENT_S2);
    }
    return b;
}

// Unit-sphere point of direction u on the lumpy surface (D * it is model space).
V3 pot_lump(V3 u)
{
    V3 g;
    return u * (1.0f + pot_bump(u, g));
}

V3 pot_surf(float th, float ph)
{
    float st = sinf(th);
    V3 u = pot_lump({st * cosf(ph), cosf(th), st * sinf(ph)});
    return {PRX * u.x, PRY * u.y, PRZ * u.z};
}

V3 pot_normal(V3 p) { return norm({p.x / (PRX * PRX), p.y / (PRY * PRY), p.z / (PRZ * PRZ)}); }

// Model -> view: spin about y, tilt the top toward the viewer, lean, place.
struct PotXf {
    float cs, sn, ct, st, cl, sl;
    explicit PotXf(float spin)
        : cs(cosf(spin)), sn(sinf(spin)), ct(cosf(P_TILT)), st(sinf(P_TILT)), cl(cosf(P_LEAN)), sl(sinf(P_LEAN))
    {
    }
    V3 rot(V3 p) const
    {
        V3 a = {p.x * cs - p.z * sn, p.y, p.z * cs + p.x * sn};
        V3 b = {a.x, a.y * ct - a.z * st, a.y * st + a.z * ct};
        return {b.x * cl - b.y * sl, b.x * sl + b.y * cl, b.z};
    }
    V3 operator()(V3 p) const { return rot(p) + V3{PCX, PCY, 0}; }
};

// Where the crank puts the stroke: strip k, peel progress pp (0..1), the
// lift-and-turn part after it (ret 0..1), and the resulting spin.
struct PotPose {
    int k;
    float pp, ret, spin;
};

PotPose pot_pose()
{
    PotPose q;
    float ph = std::min(s_pot.phase, P_STRIPS * P_PERIOD) / P_PERIOD;
    q.k = (int)floorf(ph);
    float p = ph - q.k;
    if (q.k >= P_STRIPS) q.k = P_STRIPS, p = 0;
    q.pp = std::min(1.0f, p / P_PEEL_END);
    q.ret = p > P_PEEL_END ? (p - P_PEEL_END) / (1.0f - P_PEEL_END) : 0.0f;
    q.spin = P_DELTA * (q.k + smooth01(q.ret));
    return q;
}

float pot_stroke_th(const PotPose &q) { return P_TH0 + (P_TH1 - P_TH0) * q.pp; }

// Per strip, cos(th) of the lowest peeled point: a point is peeled when its
// u.y is above it (2: untouched, -2: done).
void pot_peel(const PotPose &q, float pc[P_STRIPS])
{
    for (int k = 0; k < P_STRIPS; k++) {
        if (k < q.k || (k == q.k && q.pp >= 1.0f)) pc[k] = -2.0f;
        else if (k == q.k && q.pp > 0) pc[k] = cosf(pot_stroke_th(q));
        else pc[k] = 2.0f;
    }
}

float fast_atan2(float y, float x)
{
    float ax = fabsf(x), ay = fabsf(y);
    float mx = std::max(ax, ay);
    if (mx < 1e-9f) return 0;
    float a = std::min(ax, ay) / mx, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = P_PI - r;
    return y < 0 ? -r : r;
}

// Strip edges wander a little with height so the cuts look hand-made: the
// longitude shift at unit-sphere height uy.
float pot_wobble(float uy) { return 0.16f * uy * (1.0f - uy * uy); }

// The strip under unit-sphere height uy, model longitude ph.
int pot_strip(float uy, float ph)
{
    int k = (int)floorf((P_PEEL_W + P_DELTA * 0.5f - ph - pot_wobble(uy)) / P_DELTA) % P_STRIPS;
    return k < 0 ? k + P_STRIPS : k;
}

bool pot_peeled(const float *pc, float uy, float ph) { return uy > pc[pot_strip(uy, ph)]; }

// Flat bands by threshold (descending), each edge blended over w so the
// steps read as smooth curves rather than pixel stairs.
lv_color_t pot_band(const lv_color_t *pal, const float *th, int n, float v, float w)
{
    int lvl = n;
    for (int i = 0; i < n; i++) {
        if (v > th[i]) {
            lvl = i;
            break;
        }
    }
    lv_color_t col = pal[lvl];
    if (lvl > 0) {
        float f = (v - th[lvl - 1]) / w + 0.5f;
        if (f > 0) col = lv_color_mix(pal[lvl - 1], col, (uint8_t)(std::min(0.5f, f) * 255));
    }
    if (lvl < n) {
        float f = 0.5f - (v - th[lvl]) / w;
        if (f > 0) col = lv_color_mix(pal[lvl + 1], col, (uint8_t)(std::min(0.5f, f) * 255));
    }
    return col;
}

// ---- The potato image ----------------------------------------------------------

constexpr uint32_t STAGE_BG = 0x000000;   // the stage's colour: the potato image is blended on it

PotPixel pot_pack(lv_color_t c, float cov)
{
    uint8_t a = (uint8_t)(std::min(1.0f, cov) * 255);
    if (a < 255) c = lv_color_mix(c, lv_color_hex(STAGE_BG), a);
    return lv_color_to_u16(c);
}

PotPixel pot_clear()
{
    return lv_color_to_u16(lv_color_hex(STAGE_BG));
}

// 1/sqrt(x) to ~0.2%, one Newton step (KF.4c: the cast's normalisations).
float rsqrt_fast(float x)
{
    uint32_t i;
    float y;
    memcpy(&i, &x, 4);
    i = 0x5f3759dfu - (i >> 1);
    memcpy(&y, &i, 4);
    return y * (1.5f - 0.5f * x * y * y);
}

V3 norm_fast(V3 a)
{
    float l2 = dot(a, a);
    return l2 > 1e-12f ? a * rsqrt_fast(l2) : a;
}

// How a cast is done. The defaults are the fast path; the emulator's cast
// check compares it with the exact one.
struct CastMode {
    bool adaptive;   // KF.4a: 2x2 blocks filled from agreeing corners
    bool fast;       // KF.4c: approximate normalisation
    int iters;       // KF.4b: refinements of the surface hit
};
constexpr CastMode CAST_EXACT = {false, false, 3};
// Two refinements instead of three moved band edges on 2-4.5% of the pixels
// (the emulator's cast check), past the plan's 0.25 px bound, for a 10-15%
// gain: KF.4b stays at three.
constexpr CastMode CAST_FAST = {true, true, 3};

// The ray set-up for one spin and layout: orthographic rays down -z through
// each pixel centre, in unit-sphere space u = D^-1 R^T (P - C), so a pixel's
// ray is u(t) = a + t b with a linear in the pixel.
struct PotCast {
    V3 r0, r1, r2, b, ax, ay, a0;
    float bb, w, pix;
    CastMode m;
};

// What a cast finds under one pixel.
struct PotSample {
    float cov;       // <= 0: background
    V3 un;           // the surface hit, on the unit sphere
    uint8_t s0, s1;  // as PotPx
    lv_color_t flesh, skin;
};

// The strips under hit un: its own, and probes a little above and to
// either side (skin beside the flesh shows its cut edge, which gives the
// peel depth).
void pot_sample_strips(PotSample &o)
{
    const V3 &un = o.un;
    float ph = fast_atan2(un.z, un.x);
    float e = 0.055f / sqrtf(std::max(0.05f, 1.0f - un.y * un.y));
    o.s0 = (uint8_t)(pot_strip(un.y, ph) | pot_strip(un.y + 0.045f, ph) << 4);
    o.s1 = (uint8_t)(pot_strip(un.y, ph + e) | pot_strip(un.y, ph - e) << 4);
}

PotCast pot_cast_setup(const Ctx &c, const PotPose &q, int32_t x0, int32_t y0, const CastMode &m)
{
    PotCast k;
    PotXf xf(q.spin);
    k.r0 = xf.rot({1, 0, 0}), k.r1 = xf.rot({0, 1, 0}), k.r2 = xf.rot({0, 0, 1});
    auto to_u = [&](V3 v) { return V3{dot(k.r0, v) / PRX, dot(k.r1, v) / PRY, dot(k.r2, v) / PRZ}; };
    k.b = to_u({0, 0, 1});
    k.ax = to_u({1.0f / c.s, 0, 0}), k.ay = to_u({0, -1.0f / c.s, 0});
    k.a0 = to_u({(x0 + 0.5f - c.ox) / c.s - PCX, (c.oy - (y0 + 0.5f)) / c.s - PCY, 0});
    k.bb = dot(k.b, k.b);
    k.w = 1.6f / (PRZ * c.s);   // lighting change across about a pixel
    // A pixel's size in unit-sphere space, across the ray.
    V3 axp = k.ax - k.b * (dot(k.ax, k.b) / k.bb), ayp = k.ay - k.b * (dot(k.ay, k.b) / k.bb);
    k.pix = sqrtf(std::max(dot(axp, axp), dot(ayp, ayp)));
    k.m = m;
    return k;
}

void pot_cast(const PotCast &k, V3 a, PotSample &o)
{
    static const float kFleshT[3] = {0.84f, 0.5f, 0.12f};
    static const float kSkinT[3] = {0.7f, 0.33f, 0.0f};
    // Closest approach of the ray to the centre; the surface radius in that
    // direction decides the silhouette (antialiased).
    float ts = -dot(a, k.b) / k.bb;
    V3 pm = a + k.b * ts;
    float dm2 = dot(pm, pm), dm = sqrtf(dm2);
    o.cov = 0;
    if (dm > 1.3f) return;
    V3 g;
    float rho = 1.0f + pot_bump(dm > 1e-6f ? pm * (1.0f / dm) : V3{0, 0, 1}, g);
    o.cov = (rho - dm) / k.pix + 0.5f;
    if (o.cov <= 0) return;
    // Front hit: intersect the sphere of the local radius, refine.
    V3 u = pm, un = u;
    for (int it = 0; it < k.m.iters; it++) {
        u = a + k.b * (ts + sqrtf(std::max(0.0f, rho * rho - dm2) / k.bb));
        un = k.m.fast ? norm_fast(u) : norm(u);
        rho = 1.0f + pot_bump(un, g);
    }
    // Normal: the sphere's, tilted by the bump's slope, then through D.
    V3 ns = un - (g - un * dot(g, un)), nm;
    if (k.m.fast) {
        ns = norm_fast(ns);
        nm = norm_fast({ns.x / PRX, ns.y / PRY, ns.z / PRZ});
    } else {
        ns = norm(ns);
        nm = norm({ns.x / PRX, ns.y / PRY, ns.z / PRZ});
    }
    V3 n = k.r0 * nm.x + k.r1 * nm.y + k.r2 * nm.z;
    float l = dot(n, kLightP);
    o.un = un;
    pot_sample_strips(o);
    o.flesh = pot_band(C_PFLESH, kFleshT, 3, l, k.w);
    // Blotches in the skin: a smooth pattern fixed to the potato.
    float blot = 0.18f * (2.0f * un.x * un.z + un.x * un.y - 1.2f * un.z * un.y * un.y + 0.5f * un.y);
    o.skin = pot_band(C_PSKIN, kSkinT, 3, l + blot, k.w);
}

// The look of a cached pixel under peel state pc.
PotPixel pot_pick(const PotPx &p, const float *pc)
{
    if (p.uy > pc[p.s0 & 15]) return p.flesh;
    if (p.uy + 0.045f > pc[p.s0 >> 4] || p.uy > pc[p.s1 & 15] || p.uy > pc[p.s1 >> 4]) return p.rim;
    return p.skin;
}

// The distinct strips a cached pixel looks at (1..4).
int pot_px_strips(const PotPx &p, uint8_t out[4])
{
    const uint8_t all[4] = {(uint8_t)(p.s0 & 15), (uint8_t)(p.s0 >> 4), (uint8_t)(p.s1 & 15), (uint8_t)(p.s1 >> 4)};
    int n = 0;
    for (uint8_t k : all) {
        bool seen = false;
        for (int m = 0; m < n; m++) seen |= out[m] == k;
        if (!seen) out[n++] = k;
    }
    return n;
}

bool same_color(lv_color_t a, lv_color_t b) { return a.red == b.red && a.green == b.green && a.blue == b.blue; }

// Agreeing corners: all background, or all fully covered with the same
// colours. o gets their look; its hit is theirs averaged, and its strips
// are worked out from that (cheap next to a cast), so strip edges stay
// sharp through a block.
bool pot_agree(const PotSample *const *sm, int n, PotSample &o)
{
    const PotSample &f = *sm[0];
    bool bg = f.cov <= 0;
    V3 un = {0, 0, 0};
    for (int m = 0; m < n; m++) {
        const PotSample &x = *sm[m];
        if (bg) {
            if (x.cov > 0) return false;
            continue;
        }
        if (x.cov < 1 || !same_color(x.flesh, f.flesh) || !same_color(x.skin, f.skin)) return false;
        un = un + x.un;
    }
    o = f;
    if (!bg) {
        o.un = norm_fast(un);
        pot_sample_strips(o);
    }
    return true;
}

// The pixel cache is large (~16 B a covered pixel): PSRAM on the device,
// the C heap on the emulator (LVGL's own pool there is too small).
void *cache_realloc(void *p, size_t n)
{
#ifdef ARDUINO
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return realloc(p, n);
#endif
}

void cache_free(void *p)
{
#ifdef ARDUINO
    heap_caps_free(p);
#else
    free(p);
#endif
}

// Writes the pixel and, when covered, caches it. False: out of memory.
bool pot_put(PotImage &im, int i, int j, const PotSample &o, const float *pc)
{
    uint32_t off = (uint32_t)j * im.buf->header.stride + (uint32_t)i * sizeof(PotPixel);
    PotPixel *dst = (PotPixel *)(im.buf->data + off);
    if (o.cov <= 0) {
        *dst = pot_clear();
        return true;
    }
    PotPx p;
    p.off = off, p.uy = o.un.y, p.s0 = o.s0, p.s1 = o.s1;
    p.flesh = pot_pack(o.flesh, o.cov), p.skin = pot_pack(o.skin, o.cov), p.rim = pot_pack(C_PRIM, o.cov);
    *dst = pot_pick(p, pc);
    if (im.n_px == im.cap_px) {
        uint32_t cap = im.cap_px ? im.cap_px * 2 : 4096;
        PotPx *px = kf_fail("pot_px") ? nullptr : (PotPx *)cache_realloc(im.px, cap * sizeof(PotPx));
        if (!px) return false;
        im.px = px, im.cap_px = cap;
    }
    im.px[im.n_px++] = p;
    return true;
}

// Ray-cast the whole image for pose q and index its pixels by strip.
// False: the cache is incomplete (out of memory, or a forced failure); the
// image is still right.
bool pot_full_cast(PotImage &im, const Ctx &c, const PotPose &q, const float *pc, int size, const CastMode &m)
{
    PotCast k = pot_cast_setup(c, q, im.x0, im.y0, m);
    im.n_px = 0;
    bool ok = true;
    PotSample o;
    PotSample *rows = m.adaptive && !kf_fail("pot_rows") ? (PotSample *)lv_malloc(size / 2 * 2 * sizeof(PotSample)) : nullptr;
    if (!rows) {
        for (int j = 0; j < size; j++) {
            V3 a = k.a0 + k.ay * (float)j;
            for (int i = 0; i < size; i++, a = a + k.ax) {
                pot_cast(k, a, o);
                ok &= pot_put(im, i, j, o, pc);
            }
        }
    } else {
        // KF.4a: cast every other pixel of every other row (the grid); a
        // pixel between grid casts that agree takes their look, any other
        // is cast too, so the silhouette, band edges and strip edges stay exact.
        int g = size / 2;   // size is even: grid columns 0, 2, .., size - 2
        PotSample *A = rows, *B = rows + g;
        auto cast_at = [&](int i, int j, PotSample &s) { pot_cast(k, k.a0 + k.ay * (float)j + k.ax * (float)i, s); };
        for (int x = 0; x < g; x++) cast_at(2 * x, 0, A[x]);
        for (int j = 0; j < size; j += 2) {
            for (int i = 0; i < size; i++) {
                if (!(i & 1)) {
                    o = A[i / 2];
                } else {
                    const PotSample *sm[2] = {&A[i / 2], &A[i / 2 + 1]};
                    if (i + 1 >= size || !pot_agree(sm, 2, o)) cast_at(i, j, o);
                }
                ok &= pot_put(im, i, j, o, pc);
            }
            bool next = j + 2 < size;
            if (next)
                for (int x = 0; x < g; x++) cast_at(2 * x, j + 2, B[x]);
            for (int i = 0; i < size; i++) {
                bool hit = false;
                if (next && !(i & 1)) {
                    const PotSample *sm[2] = {&A[i / 2], &B[i / 2]};
                    hit = pot_agree(sm, 2, o);
                } else if (next && i + 1 < size) {
                    const PotSample *sm[4] = {&A[i / 2], &A[i / 2 + 1], &B[i / 2], &B[i / 2 + 1]};
                    hit = pot_agree(sm, 4, o);
                }
                if (!hit) cast_at(i, j + 1, o);
                ok &= pot_put(im, i, j + 1, o, pc);
            }
            std::swap(A, B);
        }
        lv_free(rows);
    }
    if (!ok) return false;

    // Index the pixels by every strip they look at, and box each strip.
    uint32_t count[P_STRIPS] = {};
    uint8_t st[4];
    for (uint32_t n = 0; n < im.n_px; n++) {
        int ns = pot_px_strips(im.px[n], st);
        for (int m = 0; m < ns; m++) count[st[m]]++;
    }
    im.start[0] = 0;
    for (int s = 0; s < P_STRIPS; s++) im.start[s + 1] = im.start[s] + count[s];
    uint32_t total = im.start[P_STRIPS];
    if (total > im.cap_list) {
        uint32_t *l = kf_fail("pot_list") ? nullptr : (uint32_t *)cache_realloc(im.list, total * sizeof(uint32_t));
        if (!l) return false;
        im.list = l, im.cap_list = total;
    }
    uint32_t pos[P_STRIPS];
    for (int s = 0; s < P_STRIPS; s++) pos[s] = im.start[s], im.sbox[s] = NO_AREA;
    uint32_t stride = im.buf->header.stride;
    for (uint32_t n = 0; n < im.n_px; n++) {
        const PotPx &p = im.px[n];
        int32_t y = im.y0 + (int32_t)(p.off / stride), x = im.x0 + (int32_t)(p.off % stride / sizeof(PotPixel));
        lv_area_t a = {x, y, x, y};
        int ns = pot_px_strips(p, st);
        for (int m = 0; m < ns; m++) {
            im.list[pos[st[m]]++] = n;
            area_add(im.sbox[st[m]], a);
        }
    }
    return true;
}

void pot_free_buf(PotImage &im)
{
    if (!im.buf) return;
    lv_image_cache_drop(im.buf);
#ifdef ARDUINO
    if (im.ram) {
        heap_caps_free(im.ram);
        im.ram = nullptr;
        im.buf = nullptr;
        return;
    }
#endif
    lv_draw_buf_destroy(im.buf);
    im.buf = nullptr;
}

void pot_alloc_buf(PotImage &im, int size)
{
    if (kf_fail("pot_buf")) return;
#ifdef ARDUINO
    // KF.5: on the pager and watch the image fits in internal RAM, which
    // the blit reads faster than PSRAM. Leave room for everything else.
    uint32_t stride = lv_draw_buf_width_to_stride(size, POT_CF), bytes = stride * size;
    constexpr uint32_t CAP = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    if (bytes <= 64 * 1024 && heap_caps_get_largest_free_block(CAP) > bytes + 48 * 1024) {
        im.ram = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, bytes, CAP);
        if (im.ram && lv_draw_buf_init(&im.ram_buf, size, size, POT_CF, stride, im.ram, bytes) == LV_RESULT_OK) {
            im.buf = &im.ram_buf;
            return;
        }
        heap_caps_free(im.ram);
        im.ram = nullptr;
    }
#endif
    im.buf = lv_draw_buf_create(size, size, POT_CF, 0);
}

void pot_free(PotImage &im)
{
    pot_free_buf(im);
    cache_free(im.px);
    cache_free(im.list);
    im.px = nullptr, im.list = nullptr;
    im.n_px = im.cap_px = im.cap_list = 0;
    im.valid = false;
    im.indexed = false;
}

int pot_eye_r(const Ctx &c) { return std::max(1, (int)lroundf(c.s * 0.035f)); }

// Bring the image up to the pose: a full cast when the spin or the layout
// changed, else a recolour of the strips whose peel line moved. Returns
// the screen area that changed (eyes included), empty when none did.
lv_area_t update_potato(const Ctx &c, const PotPose &q, const float *pc)
{
    int half = (int)ceilf(PRY * 1.25f * c.s) + 2;   // room for the lumps
    int size = 2 * half;
    PotImage &im = s_pimg;
    if (im.buf && (int)im.buf->header.w != size) pot_free_buf(im);
    if (!im.buf) {
        pot_alloc_buf(im, size);
        im.valid = false;
        if (!im.buf) return NO_AREA;
    }
    lv_area_t dirty = NO_AREA;
    bool repeel = memcmp(im.pc, pc, sizeof(im.pc)) != 0;
    if (!im.valid || im.spin != q.spin || im.s != c.s || im.ox != c.ox || im.oy != c.oy || (repeel && !im.indexed)) {
#ifdef KITCHEN_PROBE
        uint64_t t0 = kp_now();
#endif
        im.spin = q.spin, im.s = c.s, im.ox = c.ox, im.oy = c.oy;
        im.x0 = (int32_t)floorf(c.ox + c.s * PCX) - half;
        im.y0 = (int32_t)floorf(c.oy - c.s * PCY) - half;
        im.indexed = pot_full_cast(im, c, q, pc, size, CAST_FAST);
        im.valid = true;
        memcpy(im.pc, pc, sizeof(im.pc));
        dirty = {im.x0, im.y0, im.x0 + size - 1, im.y0 + size - 1};
#ifdef KITCHEN_PROBE
        uint64_t us = kp_now() - t0;
        s_kp.casts_since_refr++;
        kp_add([&](KpStats &st) {
            st.casts++;
            st.cast_us += us;
        });
#endif
    } else if (repeel) {
#ifdef KITCHEN_PROBE
        uint64_t t0 = kp_now();
#endif
        for (int k = 0; k < P_STRIPS; k++) {
            if (im.pc[k] == pc[k]) continue;
            for (uint32_t n = im.start[k]; n < im.start[k + 1]; n++) {
                const PotPx &p = im.px[im.list[n]];
                *(PotPixel *)(im.buf->data + p.off) = pot_pick(p, pc);
            }
            area_add(dirty, im.sbox[k]);
        }
        memcpy(im.pc, pc, sizeof(im.pc));
        int r = pot_eye_r(c) + 2;
        if (!area_empty(dirty)) dirty.x1 -= r, dirty.y1 -= r, dirty.x2 += r, dirty.y2 += r;
#ifdef KITCHEN_PROBE
        uint64_t us = kp_now() - t0;
        kp_add([&](KpStats &st) {
            st.recolours++;
            st.recolour_us += us;
        });
#endif
    }
    if (!area_empty(dirty)) lv_image_cache_drop(im.buf);
    return dirty;
}

void draw_potato(const Ctx &c, const PotPose &q, const float *pc)
{
    update_potato(c, q, pc);   // normally done already, by the frame timer
    if (!s_pimg.buf) return;
    int32_t sz = (int32_t)s_pimg.buf->header.w;
    lv_area_t a = {s_pimg.x0, s_pimg.y0, s_pimg.x0 + sz - 1, s_pimg.y0 + sz - 1};
    if (emit(c, a.x1, a.y1, a.x2, a.y2)) {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = s_pimg.buf;
        lv_draw_image(c.layer, &d, &a);
    }

    // Eyes, on the skin that is left.
    PotXf xf(q.spin);
    int r = pot_eye_r(c);
    for (const V3 &u : s_pot.eyes) {
        if (pot_peeled(pc, u.y, fast_atan2(u.z, u.x))) continue;
        V3 nm = norm({u.x / PRX, u.y / PRY, u.z / PRZ});
        if (xf.rot(nm).z < 0.3f) continue;
        V3 l = pot_lump(u);
        dot2(c, proj_o(c, xf({l.x * PRX, l.y * PRY, l.z * PRZ})), r, C_PEYE);
    }
}

// The peel coming off strip q.k: the very patch of skin the strip covers,
// same wavy edges, on the same lumpy surface, eyes and all. Points run from
// the top of the potato down to the blade (or to the bottom once the whole
// strip is off); the part furthest from the blade has curled off the most.
bool peel_ribbon(const PotPose &q, Ribbon &r, bool whole)
{
    if (q.k >= P_STRIPS || (!whole && (q.ret > 0 || q.pp < 0.05f))) return false;
    PotXf xf(q.spin);
    float phc = P_PEEL_W - q.k * P_DELTA;   // strip centre, model longitude
    float th0 = 0.02f, th1 = whole ? P_PI - 0.02f : pot_stroke_th(q);
    auto lift = [&](float th) {
        float f = 1.0f - (th - th0) / (th1 - th0);
        return 0.08f + 1.1f * powf(std::max(0.0f, f), 1.3f);
    };
    for (int i = 0; i < SH_PTS; i++) {
        float th = th0 + (th1 - th0) * i / (SH_PTS - 1);
        float w = pot_wobble(cosf(th));
        V3 pm = pot_surf(th, phc - w), n = pot_normal(pm);
        V3 off = n * lift(th);
        r.l[i] = xf(pot_surf(th, phc + P_DELTA * 0.5f - w) + off);
        r.r[i] = xf(pot_surf(th, phc - P_DELTA * 0.5f - w) + off);
        r.nrm[i] = xf.rot(n);
    }
    r.n_eye = 0;
    for (const V3 &u : s_pot.eyes) {
        if (r.n_eye == MAX_RIB_EYES) break;
        float th = acosf(std::max(-1.0f, std::min(1.0f, u.y)));
        if (th > th1 || pot_strip(u.y, fast_atan2(u.z, u.x)) != q.k) continue;
        V3 l = pot_lump(u), p = {l.x * PRX, l.y * PRY, l.z * PRZ};
        r.eye[r.n_eye++] = xf(p + pot_normal(p) * (lift(th) + 0.01f));
    }
    return true;
}

// Two-sided: the skin outside, pale flesh on the underside.
void draw_ribbon(const Ctx &c, const Ribbon &r, int step = 1)
{
    lv_point_precise_t L[SH_PTS], R[SH_PTS];
    for (int i = 0; i < SH_PTS; i++) {
        L[i] = proj_o(c, r.l[i]);
        R[i] = proj_o(c, r.r[i]);
    }
    // next point after i at this step, never past the end
    auto next = [&](int i) { return std::min(i + step, SH_PTS - 1); };
    for (int i = 0; i + 1 < SH_PTS; i += step) {
        int m = next(i);
        V3 n = norm(r.nrm[i] + r.nrm[m]);
        float l = dot(n, kLightP);
        lv_color_t col;
        if (n.z >= 0) col = l > 0.55f ? C_PSKIN[0] : l > 0.15f ? C_PSKIN[1] : l > -0.2f ? C_PSKIN[2] : C_PSKIN[3];
        else col = -l > 0.3f ? C_PFLESH[1] : C_PFLESH[2];
        tri_solid(c, L[i], R[i], R[m], col);
        tri_solid(c, L[i], R[m], L[m], col);
    }
    for (int i = 0; i + 1 < SH_PTS; i += step) {
        int m = next(i);
        line2(c, L[i], L[m], C_PRIM, 1);
        line2(c, R[i], R[m], C_PRIM, 1);
    }
    line2(c, L[SH_PTS - 1], R[SH_PTS - 1], C_PRIM, 1);
    if (r.nrm[SH_PTS / 2].z > 0) {
        int er = std::max(1, (int)lroundf(c.s * 0.035f));
        for (int k = 0; k < r.n_eye; k++) dot2(c, proj_o(c, r.eye[k]), er, C_PEYE);
    }
}

V3 rodrigues(V3 v, V3 k, float ca, float sa) { return v * ca + cross(k, v) * sa + k * (dot(k, v) * (1 - ca)); }

// KF.20a: a peel in the air tumbles about its own axis, so its projected
// shape changes every frame and can't be one baked image. Falling and
// spinning, half the segments can't be told from all of them: ~33 tasks, not ~61.
void draw_shavings(const Ctx &c)
{
    for (int k = 0; k < s_pot.n_sh; k++) {
        const Shaving &s = s_pot.sh[k];
        V3 cen = {0, 0, 0};
        for (int i = 0; i < SH_PTS; i++) cen = cen + (s.r.l[i] + s.r.r[i]) * (0.5f / SH_PTS);
        float a = s.spin * s.t, ca = cosf(a), sa = sinf(a);
        V3 move = s.vel * s.t + V3{0, -7.0f * s.t * s.t, 0};
        Ribbon r;
        for (int i = 0; i < SH_PTS; i++) {
            r.l[i] = cen + move + rodrigues(s.r.l[i] - cen, s.axis, ca, sa);
            r.r[i] = cen + move + rodrigues(s.r.r[i] - cen, s.axis, ca, sa);
            r.nrm[i] = rodrigues(s.r.nrm[i], s.axis, ca, sa);
        }
        r.n_eye = s.r.n_eye;
        for (int i = 0; i < r.n_eye; i++) r.eye[i] = cen + move + rodrigues(s.r.eye[i] - cen, s.axis, ca, sa);
        draw_ribbon(c, r, 2);
    }
}

// Screen-space 2D helpers for the peeler.
struct P2 {
    float x, y;
};
P2 p2_norm(P2 a)
{
    float l = sqrtf(a.x * a.x + a.y * a.y);
    return l > 1e-4f ? P2{a.x / l, a.y / l} : P2{0, 1};
}

// The peeler: a Y-peeler modelled in 3D and drawn as line art. Its profile
// lives in its own plane: u runs from the blade (u = 0, on the skin) out to
// the butt, v along the blade, in world units. The profile is extruded to a
// slab PY_T thick, rolled and pitched so its side shows, and drawn back to
// front with black fills hiding the lines behind: back face, side walls
// (cross-lines on the ones facing us give the wireframe), front face.
constexpr float PY_R = 0.95f, PY_ARM = 0.21f;      // fork: radius, half thickness
constexpr float PY_H0 = 1.5f, PY_H1 = 3.4f;        // straight handle from / to (u)
constexpr float PY_W0 = 0.27f, PY_W1 = 0.46f;      // handle half widths there
constexpr float PY_HOLE_U = 3.38f, PY_HOLE_R = 0.18f;
constexpr float PY_T = 0.3f;                       // thickness
constexpr float PY_SCALE = 1.15f;
constexpr int PY_MAXP = 96;

struct PeelerProfile {
    float p[PY_MAXP][2];
    int n;
    uint8_t tri[PY_MAXP][3];
    int ntri;
    float sign;   // orientation of the outline: +1 counter-clockwise in (u, v)
};

float cross2(const float *o, const float *a, const float *b)
{
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}

// Ear clipping; the hole is joined to the outline by a zero-width bridge.
void py_triangulate(PeelerProfile &pr)
{
    int idx[PY_MAXP], m = pr.n;
    for (int i = 0; i < m; i++) idx[i] = i;
    pr.ntri = 0;
    for (int guard = 0; m > 3 && guard < 4 * PY_MAXP; guard++) {
        bool cut = false;
        for (int i = 0; i < m && !cut; i++) {
            int a = idx[(i + m - 1) % m], b = idx[i], c = idx[(i + 1) % m];
            if (cross2(pr.p[a], pr.p[b], pr.p[c]) * pr.sign <= 1e-6f) continue;   // reflex
            bool inside = false;
            for (int k = 0; k < m && !inside; k++) {
                const float *q = pr.p[idx[k]];
                if (idx[k] == a || idx[k] == b || idx[k] == c) continue;
                float d0 = cross2(pr.p[a], pr.p[b], q) * pr.sign, d1 = cross2(pr.p[b], pr.p[c], q) * pr.sign,
                      d2 = cross2(pr.p[c], pr.p[a], q) * pr.sign;
                inside = d0 > 1e-5f && d1 > 1e-5f && d2 > 1e-5f;
            }
            if (inside) continue;
            pr.tri[pr.ntri][0] = (uint8_t)a, pr.tri[pr.ntri][1] = (uint8_t)b, pr.tri[pr.ntri][2] = (uint8_t)c;
            pr.ntri++;
            for (int k = i; k + 1 < m; k++) idx[k] = idx[k + 1];
            m--;
            cut = true;
        }
        if (!cut) break;
    }
    if (m == 3) {
        pr.tri[pr.ntri][0] = (uint8_t)idx[0], pr.tri[pr.ntri][1] = (uint8_t)idx[1], pr.tri[pr.ntri][2] = (uint8_t)idx[2];
        pr.ntri++;
    }
}

const PeelerProfile &py_profile()
{
    static PeelerProfile pr;
    if (pr.n) return pr;
    auto add = [&](float u, float v) {
        if (pr.n < PY_MAXP) pr.p[pr.n][0] = u, pr.p[pr.n][1] = v, pr.n++;
    };
    auto w = [](float u) { return PY_W0 + (PY_W1 - PY_W0) * (u - PY_H0) / (PY_H1 - PY_H0); };
    const float ro = PY_R + PY_ARM, ri = PY_R - PY_ARM, aj = 0.6f;   // aj: where the arm meets the fillet
    // Upper tip, outer arc, fillet into the handle's upper edge.
    for (int i = 0; i <= 5; i++) {
        float a = -P_PI * 0.5f - P_PI * i / 5;
        add(PY_ARM * cosf(a), PY_R + PY_ARM * sinf(a));
    }
    for (int i = 1; i <= 6; i++) {
        float a = P_PI * 0.5f - (P_PI * 0.5f - aj) * i / 6;
        add(ro * cosf(a), ro * sinf(a));
    }
    float f0u = ro * cosf(aj), f0v = ro * sinf(aj), cu = ro * cosf(0.24f), cv = PY_W0;
    for (int i = 1; i <= 4; i++) {   // quadratic Bezier fillet
        float t = i / 5.0f, s = 1 - t;
        add(s * s * f0u + 2 * s * t * cu + t * t * PY_H0, s * s * f0v + 2 * s * t * cv + t * t * PY_W0);
    }
    for (int i = 0; i <= 3; i++) {
        float u = PY_H0 + (PY_H1 - PY_H0) * i / 3;
        add(u, w(u));
    }
    // Butt, round; the bridge in to the hole leaves from its tip.
    for (int i = 1; i <= 4; i++) {
        float a = P_PI * 0.5f - P_PI * 0.5f * i / 4;
        add(PY_H1 + PY_W1 * cosf(a), PY_W1 * sinf(a));
    }
    for (int i = 0; i <= 12; i++) {   // hole, the other way round
        float a = 2 * P_PI * i / 12;
        add(PY_HOLE_U + PY_HOLE_R * cosf(a), -PY_HOLE_R * sinf(a));
    }
    add(PY_H1 + PY_W1, 0);
    for (int i = 1; i <= 4; i++) {
        float a = -P_PI * 0.5f * i / 4;
        add(PY_H1 + PY_W1 * cosf(a), PY_W1 * sinf(a));
    }
    for (int i = 1; i <= 3; i++) {
        float u = PY_H1 - (PY_H1 - PY_H0) * i / 3;
        add(u, -w(u));
    }
    for (int i = 1; i <= 4; i++) {
        float t = i / 5.0f, s = 1 - t;
        add(s * s * PY_H0 + 2 * s * t * cu + t * t * f0u, -(s * s * PY_W0 + 2 * s * t * cv + t * t * f0v));
    }
    for (int i = 0; i <= 5; i++) {
        float a = -aj - (P_PI * 0.5f - aj) * i / 5;
        add(ro * cosf(a), ro * sinf(a));
    }
    // Lower tip, then the inside of the fork back up.
    for (int i = 1; i <= 5; i++) {
        float a = -P_PI * 0.5f - P_PI * i / 5;
        add(PY_ARM * cosf(a), -PY_R + PY_ARM * sinf(a));
    }
    for (int i = 1; i < 12; i++) {
        float a = -P_PI * 0.5f + P_PI * i / 12;
        add(ri * cosf(a), ri * sinf(a));
    }
    float area = 0;
    for (int i = 0; i < pr.n; i++) area += cross2(pr.p[0], pr.p[i], pr.p[(i + 1) % pr.n]);
    pr.sign = area > 0 ? 1.0f : -1.0f;
    py_triangulate(pr);
    return pr;
}

// KF.19: the peeler is ~270 tasks a frame, and the same few shapes over and
// over. Its shape only depends on the handle's screen angle and its roll,
// so those are snapped to steps and each resulting pose is baked once into
// an image (ARGB8888: it has antialiased edges over the potato and peel).
// Nothing about them depends on the strip, so they last for the whole potato.
constexpr float PY_ANG_STEP = 4.0f * P_PI / 180.0f, PY_ROLL_STEP = 0.1f;
constexpr int PY_SLOTS = 32;
constexpr size_t PY_BUDGET = 4 * 1024 * 1024;   // PSRAM; least recently used goes

struct PeelerPose {
    V3 o;           // where the shape's origin goes (view space)
    int ang, roll;  // in steps
    bool flip;      // the fork's far side faces down the stroke
};

struct PeelerSprite {
    int key;
    lv_draw_buf_t buf;
    void *mem;
    size_t bytes;
    lv_area_t box;    // around the pose's origin
    uint32_t used;
};

struct PeelerSprites {
    PeelerSprite slot[PY_SLOTS];
    int n;
    size_t bytes;
    uint32_t clock;
    float s;           // the scale they were baked for
    lv_obj_t *canvas;  // offscreen screen the bake draws through
    uint32_t bakes, misses;
};
PeelerSprites s_py = {};

void py_evict(int i)
{
    PeelerSprite &sp = s_py.slot[i];
    lv_image_cache_drop(&sp.buf);
    cache_free(sp.mem);
    s_py.bytes -= sp.bytes;
    s_py.slot[i] = s_py.slot[--s_py.n];
}

void py_free()
{
    while (s_py.n) py_evict(s_py.n - 1);
    if (s_py.canvas) lv_obj_delete(s_py.canvas);
    s_py = PeelerSprites{};
}

int py_key(const PeelerPose &p) { return (p.ang + 512) << 8 | (p.roll & 127) << 1 | (p.flip ? 1 : 0); }

// The blade rides the stroke on the skin; the handle sticks out away from
// the potato, sloping down, its plane rolled toward us so its edge shows.
// Drawn back to front with black fills hiding the lines behind.
PeelerPose peeler_pose(const Ctx &c, const PotPose &q)
{
    PotXf xf(q.spin);
    float ph = P_PEEL_W - q.spin;
    float th = pot_stroke_th(q), lift = 0;
    if (q.k >= P_STRIPS) {
        th = 0.45f * P_PI, lift = 0.8f;   // done: set down beside it
    } else if (q.ret > 0) {
        th = P_TH1 + (P_TH0 - P_TH1) * smooth01(q.ret);
        lift = 0.9f * sinf(P_PI * q.ret);
    }
    V3 cv = xf(pot_surf(th, ph));
    lv_point_precise_t a = proj_o(c, cv);
    lv_point_precise_t b = proj_o(c, xf(pot_surf(th + 0.05f, ph)));
    lv_point_precise_t pc = proj_o(c, xf({0, 0, 0}));
    P2 t = p2_norm({(float)(b.x - a.x), (float)(b.y - a.y)});   // down the stroke (screen)
    P2 n = {-t.y, t.x};
    if ((a.x - pc.x) * n.x + (a.y - pc.y) * n.y < 0) n = {-n.x, -n.y};   // away from the potato
    P2 h = p2_norm({n.x + 0.9f, n.y + 0.25f});                            // handle direction

    PeelerPose p;
    p.ang = (int)lroundf(atan2f(h.y, h.x) / PY_ANG_STEP);
    p.roll = (int)lroundf((0.45f + 0.2f * sinf(P_PI * q.pp)) / PY_ROLL_STEP);
    V3 v0 = {h.y, h.x, 0};
    p.flip = v0.x * t.x - v0.y * t.y < 0;
    float off = lift + 0.05f;
    p.o = cv + V3{n.x, -n.y, 0} * off + V3{0, 0, 1.0f};
    return p;
}

void draw_peeler_shape(const Ctx &c, const PeelerPose &p)
{
    float ang = p.ang * PY_ANG_STEP;
    P2 h = {cosf(ang), sinf(ang)};
    V3 h0 = {h.x, -h.y, 0}, v0 = {h.y, h.x, 0}, w0 = {0, 0, 1};
    if (p.flip) v0 = v0 * -1.0f;
    float roll = p.roll * PY_ROLL_STEP, pitch = 0.3f;

    // Peeler frame in view space (y up): roll about the handle, then pitch
    // the handle away from us.
    V3 vv = v0 * cosf(roll) + w0 * sinf(roll), w1 = w0 * cosf(roll) - v0 * sinf(roll);
    V3 hh = h0 * cosf(pitch) + w1 * sinf(pitch), ww = w1 * cosf(pitch) - h0 * sinf(pitch);
    const V3 o = p.o;
    hh = hh * PY_SCALE, vv = vv * PY_SCALE;
    V3 wt = ww * (PY_T * 0.5f * PY_SCALE * (ww.z >= 0 ? 1.0f : -1.0f));   // toward the viewer
    auto at = [&](float u, float v, float side) { return proj_o(c, o + hh * u + vv * v + wt * side); };

    const PeelerProfile &pr = py_profile();
    lv_point_precise_t fp[PY_MAXP], bp[PY_MAXP];
    for (int i = 0; i < pr.n; i++) {
        fp[i] = at(pr.p[i][0], pr.p[i][1], 1);
        bp[i] = at(pr.p[i][0], pr.p[i][1], -1);
    }
    const lv_color_t black = lv_color_black();
    int lw = line_w(c, 0.045f);

    auto fill_face = [&](const lv_point_precise_t *p) {
        for (int k = 0; k < pr.ntri; k++) tri_solid(c, p[pr.tri[k][0]], p[pr.tri[k][1]], p[pr.tri[k][2]], black);
    };
    // A side wall faces us when its outward normal (in the profile plane,
    // carried into view space) points at the viewer.
    bool facing[PY_MAXP];
    for (int i = 0; i < pr.n; i++) {
        int j = (i + 1) % pr.n;
        float eu = pr.p[j][0] - pr.p[i][0], ev = pr.p[j][1] - pr.p[i][1];
        float nu = ev * pr.sign, nv = -eu * pr.sign;   // outward
        facing[i] = (hh * nu + vv * nv).z > 0 && (eu * eu + ev * ev) > 1e-8f;
    }

    // The slab is closed and filled black, so any line of sight to its
    // back face or a wall turned away first crosses the front face or a
    // facing wall: only those are filled (KF.7a).
    for (int i = 0; i < pr.n; i++) {
        if (!facing[i]) continue;
        int j = (i + 1) % pr.n;
        tri_solid(c, fp[i], fp[j], bp[j], black);
        tri_solid(c, fp[i], bp[j], bp[i], black);
    }
    for (int i = 0; i < pr.n; i++) {
        if (!facing[i]) continue;
        int j = (i + 1) % pr.n;
        line2(c, bp[i], bp[j], C_LINE, lw);
        // Cross-lines on the walls: the wireframe. Every other vertex, plus
        // where a wall turns away (its silhouette).
        int ip = (i + pr.n - 1) % pr.n;
        if (i % 2 == 0 || !facing[ip]) line2(c, fp[i], bp[i], C_LINE, lw);
        if (!facing[j]) line2(c, fp[j], bp[j], C_LINE, lw);
    }
    fill_face(fp);
    for (int i = 0; i < pr.n; i++) {
        int j = (i + 1) % pr.n;
        float du = pr.p[j][0] - pr.p[i][0], dv = pr.p[j][1] - pr.p[i][1];
        if (du * du + dv * dv < 1e-8f) continue;
        if (fabsf(pr.p[i][1]) < 1e-4f && fabsf(pr.p[j][1]) < 1e-4f) continue;   // the hole's bridge
        line2(c, fp[i], fp[j], C_LINE, lw);
    }

    // Wires from the fork tips to the blade, then the blade: a small box.
    constexpr float BL = 0.55f, BU = 0.07f, BW = 0.1f;
    line2(c, at(0, PY_R, 0), at(0, BL, 0), C_LINE, lw);
    line2(c, at(0, -PY_R, 0), at(0, -BL, 0), C_LINE, lw);
    V3 bc[8];
    for (int k = 0; k < 8; k++) {
        float su = (k & 1) ? BU : -BU, sv = (k & 2) ? BL : -BL, sw = (k & 4) ? BW : -BW;
        bc[k] = o + hh * su + vv * sv + ww * sw;
    }
    static const uint8_t faces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
    V3 bctr = o;
    for (const auto &f : faces) {
        V3 m = (bc[f[0]] + bc[f[1]] + bc[f[2]] + bc[f[3]]) * 0.25f;
        V3 nrm = cross(bc[f[1]] - bc[f[0]], bc[f[3]] - bc[f[0]]);
        if (dot(nrm, m - bctr) < 0) nrm = nrm * -1.0f;
        if (nrm.z <= 0) continue;
        lv_point_precise_t qd[4];
        for (int k = 0; k < 4; k++) qd[k] = proj_o(c, bc[f[k]]);
        fill_poly(c, qd, 4, black);
        outline_poly(c, qd, 4, C_LINE, lw);
    }
}

// The sprite for pose p, baking it when bake is set. Null: draw it live.
PeelerSprite *py_sprite(const Ctx &c, const PeelerPose &p, bool bake)
{
    if (s_py.s != c.s) {
        lv_obj_t *canvas = s_py.canvas;
        s_py.canvas = nullptr;
        py_free();
        s_py.canvas = canvas;
        s_py.s = c.s;
    }
    int key = py_key(p);
    for (int i = 0; i < s_py.n; i++) {
        if (s_py.slot[i].key != key) continue;
        s_py.slot[i].used = ++s_py.clock;
        return &s_py.slot[i];
    }
    if (!bake) return nullptr;
    s_py.misses++;

    PeelerPose at0 = p;
    at0.o = {0, 0, 0};
    Ctx v = {nullptr, 0, 0, c.s, NO_AREA, nullptr};
    lv_area_t box = NO_AREA;
    draw_peeler_shape(measure_ctx(v, &box), at0);
    if (area_empty(box)) return nullptr;
    int32_t w = lv_area_get_width(&box), h = lv_area_get_height(&box);
    uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_ARGB8888), size = stride * h;
    size_t bytes = size + LV_DRAW_BUF_ALIGN;
    while (s_py.n && (s_py.n == PY_SLOTS || s_py.bytes + bytes > PY_BUDGET)) {
        int oldest = 0;
        for (int i = 1; i < s_py.n; i++)
            if (s_py.slot[i].used < s_py.slot[oldest].used) oldest = i;
        py_evict(oldest);
    }
    if (bytes > PY_BUDGET || kf_fail("py_tasks")) return nullptr;
    void *room = lv_malloc(3 * PILE_TASK_RESERVE);   // the bake queues a whole peeler's tasks
    if (!room) return nullptr;
    lv_free(room);
    if (!s_py.canvas && !kf_fail("py_canvas")) s_py.canvas = lv_canvas_create(nullptr);
    void *mem = kf_fail("py_buf") ? nullptr : cache_realloc(nullptr, bytes);
    if (!s_py.canvas || !mem) {
        cache_free(mem);
        return nullptr;
    }
    PeelerSprite &sp = s_py.slot[s_py.n];
    sp.key = key, sp.mem = mem, sp.bytes = bytes, sp.box = box, sp.used = ++s_py.clock;
    if (lv_draw_buf_init(&sp.buf, w, h, LV_COLOR_FORMAT_ARGB8888, stride, lv_draw_buf_align(mem, LV_COLOR_FORMAT_ARGB8888), size) != LV_RESULT_OK) {
        cache_free(mem);
        return nullptr;
    }
    lv_draw_buf_set_flag(&sp.buf, LV_IMAGE_FLAGS_MODIFIABLE);
    s_py.n++;
    s_py.bytes += bytes;
    s_py.bakes++;
    lv_canvas_set_draw_buf(s_py.canvas, &sp.buf);
    lv_canvas_fill_bg(s_py.canvas, lv_color_black(), LV_OPA_TRANSP);
    lv_layer_t layer;
    lv_canvas_init_layer(s_py.canvas, &layer);
    Ctx b = draw_ctx(v, &layer);
    b.ox = (float)-box.x1, b.oy = (float)-box.y1;
    draw_peeler_shape(b, at0);
    lv_canvas_finish_layer(s_py.canvas, &layer);
    lv_image_cache_drop(&sp.buf);
    return &sp;
}

// The measure pass runs on every frame the peeler moves, before the refresh,
// so that's when a new pose is baked; drawing only ever reuses one.
void draw_peeler(const Ctx &c, const PotPose &q)
{
    PeelerPose p = peeler_pose(c, q);
    PeelerSprite *sp = py_sprite(c, p, !c.layer);
    if (!sp) {
        draw_peeler_shape(c, p);
        return;
    }
    lv_point_precise_t at = proj_o(c, p.o);
    lv_area_t a = sp->box;
    lv_area_move(&a, (int32_t)at.x, (int32_t)at.y);
    if (!emit(c, a.x1, a.y1, a.x2, a.y2)) return;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = &sp->buf;
    lv_draw_image(c.layer, &d, &a);
}

// KF.21a: a part whose whole measured area misses this dirty area needn't
// even be projected. The areas are only current once a frame has measured
// them, and every state change goes through a frame.
bool part_hit(const Ctx &c, int id)
{
    if (!s_dirty.valid || s_dirty.stage != s_stage || s_dirty.s != c.s || s_dirty.ox != c.ox || s_dirty.oy != c.oy) return true;
    const lv_area_t &a = s_dirty.part[id];
    if (area_empty(a)) return false;
    return !(a.x2 < c.clip.x1 || a.x1 > c.clip.x2 || a.y2 < c.clip.y1 || a.y1 > c.clip.y2);
}

void draw_potato_stage(const Ctx &c)
{
    PotPose q = pot_pose();
    float pc[P_STRIPS];
    pot_peel(q, pc);
    draw_potato(c, q, pc);
    bool peeler = part_hit(c, DP_PEELER);
    Ribbon rib;
    if (peeler && peel_ribbon(q, rib, false)) draw_ribbon(c, rib);
    if (part_hit(c, DP_SHAVINGS)) draw_shavings(c);
    if (peeler) draw_peeler(c, q);
}

// What the crank moves besides the potato: the live peel and the peeler.
void draw_peeler_now(const Ctx &c)
{
    PotPose q = pot_pose();
    Ribbon rib;
    if (peel_ribbon(q, rib, false)) draw_ribbon(c, rib);
    draw_peeler(c, q);
}

void finish_strip(int k)
{
    PotPose q = {k, 1.0f, 0.0f, k * P_DELTA};
    if (s_pot.n_sh == MAX_SH) {   // drop the oldest
        for (int i = 1; i < MAX_SH; i++) s_pot.sh[i - 1] = s_pot.sh[i];
        s_pot.n_sh--;
    }
    Shaving &s = s_pot.sh[s_pot.n_sh++];
    peel_ribbon(q, s.r, true);
    s.vel = {0.8f + frand() * 0.8f, 1.2f, 0};
    s.axis = norm({frand() - 0.5f, frand() - 0.5f, frand() - 0.5f});
    s.spin = 2.0f + frand() * 2.0f;
    s.t = 0;
    s_pot.strips = k + 1;
    hw_feedback();
    update_labels();
}

// The view for the current game stage, from the stage's coordinates (no
// layer). False before the stage has a size.
bool stage_view(Ctx &c)
{
    if (!stage) return false;
    lv_area_t a;
    lv_obj_get_coords(stage, &a);
    float w = (float)lv_area_get_width(&a), h = (float)lv_area_get_height(&a);
    if (w <= 0 || h <= 0) return false;
    c = Ctx{nullptr, 0, 0, 0, NO_AREA, nullptr};
    if (s_stage == 1) {
        // View bounds are y-up here (the cucumber's are projected, y-down).
        c.s = std::min(w / (PV_X1 - PV_X0), h / (PV_Y1 - PV_Y0));
        c.ox = a.x1 + w * 0.5f - c.s * (PV_X0 + PV_X1) * 0.5f;
        c.oy = a.y1 + h * 0.5f + c.s * (PV_Y0 + PV_Y1) * 0.5f;
    } else {
        c.s = std::min(w / (VIEW_X1 - VIEW_X0), h / (VIEW_Y1 - VIEW_Y0));
        c.ox = a.x1 + w * 0.5f - c.s * (VIEW_X0 + VIEW_X1) * 0.5f;
        c.oy = a.y1 + h * 0.5f - c.s * (VIEW_Y0 + VIEW_Y1) * 0.5f;
    }
    return true;
}

void draw_cucumber(const Ctx &c)
{
    draw_cucumber_back(c);
    draw_cucumber_front(c);
}

void stage_draw(lv_layer_t *layer)
{
    Ctx v;
    if (!stage_view(v)) return;
    Ctx c = draw_ctx(v, layer);
    if (s_stage == 1) {
        draw_potato_stage(c);
        return;
    }
    bool cuke = part_hit(c, DP_CUKE);
    draw_board(c);
    if (cuke) draw_cucumber_back(c);
    if (part_hit(c, DP_KNIFE)) draw_knife(c);
    if (cuke) draw_cucumber_front(c);
    draw_pile(c);
    if (part_hit(c, DP_FLY)) draw_flying(c);
}

void stage_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (obj != stage || !layer) return;
#ifdef KITCHEN_PROBE
    uint64_t t0 = kp_now();
    stage_draw(layer);
    s_kp.cur.draw_us += kp_now() - t0;
    s_kp.cur.draw_calls++;
#else
    stage_draw(layer);
#endif
}

// ---- Simulation -----------------------------------------------------------------

// What a step moved, so the frame only invalidates there (KF.6 / KF.8).
enum : unsigned {
    MV_KNIFE = 1,      // stage 1: the knife
    MV_CUKE = 2,       //          the cucumber (pushed forward, or cut)
    MV_FLY = 4,        //          slices in the air
    MV_LAND = 8,       //          a slice landed on the pile
    MV_POTATO = 16,    // stage 2: the crank (potato, peeler, live peel)
    MV_SHAVINGS = 32,  //          peels in the air
};

unsigned cucumber_step(float dt)
{
    unsigned mv = 0;

    // Crank: ease toward the wheel position, never more than half a stroke
    // per frame so a fast spin cannot skip a cut.
    float diff = s_game.phase_target - s_game.phase;
    if (fabsf(diff) > 0.001f) {
        float step = diff * std::min(1.0f, dt * 16.0f);
        if (fabsf(diff) < 0.02f) step = diff;
        step = std::max(-PERIOD * 0.45f, std::min(PERIOD * 0.45f, step));
        float before = s_game.phase;
        s_game.phase += step;
        mv |= MV_KNIFE;

        // A crossing of a stroke's bottom (phase = P/2 mod P) is the cut.
        float b0 = floorf((before - PERIOD * 0.5f) / PERIOD);
        float b1 = floorf((s_game.phase - PERIOD * 0.5f) / PERIOD);
        if (b0 != b1 && s_game.armed && !s_game.done && s_game.zf > T * 0.6f) {
            cut();
            mv |= MV_CUKE | MV_FLY;
        }

        if (!s_game.armed && tri_wave(s_game.phase) > 0.8f) {
            s_game.armed = true;
            if (s_game.pending_advance) {
                s_game.pending_advance = false;
                s_game.zf_target = T;
            }
        }
    }

    if (fabsf(s_game.zf_target - s_game.zf) > 0.001f) {
        s_game.zf += (s_game.zf_target - s_game.zf) * std::min(1.0f, dt * 14.0f);
        if (fabsf(s_game.zf_target - s_game.zf) < 0.005f) s_game.zf = s_game.zf_target;
        mv |= MV_CUKE;
    }

    for (int i = 0; i < s_game.cuts; i++) {
        Slice &s = s_game.slices[i];
        if (s.t < 1.0f) {
            s.t = std::min(1.0f, s.t + dt / 0.45f);
            mv |= s.t >= 1.0f ? MV_FLY | MV_LAND : MV_FLY;
        }
    }
    return mv;
}

unsigned potato_step(float dt)
{
    unsigned mv = 0;
    // Same easing as the knife; the crank only ever runs forward here.
    float diff = s_pot.phase_target - s_pot.phase;
    if (diff > 0.001f) {
        float step = diff * std::min(1.0f, dt * 16.0f);
        if (diff < 0.02f) step = diff;
        step = std::min(P_PERIOD * 0.45f, step);
        float before = s_pot.phase;
        s_pot.phase += step;
        mv |= MV_POTATO;

        // The blade leaving the bottom of strip k releases its peel.
        float off = P_PERIOD * P_PEEL_END;
        float b0 = floorf((before - off) / P_PERIOD), b1 = floorf((s_pot.phase - off) / P_PERIOD);
        if (b1 > b0 && b1 >= 0 && b1 < P_STRIPS) finish_strip((int)b1);

        if (!s_pot.done && s_pot.phase >= P_STRIPS * P_PERIOD - 0.01f) {
            s_pot.phase = s_pot.phase_target = P_STRIPS * P_PERIOD;
            s_pot.done = true;
            update_labels();
        }
    }

    int keep = 0;
    for (int i = 0; i < s_pot.n_sh; i++) {
        Shaving &s = s_pot.sh[i];
        s.t += dt;
        if (s.t < 1.6f) s_pot.sh[keep++] = s;
    }
    if (s_pot.n_sh) mv |= MV_SHAVINGS;   // one more frame to clear the last one
    s_pot.n_sh = keep;
    return mv;
}

// ---- Dirty rectangles (KF.6 / KF.8) ------------------------------------------
// A part's area is measured with the very code that draws it. A frame
// invalidates a moved part's old and new areas only; anything unsure
// invalidates the stage.

void (*const kPartDraw[DP_N])(const Ctx &) = {draw_knife, draw_cucumber, draw_flying, draw_peeler_now, draw_shavings};

// KF.22: every dirty area re-runs the stage's draw and its projections, so
// areas that nearly touch are worth merging: union <= 1.3 x their sum.
struct AreaList {
    lv_area_t a[8];
    int n = 0;
    void add(const lv_area_t &r)
    {
        if (area_empty(r)) return;
        if (n == 8) {   // can't happen with today's parts; never lose an area
            area_add(a[7], r);
            return;
        }
        a[n++] = r;
    }
    void flush()
    {
        for (bool again = true; again;) {
            again = false;
            for (int i = 0; i < n && !again; i++) {
                for (int j = i + 1; j < n && !again; j++) {
                    lv_area_t u = a[i];
                    area_add(u, a[j]);
                    if ((uint64_t)lv_area_get_size(&u) * 10 > ((uint64_t)lv_area_get_size(&a[i]) + lv_area_get_size(&a[j])) * 13) continue;
                    a[i] = u;
                    a[j] = a[--n];
                    again = true;
                }
            }
        }
        for (int i = 0; i < n; i++) lv_obj_invalidate_area(stage, &a[i]);
        n = 0;
    }
};

lv_area_t measure_part(const Ctx &v, int id)
{
    lv_area_t a = NO_AREA;
    kPartDraw[id](measure_ctx(v, &a));
    return a;
}

void invalidate_moved(unsigned mv)
{
    Ctx v;
    if (!stage_view(v)) {
        lv_obj_invalidate(stage);
        s_dirty.valid = false;
        return;
    }
    AreaList dirty;
    bool same = s_dirty.valid && s_dirty.stage == s_stage && s_dirty.s == v.s && s_dirty.ox == v.ox &&
                s_dirty.oy == v.oy;
    if (s_stage == 0) {
        lv_area_t a = pile_update(v);
        if (same) dirty.add(a);
    } else {
        PotPose q = pot_pose();
        float pc[P_STRIPS];
        pot_peel(q, pc);
        lv_area_t a = update_potato(v, q, pc);
        if (same) dirty.add(a);
    }
    if (!same) {
        lv_obj_invalidate(stage);
        s_dirty.valid = true;
        s_dirty.stage = s_stage;
        s_dirty.s = v.s, s_dirty.ox = v.ox, s_dirty.oy = v.oy;
        for (int id = 0; id < DP_N; id++) s_dirty.part[id] = NO_AREA;
        mv = ~0u;
    }
    auto part = [&](int id) {
        dirty.add(s_dirty.part[id]);
        s_dirty.part[id] = measure_part(v, id);
        dirty.add(s_dirty.part[id]);
    };
    if (s_stage == 0) {
        if (mv & MV_KNIFE) part(DP_KNIFE);
        if (mv & MV_CUKE) part(DP_CUKE);
        if (mv & MV_FLY) part(DP_FLY);
    } else {
        if (mv & MV_POTATO) part(DP_PEELER);
        if (mv & MV_SHAVINGS) part(DP_SHAVINGS);
    }
    dirty.flush();
}

#ifdef KITCHEN_PROBE
void bench_tick(float dt);
#endif

void frame_cb(lv_timer_t *)
{
    float dt = std::min(0.1f, lv_tick_elaps(s_last_tick) / 1000.0f);
    s_last_tick = lv_tick_get();
#ifdef KITCHEN_PROBE
    bench_tick(dt);
    kp_report_tick();
#endif
    unsigned mv = s_stage == 0 ? cucumber_step(dt) : potato_step(dt);
    if (!stage) return;
    if (mv || !s_dirty.valid) invalidate_moved(mv);
}

void crank(float notches)
{
    if (s_game.paused) return;
    if (s_stage == 1) {
        if (s_pot.done) return;
        bool first = s_pot.phase_target <= 0;
        float t = s_pot.phase_target + fabsf(notches);
        s_pot.phase_target = std::min({t, s_pot.phase + P_PERIOD * 2.0f, P_STRIPS * P_PERIOD});
        if (first) update_labels();
        return;
    }
    float t = s_game.phase_target + notches;
    // Don't queue more than two strokes of spin.
    float lim = PERIOD * 2.0f;
    s_game.phase_target = std::max(s_game.phase - lim, std::min(s_game.phase + lim, t));
}

// ---- Bench (KF.1) and emulator hooks (KF.2) --------------------------------

#ifdef KITCHEN_PROBE
constexpr uint32_t BENCH_SEED = 0x2545f491u;
constexpr float BENCH_NOTCHES_PER_S = 12.0f;

int kp_bucket()
{
    if (s_stage == 0) {
        if (s_game.done) return -1;
        return s_game.cuts <= 5 ? KB_EARLY_CHOP : s_game.cuts >= 35 ? KB_LATE_CHOP : -1;
    }
    if (s_pot.done) return -1;
    return pot_pose().ret > 0 ? KB_PEEL_TURN : KB_PEEL_STROKE;
}

#if defined(KITCHEN_BENCH) || !defined(ARDUINO)
void bench_begin()
{
    for (auto &b : s_kp.bench) b = KpStats{};
    s_kp.multi_cast_refrs = 0;
    s_kp.bench_on = true;
    s_kp.bench_finished = false;
    s_game.rng = BENCH_SEED;
    new_game();
    s_game.paused = false;
    printf("[kitchen] bench: start (%.0f notches/s)\n", BENCH_NOTCHES_PER_S);
}
#endif

// Crank at a fixed rate through both stages, then print the totals.
void bench_tick(float dt)
{
    if (!s_kp.bench_on) return;
    if (s_stage == 0 && s_game.done) {
        bool landed = true;
        for (int i = 0; i < s_game.cuts; i++) landed &= s_game.slices[i].t >= 1.0f;
        if (landed) start_potato();
        return;
    }
    if (s_stage == 1 && s_pot.done) {
        s_kp.bench_on = false;
        s_kp.bench_finished = true;
        for (int b = 0; b < KB_N; b++) {
            char what[40];
            snprintf(what, sizeof(what), "bench %s", kBenchNames[b]);
            s_kp.bench[b].print(what);
        }
        printf("[kitchen] bench: peeler sprites: %u baked, %u misses, %d kept, %u KB\n", (unsigned)s_py.bakes, (unsigned)s_py.misses, s_py.n, (unsigned)(s_py.bytes / 1024));
        printf("[kitchen] bench: %u refreshes followed more than one cast\n", (unsigned)s_kp.multi_cast_refrs);
        kp_print_heap();
        printf("[kitchen] bench: done\n");
        return;
    }
    s_game.paused = false;
    crank(BENCH_NOTCHES_PER_S * dt);
}
#endif

void do_exit() { menu_show_deferred(); }
void back_cb(lv_event_t *) { do_exit(); }

void set_paused(bool p)
{
    s_game.paused = p;
    update_labels();
}

void stage_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_group_t *g = lv_obj_get_group(stage);
    if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);
        switch (key) {
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN: crank(1); break;
        case LV_KEY_LEFT:
        case LV_KEY_UP: crank(-1); break;
        case LV_KEY_ESC: do_exit(); return;
        case LV_KEY_ENTER:
            if (stage_done()) {
                next_stage();
            } else if (g && lv_indev_active() && lv_indev_get_type(lv_indev_active()) == LV_INDEV_TYPE_ENCODER) {
                // Leave edit mode so the wheel can move focus to Back.
                lv_group_set_editing(g, false);
            }
            break;
        default: return;
        }
        lv_event_stop_processing(e);
    } else if (code == LV_EVENT_FOCUSED) {
        // Edit mode is how the wheel reaches us; outside it we are paused.
        set_paused(g && !lv_group_get_editing(g));
    } else if (code == LV_EVENT_DEFOCUSED) {
        set_paused(true);
    } else if (code == LV_EVENT_PRESSING) {
        lv_indev_t *in = lv_indev_active();
        if (!in || lv_indev_get_type(in) != LV_INDEV_TYPE_POINTER) return;
        lv_point_t v;
        lv_indev_get_vect(in, &v);
        s_game.paused = false;
        crank(v.y / 8.0f);
    } else if (code == LV_EVENT_CLICKED) {
        lv_indev_t *in = lv_indev_active();
        if (stage_done() && in && lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER) next_stage();
    }
}

lv_obj_t *make_label(lv_obj_t *parent, lv_align_t align, int32_t x, int32_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_color(l, lv_color_hex(0xb8b8b8), 0);
    lv_obj_align(l, align, x, y);
    return l;
}

void ui_kitchen_enter(lv_obj_t *parent)
{
    if (stage) return;
    enable_keyboard();
    ui_show_back_button(back_cb);

    stage = lv_obj_create(parent);
    lv_obj_remove_style_all(stage);
    lv_obj_set_size(stage, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(stage, lv_color_hex(STAGE_BG), 0);   // the potato image is blended on it
    lv_obj_set_style_bg_opa(stage, LV_OPA_COVER, 0);
    // Scrollable (with nothing to scroll) makes the encoder treat the stage as
    // editable: a click enters edit mode, after which the wheel arrives here
    // as LEFT/RIGHT keys instead of moving focus.
    lv_obj_add_flag(stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scrollbar_mode(stage, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(stage, stage_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(stage, stage_event_cb, LV_EVENT_ALL, nullptr);

    lbl_title = make_label(stage, LV_ALIGN_TOP_LEFT, 6, 4);
    lbl_count = make_label(stage, LV_ALIGN_TOP_RIGHT, -6, 4);
    lbl_hint = make_label(stage, LV_ALIGN_TOP_LEFT, 6, 22);

    new_game();

    lv_group_t *g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, stage);
        lv_group_focus_obj(stage);
        lv_group_set_editing(g, true);
    }
    s_game.paused = false;
    update_labels();

    s_last_tick = lv_tick_get();
    frame_timer = lv_timer_create(frame_cb, KITCHEN_REFR_MS, nullptr);
#if KITCHEN_REFR_MS != LV_DEF_REFR_PERIOD
    if (lv_display_t *disp = lv_display_get_default()) lv_timer_set_period(lv_display_get_refr_timer(disp), KITCHEN_REFR_MS);
#endif
#ifdef KITCHEN_PROBE
    s_kp.win = KpStats{};
    s_kp.win_t0 = lv_tick_get();
    if (lv_display_t *disp = lv_display_get_default()) {
        lv_display_add_event_cb(disp, kp_refr_cb, LV_EVENT_REFR_START, nullptr);
        lv_display_add_event_cb(disp, kp_refr_cb, LV_EVENT_REFR_READY, nullptr);
#ifndef ARDUINO
        lv_display_add_event_cb(disp, kp_flush_cb, LV_EVENT_FLUSH_START, nullptr);
#endif
    }
#ifdef KITCHEN_BENCH
    bench_begin();
#endif
#endif

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button(back_cb, NULL);
#endif
}

void ui_kitchen_exit()
{
    ui_hide_back_button();
    disable_keyboard();
    if (frame_timer) {
        lv_timer_del(frame_timer);
        frame_timer = nullptr;
    }
#ifdef KITCHEN_PROBE
    if (lv_display_t *disp = lv_display_get_default()) {
        while (lv_display_remove_event_cb_with_user_data(disp, kp_refr_cb, nullptr)) {
        }
#ifndef ARDUINO
        while (lv_display_remove_event_cb_with_user_data(disp, kp_flush_cb, nullptr)) {
        }
#endif
    }
    s_kp.bench_on = false;
#endif
#if KITCHEN_REFR_MS != LV_DEF_REFR_PERIOD
    if (lv_display_t *disp = lv_display_get_default()) lv_timer_set_period(lv_display_get_refr_timer(disp), LV_DEF_REFR_PERIOD);
#endif
    if (lv_group_t *g = lv_group_get_default()) lv_group_set_editing(g, false);
    ui_app_widgets_del(&stage, &quit_btn);
    lbl_title = lbl_count = lbl_hint = nullptr;
    pot_free(s_pimg);
    pile_free();
    py_free();
    s_dirty.valid = false;
    s_stage = 0;
}

} // namespace

#include "app_registry.h"

namespace {
class KitchenApp : public core::App {
public:
    KitchenApp() : core::App("Kitchen") {}
    void onStart(lv_obj_t *parent) override { ui_kitchen_enter(parent); }
    void onStop() override
    {
        ui_kitchen_exit();
        core::App::onStop();
    }
};
} // namespace

namespace apps {
APP_FACTORY(make_kitchen_app, KitchenApp)
} // namespace apps

#ifndef ARDUINO
#include <vector>

namespace {
// Seeded states for the screenshot sweep (KF.2): crank at a steady rate
// until the state is reached, then stop the crank where it is.
constexpr float SIM_DT = 0.03f;

void sim(float notches, bool (*until)())
{
    s_game.paused = false;
    for (int guard = 0; guard < 20000 && !until(); guard++) {
        crank(notches);
        if (s_stage == 0) cucumber_step(SIM_DT);
        else potato_step(SIM_DT);
    }
}

void sim_settle(int frames)
{
    for (int i = 0; i < frames; i++) {
        if (s_stage == 0) cucumber_step(SIM_DT);
        else potato_step(SIM_DT);
    }
}

void hold_crank()
{
    s_game.phase_target = s_game.phase;
    s_pot.phase_target = s_pot.phase;
}

bool all_landed()
{
    for (int i = 0; i < s_game.cuts; i++)
        if (s_game.slices[i].t < 1.0f) return false;
    return true;
}

void seed_cucumber()
{
    s_game.rng = BENCH_SEED;
    new_game();
}

void seed_potato()
{
    seed_cucumber();
    start_potato();
}

const char *shot_state(int i)
{
    switch (i) {
    case 0:   // the whole pile, knife mid-stroke
        seed_cucumber();
        sim(0.5f, [] { return s_game.done && all_landed(); });
        crank(1.5f);
        sim_settle(40);
        return "pile";
    case 1:   // a slice in flight over a part pile
        seed_cucumber();
        sim(0.5f, [] { return s_game.cuts == 12; });
        hold_crank();
        sim(0, [] { return s_game.slices[11].t >= 0.35f; });
        return "flight";
    case 2:   // peeler halfway down strip 3
        seed_potato();
        sim(0.4f, [] {
            PotPose q = pot_pose();
            return q.k == 3 && q.pp >= 0.5f;
        });
        hold_crank();
        return "potato_stroke";
    case 3:   // halfway through the turn after strip 5
        seed_potato();
        sim(0.4f, [] {
            PotPose q = pot_pose();
            return q.k == 5 && q.ret >= 0.5f;
        });
        hold_crank();
        return "potato_turn";
    case 4:   // two peels in the air
        seed_potato();
        sim(1.2f, [] { return s_pot.n_sh == 2; });
        hold_crank();
        sim_settle(8);
        return "shavings";
    default: return nullptr;
    }
}
} // namespace

namespace kitchen {
int shot_count() { return 5; }

const char *shot_setup(int i)
{
    if (!stage || !frame_timer) return nullptr;
    lv_timer_pause(frame_timer);
    const char *name = shot_state(i);
    s_dirty.valid = false;   // the seeding bypassed the frames
    invalidate_moved(~0u);   // bakes and measures as a frame does
    return name;
}

void shot_release()
{
    if (frame_timer) lv_timer_resume(frame_timer);
}

void debug_crank(float notches)
{
    s_game.paused = false;
    crank(notches);
}

void debug_invalidate_all()
{
    if (stage) lv_obj_invalidate(stage);
}

// KF.4: each shortcut of the fast cast against the exact one, on the pose
// shown now: pixels that differ, the worst channel delta, and the time.
void cast_check()
{
    Ctx v;
    if (s_stage != 1 || !s_pimg.buf || !stage_view(v)) return;
    PotPose q = pot_pose();
    float pc[P_STRIPS];
    pot_peel(q, pc);
    int size = (int)s_pimg.buf->header.w;
    static const struct {
        const char *name;
        CastMode m;
    } modes[] = {
        {"exact", CAST_EXACT},          {"2x2 blocks (a)", {true, false, 3}}, {"2 refinements (b)", {false, false, 2}},
        {"rsqrt (c)", {false, true, 3}}, {"all (fast)", CAST_FAST},
    };
    std::vector<PotPixel> ref;
    for (const auto &md : modes) {
        PotImage t = {};
        t.x0 = s_pimg.x0, t.y0 = s_pimg.y0;
        t.buf = lv_draw_buf_create(size, size, POT_CF, 0);
        if (!t.buf) return;
        uint64_t t0 = kp_now();
        pot_full_cast(t, v, q, pc, size, md.m);
        uint64_t us = kp_now() - t0;
        uint32_t n = 0;
        int worst = 0;
        for (int j = 0; j < size; j++) {
            const PotPixel *row = (const PotPixel *)(t.buf->data + j * t.buf->header.stride);
            for (int i = 0; i < size; i++) {
                PotPixel a = row[i];
                if (ref.size() < (size_t)size * size) {
                    ref.push_back(a);
                    continue;
                }
                PotPixel b = ref[(size_t)j * size + i];
                if (a == b) continue;
                n++;
                // per 565 channel, scaled to 8 bits
                int d = std::max({abs((a >> 11) - (b >> 11)) * 8, abs(((a >> 5) & 63) - ((b >> 5) & 63)) * 4,
                                  abs((a & 31) - (b & 31)) * 8});
                worst = std::max(worst, d);
            }
        }
        printf("[kitchen] cast check %dpx, %-18s %6.1f ms, %u covered px, %u px differ (%.2f%%), worst channel delta %d\n",
               size, md.name, us / 1000.0f, (unsigned)t.n_px, (unsigned)n, 100.0f * n / (size * size), worst);
        pot_free(t);
    }
}

void bench_start() { bench_begin(); }
void probe_mute(bool on) { s_kp.muted = on; }
bool bench_done() { return s_kp.bench_finished; }
} // namespace kitchen
#endif

#ifdef KITCHEN_PROBE
namespace kitchen {
void note_flush(uint32_t px, uint32_t swap_us, uint32_t push_us, uint32_t blocked_us)
{
    if (s_kp.muted) return;
    s_kp.cur.flushes++;
    s_kp.cur.flush_px += px;
    s_kp.cur.swap_us += swap_us;
    s_kp.cur.push_us += push_us;
    s_kp.cur.blocked_us += blocked_us;
}
} // namespace kitchen
#endif
