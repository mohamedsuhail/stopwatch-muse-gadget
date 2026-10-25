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
 * hold their last frame; the rest loop. When the clip changes, the new one
 * fades in over its first three frames from a still of the last frame shown
 * (decoding the old clip as well would take two decodes a frame). Without the
 * partition (or with a bad one) the canvas stays black.
 *
 * Everything is read with esp_partition_read() rather than through a
 * mapping: with XIP from PSRAM the UI keeps running while NVS writes flash,
 * and a mapped read then can return erased-flash bytes. The driver's reads
 * wait for the write instead.
 */

#include "muse_pixel.h"
#include "muse_board.h"
#include "muse_state.h"

#include <math.h>
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
#define FADE_FRAMES 3        /* a new clip's first frames, blended 3/4, 1/2, 1/4 with the last one shown */

/* What the UI asks the decoder for: a frame, how much of the still to blend
 * in (quarters, 0: none), and which clip change the still belongs to. */
#define WANT(frame, mix, gen) ((frame) | (mix) << 16 | ((gen) & 0x1fff) << 18)
#define WANT_FRAME(w) ((w) & 0xffff)
#define WANT_MIX(w) ((w) >> 16 & 3)
#define WANT_GEN(w) ((w) >> 18)

/* The pack's clips, in make_anims.py's order. Version 1 packs had only the first 8;
 * the others then stand in from those (open_pack). */
enum { CLIP_WAKE, CLIP_IDLE, CLIP_LISTENING, CLIP_THINKING, CLIP_SPEAKING, CLIP_HAPPY, CLIP_ERROR,
       CLIP_GOODBYE, CLIP_SAD, CLIP_NOD, CLIP_CHECKING, CLIP_IDLE_A, CLIP_IDLE_B, CLIP_COUNT,
       CLIP_WAVE = CLIP_COUNT };   /* not in the pack: the wave out of goodbye, for a tap */
#define CLIPS_V1 8

/* Goodbye's wave: the arm up and waving from WAVE_FROM_S for WAVE_S, before the
 * eyes start to close for sleep (version 1's clip waved 1.35 s in, for 1.25 s). */
#define WAVE_FROM_S 0.7f
#define WAVE_S 1.5f
#define WAVE_FROM_S_V1 1.35f
#define WAVE_S_V1 1.25f

/* Idle: a variation (bubbles, juggling) every few loops of the calm idle clip. */
#define IDLE_LOOPS_BETWEEN 2

typedef struct {
    uint16_t first, count, flags, fps;   /* fps 0: the pack's */
} clip_t;

static const esp_partition_t *s_part;
static clip_t s_clips[CLIP_COUNT + 1];
static uint32_t *s_index;            /* (offset, size) per frame, copied to RAM */
static uint32_t s_frames;
static uint8_t *s_jpeg;              /* the frame being decoded, read from flash */
static jpeg_dec_handle_t s_dec;      /* the decoder's */
static int s_w, s_h, s_fps;
static bool s_tried, s_ok;

static uint16_t *s_frame;            /* the shown frame, RGB565 (the UI's) */
static uint16_t *s_back;             /* the next one (the decoder's, until it's ready) */
static uint16_t *s_hold;             /* the last frame shown before the clip changed (the decoder's) */
static int s_shown = -1;             /* what s_frame shows: a WANT() */
static int s_back_dirty[4];          /* where s_back differs from s_frame: x1, y1, x2, y2 (x2 < 0: nowhere) */
static bool s_dirty_all = true;      /* redraw the whole canvas (the first frame) */
static int s_dirty[4] = { 0, 0, -1, -1 };   /* source pixels changed since the UI last asked */
static TaskHandle_t s_decoder;
static atomic_int s_want = -1;       /* what the UI asked for: a WANT() */
static atomic_int s_ready = -1;      /* s_back holds this WANT(), for the UI to swap in; -1: the decoder's */

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
    int version = rd16(head + 4);
    if (memcmp(head, "MANI", 4) || (version != 1 && version != 2)) {
        ESP_LOGW(TAG, "no animations written yet (scripts/flash.sh writes them)");
        return false;
    }
    s_w = rd16(head + 6);
    s_h = rd16(head + 8);
    s_fps = rd16(head + 10);
    /* v1: 8 clips from byte 12; v2: a clip count at 12 and the clips from 16, each with its fps */
    int nclips = version == 1 ? CLIPS_V1 : rd16(head + 12);
    size_t table = version == 1 ? 12 : 16;
    nclips = nclips < CLIP_COUNT ? nclips : CLIP_COUNT;
    if (version == 2 && rd16(head + 12) > CLIP_COUNT) {
        ESP_LOGW(TAG, "%d clips in the pack, %d known: the rest are left out", rd16(head + 12), CLIP_COUNT);
    }
    size_t count_at = table + (version == 1 ? CLIPS_V1 : rd16(head + 12)) * 8;
    uint8_t fc[4];
    if (esp_partition_read(part, count_at, fc, 4) != ESP_OK) {
        return false;
    }
    s_frames = rd32(fc);
    uint32_t index_end = count_at + 4 + s_frames * 8;
    if (s_w <= 0 || s_h <= 0 || s_w > MAX_SIDE || s_h > MAX_SIDE || !s_fps || !s_frames || s_frames > 0xffff ||
        index_end > part->size) {
        ESP_LOGW(TAG, "animation header out of range");
        return false;
    }
    for (int c = 0; c < nclips; c++) {
        const uint8_t *e = head + table + c * 8;
        s_clips[c] = (clip_t){ rd16(e), rd16(e + 2), rd16(e + 4), version == 1 ? 0 : rd16(e + 6) };
        if (!s_clips[c].count || s_clips[c].first + s_clips[c].count > s_frames) {
            ESP_LOGW(TAG, "clip %d out of range", c);
            return false;
        }
    }
    /* What a version 1 pack hasn't: sad was the error face, nod and the idle
     * variations the idle clip, checking the thinking one. */
    static const int8_t STAND_IN[CLIP_COUNT] = {
        [CLIP_SAD] = CLIP_ERROR, [CLIP_NOD] = CLIP_IDLE, [CLIP_CHECKING] = CLIP_THINKING,
        [CLIP_IDLE_A] = CLIP_IDLE, [CLIP_IDLE_B] = CLIP_IDLE,
    };
    for (int c = nclips; c < CLIP_COUNT; c++) {
        s_clips[c] = s_clips[STAND_IN[c]];
    }
    /* Played once, holding its last frame (arm up) until the tap reaction is
     * over. A shorter goodbye just waves for what it has. */
    const clip_t *bye = &s_clips[CLIP_GOODBYE];
    int bye_fps = bye->fps ? bye->fps : s_fps;
    int from = (int)((version == 1 ? WAVE_FROM_S_V1 : WAVE_FROM_S) * bye_fps);
    int len = (int)((version == 1 ? WAVE_S_V1 : WAVE_S) * bye_fps);
    from = bye->count > from ? from : 0;
    int n = bye->count - from < len ? bye->count - from : len;
    s_clips[CLIP_WAVE] = (clip_t){ bye->first + from, n, 1, bye->fps };
    s_index = heap_caps_malloc(s_frames * 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_index || esp_partition_read(part, count_at + 4, s_index, s_frames * 8) != ESP_OK) {
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
    s_hold = heap_caps_aligned_calloc(16, (size_t)s_w * s_h, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_jpeg = heap_caps_malloc(biggest, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame || !s_back || !s_hold || !s_jpeg) {
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
    ESP_LOGI(TAG, "%u frames, %dx%d, %d clips (pack version %d)", (unsigned)s_frames, s_w, s_h, nclips, version);
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

/* Per-channel average of two pairs of RGB565 pixels, with no multiplies. */
static inline uint32_t avg565x2(uint32_t a, uint32_t b)
{
    return (a & b) + (((a ^ b) & 0xf7def7de) >> 1);
}

/* Blends the still into s_back: mix quarters of it (1..3), two pixels at a time. */
static void blend_hold(int mix)
{
    const uint32_t *h = (const uint32_t *)s_hold;
    uint32_t *b = (uint32_t *)s_back;
    for (int i = 0, n = s_w * s_h / 2; i < n; i++) {
        uint32_t half = avg565x2(h[i], b[i]);
        b[i] = mix == 2 ? half : avg565x2(mix == 3 ? h[i] : b[i], half);
    }
}

/* Decodes what the UI asked for into s_back: the frame, blended with the still
 * while a clip fades in (which the first blend of a clip change takes from the
 * shown frame). A failure (logged once per run of them) leaves the frame
 * unshown, so the next tick asks again. */
static bool decode(int want)
{
    static int s_failing;        /* failures in a row */
    static int s_hold_gen;       /* the clip change s_hold belongs to */
    int frame = WANT_FRAME(want), mix = WANT_MIX(want);
    uint32_t size = s_index[frame * 2 + 1];
    int64_t t0 = esp_timer_get_time();
    if (mix && WANT_GEN(want) != s_hold_gen) {
        memcpy(s_hold, s_frame, (size_t)s_w * s_h * sizeof(uint16_t));
        s_hold_gen = WANT_GEN(want);
    }
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
    if (mix) {
        blend_hold(mix);
    }
    /* How long decodes (and blends) take, every 10 s: past a frame's time,
     * frames show late. Logged after boot, and whenever Muse isn't idle. */
    static int s_count, s_late, s_windows;
    static int64_t s_total, s_max;
    int64_t us = esp_timer_get_time() - t0;
    s_total += us;
    s_max = us > s_max ? us : s_max;
    s_late += us > 1000000 / s_fps;
    if (++s_count == 10 * s_fps) {
        if (!s_windows++ || muse_state_mode(NULL) != MUSE_MODE_IDLE) {
            ESP_LOGI(TAG, "%d frames decode in %d ms on average, %d ms at most; %d took longer than a frame",
                     s_count, (int)(s_total / s_count / 1000), (int)(s_max / 1000), s_late);
        }
        s_count = s_late = 0;
        s_total = s_max = 0;
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
        int want = atomic_load(&s_want);
        if (want >= 0 && atomic_load(&s_ready) < 0 && decode(want)) {
            find_changes(s_frame, s_back, s_back_dirty);
            atomic_store(&s_ready, want);
        }
    }
}

/* --- frame choice -------------------------------------------------------------------- */

/* A loop clip played as a face is shown for at least this long, then gives way to talking. */
#define REACT_MIN_SECS 2.5f

/*
 * The clip for a face Muse asked for in its reply, or -1 once it's played.
 * Goodnight (waving, then asleep) plays once and holds until the reply ends.
 * Idle right after a reply replays its last face once as the ending
 * (muse_state.h), goodnight included, then gives way to the idle clip.
 */
static inline int clip_fps(const clip_t *c)
{
    return c->fps ? c->fps : s_fps;
}

static int reaction_clip(const muse_pose_t *p)
{
    static const int8_t CLIP_OF[] = {
        [MUSE_REACT_JOY] = CLIP_HAPPY,
        [MUSE_REACT_THINKING] = CLIP_THINKING,
        [MUSE_REACT_SAD] = CLIP_SAD,
        [MUSE_REACT_GOODNIGHT] = CLIP_GOODBYE,
    };
    bool ending = p->mode == MUSE_MODE_IDLE;
    if (p->reaction <= MUSE_REACT_NONE || p->reaction > MUSE_REACT_GOODNIGHT ||
        (!ending && p->mode != MUSE_MODE_SPEAKING && p->mode != MUSE_MODE_THINKING)) {
        return -1;
    }
    int clip = CLIP_OF[p->reaction];
    float secs = (float)s_clips[clip].count / clip_fps(&s_clips[clip]);
    secs = secs > REACT_MIN_SECS ? secs : REACT_MIN_SECS;
    return (!ending && p->reaction == MUSE_REACT_GOODNIGHT) || p->reaction_t < secs ? clip : -1;
}

static bool s_trick_again;   /* a trick began afresh: its clip restarts even if it's showing already */
static float s_switched_at = -1;   /* when one clip last changed into another: the UI's poof */

float muse_pixel_switched_at(void)
{
    return s_switched_at;
}

static int clip_for(const muse_pose_t *p)
{
    /* A trick the live voice was asked for: once through, over anything but an error. */
    float trick_t;
    muse_trick_t trick = muse_state_trick(&trick_t);
    static float s_last_trick_t = 1e9f;
    s_trick_again = trick > MUSE_TRICK_NONE && trick_t < s_last_trick_t;   /* its clock went back */
    s_last_trick_t = trick > MUSE_TRICK_NONE ? trick_t : 1e9f;
    if (trick > MUSE_TRICK_NONE && trick <= MUSE_TRICK_WAVE && p->mode != MUSE_MODE_ERROR &&
        p->mode != MUSE_MODE_OFF && p->mode != MUSE_MODE_BOOT) {
        static const int8_t CLIP_OF_TRICK[] = {
            [MUSE_TRICK_BUBBLES] = CLIP_IDLE_A,
            [MUSE_TRICK_JUGGLE] = CLIP_IDLE_B,
            [MUSE_TRICK_WAVE] = CLIP_WAVE,
        };
        int clip = CLIP_OF_TRICK[trick];
        if (trick_t < (float)s_clips[clip].count / clip_fps(&s_clips[clip])) {
            return clip;
        }
    }
    int face = reaction_clip(p);
    if (face >= 0) {
        return face;
    }
    if (p->happy > 0.2f && p->mode != MUSE_MODE_ERROR) {
        return CLIP_WAVE;   /* a tap waves back; [face-joy] keeps the hop */
    }
    /* Waiting on Muse in a conversation: a nod ("got it"), then checking the wrist. */
    static float s_checking_since = -1;
    if (p->mode == MUSE_MODE_LISTENING && muse_state_waiting() && muse_state_responder() == MUSE_RESPONDER_MUSE) {
        if (s_checking_since < 0) {
            s_checking_since = p->t;
        }
        const clip_t *nod = &s_clips[CLIP_NOD];
        return p->t - s_checking_since < (float)nod->count / clip_fps(nod) ? CLIP_NOD : CLIP_CHECKING;
    }
    s_checking_since = -1;
    /* Idle: the calm loop, and every IDLE_LOOPS_BETWEEN loops of it a variation,
     * in turn, starting on the loop's boundary (both start and end on the same pose). */
    static float s_idle_since = -1, s_var_until = -1;
    static int s_var;
    if (p->mode == MUSE_MODE_IDLE) {
        if (s_idle_since < 0) {
            s_idle_since = p->t;
        }
        if (p->t < s_var_until) {
            return s_var;
        }
        const clip_t *idle = &s_clips[CLIP_IDLE];
        float loop = (float)idle->count / clip_fps(idle);
        float in = p->t - s_idle_since;
        if (s_clips[CLIP_IDLE_A].first != idle->first && in >= loop * IDLE_LOOPS_BETWEEN &&
            fmodf(in, loop) < 0.15f) {
            s_var = s_var == CLIP_IDLE_A ? CLIP_IDLE_B : CLIP_IDLE_A;
            const clip_t *v = &s_clips[s_var];
            s_var_until = p->t + (float)v->count / clip_fps(v);
            s_idle_since = s_var_until;   /* the next one after more loops */
            return s_var;
        }
    } else {
        s_idle_since = s_var_until = -1;
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

/* Timing, over five seconds from 14 s after boot (past Wi-Fi and the Muse
 * connection, before the 30 s auto-sleep) and every five seconds Muse isn't
 * idle: how often the UI ticks, and how long scaling the frame takes per tick. */
static int64_t s_scale_us;

static void log_ticks(void)
{
    static int s_ticks, s_windows;
    static int64_t s_first, s_last, s_gap_max, s_scale_total, s_scale_max;
    int64_t now = esp_timer_get_time();
    if (now < 14000000 || (s_windows && muse_state_mode(NULL) == MUSE_MODE_IDLE)) {
        s_scale_us = 0;
        s_ticks = 0;
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
        s_windows++;
        s_ticks = 0;
        s_gap_max = s_scale_total = s_scale_max = 0;
    }
}

void muse_pixel_render(const muse_pose_t *p)
{
    if (!open_pack()) {
        return;
    }
    log_ticks();
    /* A clip starts from its first frame when it comes on, fading in over
     * what was shown (but not the first clip, over the blank canvas). */
    static int s_clip = -1, s_gen;
    static float s_since;
    static bool s_fade;
    int clip = clip_for(p);
    if (clip != s_clip || s_trick_again) {
        s_fade = s_clip >= 0;
        if (s_fade) {
            s_switched_at = p->t;
        }
        s_gen += s_fade;
        s_clip = clip;
        s_since = p->t;
    }
    const clip_t *c = &s_clips[clip];
    int t = (int)((p->t - s_since) * clip_fps(c));
    int n = c->flags & 1 ? (t < c->count ? t : c->count - 1) : t % c->count;
    int mix = s_fade && t < FADE_FRAMES ? FADE_FRAMES - t : 0;
    int want = WANT(c->first + n, mix, s_gen);
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
    if (want != s_shown) {
        atomic_store(&s_want, want);
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
