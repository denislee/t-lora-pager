/**
 * @file      ui_kitchen.cpp
 * @brief     Kitchen: a vegetable-prep game. Stage 1 chops a cucumber.
 *            Turning the wheel rocks a chef's knife on its belly; every
 *            full stroke cuts a slice that tumbles onto the board, and the
 *            cucumber is pushed forward for the next one.
 *
 * Rendering is a small hand-rolled 3D pass on one plain lv_obj
 * (LV_EVENT_DRAW_MAIN): an oblique projection of shaded low-poly cylinders
 * (cucumber, slices) plus outlined flat shapes (board, knife). Nothing is
 * buffered; the frame timer only invalidates while something moves.
 *
 * Input: wheel / arrow keys crank the knife (either direction chops),
 * touch drag does the same, click pauses (wheel then reaches Back) or
 * restarts once the cucumber is done, ESC leaves.
 *
 * State reset on onStop (ui_kitchen_exit):
 *   widgets: stage (labels are its children), quit_btn — deleted, nulled
 *   timers:  frame_timer                               — lv_timer_del + null
 *   state:   s_game — rebuilt by new_game() on the next enter; the group's
 *            edit mode is switched back off so the next app starts in nav.
 * No tasks.
 */
#include "../ui_define.h"

#include <algorithm>
#include <cmath>

// lv_draw_triangle_dsc_t renamed bg_color -> color after 9.2 (the emulator
// pins 9.2.2, hardware 9.5.0).
#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR < 3
#define KT_TRI_COLOR(d) (d).bg_color
#else
#define KT_TRI_COLOR(d) (d).color
#endif

namespace {

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

struct Ctx {
    lv_layer_t *layer;
    float ox, oy, s;
};

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
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.p[0] = a;
    t.p[1] = b;
    t.p[2] = d;
#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR < 3
    fix_vertical_side(t.p);
#endif
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
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.p1 = a;
    l.p2 = b;
    l.color = col;
    l.width = w;
    l.round_start = 1;
    l.round_end = 1;
    lv_draw_line(c.layer, &l);
}

void line3(const Ctx &c, V3 a, V3 b, lv_color_t col, int w) { line2(c, proj(c, a), proj(c, b), col, w); }

void dot2(const Ctx &c, lv_point_precise_t p, int rad, lv_color_t col)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = col;
    d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = {(int32_t)p.x - rad, (int32_t)p.y - rad, (int32_t)p.x + rad, (int32_t)p.y + rad};
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
    lv_point_precise_t p0[SEGS], p1[SEGS];
    float cs[SEGS], sn[SEGS];
    for (int i = 0; i < SEGS; i++) {
        float th = i * 6.2831853f / SEGS;
        cs[i] = cosf(th);
        sn[i] = sinf(th);
        p0[i] = proj(c, c0 + (u * cs[i] + v * sn[i]) * r0);
        p1[i] = proj(c, c1 + (u * cs[i] + v * sn[i]) * r1);
    }
    for (int i = 0; i < SEGS; i++) {
        int j = (i + 1) % SEGS;
        V3 n = norm(norm(u * (cs[i] + cs[j]) + v * (sn[i] + sn[j])) + a * k);
        if (dot(n, kView) <= 0) continue;
        lv_color_t col = skin_shade(n);
        if (i & 1) col = lv_color_darken(col, 26);   // fine ridges along the skin
        tri_solid(c, p0[i], p0[j], p1[j], col);
        tri_solid(c, p0[i], p1[j], p1[i], col);
    }
    if (face1 && dot(a, kView) > 0) draw_face(c, c1, u, v, r1, p1);
    if (face0 && dot(a, kView) < 0) draw_face(c, c0, u, v, r0, p0);
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
    lv_label_set_text_fmt(lbl_count, "%d / %d", s_game.cuts, MAX_SLICES);
    const char *hint;
    if (s_game.done) hint = "All chopped! Click for another cucumber";
    else if (s_game.paused) hint = "Paused - click to resume";
    else if (s_game.cuts == 0) hint = "Turn the wheel to chop";
    else hint = "";
    lv_label_set_text(lbl_hint, hint);
}

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
    update_labels();
    if (stage) lv_obj_invalidate(stage);
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
        lv_draw_rect(c.layer, &d, &a);
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

// Painter's order for the landed pile: a slice resting on another must come
// after it; otherwise the one nearer the viewer (on the board) goes last.
// Slices still in the air are drawn on top of everything.
void draw_slices(const Ctx &c)
{
    const int n = s_game.cuts;
    V3 ctr[MAX_SLICES], ax[MAX_SLICES];
    bool placed[MAX_SLICES] = {};
    for (int i = 0; i < n; i++) slice_pose(s_game.slices[i], ctr[i], ax[i]);
    auto landed = [&](int i) { return s_game.slices[i].t >= 1.0f; };
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
        placed[best] = true;
        draw_one_slice(c, s_game.slices[best], ctr[best], ax[best]);
    }
    for (int i = 0; i < n; i++)
        if (!landed(i)) draw_one_slice(c, s_game.slices[i], ctr[i], ax[i]);
}

void stage_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!obj || !layer) return;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    float w = (float)lv_area_get_width(&a), h = (float)lv_area_get_height(&a);
    Ctx c;
    c.layer = layer;
    c.s = std::min(w / (VIEW_X1 - VIEW_X0), h / (VIEW_Y1 - VIEW_Y0));
    c.ox = a.x1 + w * 0.5f - c.s * (VIEW_X0 + VIEW_X1) * 0.5f;
    c.oy = a.y1 + h * 0.5f - c.s * (VIEW_Y0 + VIEW_Y1) * 0.5f;

    draw_board(c);
    draw_cucumber_back(c);
    draw_knife(c);
    draw_cucumber_front(c);
    draw_slices(c);
}

// ---- Simulation -----------------------------------------------------------------

void frame_cb(lv_timer_t *)
{
    float dt = std::min(0.1f, lv_tick_elaps(s_last_tick) / 1000.0f);
    s_last_tick = lv_tick_get();
    bool dirty = false;

    // Crank: ease toward the wheel position, never more than half a stroke
    // per frame so a fast spin cannot skip a cut.
    float diff = s_game.phase_target - s_game.phase;
    if (fabsf(diff) > 0.001f) {
        float step = diff * std::min(1.0f, dt * 16.0f);
        if (fabsf(diff) < 0.02f) step = diff;
        step = std::max(-PERIOD * 0.45f, std::min(PERIOD * 0.45f, step));
        float before = s_game.phase;
        s_game.phase += step;
        dirty = true;

        // A crossing of a stroke's bottom (phase = P/2 mod P) is the cut.
        float b0 = floorf((before - PERIOD * 0.5f) / PERIOD);
        float b1 = floorf((s_game.phase - PERIOD * 0.5f) / PERIOD);
        if (b0 != b1 && s_game.armed && !s_game.done && s_game.zf > T * 0.6f) cut();

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
        dirty = true;
    }

    for (int i = 0; i < s_game.cuts; i++) {
        Slice &s = s_game.slices[i];
        if (s.t < 1.0f) {
            s.t = std::min(1.0f, s.t + dt / 0.45f);
            dirty = true;
        }
    }

    if (dirty && stage) lv_obj_invalidate(stage);
}

void crank(float notches)
{
    if (s_game.paused) return;
    float t = s_game.phase_target + notches;
    // Don't queue more than two strokes of spin.
    float lim = PERIOD * 2.0f;
    s_game.phase_target = std::max(s_game.phase - lim, std::min(s_game.phase + lim, t));
}

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
            if (s_game.done) {
                new_game();
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
        if (s_game.done && in && lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER) new_game();
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
    lv_obj_set_style_bg_color(stage, lv_color_black(), 0);
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
    lv_label_set_text(lbl_title, "Stage 1: Cucumber");
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
    frame_timer = lv_timer_create(frame_cb, 30, nullptr);

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
    if (lv_group_t *g = lv_group_get_default()) lv_group_set_editing(g, false);
    ui_app_widgets_del(&stage, &quit_btn);
    lbl_title = lbl_count = lbl_hint = nullptr;
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
