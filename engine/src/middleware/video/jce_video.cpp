/*
 * jce_video.cpp  MP4 metadata + cross-platform frame decode.
 *
 * Container:   Manual MP4/ISO-BMFF parsing via jce_mp4_parser (all platforms).
 * Decode:      OpenH264 software H.264 decoder (cross-platform).
 */

#include <jce/middleware/video/jce_av1_decode.h>
#include <jce/middleware/video/jce_mp4_parser.h>
#include <jce/middleware/video/jce_video.h>
#include <jce/middleware/video/jce_vp8_decode.h>
#include <jce/middleware/video/jce_webm_parser.h>

#include "jce_audio_stream.h"

extern "C" {
#include <opus.h>
}
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>

#include "jce_aac_decode.h"
#include "jce_h264_decode.h"
#include "jce_h265_decode.h"
#include "jce_yuv_convert.h"
#include "os/core/jce_memory.h"

#include <cmath>
#include <cstdio>
#include <cstring>

/* perf timing comes from <jce/core/jce_timer.h> */
#include <jce/os/core/jce_timer.h>

#include <SDL3/SDL.h> /* SDL_HasAVX2 */

/* S3: Enable AVX2 YUV→RGBA path when available (checked once at startup). */
static const int s_yuv_avx2_init = (jce_yuv_set_avx2(SDL_HasAVX2()), 0);

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

    /* Decoded embedded audio (full-clip s16 PCM, owned).
     * DEPRECATED: still produced by the AAC and MP4/Opus paths; the
     * audio_stream below is what the new public API pulls from. The
     * blob-fed adapter (blob_stream_*) bridges the old paths into the
     * streaming surface without re-decoding. */
    int16_t *audio_pcm;
    uint32_t audio_pcm_frames;
    uint32_t audio_pcm_channels;
    uint32_t audio_pcm_samplerate;

    /* Streaming audio source. Lazily created on first pull or, for the
     * WebM/Opus path, upfront at load time. Owned. */
    JceAudioStream *audio_stream;

    /* Owned WebM data copy kept alive for as long as the streaming
     * Opus decoder needs it. NULL for non-WebM paths. */
    uint8_t *webm_data;
    size_t   webm_data_size;

    JceVideoDecoder decoder;

    /* ── Async decode pipeline (Stage 1) ──────────────────────────
     * A dedicated worker thread drives the decoder end-to-end,
     * pushing RGBA frames into the queue. jce_video_advance pops
     * frames based on audio/wall clock, copies into display_rgba.
     *
     * slot->rgba (above) is the DECODER scratch — worker-only.
     * slot->display_rgba is what UI uploads to GPU. */
    bool          queue_active;
    bool          queue_yuv_mode;        /* AV1/VP8: queue stores YUV planes (Stage 2) */
    bool          worker_running;
    bool          quit_request;
    bool          seek_request;
    double        seek_target_sec;
    bool          seek_exact;
    bool          worker_eof;
    JceMutex     *qmtx;
    JceCondVar   *q_not_full;
    JceCondVar   *q_not_empty;
    /* Frame queue (ring buffer; capacity defined by JCE_VIDEO_Q_CAP). */
    void         *qslots;              /* VideoQueueEntry[JCE_VIDEO_Q_CAP] */
    int           q_head;
    int           q_tail;
    int           q_count;
    uint64_t      seek_serial;

    /* Worker scratch: YUV planes from the most recent decode call.
     * Valid only between decoder_emit_frame_yuv420 and the next decode.
     * Pointers reference decoder-owned memory (dav1d / libvpx). */
    const uint8_t *pending_yuv_y;
    const uint8_t *pending_yuv_u;
    const uint8_t *pending_yuv_v;
    ptrdiff_t      pending_yuv_y_stride;
    ptrdiff_t      pending_yuv_uv_stride;
    uint32_t       pending_yuv_w;
    uint32_t       pending_yuv_h;
    bool           pending_yuv_valid;

    uint8_t *display_rgba;
    int      display_rgba_w;
    int      display_rgba_h;
    size_t   display_rgba_capacity;
    double   display_frame_time;
    uint64_t display_frame_counter;

    /* Stage 2 zero-copy pop: pop swaps YUV plane pointers between the
     * queue entry and these display-side buffers. The expensive
     * YUV → RGBA conversion runs OUTSIDE the queue lock so the worker
     * can keep decoding. Buffers cycle entry ↔ display each pop. */
    uint8_t *display_y; size_t display_y_capacity;
    uint8_t *display_u; size_t display_u_capacity;
    uint8_t *display_v; size_t display_v_capacity;
    int      display_y_stride;
    int      display_uv_stride;
    int      display_yuv_w;
    int      display_yuv_h;
    bool     display_yuv_pending;

    void *worker_thread;               /* JceThread* */

    /* ── Perf probes (S1) ─────────────────────────────────────────
     * EMA-smoothed per-frame timings in microseconds. Updated by
     * worker thread (decode/convert/push) and UI thread (pop).
     * Read race-free via jce_video_get_perf_stats — we accept stale
     * reads since this is debug-only. */
    double   perf_decode_us_ema;
    double   perf_convert_us_ema;     /* worker-side YUV→RGBA SIMD */
    double   perf_push_us_ema;
    double   perf_pop_us_ema;
    double   perf_decode_us_last;
    double   perf_convert_us_last;
    double   perf_push_us_last;
    double   perf_pop_us_last;
    uint64_t perf_frames_decoded;
    uint64_t perf_frames_displayed;
    uint64_t perf_frames_dropped;     /* late frames skipped during pop */
    uint64_t perf_perf_freq;          /* jce_time_perf_freq() cache */
} VideoSlot;

#define JCE_VIDEO_Q_CAP 8

struct VideoQueueEntry {
    /* RGBA path (H264/H265 / legacy). */
    uint8_t *rgba;
    size_t   rgba_capacity;
    /* YUV path (AV1 / VP8, Stage 2 — saves ~100MB at 4K vs RGBA). */
    uint8_t *y;        size_t y_capacity;
    uint8_t *u;        size_t u_capacity;
    uint8_t *v;        size_t v_capacity;
    int      y_stride;       /* tight: == w   */
    int      uv_stride;      /* tight: == w/2 */
    bool     is_yuv;         /* true: use y/u/v; false: use rgba */
    int      w;
    int      h;
    double   pts_sec;
    uint64_t serial;
};

static VideoSlot s_slots[JCE_MAX_VIDEOS];

/* ── Perf probe helpers (S1) ─────────────────────────────────────
 * Sub-microsecond timing via jce_time_perf_counter(). EMA with
 * alpha=0.1 (smooths jitter while still tracking changes within ~10
 * frames). Last value retained for spike inspection. */
static inline uint64_t perf_now(void) { return jce_time_perf_counter(); }
static inline uint64_t perf_freq_cached(VideoSlot *s)
{
    if (s->perf_perf_freq == 0u) s->perf_perf_freq = jce_time_perf_freq();
    return s->perf_perf_freq;
}
static inline double perf_us(VideoSlot *s, uint64_t t0, uint64_t t1)
{
    if (t1 <= t0) return 0.0;
    return (double)(t1 - t0) * 1e6 / (double)perf_freq_cached(s);
}
static inline void perf_ema_update(double *ema, double *last, double sample)
{
    *last = sample;
    if (*ema <= 0.0) *ema = sample;
    else             *ema = (*ema) * 0.9 + sample * 0.1;
}

/* Forward decl so adapters can call set_audio_status. */
static void set_audio_status(VideoSlot *slot,
                             JceVideoAudioStatus status,
                             const char *text);
/* ── Streaming codec adapter: WebM/Opus (true streaming) ──────────
 * Owns its own WebM parser instance + persistent OpusDecoder, decoded
 * lazily, one packet at a time. Memory footprint is bounded by the
 * JceAudioStream ring buffer (~400 KB) plus the parser/decoder state
 * (a few KB), down from the previous ~22 MB blob. */
typedef struct {
    JceWebmParser *parser;
    OpusDecoder   *opus;
    uint32_t       channels;
    uint32_t       samplerate;
    /* Scratch for one decoded packet (max Opus packet = 120ms @ 48k). */
    int16_t        scratch[5760 * 8];
    uint32_t       scratch_avail; /* frames left in scratch from prev call */
    uint32_t       scratch_pos;   /* read offset in scratch (frames) */
    bool           eof;
    /* Fine-seek target: drop packets/frames whose end-pts is before this.
     * Set by webm_opus_stream_seek; cleared once reached. */
    uint64_t       seek_target_ns;
    bool           seek_active;
} WebmOpusStreamState;

static uint32_t webm_opus_stream_decode_next(void *ud,
                                             int16_t *out, uint32_t max)
{
    WebmOpusStreamState *st = (WebmOpusStreamState *)ud;
    /* Drain leftover from a prior packet first. */
    if (st->scratch_avail > 0) {
        uint32_t n = st->scratch_avail;
        if (n > max) n = max;
        memcpy(out,
               st->scratch + (size_t)st->scratch_pos * st->channels,
               (size_t)n * st->channels * sizeof(int16_t));
        st->scratch_pos   += n;
        st->scratch_avail -= n;
        return n;
    }
    if (st->eof) return 0;
    /* Pump packets until we get one that produces samples. */
    for (;;) {
        const uint8_t *pkt = NULL;
        size_t pkt_size = 0;
        uint64_t pts = 0;
        if (!jce_webm_read_audio_packet(st->parser, &pkt, &pkt_size, &pts)) {
            st->eof = true;
            return 0;
        }
        int got = opus_decode(st->opus, pkt, (opus_int32)pkt_size,
                              st->scratch, 5760, 0);
        if (got <= 0) continue;
        uint32_t produced = (uint32_t)got;

        /* Honor a pending fine-seek: webm cluster seek can land seconds
         * before the requested time. Skip whole packets whose end pts
         * is still before target; trim the leading samples of the
         * packet that straddles the boundary. */
        if (st->seek_active) {
            uint64_t pkt_dur_ns = (uint64_t)produced * 1000000000ull
                                / (uint64_t)st->samplerate;
            if (pts + pkt_dur_ns <= st->seek_target_ns) {
                continue; /* whole packet before target */
            }
            uint64_t skip_frames = 0;
            if (pts < st->seek_target_ns) {
                skip_frames = (st->seek_target_ns - pts)
                              * (uint64_t)st->samplerate / 1000000000ull;
                if (skip_frames > produced) skip_frames = produced;
            }
            st->seek_active = false;
            if (skip_frames > 0) {
                uint32_t remain = produced - (uint32_t)skip_frames;
                if (remain == 0) continue;
                /* Shift kept samples to scratch start. */
                memmove(st->scratch,
                        st->scratch + (size_t)skip_frames * st->channels,
                        (size_t)remain * st->channels * sizeof(int16_t));
                produced = remain;
            }
        }

        /* Hand back as many frames as caller wants, stash the rest. */
        uint32_t n = produced;
        if (n > max) n = max;
        memcpy(out, st->scratch,
               (size_t)n * st->channels * sizeof(int16_t));
        st->scratch_pos   = n;
        st->scratch_avail = produced - n;
        return n;
    }
}

static void webm_opus_stream_seek(void *ud, double sec)
{
    WebmOpusStreamState *st = (WebmOpusStreamState *)ud;
    if (sec < 0.0) sec = 0.0;
    uint64_t ns = (uint64_t)(sec * 1e9);
    jce_webm_seek(st->parser, ns);
    /* Reset the decoder to clear stateful prediction noise after a
     * non-contiguous jump, and discard any half-consumed packet. */
    if (st->opus) opus_decoder_ctl(st->opus, OPUS_RESET_STATE);
    st->scratch_avail   = 0;
    st->scratch_pos     = 0;
    st->eof             = false;
    st->seek_target_ns  = ns;
    st->seek_active     = true;
}

static void webm_opus_stream_destroy(void *ud)
{
    WebmOpusStreamState *st = (WebmOpusStreamState *)ud;
    if (!st) return;
    if (st->opus)   opus_decoder_destroy(st->opus);
    if (st->parser) jce_webm_close(st->parser);
    JCE_FREE(st);
}

/* Build a JceAudioStream that streams WebM/Opus from `webm_data` (which
 * must outlive the stream — the caller stores it on the VideoSlot). */
static JceAudioStream *create_webm_opus_stream(const void *webm_data,
                                               size_t webm_size,
                                               uint32_t channels,
                                               double duration_sec)
{
    if (!webm_data || webm_size == 0 || channels == 0 || channels > 8) {
        return NULL;
    }
    JceWebmInfo info;
    JceWebmParser *parser = jce_webm_open_memory(webm_data, webm_size, &info);
    if (!parser) return NULL;
    int err = 0;
    OpusDecoder *od = opus_decoder_create(48000, (int)channels, &err);
    if (!od || err != OPUS_OK) {
        if (od) opus_decoder_destroy(od);
        jce_webm_close(parser);
        return NULL;
    }

    WebmOpusStreamState *st = (WebmOpusStreamState *)JCE_CALLOC(
        1, sizeof(*st));
    if (!st) {
        opus_decoder_destroy(od);
        jce_webm_close(parser);
        return NULL;
    }
    st->parser     = parser;
    st->opus       = od;
    st->channels   = channels;
    st->samplerate = 48000u;

    JceAudioStreamDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.decode_next = webm_opus_stream_decode_next;
    desc.seek        = webm_opus_stream_seek;
    desc.destroy     = webm_opus_stream_destroy;
    desc.ud          = st;
    desc.channels    = channels;
    desc.samplerate  = 48000u;
    desc.duration_sec = duration_sec;

    JceAudioStream *s = jce_audio_stream_create(&desc);
    if (!s) {
        webm_opus_stream_destroy(st);
        return NULL;
    }
    return s;
}

/* ── Streaming codec adapter: MP4/Opus (true streaming) ──────────── */
typedef struct {
    JceMp4Parser *parser;
    OpusDecoder  *opus;
    uint32_t      channels;
    uint32_t      samplerate;
    uint32_t      timescale;
    uint32_t      sample_count;
    uint32_t      sample_idx;
    uint8_t      *sbuf;
    uint32_t      sbuf_cap;
    int16_t       scratch[5760 * 8];
    uint32_t      scratch_avail;
    uint32_t      scratch_pos;
} Mp4OpusStreamState;

static uint32_t mp4_opus_stream_decode_next(void *ud,
                                            int16_t *out, uint32_t max)
{
    Mp4OpusStreamState *st = (Mp4OpusStreamState *)ud;
    if (st->scratch_avail > 0) {
        uint32_t n = st->scratch_avail; if (n > max) n = max;
        memcpy(out,
               st->scratch + (size_t)st->scratch_pos * st->channels,
               (size_t)n * st->channels * sizeof(int16_t));
        st->scratch_pos   += n;
        st->scratch_avail -= n;
        return n;
    }
    while (st->sample_idx < st->sample_count) {
        JceMp4SampleInfo sinfo;
        if (!jce_mp4_parser_get_audio_sample(st->parser, st->sample_idx,
                                              &sinfo)) {
            st->sample_idx = st->sample_count;
            break;
        }
        if (sinfo.size_bytes > st->sbuf_cap) {
            JCE_FREE(st->sbuf);
            st->sbuf_cap = sinfo.size_bytes + 256u;
            st->sbuf = (uint8_t *)JCE_MALLOC(st->sbuf_cap);
            if (!st->sbuf) { st->sample_idx = st->sample_count; break; }
        }
        uint32_t copied = 0;
        if (!jce_mp4_parser_copy_audio_sample(st->parser, st->sample_idx,
                                               st->sbuf, st->sbuf_cap,
                                               &copied)) {
            st->sample_idx++;
            continue;
        }
        st->sample_idx++;
        int got = opus_decode(st->opus, st->sbuf, (opus_int32)copied,
                              st->scratch, 5760, 0);
        if (got <= 0) continue;
        uint32_t produced = (uint32_t)got;
        uint32_t n = produced > max ? max : produced;
        memcpy(out, st->scratch,
               (size_t)n * st->channels * sizeof(int16_t));
        st->scratch_pos   = n;
        st->scratch_avail = produced - n;
        return n;
    }
    return 0;
}

static void mp4_opus_stream_seek(void *ud, double sec)
{
    Mp4OpusStreamState *st = (Mp4OpusStreamState *)ud;
    if (sec < 0.0) sec = 0.0;
    uint64_t target_ts = (uint64_t)(sec * (double)st->timescale);
    uint32_t i = 0;
    for (; i < st->sample_count; ++i) {
        JceMp4SampleInfo info;
        if (!jce_mp4_parser_get_audio_sample(st->parser, i, &info)) break;
        if (info.timestamp >= target_ts) break;
    }
    st->sample_idx = i;
    if (st->opus) opus_decoder_ctl(st->opus, OPUS_RESET_STATE);
    st->scratch_avail = 0;
    st->scratch_pos   = 0;
}

static void mp4_opus_stream_destroy(void *ud)
{
    Mp4OpusStreamState *st = (Mp4OpusStreamState *)ud;
    if (!st) return;
    if (st->opus)   opus_decoder_destroy(st->opus);
    if (st->parser) jce_mp4_parser_close(st->parser);
    if (st->sbuf)   JCE_FREE(st->sbuf);
    JCE_FREE(st);
}

static JceAudioStream *create_mp4_opus_stream(const void *mp4_data,
                                              size_t mp4_size,
                                              uint32_t channels,
                                              uint32_t samplerate,
                                              uint32_t sample_count,
                                              double duration_sec)
{
    if (!mp4_data || mp4_size == 0 || channels == 0 || channels > 8) {
        return NULL;
    }
    JceMp4Info dummy;
    JceMp4Parser *parser = jce_mp4_parser_open_memory(mp4_data, mp4_size,
                                                       &dummy);
    if (!parser) return NULL;
    JceMp4AudioTrackInfo atr;
    if (!jce_mp4_parser_get_audio_track_info(parser, &atr)) {
        jce_mp4_parser_close(parser);
        return NULL;
    }
    int err = 0;
    OpusDecoder *od = opus_decoder_create(48000, (int)channels, &err);
    if (!od || err != OPUS_OK) {
        if (od) opus_decoder_destroy(od);
        jce_mp4_parser_close(parser);
        return NULL;
    }
    Mp4OpusStreamState *st = (Mp4OpusStreamState *)JCE_CALLOC(1, sizeof(*st));
    if (!st) {
        opus_decoder_destroy(od);
        jce_mp4_parser_close(parser);
        return NULL;
    }
    st->parser       = parser;
    st->opus         = od;
    st->channels     = channels;
    st->samplerate   = samplerate ? samplerate : 48000u;
    st->timescale    = atr.timescale ? atr.timescale : st->samplerate;
    st->sample_count = sample_count ? sample_count : atr.sample_count;
    st->sample_idx   = 0;

    JceAudioStreamDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.decode_next  = mp4_opus_stream_decode_next;
    desc.seek         = mp4_opus_stream_seek;
    desc.destroy      = mp4_opus_stream_destroy;
    desc.ud           = st;
    desc.channels     = channels;
    desc.samplerate   = st->samplerate;
    desc.duration_sec = duration_sec;

    JceAudioStream *s = jce_audio_stream_create(&desc);
    if (!s) { mp4_opus_stream_destroy(st); return NULL; }
    return s;
}

#ifdef JCE_ENABLE_PATENTED_CODECS
/* ── Streaming codec adapter: MP4/AAC (true streaming) ───────────── */
typedef struct {
    JceMp4Parser  *parser;
    JceAacDecoder *aac;
    uint32_t       channels;
    uint32_t       samplerate;
    uint32_t       timescale;
    uint32_t       sample_count;
    uint32_t       sample_idx;
    uint8_t       *sbuf;
    uint32_t       sbuf_cap;
    int16_t        scratch[2048 * 8];   /* AAC frame ≤ 1024 frames * 8 ch */
    uint32_t       scratch_avail;
    uint32_t       scratch_pos;
} Mp4AacStreamState;

static uint32_t mp4_aac_stream_decode_next(void *ud,
                                           int16_t *out, uint32_t max)
{
    Mp4AacStreamState *st = (Mp4AacStreamState *)ud;
    if (st->scratch_avail > 0) {
        uint32_t n = st->scratch_avail; if (n > max) n = max;
        memcpy(out,
               st->scratch + (size_t)st->scratch_pos * st->channels,
               (size_t)n * st->channels * sizeof(int16_t));
        st->scratch_pos   += n;
        st->scratch_avail -= n;
        return n;
    }
    while (st->sample_idx < st->sample_count) {
        JceMp4SampleInfo sinfo;
        if (!jce_mp4_parser_get_audio_sample(st->parser, st->sample_idx,
                                              &sinfo)) {
            st->sample_idx = st->sample_count;
            break;
        }
        if (sinfo.size_bytes > st->sbuf_cap) {
            JCE_FREE(st->sbuf);
            st->sbuf_cap = sinfo.size_bytes + 256u;
            st->sbuf = (uint8_t *)JCE_MALLOC(st->sbuf_cap);
            if (!st->sbuf) { st->sample_idx = st->sample_count; break; }
        }
        uint32_t copied = 0;
        if (!jce_mp4_parser_copy_audio_sample(st->parser, st->sample_idx,
                                               st->sbuf, st->sbuf_cap,
                                               &copied)) {
            st->sample_idx++;
            continue;
        }
        st->sample_idx++;
        uint32_t samp_out = 0;
        if (!jce_aac_decode_frame(st->aac, st->sbuf, copied,
                                   st->scratch,
                                   (uint32_t)(sizeof(st->scratch) / sizeof(int16_t)),
                                   &samp_out)) {
            continue;
        }
        if (samp_out == 0) continue;
        uint32_t produced = samp_out / st->channels;
        uint32_t n = produced > max ? max : produced;
        memcpy(out, st->scratch,
               (size_t)n * st->channels * sizeof(int16_t));
        st->scratch_pos   = n;
        st->scratch_avail = produced - n;
        return n;
    }
    return 0;
}

static void mp4_aac_stream_seek(void *ud, double sec)
{
    Mp4AacStreamState *st = (Mp4AacStreamState *)ud;
    if (sec < 0.0) sec = 0.0;
    uint64_t target_ts = (uint64_t)(sec * (double)st->timescale);
    uint32_t i = 0;
    for (; i < st->sample_count; ++i) {
        JceMp4SampleInfo info;
        if (!jce_mp4_parser_get_audio_sample(st->parser, i, &info)) break;
        if (info.timestamp >= target_ts) break;
    }
    st->sample_idx    = i;
    st->scratch_avail = 0;
    st->scratch_pos   = 0;
    /* AAC has no public decoder-reset — the next decode will produce
     * the right output (with possible click), which matches industry
     * baseline for non-crossfaded seeks. */
}

static void mp4_aac_stream_destroy(void *ud)
{
    Mp4AacStreamState *st = (Mp4AacStreamState *)ud;
    if (!st) return;
    if (st->aac)    jce_aac_decoder_close(st->aac);
    if (st->parser) jce_mp4_parser_close(st->parser);
    if (st->sbuf)   JCE_FREE(st->sbuf);
    JCE_FREE(st);
}

static JceAudioStream *create_mp4_aac_stream(const void *mp4_data,
                                             size_t mp4_size,
                                             const void *asc, uint32_t asc_bytes,
                                             uint32_t channels,
                                             uint32_t samplerate,
                                             uint32_t sample_count,
                                             double duration_sec)
{
    if (!mp4_data || mp4_size == 0 || !asc || asc_bytes == 0) return NULL;
    if (channels == 0 || channels > 8) return NULL;
    JceMp4Info dummy;
    JceMp4Parser *parser = jce_mp4_parser_open_memory(mp4_data, mp4_size,
                                                       &dummy);
    if (!parser) return NULL;
    JceMp4AudioTrackInfo atr;
    if (!jce_mp4_parser_get_audio_track_info(parser, &atr)) {
        jce_mp4_parser_close(parser);
        return NULL;
    }
    JceAacDecoder *aac = jce_aac_decoder_open(asc, asc_bytes);
    if (!aac) {
        jce_mp4_parser_close(parser);
        return NULL;
    }
    Mp4AacStreamState *st = (Mp4AacStreamState *)JCE_CALLOC(1, sizeof(*st));
    if (!st) {
        jce_aac_decoder_close(aac);
        jce_mp4_parser_close(parser);
        return NULL;
    }
    st->parser       = parser;
    st->aac          = aac;
    st->channels     = channels;
    st->samplerate   = samplerate;
    st->timescale    = atr.timescale ? atr.timescale : samplerate;
    st->sample_count = sample_count ? sample_count : atr.sample_count;

    JceAudioStreamDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.decode_next  = mp4_aac_stream_decode_next;
    desc.seek         = mp4_aac_stream_seek;
    desc.destroy      = mp4_aac_stream_destroy;
    desc.ud           = st;
    desc.channels     = channels;
    desc.samplerate   = samplerate;
    desc.duration_sec = duration_sec;

    JceAudioStream *s = jce_audio_stream_create(&desc);
    if (!s) { mp4_aac_stream_destroy(st); return NULL; }
    return s;
}
#endif /* JCE_ENABLE_PATENTED_CODECS */

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

/* Either stash YUV planes for queue push (Stage 2 YUV mode, AV1/VP8) or
 * convert to RGBA in slot->rgba (legacy / H264 / H265 path). Stashed
 * pointers reference decoder-owned memory and are valid only until the
 * next decode call — worker_push_current_frame must consume them first. */
static bool decoder_emit_frame_yuv420(VideoSlot *slot,
                                      const uint8_t *y, ptrdiff_t y_stride,
                                      const uint8_t *u, ptrdiff_t uv_stride,
                                      const uint8_t *v,
                                      uint32_t w, uint32_t h)
{
    if (!slot || w == 0u || h == 0u) return false;
    slot->decoder.width  = w;
    slot->decoder.height = h;
    if (slot->queue_yuv_mode) {
        slot->pending_yuv_y         = y;
        slot->pending_yuv_u         = u;
        slot->pending_yuv_v         = v;
        slot->pending_yuv_y_stride  = y_stride;
        slot->pending_yuv_uv_stride = uv_stride;
        slot->pending_yuv_w         = w;
        slot->pending_yuv_h         = h;
        slot->pending_yuv_valid     = true;
        return true;
    }
    if (!ensure_rgba(slot, w, h)) return false;
    const uint64_t t0 = perf_now();
    jce_yuv420_to_rgba(y, (int)y_stride, u, (int)uv_stride, v, (int)uv_stride,
                       slot->rgba, w, h);
    perf_ema_update(&slot->perf_convert_us_ema, &slot->perf_convert_us_last,
                    perf_us(slot, t0, perf_now()));
    return true;
}

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
        if (decoder_emit_frame_yuv420(slot, yp, ys, up, uvs, vp, fw, fh)) {
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
    if (!decoder_emit_frame_yuv420(slot, yp, ys, up, uvs, vp, fw, fh)) return false;

    /* IVF has no PTS table here — synthesize using IVF timebase and the
     * decoded-frame index. normalize_frame_time enforces monotonicity. */
    double ts = (double)slot->decoder.av1_frame_idx * av1_frame_interval(slot);
    slot->frame_time = normalize_frame_time(slot, ts, true);
    slot->decoder.av1_frame_idx++;
    slot->frame_counter++;
    slot->decoder.ended = false;
    return true;
}

static bool av1_decoder_seek(VideoSlot *slot, double time_sec, bool exact)
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
    const uint64_t start_ms = jce_time_ticks_ms();
    const uint64_t budget_ms = exact ? 500u : 100u;
    while (slot->decoder.av1_frame_idx < target_idx + 1u) {
        if (!av1_decoder_read_next(slot)) {
            /* Only bail on real EOF. AV1 has output delay — a packet
             * may be consumed without a frame coming back yet; we must
             * keep feeding to drain the pipeline. */
            if (slot->decoder.ended) break;
        }
        if ((jce_time_ticks_ms() - start_ms) > budget_ms) break;
    }
    /* Don't lie about where the decoder actually landed: leave
     * slot->frame_time at the real PTS of the last decoded frame so
     * the outer catch-up in jce_video_seek (and the per-tick decode
     * loop in jce_video_advance) can continue feeding samples until
     * the picture matches slot->time. */
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

    if (!decoder_emit_frame_yuv420(slot, yp, ys, up, uvs, vp, fw, fh)) return false;

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
    /* Anchor decode_ts_base to 0 so post-seek frame_time stays absolute.
     * MP4 sample timestamps are already absolute from track start, so no
     * offset is needed. See webm_decoder_seek for full rationale. */
    slot->frame_counter = 1;
    slot->decode_ts_base_set = true;
    slot->decode_ts_base_sec = 0.0;
    slot->time = time_sec;
    slot->frame_time = 0.0;
    /* Seq header may live only in sample 0 / earlier keyframes; if the
     * seek target is past those samples and av1C had configOBUs, prime
     * the decoder so the next sample's frame OBU isn't rejected. */
    mp4_av1_prime_seq_header(slot);

    /* Drain forward from the keyframe to the target. Bounded for inexact
     * (preview) seek so dragging stays smooth; unbounded for exact
     * (release) seek so audio actually aligns. */
    const uint64_t start_ms = jce_time_ticks_ms();
    while (slot->decoder.sample_idx < slot->decoder.vtrack.sample_count) {
        JceMp4SampleInfo si;
        if (!jce_mp4_parser_get_video_sample(slot->decoder.parser,
                                             slot->decoder.sample_idx, &si))
            break;
        bool got = false;
        mp4_av1_decode_one_sample(slot, slot->decoder.sample_idx, &got);
        slot->decoder.sample_idx++;
        if (si.timestamp >= target_ts) break;
        if (!exact && (jce_time_ticks_ms() - start_ms) > 50u) break;
    }
    /* Leave slot->frame_time at the real last-decoded PTS — see
     * av1_decoder_seek for the rationale. */
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
    if (!decoder_emit_frame_yuv420(slot, yp, ys, up, uvs, vp, fw, fh)) return false;

    double ts = (double)pts_ns / 1.0e9;
    slot->frame_time = normalize_frame_time(slot, ts, true);
    slot->decoder.webm_frame_idx++;
    slot->frame_counter++;
    slot->decoder.ended = false;
    return true;
}

static bool webm_decoder_seek(VideoSlot *slot, double time_sec, bool exact)
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
    /* Keep decode_ts_base anchored to 0 so post-seek frame_time stays
     * absolute (WebM SimpleBlock PTS are already segment-relative).
     * Setting frame_counter=1 + decode_ts_base_set=true bypasses the
     * "first frame seeds base" branch in normalize_frame_time, which
     * would otherwise re-anchor base to whatever PTS the first
     * post-seek packet happens to carry — making frame_time relative
     * to that point and decoupling it from slot->time (slider). */
    slot->frame_counter = 1;
    slot->decode_ts_base_set = true;
    slot->decode_ts_base_sec = 0.0;
    slot->time = time_sec;
    slot->frame_time = 0.0;

    /* Drain forward until we land on a frame at or past the target. */
    const uint64_t start_ms = jce_time_ticks_ms();
    const uint64_t budget_ms = exact ? 500u : 100u;
    while (slot->frame_time + 1e-3 < time_sec) {
        if (!webm_decoder_read_next(slot)) {
            /* VP8/AV1 may consume a packet without producing a frame
             * yet (output delay / B-frame reorder). Only abort on
             * true EOF — otherwise keep feeding. */
            if (slot->decoder.ended) break;
        }
        if ((jce_time_ticks_ms() - start_ms) > budget_ms) break;
    }
    /* Leave slot->frame_time at its real last-decoded PTS. */
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
        const uint64_t seek_start_ms = jce_time_ticks_ms();
        while (slot->decoder.sample_idx <= target_idx
               && slot->decoder.sample_idx < sc) {
            if (!decoder_read_next(slot)) {
                if (slot->decoder.ended) break;
            }
            /* Cap wall-clock time so the UI stays responsive. The exact
             * (release) seek gets a generous 500 ms so the picture
             * actually lands on the target frame; otherwise the lie
             * `frame_time = time_sec` below would mask a stale picture
             * and the audio would visibly lead. */
            if ((jce_time_ticks_ms() - seek_start_ms) > 500u)
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
    /* Do NOT overwrite slot->frame_time with time_sec here. The real
     * PTS of the last decoded frame (set by decoder_read_next via
     * normalize_frame_time) is what advance()'s catch-up loop needs to
     * keep feeding samples until the picture catches up to slot->time.
     * Lying about it makes the picture freeze on the keyframe while
     * audio plays forward — visible as "audio leads, picture late". */
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
 *  Async decode pipeline — Stage 1: per-slot worker thread + RGBA queue
 *
 * The worker thread:
 *   - Owns the parser + decoder exclusively (no locks needed for those)
 *   - Loops decoder_read_next; pushes the decoded RGBA into the queue
 *   - Handles seek / loop-back EOF autonomously
 *
 * The UI thread (jce_video_advance):
 *   - Reads audio_master clock as before
 *   - Pops the queue head whose PTS <= target+eps; copies into display_rgba
 *
 * Synchronization: a single mutex (qmtx) guards: q_head, q_tail, q_count,
 * quit_request, seek_request, seek_target_sec, seek_exact, worker_eof,
 * seek_serial, qslots[*].serial. Two condition variables: q_not_full
 * (waited by worker, signaled by UI on pop) and q_not_empty (waited by
 * UI when forcibly waiting on seek-resync, signaled by worker on push).
 * ====================================================================== */

static VideoQueueEntry *qe_at(const VideoSlot *slot, int idx)
{
    return ((VideoQueueEntry *)slot->qslots) + idx;
}

static void queue_clear_locked(VideoSlot *slot)
{
    /* Caller holds qmtx. Buffers stay allocated for reuse. */
    slot->q_head = 0;
    slot->q_tail = 0;
    slot->q_count = 0;
}

static bool queue_init(VideoSlot *slot)
{
    slot->qslots = JCE_CALLOC(JCE_VIDEO_Q_CAP, sizeof(VideoQueueEntry));
    if (!slot->qslots) return false;
    slot->qmtx = jce_mutex_create();
    slot->q_not_full = jce_cond_create();
    slot->q_not_empty = jce_cond_create();
    if (!slot->qmtx || !slot->q_not_full || !slot->q_not_empty) {
        if (slot->qmtx) jce_mutex_destroy(slot->qmtx);
        if (slot->q_not_full) jce_cond_destroy(slot->q_not_full);
        if (slot->q_not_empty) jce_cond_destroy(slot->q_not_empty);
        JCE_FREE(slot->qslots);
        slot->qslots = NULL; slot->qmtx = NULL;
        slot->q_not_full = NULL; slot->q_not_empty = NULL;
        return false;
    }
    slot->q_head = slot->q_tail = slot->q_count = 0;
    slot->seek_serial = 1;
    slot->quit_request = false;
    slot->seek_request = false;
    slot->seek_target_sec = 0.0;
    slot->seek_exact = false;
    slot->worker_eof = false;
    return true;
}

static void queue_destroy(VideoSlot *slot)
{
    if (slot->qslots) {
        for (int i = 0; i < JCE_VIDEO_Q_CAP; ++i) {
            VideoQueueEntry *e = qe_at(slot, i);
            if (e->rgba) JCE_FREE(e->rgba);
            if (e->y) JCE_FREE(e->y);
            if (e->u) JCE_FREE(e->u);
            if (e->v) JCE_FREE(e->v);
        }
        JCE_FREE(slot->qslots);
        slot->qslots = NULL;
    }
    if (slot->qmtx) { jce_mutex_destroy(slot->qmtx); slot->qmtx = NULL; }
    if (slot->q_not_full) { jce_cond_destroy(slot->q_not_full); slot->q_not_full = NULL; }
    if (slot->q_not_empty) { jce_cond_destroy(slot->q_not_empty); slot->q_not_empty = NULL; }
}

/* Worker thread: pushes one decoded frame into the next free queue slot.
 * Routes to RGBA or YUV-planes payload based on slot->queue_yuv_mode.
 * Must be called only from the worker thread (touches decoder and
 * queue). Returns false on push failure (alloc OOM); true otherwise. */
static bool worker_push_current_frame(VideoSlot *slot, uint64_t serial)
{
    /* YUV mode (Stage 2: AV1 / VP8 — saves ~100MB at 4K vs RGBA). */
    if (slot->queue_yuv_mode && slot->pending_yuv_valid) {
        const uint32_t w = slot->pending_yuv_w;
        const uint32_t h = slot->pending_yuv_h;
        if (w == 0u || h == 0u) { slot->pending_yuv_valid = false; return true; }
        const size_t y_need = (size_t)w * (size_t)h;
        const size_t uv_need = (size_t)(w / 2u) * (size_t)(h / 2u);

        jce_mutex_lock(slot->qmtx);
        while (!slot->quit_request && !slot->seek_request
            && slot->q_count >= JCE_VIDEO_Q_CAP) {
            /* S7: if the oldest queued frame is >2 s behind the incoming PTS
             * it will be superseded before the UI pops it anyway.  Drop it now
             * so the worker stays current and avoids blocking for an entire
             * frame budget.  Conservative threshold prevents spurious drops at
             * normal 60 fps cadence (~16 ms inter-frame). */
            VideoQueueEntry *head = qe_at(slot, slot->q_head);
            if (head->pts_sec + 2.0 < slot->frame_time) {
                slot->q_head = (slot->q_head + 1) % JCE_VIDEO_Q_CAP;
                slot->q_count--;
                slot->perf_frames_dropped++;
                break;
            }
            jce_cond_wait(slot->q_not_full, slot->qmtx);
        }
        if (slot->quit_request || slot->seek_request) {
            jce_mutex_unlock(slot->qmtx);
            slot->pending_yuv_valid = false;
            return true;
        }

        VideoQueueEntry *e = qe_at(slot, slot->q_tail);
        /* Free any RGBA buffer that may have been allocated in legacy mode. */
        if (e->rgba) { JCE_FREE(e->rgba); e->rgba = NULL; e->rgba_capacity = 0; }
        if (e->y_capacity < y_need) {
            JCE_FREE(e->y);
            e->y = (uint8_t *)JCE_MALLOC(y_need);
            if (!e->y) { e->y_capacity = 0; jce_mutex_unlock(slot->qmtx); return false; }
            e->y_capacity = y_need;
        }
        if (e->u_capacity < uv_need) {
            JCE_FREE(e->u);
            e->u = (uint8_t *)JCE_MALLOC(uv_need);
            if (!e->u) { e->u_capacity = 0; jce_mutex_unlock(slot->qmtx); return false; }
            e->u_capacity = uv_need;
        }
        if (e->v_capacity < uv_need) {
            JCE_FREE(e->v);
            e->v = (uint8_t *)JCE_MALLOC(uv_need);
            if (!e->v) { e->v_capacity = 0; jce_mutex_unlock(slot->qmtx); return false; }
            e->v_capacity = uv_need;
        }
        /* Copy planes tight (drop dav1d/libvpx padding strides). */
        const ptrdiff_t srcy = slot->pending_yuv_y_stride;
        const ptrdiff_t srcuv = slot->pending_yuv_uv_stride;
        const uint32_t hw = w / 2u;
        const uint32_t hh = h / 2u;
        for (uint32_t row = 0; row < h; ++row) {
            memcpy(e->y + (size_t)row * w, slot->pending_yuv_y + (size_t)row * srcy, w);
        }
        for (uint32_t row = 0; row < hh; ++row) {
            memcpy(e->u + (size_t)row * hw, slot->pending_yuv_u + (size_t)row * srcuv, hw);
            memcpy(e->v + (size_t)row * hw, slot->pending_yuv_v + (size_t)row * srcuv, hw);
        }
        e->y_stride  = (int)w;
        e->uv_stride = (int)hw;
        e->is_yuv = true;
        e->w = (int)w;
        e->h = (int)h;
        e->pts_sec = slot->frame_time;
        e->serial  = serial;

        slot->q_tail = (slot->q_tail + 1) % JCE_VIDEO_Q_CAP;
        slot->q_count++;
        jce_cond_signal(slot->q_not_empty);
        jce_mutex_unlock(slot->qmtx);
        slot->pending_yuv_valid = false;
        return true;
    }

    /* RGBA mode (legacy, H264 / H265). */
    if (!slot->rgba || slot->rgba_w <= 0 || slot->rgba_h <= 0) return true;
    size_t need = (size_t)slot->rgba_w * (size_t)slot->rgba_h * 4u;

    jce_mutex_lock(slot->qmtx);
    /* Wait until queue has space (or quit/seek). S7: drop stale head. */
    while (!slot->quit_request && !slot->seek_request
        && slot->q_count >= JCE_VIDEO_Q_CAP) {
        VideoQueueEntry *head = qe_at(slot, slot->q_head);
        if (head->pts_sec + 2.0 < slot->frame_time) {
            slot->q_head = (slot->q_head + 1) % JCE_VIDEO_Q_CAP;
            slot->q_count--;
            slot->perf_frames_dropped++;
            break;
        }
        jce_cond_wait(slot->q_not_full, slot->qmtx);
    }
    if (slot->quit_request || slot->seek_request) {
        jce_mutex_unlock(slot->qmtx);
        return true;
    }

    VideoQueueEntry *e = qe_at(slot, slot->q_tail);
    if (e->rgba_capacity < need) {
        JCE_FREE(e->rgba);
        e->rgba = (uint8_t *)JCE_MALLOC(need);
        if (!e->rgba) { e->rgba_capacity = 0; jce_mutex_unlock(slot->qmtx); return false; }
        e->rgba_capacity = need;
    }
    memcpy(e->rgba, slot->rgba, need);
    e->is_yuv = false;
    e->w = slot->rgba_w;
    e->h = slot->rgba_h;
    e->pts_sec = slot->frame_time;
    e->serial = serial;

    slot->q_tail = (slot->q_tail + 1) % JCE_VIDEO_Q_CAP;
    slot->q_count++;
    jce_cond_signal(slot->q_not_empty);
    jce_mutex_unlock(slot->qmtx);
    return true;
}

static void worker_main(VideoSlot *slot)
{
    uint64_t my_serial;
    {
        jce_mutex_lock(slot->qmtx);
        my_serial = slot->seek_serial;
        jce_mutex_unlock(slot->qmtx);
    }

    int consec_fail = 0;
    while (true) {
        /* Honor quit / seek commands FIRST. */
        bool do_seek = false;
        double seek_t = 0.0;
        bool   seek_exact = false;
        jce_mutex_lock(slot->qmtx);
        if (slot->quit_request) {
            jce_mutex_unlock(slot->qmtx);
            return;
        }
        if (slot->seek_request) {
            do_seek = true;
            seek_t  = slot->seek_target_sec;
            seek_exact = slot->seek_exact;
            slot->seek_request = false;
            queue_clear_locked(slot);
            slot->worker_eof = false;
            my_serial = slot->seek_serial;
            jce_cond_broadcast(slot->q_not_full);   /* wake any pusher waiting */
        }
        jce_mutex_unlock(slot->qmtx);

        if (do_seek) {
            slot->post_seek = true;
            (void)decoder_seek(slot, seek_t, seek_exact);
            slot->post_seek = false;
            consec_fail = 0;
            continue;
        }

        /* If we've hit EOF without loop, idle until quit/seek. */
        if (slot->decoder.ended) {
            jce_mutex_lock(slot->qmtx);
            slot->worker_eof = true;
            while (!slot->quit_request && !slot->seek_request) {
                jce_cond_wait(slot->q_not_full, slot->qmtx);
            }
            jce_mutex_unlock(slot->qmtx);
            consec_fail = 0;
            continue;
        }

        /* Loop-back: if we're at end and looping, seek to 0 ourselves. */
        if (slot->loop && slot->duration > 0.0
            && slot->frame_time >= slot->duration - 0.001) {
            slot->post_seek = true;
            if (decoder_seek(slot, 0.0, false)) {
                slot->frame_time = 0.0;
            }
            slot->post_seek = false;
            consec_fail = 0;
            continue;
        }

        /* Decode one frame. */
        const uint64_t t_dec0 = perf_now();
        bool ok = decoder_read_next(slot);
        const uint64_t t_dec1 = perf_now();
        if (ok) {
            perf_ema_update(&slot->perf_decode_us_ema,
                            &slot->perf_decode_us_last,
                            perf_us(slot, t_dec0, t_dec1));
            slot->perf_frames_decoded++;
        }
        if (!ok) {
            if (slot->decoder.ended) {
                /* Mark EOF; wait for quit/seek. */
                jce_mutex_lock(slot->qmtx);
                slot->worker_eof = true;
                jce_cond_signal(slot->q_not_empty);
                jce_mutex_unlock(slot->qmtx);
                consec_fail = 0;
                continue;
            }
            /* B-frame reorder OR transient decoder rejection (e.g. dav1d
             * EINVAL on a tricky 4K AV1 packet). Spin a few times to
             * absorb the normal reorder latency, then back off so we
             * don't pin a CPU core and starve the UI thread. */
            ++consec_fail;
            if (consec_fail >= 8) {
                jce_thread_sleep_ms(2);
                if (consec_fail >= 64) jce_thread_sleep_ms(8);
            }
            continue;
        }
        consec_fail = 0;

        const uint64_t t_push0 = perf_now();
        const bool push_ok = worker_push_current_frame(slot, my_serial);
        perf_ema_update(&slot->perf_push_us_ema,
                        &slot->perf_push_us_last,
                        perf_us(slot, t_push0, perf_now()));
        if (!push_ok) {
            /* OOM on queue buffer; back off briefly. */
            jce_thread_sleep_ms(10);
        }
    }
}

static void worker_thread_entry(void *arg)
{
    worker_main((VideoSlot *)arg);
}

static bool worker_start(VideoSlot *slot)
{
    if (slot->queue_active) return true;
    if (!queue_init(slot)) return false;
    /* S4: YUV queue mode enabled.  The queue now stores raw YUV420 planes
     * (~12 MB/frame at 4K vs ~33 MB RGBA), and YUV→RGBA conversion (S3 AVX2,
     * now ~5 ms @ 4K) runs OUTSIDE the queue lock in ui_pop_for_target so the
     * worker can keep decoding concurrently.  RAM footprint drops ~120 MB vs
     * the old RGBA-in-queue path for a 4-slot queue at 4K.
     * Requires: S3 (AVX2 conversion) to keep UI thread < 8 ms/frame. */
    slot->queue_yuv_mode = true;
    slot->pending_yuv_valid = false;
    slot->queue_active = true;
    slot->worker_running = true;
    slot->worker_thread = jce_thread_create(worker_thread_entry, slot,
                                            "jce_video_worker");
    if (!slot->worker_thread) {
        slot->queue_active = false;
        slot->worker_running = false;
        queue_destroy(slot);
        return false;
    }
    return true;
}

static void worker_stop(VideoSlot *slot)
{
    if (!slot->queue_active) return;
    jce_mutex_lock(slot->qmtx);
    slot->quit_request = true;
    jce_cond_broadcast(slot->q_not_full);
    jce_cond_broadcast(slot->q_not_empty);
    jce_mutex_unlock(slot->qmtx);
    if (slot->worker_thread) {
        jce_thread_join((JceThread *)slot->worker_thread);
        slot->worker_thread = NULL;
    }
    slot->worker_running = false;
    slot->queue_active = false;
    queue_destroy(slot);
}

/* UI-side request: bump serial, set seek params, signal worker. */
static void worker_request_seek(VideoSlot *slot, double target_sec, bool exact)
{
    if (!slot->queue_active) return;
    jce_mutex_lock(slot->qmtx);
    slot->seek_request = true;
    slot->seek_target_sec = target_sec;
    slot->seek_exact = exact;
    slot->seek_serial++;
    queue_clear_locked(slot);
    slot->worker_eof = false;
    jce_cond_broadcast(slot->q_not_full);
    jce_cond_broadcast(slot->q_not_empty);
    jce_mutex_unlock(slot->qmtx);
}

/* Ensure display_rgba has at least w*h*4 capacity (only realloc on
 * shortage; track w/h as the active payload extents). */
static bool ensure_display_rgba(VideoSlot *slot, int w, int h)
{
    if (w <= 0 || h <= 0) return false;
    size_t need = (size_t)w * (size_t)h * 4u;
    if (slot->display_rgba && slot->display_rgba_capacity >= need) {
        slot->display_rgba_w = w;
        slot->display_rgba_h = h;
        return true;
    }
    JCE_FREE(slot->display_rgba);
    slot->display_rgba = (uint8_t *)JCE_MALLOC(need);
    if (!slot->display_rgba) {
        slot->display_rgba_w = 0;
        slot->display_rgba_h = 0;
        slot->display_rgba_capacity = 0;
        return false;
    }
    slot->display_rgba_capacity = need;
    slot->display_rgba_w = w;
    slot->display_rgba_h = h;
    return true;
}

/* Swap pointers/capacities between an entry plane and the display-side
 * scratch slot. Worker reuses entry buffers when refilling — sizes will
 * be fixed up by realloc-on-grow at next push, so size mismatches are
 * benign. */
static inline void swap_buf(uint8_t **a, size_t *acap, uint8_t **b, size_t *bcap)
{
    uint8_t *tp = *a; *a = *b; *b = tp;
    size_t   tc = *acap; *acap = *bcap; *bcap = tc;
}

/* UI pop: pop the single most-appropriate frame whose PTS <= target+eps.
 * Returns true if a new frame was displayed. Drops over-stale frames
 * (>2s behind target) silently — these come from the keyframe walk after
 * a seek to a mid-clip target.
 *
 * Stage 2 perf: the lock is released BEFORE the YUV → RGBA conversion
 * (or RGBA memcpy) so the worker thread can immediately push the next
 * frame. This avoids ~5-15ms (4K SIMD) of blocked-decode that
 * desynchronises audio/video and stutters the main thread. */
static bool ui_pop_for_target(VideoSlot *slot, double target_sec)
{
    if (!slot->queue_active) return false;
    const uint64_t t_pop0 = perf_now();
    bool   have_frame = false;
    bool   is_yuv = false;
    int    fw = 0, fh = 0;
    double pts = 0.0;
    int    drops_this_call = 0;

    jce_mutex_lock(slot->qmtx);
    uint64_t cur_serial = slot->seek_serial;

    /* Drop frames from older serials (post-seek leftovers). */
    while (slot->q_count > 0) {
        VideoQueueEntry *e = qe_at(slot, slot->q_head);
        if (e->serial == cur_serial) break;
        slot->q_head = (slot->q_head + 1) % JCE_VIDEO_Q_CAP;
        slot->q_count--;
    }

    /* Drop drastically stale frames (post-seek keyframe walk artifacts). */
    while (slot->q_count > 1) {
        VideoQueueEntry *e = qe_at(slot, slot->q_head);
        if (e->pts_sec >= target_sec - 2.0) break;
        slot->q_head = (slot->q_head + 1) % JCE_VIDEO_Q_CAP;
        slot->q_count--;
        ++drops_this_call;
    }

    /* Pop all frames with pts <= target (display only the LAST). */
    VideoQueueEntry *display_e = NULL;
    while (slot->q_count > 0) {
        VideoQueueEntry *e = qe_at(slot, slot->q_head);
        if (e->pts_sec > target_sec + 0.0005) break;
        if (display_e) ++drops_this_call;  /* superseded by a newer pop */
        display_e = e;
        slot->q_head = (slot->q_head + 1) % JCE_VIDEO_Q_CAP;
        slot->q_count--;
    }

    if (display_e) {
        is_yuv = display_e->is_yuv;
        fw = display_e->w;
        fh = display_e->h;
        pts = display_e->pts_sec;
        if (is_yuv) {
            swap_buf(&slot->display_y, &slot->display_y_capacity,
                     &display_e->y,    &display_e->y_capacity);
            swap_buf(&slot->display_u, &slot->display_u_capacity,
                     &display_e->u,    &display_e->u_capacity);
            swap_buf(&slot->display_v, &slot->display_v_capacity,
                     &display_e->v,    &display_e->v_capacity);
            slot->display_y_stride  = display_e->y_stride;
            slot->display_uv_stride = display_e->uv_stride;
            slot->display_yuv_w     = fw;
            slot->display_yuv_h     = fh;
            slot->display_yuv_pending = true;
        } else {
            /* RGBA path: zero-copy swap pointers (no 33MB memcpy under
             * lock, so the worker can push the next frame immediately).
             * The display buffer's capacity may temporarily not match the
             * exact w*h*4 of this frame, but it is always at least that
             * size because the worker's push allocates rgba_capacity =
             * (current frame bytes) before swapping. After swap we mark
             * the active extents (w/h) so consumers know what to read. */
            const size_t need = (size_t)fw * (size_t)fh * 4u;
            swap_buf(&slot->display_rgba, &slot->display_rgba_capacity,
                     &display_e->rgba,    &display_e->rgba_capacity);
            slot->display_rgba_w = fw;
            slot->display_rgba_h = fh;
            /* Defensive: if the swap left us with a too-small buffer
             * (shouldn't happen in normal operation), realloc up. */
            if (slot->display_rgba_capacity < need) {
                if (!ensure_display_rgba(slot, fw, fh)) {
                    jce_mutex_unlock(slot->qmtx);
                    return false;
                }
            }
        }
        have_frame = true;
        jce_cond_broadcast(slot->q_not_full);
    }
    jce_mutex_unlock(slot->qmtx);

    if (!have_frame) return false;

    /* OUTSIDE the lock: do the SIMD YUV → RGBA so the worker can push
     * concurrently. RGBA mode is already done (swap copied the pixels). */
    if (is_yuv) {
        if (!ensure_display_rgba(slot, fw, fh)) return false;
        uint64_t t_conv0 = perf_now();
        jce_yuv420_to_rgba(slot->display_y, slot->display_y_stride,
                           slot->display_u, slot->display_uv_stride,
                           slot->display_v, slot->display_uv_stride,
                           slot->display_rgba,
                           (uint32_t)fw, (uint32_t)fh);
        perf_ema_update(&slot->perf_convert_us_ema, &slot->perf_convert_us_last,
                        perf_us(slot, t_conv0, perf_now()));
        slot->display_yuv_pending = false;
    }
    slot->display_frame_time = pts;
    slot->display_frame_counter++;
    slot->perf_frames_displayed++;
    slot->perf_frames_dropped += (uint64_t)drops_this_call;
    perf_ema_update(&slot->perf_pop_us_ema, &slot->perf_pop_us_last,
                    perf_us(slot, t_pop0, perf_now()));
    return true;
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

        if (!worker_start(slot)) {
            LOG_ERROR(LOG_TAG, "failed to start decode worker (IVF/AV1)");
        }

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
            /* Streaming WebM/Opus: keep a private copy of the WebM bytes
             * alive on the slot, build a JceAudioStream that owns its
             * own webm parser + Opus decoder, and decode lazily as the
             * audio engine pulls. No background blob worker, no 22 MB
             * allocation. */
            uint32_t channels = slot->decoder.webm_audio_channels;
            uint8_t *wcopy = (uint8_t *)JCE_MALLOC(size);
            snprintf(slot->audio_codec, sizeof(slot->audio_codec), "opus");
            if (!wcopy) {
                set_audio_status(slot,
                    JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                    "out of memory for WebM Opus streaming buffer");
            } else {
                memcpy(wcopy, data, size);
                JceAudioStream *as = create_webm_opus_stream(
                    wcopy, (size_t)size, channels, slot->duration);
                if (!as) {
                    JCE_FREE(wcopy);
                    set_audio_status(slot,
                        JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                        "failed to initialise WebM Opus stream");
                } else {
                    slot->webm_data            = wcopy;
                    slot->webm_data_size       = (size_t)size;
                    slot->audio_stream         = as;
                    slot->has_audio            = true;
                    slot->samplerate           = 48000;
                    slot->audio_channels       = (int)channels;
                    slot->audio_pcm_channels   = channels;
                    slot->audio_pcm_samplerate = 48000u;
                    slot->audio_pcm_frames     = (uint32_t)
                        (slot->duration * 48000.0);
                    slot->audio_sample_count   = slot->audio_pcm_frames;
                    set_audio_status(slot, JCE_VIDEO_AUDIO_STATUS_READY,
                                     "WebM Opus streaming ready");
                    LOG_INFO(LOG_TAG,
                        "WebM Opus streaming initialised (%uch @ 48000Hz)",
                        channels);
                }
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

        if (!worker_start(slot)) {
            LOG_ERROR(LOG_TAG, "failed to start decode worker (WebM)");
        }

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
                /* ── Streaming AAC: ring-buffered, no full PCM blob ─ */
                uint8_t *mp4_copy = (uint8_t *)JCE_MALLOC(size);
                uint8_t *cfg_copy = (uint8_t *)JCE_MALLOC(
                    atr.decoder_config_bytes);
                if (!mp4_copy || !cfg_copy) {
                    JCE_FREE(mp4_copy); JCE_FREE(cfg_copy);
                    set_audio_status(slot,
                        JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                        "out of memory for streaming AAC buffers");
                } else {
                    memcpy(mp4_copy, data, size);
                    memcpy(cfg_copy, atr.decoder_config,
                           atr.decoder_config_bytes);
                    uint32_t ch  = atr.channels    ? atr.channels    : 2u;
                    uint32_t sr  = atr.samplerate_hz ? atr.samplerate_hz : 48000u;
                    JceAudioStream *as = create_mp4_aac_stream(
                        mp4_copy, (size_t)size,
                        cfg_copy, atr.decoder_config_bytes,
                        ch, sr, atr.sample_count, mp4.duration_seconds);
                    /* fdk-aac copies ASC into its own state in
                     * aacDecoder_ConfigRaw — cfg_copy can be freed now. */
                    JCE_FREE(cfg_copy);
                    if (!as) {
                        JCE_FREE(mp4_copy);
                        set_audio_status(slot,
                            JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                            "failed to init streaming AAC source");
                    } else {
                        slot->webm_data            = mp4_copy;
                        slot->webm_data_size       = (size_t)size;
                        slot->audio_stream         = as;
                        slot->samplerate           = (int)sr;
                        slot->audio_channels       = (int)ch;
                        slot->audio_pcm_channels   = ch;
                        slot->audio_pcm_samplerate = sr;
                        slot->audio_pcm_frames     = (uint32_t)
                            (mp4.duration_seconds * (double)sr);
                        slot->audio_sample_count   = slot->audio_pcm_frames;
                        set_audio_status(slot,
                            JCE_VIDEO_AUDIO_STATUS_READY,
                            "AAC streaming source ready");
                        LOG_INFO(LOG_TAG,
                            "MP4 AAC streaming initialised (%uch @ %uHz)",
                            ch, sr);
                    }
                }
#endif /* JCE_ENABLE_PATENTED_CODECS */
            } else if (strcmp(atr.codec, "mp4a") == 0) {
                set_audio_status(slot,
                                 JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                                 "AAC track found but decoder config missing");
            } else if (strcmp(atr.codec, "opus") == 0) {
                /* ── Streaming MP4 Opus: ring-buffered, no full PCM blob ─ */
                uint8_t *mp4_copy = (uint8_t *)JCE_MALLOC(size);
                if (!mp4_copy) {
                    set_audio_status(slot,
                        JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                        "out of memory for streaming MP4 Opus buffer");
                } else {
                    memcpy(mp4_copy, data, size);
                    uint32_t ch  = atr.channels    ? atr.channels    : 2u;
                    uint32_t sr  = atr.samplerate_hz ? atr.samplerate_hz : 48000u;
                    JceAudioStream *as = create_mp4_opus_stream(
                        mp4_copy, (size_t)size, ch, sr,
                        atr.sample_count, mp4.duration_seconds);
                    if (!as) {
                        JCE_FREE(mp4_copy);
                        set_audio_status(slot,
                            JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                            "failed to init streaming MP4 Opus source");
                    } else {
                        slot->webm_data            = mp4_copy;
                        slot->webm_data_size       = (size_t)size;
                        slot->audio_stream         = as;
                        slot->samplerate           = (int)sr;
                        slot->audio_channels       = (int)ch;
                        slot->audio_pcm_channels   = ch;
                        slot->audio_pcm_samplerate = sr;
                        slot->audio_pcm_frames     = (uint32_t)
                            (mp4.duration_seconds * (double)sr);
                        slot->audio_sample_count   = slot->audio_pcm_frames;
                        set_audio_status(slot,
                            JCE_VIDEO_AUDIO_STATUS_READY,
                            "MP4 Opus streaming source ready");
                        LOG_INFO(LOG_TAG,
                            "MP4 Opus streaming initialised (%uch @ %uHz)",
                            ch, sr);
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

    if (!slot->metadata_only) {
        if (!worker_start(slot)) {
            LOG_ERROR(LOG_TAG, "failed to start decode worker (MP4)");
        }
    }

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

    /* Stop the decode worker FIRST — it owns the decoder/parser. */
    worker_stop(slot);

    /* Tear the streaming source down BEFORE the underlying buffers it
     * borrows (audio_pcm or webm_data). Its worker thread reads from
     * those, so the order matters. */
    if (slot->audio_stream) {
        jce_audio_stream_destroy(slot->audio_stream);
        slot->audio_stream = NULL;
    }

    decoder_close(slot);

    if (slot->audio_pcm) { JCE_FREE(slot->audio_pcm); slot->audio_pcm = NULL; }
    if (slot->webm_data) { JCE_FREE(slot->webm_data); slot->webm_data = NULL; }
    if (slot->rgba) { JCE_FREE(slot->rgba); slot->rgba = NULL; }
    if (slot->display_rgba) { JCE_FREE(slot->display_rgba); slot->display_rgba = NULL; }
    if (slot->display_y) { JCE_FREE(slot->display_y); slot->display_y = NULL; slot->display_y_capacity = 0; }
    if (slot->display_u) { JCE_FREE(slot->display_u); slot->display_u = NULL; slot->display_u_capacity = 0; }
    if (slot->display_v) { JCE_FREE(slot->display_v); slot->display_v = NULL; slot->display_v_capacity = 0; }
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
    /* ── Audio-master clock ──────────────────────────────────────
     * If we have an audio stream that has actually started producing
     * samples, slave the playback clock to the audio device clock.
     * This is the canonical FFplay/MPV approach: audio plays at
     * hardware rate (which IS wall-clock); video decodes/displays
     * frames whose PTS matches the audio clock. Without this the
     * video clock and audio clock drift independently.
     *
     * Fallback to wall-clock dt when:
     *  - no audio stream (silent video / image sequence);
     *  - audio not yet primed (cold start / immediately post-seek);
     *  - audio EOF + drained (audio track ended but video continues).
     */
    double target;
    bool audio_master = false;
    if (slot->audio_stream
        && jce_audio_stream_is_primed(slot->audio_stream)
        && !jce_audio_stream_eof(slot->audio_stream)) {
        target = jce_audio_stream_get_time(slot->audio_stream);
        audio_master = true;
        /* Guard: never let audio drag the clock backward (a stale pull
         * that returned <0 frames between get_time samples could in
         * theory measure as a tiny regression). */
        if (target < slot->time) target = slot->time;
        /* Bound forward jumps to avoid swallowing a huge gap in one
         * tick when audio races ahead during decoder hiccups. Allow up
         * to 2× the wall-clock dt of catch-up per tick. */
        double max_step = dt_seconds * 2.0;
        if (target > slot->time + max_step) target = slot->time + max_step;
    } else {
        target = slot->time + dt_seconds;
    }
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
                if (slot->audio_stream)
                    jce_audio_stream_seek(slot->audio_stream, 0.0);
            }
        } else if (target >= slot->duration) {
            target = slot->duration;
        }
    }
    (void)audio_master;
    slot->time = target;

    /* ── Async path (preferred): pop frame from worker queue ──────── */
    if (slot->queue_active) {
        ui_pop_for_target(slot, slot->time);

        /* Loop-back audio reset: if worker has reset itself to 0 on loop,
         * reset audio too. Detect by frame_time going backward. */
        if (slot->loop && slot->duration > 0.0
            && slot->display_frame_time + 1.0 < slot->time
            && slot->time > slot->duration * 0.9) {
            /* Crossing the loop boundary; audio_stream needs to follow. */
            if (slot->audio_stream)
                jce_audio_stream_seek(slot->audio_stream, 0.0);
        }

        /* End-of-clip detection (no loop). */
        if (slot->duration > 0.0 && !slot->loop
            && slot->time >= slot->duration - 0.000001) {
            bool worker_done;
            jce_mutex_lock(slot->qmtx);
            worker_done = slot->worker_eof && slot->q_count == 0;
            jce_mutex_unlock(slot->qmtx);
            bool audio_done = !slot->audio_stream
                           || jce_audio_stream_eof(slot->audio_stream);
            bool video_caught_up =
                slot->display_frame_time + 0.250 >= slot->duration
                || worker_done;
            if (audio_done && video_caught_up) {
                slot->ended = true;
            }
        }
        return;
    }

    /* ── Legacy synchronous path (metadata-only fallback / no worker) ── */
    const uint64_t pixels = (uint64_t)(slot->width > 0 ? slot->width : 0)
                          * (uint64_t)(slot->height > 0 ? slot->height : 0);
    const bool heavy_decode = pixels >= (uint64_t)2560u * (uint64_t)1440u;
    const int max_decode_per_tick = heavy_decode ? 2 : 8;
    const uint64_t decode_budget_ms = heavy_decode ? 6u : 12u;
    const uint64_t decode_start_ms = jce_time_ticks_ms();

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
            if ((jce_time_ticks_ms() - decode_start_ms) >= decode_budget_ms)
                break;
            continue;
        }
        slot->ended = false;
        ++safety;

        if ((jce_time_ticks_ms() - decode_start_ms) >= decode_budget_ms) {
            break;
        }
    }

    /* NOTE: previously, when the decoder couldn't keep up within the
     * per-tick budget, we capped slot->time backward to
     * `frame_time + frame_interval`. That made the playback clock walk
     * SLOWER than wall-clock during heavy decode, so video would
     * finish well after audio. With audio as the reference clock, we
     * instead let slot->time keep matching wall-clock dt; the
     * displayed frame may be a few frames stale during decode stalls,
     * but the clock stays in sync with audio and `has_ended` triggers
     * at the same time as the audio EOF. */

    if (slot->duration > 0.0 && !slot->loop
        && slot->time >= slot->duration - 0.000001) {
        /* Three-way end gate:
         *   1. playback clock reached duration;
         *   2. audio stream (if any) drained — without this, ending
         *      truncates the trailing audio mid-sample;
         *   3. picture caught up — slot->frame_time reflects the real
         *      PTS of the last decoded frame (no longer lied to by
         *      decoder_seek). When user scrubs to near-end, the
         *      decoder needs several ticks to walk from a keyframe to
         *      duration; if we end the moment slot->time hits the cap,
         *      the picture freezes on the keyframe (visible as
         *      "scrub-to-end shows a stale frame then instantly
         *      ends"). Defer ending until frame_time catches up,
         *      OR until the catch-up grace period expires. */
        bool audio_done = !slot->audio_stream
                       || jce_audio_stream_eof(slot->audio_stream);
        bool video_caught_up =
            slot->frame_time + 0.250 >= slot->duration
            || slot->decoder.ended;
        if (audio_done && video_caught_up) {
            slot->ended = true;
        }
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

    slot->time = time_sec;
    slot->ended = false;
    slot->display_frame_time = time_sec;

    if (slot->queue_active) {
        worker_request_seek(slot, time_sec, exact);
        if (exact && slot->audio_stream) {
            jce_audio_stream_seek(slot->audio_stream, time_sec);
        }
        /* For exact seeks, wait briefly for worker to land on a frame
         * near the target so the user sees the requested frame on
         * release (UI mental model). Cap at 1.5s to avoid lockups on
         * very slow decoders. */
        if (exact) {
            const uint64_t budget_ms = 1500u;
            const uint64_t start_ms = jce_time_ticks_ms();
            while (true) {
                jce_mutex_lock(slot->qmtx);
                bool got = slot->q_count > 0
                    && qe_at(slot, slot->q_head)->serial == slot->seek_serial
                    && qe_at(slot, slot->q_head)->pts_sec + 0.05 >= time_sec;
                bool done = slot->worker_eof;
                jce_mutex_unlock(slot->qmtx);
                if (got || done) break;
                if ((jce_time_ticks_ms() - start_ms) >= budget_ms) break;
                jce_thread_sleep_ms(5);
            }
            /* Pop one frame so display_rgba reflects the new position. */
            ui_pop_for_target(slot, time_sec);
        }
        return;
    }

    /* Legacy fallback (no worker). */
    slot->post_seek = true;
    if (!decoder_seek(slot, time_sec, exact)) {
        slot->time  = time_sec;
        slot->ended = false;
    } else {
        slot->ended = false;
    }
    slot->post_seek = false;
    if (exact && !slot->metadata_only && slot->duration > 0.0) {
        const uint64_t budget_ms = 3000u;
        const uint64_t start_ms = jce_time_ticks_ms();
        while (slot->frame_time + 0.000001 < slot->time) {
            if (!decoder_read_next(slot)) {
                if (slot->decoder.ended) break;
            }
            if ((jce_time_ticks_ms() - start_ms) >= budget_ms) break;
        }
    }
    if (exact && slot->audio_stream) {
        jce_audio_stream_seek(slot->audio_stream, time_sec);
    }
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

    slot->time = 0.0;
    slot->ended = false;
    slot->display_frame_time = 0.0;

    if (slot->queue_active) {
        worker_request_seek(slot, 0.0, true);
        if (slot->audio_stream)
            jce_audio_stream_seek(slot->audio_stream, 0.0);
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
    /* Async path: return UI-side display buffer (filled by ui_pop_for_target).
     * Legacy fallback (metadata-only or worker-disabled): return decoder buffer. */
    if (slot->queue_active && slot->display_rgba) {
        if (out_w) *out_w = slot->display_rgba_w;
        if (out_h) *out_h = slot->display_rgba_h;
        if (out_frame_time) *out_frame_time = slot->display_frame_time;
        return slot->display_rgba;
    }
    if (out_w) *out_w = slot->rgba_w;
    if (out_h) *out_h = slot->rgba_h;
    if (out_frame_time) *out_frame_time = slot->frame_time;
    return slot->rgba;
}

uint64_t jce_video_get_frame_counter(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot) return 0u;
    if (slot->queue_active) return slot->display_frame_counter;
    return slot->frame_counter;
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

/* ── Streaming audio public API ─────────────────────────────────── */

bool jce_video_get_audio_format(JceVideo v,
                                 uint32_t *out_channels,
                                 uint32_t *out_samplerate,
                                 double   *out_duration_sec)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot || slot->audio_status != JCE_VIDEO_AUDIO_STATUS_READY)
        return false;
    if (slot->audio_pcm_channels == 0 || slot->audio_pcm_samplerate == 0)
        return false;
    if (out_channels)     *out_channels     = slot->audio_pcm_channels;
    if (out_samplerate)   *out_samplerate   = slot->audio_pcm_samplerate;
    if (out_duration_sec) *out_duration_sec = slot->duration;
    return true;
}

uint32_t jce_video_audio_pull(JceVideo v, int16_t *out, uint32_t frames)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot || !out || frames == 0) return 0;
    if (slot->audio_status != JCE_VIDEO_AUDIO_STATUS_READY) return 0;
    if (!slot->audio_stream) return 0;
    return jce_audio_stream_pull(slot->audio_stream, out, frames);
}

void jce_video_audio_seek(JceVideo v, double time_sec)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot || !slot->audio_stream) return;
    jce_audio_stream_seek(slot->audio_stream, time_sec);
}

double jce_video_audio_get_time(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot || !slot->audio_stream) return 0.0;
    return jce_audio_stream_get_time(slot->audio_stream);
}

bool jce_video_audio_eof(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot || !slot->audio_stream) return true;
    return jce_audio_stream_eof(slot->audio_stream);
}

bool jce_video_get_perf_stats(JceVideo v, JceVideoPerfStats *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    VideoSlot *slot = slot_from_handle(v);
    if (!slot) return false;
    /* Race-tolerant snapshot: take qmtx for q_count, copy scalars after.
     * Stale reads are acceptable for a debug overlay. */
    if (slot->qmtx) {
        jce_mutex_lock(slot->qmtx);
        out->q_count = slot->q_count;
        jce_mutex_unlock(slot->qmtx);
    }
    out->q_capacity        = JCE_VIDEO_Q_CAP;
    out->decode_us_ema     = slot->perf_decode_us_ema;
    out->decode_us_last    = slot->perf_decode_us_last;
    out->convert_us_ema    = slot->perf_convert_us_ema;
    out->convert_us_last   = slot->perf_convert_us_last;
    out->push_us_ema       = slot->perf_push_us_ema;
    out->push_us_last      = slot->perf_push_us_last;
    out->pop_us_ema        = slot->perf_pop_us_ema;
    out->pop_us_last       = slot->perf_pop_us_last;
    out->frames_decoded    = slot->perf_frames_decoded;
    out->frames_displayed  = slot->perf_frames_displayed;
    out->frames_dropped    = slot->perf_frames_dropped;
    out->worker_running    = slot->worker_running;
    out->worker_eof        = slot->worker_eof;
    out->queue_yuv_mode    = slot->queue_yuv_mode;
    return true;
}

} /* extern "C" */
