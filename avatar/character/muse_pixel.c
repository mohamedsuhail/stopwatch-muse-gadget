/*
 * The avatar as video clips: the toy astronaut, one clip per state, played
 * from the "anims" flash partition (avatar/character/make_anims.py packs
 * them; scripts/flash.sh writes them). It stands in for the pixel renderer
 * behind the same muse_pixel.h API, so the UI around it is unchanged.
 *
 * Each frame is a baseline JPEG, decoded with the ROM's TJpgDec when the
 * frame changes and scaled to the canvas with bilinear filtering. Wake and
 * goodbye play once and hold their last frame; the rest loop. Without the
 * partition (or with a bad one) the canvas stays black.
 */

#include "muse_pixel.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp32s3/rom/tjpgd.h"

static const char *TAG = "muse_anim";

#define PART_NAME "anims"
#define MAX_SIDE 256

enum { CLIP_WAKE, CLIP_IDLE, CLIP_LISTENING, CLIP_THINKING, CLIP_SPEAKING, CLIP_HAPPY, CLIP_ERROR,
       CLIP_GOODBYE, CLIP_COUNT };

typedef struct {
    uint16_t first, count, flags, pad;
} clip_t;

static const uint8_t *s_pack;        /* the mapped partition */
static const clip_t *s_clips;
static const uint32_t *s_index;      /* (offset, size) per frame */
static uint32_t s_frames;
static int s_w, s_h, s_fps;
static bool s_tried, s_ok;

static uint16_t *s_frame;            /* the decoded frame, RGB565 */
static int s_shown = -1;             /* its global frame number */

/* Accents for the UI around the avatar: the same per-state colours as before. */
static const uint32_t ACCENT[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT] = 0xa9c0ff,
    [MUSE_MODE_IDLE] = 0xa77dff,
    [MUSE_MODE_LISTENING] = 0x5cb8ff,
    [MUSE_MODE_THINKING] = 0xe07bff,
    [MUSE_MODE_SPEAKING] = 0x6ff0bf,
    [MUSE_MODE_ERROR] = 0xff5c5c,
    [MUSE_MODE_OFF] = 0x7c72d0,
};

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    return ACCENT[mode < MUSE_MODE_COUNT ? mode : MUSE_MODE_IDLE];
}

static inline uint16_t rd16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static inline uint32_t rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Maps the partition and checks its header once. */
static bool open_pack(void)
{
    if (s_tried) {
        return s_ok;
    }
    s_tried = true;
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                                           PART_NAME);
    if (!part) {
        ESP_LOGW(TAG, "no \"%s\" partition: the avatar stays blank", PART_NAME);
        return false;
    }
    const void *map;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &map, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "can't map the animations");
        return false;
    }
    const uint8_t *p = map;
    if (memcmp(p, "MANI", 4) || rd16(p + 4) != 1) {
        ESP_LOGW(TAG, "no animations written yet (scripts/flash.sh writes them)");
        return false;
    }
    s_w = rd16(p + 6);
    s_h = rd16(p + 8);
    s_fps = rd16(p + 10);
    s_clips = (const clip_t *)(p + 12);
    s_frames = rd32(p + 12 + CLIP_COUNT * 8);
    s_index = (const uint32_t *)(p + 16 + CLIP_COUNT * 8);
    if (s_w <= 0 || s_h <= 0 || s_w > MAX_SIDE || s_h > MAX_SIDE || !s_fps || !s_frames ||
        16 + CLIP_COUNT * 8 + s_frames * 8 > part->size) {
        ESP_LOGW(TAG, "animation header out of range");
        return false;
    }
    for (int c = 0; c < CLIP_COUNT; c++) {
        if (!s_clips[c].count || s_clips[c].first + s_clips[c].count > s_frames) {
            ESP_LOGW(TAG, "clip %d out of range", c);
            return false;
        }
    }
    s_frame = heap_caps_calloc((size_t)s_w * s_h, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame) {
        return false;
    }
    s_pack = p;
    s_ok = true;
    ESP_LOGI(TAG, "%u frames, %dx%d at %d fps", (unsigned)s_frames, s_w, s_h, s_fps);
    return true;
}

/* --- JPEG ------------------------------------------------------------------------ */

typedef struct {
    const uint8_t *data;
    uint32_t size, pos;
} src_t;

static UINT jpeg_in(JDEC *jd, BYTE *buf, UINT n)
{
    src_t *s = jd->device;
    if (s->pos + n > s->size) {
        n = s->size - s->pos;
    }
    if (buf) {
        memcpy(buf, s->data + s->pos, n);
    }
    s->pos += n;
    return n;
}

static UINT jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void)jd;
    const uint8_t *rgb = bitmap;   /* RGB888, the ROM's format */
    for (int y = rect->top; y <= rect->bottom; y++) {
        uint16_t *row = s_frame + y * s_w;
        for (int x = rect->left; x <= rect->right; x++, rgb += 3) {
            row[x] = (uint16_t)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
        }
    }
    return 1;
}

static void decode(int frame)
{
    static uint8_t pool[3100];   /* TJpgDec's work area */
    src_t src = { s_pack + rd32((const uint8_t *)&s_index[frame * 2]),
                  rd32((const uint8_t *)&s_index[frame * 2 + 1]), 0 };
    JDEC jd;
    int64_t t0 = esp_timer_get_time();
    JRESULT r = jd_prepare(&jd, jpeg_in, pool, sizeof(pool), &src);
    if (r == JDR_OK) {
        r = jd_decomp(&jd, jpeg_out, 0);
    }
    static bool logged;
    if (r != JDR_OK) {
        ESP_LOGW(TAG, "frame %d: decode error %d", frame, r);
    } else if (!logged) {
        logged = true;
        ESP_LOGI(TAG, "a frame decodes in %d ms", (int)((esp_timer_get_time() - t0) / 1000));
    }
}

/* --- frame choice -------------------------------------------------------------------- */

static int clip_for(const muse_pose_t *p)
{
    if (p->happy > 0.2f && p->mode != MUSE_MODE_ERROR) {
        return CLIP_HAPPY;
    }
    switch (p->mode) {
    case MUSE_MODE_BOOT:      return CLIP_WAKE;
    case MUSE_MODE_LISTENING: return CLIP_LISTENING;
    case MUSE_MODE_THINKING:  return CLIP_THINKING;
    case MUSE_MODE_SPEAKING:  return CLIP_SPEAKING;
    case MUSE_MODE_ERROR:     return CLIP_ERROR;
    case MUSE_MODE_OFF:       return CLIP_GOODBYE;
    default:                  return CLIP_IDLE;
    }
}

void muse_pixel_render(const muse_pose_t *p)
{
    if (!open_pack()) {
        return;
    }
    /* A clip starts from its first frame when it comes on. */
    static int s_clip = -1;
    static float s_since;
    int clip = clip_for(p);
    if (clip != s_clip) {
        s_clip = clip;
        s_since = p->t;
    }
    const clip_t *c = &s_clips[clip];
    int n = (int)((p->t - s_since) * s_fps);
    n = c->flags & 1 ? (n < c->count ? n : c->count - 1) : n % c->count;
    int frame = c->first + n;
    if (frame != s_shown) {
        decode(frame);
        s_shown = frame;
    }
}

/* --- scaling ---------------------------------------------------------------------------- */

#define MAP_MAX 512
static uint16_t s_src[MAP_MAX];   /* screen pixel -> source pixel (left/top of the pair) */
static uint8_t s_wt[MAP_MAX];     /* -> weight of the next source pixel, 0..255 */
static int s_size;

void muse_pixel_set_size(int px)
{
    s_size = px < MAP_MAX ? px : MAP_MAX;
    open_pack();
    int side = s_ok ? s_w : 64;
    for (int i = 0; i < s_size; i++) {
        /* Centre-aligned sampling, as an image scaler would. */
        int32_t pos = (int32_t)(((2 * i + 1) * side * 128) / s_size) - 128;   /* in 1/256 px */
        if (pos < 0) {
            pos = 0;
        }
        int a = pos >> 8;
        if (a >= side - 1) {
            a = side - 1;
            pos = a << 8;
        }
        s_src[i] = (uint16_t)a;
        s_wt[i] = (uint8_t)(pos & 0xff);
    }
}

static inline uint16_t lerp565(uint16_t a, uint16_t b, int w)
{
    if (a == b || !w) {
        return a;
    }
    int r = ((a >> 11) * (256 - w) + (b >> 11) * w) >> 8;
    int g = (((a >> 5) & 63) * (256 - w) + ((b >> 5) & 63) * w) >> 8;
    int bl = ((a & 31) * (256 - w) + (b & 31) * w) >> 8;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    if (!s_ok) {
        for (int y = y0; y <= y1; y++, dst += stride_px) {
            memset(dst, 0, n * sizeof(uint16_t));
        }
        return;
    }
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        int sy = s_src[y], wy = s_wt[y];
        const uint16_t *r0 = s_frame + sy * s_w;
        const uint16_t *r1 = sy + 1 < s_h ? r0 + s_w : r0;
        for (int i = 0; i < n; i++) {
            int x = x0 + i, sx = s_src[x], wx = s_wt[x];
            int sx1 = sx + 1 < s_w ? sx + 1 : sx;
            uint16_t top = lerp565(r0[sx], r0[sx1], wx);
            uint16_t bot = lerp565(r1[sx], r1[sx1], wx);
            dst[i] = lerp565(top, bot, wy);
        }
    }
}
