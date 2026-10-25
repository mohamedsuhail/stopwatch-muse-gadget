// Copyright (c) Meta Platforms, Inc. and affiliates.

/*
 * The avatar: a little toy astronaut.
 *
 * Shape: a big round white helmet with an orange visor rim, two cyan-tipped
 * antennae and orange ear discs with yellow centres. Behind the glass, a soft
 * beige face with bead eyes, rosy cheeks and a small smile. Below it, a chunky
 * orange suit with a cyan collar and waist band, a white chest panel with
 * coloured buttons and a rocket badge, a blue-and-white backpack, white-and-cyan
 * cuffs, orange gloves, white knee pads and orange boots.
 *
 * Personality: cheerful and curious, a bit bouncy; the antennae perk up when
 * it listens and wobble when it's happy.
 */

#include "muse_pixel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W MUSE_PX_W
#define H MUSE_PX_H
#define TAU 6.2831853f

/* ---------------------------------------------------------------------------
 * Palette
 * ------------------------------------------------------------------------- */

enum {
    C_BG = 0,
    C_OUT,       /* outline */
    C_OUT2,      /* shadow inside the visor rim */
    C_HD,        /* helmet white: dark ... */
    C_HM,
    C_HL,
    C_HH,        /* ... highlight (also pure white) */
    C_OD,        /* suit orange: dark ... */
    C_OM,
    C_OL,
    C_OH,        /* ... highlight */
    C_CY,        /* cyan trim */
    C_CYL,
    C_YEL,       /* yellow details */
    C_RIM,       /* state-tinted rim light */
    C_SKIND,     /* face shade and fluffy lining */
    C_SKIN,
    C_SKINL,
    C_IRIS,      /* bead eyes */
    C_BLUSH,
    C_MOUTH,
    C_TONGUE,
    C_G0,        /* state glow ramp, bright ... */
    C_G1,
    C_G2,
    C_G3,        /* ... deep */
    C_AURA1,
    C_AURA2,
    C_SPK,
    C_ACC,
    C_SHADOW,
    C_HEART,
    C_COUNT,
};

/* Shared roles. */
#define C_WHITE C_HH
#define C_SHINE C_HH
#define C_BROW C_MOUTH
#define C_BLUSHD C_BLUSH

typedef struct {
    float r, g, b;
} rgb_t;

/* Per-mode glow ramp (bright -> deep) and accent. */
typedef struct {
    uint32_t f[4];
    uint32_t acc;
} scheme_t;

static const scheme_t SCHEMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT]      = { { 0xffffff, 0xcfe0ff, 0x8fa8ff, 0x5a5fe0 }, 0xa9c0ff },
    [MUSE_MODE_IDLE]      = { { 0xf4e8ff, 0xc7a4ff, 0x9a6bff, 0x5b3fd9 }, 0xa77dff },
    [MUSE_MODE_LISTENING] = { { 0xe8faff, 0x8fdcff, 0x3fa2ff, 0x2a5bd7 }, 0x5cb8ff },
    [MUSE_MODE_THINKING]  = { { 0xffe6ff, 0xff9cf0, 0xd35bff, 0x7a2bd9 }, 0xe07bff },
    [MUSE_MODE_SPEAKING]  = { { 0xeafff4, 0x9ff5cf, 0x3fd9a0, 0x1f9a7a }, 0x6ff0bf },
    [MUSE_MODE_ERROR]     = { { 0xffd6d6, 0xff6b6b, 0xc7304a, 0x6b1a3a }, 0xff5c5c },
    [MUSE_MODE_OFF]       = { { 0xd8d4ff, 0x8f86d9, 0x5a4fb0, 0x2e2870 }, 0x7c72d0 },
};

/* White helmet, orange suit, cyan trim and a beige face. */
static const uint32_t FIXED[C_COUNT] = {
    [C_BG] = 0x000000,
    [C_OUT] = 0x2a1a14,
    [C_OUT2] = 0x8a3a14,
    [C_HD] = 0xa9b4bf,
    [C_HM] = 0xd3dae0,
    [C_HL] = 0xeef1f4,
    [C_HH] = 0xffffff,
    [C_OD] = 0xb8461a,
    [C_OM] = 0xe2621f,
    [C_OL] = 0xf7833a,
    [C_OH] = 0xffac6e,
    [C_CY] = 0x2f9cbc,
    [C_CYL] = 0x6cd2e4,
    [C_YEL] = 0xf6c51d,
    [C_SKIND] = 0xd2bb96,
    [C_SKIN] = 0xeedcbc,
    [C_SKINL] = 0xf8ecd6,
    [C_IRIS] = 0x120d0b,
    [C_BLUSH] = 0xf2a3a0,
    [C_MOUTH] = 0x3a1f1a,
    [C_TONGUE] = 0xe86a7a,
    [C_SHADOW] = 0x16101f,
    [C_HEART] = 0xff4f8b,
};

static rgb_t s_scheme[5];      /* live, blended: f0..f3, acc */
static bool s_scheme_init;
static uint16_t s_pal[C_COUNT];
static uint16_t s_pal_dim[C_COUNT];

static uint8_t s_fb[W * H];
static uint8_t s_mask[W * H];

static const uint8_t BAYER4[4][4] = {
    { 0, 8, 2, 10 },
    { 12, 4, 14, 6 },
    { 3, 11, 1, 9 },
    { 15, 7, 13, 5 },
};

/*
 * The per-pixel work is fixed point (Q12: ONE = 1.0), with a table for the
 * root: chips without an FPU (ESP32-C6) emulate float in software. At 64 px
 * the quantisation is invisible.
 */
#define Q 12
#define ONE (1 << Q)
#define QF(v) ((int32_t)((v) * ONE))
#define SQRT_LUT_N 1024                 /* indexed by x >> 2 */
static int16_t s_sqrt[SQRT_LUT_N + 1];

static void init_luts(void)
{
    for (int i = 0; i <= SQRT_LUT_N; i++) {
        s_sqrt[i] = (int16_t)(sqrtf((float)i / SQRT_LUT_N) * ONE);
    }
}

/* sqrt of x in [0, ONE] */
static inline int32_t sqrt_q(int32_t x)
{
    return s_sqrt[x >> 2];
}

static inline int32_t bayer_q(int x, int y)
{
    return BAYER4[y & 3][x & 3] * (ONE / 16) + ONE / 32;
}

static inline float bayer(int x, int y)
{
    return (BAYER4[y & 3][x & 3] + 0.5f) / 16.0f;
}

static inline rgb_t hex_rgb(uint32_t c)
{
    return (rgb_t){ (float)((c >> 16) & 0xff), (float)((c >> 8) & 0xff), (float)(c & 0xff) };
}

static inline rgb_t mix(rgb_t a, rgb_t b, float t)
{
    return (rgb_t){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

static inline rgb_t scale_rgb(rgb_t a, float k)
{
    return (rgb_t){ a.r * k, a.g * k, a.b * k };
}

static inline uint16_t to565(rgb_t c)
{
    int r = (int)(c.r + 0.5f), g = (int)(c.g + 0.5f), b = (int)(c.b + 0.5f);
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    return SCHEMES[mode < MUSE_MODE_COUNT ? mode : MUSE_MODE_IDLE].acc;
}

static void update_palette(const scheme_t *target, float dt)
{
    rgb_t tgt[5];
    for (int i = 0; i < 4; i++) {
        tgt[i] = hex_rgb(target->f[i]);
    }
    tgt[4] = hex_rgb(target->acc);

    float k = s_scheme_init ? 1.0f - expf(-dt * 7.0f) : 1.0f;
    for (int i = 0; i < 5; i++) {
        s_scheme[i] = mix(s_scheme[i], tgt[i], k);
    }
    s_scheme_init = true;

    rgb_t pal[C_COUNT];
    for (int i = 0; i < C_COUNT; i++) {
        pal[i] = hex_rgb(FIXED[i]);
    }
    rgb_t acc = s_scheme[4];
    pal[C_G0] = s_scheme[0];
    pal[C_G1] = s_scheme[1];
    pal[C_G2] = s_scheme[2];
    pal[C_G3] = s_scheme[3];
    pal[C_ACC] = acc;
    pal[C_RIM] = mix(pal[C_HL], acc, 0.45f);
    pal[C_AURA1] = scale_rgb(acc, 0.16f);
    pal[C_AURA2] = scale_rgb(acc, 0.34f);
    pal[C_SPK] = mix(acc, pal[C_WHITE], 0.45f);

    for (int i = 0; i < C_COUNT; i++) {
        s_pal[i] = to565(pal[i]);
        /* The block edge shade gives the enlarged pixels a faint grid texture. */
        s_pal_dim[i] = to565(scale_rgb(pal[i], 0.72f));
    }
}

/* ---------------------------------------------------------------------------
 * Primitive helpers
 * ------------------------------------------------------------------------- */

static inline void px(int x, int y, uint8_t c)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        s_fb[y * W + x] = c;
    }
}

static inline uint8_t get_px(int x, int y)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        return s_fb[y * W + x];
    }
    return C_BG;
}

static inline void mark(int x, int y, uint8_t c, uint8_t m)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        s_fb[y * W + x] = c;
        s_mask[y * W + x] = m;
    }
}

static inline int iround(float v)
{
    return (int)floorf(v + 0.5f);
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float fracf(float v)
{
    return v - floorf(v);
}

/* Cheap deterministic pseudo-random for idle behaviour. */
static uint32_t s_rng = 0x9e3779b9u;
static float frand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng & 0xffffff) / (float)0x1000000;
}

/* Draw a small bitmap given as rows of '.'/'#'/'o' characters. */
static void stamp(const char *const *rows, int nrows, int x0, int y0, uint8_t fill, uint8_t alt)
{
    for (int r = 0; r < nrows; r++) {
        for (int c = 0; rows[r][c]; c++) {
            char ch = rows[r][c];
            if (ch == '#') {
                px(x0 + c, y0 + r, fill);
            } else if (ch == 'o') {
                px(x0 + c, y0 + r, alt);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Idle behaviour: blinking and gaze
 * ------------------------------------------------------------------------- */

typedef struct {
    float next_blink;
    float blink_start;
    float next_gaze;
    float gx, gy;          /* current gaze, -1..1 */
    float tgx, tgy;        /* target gaze */
    float last_t;
} eyes_t;

static eyes_t s_eyes = { .next_blink = 1.5f, .blink_start = -10, .next_gaze = 1.0f };

static float eyes_update(const muse_pose_t *p, float dt)
{
    eyes_t *e = &s_eyes;

    if (p->t >= e->next_blink) {
        e->blink_start = p->t;
        /* Occasionally double-blink. */
        e->next_blink = p->t + (frand() < 0.2f ? 0.28f : 2.2f + frand() * 3.0f);
    }

    if (p->t >= e->next_gaze) {
        e->next_gaze = p->t + 1.2f + frand() * 2.4f;
        if (frand() < 0.35f) {
            e->tgx = 0;
            e->tgy = 0;
        } else {
            e->tgx = frand() * 2 - 1;
            e->tgy = (frand() * 2 - 1) * 0.6f;
        }
    }

    float tgx = e->tgx, tgy = e->tgy;
    switch (p->mode) {
    case MUSE_MODE_LISTENING:
        tgx = 0;
        tgy = 0.1f;
        break;
    case MUSE_MODE_THINKING:
        tgx = 0.75f * sinf(p->mode_t * 1.3f) + 0.25f;
        tgy = -0.85f;
        break;
    case MUSE_MODE_SPEAKING:
        tgx *= 0.3f;
        tgy = 0;
        break;
    default:
        break;
    }
    float k = 1.0f - expf(-dt * 14.0f);
    e->gx += (tgx - e->gx) * k;
    e->gy += (tgy - e->gy) * k;

    /* Blink curve: 0 = open, 1 = shut. */
    float bt = (p->t - e->blink_start) / 0.16f;
    if (bt < 0 || bt > 1) {
        return 0;
    }
    return 1.0f - fabsf(bt * 2 - 1);
}

/* ---------------------------------------------------------------------------
 * Scene layers
 * ------------------------------------------------------------------------- */

static void draw_aura(float cx, float cy, float radius, float strength)
{
    int x0 = (int)(cx - radius - 1), x1 = (int)(cx + radius + 1);
    int y0 = (int)(cy - radius - 1), y1 = (int)(cy + radius + 1);
    /* Distances in 1/16 px; the root of d2 / r2 comes from the table. */
    int32_t cx16 = (int32_t)(cx * 16), r2 = (int32_t)(radius * radius * 256);
    int32_t to_idx = (int32_t)((float)SQRT_LUT_N * 65536 / r2);
    int32_t str = QF(strength);
    for (int y = y0; y <= y1; y++) {
        int32_t dy = (int32_t)((y + 0.5f - cy) * 1.1f * 16);
        int32_t dy2 = dy * dy;
        if (dy2 >= r2) {
            continue;
        }
        for (int x = x0; x <= x1; x++) {
            int32_t dx = x * 16 + 8 - cx16;
            int32_t d2 = dx * dx + dy2;
            if (d2 >= r2) {
                continue;
            }
            int32_t i = ((ONE - s_sqrt[(d2 * to_idx) >> 16]) * str) >> Q;
            int32_t b = bayer_q(x, y);
            if (i > QF(0.55f) + ((b * QF(0.35f)) >> Q)) {
                px(x, y, C_AURA2);
            } else if (i > (b * QF(0.9f)) >> Q) {
                px(x, y, C_AURA1);
            }
        }
    }
}

/* Expanding dotted rings (listening / speaking). */
static void draw_rings(float cx, float cy, float t, float level, float speed)
{
    for (int k = 0; k < 2; k++) {
        float ph = fracf(t * speed + k * 0.5f);
        float r = 20 + ph * 11;
        float fade = (1 - ph) * (0.35f + level);
        int n = (int)(r * 2.2f);
        for (int i = 0; i < n; i++) {
            float a = i * TAU / n;
            int x = iround(cx + cosf(a) * r);
            int y = iround(cy + sinf(a) * r * 0.92f);
            if (get_px(x, y) == C_BG || get_px(x, y) == C_AURA1) {
                if (bayer(x, y) < fade) {
                    px(x, y, fade > 0.6f ? C_ACC : C_AURA2);
                }
            }
        }
    }
}

static void draw_shadow(float cx, float y, float half_w)
{
    for (int row = 0; row < 3; row++) {
        float hw = half_w * (row == 1 ? 1.0f : 0.72f);
        for (int x = iround(cx - hw); x <= iround(cx + hw); x++) {
            float edge = fabsf(x + 0.5f - cx) / hw;
            if (bayer(x, (int)y + row) > edge * 0.8f) {
                px(x, (int)y + row, C_SHADOW);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * The astronaut
 * ------------------------------------------------------------------------- */

/* Four tones from a fake surface normal: base, base+1 .. base+3 run dark to light. */
static uint8_t tone4(int32_t nx, int32_t ny, int x, int y, uint8_t base)
{
    int32_t r2 = (nx * nx + ny * ny) >> Q;
    int32_t nz = r2 >= ONE ? 0 : sqrt_q(ONE - r2);
    int32_t l = (-QF(0.40f) * nx - QF(0.50f) * ny + QF(0.76f) * nz) >> Q;
    int32_t b = bayer_q(x, y);
    int32_t lv = l + (((b - ONE / 2) * QF(0.24f)) >> Q);
    if (r2 > QF(0.80f) && ((nx * QF(0.6f) + ny * QF(0.8f)) >> Q) > QF(0.62f) && b < QF(0.45f)) {
        return C_RIM;
    }
    return (uint8_t)(base + (lv > QF(0.92f) ? 3 : lv > QF(0.60f) ? 2 : lv > QF(0.25f) ? 1 : 0));
}

/* Two-tone cyan trim. */
static inline uint8_t cyan(int32_t nx, int32_t ny, int x, int y)
{
    int32_t l = (-QF(0.45f) * nx - QF(0.6f) * ny) >> Q;
    return l + ((bayer_q(x, y) - ONE / 2) >> 2) > QF(0.05f) ? C_CYL : C_CY;
}

typedef struct {
    float x, y;
    float angle;    /* radians; positive tips the bottom outward to the right */
} limb_t;

/* An ellipse, set up for Q12 hit tests in its own rotated frame. */
typedef struct {
    int32_t x, y, c, s, inv_rx, inv_ry, r;
} ell_t;

static void ell_setup(float x, float y, float rx, float ry, float angle, ell_t *q)
{
    q->x = QF(x);
    q->y = QF(y);
    q->c = QF(cosf(angle));
    q->s = QF(sinf(angle));
    q->inv_rx = QF(1.0f / rx);
    q->inv_ry = QF(1.0f / ry);
    q->r = QF(rx > ry ? rx : ry);
}

/* Inside the ellipse? (lx, ly) are its normalised local coordinates. */
static inline bool in_ell(const ell_t *l, int32_t x, int32_t y, int32_t *lx, int32_t *ly)
{
    int32_t dx = x - l->x, dy = y - l->y;
    if (abs(dx) > l->r || abs(dy) > l->r) {
        return false;
    }
    int32_t u = ((((dx * l->c) >> Q) + ((dy * l->s) >> Q)) * l->inv_rx) >> Q;
    int32_t v = ((((dy * l->c) >> Q) - ((dx * l->s) >> Q)) * l->inv_ry) >> Q;
    *lx = u;
    *ly = v;
    return u * u + v * v <= ONE * ONE;
}

/* Rounded box |u|^4 + |v|^4, from coordinates already normalised (Q12). */
static inline int32_t box4(int32_t u, int32_t v)
{
    u = u > 3 * ONE ? 3 * ONE : u < -3 * ONE ? -3 * ONE : u;
    v = v > 3 * ONE ? 3 * ONE : v < -3 * ONE ? -3 * ONE : v;
    int32_t u2 = (u * u) >> Q, v2 = (v * v) >> Q;
    return ((u2 * u2) >> Q) + ((v2 * v2) >> Q);
}

/* Flat-sided rounded box |u|^6 + |v|^6, from normalised coordinates (Q12). */
static inline int32_t box6(int32_t u, int32_t v)
{
    u = u > 2 * ONE ? 2 * ONE : u < -2 * ONE ? -2 * ONE : u;
    v = v > 2 * ONE ? 2 * ONE : v < -2 * ONE ? -2 * ONE : v;
    int32_t u2 = (u * u) >> Q, v2 = (v * v) >> Q;
    return ((((u2 * u2) >> Q) * u2) >> Q) + ((((v2 * v2) >> Q) * v2) >> Q);
}

/* A limb: a capsule from a (shoulder or hip) to b (hand or ankle), in 1/16 px. */
typedef struct {
    int32_t ax, ay, dx, dy, len2, r0, r1;
    int x0, x1, y0, y1;     /* bounding box, px */
} cap_t;

static void cap_setup(const float seg[4], float r0, float r1, cap_t *c)
{
    c->ax = (int32_t)(seg[0] * 16);
    c->ay = (int32_t)(seg[1] * 16);
    c->dx = (int32_t)((seg[2] - seg[0]) * 16);
    c->dy = (int32_t)((seg[3] - seg[1]) * 16);
    c->len2 = c->dx * c->dx + c->dy * c->dy;
    if (!c->len2) {
        c->len2 = 1;
    }
    c->r0 = (int32_t)(r0 * 16);
    c->r1 = (int32_t)(r1 * 16);
    float r = (r0 > r1 ? r0 : r1) + 1;
    c->x0 = (int)floorf(fminf(seg[0], seg[2]) - r);
    c->x1 = (int)ceilf(fmaxf(seg[0], seg[2]) + r);
    c->y0 = (int)floorf(fminf(seg[1], seg[3]) - r);
    c->y1 = (int)ceilf(fmaxf(seg[1], seg[3]) + r);
}

/* Inside the limb? *t runs 0 (a) to ONE (b); (*nx, *ny) is the normal across it, Q12. */
static inline bool in_cap(const cap_t *c, int x, int y, int32_t *t, int32_t *nx, int32_t *ny)
{
    if (x < c->x0 || x > c->x1 || y < c->y0 || y > c->y1) {
        return false;
    }
    int32_t px16 = x * 16 + 8 - c->ax, py16 = y * 16 + 8 - c->ay;
    int32_t dot = px16 * c->dx + py16 * c->dy;
    int32_t tq = dot <= 0 ? 0 : dot >= c->len2 ? ONE : (int32_t)(((int64_t)dot << Q) / c->len2);
    int32_t ex = px16 - ((c->dx * tq) >> Q), ey = py16 - ((c->dy * tq) >> Q);
    int32_t r = c->r0 + (((c->r1 - c->r0) * tq) >> Q);
    if (ex * ex + ey * ey > r * r) {
        return false;
    }
    *t = tq;
    *nx = (ex << Q) / r;
    *ny = (ey << Q) / r;
    return true;
}

typedef struct {
    float cx;                   /* centre line */
    float hx, hy, hrx, hry;     /* helmet */
    float fx, fy, fa, fb;       /* visor window (the face) */
    float tx, ty, ta, tb;       /* torso */
    float bx, by, ba, bb;       /* backpack */
    float kx, ky, krx, kry;     /* collar */
} astro_t;

/* M_SHOULDER: the root of an arm, joined to the torso without a seam. */
enum { M_NONE, M_BODY, M_HELM, M_FACE, M_ARM, M_SHOULDER, M_LEG, M_FOOT, M_PACK, M_EAR, M_ANT };

/* Antenna: a thin stalk from the helmet with a cyan ball on top. */
static void draw_antenna(float bx, float by, float angle, float len)
{
    float tx = bx + sinf(angle) * len, ty = by - cosf(angle) * len;
    int n = (int)(len * 2) + 1;
    for (int i = 0; i <= n; i++) {
        float k = (float)i / n;
        mark(iround(bx + (tx - bx) * k), iround(by + (ty - by) * k), C_HD, M_ANT);
    }
    static const char *const BALL[] = { ".##.", "#oo#", "#o##", ".##." };
    int x0 = iround(tx) - 2, y0 = iround(ty) - 2;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            char ch = BALL[r][c];
            if (ch != '.') {
                mark(x0 + c, y0 + r, ch == 'o' ? C_CYL : C_CY, M_BODY);
            }
        }
    }
    px(x0 + 1, y0 + 1, C_HH);   /* glint */
}

static void draw_astro(const astro_t *a, const float arms[2][4], const float legs[2][4], const limb_t feet[2],
                       const float ant[2][3])
{
    memset(s_mask, 0, sizeof(s_mask));

    /* Antennae first: the helmet covers their bases. */
    for (int i = 0; i < 2; i++) {
        draw_antenna(ant[i][0], ant[i][1], ant[i][2], 6.5f);
    }

    cap_t arm_q[2], leg_q[2];
    ell_t foot_q[2], ear_q[2], helm_q, collar_q;
    for (int i = 0; i < 2; i++) {
        cap_setup(arms[i], 3.0f, 3.4f, &arm_q[i]);
        cap_setup(legs[i], 4.3f, 3.8f, &leg_q[i]);
        ell_setup(feet[i].x, feet[i].y, 4.5f, 2.4f, feet[i].angle, &foot_q[i]);
        ell_setup(a->hx + (i ? 1 : -1) * a->hrx * 0.96f, a->fy + 0.5f, 3.2f, 3.4f, 0, &ear_q[i]);
    }
    ell_setup(a->hx, a->hy, a->hrx, a->hry, 0, &helm_q);
    ell_setup(a->kx, a->ky, a->krx, a->kry, 0, &collar_q);

    int32_t fcx = QF(a->fx), fcy = QF(a->fy), inv_fa = QF(1.0f / a->fa), inv_fb = QF(1.0f / a->fb);
    int32_t tcx = QF(a->tx), tcy = QF(a->ty), inv_tb = QF(1.0f / a->tb);
    int32_t bcx = QF(a->bx), bcy = QF(a->by), inv_ba = QF(1.0f / a->ba), inv_bb = QF(1.0f / a->bb);
    /* The chest panel, inside the torso's normalised frame. */
    int32_t pcy = -QF(0.20f), inv_pa = QF(1.0f / 0.46f), inv_pb = QF(1.0f / 0.30f);

    /* Only the box that can hold the astronaut is shaded. */
    int x0 = (int)(a->cx - 26), x1 = (int)(a->cx + 26);
    int y0 = (int)(a->hy - a->hry - 1), y1 = 60;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 >= W ? W - 1 : x1;

    for (int y = y0; y <= y1; y++) {
        int32_t fy = y * ONE + ONE / 2;
        /* The suit tapers: a little narrower at the shoulders than the hips. */
        float tvr = clampf((y + 0.5f - a->ty) / a->tb, -1, 1);
        int32_t inv_ta = QF(1.0f / (a->ta * (0.90f + 0.10f * tvr)));
        for (int x = x0; x <= x1; x++) {
            int32_t fx = x * ONE + ONE / 2;
            int32_t lx, ly;
            int32_t b = bayer_q(x, y);

            /* Arms in front of everything, from the shoulder: sleeve, cyan band, fluffy cuff, glove. */
            bool hit = false;
            for (int i = 0; i < 2 && !hit; i++) {
                int32_t at, nx, ny;
                if (in_cap(&arm_q[i], x, y, &at, &nx, &ny)) {
                    s_mask[y * W + x] = at < QF(0.30f) ? M_SHOULDER : M_ARM;
                    nx = (nx * QF(0.9f)) >> Q;
                    ny = ((ny * QF(0.9f)) >> Q) - QF(0.1f);
                    if (at > QF(0.80f)) {
                        uint8_t c = tone4(nx, ny, x, y, C_OD);
                        px(x, y, c == C_OH ? C_OL : c == C_OL ? C_OM : c);   /* gloves a shade deeper */
                    } else if (at > QF(0.70f)) {
                        px(x, y, b < QF(0.65f) ? C_HL : C_SKIND);
                    } else if (at > QF(0.60f)) {
                        px(x, y, cyan(nx, ny, x, y));
                    } else {
                        px(x, y, tone4(nx, ny, x, y, C_OD));
                    }
                    hit = true;
                }
            }
            if (hit) {
                continue;
            }

            /* Ear discs on the helmet's sides. */
            for (int i = 0; i < 2 && !hit; i++) {
                if (in_ell(&ear_q[i], fx, fy, &lx, &ly)) {
                    s_mask[y * W + x] = M_EAR;
                    int32_t d = (lx * lx + ly * ly) >> Q;
                    px(x, y, d < QF(0.30f) ? (lx + ly < -QF(0.3f) ? C_HH : C_YEL) : tone4(lx, ly, x, y, C_OD));
                    hit = true;
                }
            }
            if (hit) {
                continue;
            }

            /* Helmet: a white sphere with the visor window cut in. */
            if (in_ell(&helm_q, fx, fy, &lx, &ly)) {
                int32_t fu = ((fx - fcx) * inv_fa) >> Q, fv = ((fy - fcy) * inv_fb) >> Q;
                int32_t ff = box4(fu, fv);
                if (ff <= ONE) {
                    s_mask[y * W + x] = M_FACE;
                    uint8_t c = C_SKIN;
                    int32_t s = fu + fv;
                    if (fu < 0 && fv < -QF(0.15f) && s > -QF(1.30f) && s < -QF(0.98f) && b < QF(0.75f)) {
                        c = C_HH;     /* glint on the glass */
                    } else if (ff > QF(0.60f) + ((b * QF(0.30f)) >> Q)) {
                        c = C_SKIND;  /* fluffy lining around the window */
                    } else if (fv < -QF(0.45f) && b < ((-fv - QF(0.35f)) * QF(1.6f)) >> Q) {
                        c = C_SKIND;  /* the helmet shades the top of the face */
                    } else if (((fu * fu) >> Q) + (((fv + QF(0.15f)) * (fv + QF(0.15f))) >> Q) < QF(0.25f) &&
                               b < QF(0.5f)) {
                        c = C_SKINL;
                    }
                    px(x, y, c);
                } else if (ff <= QF(2.0f)) {
                    s_mask[y * W + x] = M_HELM;
                    /* Orange visor rim, darker just inside its lip. */
                    px(x, y, ff < QF(1.16f) ? C_OUT2 : tone4(lx, ly, x, y, C_OD));
                } else {
                    s_mask[y * W + x] = M_HELM;
                    px(x, y, tone4(lx, ly, x, y, C_HD));
                }
                continue;
            }

            /* Cyan collar ring under the helmet. */
            if (in_ell(&collar_q, fx, fy, &lx, &ly)) {
                s_mask[y * W + x] = M_BODY;
                px(x, y, cyan(lx, ly, x, y));
                continue;
            }

            /* Torso: a flat-sided suit, lit like a cylinder, with chest panel, seam and waist band. */
            int32_t tu = ((fx - tcx) * inv_ta) >> Q, tv = ((fy - tcy) * inv_tb) >> Q;
            if (box6(tu, tv) <= ONE) {
                s_mask[y * W + x] = M_BODY;
                int32_t pu = (tu * inv_pa) >> Q, pv = ((tv - pcy) * inv_pb) >> Q;
                if (box4(pu, pv) <= ONE) {
                    px(x, y, pu + pv < -QF(0.2f) && b < QF(0.6f) ? C_HH : (pv > QF(0.6f) ? C_HM : C_HL));
                } else if (tv > QF(0.40f) && tv < QF(0.58f)) {
                    px(x, y, cyan(tu, -QF(0.3f), x, y));
                } else if (tv > QF(0.10f) && abs(fx - tcx) < QF(0.55f)) {
                    px(x, y, C_OD);   /* the suit's centre seam */
                } else {
                    px(x, y, tone4((tu * QF(0.9f)) >> Q, ((tv * QF(0.3f)) >> Q) - QF(0.15f), x, y, C_OD));
                }
                continue;
            }

            /* Boots, then pant legs out of the suit with white knee pads and a cyan band. */
            for (int i = 0; i < 2 && !hit; i++) {
                if (in_ell(&foot_q[i], fx, fy, &lx, &ly)) {
                    s_mask[y * W + x] = M_FOOT;
                    px(x, y, ly > QF(0.40f) ? C_HL : ly < -QF(0.25f) ? C_OL : C_OM);
                    hit = true;
                }
            }
            for (int i = 0; i < 2 && !hit; i++) {
                int32_t lt, nx, ny;
                if (in_cap(&leg_q[i], x, y, &lt, &nx, &ny)) {
                    s_mask[y * W + x] = M_LEG;
                    if (abs(nx) < QF(0.72f) && lt > QF(0.42f) && lt < QF(0.70f)) {
                        px(x, y, lt < QF(0.52f) && b < QF(0.6f) ? C_HH : C_HL);
                    } else if (lt > QF(0.80f) && lt < QF(0.95f)) {
                        px(x, y, cyan(nx, -QF(0.3f), x, y));
                    } else {
                        px(x, y, tone4(nx, -QF(0.15f), x, y, C_OD));
                    }
                    hit = true;
                }
            }
            if (hit) {
                continue;
            }

            /* Backpack peeking out behind the left shoulder: blue lid, white box. */
            int32_t bu = ((fx - bcx) * inv_ba) >> Q, bv = ((fy - bcy) * inv_bb) >> Q;
            if (box4(bu, bv) <= ONE) {
                s_mask[y * W + x] = M_PACK;
                px(x, y, bv < -QF(0.40f) ? cyan(bu, bv, x, y) : tone4((bu * QF(0.7f)) >> Q, (bv * QF(0.5f)) >> Q, x, y, C_HD));
            }
        }
    }

    /* Orange nubs where the antennae meet the helmet. */
    for (int i = 0; i < 2; i++) {
        int nx = iround(ant[i][0]), ny = iround(ant[i][1]);
        mark(nx - 1, ny, C_OM, M_HELM);
        mark(nx, ny, C_OL, M_HELM);
        mark(nx - 1, ny + 1, C_OD, M_HELM);
        mark(nx, ny + 1, C_OM, M_HELM);
    }

    /* Hard outline on the silhouette, and seams where parts overlap. */
    static const int8_t N4[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t m = s_mask[y * W + x];
            if (m == M_NONE || m == M_FACE || m == M_ANT) {
                continue;
            }
            /* A shoulder belongs to the torso: outlined only against the background. */
            for (int k = 0; k < 4; k++) {
                int xx = x + N4[k][0], yy = y + N4[k][1];
                uint8_t n = ((unsigned)xx < W && (unsigned)yy < H) ? s_mask[yy * W + xx] : M_NONE;
                bool seam = n == M_NONE || n == M_ANT ||
                            (m == M_ARM && n != M_ARM && n != M_SHOULDER) ||
                            (m == M_EAR && n == M_HELM) ||
                            (m == M_HELM && (n == M_BODY || n == M_SHOULDER)) ||
                            ((m == M_BODY || m == M_SHOULDER) && (n == M_FOOT || n == M_PACK)) ||
                            (m == M_LEG && n == M_FOOT) ||
                            (m == M_PACK && (n == M_HELM || n == M_SHOULDER));
                if (seam) {
                    px(x, y, C_OUT);
                    break;
                }
            }
        }
    }

    /* Chest panel details: rocket badge, buttons and the yellow hose. */
    int cx = iround(a->tx), cy = iround(a->ty - a->tb * 0.16f);
    static const char *const BADGE[] = { ".##.", "#oo#", "#oo#", ".##." };
    stamp(BADGE, 4, cx - 4, cy - 2, C_CY, C_HH);
    px(cx - 3, cy - 1, C_OM);   /* the rocket */
    px(cx - 2, cy, C_OM);
    px(cx + 1, cy - 1, C_OM);
    px(cx + 2, cy - 1, C_OD);
    px(cx + 1, cy + 1, C_CY);
    px(cx + 3, cy + 1, C_YEL);
    px(cx + 3, cy + 2, C_YEL);
    int hose = iround(a->ty + a->tb * 0.30f);
    for (int x = cx - 4; x <= cx + 5; x++) {
        if (s_mask[hose * W + x] == M_BODY && get_px(x, hose) != C_OUT) {
            px(x, hose, (x & 1) ? C_YEL : (x < cx ? C_YEL : C_OL));
        }
    }
    /* Dials on the backpack. */
    int bxp = iround(a->bx - a->ba * 0.2f), byp = iround(a->by + a->bb * 0.05f);
    if (s_mask[byp * W + bxp] == M_PACK) {
        px(bxp, byp, C_YEL);
        px(bxp, byp + 3, C_CY);
        px(bxp + 2, byp + 3, C_OM);
    }
}

typedef enum {
    EYES_NORMAL,
    EYES_WIDE,
    EYES_HAPPY,
    EYES_X,
} eye_style_t;

/* The eyes are small glossy black beads. */
static void draw_eye(float ex, float ey, float openness, eye_style_t style, float gx, float gy)
{
    int cx = iround(ex + gx * 0.8f), cy = iround(ey + gy * 0.7f);

    if (style == EYES_HAPPY) {
        static const char *const HAPPY[] = { ".##.", "#..#" };
        stamp(HAPPY, 2, iround(ex) - 2, iround(ey), C_IRIS, C_IRIS);
        return;
    }
    if (style == EYES_X) {
        static const char *const XS[] = { "#..#", ".##.", ".##.", "#..#" };
        stamp(XS, 4, iround(ex) - 2, iround(ey) - 1, C_IRIS, C_IRIS);
        return;
    }
    if (openness < 0.3f) {
        static const char *const SHUT[] = { "#..#", ".##." };
        stamp(SHUT, 2, iround(ex) - 2, iround(ey) + 1, C_IRIS, C_IRIS);
        return;
    }

    static const char *const BEAD[] = { ".##.", "#o##", "####", ".##." };
    static const char *const BIG[] = { ".##.", "#o##", "#o##", "####", ".##." };
    const char *const *rows = style == EYES_WIDE ? BIG : BEAD;
    int n = style == EYES_WIDE ? 5 : 4;
    /* Lids close from the top: skip the upper rows as openness drops. */
    int skip = iround((1 - openness) * (n - 1));
    stamp(rows + skip, n - skip, cx - 2, cy - 2 + skip, C_IRIS, skip ? C_IRIS : C_SHINE);
}

static void draw_blush(int x, int y, float strength)
{
    static const char *const CHEEK[] = { ".##.", "####", ".##." };
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 4; i++) {
            if (CHEEK[j][i] != '#') {
                continue;
            }
            float b = bayer(x + i, y + j);
            if (b < strength) {
                px(x - 2 + i, y + j, (j == 1 && b < strength * 0.5f) ? C_BLUSHD : C_BLUSH);
            }
        }
    }
}

typedef enum {
    MOUTH_SMILE,
    MOUTH_O,
    MOUTH_HMM,
    MOUTH_TALK,
    MOUTH_GRIN,
    MOUTH_FLAT,
} mouth_t;

static void draw_mouth(int x, int y, mouth_t m, float open)
{
    switch (m) {
    case MOUTH_SMILE: {
        static const char *const S[] = { "#..#", ".##." };
        stamp(S, 2, x - 2, y, C_MOUTH, C_MOUTH);
        break;
    }
    case MOUTH_O: {
        static const char *const S[] = { ".##.", "#oo#", ".##." };
        stamp(S, 3, x - 2, y - 1, C_MOUTH, C_TONGUE);
        break;
    }
    case MOUTH_HMM: {
        static const char *const S[] = { "..#", "##." };
        stamp(S, 2, x - 1, y, C_MOUTH, C_MOUTH);
        break;
    }
    case MOUTH_TALK: {
        int h = 1 + iround(clampf(open, 0, 1) * 3.0f);
        int w = h > 2 ? 4 : 3;
        for (int j = 0; j < h; j++) {
            int inset = (j == 0 || j == h - 1) && h > 2 ? 1 : 0;
            for (int i = inset; i < w - inset; i++) {
                bool tongue = h >= 3 && j == h - 2 && i > inset && i < w - inset - 1;
                px(x - w / 2 + i, y + j, tongue ? C_TONGUE : C_MOUTH);
            }
        }
        break;
    }
    case MOUTH_GRIN: {
        static const char *const S[] = { "#####", ".#o#.", "..#.." };
        stamp(S, 3, x - 2, y, C_MOUTH, C_TONGUE);
        break;
    }
    case MOUTH_FLAT: {
        static const char *const S[] = { "##" };
        stamp(S, 1, x - 1, y + 1, C_MOUTH, C_MOUTH);
        break;
    }
    }
}

static void draw_sparkle(int x, int y, float twinkle, bool front)
{
    uint8_t arm = front ? C_ACC : C_AURA2;
    uint8_t core = front ? C_WHITE : C_SPK;
    if (twinkle > 0.8f) {
        px(x, y, core);
        for (int k = 1; k <= 2; k++) {
            uint8_t c = k == 1 ? (front ? C_SPK : arm) : arm;
            px(x + k, y, c);
            px(x - k, y, c);
            px(x, y + k, c);
            px(x, y - k, c);
        }
    } else if (twinkle > 0.45f) {
        px(x, y, front ? C_SPK : arm);
        px(x + 1, y, arm);
        px(x - 1, y, arm);
        px(x, y + 1, arm);
        px(x, y - 1, arm);
    } else if (twinkle > 0.15f) {
        px(x, y, arm);
    }
}

static void draw_sparkles(const muse_pose_t *p, float cx, float cy, bool front, float speed, int count)
{
    for (int i = 0; i < count; i++) {
        float a = p->t * speed + i * TAU / count;
        float s = sinf(a);
        if ((s > 0) != front) {
            continue;
        }
        float rr = 26.0f + 2.0f * sinf(i * 1.9f + p->t * 0.7f);
        int x = iround(cx + cosf(a) * rr);
        int y = iround(cy - 3 + s * rr * 0.42f);
        float tw = 0.5f + 0.5f * sinf(p->t * 5.0f + i * 1.7f);
        draw_sparkle(x, y, tw, front);
    }
}

/* Sound waves either side of the head. */
static void draw_waves(float cx, float cy, float body_rx, float level, float t)
{
    int n = 1 + (int)(clampf(level, 0, 1) * 3.2f);
    if (n > 3) {
        n = 3;
    }
    for (int k = 0; k < n; k++) {
        float r = body_rx + 5.0f + k * 3.0f;
        int span = 2 + k;
        float flick = 0.5f + 0.5f * sinf(t * 12.0f - k * 1.4f);
        for (int side = -1; side <= 1; side += 2) {
            for (int j = -span; j <= span; j++) {
                float xo = r - (float)(j * j) / (2.0f * r) * 3.0f;
                int x = iround(cx + side * xo);
                int y = iround(cy - 2 + j);
                if (k == 0 || bayer(x, y) < 0.35f + flick * 0.65f) {
                    px(x, y, k == 0 ? C_ACC : (k == 1 ? C_G1 : C_G2));
                }
            }
        }
    }
}

static void draw_thought_dots(float x, float y, float t)
{
    int active = (int)(fracf(t * 1.6f) * 3.0f);
    for (int i = 0; i < 3; i++) {
        int bx = iround(x + i * 4);
        int by = iround(y - i * 3) - (i == active ? 1 : 0);
        uint8_t c = i == active ? C_G0 : C_ACC;
        px(bx, by, c);
        px(bx + 1, by, c);
        px(bx, by + 1, c);
        px(bx + 1, by + 1, i == active ? C_G1 : C_G2);
    }
}

static void draw_hearts(float cx, float top, float t, float amount)
{
    static const char *const HEART[] = { ".#.#.", "#o###", "#####", ".###.", "..#.." };
    for (int i = 0; i < 2; i++) {
        float ph = fracf(t * 0.9f + i * 0.5f);
        if (ph > amount) {
            continue;
        }
        int hx = iround(cx + (i ? 13 : -18) + sinf(ph * TAU + i) * 2);
        int hy = iround(top - ph * 10);
        stamp(HEART, 5, hx, hy, C_HEART, C_WHITE);
    }
}

static void draw_alert(int x, int y)
{
    static const char *const BANG[] = { ".##.", ".##.", ".##.", ".##.", "....", ".##." };
    stamp(BANG, 6, x - 2, y, C_ACC, C_ACC);
}

/* ---------------------------------------------------------------------------
 * Frame
 * ------------------------------------------------------------------------- */

/*
 * Screen pixel -> grid cell, with 0x80 set on a cell's last screen pixel. When
 * cells are 3+ pixels, that edge is drawn dimmer so the pixel grid shows.
 */
#define MAP_MAX 512
static uint8_t s_map[MAP_MAX];
static int s_size;

void muse_pixel_set_size(int px)
{
    s_size = px < MAP_MAX ? px : MAP_MAX;
    bool grid = s_size >= 3 * W;
    for (int i = 0; i < s_size; i++) {
        int cell = i * W / s_size;
        bool edge = grid && (i + 1) * W / s_size != cell;
        s_map[i] = (uint8_t)(cell | (edge ? 0x80 : 0));
    }
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    const uint8_t *xmap = &s_map[x0];
    const uint16_t *prev = NULL;
    uint8_t prev_m = 0;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        uint8_t m = s_map[y];
        if (prev && m == prev_m) {
            memcpy(dst, prev, n * sizeof(uint16_t));
            continue;
        }
        const uint8_t *row = &s_fb[(m & 0x7f) * W];
        if (m & 0x80) {
            for (int i = 0; i < n; i++) {
                dst[i] = s_pal_dim[row[xmap[i] & 0x7f]];
            }
        } else {
            for (int i = 0; i < n; i++) {
                uint8_t xm = xmap[i];
                uint8_t c = row[xm & 0x7f];
                dst[i] = xm & 0x80 ? s_pal_dim[c] : s_pal[c];
            }
        }
        prev = dst;
        prev_m = m;
    }
}

void muse_pixel_render(const muse_pose_t *p)
{
    static bool s_luts;
    if (!s_luts) {
        init_luts();
        s_luts = true;
    }
    float dt = s_eyes.last_t > 0 ? clampf(p->t - s_eyes.last_t, 0, 0.2f) : 0.04f;
    s_eyes.last_t = p->t;

    muse_mode_t mode = p->mode;
    /* Powering down: the astronaut waves, then nods off as the glow fades. */
    float fade = mode == MUSE_MODE_OFF ? clampf(1.0f - p->mode_t / 1.3f, 0, 1) : 1.0f;
    float happy = mode == MUSE_MODE_ERROR ? 0.0f : p->happy;
    float level = p->level;
    float t = p->t;

    update_palette(&SCHEMES[mode], dt);
    float blink = eyes_update(p, dt);

    memset(s_fb, C_BG, sizeof(s_fb));

    /* ---- body motion ---- */
    float bob, lean = 0, hop = 0;
    switch (mode) {
    case MUSE_MODE_LISTENING:
        bob = sinf(t * 3.0f) * 0.6f;
        break;
    case MUSE_MODE_THINKING:
        bob = sinf(t * 2.4f) * 0.8f;
        lean = sinf(t * 1.3f) * 1.2f;
        break;
    case MUSE_MODE_SPEAKING:
        bob = sinf(t * 5.0f) * 0.6f - level * 1.5f;
        break;
    case MUSE_MODE_ERROR:
        bob = 1.0f;
        lean = sinf(t * 18.0f) * (p->mode_t < 0.6f ? 1.2f : 0.0f);
        break;
    default:
        bob = sinf(t * 1.8f) * 1.0f;
        break;
    }
    if (happy > 0) {
        hop = fabsf(sinf(t * 9.0f)) * 3.0f * happy;
    }

    /* Boot: the astronaut pops up from a squash, then opens their eyes. */
    float boot = mode == MUSE_MODE_BOOT ? clampf(p->mode_t / 1.4f, 0, 1) : 1.0f;
    float pop = mode == MUSE_MODE_BOOT ? clampf(p->mode_t / 0.6f, 0, 1) : 1.0f;
    float sy = 1.0f - (1.0f - pop) * 0.35f + sinf(pop * 3.1416f) * 0.06f;
    float sx = 2.0f - sy;
    float breathe = sinf(t * 2.0f + 1.0f) * 0.03f;

    /* Rest-pose y (feet at 56.5) -> this frame's y: squash toward the ground, bob and hop. */
    const float G = 59.0f;
    float lift = hop - bob * 0.5f;
#define YP(yr) (G - (56.5f - (yr)) * sy - lift)   /* sits 2.5 px lower than the rest pose */

    astro_t a;
    a.cx = 32.0f + lean;
    a.hx = a.cx + lean * 0.4f;
    a.hy = YP(20.0f) - breathe * 8.0f;
    a.hrx = 14.3f * sx * (1 + breathe * 0.5f);
    a.hry = 13.2f * sy;
    a.fx = a.hx + 0.3f;
    a.fy = a.hy + 1.6f * sy;
    a.fa = 8.6f * sx;
    a.fb = 7.3f * sy;
    a.tx = a.cx;
    a.ty = YP(42.8f);
    a.ta = 11.6f * sx * (1 + breathe) + level * 0.6f;
    a.tb = 9.6f * sy * (1 - breathe);
    a.kx = a.hx;
    a.ky = YP(33.6f) - breathe * 4.0f;
    a.krx = 9.8f * sx;
    a.kry = 2.4f * sy;
    a.bx = a.cx - 11.8f * sx;
    a.by = YP(37.0f);
    a.ba = 4.6f * sx;
    a.bb = 7.4f * sy;

    /* ---- background layers ---- */
    float aura_r = 29.0f + level * 4.0f + sinf(t * 1.5f) * 1.0f;
    float aura_s = (0.75f * boot + level * 0.4f) * fade;
    draw_aura(a.cx, 35.5f, aura_r, aura_s);
    if (mode == MUSE_MODE_LISTENING) {
        draw_rings(a.cx, a.fy + 2, t, level, 0.9f);
    } else if (mode == MUSE_MODE_SPEAKING) {
        draw_rings(a.cx, a.fy + 2, t, level, 0.6f);
    }
    draw_shadow(a.cx, 60.5f, 13.0f - hop * 0.8f);

    float spk_speed = mode == MUSE_MODE_THINKING ? 2.8f : mode == MUSE_MODE_LISTENING ? 1.2f
                    : mode == MUSE_MODE_SPEAKING ? 1.5f : 0.6f;
    int spk_count = mode == MUSE_MODE_BOOT ? (int)(boot * 6) : (int)(6 * fade);
    draw_sparkles(p, a.cx, 35.5f, false, spk_speed, spk_count);

    /* ---- legs and boots: pant legs from the hips, boots under them ---- */
    float step = mode == MUSE_MODE_SPEAKING ? sinf(t * 5.0f) * 0.6f : 0.0f;
    float kick = happy > 0 ? hop * 0.3f : 0.0f;
    float hip_y = a.ty + a.tb * 0.45f;
    float legs[2][4] = {
        { a.tx - 5.0f, hip_y, a.cx - 5.4f, YP(51.6f) + kick + step * 0.6f },
        { a.tx + 5.0f, hip_y, a.cx + 5.4f, YP(51.6f) + kick - step * 0.6f },
    };
    limb_t feet[2];
    feet[0] = (limb_t){ a.cx - 5.8f, YP(54.6f) + kick + step, -0.12f };
    feet[1] = (limb_t){ a.cx + 5.8f, YP(54.6f) + kick - step, 0.12f };

    /* ---- arms: each from a shoulder inside the suit out to its hand ---- */
    float shx = a.ta - 3.2f, shy = a.ty - a.tb + 3.6f;
    float hand[2][2];
    float rest_sway = sinf(t * 1.8f + 0.6f) * 0.8f;
    hand[0][0] = a.tx - a.ta - 1.8f - rest_sway;
    hand[0][1] = a.ty + 4.6f;
    hand[1][0] = a.tx + a.ta + 1.8f - rest_sway;
    hand[1][1] = a.ty + 4.6f;
    switch (mode) {
    case MUSE_MODE_LISTENING:
        /* Gloves up beside the helmet, like cupping an ear. */
        hand[0][0] = a.hx - a.hrx + 1.5f;
        hand[1][0] = a.hx + a.hrx - 1.5f;
        hand[0][1] = hand[1][1] = a.fy + 5.5f;
        break;
    case MUSE_MODE_THINKING:
        /* One glove up to the chin of the helmet. */
        hand[1][0] = a.cx + 4.5f;
        hand[1][1] = a.hy + a.hry + 0.5f;
        break;
    case MUSE_MODE_OFF: {
        /* Waving goodbye. */
        float w = sinf(t * 12.0f) * fade;
        hand[1][0] = a.tx + a.ta + 4.0f + w * 1.5f;
        hand[1][1] = shy - 8.5f;
        break;
    }
    case MUSE_MODE_SPEAKING: {
        float w = sinf(t * 7.0f) * (0.25f + level * 0.45f);
        hand[0][0] = a.tx - a.ta - 2.8f;
        hand[0][1] = a.ty + 3.0f - w * 6.0f;
        hand[1][0] = a.tx + a.ta + 2.8f;
        hand[1][1] = a.ty + 3.0f + w * 6.0f;
        break;
    }
    default:
        break;
    }
    if (happy > 0 && mode != MUSE_MODE_LISTENING && mode != MUSE_MODE_THINKING) {
        /* Arms up and wiggling. */
        float wig = sinf(t * 14.0f) * 1.2f;
        hand[0][0] = a.tx - a.ta - 3.5f + wig;
        hand[1][0] = a.tx + a.ta + 3.5f - wig;
        hand[0][1] = hand[1][1] = shy - 9.0f;
    }
    float arms[2][4] = {
        { a.tx - shx, shy, hand[0][0], hand[0][1] },
        { a.tx + shx, shy, hand[1][0], hand[1][1] },
    };

    /* ---- antennae: perk up when listening, wobble when happy ---- */
    float spread = 0.42f, wob = sinf(t * 2.2f) * 0.06f, wob2 = -wob;
    if (mode == MUSE_MODE_LISTENING) {
        spread = 0.22f + sinf(t * 9.0f) * 0.03f * (1 + level * 3);
        wob = wob2 = 0;
    } else if (mode == MUSE_MODE_THINKING) {
        wob = sinf(t * 3.0f) * 0.25f;
        wob2 = 0.15f;
    } else if (mode == MUSE_MODE_SPEAKING) {
        wob = sinf(t * 10.0f) * 0.08f * (0.5f + level);
        wob2 = -wob;
    } else if (mode == MUSE_MODE_ERROR || mode == MUSE_MODE_OFF) {
        spread = 0.42f + (mode == MUSE_MODE_ERROR ? 0.35f : 0.5f * (1 - fade));   /* droop */
    }
    if (happy > 0) {
        wob += sinf(t * 16.0f) * 0.3f * happy;
        wob2 -= sinf(t * 16.0f + 1.0f) * 0.3f * happy;
    }
    float ant[2][3] = {
        { a.hx - 5.4f * sx, a.hy - a.hry * 0.86f, -spread + wob },
        { a.hx + 5.4f * sx, a.hy - a.hry * 0.86f, spread + wob2 },
    };

    draw_astro(&a, (const float(*)[4])arms, (const float(*)[4])legs, feet, ant);

    /* ---- face ---- */
    float eye_y = a.fy - 0.8f;
    float eye_dx = a.fa * 0.40f;
    eye_style_t style = EYES_NORMAL;
    float open = 1.0f - blink;
    mouth_t mouth = MOUTH_SMILE;
    float mouth_open = 0;

    switch (mode) {
    case MUSE_MODE_BOOT:
        open = p->mode_t < 0.9f ? 0.0f : clampf((p->mode_t - 0.9f) / 0.3f, 0, 1);
        break;
    case MUSE_MODE_LISTENING:
        style = EYES_WIDE;
        mouth = MOUTH_O;
        break;
    case MUSE_MODE_THINKING:
        open *= 0.85f;
        mouth = MOUTH_HMM;
        break;
    case MUSE_MODE_SPEAKING:
        mouth = MOUTH_TALK;
        mouth_open = level * 1.3f + 0.1f * (0.5f + 0.5f * sinf(t * 22.0f));
        break;
    case MUSE_MODE_ERROR:
        style = EYES_X;
        mouth = MOUTH_FLAT;
        break;
    case MUSE_MODE_OFF:
        open = clampf((1.0f - p->mode_t / 1.0f) * 1.5f, 0, 1);
        mouth = MOUTH_SMILE;
        break;
    default:
        break;
    }
    if (happy > 0.2f) {
        style = EYES_HAPPY;
        mouth = MOUTH_GRIN;
    }

    draw_eye(a.fx - eye_dx, eye_y, open, style, s_eyes.gx, s_eyes.gy);
    draw_eye(a.fx + eye_dx, eye_y, open, style, s_eyes.gx, s_eyes.gy);

    /* Tiny brows for the expressive states. */
    int bl = iround(a.fx - eye_dx), br = iround(a.fx + eye_dx), by = iround(eye_y) - 4;
    if (mode == MUSE_MODE_THINKING) {
        px(bl - 1, by + 1, C_BROW); px(bl, by + 1, C_BROW);
        px(br - 1, by, C_BROW); px(br, by - 1, C_BROW);
    } else if (mode == MUSE_MODE_LISTENING) {
        px(bl - 1, by - 1, C_BROW); px(bl, by - 1, C_BROW);
        px(br - 1, by - 1, C_BROW); px(br, by - 1, C_BROW);
    }

    float blush = 0.55f + happy * 0.45f + (mode == MUSE_MODE_SPEAKING ? 0.15f : 0.0f);
    draw_blush(iround(a.fx - a.fa * 0.66f), iround(eye_y + 2), blush);
    draw_blush(iround(a.fx + a.fa * 0.66f), iround(eye_y + 2), blush);

    draw_mouth(iround(a.fx), iround(eye_y + 3), mouth, mouth_open);

    /* ---- foreground ---- */
    draw_sparkles(p, a.cx, 35.5f, true, spk_speed, spk_count);

    float top = a.hy - a.hry;
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING) {
        draw_waves(a.hx, a.fy + 2, a.hrx, level, t);
    }
    if (mode == MUSE_MODE_THINKING) {
        draw_thought_dots(a.cx + 15, top + 2, t);
    }
    if (happy > 0) {
        draw_hearts(a.cx, top + 1, t, happy);
    }
    if (mode == MUSE_MODE_ERROR) {
        draw_alert(iround(a.cx + 19), iround(top - 1));
    }
#undef YP
}

/* The pixel art has no clips to change between: no poof. */
float muse_pixel_switched_at(void)
{
    return -1;
}
