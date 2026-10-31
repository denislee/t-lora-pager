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
 * is ray-cast per pixel into an ARGB image (a lumpy ellipsoid, flat colour
 * bands, antialiased edge); the peeler is a 3D slab drawn as line art.
 * Nothing is buffered; the frame timer only invalidates while something moves.
 *
 * Input: wheel / arrow keys crank the knife (either direction chops),
 * touch drag does the same, click pauses (wheel then reaches Back) or,
 * once a stage is done, moves on to the next (the potato loops back to the
 * cucumber), ESC leaves.
 *
 * State reset on onStop (ui_kitchen_exit):
 *   widgets: stage (labels are its children), quit_btn — deleted, nulled
 *   timers:  frame_timer                               — lv_timer_del + null
 *   state:   s_game — rebuilt by new_game() on the next enter; the group's
 *            edit mode is switched back off so the next app starts in nav.
 *            s_pot / s_stage — rebuilt by start_potato() / new_game().
 *   memory:  s_pimg.buf (the potato's ARGB image)       — lv_draw_buf_destroy + null
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
int s_stage = 0;   // 0 cucumber, 1 potato

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

// The potato is ray-cast into an ARGB image, redrawn only when the pose or
// the layout changes (a falling peel alone reuses it).
struct PotImage {
    lv_draw_buf_t *buf;
    float phase, s, ox, oy;
    int32_t x0, y0;
};

Potato s_pot;
PotImage s_pimg = {};

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
    s_pimg.phase = -1;   // fresh skin: redraw the image
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

lv_color_t pot_color(V3 u, V3 n, const float *pc, float w)
{
    static const float kFleshT[3] = {0.84f, 0.5f, 0.12f};
    static const float kSkinT[3] = {0.7f, 0.33f, 0.0f};
    float l = dot(n, kLightP);
    float ph = fast_atan2(u.z, u.x);
    if (pot_peeled(pc, u.y, ph)) return pot_band(C_PFLESH, kFleshT, 3, l, w);
    // Skin beside the flesh shows its cut edge, which gives the peel depth.
    float e = 0.055f / sqrtf(std::max(0.05f, 1.0f - u.y * u.y));
    if (pot_peeled(pc, u.y + 0.045f, ph) || pot_peeled(pc, u.y, ph + e) || pot_peeled(pc, u.y, ph - e)) return C_PRIM;
    // Blotches in the skin: a smooth pattern fixed to the potato.
    float blot = 0.18f * (2.0f * u.x * u.z + u.x * u.y - 1.2f * u.z * u.y * u.y + 0.5f * u.y);
    return pot_band(C_PSKIN, kSkinT, 3, l + blot, w);
}

void render_potato(const Ctx &c, const PotPose &q, const float *pc)
{
    int half = (int)ceilf(PRY * 1.25f * c.s) + 2;   // room for the lumps
    int size = 2 * half;
    PotImage &im = s_pimg;
    if (im.buf && (int)im.buf->header.w != size) {
        lv_draw_buf_destroy(im.buf);
        im.buf = nullptr;
    }
    if (!im.buf) {
        im.buf = lv_draw_buf_create(size, size, LV_COLOR_FORMAT_ARGB8888, 0);
        if (!im.buf) return;
        im.phase = -1;
    }
    if (im.phase == s_pot.phase && im.s == c.s && im.ox == c.ox && im.oy == c.oy) return;
    im.phase = s_pot.phase, im.s = c.s, im.ox = c.ox, im.oy = c.oy;
    im.x0 = (int32_t)floorf(c.ox + c.s * PCX) - half;
    im.y0 = (int32_t)floorf(c.oy - c.s * PCY) - half;

    // Orthographic rays down -z through each pixel centre, in unit-sphere
    // space u = D^-1 R^T (P - C): u(t) = a + t b with a linear in the pixel.
    PotXf xf(q.spin);
    V3 r0 = xf.rot({1, 0, 0}), r1 = xf.rot({0, 1, 0}), r2 = xf.rot({0, 0, 1});
    auto to_u = [&](V3 v) { return V3{dot(r0, v) / PRX, dot(r1, v) / PRY, dot(r2, v) / PRZ}; };
    V3 b = to_u({0, 0, 1});
    V3 ax = to_u({1.0f / c.s, 0, 0}), ay = to_u({0, -1.0f / c.s, 0});
    V3 a0 = to_u({(im.x0 + 0.5f - c.ox) / c.s - PCX, (c.oy - (im.y0 + 0.5f)) / c.s - PCY, 0});
    float bb = dot(b, b);
    float w = 1.6f / (PRZ * c.s);   // lighting change across about a pixel
    // A pixel's size in unit-sphere space, across the ray.
    V3 axp = ax - b * (dot(ax, b) / bb), ayp = ay - b * (dot(ay, b) / bb);
    float pix = sqrtf(std::max(dot(axp, axp), dot(ayp, ayp)));

    uint32_t stride = im.buf->header.stride;
    for (int j = 0; j < size; j++) {
        lv_color32_t *row = (lv_color32_t *)(im.buf->data + j * stride);
        V3 a = a0 + ay * (float)j;
        for (int i = 0; i < size; i++, a = a + ax) {
            // Closest approach of the ray to the centre; the surface radius
            // in that direction decides the silhouette (antialiased).
            float ts = -dot(a, b) / bb;
            V3 pm = a + b * ts;
            float dm2 = dot(pm, pm), dm = sqrtf(dm2);
            if (dm > 1.3f) {
                row[i] = {0, 0, 0, 0};
                continue;
            }
            V3 g;
            float rho = 1.0f + pot_bump(dm > 1e-6f ? pm * (1.0f / dm) : V3{0, 0, 1}, g);
            float cov = (rho - dm) / pix + 0.5f;
            if (cov <= 0) {
                row[i] = {0, 0, 0, 0};
                continue;
            }
            // Front hit: intersect the sphere of the local radius, refine.
            V3 u = pm, un = u;
            for (int it = 0; it < 3; it++) {
                u = a + b * (ts + sqrtf(std::max(0.0f, rho * rho - dm2) / bb));
                un = norm(u);
                rho = 1.0f + pot_bump(un, g);
            }
            // Normal: the sphere's, tilted by the bump's slope, then through D.
            V3 ns = norm(un - (g - un * dot(g, un)));
            V3 nm = norm({ns.x / PRX, ns.y / PRY, ns.z / PRZ});
            V3 n = r0 * nm.x + r1 * nm.y + r2 * nm.z;
            lv_color_t col = pot_color(un, n, pc, w);
            row[i] = {col.blue, col.green, col.red, (uint8_t)(std::min(1.0f, cov) * 255)};
        }
    }
    lv_image_cache_drop(im.buf);
}

void draw_potato(const Ctx &c, const PotPose &q, const float *pc)
{
    render_potato(c, q, pc);
    if (!s_pimg.buf) return;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = s_pimg.buf;
    int32_t sz = (int32_t)s_pimg.buf->header.w;
    lv_area_t a = {s_pimg.x0, s_pimg.y0, s_pimg.x0 + sz - 1, s_pimg.y0 + sz - 1};
    lv_draw_image(c.layer, &d, &a);

    // Eyes, on the skin that is left.
    PotXf xf(q.spin);
    int r = std::max(1, (int)lroundf(c.s * 0.035f));
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
void draw_ribbon(const Ctx &c, const Ribbon &r)
{
    lv_point_precise_t L[SH_PTS], R[SH_PTS];
    for (int i = 0; i < SH_PTS; i++) {
        L[i] = proj_o(c, r.l[i]);
        R[i] = proj_o(c, r.r[i]);
    }
    for (int i = 0; i + 1 < SH_PTS; i++) {
        V3 n = norm(r.nrm[i] + r.nrm[i + 1]);
        float l = dot(n, kLightP);
        lv_color_t col;
        if (n.z >= 0) col = l > 0.55f ? C_PSKIN[0] : l > 0.15f ? C_PSKIN[1] : l > -0.2f ? C_PSKIN[2] : C_PSKIN[3];
        else col = -l > 0.3f ? C_PFLESH[1] : C_PFLESH[2];
        tri_solid(c, L[i], R[i], R[i + 1], col);
        tri_solid(c, L[i], R[i + 1], L[i + 1], col);
    }
    for (int i = 0; i + 1 < SH_PTS; i++) {
        line2(c, L[i], L[i + 1], C_PRIM, 1);
        line2(c, R[i], R[i + 1], C_PRIM, 1);
    }
    line2(c, L[SH_PTS - 1], R[SH_PTS - 1], C_PRIM, 1);
    if (r.nrm[SH_PTS / 2].z > 0) {
        int er = std::max(1, (int)lroundf(c.s * 0.035f));
        for (int k = 0; k < r.n_eye; k++) dot2(c, proj_o(c, r.eye[k]), er, C_PEYE);
    }
}

V3 rodrigues(V3 v, V3 k, float ca, float sa) { return v * ca + cross(k, v) * sa + k * (dot(k, v) * (1 - ca)); }

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
        draw_ribbon(c, r);
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

// The blade rides the stroke on the skin; the handle sticks out away from
// the potato, sloping down, its plane rolled toward us so its edge shows.
void draw_peeler(const Ctx &c, const PotPose &q)
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

    // Peeler frame in view space (y up): roll about the handle, then pitch
    // the handle away from us.
    V3 h0 = {h.x, -h.y, 0}, v0 = {h.y, h.x, 0}, w0 = {0, 0, 1};
    if (v0.x * t.x - v0.y * t.y < 0) v0 = v0 * -1.0f;
    float roll = 0.45f + 0.2f * sinf(P_PI * q.pp), pitch = 0.3f;
    V3 vv = v0 * cosf(roll) + w0 * sinf(roll), w1 = w0 * cosf(roll) - v0 * sinf(roll);
    V3 hh = h0 * cosf(pitch) + w1 * sinf(pitch), ww = w1 * cosf(pitch) - h0 * sinf(pitch);
    float off = lift + 0.05f;
    V3 o = cv + V3{n.x, -n.y, 0} * off + V3{0, 0, 1.0f};
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

    fill_face(bp);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < pr.n; i++) {
            if (facing[i] != (pass == 1)) continue;
            int j = (i + 1) % pr.n;
            tri_solid(c, fp[i], fp[j], bp[j], black);
            tri_solid(c, fp[i], bp[j], bp[i], black);
        }
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

void draw_potato_stage(const Ctx &c)
{
    PotPose q = pot_pose();
    float pc[P_STRIPS];
    pot_peel(q, pc);
    draw_potato(c, q, pc);
    Ribbon rib;
    if (peel_ribbon(q, rib, false)) draw_ribbon(c, rib);
    draw_shavings(c);
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
    if (s_stage == 1) {
        // View bounds are y-up here (the cucumber's are projected, y-down).
        c.s = std::min(w / (PV_X1 - PV_X0), h / (PV_Y1 - PV_Y0));
        c.ox = a.x1 + w * 0.5f - c.s * (PV_X0 + PV_X1) * 0.5f;
        c.oy = a.y1 + h * 0.5f + c.s * (PV_Y0 + PV_Y1) * 0.5f;
        draw_potato_stage(c);
        return;
    }
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

bool cucumber_step(float dt)
{
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
    return dirty;
}

bool potato_step(float dt)
{
    bool dirty = false;
    // Same easing as the knife; the crank only ever runs forward here.
    float diff = s_pot.phase_target - s_pot.phase;
    if (diff > 0.001f) {
        float step = diff * std::min(1.0f, dt * 16.0f);
        if (diff < 0.02f) step = diff;
        step = std::min(P_PERIOD * 0.45f, step);
        float before = s_pot.phase;
        s_pot.phase += step;
        dirty = true;

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
    if (s_pot.n_sh) dirty = true;   // one more frame to clear the last one
    s_pot.n_sh = keep;
    return dirty;
}

void frame_cb(lv_timer_t *)
{
    float dt = std::min(0.1f, lv_tick_elaps(s_last_tick) / 1000.0f);
    s_last_tick = lv_tick_get();
    bool dirty = s_stage == 0 ? cucumber_step(dt) : potato_step(dt);
    if (dirty && stage) lv_obj_invalidate(stage);
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
    if (s_pimg.buf) {
        lv_draw_buf_destroy(s_pimg.buf);
        s_pimg.buf = nullptr;
    }
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
