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

#include <stdbool.h>
#include <stdint.h>

#include <jce/video/jce_video_types.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/* Access decoded audio PCM buffer (interleaved s16, full clip).
 * Returns NULL when no embedded audio was decoded.
 * Output parameters are set to 0 on failure. */
const int16_t *jce_video_get_audio_pcm(JceVideo v,
                                        uint32_t *out_frame_count,
                                        uint32_t *out_channels,
                                        uint32_t *out_samplerate);

#ifdef __cplusplus
}
#endif

#endif /* JCE_VIDEO_H */
