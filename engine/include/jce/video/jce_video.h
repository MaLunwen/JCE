/*
 * jce_video.h  Cross-platform MPEG-1 video via pl_mpeg.
 *
 * Decodes MPEG-1 video + MP2 audio from MPEG Program Stream containers
 * (.mpg, .mpeg, .m1v) entirely in-engine, without any external codec
 * dependency.  Mirrors the shape of jce_audio.h — create once per clip,
 * hold a small pool of slots, render frames by polling.
 *
 * Typical usage:
 *
 *     // load from a full in-memory asset
 *     JceVideo v = jce_video_load_memory(bytes, size, "intro.mpg");
 *
 *     // per frame
 *     jce_video_advance(v, delta_seconds);
 *     int w, h;
 *     double t;
 *     const uint8_t *rgba = jce_video_get_frame_rgba(v, &w, &h, &t);
 *     if (rgba) upload_to_gpu(rgba, w, h);
 *
 *     // transport
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
} JceVideoInfo;

/* -- Lifecycle ----------------------------------------------------- */

/* Load a video clip from raw file bytes in memory (.mpg / .mpeg / .m1v).
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

/* Width / height of the currently-decoded frame (0 if none yet). */
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

/* Returns a pointer to the most-recently-decoded frame in packed RGBA8
 * (top-left origin, stride = width * 4).  The pointer is valid until
 * the next jce_video_advance / jce_video_seek / jce_video_unload call.
 * Returns NULL if no frame has been decoded yet.
 *
 * If out_frame_time is non-NULL it receives the presentation time of
 * the returned frame in seconds. */
const uint8_t *jce_video_get_frame_rgba(JceVideo v,
                                         int *out_w, int *out_h,
                                         double *out_frame_time);

/* Monotonic counter that increments each time a new frame is written
 * to the RGBA buffer.  Callers can use this to skip uploading the same
 * frame repeatedly. */
uint64_t jce_video_get_frame_counter(JceVideo v);

#ifdef __cplusplus
}
#endif

#endif /* JCE_VIDEO_H */
