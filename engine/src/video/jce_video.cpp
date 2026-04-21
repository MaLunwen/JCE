/*
 * jce_video.cpp  MP4 metadata + cross-platform frame decode.
 *
 * Container:   Manual MP4/ISO-BMFF parsing via jce_mp4_parser (all platforms).
 * Decode:      OpenH264 software H.264 decoder (cross-platform).
 */

#include <jce/video/jce_video.h>
#include <jce/video/jce_mp4_parser.h>
#include <jce/video/jce_av1_decode.h>
#include <jce/video/jce_vp8_decode.h>
#include <jce/video/jce_webm_parser.h>

extern "C" {
#include <opus.h>
}
#include <jce/core/jce_log.h>
#include <jce/core/jce_thread.h>
#include "core/jce_memory.h"
#include "jce_aac_decode.h"
#include "jce_h264_decode.h"
#include "jce_h265_decode.h"
#include "jce_yuv_convert.h"

#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>

/* ======================================================================
 *  Shared slot pool
 * ====================================================================== */

#define LOG_TAG       "jce_video"
#define JCE_MAX_VIDEOS 32
#define JCE_VIDEO_AUDIO_PCM_MAX_BYTES (100u * 1024u * 1024u) /* 100 MB */

typedef struct {
    JceMp4Parser        *parser;        /* stays open for sample access */
    JceH264Decoder      *h264;          /* OpenH264 H.264 decoder */
    JceH265Decoder      *h265;          /* AOSP libhevc H.265/HEVC decoder */
    JceAv1Decoder       *av1;           /* dav1d AV1 decoder (royalty-free) */
    JceMp4VideoTrackInfo vtrack;        /* cached video track info */
    uint32_t             sample_idx;    /* next sample to decode */
    bool                 ended;
    uint32_t             width;
    uint32_t             height;
    uint8_t             *mp4_copy;      /* owned copy of raw MP4 data */
    size_t               mp4_copy_size;

    /* AV1 / IVF: dav1d decoder owns these (must outlive decoder). */
    uint8_t             *ivf_copy;      /* owned copy of raw IVF stream */
    size_t               ivf_copy_size;
    uint32_t             av1_fps_num;   /* IVF timebase numerator */
    uint32_t             av1_fps_den;   /* IVF timebase denominator */
    uint32_t             av1_frame_idx; /* index of next decoded AV1 frame */

    /* WebM / Matroska: container demuxer + VP8 or AV1 video decoder.
     * (AV1-in-WebM uses slot->decoder.av1 with packet-driven mode.)
     * Audio is not currently routed through the worker (silent track). */
    JceWebmParser       *webm;
    JceVp8Decoder       *vp8;
    uint8_t             *webm_copy;      /* owned copy of raw WebM stream */
    size_t               webm_copy_size;
    uint64_t             webm_duration_ns;
    uint32_t             webm_frame_idx;
    int                  webm_video_codec; /* JceWebmVideoCodec */
    int                  webm_audio_codec; /* JceWebmAudioCodec */
    uint32_t             webm_audio_channels;
    uint32_t             webm_audio_samplerate;

    /* Pre-built keyframe index for O(log n) seek. */
    uint32_t            *keyframe_indices;  /* sorted array of keyframe sample indices */
    uint32_t             keyframe_count;
    uint32_t             last_seek_kf_idx;  /* last keyframe used for inexact seek (skip dup) */
} JceVideoDecoder;

typedef struct {
    bool     used;

    int      width;
    int      height;
    double   framerate;
    double   duration;

    bool     has_audio;
    int      samplerate;
    int      audio_channels;
    uint32_t audio_sample_count;
    JceVideoAudioStatus audio_status;
    char     audio_status_text[128];

    bool     metadata_only;
    char     video_codec[5];
    char     audio_codec[5];

    bool     loop;
    bool     ended;
    double   time;

    uint8_t *rgba;
    int      rgba_w;
    int      rgba_h;
    double   frame_time;
    uint64_t frame_counter;
    bool     decode_ts_base_set;
    double   decode_ts_base_sec;
    bool     post_seek;          /* suppress monotonicity in normalize_frame_time */

    /* Decoded embedded audio (full-clip s16 PCM, owned). */
    int16_t *audio_pcm;
    uint32_t audio_pcm_frames;
    uint32_t audio_pcm_channels;
    uint32_t audio_pcm_samplerate;

    JceVideoDecoder decoder;

    /* Async audio decode (background thread). */
    JceTask *audio_task;
} VideoSlot;

static VideoSlot s_slots[JCE_MAX_VIDEOS];

/* Lazy-init 1-worker pool for async audio decode. */
static JceThreadPool *s_audio_pool = NULL;

static JceThreadPool *get_audio_pool(void)
{
    if (!s_audio_pool)
        s_audio_pool = jce_thread_pool_create(2); /* 2 = 1 background worker + main */
    return s_audio_pool;
}

/* Context passed to the background audio decode thread. */
typedef struct {
    VideoSlot   *slot;
    uint8_t     *mp4_copy;          /* owned copy of MP4 data */
    size_t       mp4_copy_size;
    uint8_t     *decoder_config;    /* owned copy of ASC blob */
    uint32_t     decoder_config_bytes;
    uint32_t     sample_count;
    uint32_t     channels_est;
    uint32_t     samplerate_hz;
} AudioDecodeCtx;

/* Context for async WebM Opus decode. */
typedef struct {
    VideoSlot *slot;
    uint8_t   *webm_copy;
    size_t     webm_copy_size;
    uint32_t   channels;
} WebmOpusDecodeCtx;

/* Context for async MP4 Opus decode (Opus packets stored as MP4 samples). */
typedef struct {
    VideoSlot *slot;
    uint8_t   *mp4_copy;
    size_t     mp4_copy_size;
    uint32_t   channels;
    uint32_t   samplerate_hz;     /* From OpusSpecificBox, typically 48000. */
    uint32_t   sample_count;      /* Audio sample (== Opus packet) count. */
} Mp4OpusDecodeCtx;

/* Forward decls so the worker can call them (defined later in the file). */
static bool webm_decode_opus_to_pcm(const void *data, size_t size,
                                    uint32_t channels,
                                    int16_t **out_pcm,
                                    uint32_t *out_frames,
                                    uint32_t *out_samplerate);
static void set_audio_status(VideoSlot *slot,
                             JceVideoAudioStatus status,
                             const char *text);

static void webm_opus_decode_worker(void *arg)
{
    WebmOpusDecodeCtx *ctx = (WebmOpusDecodeCtx *)arg;
    VideoSlot *slot = ctx->slot;

    int16_t *pcm = NULL;
    uint32_t frames = 0, sr = 0;
    if (webm_decode_opus_to_pcm(ctx->webm_copy, ctx->webm_copy_size,
                                ctx->channels, &pcm, &frames, &sr)) {
        slot->audio_pcm            = pcm;
        slot->audio_pcm_frames     = frames;
        slot->audio_pcm_channels   = ctx->channels;
        slot->audio_pcm_samplerate = sr;
        slot->has_audio            = true;
        slot->samplerate           = (int)sr;
        slot->audio_channels       = (int)ctx->channels;
        slot->audio_sample_count   = frames;
        set_audio_status(slot, JCE_VIDEO_AUDIO_STATUS_READY,
                         "WebM Opus decoded to PCM (async)");
    } else {
        set_audio_status(slot, JCE_VIDEO_AUDIO_STATUS_DEMUX_ERROR,
                         "async: WebM Opus decode failed");
    }
    JCE_FREE(ctx->webm_copy);
    JCE_FREE(ctx);
}

/* Decode all Opus packets stored inside an MP4 audio track into an
 * interleaved s16 PCM buffer. Mirrors webm_decode_opus_to_pcm but uses
 * the MP4 parser to walk audio samples. */
static void mp4_opus_decode_worker(void *arg)
{
    Mp4OpusDecodeCtx *ctx = (Mp4OpusDecodeCtx *)arg;
    VideoSlot *slot = ctx->slot;
    int16_t *pcm = NULL;

    JceMp4Info dummy;
    JceMp4Parser *parser = jce_mp4_parser_open_memory(
        ctx->mp4_copy, ctx->mp4_copy_size, &dummy);
    if (!parser) {
        set_audio_status(slot, JCE_VIDEO_AUDIO_STATUS_DEMUX_ERROR,
                         "async: failed to re-open MP4 for Opus audio");
        goto cleanup;
    }

    {
        uint32_t channels = ctx->channels ? ctx->channels : 2u;
        if (channels > 8u) channels = 8u;

        int err = 0;
        OpusDecoder *od = opus_decoder_create(48000, (int)channels, &err);
        if (!od || err != OPUS_OK) {
            if (od) opus_decoder_destroy(od);
            set_audio_status(slot, JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                             "async: opus_decoder_create failed for MP4 Opus");
            jce_mp4_parser_close(parser);
            goto cleanup;
        }

        enum { MAX_FRAME = 5760 };
        int16_t scratch[MAX_FRAME * 8];

        /* Estimate capacity from sample_count * MAX_FRAME, capped. */
        uint64_t est_frames = (uint64_t)ctx->sample_count * 960ull; /* 20ms typical */
        if (est_frames < 48000ull) est_frames = 48000ull;
        const uint64_t MAX_INIT_FRAMES = 4u * 1024u * 1024u;
        if (est_frames > MAX_INIT_FRAMES) est_frames = MAX_INIT_FRAMES;
        size_t cap = (size_t)est_frames * channels;
        pcm = (int16_t *)JCE_MALLOC(cap * sizeof(int16_t));
        if (!pcm) {
            opus_decoder_destroy(od);
            jce_mp4_parser_close(parser);
            set_audio_status(slot, JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                             "async: out of memory for MP4 Opus decode");
            goto cleanup;
        }
        size_t pos = 0;
        const size_t HARD_CAP_FRAMES = 5ull * 60ull * 48000ull;

        uint8_t *sbuf = NULL;
        uint32_t sbuf_cap = 0;
        for (uint32_t si = 0; si < ctx->sample_count; ++si) {
            JceMp4SampleInfo sinfo;
            if (!jce_mp4_parser_get_audio_sample(parser, si, &sinfo)) break;
            if (sinfo.size_bytes > sbuf_cap) {
                JCE_FREE(sbuf);
                sbuf_cap = sinfo.size_bytes + 256u;
                sbuf = (uint8_t *)JCE_MALLOC(sbuf_cap);
                if (!sbuf) break;
            }
            uint32_t copied = 0;
            if (!jce_mp4_parser_copy_audio_sample(parser, si, sbuf, sbuf_cap, &copied))
                break;
            int got = opus_decode(od, sbuf, (opus_int32)copied,
                                  scratch, MAX_FRAME, 0);
            if (got <= 0) continue;
            if ((pos / channels) + (size_t)got > HARD_CAP_FRAMES) {
                LOG_WARN(LOG_TAG,
                    "MP4 Opus decode hit hard cap (%zu frames) — bailing",
                    (size_t)HARD_CAP_FRAMES);
                break;
            }
            size_t need = pos + (size_t)got * channels;
            if (need > cap) {
                size_t new_cap = cap * 2u;
                if (new_cap < need) new_cap = need;
                int16_t *re = (int16_t *)JCE_REALLOC(pcm, new_cap * sizeof(int16_t));
                if (!re) { JCE_FREE(sbuf); break; }
                pcm = re; cap = new_cap;
            }
            memcpy(pcm + pos, scratch, (size_t)got * channels * sizeof(int16_t));
            pos = need;
        }
        JCE_FREE(sbuf);
        opus_decoder_destroy(od);
        jce_mp4_parser_close(parser);

        if (pos == 0) {
            JCE_FREE(pcm); pcm = NULL;
            set_audio_status(slot, JCE_VIDEO_AUDIO_STATUS_DEMUX_ERROR,
                             "async: MP4 Opus produced no PCM");
            goto cleanup;
        }
        int16_t *trimmed = (int16_t *)JCE_REALLOC(pcm, pos * sizeof(int16_t));
        if (trimmed) pcm = trimmed;
        uint32_t frames = (uint32_t)(pos / channels);
        slot->audio_pcm            = pcm;
        slot->audio_pcm_frames     = frames;
        slot->audio_pcm_channels   = channels;
        slot->audio_pcm_samplerate = 48000u;
        slot->has_audio            = true;
        slot->samplerate           = 48000;
        slot->audio_channels       = (int)channels;
        slot->audio_sample_count   = frames;
        pcm = NULL; /* ownership transferred to slot */
        set_audio_status(slot, JCE_VIDEO_AUDIO_STATUS_READY,
                         "MP4 Opus decoded to PCM (async)");
        LOG_INFO(LOG_TAG,
            "MP4 Opus decoded %u packets -> %u frames (48000Hz x %uch)",
            ctx->sample_count, frames, channels);
    }

cleanup:
    JCE_FREE(pcm);
    JCE_FREE(ctx->mp4_copy);
    JCE_FREE(ctx);
}

static void audio_decode_worker(void *arg)
{
    AudioDecodeCtx *ctx = (AudioDecodeCtx *)arg;
    VideoSlot *slot = ctx->slot;

    JceMp4Info dummy;
    JceMp4Parser *parser = jce_mp4_parser_open_memory(
        ctx->mp4_copy, ctx->mp4_copy_size, &dummy);
    if (!parser) {
        set_audio_status(slot, JCE_VIDEO_AUDIO_STATUS_DEMUX_ERROR,
                         "async: failed to re-open MP4 for audio");
        goto cleanup;
    }

    {
        JceAacDecoder *aac = jce_aac_decoder_open(
            ctx->decoder_config, ctx->decoder_config_bytes);
        if (!aac) {
            set_audio_status(slot,
                JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                "async: failed to initialize AAC decoder");
            jce_mp4_parser_close(parser);
            goto cleanup;
        }

        uint32_t ch_est      = ctx->channels_est > 0 ? ctx->channels_est : 2u;
        uint32_t max_frame   = 2048u;
        uint64_t est_samples = (uint64_t)ctx->sample_count * max_frame * ch_est;
        uint64_t est_bytes   = est_samples * sizeof(int16_t);

        if (est_bytes > JCE_VIDEO_AUDIO_PCM_MAX_BYTES) {
            set_audio_status(slot,
                JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                "async: audio track exceeds memory limit");
            jce_aac_decoder_close(aac);
            jce_mp4_parser_close(parser);
            goto cleanup;
        }

        int16_t *pcm_buf = (int16_t *)JCE_MALLOC((size_t)est_bytes);
        if (!pcm_buf) {
            set_audio_status(slot,
                JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                "async: out of memory for audio decode");
            jce_aac_decoder_close(aac);
            jce_mp4_parser_close(parser);
            goto cleanup;
        }

        uint32_t pcm_pos = 0;
        uint32_t pcm_cap = (uint32_t)est_samples;
        bool     ok      = true;
        uint8_t *sbuf    = NULL;
        uint32_t sbuf_cap = 0;

        for (uint32_t si = 0; si < ctx->sample_count; ++si) {
            JceMp4SampleInfo sinfo;
            if (!jce_mp4_parser_get_audio_sample(parser, si, &sinfo)) {
                ok = false;
                break;
            }
            if (sinfo.size_bytes > sbuf_cap) {
                JCE_FREE(sbuf);
                sbuf_cap = sinfo.size_bytes + 256u;
                sbuf = (uint8_t *)JCE_MALLOC(sbuf_cap);
                if (!sbuf) { ok = false; break; }
            }
            uint32_t copied = 0;
            if (!jce_mp4_parser_copy_audio_sample(
                    parser, si, sbuf, sbuf_cap, &copied)) {
                ok = false;
                break;
            }
            uint32_t samp_out = 0;
            uint32_t remain   = pcm_cap - pcm_pos;
            if (!jce_aac_decode_frame(
                    aac, sbuf, copied,
                    pcm_buf + pcm_pos, remain, &samp_out)) {
                continue;
            }
            pcm_pos += samp_out;
        }
        JCE_FREE(sbuf);

        if (ok && pcm_pos > 0) {
            uint32_t ach = jce_aac_decoder_get_channels(aac);
            uint32_t asr = jce_aac_decoder_get_samplerate(aac);
            if (ach == 0) ach = ch_est;
            if (asr == 0) asr = ctx->samplerate_hz;
            uint32_t aframes = pcm_pos / ach;

            size_t actual = (size_t)pcm_pos * sizeof(int16_t);
            int16_t *trimmed = (int16_t *)JCE_REALLOC(pcm_buf, actual);
            if (trimmed) pcm_buf = trimmed;

            slot->audio_pcm            = pcm_buf;
            slot->audio_pcm_frames     = aframes;
            slot->audio_pcm_channels   = ach;
            slot->audio_pcm_samplerate = asr;
            set_audio_status(slot,
                JCE_VIDEO_AUDIO_STATUS_READY,
                "AAC audio decoded to PCM (async)");
            pcm_buf = NULL;
        } else {
            set_audio_status(slot,
                JCE_VIDEO_AUDIO_STATUS_DEMUX_ERROR,
                "async: failed to decode AAC audio samples");
        }
        JCE_FREE(pcm_buf);
        jce_aac_decoder_close(aac);
        jce_mp4_parser_close(parser);
    }

cleanup:
    JCE_FREE(ctx->mp4_copy);
    JCE_FREE(ctx->decoder_config);
    JCE_FREE(ctx);
}

static void set_audio_status(VideoSlot *slot,
                             JceVideoAudioStatus status,
                             const char *text)
{
    if (!slot) {
        return;
    }
    slot->audio_status = status;
    snprintf(slot->audio_status_text,
             sizeof(slot->audio_status_text),
             "%s",
             text ? text : "");
}

static VideoSlot *slot_from_handle(JceVideo v)
{
    if (v == JCE_VIDEO_INVALID) return NULL;
    uint32_t idx = (uint32_t)v - 1u;
    if (idx >= JCE_MAX_VIDEOS) return NULL;
    if (!s_slots[idx].used) return NULL;
    return &s_slots[idx];
}

static int alloc_slot(void)
{
    for (int i = 0; i < JCE_MAX_VIDEOS; ++i)
        if (!s_slots[i].used) return i;
    return -1;
}

/* ======================================================================
 *  Shared RGBA buffer helper
 * ====================================================================== */

static bool ensure_rgba(VideoSlot *slot, uint32_t w, uint32_t h)
{
    if (!slot || w == 0u || h == 0u) return false;
    if (slot->rgba && slot->rgba_w == (int)w && slot->rgba_h == (int)h)
        return true;

    size_t need = (size_t)w * (size_t)h * 4u;
    JCE_FREE(slot->rgba);
    slot->rgba = (uint8_t *)JCE_MALLOC(need);
    if (!slot->rgba) { slot->rgba_w = 0; slot->rgba_h = 0; return false; }
    slot->rgba_w = (int)w;
    slot->rgba_h = (int)h;
    return true;
}

static double frame_interval_seconds(const VideoSlot *slot)
{
    double fps = slot ? slot->framerate : 0.0;
    if (!(fps >= 1.0 && fps <= 240.0)) {
        fps = 30.0;
    }
    return 1.0 / fps;
}

/*
 * Normalize backend timestamps to a monotonic local playback clock.
 * Some files expose large/non-zero starts or repeated timestamps,
 * which would otherwise stall frame advancement logic.
 */
static double normalize_frame_time(VideoSlot *slot,
                                   double raw_time_sec,
                                   bool has_raw_time)
{
    if (!slot) {
        return 0.0;
    }

    if (slot->frame_counter == 0u) {
        if (has_raw_time && raw_time_sec >= 0.0) {
            slot->decode_ts_base_set = true;
            slot->decode_ts_base_sec = raw_time_sec;
        } else {
            slot->decode_ts_base_set = false;
            slot->decode_ts_base_sec = 0.0;
        }
        return 0.0;
    }

    double t = slot->frame_time + frame_interval_seconds(slot);
    if (has_raw_time && raw_time_sec >= 0.0) {
        if (!slot->decode_ts_base_set) {
            slot->decode_ts_base_set = true;
            slot->decode_ts_base_sec = raw_time_sec;
        }
        t = raw_time_sec - slot->decode_ts_base_sec;
        if (t < 0.0) {
            t = 0.0;
        }
        /* Only enforce monotonicity during normal playback, not after
         * a seek — backward seeks legitimately produce earlier timestamps. */
        if (!slot->post_seek && t <= slot->frame_time + 0.0005) {
            t = slot->frame_time + frame_interval_seconds(slot);
        }
    }
    return t;
}

/* ======================================================================
 *  AV1 / IVF backend (royalty-free path)
 * ====================================================================== */

static bool av1_decoder_open(VideoSlot *slot, const void *data, uint32_t size)
{
    if (!slot || !data || size < 32u) return false;

    /* dav1d decoder reads from caller-owned memory; copy so the
     * source buffer (which the caller may free) stays valid. */
    slot->decoder.ivf_copy = (uint8_t *)JCE_MALLOC(size);
    if (!slot->decoder.ivf_copy) return false;
    memcpy(slot->decoder.ivf_copy, data, size);
    slot->decoder.ivf_copy_size = (size_t)size;

    JceAv1FrameInfo info = {0};
    slot->decoder.av1 = jce_av1_open_memory(
        slot->decoder.ivf_copy, slot->decoder.ivf_copy_size, &info);
    if (!slot->decoder.av1) {
        JCE_FREE(slot->decoder.ivf_copy);
        slot->decoder.ivf_copy = NULL;
        slot->decoder.ivf_copy_size = 0;
        return false;
    }

    slot->decoder.width        = info.width;
    slot->decoder.height       = info.height;
    slot->decoder.av1_fps_num  = info.fps_num;
    slot->decoder.av1_fps_den  = info.fps_den;
    slot->decoder.av1_frame_idx = 0;
    slot->decoder.ended        = false;

    /* Pull the first frame so RGBA/size are populated immediately. */
    const uint8_t *yp = NULL, *up = NULL, *vp = NULL;
    ptrdiff_t ys = 0, uvs = 0;
    uint32_t fw = 0, fh = 0;
    if (jce_av1_decode_next(slot->decoder.av1,
                            &yp, &ys, &up, &uvs, &vp, &fw, &fh)) {
        if (fw > 0 && fh > 0 && ensure_rgba(slot, fw, fh)) {
            slot->decoder.width  = fw;
            slot->decoder.height = fh;
            jce_yuv420_to_rgba(yp, (int)ys, up, (int)uvs, vp, (int)uvs,
                               slot->rgba, fw, fh);
            slot->frame_counter = 1;
            slot->decoder.av1_frame_idx = 1;
        }
    }
    slot->frame_time = normalize_frame_time(slot, 0.0, true);
    return true;
}

static double av1_frame_interval(const VideoSlot *slot)
{
    uint32_t num = slot->decoder.av1_fps_num;
    uint32_t den = slot->decoder.av1_fps_den;
    if (num == 0u || den == 0u) return 1.0 / 30.0;
    /* IVF timebase is reciprocal of fps: dt = den / num. */
    return (double)den / (double)num;
}

static bool av1_decoder_read_next(VideoSlot *slot)
{
    if (!slot || !slot->decoder.av1) return false;

    const uint8_t *yp = NULL, *up = NULL, *vp = NULL;
    ptrdiff_t ys = 0, uvs = 0;
    uint32_t fw = 0, fh = 0;
    if (!jce_av1_decode_next(slot->decoder.av1,
                             &yp, &ys, &up, &uvs, &vp, &fw, &fh)) {
        slot->decoder.ended = true;
        return false;
    }
    if (fw == 0u || fh == 0u || !ensure_rgba(slot, fw, fh)) return false;
    slot->decoder.width  = fw;
    slot->decoder.height = fh;
    jce_yuv420_to_rgba(yp, (int)ys, up, (int)uvs, vp, (int)uvs,
                       slot->rgba, fw, fh);

    /* IVF has no PTS table here — synthesize using IVF timebase and the
     * decoded-frame index. normalize_frame_time enforces monotonicity. */
    double ts = (double)slot->decoder.av1_frame_idx * av1_frame_interval(slot);
    slot->frame_time = normalize_frame_time(slot, ts, true);
    slot->decoder.av1_frame_idx++;
    slot->frame_counter++;
    slot->decoder.ended = false;
    return true;
}

static bool av1_decoder_seek(VideoSlot *slot, double time_sec, bool /*exact*/)
{
    /* IVF has no random-access index; emulate seek by tearing down and
     * decoding from the start. Inexact seeks behave the same as exact. */
    if (!slot || !slot->decoder.av1) return false;
    if (time_sec < 0.0) time_sec = 0.0;

    jce_av1_close(slot->decoder.av1);
    slot->decoder.av1 = NULL;

    JceAv1FrameInfo info = {0};
    slot->decoder.av1 = jce_av1_open_memory(
        slot->decoder.ivf_copy, slot->decoder.ivf_copy_size, &info);
    if (!slot->decoder.av1) {
        slot->decoder.ended = true;
        return false;
    }
    slot->decoder.av1_frame_idx = 0;
    slot->decoder.ended = false;
    slot->frame_counter = 0;
    slot->decode_ts_base_set = false;
    slot->time = time_sec;
    slot->frame_time = 0.0;

    /* Decode forward until we land on (or just past) the target time.
     * Bounded to keep UI responsive — caller can resume on next tick. */
    double interval = av1_frame_interval(slot);
    if (interval <= 0.0) interval = 1.0 / 30.0;
    uint32_t target_idx = (uint32_t)(time_sec / interval);
    auto start = std::chrono::steady_clock::now();
    while (slot->decoder.av1_frame_idx < target_idx + 1u) {
        if (!av1_decoder_read_next(slot)) break;
        if (std::chrono::steady_clock::now() - start
            > std::chrono::milliseconds(100)) break;
    }
    slot->frame_time = time_sec;
    return true;
}

static void av1_decoder_close(VideoSlot *slot)
{
    if (!slot) return;
    if (slot->decoder.av1) {
        jce_av1_close(slot->decoder.av1);
        slot->decoder.av1 = NULL;
    }
    if (slot->decoder.ivf_copy) {
        JCE_FREE(slot->decoder.ivf_copy);
        slot->decoder.ivf_copy = NULL;
        slot->decoder.ivf_copy_size = 0;
    }
}

/* ---- MP4-AV1 (av01-in-MP4 / .mp4 with AV1) ----------------------------
 *  Sample data in MP4 av01 is the raw OBU bitstream — feed it directly
 *  to dav1d via jce_av1_decode_packet().  Container parser owns sample
 *  index + timing.  Discriminator vs IVF/WebM: parser != NULL && av1 !=
 *  NULL && !webm && ivf_copy == NULL.
 */
static bool mp4_av1_is_active(const VideoSlot *slot)
{
    return slot && slot->decoder.av1 && slot->decoder.parser
        && !slot->decoder.webm && !slot->decoder.ivf_copy;
}

static bool mp4_av1_decode_one_sample(VideoSlot *slot, uint32_t sample_idx,
                                      bool *out_got_frame)
{
    JceMp4SampleInfo si;
    if (!jce_mp4_parser_get_video_sample(slot->decoder.parser, sample_idx, &si))
        return false;

    const uint8_t *yp = NULL, *up = NULL, *vp = NULL;
    ptrdiff_t ys = 0, uvs = 0;
    uint32_t fw = 0, fh = 0;
    bool got = jce_av1_decode_packet(
        slot->decoder.av1,
        slot->decoder.mp4_copy + si.offset, si.size_bytes,
        &yp, &ys, &up, &uvs, &vp, &fw, &fh);
    if (out_got_frame) *out_got_frame = got;
    if (!got) return true;

    if (fw == 0u || fh == 0u || !ensure_rgba(slot, fw, fh)) return false;
    slot->decoder.width  = fw;
    slot->decoder.height = fh;
    jce_yuv420_to_rgba(yp, (int)ys, up, (int)uvs, vp, (int)uvs,
                       slot->rgba, fw, fh);

    double ts_sec = 0.0;
    if (slot->decoder.vtrack.timescale > 0) {
        ts_sec = (double)si.timestamp / (double)slot->decoder.vtrack.timescale;
    }
    slot->frame_time = normalize_frame_time(slot, ts_sec, true);
    slot->frame_counter++;
    slot->decoder.ended = false;
    return true;
}

/* Send the AV1 configOBUs (sequence-header) embedded in the av1C box to
 * dav1d. ISOBMFF-AV1 stores the seq_header only in av1C.configOBUs (NOT
 * inside each sample like WebM does), so without this dav1d would reject
 * sample data with "Error parsing OBU data" / -EINVAL. */
static void mp4_av1_prime_seq_header(VideoSlot *slot)
{
    const uint8_t *dsi = (const uint8_t *)slot->decoder.vtrack.decoder_config;
    uint32_t       sz  = slot->decoder.vtrack.decoder_config_bytes;
    /* av1C box layout: 4-byte AV1CodecConfigurationRecord header,
     * followed by configOBUs[] (raw OBU bytestream). */
    if (!dsi || sz <= 4u) return;
    const uint8_t *obus = dsi + 4;
    size_t         len  = (size_t)(sz - 4u);
    bool got = false;
    /* Decode-packet returns whether a *picture* came out; for a header-only
     * push we don't expect a picture, just want dav1d to absorb the OBUs. */
    (void)got;
    const uint8_t *yp = NULL, *up = NULL, *vp = NULL;
    ptrdiff_t ys = 0, uvs = 0;
    uint32_t fw = 0, fh = 0;
    jce_av1_decode_packet(slot->decoder.av1, obus, len,
                          &yp, &ys, &up, &uvs, &vp, &fw, &fh);
}

/* ── AV1 OBU keyframe scanner ─────────────────────────────────────
 * AV1-in-ISOBMFF samples are raw OBU bytestreams in the "low overhead"
 * format where each OBU has obu_has_size_field=1. Walk OBUs in a sample
 * and decide if it's a random-access point (contains OBU_SEQUENCE_HEADER,
 * type==1, OR an OBU_FRAME (type==6) whose frame_type==KEY_FRAME (0)).
 * Returns true on a keyframe sample. */
static bool av1_uleb128_read(const uint8_t *p, size_t avail,
                             uint64_t *out_val, size_t *out_len)
{
    uint64_t val = 0;
    size_t   i = 0;
    for (; i < 8 && i < avail; ++i) {
        uint8_t b = p[i];
        val |= ((uint64_t)(b & 0x7Fu)) << (7u * i);
        if ((b & 0x80u) == 0u) {
            *out_val = val;
            *out_len = i + 1;
            return true;
        }
    }
    return false;
}

static bool av1_sample_is_keyframe(const uint8_t *data, size_t size)
{
    size_t pos = 0;
    while (pos < size) {
        uint8_t hdr = data[pos++];
        uint8_t obu_type = (uint8_t)((hdr >> 3) & 0x0Fu);
        bool    ext_flag = (hdr & 0x04u) != 0;
        bool    has_size = (hdr & 0x02u) != 0;
        if (ext_flag) {
            if (pos >= size) return false;
            pos++;
        }
        if (obu_type == 1u /* OBU_SEQUENCE_HEADER */) return true;

        uint64_t obu_size = 0;
        if (has_size) {
            size_t lebn = 0;
            if (!av1_uleb128_read(data + pos, size - pos, &obu_size, &lebn))
                return false;
            pos += lebn;
        } else {
            obu_size = (uint64_t)(size - pos);
        }
        if (obu_size > size - pos) return false;

        if (obu_type == 6u /* OBU_FRAME */
            || obu_type == 3u /* OBU_FRAME_HEADER */) {
            /* frame_type lives in the first 1-2 bits after show_existing_frame
             * flag. For seek-index purposes we only need a conservative test:
             * if obu_size > 0 and the first byte's top bit is 0, frame_type
             * occupies bits 6-5 of byte[0]. show_existing_frame=0 → those
             * are frame_type. */
            if (obu_size > 0u) {
                uint8_t b0 = data[pos];
                bool    show_existing = (b0 & 0x80u) != 0;
                if (!show_existing) {
                    uint8_t ftype = (uint8_t)((b0 >> 5) & 0x03u);
                    if (ftype == 0u /* KEY_FRAME */) return true;
                }
            }
        }
        pos += (size_t)obu_size;
    }
    return false;
}

static void mp4_av1_build_keyframe_index(VideoSlot *slot)
{
    slot->decoder.keyframe_indices = NULL;
    slot->decoder.keyframe_count   = 0;
    if (!slot->decoder.parser) return;
    uint32_t total = slot->decoder.vtrack.sample_count;
    if (total == 0u) return;

    uint32_t cap = 32u;
    uint32_t *kf = (uint32_t *)JCE_MALLOC(cap * sizeof(uint32_t));
    if (!kf) return;
    uint32_t cnt = 0;

    for (uint32_t si = 0; si < total; ++si) {
        JceMp4SampleInfo info;
        if (!jce_mp4_parser_get_video_sample(slot->decoder.parser, si, &info))
            continue;
        if (info.size_bytes == 0u) continue;
        const uint8_t *p = slot->decoder.mp4_copy + info.offset;
        size_t scan = info.size_bytes < 512u ? info.size_bytes : 512u;
        if (!av1_sample_is_keyframe(p, scan)) continue;
        if (cnt >= cap) {
            uint32_t new_cap = cap * 2u;
            uint32_t *re = (uint32_t *)JCE_REALLOC(kf, new_cap * sizeof(uint32_t));
            if (!re) break;
            kf = re; cap = new_cap;
        }
        kf[cnt++] = si;
    }
    if (cnt == 0u) { JCE_FREE(kf); return; }
    /* Always treat sample 0 as a random-access point. */
    if (kf[0] != 0u) {
        if (cnt >= cap) {
            uint32_t *re = (uint32_t *)JCE_REALLOC(kf, (cap + 1) * sizeof(uint32_t));
            if (re) { kf = re; cap = cap + 1u; }
        }
        if (cnt < cap) {
            memmove(kf + 1, kf, cnt * sizeof(uint32_t));
            kf[0] = 0u; cnt++;
        }
    }
    slot->decoder.keyframe_indices = kf;
    slot->decoder.keyframe_count   = cnt;
    LOG_INFO(LOG_TAG, "MP4-AV1 keyframe index: %u keyframes / %u samples",
             cnt, total);
}

static bool mp4_av1_decoder_open(VideoSlot *slot)
{
    /* Presumes slot->decoder.parser + slot->decoder.vtrack already set up
     * by decoder_open's MP4 setup. Creates packet-driven dav1d ctx and
     * decodes the first sample so the slot has frame data immediately. */
    slot->decoder.av1 = jce_av1_open_packet();
    if (!slot->decoder.av1) {
        LOG_WARN(LOG_TAG, "mp4_av1_decoder_open: jce_av1_open_packet failed");
        return false;
    }
    slot->decoder.width  = slot->decoder.vtrack.width;
    slot->decoder.height = slot->decoder.vtrack.height;
    slot->decoder.sample_idx = 0;
    slot->decoder.ended = false;
    mp4_av1_prime_seq_header(slot);

    bool got = false;
    if (!mp4_av1_decode_one_sample(slot, 0, &got)) {
        jce_av1_close(slot->decoder.av1);
        slot->decoder.av1 = NULL;
        return false;
    }
    if (!got) {
        slot->frame_counter = 0;
        LOG_INFO(LOG_TAG, "mp4_av1_decoder_open: first frame deferred");
    }
    slot->decoder.sample_idx = 1;
    slot->frame_time = normalize_frame_time(slot, 0.0, true);
    return true;
}

static bool mp4_av1_decoder_read_next(VideoSlot *slot)
{
    if (slot->decoder.sample_idx >= slot->decoder.vtrack.sample_count) {
        slot->decoder.ended = true;
        return false;
    }
    bool got = false;
    bool ok = mp4_av1_decode_one_sample(slot, slot->decoder.sample_idx, &got);
    slot->decoder.sample_idx++;
    return ok && got;
}

static bool mp4_av1_decoder_seek(VideoSlot *slot, double time_sec, bool exact)
{
    /* Fast seek for AV1 in MP4: find nearest keyframe at-or-before target
     * via the prebuilt keyframe_indices, then decode forward from there.
     * Falls back to sample 0 when the index is empty. */
    if (time_sec < 0.0) time_sec = 0.0;
    if (slot->decoder.av1) {
        jce_av1_close(slot->decoder.av1);
        slot->decoder.av1 = NULL;
    }
    slot->decoder.av1 = jce_av1_open_packet();
    if (!slot->decoder.av1) {
        slot->decoder.ended = true;
        return false;
    }

    uint32_t ts_scale = slot->decoder.vtrack.timescale;
    if (ts_scale == 0) ts_scale = 1;
    uint64_t target_ts = (uint64_t)(time_sec * (double)ts_scale);

    /* Find largest keyframe sample whose timestamp <= target_ts. */
    uint32_t kf_sample = 0;
    if (slot->decoder.keyframe_indices && slot->decoder.keyframe_count > 0) {
        const uint32_t *kf = slot->decoder.keyframe_indices;
        uint32_t kn = slot->decoder.keyframe_count;
        for (uint32_t i = 0; i < kn; ++i) {
            JceMp4SampleInfo si;
            if (!jce_mp4_parser_get_video_sample(slot->decoder.parser, kf[i], &si))
                continue;
            if (si.timestamp <= target_ts) kf_sample = kf[i];
            else break;
        }
    }

    slot->decoder.sample_idx = kf_sample;
    slot->decoder.ended = false;
    slot->frame_counter = 0;
    slot->decode_ts_base_set = false;
    slot->time = time_sec;
    slot->frame_time = 0.0;
    /* Seq header may live only in sample 0 / earlier keyframes; if the
     * seek target is past those samples and av1C had configOBUs, prime
     * the decoder so the next sample's frame OBU isn't rejected. */
    mp4_av1_prime_seq_header(slot);

    /* Drain forward from the keyframe to the target. Bounded for inexact
     * (preview) seek so dragging stays smooth; unbounded for exact
     * (release) seek so audio actually aligns. */
    auto start = std::chrono::steady_clock::now();
    while (slot->decoder.sample_idx < slot->decoder.vtrack.sample_count) {
        JceMp4SampleInfo si;
        if (!jce_mp4_parser_get_video_sample(slot->decoder.parser,
                                             slot->decoder.sample_idx, &si))
            break;
        bool got = false;
        mp4_av1_decode_one_sample(slot, slot->decoder.sample_idx, &got);
        slot->decoder.sample_idx++;
        if (si.timestamp >= target_ts) break;
        if (!exact && std::chrono::steady_clock::now() - start
            > std::chrono::milliseconds(50)) break;
    }
    slot->frame_time = time_sec;
    return true;
}

/* ======================================================================
 *  WebM / VP8 backend (royalty-free path)
 *
 *  Currently supports VP8 video; audio (Opus / Vorbis) is detected and
 *  reported in the format header but not yet routed through the audio
 *  worker thread (set_audio_status(NONE)). Track/duration metadata and
 *  coarse seek work end-to-end.
 * ====================================================================== */

static bool webm_decoder_open(VideoSlot *slot, const void *data, uint32_t size)
{
    if (!slot || !data || size == 0u) return false;

    slot->decoder.webm_copy = (uint8_t *)JCE_MALLOC(size);
    if (!slot->decoder.webm_copy) return false;
    memcpy(slot->decoder.webm_copy, data, size);
    slot->decoder.webm_copy_size = (size_t)size;

    JceWebmInfo info{};
    slot->decoder.webm = jce_webm_open_memory(
        slot->decoder.webm_copy, slot->decoder.webm_copy_size, &info);
    if (!slot->decoder.webm) {
        JCE_FREE(slot->decoder.webm_copy);
        slot->decoder.webm_copy = NULL;
        slot->decoder.webm_copy_size = 0;
        return false;
    }

    if (info.video_codec == JCE_WEBM_VIDEO_VP8) {
        slot->decoder.vp8 = jce_vp8_decoder_open();
        if (!slot->decoder.vp8) {
            jce_webm_close(slot->decoder.webm);
            slot->decoder.webm = NULL;
            JCE_FREE(slot->decoder.webm_copy);
            slot->decoder.webm_copy = NULL;
            slot->decoder.webm_copy_size = 0;
            return false;
        }
    } else if (info.video_codec == JCE_WEBM_VIDEO_AV1) {
        slot->decoder.av1 = jce_av1_open_packet();
        if (!slot->decoder.av1) {
            jce_webm_close(slot->decoder.webm);
            slot->decoder.webm = NULL;
            JCE_FREE(slot->decoder.webm_copy);
            slot->decoder.webm_copy = NULL;
            slot->decoder.webm_copy_size = 0;
            return false;
        }
    } else {
        LOG_WARN(LOG_TAG, "WebM video codec %d not supported (VP8/AV1 only)",
                 (int)info.video_codec);
        jce_webm_close(slot->decoder.webm);
        slot->decoder.webm = NULL;
        JCE_FREE(slot->decoder.webm_copy);
        slot->decoder.webm_copy = NULL;
        slot->decoder.webm_copy_size = 0;
        return false;
    }

    slot->decoder.width             = info.width;
    slot->decoder.height            = info.height;
    slot->decoder.webm_duration_ns  = info.duration_ns;
    slot->decoder.webm_frame_idx    = 0;
    slot->decoder.webm_video_codec  = (int)info.video_codec;
    slot->decoder.webm_audio_codec  = (int)info.audio_codec;
    slot->decoder.webm_audio_channels   = info.audio_channels;
    slot->decoder.webm_audio_samplerate = info.audio_samplerate;
    slot->decoder.ended             = false;
    return true;
}

static bool webm_decoder_read_next(VideoSlot *slot)
{
    if (!slot || !slot->decoder.webm) return false;

    const uint8_t *pkt = NULL; size_t pkt_sz = 0;
    uint64_t pts_ns = 0; bool kf = false;
    if (!jce_webm_read_video_packet(slot->decoder.webm,
                                    &pkt, &pkt_sz, &pts_ns, &kf)) {
        slot->decoder.ended = true;
        return false;
    }

    const uint8_t *yp = NULL, *up = NULL, *vp = NULL;
    ptrdiff_t ys = 0, uvs = 0;
    uint32_t fw = 0, fh = 0;
    bool got = false;
    if (slot->decoder.vp8) {
        got = jce_vp8_decode_packet(slot->decoder.vp8, pkt, pkt_sz,
                                    &yp, &ys, &up, &uvs, &vp, &fw, &fh);
    } else if (slot->decoder.av1) {
        got = jce_av1_decode_packet(slot->decoder.av1, pkt, pkt_sz,
                                    &yp, &ys, &up, &uvs, &vp, &fw, &fh);
    }
    if (!got) return false;
    if (fw == 0u || fh == 0u || !ensure_rgba(slot, fw, fh)) return false;
    slot->decoder.width  = fw;
    slot->decoder.height = fh;
    jce_yuv420_to_rgba(yp, (int)ys, up, (int)uvs, vp, (int)uvs,
                       slot->rgba, fw, fh);

    double ts = (double)pts_ns / 1.0e9;
    slot->frame_time = normalize_frame_time(slot, ts, true);
    slot->decoder.webm_frame_idx++;
    slot->frame_counter++;
    slot->decoder.ended = false;
    return true;
}

static bool webm_decoder_seek(VideoSlot *slot, double time_sec, bool /*exact*/)
{
    if (!slot || !slot->decoder.webm) return false;
    if (time_sec < 0.0) time_sec = 0.0;

    uint64_t target_ns = (uint64_t)(time_sec * 1.0e9);
    if (!jce_webm_seek(slot->decoder.webm, target_ns)) return false;

    /* Recreate the video decoder so the next packet (a keyframe) starts
     * fresh — VP8/AV1 both maintain reference frames internally. */
    if (slot->decoder.vp8) {
        jce_vp8_close(slot->decoder.vp8);
        slot->decoder.vp8 = jce_vp8_decoder_open();
        if (!slot->decoder.vp8) { slot->decoder.ended = true; return false; }
    } else if (slot->decoder.av1) {
        jce_av1_close(slot->decoder.av1);
        slot->decoder.av1 = jce_av1_open_packet();
        if (!slot->decoder.av1) { slot->decoder.ended = true; return false; }
    }

    slot->decoder.webm_frame_idx = 0;
    slot->decoder.ended = false;
    slot->frame_counter = 0;
    slot->decode_ts_base_set = false;
    slot->time = time_sec;
    slot->frame_time = 0.0;

    /* Drain forward until we land on a frame at or past the target. */
    auto start = std::chrono::steady_clock::now();
    while (slot->frame_time + 1e-3 < time_sec) {
        if (!webm_decoder_read_next(slot)) break;
        if (std::chrono::steady_clock::now() - start
            > std::chrono::milliseconds(100)) break;
    }
    slot->frame_time = time_sec;
    return true;
}

static void webm_decoder_close(VideoSlot *slot)
{
    if (!slot) return;
    if (slot->decoder.vp8) {
        jce_vp8_close(slot->decoder.vp8);
        slot->decoder.vp8 = NULL;
    }
    if (slot->decoder.webm) {
        jce_webm_close(slot->decoder.webm);
        slot->decoder.webm = NULL;
    }
    if (slot->decoder.webm_copy) {
        JCE_FREE(slot->decoder.webm_copy);
        slot->decoder.webm_copy = NULL;
        slot->decoder.webm_copy_size = 0;
    }
}

/* Decode all Opus packets from a WebM/MKV stream into an interleaved
 * s16 PCM buffer. Spawns a fresh parser instance over `data` so the main
 * video parser is not disturbed. Caller owns *out_pcm. */
static bool webm_decode_opus_to_pcm(const void *data, size_t size,
                                    uint32_t channels,
                                    int16_t **out_pcm,
                                    uint32_t *out_frames,
                                    uint32_t *out_samplerate)
{
    if (!data || size == 0 || channels == 0 || channels > 8) return false;

    JceWebmInfo info{};
    JceWebmParser *p = jce_webm_open_memory(data, size, &info);
    if (!p) return false;
    if (info.audio_codec != JCE_WEBM_AUDIO_OPUS) {
        jce_webm_close(p);
        return false;
    }

    int err = 0;
    OpusDecoder *od = opus_decoder_create(48000, (int)channels, &err);
    if (!od || err != OPUS_OK) {
        if (od) opus_decoder_destroy(od);
        jce_webm_close(p);
        return false;
    }

    enum { MAX_FRAME = 5760 }; /* 120 ms @ 48 kHz */
    int16_t scratch[MAX_FRAME * 8];

    /* Estimate capacity from duration + 20 % slack, but clamp so a
     * pathological/corrupt duration_ns cannot trigger a huge alloc.
     * Cap the initial reservation at 16 MB worth of frames (~85 s @ 48k stereo);
     * the realloc-double strategy will grow it from there as needed. */
    uint64_t est_frames = (info.duration_ns / 1000000ull) * 48ull;
    if (est_frames < 48000) est_frames = 48000;
    est_frames = est_frames + est_frames / 5u;
    const uint64_t MAX_INIT_FRAMES = 4u * 1024u * 1024u; /* 4 M frames */
    if (est_frames > MAX_INIT_FRAMES) est_frames = MAX_INIT_FRAMES;
    size_t cap = (size_t)est_frames * channels;
    int16_t *pcm = (int16_t *)JCE_MALLOC(cap * sizeof(int16_t));
    if (!pcm) { opus_decoder_destroy(od); jce_webm_close(p); return false; }
    size_t pos = 0;

    const uint8_t *pkt = NULL; size_t pkt_sz = 0;
    uint64_t pts_ns = 0;
    /* Hard safety cap: 5 minutes of stereo @ 48kHz s16 ≈ 55 MB.
     * Any honest video clip stays well under this; if we cross it the
     * parser is likely stuck in a loop and we want to bail rather than OOM. */
    const size_t HARD_CAP_FRAMES = 5ull * 60ull * 48000ull;
    size_t pkt_count = 0;
    while (jce_webm_read_audio_packet(p, &pkt, &pkt_sz, &pts_ns)) {
        ++pkt_count;
        int got = opus_decode(od, pkt, (opus_int32)pkt_sz,
                              scratch, MAX_FRAME, 0);
        if (got <= 0) continue;
        size_t need = pos + (size_t)got * channels;
        if ((pos / channels) + (size_t)got > HARD_CAP_FRAMES) {
            LOG_WARN(LOG_TAG,
                "WebM Opus decode hit hard cap (%zu frames, %zu pkts) — bailing to prevent OOM",
                (size_t)HARD_CAP_FRAMES, pkt_count);
            break;
        }
        if (need > cap) {
            size_t new_cap = cap * 2u;
            if (new_cap < need) new_cap = need;
            int16_t *re = (int16_t *)JCE_REALLOC(pcm, new_cap * sizeof(int16_t));
            if (!re) {
                JCE_FREE(pcm); opus_decoder_destroy(od); jce_webm_close(p);
                return false;
            }
            pcm = re; cap = new_cap;
        }
        memcpy(pcm + pos, scratch, (size_t)got * channels * sizeof(int16_t));
        pos = need;
    }
    LOG_INFO(LOG_TAG,
        "WebM Opus decoded %zu packets -> %zu frames (%uHz x %uch)",
        pkt_count, pos / channels, 48000u, channels);

    opus_decoder_destroy(od);
    jce_webm_close(p);

    if (pos == 0) { JCE_FREE(pcm); return false; }

    int16_t *trimmed = (int16_t *)JCE_REALLOC(pcm, pos * sizeof(int16_t));
    if (trimmed) pcm = trimmed;
    *out_pcm        = pcm;
    *out_frames     = (uint32_t)(pos / channels);
    *out_samplerate = 48000u;
    return true;
}

/* ======================================================================
 *  Unified OpenH264 decoder wrappers
 * ====================================================================== */

static bool decoder_open(VideoSlot *slot, const void *data, uint32_t size,
                         JceMp4Parser *parser)
{
    if (!slot || !parser) return false;

    JceMp4VideoTrackInfo vti;
    if (!jce_mp4_parser_get_video_track_info(parser, &vti)) {
        LOG_WARN(LOG_TAG, "decoder_open: no video track in MP4");
        return false;
    }

    /* Check for supported codecs: AV1 (royalty-free), H.264 (AVC), or
     * H.265 (HEVC). H.264/H.265 are patent-encumbered and gated behind
     * JCE_ENABLE_PATENTED_CODECS. */
    bool is_av1  = (strcmp(vti.codec, "av01") == 0);
    bool is_avc  = (strcmp(vti.codec, "avc1") == 0 || strcmp(vti.codec, "avc3") == 0);
    bool is_hevc = (strcmp(vti.codec, "hvc1") == 0 || strcmp(vti.codec, "hev1") == 0);
    if (!is_av1 && !is_avc && !is_hevc) {
        LOG_WARN(LOG_TAG, "decoder_open: unsupported video codec '%s'",
                 vti.codec);
        return false;
    }
#ifndef JCE_ENABLE_PATENTED_CODECS
    if (is_avc || is_hevc) {
        LOG_WARN(LOG_TAG,
            "patent-encumbered codec '%s' is disabled in this build "
            "(rebuild with -DJCE_ENABLE_PATENTED_CODECS=ON to enable %s playback). "
            "Falling back to metadata-only mode — no video frames will be decoded.",
            vti.codec, is_avc ? "H.264/AVC" : "H.265/HEVC");
        return false;
    }
#endif

    /* AV1 needs no avcC/hvcC-style DSI (av1C is metadata only — OBUs are
     * self-describing). H.264/H.265 require a parsed config blob. */
    if (!is_av1) {
        uint32_t min_dsi = is_avc ? 7u : 23u;
        if (!vti.decoder_config || vti.decoder_config_bytes < min_dsi) {
            LOG_WARN(LOG_TAG, "decoder_open: missing %s decoder config",
                     is_avc ? "avcC" : "hvcC");
            return false;
        }
    }

    /* Copy the raw MP4 data so the parser's blob reference stays valid. */
    slot->decoder.mp4_copy = (uint8_t *)JCE_MALLOC(size);
    if (!slot->decoder.mp4_copy) return false;
    memcpy(slot->decoder.mp4_copy, data, size);
    slot->decoder.mp4_copy_size = (size_t)size;

    /* Re-open parser on the owned copy (the original may be freed). */
    JceMp4Info dummy;
    slot->decoder.parser = jce_mp4_parser_open_memory(
        slot->decoder.mp4_copy, (size_t)size, &dummy);
    if (!slot->decoder.parser) {
        JCE_FREE(slot->decoder.mp4_copy);
        slot->decoder.mp4_copy = NULL;
        return false;
    }

    /* Re-query video track info from the new parser instance. */
    if (!jce_mp4_parser_get_video_track_info(slot->decoder.parser,
                                             &slot->decoder.vtrack)) {
        jce_mp4_parser_close(slot->decoder.parser);
        slot->decoder.parser = NULL;
        JCE_FREE(slot->decoder.mp4_copy);
        slot->decoder.mp4_copy = NULL;
        return false;
    }

    /* Create the appropriate decoder backend. */
    if (is_av1) {
        if (!mp4_av1_decoder_open(slot)) {
            LOG_WARN(LOG_TAG, "decoder_open: failed to init dav1d for MP4-AV1");
            jce_mp4_parser_close(slot->decoder.parser);
            slot->decoder.parser = NULL;
            JCE_FREE(slot->decoder.mp4_copy);
            slot->decoder.mp4_copy = NULL;
            return false;
        }
        /* Build AV1 keyframe sample index by scanning OBUs in each video
         * sample. Samples that contain an OBU_SEQUENCE_HEADER (type=1) or
         * an OBU_FRAME (type=6) with frame_type==KEY_FRAME (0) are random-
         * access points. This lets seek match WebM cluster-seek perf. */
        mp4_av1_build_keyframe_index(slot);
        slot->decoder.last_seek_kf_idx = 0;
        return true;
    }
    if (is_avc) {
        slot->decoder.h264 = jce_h264_decoder_open(
            slot->decoder.vtrack.decoder_config,
            slot->decoder.vtrack.decoder_config_bytes);
        if (!slot->decoder.h264) {
            LOG_WARN(LOG_TAG, "decoder_open: failed to init OpenH264");
            jce_mp4_parser_close(slot->decoder.parser);
            slot->decoder.parser = NULL;
            JCE_FREE(slot->decoder.mp4_copy);
            slot->decoder.mp4_copy = NULL;
            return false;
        }
    } else {
        slot->decoder.h265 = jce_h265_decoder_open(
            slot->decoder.vtrack.decoder_config,
            slot->decoder.vtrack.decoder_config_bytes);
        if (!slot->decoder.h265) {
            LOG_WARN(LOG_TAG, "decoder_open: failed to init libhevc");
            jce_mp4_parser_close(slot->decoder.parser);
            slot->decoder.parser = NULL;
            JCE_FREE(slot->decoder.mp4_copy);
            slot->decoder.mp4_copy = NULL;
            return false;
        }
    }

    slot->decoder.width = slot->decoder.vtrack.width;
    slot->decoder.height = slot->decoder.vtrack.height;
    slot->decoder.sample_idx = 0;
    slot->decoder.ended = false;

    /* Decode first frame. */
    JceMp4SampleInfo si;
    if (!jce_mp4_parser_get_video_sample(slot->decoder.parser, 0, &si)) {
        LOG_WARN(LOG_TAG, "decoder_open: could not read first video sample");
        if (slot->decoder.h264) {
            jce_h264_decoder_close(slot->decoder.h264);
            slot->decoder.h264 = NULL;
        }
        if (slot->decoder.h265) {
            jce_h265_decoder_close(slot->decoder.h265);
            slot->decoder.h265 = NULL;
        }
        jce_mp4_parser_close(slot->decoder.parser);
        slot->decoder.parser = NULL;
        JCE_FREE(slot->decoder.mp4_copy);
        slot->decoder.mp4_copy = NULL;
        return false;
    }

    const uint8_t *rgba = NULL;
    uint32_t dw = 0, dh = 0;
    bool got_frame;
    if (slot->decoder.h265) {
        got_frame = jce_h265_decode_frame(
            slot->decoder.h265,
            slot->decoder.mp4_copy + si.offset,
            si.size_bytes,
            slot->decoder.vtrack.nal_length_size,
            &rgba, &dw, &dh);
    } else {
        got_frame = jce_h264_decode_frame(
            slot->decoder.h264,
            slot->decoder.mp4_copy + si.offset,
            si.size_bytes,
            slot->decoder.vtrack.nal_length_size,
            &rgba, &dw, &dh);
    }

    if (got_frame && rgba && dw > 0 && dh > 0) {
        slot->decoder.width = dw;
        slot->decoder.height = dh;
        if (ensure_rgba(slot, dw, dh)) {
            memcpy(slot->rgba, rgba, (size_t)dw * dh * 4);
        }
        slot->frame_counter = 1;
    } else {
        /* First frame didn't produce output (B-frame delay or SPS/PPS only).
         * Leave frame_counter at 0 so upload_latest_frame won't try to
         * display a non-existent frame. */
        slot->frame_counter = 0;
        LOG_INFO(LOG_TAG, "decoder_open: first frame deferred (B-frame delay)");
    }
    slot->decoder.sample_idx = 1;
    slot->frame_time = normalize_frame_time(slot, 0.0, true);

    /* Build keyframe index by scanning NAL headers of every sample.
     * Done once at open time so seek can binary-search O(log n). */
    {
        uint32_t sc = slot->decoder.vtrack.sample_count;
        uint32_t nls = slot->decoder.vtrack.nal_length_size;
        uint32_t cap = (sc / 30) + 16; /* typical GOP ~30 frames */
        uint32_t *kf = (uint32_t *)JCE_MALLOC(cap * sizeof(uint32_t));
        uint32_t kf_count = 0;

        if (kf) {
            for (uint32_t i = 0; i < sc; i++) {
                JceMp4SampleInfo ksi;
                if (!jce_mp4_parser_get_video_sample(slot->decoder.parser, i, &ksi))
                    continue;
                if (ksi.size_bytes <= nls)
                    continue;
                const uint8_t *nd = slot->decoder.mp4_copy + ksi.offset;
                /* Read the first NAL unit type only (keyframe NAL is always first). */
                uint32_t nal_len = 0;
                for (uint32_t b = 0; b < nls; b++)
                    nal_len = (nal_len << 8) | nd[b];
                if (nal_len == 0 || nal_len > ksi.size_bytes - nls)
                    continue;
                bool is_kf = false;
                if (slot->decoder.h265) {
                    uint8_t nt = (nd[nls] >> 1) & 0x3F;
                    is_kf = (nt >= 16 && nt <= 21);
                } else {
                    uint8_t nt = nd[nls] & 0x1F;
                    is_kf = (nt == 5);
                }
                if (is_kf) {
                    if (kf_count >= cap) {
                        cap = cap * 2;
                        uint32_t *tmp = (uint32_t *)JCE_MALLOC(cap * sizeof(uint32_t));
                        if (!tmp) break;
                        memcpy(tmp, kf, kf_count * sizeof(uint32_t));
                        JCE_FREE(kf);
                        kf = tmp;
                    }
                    kf[kf_count++] = i;
                }
            }
            slot->decoder.keyframe_indices = kf;
            slot->decoder.keyframe_count = kf_count;
        } else {
            slot->decoder.keyframe_indices = NULL;
            slot->decoder.keyframe_count = 0;
        }
        slot->decoder.last_seek_kf_idx = 0;
        LOG_INFO(LOG_TAG, "keyframe index built: %u keyframes / %u samples",
                 kf_count, sc);
    }

    return true;
}

static bool decoder_read_next(VideoSlot *slot)
{
    if (!slot) return false;
    if (slot->decoder.webm) return webm_decoder_read_next(slot);
    if (mp4_av1_is_active(slot)) return mp4_av1_decoder_read_next(slot);
    if (slot->decoder.av1) return av1_decoder_read_next(slot);
    if ((!slot->decoder.h264 && !slot->decoder.h265)
        || !slot->decoder.parser) return false;

    /* Drain any pending frame buffered by the H.264 two-step DPB drain.
     * This frame was produced during the previous decode call but could
     * not be returned because the feed step already produced output. */
    if (slot->decoder.h264) {
        const uint8_t *rgba = NULL;
        uint32_t dw = 0, dh = 0;
        if (jce_h264_decoder_drain_pending(slot->decoder.h264,
                                            &rgba, &dw, &dh)) {
            slot->decoder.width = dw;
            slot->decoder.height = dh;
            if (!ensure_rgba(slot, dw, dh)) return false;
            memcpy(slot->rgba, rgba, (size_t)dw * dh * 4);
            /* Use synthetic timestamp — normalize_frame_time enforces
             * monotonicity and will bump by one frame interval. */
            slot->frame_time = normalize_frame_time(slot, slot->frame_time, true);
            slot->frame_counter++;
            slot->decoder.ended = false;
            return true;
        }
    }

    if (slot->decoder.sample_idx >= slot->decoder.vtrack.sample_count) {
        slot->decoder.ended = true;
        return false;
    }

    JceMp4SampleInfo si;
    if (!jce_mp4_parser_get_video_sample(slot->decoder.parser,
                                         slot->decoder.sample_idx, &si)) {
        slot->decoder.ended = true;
        return false;
    }

    const uint8_t *rgba = NULL;
    uint32_t dw = 0, dh = 0;
    bool got_frame;
    if (slot->decoder.h265) {
        got_frame = jce_h265_decode_frame(
            slot->decoder.h265,
            slot->decoder.mp4_copy + si.offset,
            si.size_bytes,
            slot->decoder.vtrack.nal_length_size,
            &rgba, &dw, &dh);
    } else {
        got_frame = jce_h264_decode_frame(
            slot->decoder.h264,
            slot->decoder.mp4_copy + si.offset,
            si.size_bytes,
            slot->decoder.vtrack.nal_length_size,
            &rgba, &dw, &dh);
    }

    slot->decoder.sample_idx++;

    if (!got_frame || !rgba || dw == 0 || dh == 0) {
        /* Decoder didn't produce a frame (B-frame delay, non-keyframe
         * without refs, etc).  Not an error — caller should keep going. */
        if (slot->decoder.sample_idx <= 8) {
            LOG_INFO(LOG_TAG, "sample[%u]: no frame (size=%u nal_len=%u)",
                     slot->decoder.sample_idx - 1, si.size_bytes,
                     (unsigned)slot->decoder.vtrack.nal_length_size);
        }
        return false;
    }

    if (slot->frame_counter < 3) {
        LOG_INFO(LOG_TAG, "sample[%u]: got frame %ux%u",
                 slot->decoder.sample_idx - 1, dw, dh);
    }

    slot->decoder.width = dw;
    slot->decoder.height = dh;
    if (!ensure_rgba(slot, dw, dh)) return false;
    memcpy(slot->rgba, rgba, (size_t)dw * dh * 4);

    double ts_sec = 0.0;
    if (slot->decoder.vtrack.timescale > 0) {
        ts_sec = (double)si.timestamp / (double)slot->decoder.vtrack.timescale;
    }
    slot->frame_time = normalize_frame_time(slot, ts_sec, true);
    slot->frame_counter++;
    slot->decoder.ended = false;
    return true;
}

/* Check whether a video sample is a keyframe (IDR / random-access point)
 * by inspecting NAL unit types in the bitstream data. */
static bool is_sample_keyframe(const VideoSlot *slot, uint32_t sample_index)
{
    JceMp4SampleInfo si;
    if (!jce_mp4_parser_get_video_sample(slot->decoder.parser, sample_index, &si))
        return false;

    uint32_t nls = slot->decoder.vtrack.nal_length_size;
    if (si.size_bytes <= nls)
        return false;

    const uint8_t *data = slot->decoder.mp4_copy + si.offset;
    uint32_t remaining = si.size_bytes;

    /* Walk through all NAL units in the sample (length-prefixed). */
    while (remaining > nls) {
        uint32_t nal_len = 0;
        for (uint32_t i = 0; i < nls; i++)
            nal_len = (nal_len << 8) | data[i];

        data += nls;
        remaining -= nls;

        if (nal_len == 0 || nal_len > remaining)
            break;

        if (slot->decoder.h265) {
            /* HEVC: nal_unit_type = (byte >> 1) & 0x3F
             * BLA_W_LP=16, BLA_W_RADL=17, BLA_N_LP=18,
             * IDR_W_RADL=19, IDR_N_LP=20, CRA_NUT=21 */
            uint8_t nal_type = (data[0] >> 1) & 0x3F;
            if (nal_type >= 16 && nal_type <= 21)
                return true;
        } else {
            /* H.264: nal_unit_type = byte & 0x1F
             * IDR slice = 5 */
            uint8_t nal_type = data[0] & 0x1F;
            if (nal_type == 5)
                return true;
        }

        data += nal_len;
        remaining -= nal_len;
    }
    return false;
}

static bool decoder_seek(VideoSlot *slot, double time_sec, bool exact)
{
    if (!slot) return false;
    if (slot->decoder.webm) return webm_decoder_seek(slot, time_sec, exact);
    if (mp4_av1_is_active(slot)) return mp4_av1_decoder_seek(slot, time_sec, exact);
    if (slot->decoder.av1) return av1_decoder_seek(slot, time_sec, exact);
    if ((!slot->decoder.h264 && !slot->decoder.h265)
        || !slot->decoder.parser) return false;
    if (time_sec < 0.0) time_sec = 0.0;

    /* Find sample index closest to |time_sec| via binary search. */
    uint32_t target_idx = 0;
    uint32_t sc = slot->decoder.vtrack.sample_count;
    uint32_t ts_scale = slot->decoder.vtrack.timescale;
    if (ts_scale == 0) ts_scale = 1;

    if (sc > 0) {
        uint32_t lo = 0, hi = sc - 1;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo + 1) / 2;
            JceMp4SampleInfo si;
            if (!jce_mp4_parser_get_video_sample(slot->decoder.parser, mid, &si)) {
                hi = mid - 1;
                continue;
            }
            double st = (double)si.timestamp / (double)ts_scale;
            if (st <= time_sec) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        target_idx = lo;
    }

    /* Find the nearest preceding keyframe via binary search on the
     * pre-built keyframe index.  O(log k) where k = keyframe count. */
    uint32_t keyframe_idx = 0;
    if (slot->decoder.keyframe_indices && slot->decoder.keyframe_count > 0) {
        const uint32_t *kf = slot->decoder.keyframe_indices;
        uint32_t kn = slot->decoder.keyframe_count;
        uint32_t klo = 0, khi = kn - 1;
        while (klo < khi) {
            uint32_t km = klo + (khi - klo + 1) / 2;
            if (kf[km] <= target_idx)
                klo = km;
            else
                khi = km - 1;
        }
        keyframe_idx = kf[klo];
    } else {
        /* Fallback: linear scan (only when index build failed). */
        keyframe_idx = target_idx;
        while (keyframe_idx > 0 && !is_sample_keyframe(slot, keyframe_idx))
            keyframe_idx--;
    }

    /* For inexact seek (scrub drag), skip the expensive flush+decode
     * cycle if we would land on the same keyframe as last time. */
    if (!exact && keyframe_idx == slot->decoder.last_seek_kf_idx
        && slot->frame_counter > 0) {
        slot->time = time_sec;
        /* Recalibrate base so post-scrub advance produces correct times. */
        JceMp4SampleInfo tsi;
        if (jce_mp4_parser_get_video_sample(slot->decoder.parser, target_idx, &tsi)) {
            double target_raw = (double)tsi.timestamp / (double)ts_scale;
            slot->decode_ts_base_sec = target_raw - time_sec;
            slot->decode_ts_base_set = true;
        }
        slot->frame_time = time_sec;
        return true;
    }
    slot->decoder.last_seek_kf_idx = keyframe_idx;

    /* Flush decoder state (discard stale reference frames). */
    if (slot->decoder.h265)
        jce_h265_decoder_flush(slot->decoder.h265);
    else
        jce_h264_decoder_flush(slot->decoder.h264);

    /* Reset frame counter so normalize_frame_time re-calibrates. */
    slot->frame_counter = 0;
    slot->decode_ts_base_set = false;
    slot->decoder.sample_idx = keyframe_idx;
    slot->decoder.ended = false;
    slot->ended = false;
    slot->time = time_sec;

    if (exact) {
        /* Exact seek (on scrub release): decode from keyframe up to the
         * target sample.  Stop once we've fed the target to keep
         * sample_idx aligned with the displayed content. */
        auto seek_start = std::chrono::steady_clock::now();
        while (slot->decoder.sample_idx <= target_idx
               && slot->decoder.sample_idx < sc) {
            if (!decoder_read_next(slot)) {
                if (slot->decoder.ended) break;
            }
            /* Cap wall-clock time to 100 ms so the UI stays responsive.
             * advance() will finish catching up over subsequent ticks. */
            if (std::chrono::steady_clock::now() - seek_start
                > std::chrono::milliseconds(100))
                break;
        }
    } else {
        /* Inexact seek (during scrub drag): decode the keyframe to get
         * one clean frame quickly. Fast feedback > frame accuracy. */
        int safety = 0;
        while (safety < 8 && slot->decoder.sample_idx < sc) {
            if (decoder_read_next(slot)) break;
            safety++;
        }
    }

    /* Recalibrate decode_ts_base so that subsequent normalize_frame_time
     * calls produce correct absolute playback timestamps.
     * base = raw_ts[target] - time_sec, so:
     *   normalize(raw_ts[next]) = raw_ts[next] - base
     *                            = (raw_ts[next] - raw_ts[target]) + time_sec
     *                            ≈ time_sec + frame_interval              */
    {
        JceMp4SampleInfo tsi;
        if (jce_mp4_parser_get_video_sample(slot->decoder.parser, target_idx, &tsi)) {
            double target_raw = (double)tsi.timestamp / (double)ts_scale;
            slot->decode_ts_base_sec = target_raw - time_sec;
            slot->decode_ts_base_set = true;
        }
    }
    slot->frame_time = time_sec;
    return true;
}

static void decoder_close(VideoSlot *slot)
{
    if (!slot) return;
    av1_decoder_close(slot);
    webm_decoder_close(slot);
    if (slot->decoder.h264) {
        jce_h264_decoder_close(slot->decoder.h264);
        slot->decoder.h264 = NULL;
    }
    if (slot->decoder.h265) {
        jce_h265_decoder_close(slot->decoder.h265);
        slot->decoder.h265 = NULL;
    }
    if (slot->decoder.parser) {
        jce_mp4_parser_close(slot->decoder.parser);
        slot->decoder.parser = NULL;
    }
    JCE_FREE(slot->decoder.keyframe_indices);
    slot->decoder.keyframe_indices = NULL;
    slot->decoder.keyframe_count = 0;
    JCE_FREE(slot->decoder.mp4_copy);
    slot->decoder.mp4_copy = NULL;
    slot->decoder.mp4_copy_size = 0;
}

/* ======================================================================
 *  Public API  (extern "C")
 * ====================================================================== */

extern "C" {

JceVideo jce_video_load_memory(const void *data, uint32_t size,
                               const char *hint_path)
{
    JceMp4Parser *parser;
    if (!data || size == 0u) return JCE_VIDEO_INVALID;

    int idx = alloc_slot();
    if (idx < 0) {
        LOG_WARN(LOG_TAG, "no free video slots");
        return JCE_VIDEO_INVALID;
    }

    /* ── AV1 / IVF fast path (royalty-free) ────────────────────── */
    if (jce_av1_is_ivf(data, size)) {
        VideoSlot *slot = &s_slots[idx];
        memset(slot, 0, sizeof(*slot));

        if (!av1_decoder_open(slot, data, size)) {
            LOG_ERROR(LOG_TAG, "AV1/IVF open failed for '%s'",
                      hint_path ? hint_path : "<memory>");
            memset(slot, 0, sizeof(*slot));
            return JCE_VIDEO_INVALID;
        }

        slot->width      = (int)slot->decoder.width;
        slot->height     = (int)slot->decoder.height;
        slot->framerate  = (slot->decoder.av1_fps_den > 0u)
                         ? (double)slot->decoder.av1_fps_num
                         / (double)slot->decoder.av1_fps_den
                         : 30.0;
        slot->duration   = 0.0; /* IVF has no clip-level duration field */
        slot->has_audio  = false;
        slot->samplerate = 0;
        slot->audio_channels = 0;
        slot->audio_sample_count = 0u;
        snprintf(slot->video_codec, sizeof(slot->video_codec), "av01");
        snprintf(slot->audio_codec, sizeof(slot->audio_codec), "----");
        set_audio_status(slot, JCE_VIDEO_AUDIO_STATUS_NONE,
                         "IVF stream has no embedded audio");
        slot->metadata_only = false;
        slot->ended = false;
        slot->time  = 0.0;
        slot->used  = true;

        LOG_SUCCESS(LOG_TAG,
            "parsed '%s' codec=av01 (IVF) %dx%d fps=%.3f decode=active",
            hint_path ? hint_path : "<memory>",
            slot->width, slot->height, slot->framerate);
        return (JceVideo)(idx + 1);
    }

    /* ── WebM / Matroska fast path (royalty-free: VP8 + AV1, Opus/Vorbis) ─ */
    if (jce_webm_is_webm(data, size)) {
        VideoSlot *slot = &s_slots[idx];
        memset(slot, 0, sizeof(*slot));

        if (!webm_decoder_open(slot, data, size)) {
            LOG_ERROR(LOG_TAG, "WebM open failed for '%s'",
                      hint_path ? hint_path : "<memory>");
            memset(slot, 0, sizeof(*slot));
            return JCE_VIDEO_INVALID;
        }

        /* Pull the first frame so RGBA/dims are populated. */
        webm_decoder_read_next(slot);

        slot->width      = (int)slot->decoder.width;
        slot->height     = (int)slot->decoder.height;
        slot->framerate  = 30.0; /* WebM block PTS drives the clock; this is a fallback */
        slot->duration   = (slot->decoder.webm_duration_ns > 0)
                         ? (double)slot->decoder.webm_duration_ns / 1.0e9
                         : 0.0;
        snprintf(slot->video_codec, sizeof(slot->video_codec), "%s",
                 slot->decoder.av1 ? "av01" : "vp8");

        /* WebM audio: route Opus to libopus → PCM (synchronous decode). */
        slot->has_audio = false;
        slot->samplerate = 0;
        slot->audio_channels = 0;
        slot->audio_sample_count = 0u;
        if (slot->decoder.webm_audio_codec == JCE_WEBM_AUDIO_OPUS
            && slot->decoder.webm_audio_channels > 0) {
            /* Spawn a background worker — Opus decode of a multi-minute
             * stream can take hundreds of ms; doing it sync stalls the
             * editor / runtime load path. */
            JceThreadPool *apool = get_audio_pool();
            WebmOpusDecodeCtx *octx = (WebmOpusDecodeCtx *)JCE_MALLOC(
                sizeof(WebmOpusDecodeCtx));
            uint8_t *wcopy = (uint8_t *)JCE_MALLOC(size);
            if (!apool || !octx || !wcopy) {
                JCE_FREE(octx); JCE_FREE(wcopy);
                snprintf(slot->audio_codec, sizeof(slot->audio_codec), "opus");
                set_audio_status(slot,
                    JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                    "out of memory or no audio thread pool for WebM Opus");
            } else {
                memcpy(wcopy, data, size);
                octx->slot           = slot;
                octx->webm_copy      = wcopy;
                octx->webm_copy_size = (size_t)size;
                octx->channels       = slot->decoder.webm_audio_channels;
                snprintf(slot->audio_codec, sizeof(slot->audio_codec), "opus");
                set_audio_status(slot, JCE_VIDEO_AUDIO_STATUS_DECODING,
                                 "decoding WebM Opus audio in background...");
                slot->audio_task = jce_thread_pool_submit_tracked(
                    apool, webm_opus_decode_worker, octx);
            }
        } else if (slot->decoder.webm_audio_codec == JCE_WEBM_AUDIO_VORBIS) {
            snprintf(slot->audio_codec, sizeof(slot->audio_codec), "vorb");
            set_audio_status(slot, JCE_VIDEO_AUDIO_STATUS_UNSUPPORTED_CODEC,
                             "WebM Vorbis audio not yet routed");
        } else {
            snprintf(slot->audio_codec, sizeof(slot->audio_codec), "----");
            set_audio_status(slot, JCE_VIDEO_AUDIO_STATUS_NONE,
                             "WebM has no audio track");
        }
        slot->metadata_only = false;
        slot->ended = false;
        slot->time  = 0.0;
        slot->used  = true;

        LOG_SUCCESS(LOG_TAG,
            "parsed '%s' codec=%s (WebM) %dx%d duration=%.3fs audio=%s decode=active",
            hint_path ? hint_path : "<memory>",
            slot->video_codec, slot->width, slot->height, slot->duration,
            slot->has_audio ? slot->audio_codec : "none");
        return (JceVideo)(idx + 1);
    }

    JceMp4Info mp4;
    parser = jce_mp4_parser_open_memory(data, (size_t)size, &mp4);
    if (!parser) {
        LOG_ERROR(LOG_TAG, "load failed for '%s': %s",
            hint_path ? hint_path : "<memory>",
            mp4.error[0] ? mp4.error : "invalid mp4");
        return JCE_VIDEO_INVALID;
    }

    VideoSlot *slot = &s_slots[idx];
    memset(slot, 0, sizeof(*slot));

    slot->width      = (int)mp4.width;
    slot->height     = (int)mp4.height;
    slot->framerate  = mp4.framerate;
    slot->duration   = mp4.duration_seconds;
    slot->has_audio  = mp4.has_audio_track;
    slot->samplerate = (int)mp4.audio_samplerate_hz;
    slot->audio_channels = (int)mp4.audio_channels;
    slot->audio_sample_count = 0u;
    set_audio_status(slot, JCE_VIDEO_AUDIO_STATUS_NONE, "no embedded audio");
    slot->metadata_only = true;
    snprintf(slot->video_codec, sizeof(slot->video_codec), "%s",
        mp4.video_codec[0] ? mp4.video_codec : "unkn");
    snprintf(slot->audio_codec, sizeof(slot->audio_codec), "%s",
        mp4.audio_codec[0] ? mp4.audio_codec : "unkn");

    if (slot->has_audio) {
        JceMp4AudioTrackInfo atr;
        if (jce_mp4_parser_get_audio_track_info(parser, &atr)) {
            slot->audio_sample_count = atr.sample_count;

            if (strcmp(atr.codec, "mp4a") == 0
                && atr.decoder_config && atr.decoder_config_bytes > 0) {
#ifndef JCE_ENABLE_PATENTED_CODECS
                LOG_WARN(LOG_TAG,
                    "patent-encumbered audio codec 'mp4a' (AAC) is disabled in "
                    "to enable AAC playback). No audio will be decoded.");
                set_audio_status(slot,
                    JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                    "AAC disabled (patent-encumbered; ");
#else
                /* ── Async AAC decode to PCM ───────────────────── */
                JceThreadPool *apool = get_audio_pool();
                if (!apool) {
                    set_audio_status(slot,
                        JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                        "failed to create audio thread pool");
                } else {
                    AudioDecodeCtx *actx = (AudioDecodeCtx *)JCE_MALLOC(
                        sizeof(AudioDecodeCtx));
                    if (!actx) {
                        set_audio_status(slot,
                            JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                            "out of memory for audio decode context");
                    } else {
                        /* Copy MP4 data for the background thread's parser. */
                        actx->mp4_copy = (uint8_t *)JCE_MALLOC(size);
                        actx->decoder_config = (uint8_t *)JCE_MALLOC(
                            atr.decoder_config_bytes);
                        if (!actx->mp4_copy || !actx->decoder_config) {
                            JCE_FREE(actx->mp4_copy);
                            JCE_FREE(actx->decoder_config);
                            JCE_FREE(actx);
                            set_audio_status(slot,
                                JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                                "out of memory for async audio data copy");
                        } else {
                            memcpy(actx->mp4_copy, data, size);
                            actx->mp4_copy_size = (size_t)size;
                            memcpy(actx->decoder_config, atr.decoder_config,
                                   atr.decoder_config_bytes);
                            actx->decoder_config_bytes = atr.decoder_config_bytes;
                            actx->sample_count   = atr.sample_count;
                            actx->channels_est   = atr.channels;
                            actx->samplerate_hz  = atr.samplerate_hz;
                            actx->slot           = slot;

                            set_audio_status(slot,
                                JCE_VIDEO_AUDIO_STATUS_DECODING,
                                "decoding AAC audio in background...");
                            slot->audio_task =
                                jce_thread_pool_submit_tracked(
                                    apool, audio_decode_worker, actx);
                        }
                    }
                }
#endif /* JCE_ENABLE_PATENTED_CODECS */
            } else if (strcmp(atr.codec, "mp4a") == 0) {
                set_audio_status(slot,
                                 JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                                 "AAC track found but decoder config missing");
            } else if (strcmp(atr.codec, "opus") == 0) {
                /* ── Async MP4 Opus decode to PCM (royalty-free) ─── */
                JceThreadPool *apool = get_audio_pool();
                if (!apool) {
                    set_audio_status(slot,
                        JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                        "failed to create audio thread pool");
                } else {
                    Mp4OpusDecodeCtx *octx = (Mp4OpusDecodeCtx *)JCE_MALLOC(
                        sizeof(Mp4OpusDecodeCtx));
                    if (!octx) {
                        set_audio_status(slot,
                            JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                            "out of memory for MP4 Opus context");
                    } else {
                        octx->mp4_copy = (uint8_t *)JCE_MALLOC(size);
                        if (!octx->mp4_copy) {
                            JCE_FREE(octx);
                            set_audio_status(slot,
                                JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                                "out of memory for MP4 Opus data copy");
                        } else {
                            memcpy(octx->mp4_copy, data, size);
                            octx->mp4_copy_size = (size_t)size;
                            octx->channels      = atr.channels ? atr.channels : 2u;
                            octx->samplerate_hz = atr.samplerate_hz
                                ? atr.samplerate_hz : 48000u;
                            octx->sample_count  = atr.sample_count;
                            octx->slot          = slot;
                            set_audio_status(slot,
                                JCE_VIDEO_AUDIO_STATUS_DECODING,
                                "decoding MP4 Opus audio in background...");
                            slot->audio_task =
                                jce_thread_pool_submit_tracked(
                                    apool, mp4_opus_decode_worker, octx);
                        }
                    }
                }
            } else if (atr.codec[0]) {
                char msg[128];
                snprintf(msg,
                         sizeof(msg),
                         "unsupported embedded audio codec: %s",
                         atr.codec);
                set_audio_status(slot,
                                 JCE_VIDEO_AUDIO_STATUS_UNSUPPORTED_CODEC,
                                 msg);
            } else {
                set_audio_status(slot,
                                 JCE_VIDEO_AUDIO_STATUS_DEMUX_ERROR,
                                 "audio track found but codec metadata is missing");
            }
        } else {
            set_audio_status(slot,
                             JCE_VIDEO_AUDIO_STATUS_DEMUX_ERROR,
                             "audio track advertised but sample metadata is unavailable");
        }
    }

    /* Try cross-platform OpenH264 decoder (must happen before closing parser). */
    if (!decoder_open(slot, data, size, parser)) {
        LOG_WARN(LOG_TAG,
            "decoder backend unavailable for '%s' (codec=%s) — metadata-only mode",
            hint_path ? hint_path : "<memory>",
            slot->video_codec);
        slot->metadata_only = true;
    } else {
        if (slot->width <= 0 && slot->decoder.width > 0u)
            slot->width = (int)slot->decoder.width;
        if (slot->height <= 0 && slot->decoder.height > 0u)
            slot->height = (int)slot->decoder.height;
        if (slot->framerate <= 0.0)
            slot->framerate = 30.0;
        slot->metadata_only = false;
        slot->ended = false;
        slot->time  = 0.0;
    }

    /* Close the original parser — decoder_open made its own copy. */
    jce_mp4_parser_close(parser);

    slot->used = true;

    LOG_SUCCESS(LOG_TAG,
        "parsed '%s' codec=%s %dx%d fps=%.3f dur=%.3fs audio=%d(%s %dHz %dch) decode=%s",
        hint_path ? hint_path : "<memory>",
        slot->video_codec, slot->width, slot->height,
        slot->framerate, slot->duration,
        (int)slot->has_audio,
        slot->audio_codec,
        slot->samplerate,
        slot->audio_channels,
        slot->metadata_only ? "metadata-only" : "active");

    return (JceVideo)(idx + 1);
}

void jce_video_unload(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot) return;

    /* Wait for background audio decode to finish before freeing. */
    if (slot->audio_task) {
        jce_task_wait(slot->audio_task);
        jce_task_free(slot->audio_task);
        slot->audio_task = NULL;
    }

    decoder_close(slot);

    if (slot->audio_pcm) { JCE_FREE(slot->audio_pcm); slot->audio_pcm = NULL; }
    if (slot->rgba) { JCE_FREE(slot->rgba); slot->rgba = NULL; }
    memset(slot, 0, sizeof(*slot));
}

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
    out->metadata_only  = slot->metadata_only;
    snprintf(out->video_codec, sizeof(out->video_codec), "%s", slot->video_codec);
    snprintf(out->audio_codec, sizeof(out->audio_codec), "%s", slot->audio_codec);
    return true;
}

double jce_video_get_time(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    return slot ? slot->time : 0.0;
}

double jce_video_get_duration(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    return slot ? slot->duration : 0.0;
}

bool jce_video_has_ended(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    return slot ? slot->ended : true;
}

void jce_video_get_size(JceVideo v, int *out_w, int *out_h)
{
    VideoSlot *slot = slot_from_handle(v);
    if (out_w) *out_w = slot ? slot->width  : 0;
    if (out_h) *out_h = slot ? slot->height : 0;
}

void jce_video_advance(JceVideo v, double dt_seconds)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot || dt_seconds <= 0.0) return;
    if (dt_seconds > 1.0) dt_seconds = 1.0;

    /* Metadata-only: tick the clock. */
    if (slot->metadata_only) {
        if (slot->duration <= 0.0) {
            slot->time += dt_seconds;
            slot->ended = false;
            return;
        }
        double t = slot->time + dt_seconds;
        if (t >= slot->duration) {
            if (slot->loop) {
                t = fmod(t, slot->duration);
                if (t < 0.0) t = 0.0;
                slot->ended = false;
            } else { t = slot->duration; slot->ended = true; }
        } else {
            slot->ended = false;
        }
        slot->time = t;
        return;
    }

    /* Decode path. */
    double target = slot->time + dt_seconds;
    if (slot->duration > 0.0) {
        if (slot->loop) {
            while (target >= slot->duration) {
                target -= slot->duration;
                slot->post_seek = true;
                if (!decoder_seek(slot, 0.0, false)) {
                    slot->post_seek = false;
                    LOG_WARN(LOG_TAG,
                        "loop seek failed; stopping playback at clip boundary");
                    slot->time = slot->duration;
                    slot->ended = true;
                    return;
                }
                slot->post_seek = false;
                slot->frame_time = 0.0;
            }
        } else if (target >= slot->duration) {
            target = slot->duration;
        }
    }
    slot->time = target;

    const uint64_t pixels = (uint64_t)(slot->width > 0 ? slot->width : 0)
                          * (uint64_t)(slot->height > 0 ? slot->height : 0);
    const bool heavy_decode = pixels >= (uint64_t)2560u * (uint64_t)1440u;
    const int max_decode_per_tick = heavy_decode ? 2 : 8;
    const auto decode_budget = std::chrono::milliseconds(heavy_decode ? 6 : 12);
    const auto decode_start = std::chrono::steady_clock::now();

    int safety = 0;
    while (slot->frame_time + 0.000001 < slot->time
        && safety < max_decode_per_tick) {
        if (!decoder_read_next(slot)) {
            bool eof = slot->decoder.ended;
            if (eof) {
                if (slot->loop && slot->duration > 0.0) {
                    slot->post_seek = true;
                    if (!decoder_seek(slot, 0.0, false)) {
                        slot->post_seek = false;
                        break;
                    }
                    slot->post_seek = false;
                    slot->frame_time = 0.0;
                    continue;
                }
                slot->ended = true;
                if (slot->duration > 0.0) slot->time = slot->duration;
                break;
            }
            /* Decoder didn't produce a frame yet (B-frame reordering delay).
             * Keep feeding more samples instead of waiting for the next tick. */
            ++safety;
            if (std::chrono::steady_clock::now() - decode_start >= decode_budget)
                break;
            continue;
        }
        slot->ended = false;
        ++safety;

        if (std::chrono::steady_clock::now() - decode_start >= decode_budget) {
            break;
        }
    }

    /* Avoid huge per-frame catch-up work on heavy clips. */
    if (safety >= max_decode_per_tick
        && slot->frame_time + 0.000001 < slot->time) {
        slot->time = slot->frame_time + frame_interval_seconds(slot);
        if (slot->duration > 0.0 && slot->time > slot->duration) {
            slot->time = slot->duration;
        }
    }

    if (slot->duration > 0.0 && !slot->loop
        && slot->time >= slot->duration - 0.000001) {
        if (slot->decoder.ended) slot->ended = true;
    }
}

void jce_video_seek(JceVideo v, double time_sec, bool exact)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot) return;

    if (time_sec < 0.0) time_sec = 0.0;
    if (slot->duration > 0.0 && time_sec > slot->duration)
        time_sec = slot->duration;

    if (slot->metadata_only) {
        slot->time  = time_sec;
        slot->ended = (slot->duration > 0.0
            && time_sec >= slot->duration && !slot->loop);
        return;
    }

    slot->post_seek = true;
    if (!decoder_seek(slot, time_sec, exact)) {
        slot->time  = time_sec;
        slot->ended = false;
    } else {
        /* Successful seek must always clear the slot-level ended flag.
         * The H.264/H.265 path clears it inside decoder_seek; WebM /
         * MP4-AV1 / IVF-AV1 only clear slot->decoder.ended, so without
         * this line a seek-after-EOF would leave the editor stuck in a
         * "paused at end" state and the play/pause button would do
         * nothing on the next tick. */
        slot->ended = false;
    }
    slot->post_seek = false;
    /* decoder_seek already sets frame_time and recalibrates
     * decode_ts_base_sec using the target sample's timestamp. */
}

void jce_video_rewind(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot) return;

    if (slot->metadata_only) {
        slot->time  = 0.0;
        slot->ended = false;
        return;
    }
    slot->post_seek = true;
    if (!decoder_seek(slot, 0.0, true)) {
        slot->time  = 0.0;
        slot->ended = false;
    }
    slot->post_seek = false;
    slot->frame_time = 0.0;
}

void jce_video_set_loop(JceVideo v, bool loop)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot) return;
    slot->loop = loop;
    if (loop && slot->ended) slot->ended = false;
}

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
    if (out_w) *out_w = slot->rgba_w;
    if (out_h) *out_h = slot->rgba_h;
    if (out_frame_time) *out_frame_time = slot->frame_time;
    return slot->rgba;
}

uint64_t jce_video_get_frame_counter(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    return slot ? slot->frame_counter : 0u;
}

JceVideoAudioStatus jce_video_get_audio_status(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot) {
        return JCE_VIDEO_AUDIO_STATUS_DEMUX_ERROR;
    }
    return slot->audio_status;
}

const char *jce_video_get_audio_status_text(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot) {
        return "invalid video handle";
    }
    return slot->audio_status_text;
}

uint32_t jce_video_get_audio_sample_count(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    return slot ? slot->audio_sample_count : 0u;
}

const int16_t *jce_video_get_audio_pcm(JceVideo v,
                                        uint32_t *out_frame_count,
                                        uint32_t *out_channels,
                                        uint32_t *out_samplerate)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot || !slot->audio_pcm) {
        if (out_frame_count) *out_frame_count = 0;
        if (out_channels)    *out_channels    = 0;
        if (out_samplerate)  *out_samplerate  = 0;
        return NULL;
    }
    if (out_frame_count) *out_frame_count = slot->audio_pcm_frames;
    if (out_channels)    *out_channels    = slot->audio_pcm_channels;
    if (out_samplerate)  *out_samplerate  = slot->audio_pcm_samplerate;
    return slot->audio_pcm;
}

} /* extern "C" */
