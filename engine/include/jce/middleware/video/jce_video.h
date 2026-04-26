/*
 * jce_video.h  Cross-platform video metadata/playback handle API.
 *
 * Backend always parses MP4/ISO-BMFF metadata in-engine (no external
 * codec library). On platforms with a native decode backend available,
 * frame decode is enabled; otherwise playback falls back to metadata-only.
 *
 * Typical usage:
 *
 *     // load from a full in-memory asset
 *     JceVideo v = jce_video_load_memory(bytes, size, "intro.mp4");
 *
 *     // query metadata
 *     JceVideoInfo info;
 *     jce_video_get_info(v, &info);
 *
 *     // transport/timeline
 *     jce_video_advance(v, delta_seconds);
 *     jce_video_seek(v, 12.5, true);
 *
 *     // cleanup
 *     jce_video_unload(v);
 */

#ifndef JCE_VIDEO_H
#define JCE_VIDEO_H


#include <jce/middleware/video/jce_video_types.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Per-clip metadata snapshot. */
typedef struct {
    int    width;
    int    height;
    double duration;       /* seconds, 0 if unknown */
    double framerate;      /* frames per second */
    bool   has_audio;
    int    samplerate;     /* 0 if no audio */
    int    audio_channels; /* 0 if no audio */
    bool   metadata_only;  /* true when parsed but no decode backend is active */
    char   video_codec[5]; /* fourcc (e.g. avc1, hvc1, av01) */
    char   audio_codec[5]; /* fourcc (e.g. mp4a) */
} JceVideoInfo;

typedef enum {
    JCE_VIDEO_AUDIO_STATUS_NONE = 0,
    JCE_VIDEO_AUDIO_STATUS_READY,
    JCE_VIDEO_AUDIO_STATUS_DECODING,
    JCE_VIDEO_AUDIO_STATUS_UNSUPPORTED_CODEC,
    JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
    JCE_VIDEO_AUDIO_STATUS_DEMUX_ERROR,
} JceVideoAudioStatus;

/* -- Lifecycle ----------------------------------------------------- */

/* Load a video clip from raw file bytes in memory (MP4/ISO-BMFF).
 * The module copies the bytes internally; caller retains ownership of
 * the input buffer.  Returns JCE_VIDEO_INVALID on failure. */
JceVideo jce_video_load_memory(const void *data, uint32_t size,
                                const char *hint_path);

/* Release a clip.  Safe to call with JCE_VIDEO_INVALID. */
void     jce_video_unload(JceVideo v);

/* -- Queries ------------------------------------------------------- */

bool     jce_video_get_info(JceVideo v, JceVideoInfo *out);
double   jce_video_get_time(JceVideo v);
double   jce_video_get_duration(JceVideo v);
bool     jce_video_has_ended(JceVideo v);

/* Width / height reported by parsed stream metadata. */
void     jce_video_get_size(JceVideo v, int *out_w, int *out_h);

/* -- Transport ----------------------------------------------------- */

/* Advance the internal clock by dt seconds and decode any frames that
 * fall within that window.  When paused, pass dt == 0 or skip the call. */
void     jce_video_advance(JceVideo v, double dt_seconds);

/* Seek to an absolute time.  If exact is false this snaps to the
 * nearest preceding intra-frame (fast).  If true, performs an exact
 * seek (may decode many frames to land on the target). */
void     jce_video_seek(JceVideo v, double time_sec, bool exact);

/* Reset to the beginning of the clip. */
void     jce_video_rewind(JceVideo v);

/* Enable / disable looped playback (default: off). */
void     jce_video_set_loop(JceVideo v, bool loop);

/* -- Frame access -------------------------------------------------- */

/* Returns a pointer to the most recently decoded frame in packed RGBA8
 * (top-left origin, stride = width * 4).
 *
 * If out_frame_time is non-NULL it receives the presentation time of
 * the returned frame in seconds. */
const uint8_t *jce_video_get_frame_rgba(JceVideo v,
                                         int *out_w, int *out_h,
                                         double *out_frame_time);

/* Monotonic frame counter incremented when a new frame is decoded.
 * Metadata-only mode returns 0. */
uint64_t jce_video_get_frame_counter(JceVideo v);

/* -- Audio status -------------------------------------------------- */

/* Returns high-level embedded-audio availability for the clip. */
JceVideoAudioStatus jce_video_get_audio_status(JceVideo v);

/* Human-readable reason for current audio status.
 * Returned pointer is owned by the video handle and remains valid
 * until jce_video_unload(). */
const char *jce_video_get_audio_status_text(JceVideo v);

/* Number of encoded samples in the selected MP4 audio track (0 if none/unknown). */
uint32_t jce_video_get_audio_sample_count(JceVideo v);

/* DEPRECATED: returns the full-clip PCM blob if one is currently
 * resident. With the streaming pipeline this returns NULL for codec
 * paths that have been migrated (e.g. WebM/Opus). New code should use
 * jce_video_audio_pull(). */
const int16_t *jce_video_get_audio_pcm(JceVideo v,
                                        uint32_t *out_frame_count,
                                        uint32_t *out_channels,
                                        uint32_t *out_samplerate);

/* -- Streaming audio (pull-based) --------------------------------- */

/* Returns format metadata for the embedded audio track, regardless of
 * whether playback has started. Returns false if no audio is available
 * (status != READY). */
bool jce_video_get_audio_format(JceVideo v,
                                 uint32_t *out_channels,
                                 uint32_t *out_samplerate,
                                 double   *out_duration_sec);

/* Pull up to `frames` interleaved s16 frames from the streaming
 * decoder. Returns the number written. May return less than requested
 * (under-run); callers that need continuous output should fill the
 * remainder with silence. Safe to call from the audio device thread. */
uint32_t jce_video_audio_pull(JceVideo v,
                               int16_t *out, uint32_t frames);

/* Seek the audio stream to `time_sec`. */
void jce_video_audio_seek(JceVideo v, double time_sec);

/* Audio playback time, in seconds, derived from frames pulled. */
double jce_video_audio_get_time(JceVideo v);

/* True iff the streaming source has reached EOF and the buffer is drained. */
bool jce_video_audio_eof(JceVideo v);

/* -- Performance probes (debug; S1) ------------------------------- */

/* EMA-smoothed per-frame timings (microseconds) and queue/frame
 * counters for the playback worker. Returns false if the handle is
 * invalid or playback has not been started (no worker). */
typedef struct {
    /* Worker thread (per decoded frame). */
    double   decode_us_ema;
    double   decode_us_last;
    double   convert_us_ema;     /* YUV→RGBA SIMD on the worker side */
    double   convert_us_last;
    double   push_us_ema;        /* enqueue under lock */
    double   push_us_last;
    /* UI thread (per advance). */
    double   pop_us_ema;
    double   pop_us_last;
    /* Frame queue snapshot. */
    int      q_count;
    int      q_capacity;
    /* Counters since slot init. */
    uint64_t frames_decoded;
    uint64_t frames_displayed;
    uint64_t frames_dropped;     /* late frames skipped during pop */
    /* Worker state. */
    bool     worker_running;
    bool     worker_eof;
    bool     queue_yuv_mode;     /* false: RGBA queue (current path) */
} JceVideoPerfStats;

bool jce_video_get_perf_stats(JceVideo v, JceVideoPerfStats *out);

JCE_EXTERN_C_END

#endif /* JCE_VIDEO_H */
