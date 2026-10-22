/*
 * The avatar as video clips: the toy astronaut, one clip per state, played
 * from the "anims" flash partition (avatar/character/make_anims.py packs
 * them; scripts/flash.sh writes them). It stands in for the pixel renderer
 * behind the same muse_pixel.h API, so the UI around it is unchanged.
 *
 * Each frame is a baseline JPEG, decoded with esp_new_jpeg (SIMD on the S3)
 * by a background task into a second buffer, so the UI's tick never waits on
 * a decode: the tick asks for a frame and swaps in the one decoded since. The
 * decoder also finds what changed, so only that is redrawn. The shown frame
 * is scaled to the canvas: 3:2 (256 px to 384) and 2x by repeating and
 * averaging pixels, anything else bilinearly. Wake and goodbye play once and
 * hold their last frame; the rest loop. Without the partition (or with a bad
 * one) the canvas stays black.
 *
 * Everything is read with esp_partition_read() rather than through a
 * mapping: with XIP from PSRAM the UI keeps running while NVS writes flash,
 * and a mapped read then can return erased-flash bytes. The driver's reads
 * wait for the write instead.
 */

#include "muse_pixel.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

static const char *TAG = "muse_anim";

#define PART_NAME "anims"
#define MAX_SIDE 256
#define DECODER_PRIORITY 4   /* just below the UI (5) and audio (6), above the network's background work */

enum { CLIP_WAKE, CLIP_IDLE, CLIP_LISTENING, CLIP_THINKING, CLIP_SPEAKING, CLIP_HAPPY, CLIP_ERROR,
       CLIP_GOODBYE, CLIP_COUNT };

typedef struct {
    uint16_t first, count, flags, pad;
} clip_t;

static const esp_partition_t *s_part;
static clip_t s_clips[CLIP_COUNT];
static uint32_t *s_index;            /* (offset, size) per frame, copied to RAM */
static uint32_t s_frames;
static uint8_t *s_jpeg;              /* the frame being decoded, read from flash */
static jpeg_dec_handle_t s_dec;      /* the decoder's */
static int s_w, s_h, s_fps;
static bool s_tried, s_ok;

static uint16_t *s_frame;            /* the shown frame, RGB565 (the UI's) */
static uint16_t *s_back;             /* the next one (the decoder's, until it's ready) */
static int s_shown = -1;             /* s_frame's global frame number */
static int s_back_dirty[4];          /* where s_back differs from s_frame: x1, y1, x2, y2 (x2 < 0: nowhere) */
static bool s_dirty_all = true;      /* redraw the whole canvas (the first frame) */
static int s_dirty[4] = { 0, 0, -1, -1 };   /* source pixels changed since the UI last asked */
static TaskHandle_t s_decoder;
static atomic_int s_want = -1;       /* the frame the UI asked for */
static atomic_int s_ready = -1;      /* s_back holds this frame, for the UI to swap in; -1: the decoder's */

/* Accents for the UI around the avatar (the ring and level bars), from the astronaut's suit. */
static const uint32_t ACCENT[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT] = 0xbdbdbd,        /* grey */
    [MUSE_MODE_IDLE] = 0xffffff,        /* the helmet's white */
    [MUSE_MODE_LISTENING] = 0x3cc8e6,   /* the antennae's cyan */
    [MUSE_MODE_THINKING] = 0xffc83d,    /* the buttons' yellow */
    [MUSE_MODE_SPEAKING] = 0xff7a2e,    /* the suit's orange */
    [MUSE_MODE_ERROR] = 0xff4d4d,
    [MUSE_MODE_OFF] = 0x6e6e6e,
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

static void decoder(void *arg);

/* Reads the header, clip table and frame index into RAM and checks them once. */
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
    uint8_t head[16 + CLIP_COUNT * 8];
    if (esp_partition_read(part, 0, head, sizeof(head)) != ESP_OK) {
        ESP_LOGW(TAG, "can't read the animations");
        return false;
    }
    if (memcmp(head, "MANI", 4) || rd16(head + 4) != 1) {
        ESP_LOGW(TAG, "no animations written yet (scripts/flash.sh writes them)");
        return false;
    }
    s_w = rd16(head + 6);
    s_h = rd16(head + 8);
    s_fps = rd16(head + 10);
    s_frames = rd32(head + 12 + CLIP_COUNT * 8);
    uint32_t index_end = sizeof(head) + s_frames * 8;
    if (s_w <= 0 || s_h <= 0 || s_w > MAX_SIDE || s_h > MAX_SIDE || !s_fps || !s_frames || s_frames > 0xffff ||
        index_end > part->size) {
        ESP_LOGW(TAG, "animation header out of range");
        return false;
    }
    for (int c = 0; c < CLIP_COUNT; c++) {
        const uint8_t *e = head + 12 + c * 8;
        s_clips[c] = (clip_t){ rd16(e), rd16(e + 2), rd16(e + 4), 0 };
        if (!s_clips[c].count || s_clips[c].first + s_clips[c].count > s_frames) {
            ESP_LOGW(TAG, "clip %d out of range", c);
            return false;
        }
    }
    s_index = heap_caps_malloc(s_frames * 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_index || esp_partition_read(part, sizeof(head), s_index, s_frames * 8) != ESP_OK) {
        ESP_LOGW(TAG, "can't read the frame index");
        return false;
    }
    uint32_t biggest = 0;
    for (uint32_t f = 0; f < s_frames; f++) {
        uint32_t off = s_index[f * 2], size = s_index[f * 2 + 1];
        if (off < index_end || size < 4 || size > part->size - off) {
            ESP_LOGW(TAG, "frame %u out of range", (unsigned)f);
            return false;
        }
        biggest = size > biggest ? size : biggest;
    }
    /* esp_new_jpeg writes 16 bytes at a time: its output must be 16-byte aligned. */
    s_frame = heap_caps_aligned_calloc(16, (size_t)s_w * s_h, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_back = heap_caps_aligned_calloc(16, (size_t)s_w * s_h, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_jpeg = heap_caps_malloc(biggest, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame || !s_back || !s_jpeg) {
        return false;
    }
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    jpeg_error_t opened = jpeg_dec_open(&cfg, &s_dec);
    ESP_LOGI(TAG, "the JPEG decoder takes %d bytes of internal RAM",
             (int)(internal - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    if (opened != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "can't open the JPEG decoder");
        return false;
    }
    s_part = part;
    /* Its stack in PSRAM: internal RAM is what Wi-Fi, TLS and BLE run short of. Its
     * flash reads are fine from there, since with XIP from PSRAM the cache stays on. */
    if (xTaskCreatePinnedToCoreWithCaps(decoder, "muse_anim", 4096, NULL, DECODER_PRIORITY, &s_decoder,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "can't start the decoder");
        return false;
    }
    s_ok = true;
    ESP_LOGI(TAG, "%u frames, %dx%d at %d fps", (unsigned)s_frames, s_w, s_h, s_fps);
    return true;
}

/* --- decoding ------------------------------------------------------------------------ */

/*
 * esp_new_jpeg's allocators, replacing the library's own (esp_jpeg_memory.c in
 * libesp_new_jpeg.a, which the linker then leaves out). They are the same but
 * for the "inner" pair, which put the decoder's ~9 KB of tables in internal RAM
 * first: here PSRAM comes first, since internal RAM is what Wi-Fi, TLS and BLE
 * run short of, and decoding stays well inside a frame's time either way.
 */
#define JPEG_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define JPEG_INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

void *jpeg_calloc(size_t n, size_t size)
{
    return heap_caps_calloc_prefer(n, size, 2, JPEG_PSRAM, JPEG_INTERNAL);
}

void *jpeg_calloc_inner(size_t size)
{
    return heap_caps_calloc_prefer(1, size, 2, JPEG_PSRAM, JPEG_INTERNAL);
}

void *jpeg_calloc_align(size_t size, int aligned)
{
    void *p = heap_caps_aligned_calloc(aligned, 1, size, JPEG_PSRAM);
    return p ? p : heap_caps_aligned_calloc(aligned, 1, size, JPEG_INTERNAL);
}

void *jpeg_calloc_align_inner(size_t size, int aligned)
{
    return jpeg_calloc_align(size, aligned);
}

void jpeg_free(void *p)
{
    heap_caps_free(p);
}

void jpeg_free_align(void *p)
{
    heap_caps_free(p);
}

/* The box around the pixels that differ between two frames (x2 < 0: none). */
static void find_changes(const uint16_t *was, const uint16_t *now, int box[4])
{
    int x1 = s_w, y1 = s_h, x2 = -1, y2 = -1;
    for (int y = 0; y < s_h; y++) {
        const uint16_t *a = was + y * s_w, *b = now + y * s_w;
        if (!memcmp(a, b, s_w * sizeof(uint16_t))) {
            continue;
        }
        int l = 0, r = s_w - 1;
        while (a[l] == b[l]) {
            l++;
        }
        while (a[r] == b[r]) {
            r--;
        }
        x1 = l < x1 ? l : x1;
        x2 = r > x2 ? r : x2;
        y1 = y < y1 ? y : y1;
        y2 = y;
    }
    box[0] = x1, box[1] = y1, box[2] = x2, box[3] = y2;
}

/* Decodes a frame into s_back. A failure (logged once per run of them) leaves
 * the frame unshown, so the next tick asks again. */
static bool decode(int frame)
{
    static int s_failing;        /* failures in a row */
    uint32_t size = s_index[frame * 2 + 1];
    int64_t t0 = esp_timer_get_time();
    jpeg_error_t r = JPEG_ERR_FAIL;
    if (esp_partition_read(s_part, s_index[frame * 2], s_jpeg, size) == ESP_OK && s_jpeg[0] == 0xff &&
        s_jpeg[1] == 0xd8) {
        jpeg_dec_io_t io = { .inbuf = s_jpeg, .inbuf_len = (int)size, .outbuf = (uint8_t *)s_back };
        jpeg_dec_header_info_t info;
        r = jpeg_dec_parse_header(s_dec, &io, &info);
        if (r == JPEG_ERR_OK && (info.width != s_w || info.height != s_h)) {
            r = JPEG_ERR_INVALID_PARAM;
        }
        if (r == JPEG_ERR_OK) {
            r = jpeg_dec_process(s_dec, &io);
        }
    }
    if (r != JPEG_ERR_OK) {
        if (!s_failing++) {
            ESP_LOGW(TAG, "frame %d: decode error %d", frame, r);
        }
        return false;
    }
    if (s_failing) {
        ESP_LOGI(TAG, "frames decode again (after %d failures)", s_failing);
        s_failing = 0;
    }
    /* How long decodes take, once over the first few seconds: past a frame's
     * time, frames show late. */
    static int s_count, s_late;
    static int64_t s_total, s_max;
    if (s_count < 10 * s_fps) {
        int64_t us = esp_timer_get_time() - t0;
        s_total += us;
        s_max = us > s_max ? us : s_max;
        s_late += us > 1000000 / s_fps;
        if (++s_count == 10 * s_fps) {
            ESP_LOGI(TAG, "%d frames decode in %d ms on average, %d ms at most; %d took longer than a frame",
                     s_count, (int)(s_total / s_count / 1000), (int)(s_max / 1000), s_late);
        }
    }
    return true;
}

/* Decodes whatever frame the UI asks for into s_back, finds what changed from
 * the shown frame (which the UI leaves alone until s_ready says), then hands
 * it over. */
static void decoder(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int frame = atomic_load(&s_want);
        if (frame >= 0 && atomic_load(&s_ready) < 0 && decode(frame)) {
            find_changes(s_frame, s_back, s_back_dirty);
            atomic_store(&s_ready, frame);
        }
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

/* Timing, logged once over five seconds from 14 s after boot (past Wi-Fi and
 * the Muse connection, before the 30 s auto-sleep): how often the UI ticks, and
 * how long scaling the frame to the canvas takes per tick. */
static int64_t s_scale_us;

static void log_ticks(void)
{
    static int s_ticks;
    static int64_t s_first, s_last, s_gap_max, s_scale_total, s_scale_max;
    int64_t now = esp_timer_get_time();
    if (s_ticks >= 5 * s_fps || now < 14000000) {
        s_scale_us = 0;
        return;
    }
    if (s_ticks) {
        s_gap_max = now - s_last > s_gap_max ? now - s_last : s_gap_max;
        s_scale_total += s_scale_us;
        s_scale_max = s_scale_us > s_scale_max ? s_scale_us : s_scale_max;
    } else {
        s_first = now;
    }
    s_scale_us = 0;
    s_last = now;
    if (++s_ticks == 5 * s_fps) {
        ESP_LOGI(TAG, "%d ticks at %d ms apart on average, %d ms at most; scaling takes %d ms a tick, %d ms at most",
                 s_ticks, (int)((now - s_first) / (s_ticks - 1) / 1000), (int)(s_gap_max / 1000),
                 (int)(s_scale_total / (s_ticks - 1) / 1000), (int)(s_scale_max / 1000));
    }
}

void muse_pixel_render(const muse_pose_t *p)
{
    if (!open_pack()) {
        return;
    }
    log_ticks();
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
    /* Swap in what the decoder finished since the last tick... */
    int ready = atomic_load(&s_ready);
    if (ready >= 0) {
        uint16_t *t = s_frame;
        s_frame = s_back;
        s_back = t;
        s_shown = ready;
        const int *b = s_back_dirty;
        if (b[2] >= 0) {
            if (s_dirty[2] < 0) {
                memcpy(s_dirty, b, sizeof(s_dirty));
            } else {
                s_dirty[0] = b[0] < s_dirty[0] ? b[0] : s_dirty[0];
                s_dirty[1] = b[1] < s_dirty[1] ? b[1] : s_dirty[1];
                s_dirty[2] = b[2] > s_dirty[2] ? b[2] : s_dirty[2];
                s_dirty[3] = b[3] > s_dirty[3] ? b[3] : s_dirty[3];
            }
        }
        atomic_store(&s_ready, -1);
    }
    /* ...and ask for this tick's frame, to show on the next. */
    if (frame != s_shown && frame >= 0 && frame < (int)s_frames) {
        atomic_store(&s_want, frame);
        xTaskNotifyGive(s_decoder);
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
    s_dirty_all = true;
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

static bool doubled(void)
{
    return s_size == 2 * s_w && s_w == s_h;
}

static bool three_halves(void)
{
    return 2 * s_size == 3 * s_w && s_w == s_h;
}

/* The changed area in canvas pixels: what the changed source pixels map to,
 * plus a pixel each side where scaling blends neighbours (not when doubling). */
int muse_pixel_dirty(int *x1, int *y1, int *x2, int *y2)
{
    if (s_dirty_all) {
        s_dirty_all = false;
        *x1 = *y1 = 0;
        *x2 = *y2 = s_size - 1;
        return 1;
    }
    if (!s_ok || s_dirty[2] < 0) {
        return 0;
    }
    int pad = doubled() ? 0 : 1;
    int lo[2], hi[2];
    for (int i = 0; i < 2; i++) {
        int side = i ? s_h : s_w;
        lo[i] = s_dirty[i] * s_size / side - 2 * pad;
        hi[i] = (s_dirty[i + 2] + 1) * s_size / side - 1 + 2 * pad;
        lo[i] = lo[i] < 0 ? 0 : lo[i];
        hi[i] = hi[i] > s_size - 1 ? s_size - 1 : hi[i];
    }
    *x1 = lo[0], *y1 = lo[1], *x2 = hi[0], *y2 = hi[1];
    s_dirty[2] = s_dirty[3] = -1;
    return 1;
}

/* Per-channel average of two RGB565 pixels, with no multiplies. */
static inline uint16_t avg565(uint16_t a, uint16_t b)
{
    return (uint16_t)((a & b) + (((a ^ b) & 0xf7de) >> 1));
}

/* 2x: each source pixel as a 2x2 block. Output rows 2k and 2k+1 are the same. */
static void double_2x(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        if (y > y0 && (y & 1)) {
            memcpy(dst, dst - stride_px, n * sizeof(uint16_t));   /* the row above, same source row */
            continue;
        }
        const uint16_t *src = s_frame + (y >> 1) * s_w;
        int x = x0, i = 0;
        if (x & 1) {
            dst[i++] = src[x >> 1];
            x++;
        }
        if (!((uintptr_t)(dst + i) & 3)) {
            uint32_t *d32 = (uint32_t *)(dst + i);
            for (; x + 1 <= x1; x += 2, i += 2) {
                uint32_t p = src[x >> 1];
                *d32++ = p | p << 16;
            }
        }
        for (; x <= x1; x++, i++) {
            dst[i] = src[x >> 1];
        }
    }
}

/* 3:2 along a row: each pair of source pixels a, b becomes a, (a+b)/2, b. */
static void row_3_2(uint16_t *out, const uint16_t *src, int x0, int x1)
{
    int x = x0;
    for (; x <= x1 && x % 3; x++) {   /* into the first whole triple */
        const uint16_t *s = src + 2 * (x / 3);
        *out++ = x % 3 == 2 ? s[1] : avg565(s[0], s[1]);
    }
    const uint16_t *s = src + 2 * (x / 3);
    for (; x + 2 <= x1; x += 3, s += 2, out += 3) {
        uint16_t a = s[0], b = s[1];
        out[0] = a;
        out[1] = avg565(a, b);
        out[2] = b;
    }
    if (x <= x1) {
        *out++ = s[0];
    }
    if (x + 1 <= x1) {
        *out = avg565(s[0], s[1]);
    }
}

/* 3:2 both ways: rows 3j and 3j+2 are source rows 2j and 2j+1 widened, and row
 * 3j+1 is their average, so each source row is widened once per strip. LVGL's
 * two draw threads scale at once, so the widened rows live on the caller's
 * stack. */
static void scale_3_2(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    uint16_t above[MAX_SIDE * 3 / 2], below[MAX_SIDE * 3 / 2];   /* a canvas row at most */
    for (int y = y0; y <= y1;) {
        int j = y / 3, r = y - 3 * j;
        int last = 3 * j + 2 < y1 ? 3 * j + 2 : y1;   /* this triple's last row in the strip */
        if (r <= 1) {
            row_3_2(above, s_frame + 2 * j * s_w, x0, x1);
        }
        if (last >= 3 * j + 1) {
            row_3_2(below, s_frame + (2 * j + 1) * s_w, x0, x1);
        }
        for (; y <= last; y++, r++, dst += stride_px) {
            if (r == 0) {
                memcpy(dst, above, n * sizeof(uint16_t));
            } else if (r == 2) {
                memcpy(dst, below, n * sizeof(uint16_t));
            } else {
                for (int i = 0; i < n; i++) {
                    dst[i] = avg565(above[i], below[i]);
                }
            }
        }
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
    int64_t t0 = esp_timer_get_time();
    if (doubled()) {
        double_2x(dst, stride_px, x0, x1, y0, y1);
    } else if (three_halves()) {
        scale_3_2(dst, stride_px, x0, x1, y0, y1);
    } else {
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
    s_scale_us += esp_timer_get_time() - t0;
}
