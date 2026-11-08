/* Private video slot/decoder/queue storage. Declarations only; all ownership
 * transitions remain in the existing jce_video.cpp bridge. */
#ifndef JCE_VIDEO_SLOT_INTERNAL_H
#define JCE_VIDEO_SLOT_INTERNAL_H
#include <jce/middleware/video/jce_av1_decode.h>
#include <jce/middleware/video/jce_mp4_parser.h>
#include <jce/middleware/video/jce_video.h>
#include <jce/middleware/video/jce_vp8_decode.h>
#include <jce/middleware/video/jce_vp9_decode.h>
#include <jce/middleware/video/jce_webm_parser.h>
#include "jce_h264_decode.h"
#include "jce_h265_decode.h"
#include "jce_audio_stream.h"
#include "jce_video_frame.h"
#include "jce_video_color.h"
#include "jce_video_seek_trace.h"
#include "jce_video_late_policy.h"
#include "jce_video_lifecycle_trace.h"

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
    uint8_t             *packet; /* bounded encoded sample staging */
    size_t               packet_capacity;

    /* AV1 / IVF: dav1d decoder owns these (must outlive decoder). */
    bool                 ivf;
    uint32_t             av1_fps_num;   /* IVF timebase numerator */
    uint32_t             av1_fps_den;   /* IVF timebase denominator */
    uint32_t             av1_frame_idx; /* index of next decoded AV1 frame */

    /* WebM / Matroska: container demuxer + VP8 or AV1 video decoder.
     * (AV1-in-WebM uses slot->decoder.av1 with packet-driven mode.)
     * Audio is not currently routed through the worker (silent track). */
    JceWebmParser       *webm;
    JceVp8Decoder       *vp8;
    JceVp9Decoder       *vp9;

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

    /* Legacy PCM query fields. Streaming paths expose format/count only;
     * no full-clip PCM allocation is retained. */
    int16_t *audio_pcm;
    uint32_t audio_pcm_frames;
    uint32_t audio_pcm_channels;
    uint32_t audio_pcm_samplerate;

    /* Streaming audio source. Lazily created on first pull or, for the
     * WebM/Opus path, upfront at load time. Owned. */
    JceAudioStream *audio_stream;

    /* Opt-in AV1 receive/input/send stage timing. */
    uint64_t av1_trace_receive, av1_trace_read, av1_trace_send;

    JceReadSource *source; /* shared immutable memory or host-file input */
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
    uint64_t      trace_owner;
    JceVideoLifecycleSeek trace_pending_seek; /* qmtx-protected owner request */
    JceVideoLifecycleSeek trace_active_seek;  /* worker-owned terminal */

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
    JceYuvColor    pending_yuv_color;

    uint8_t *display_rgba;
    int      display_rgba_w;
    int      display_rgba_h;
    size_t   display_rgba_capacity;
    double   display_frame_time;
    uint64_t display_frame_counter;
    bool     seek_display_pending;
    double   seek_display_target;

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
    JceYuvPreview preview; /* UI fallback for frames queued before configuration */
    JceVideoFrameProcessor frame_processor; /* worker-owned */
    JceVideoFrameProcessor display_processor; /* owner-only, borrows display RGBA */
    uint32_t worker_preview_limit; /* protected by qmtx */
    JceVideoPresentationTarget presentation; /* protected by qmtx */
    bool preview_supported; /* fixed before worker start; safe during seek */
    JceVideoSeekTrace seek_trace; /* worker-owned, opt-in diagnostic only */

    void *worker_thread;               /* JceThread* */

    /* ── Perf probes (S1) ─────────────────────────────────────────
     * EMA-smoothed per-frame timings in microseconds. Updated by
     * worker thread (decode/convert/push) and UI thread (pop).
     * Read race-free via jce_video_get_perf_stats — we accept stale
     * reads since this is debug-only. */
    double   perf_decode_us_ema;
    double   perf_convert_us_ema;     /* UI-side for queued YUV */
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

typedef struct VideoQueueEntry {
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
    JceYuvColor color;       /* travels with the same frame and seek serial */
    int      w;
    int      h;
    double   pts_sec;
    uint64_t serial;
} VideoQueueEntry;

#endif
