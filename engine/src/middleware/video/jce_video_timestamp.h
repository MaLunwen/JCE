#ifndef JCE_VIDEO_TIMESTAMP_H
#define JCE_VIDEO_TIMESTAMP_H
#include <stdbool.h>
#include <stdint.h>

static inline double jce_video_timestamp_interval(double fps)
{
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
static inline double jce_video_timestamp_normalize(
    uint64_t frame_counter, double frame_time, double framerate, bool post_seek,
    bool *base_set, double *base_sec, double raw_time_sec, bool has_raw_time)
{
    if (frame_counter == 0u) {
        if (has_raw_time && raw_time_sec >= 0.0) {
            *base_set = true;
            *base_sec = raw_time_sec;
        } else {
            *base_set = false;
            *base_sec = 0.0;
        }
        return 0.0;
    }

    double t = frame_time + jce_video_timestamp_interval(framerate);
    if (has_raw_time && raw_time_sec >= 0.0) {
        if (!*base_set) {
            *base_set = true;
            *base_sec = raw_time_sec;
        }
        t = raw_time_sec - *base_sec;
        if (t < 0.0) {
            t = 0.0;
        }
        /* Only enforce monotonicity during normal playback, not after
         * a seek — backward seeks legitimately produce earlier timestamps. */
        if (!post_seek && t <= frame_time + 0.0005) {
            t = frame_time + jce_video_timestamp_interval(framerate);
        }
    }
    return t;
}

#endif
