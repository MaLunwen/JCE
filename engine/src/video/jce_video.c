/*
 * jce_video.c  MPEG-1 video implementation backed by pl_mpeg.
 *
 * Designed to mirror jce_audio.c: a small fixed pool of clip slots,
 * each owning a plm_t decoder, a decompressed RGBA frame buffer, and
 * some playback state.  Audio decode is disabled in this revision —
 * the editor preview path consumes only the video track.  Hooking the
 * audio stream into jce_audio is left as a follow-up; the API is
 * already shaped to carry the metadata when that lands.
 */

#include <jce/video/jce_video.h>
#include <jce/core/jce_log.h>

#ifndef JCE_NO_VIDEO

#define PLM_NO_STDIO

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include "pl_mpeg.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include "core/jce_memory.h"
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "jce_video"

#define JCE_MAX_VIDEOS 8

/* ── Per-clip state ──────────────────────────────────────────────── */

typedef struct {
    bool        used;

    plm_t      *plm;          /* pl_mpeg decoder */
    uint8_t    *input_copy;   /* owned copy of the input bytes */
    size_t      input_size;

    /* Latest presented frame (RGBA8, tightly packed, width*4 stride). */
    uint8_t    *rgba;
    int         rgba_w;
    int         rgba_h;
    double      frame_time;
    uint64_t    frame_counter;

    /* Metadata. */
    int         width;
    int         height;
    double      framerate;
    double      duration;
    bool        has_audio;
    int         samplerate;
    int         audio_channels;

    bool        loop;
} VideoSlot;

static VideoSlot s_slots[JCE_MAX_VIDEOS];

static VideoSlot *slot_from_handle(JceVideo v)
{
    if (v == JCE_VIDEO_INVALID) return NULL;
    uint32_t idx = (uint32_t)v - 1;
    if (idx >= JCE_MAX_VIDEOS)       return NULL;
    if (!s_slots[idx].used)          return NULL;
    return &s_slots[idx];
}

static int alloc_slot(void)
{
    for (int i = 0; i < JCE_MAX_VIDEOS; ++i)
        if (!s_slots[i].used) return i;
    return -1;
}

/* ── Decoder callbacks ───────────────────────────────────────────── */

static void video_decode_cb(plm_t *plm, plm_frame_t *frame, void *user)
{
    (void)plm;
    VideoSlot *slot = (VideoSlot *)user;
    if (!slot || !frame) return;

    int w = (int)frame->width;
    int h = (int)frame->height;
    if (w <= 0 || h <= 0) return;

    /* (Re)allocate the RGBA scratch buffer if the geometry changed. */
    if (slot->rgba_w != w || slot->rgba_h != h || !slot->rgba) {
        JCE_FREE(slot->rgba);
        slot->rgba = (uint8_t *)JCE_MALLOC((size_t)w * (size_t)h * 4u);
        if (!slot->rgba) {
            slot->rgba_w = slot->rgba_h = 0;
            LOG_ERROR(LOG_TAG, "frame buffer alloc failed %dx%d", w, h);
            return;
        }
        slot->rgba_w = w;
        slot->rgba_h = h;
    }

    plm_frame_to_rgba(frame, slot->rgba, w * 4);
    slot->frame_time    = frame->time;
    slot->frame_counter++;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

JceVideo jce_video_load_memory(const void *data, uint32_t size,
                                const char *hint_path)
{
    if (!data || size == 0) return JCE_VIDEO_INVALID;

    int idx = alloc_slot();
    if (idx < 0) {
        LOG_WARN(LOG_TAG, "no free video slots");
        return JCE_VIDEO_INVALID;
    }

    VideoSlot *slot = &s_slots[idx];
    memset(slot, 0, sizeof(*slot));

    /* Copy bytes: pl_mpeg holds the pointer for the lifetime of the
     * decoder, so we own it here rather than relying on the caller. */
    slot->input_size = (size_t)size;
    slot->input_copy = (uint8_t *)JCE_MALLOC(slot->input_size);
    if (!slot->input_copy) {
        LOG_ERROR(LOG_TAG, "input copy alloc failed (%u bytes)", size);
        return JCE_VIDEO_INVALID;
    }
    memcpy(slot->input_copy, data, slot->input_size);

    /* free_when_done = 0 — we free the buffer ourselves in unload,
     * so that a failed plm_create path is still recoverable. */
    slot->plm = plm_create_with_memory(slot->input_copy,
                                        slot->input_size, 0);
    if (!slot->plm) {
        LOG_ERROR(LOG_TAG, "plm_create_with_memory failed for '%s'",
                  hint_path ? hint_path : "<memory>");
        JCE_FREE(slot->input_copy);
        slot->input_copy = NULL;
        return JCE_VIDEO_INVALID;
    }

    /* Video-only preview path: skip audio decode to keep things cheap.
     * The metadata we probe below still reflects what the stream reports. */
    slot->has_audio       = plm_get_num_audio_streams(slot->plm) > 0;
    plm_set_audio_enabled(slot->plm, 0);
    plm_set_video_enabled(slot->plm, 1);

    slot->width      = plm_get_width(slot->plm);
    slot->height     = plm_get_height(slot->plm);
    slot->framerate  = plm_get_framerate(slot->plm);
    slot->duration   = plm_get_duration(slot->plm);
    slot->samplerate = slot->has_audio ? plm_get_samplerate(slot->plm) : 0;
    slot->audio_channels = slot->has_audio ? 2 : 0; /* MP2 is always stereo here */

    plm_set_video_decode_callback(slot->plm, video_decode_cb, slot);

    /* Decode exactly one frame so the first render has something to show. */
    plm_frame_t *first = plm_decode_video(slot->plm);
    if (first)
        video_decode_cb(slot->plm, first, slot);

    slot->used = true;
    LOG_SUCCESS(LOG_TAG,
        "loaded '%s' %dx%d %.2ffps dur=%.2fs audio=%d",
        hint_path ? hint_path : "<memory>",
        slot->width, slot->height, slot->framerate,
        slot->duration, (int)slot->has_audio);

    return (JceVideo)(idx + 1);
}

void jce_video_unload(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot) return;

    if (slot->plm) {
        plm_destroy(slot->plm);
        slot->plm = NULL;
    }
    if (slot->input_copy) {
        JCE_FREE(slot->input_copy);
        slot->input_copy = NULL;
    }
    if (slot->rgba) {
        JCE_FREE(slot->rgba);
        slot->rgba = NULL;
    }

    memset(slot, 0, sizeof(*slot));
}

/* ── Queries ──────────────────────────────────────────────────────── */

bool jce_video_get_info(JceVideo v, JceVideoInfo *out)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot || !out) return false;

    out->width          = slot->width;
    out->height         = slot->height;
    out->duration       = slot->duration;
    out->framerate      = slot->framerate;
    out->has_audio      = slot->has_audio;
    out->samplerate     = slot->samplerate;
    out->audio_channels = slot->audio_channels;
    return true;
}

double jce_video_get_time(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot || !slot->plm) return 0.0;
    return plm_get_time(slot->plm);
}

double jce_video_get_duration(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    return slot ? slot->duration : 0.0;
}

bool jce_video_has_ended(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot || !slot->plm) return true;
    return plm_has_ended(slot->plm) != 0;
}

void jce_video_get_size(JceVideo v, int *out_w, int *out_h)
{
    VideoSlot *slot = slot_from_handle(v);
    if (out_w) *out_w = slot ? slot->width  : 0;
    if (out_h) *out_h = slot ? slot->height : 0;
}

/* ── Transport ────────────────────────────────────────────────────── */

void jce_video_advance(JceVideo v, double dt_seconds)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot || !slot->plm) return;
    if (dt_seconds <= 0.0) return;

    /* pl_mpeg clamps internally; clamp here as a safety rail so a
     * giant dt (tab regaining focus after sleep) doesn't burn the CPU
     * decoding the whole clip at once. */
    if (dt_seconds > 1.0) dt_seconds = 1.0;

    plm_decode(slot->plm, dt_seconds);
}

void jce_video_seek(JceVideo v, double time_sec, bool exact)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot || !slot->plm) return;

    if (time_sec < 0.0)                  time_sec = 0.0;
    if (slot->duration > 0.0 &&
        time_sec > slot->duration)       time_sec = slot->duration;

    /* plm_seek will invoke the video callback with the target frame
     * on success, which keeps the RGBA buffer in sync. */
    plm_seek(slot->plm, time_sec, exact ? 1 : 0);
}

void jce_video_rewind(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot || !slot->plm) return;
    plm_rewind(slot->plm);

    /* Surface the first frame again so the preview doesn't go blank. */
    plm_frame_t *f = plm_decode_video(slot->plm);
    if (f)
        video_decode_cb(slot->plm, f, slot);
}

void jce_video_set_loop(JceVideo v, bool loop)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot || !slot->plm) return;
    slot->loop = loop;
    plm_set_loop(slot->plm, loop ? 1 : 0);
}

/* ── Frame access ─────────────────────────────────────────────────── */

const uint8_t *jce_video_get_frame_rgba(JceVideo v,
                                         int *out_w, int *out_h,
                                         double *out_frame_time)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot) {
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        if (out_frame_time) *out_frame_time = 0.0;
        return NULL;
    }

    if (out_w)          *out_w          = slot->rgba_w;
    if (out_h)          *out_h          = slot->rgba_h;
    if (out_frame_time) *out_frame_time = slot->frame_time;
    return slot->rgba;
}

uint64_t jce_video_get_frame_counter(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    return slot ? slot->frame_counter : 0;
}

#else /* JCE_NO_VIDEO */

JceVideo jce_video_load_memory(const void *data, uint32_t size,
                                const char *hint_path)
{
    (void)data; (void)size; (void)hint_path;
    return JCE_VIDEO_INVALID;
}
void jce_video_unload(JceVideo v) { (void)v; }
bool jce_video_get_info(JceVideo v, JceVideoInfo *out)
{
    (void)v;
    if (out) memset(out, 0, sizeof(*out));
    return false;
}
double jce_video_get_time(JceVideo v)     { (void)v; return 0.0; }
double jce_video_get_duration(JceVideo v) { (void)v; return 0.0; }
bool   jce_video_has_ended(JceVideo v)    { (void)v; return true; }
void   jce_video_get_size(JceVideo v, int *w, int *h)
{
    (void)v; if (w) *w = 0; if (h) *h = 0;
}
void jce_video_advance(JceVideo v, double dt) { (void)v; (void)dt; }
void jce_video_seek(JceVideo v, double t, bool exact)
{ (void)v; (void)t; (void)exact; }
void jce_video_rewind(JceVideo v)   { (void)v; }
void jce_video_set_loop(JceVideo v, bool loop) { (void)v; (void)loop; }
const uint8_t *jce_video_get_frame_rgba(JceVideo v,
                                         int *w, int *h, double *t)
{
    (void)v;
    if (w) *w = 0; if (h) *h = 0; if (t) *t = 0.0;
    return NULL;
}
uint64_t jce_video_get_frame_counter(JceVideo v) { (void)v; return 0; }

#endif /* JCE_NO_VIDEO */
