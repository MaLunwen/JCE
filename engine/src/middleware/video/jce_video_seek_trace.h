#ifndef JCE_VIDEO_SEEK_TRACE_H
#define JCE_VIDEO_SEEK_TRACE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_timer.h>

typedef struct JceVideoSeekTrace {
    bool enabled, active, keyframe_known, reused, exact;
    uint64_t generation, started_ms, index_ms, flush_ms, decode_ms;
    uint64_t pictures, preroll_pictures;
    double requested_pts, keyframe_pts;
} JceVideoSeekTrace;

static inline void jce_video_seek_trace_begin(JceVideoSeekTrace *trace,
                                             uint64_t generation,
                                             double requested, uint64_t now)
{
    if (!trace || !trace->enabled) return;
    memset(trace, 0, sizeof(*trace));
    trace->enabled = trace->active = true;
    trace->generation = generation;
    trace->requested_pts = requested;
    trace->started_ms = now;
}

static inline void jce_video_seek_trace_picture(JceVideoSeekTrace *trace,
                                               double pts)
{
    if (!trace || !trace->active) return;
    trace->pictures++;
    if (pts + 0.0005 < trace->requested_pts) trace->preroll_pictures++;
}

static inline bool jce_video_seek_trace_current(const JceVideoSeekTrace *trace,
                                               uint64_t generation, bool pending,
                                               bool quitting)
{
    return trace && trace->active && trace->generation == generation
        && !pending && !quitting;
}

static inline uint64_t jce_video_seek_trace_elapsed(uint64_t begin, uint64_t end)
{
    return end >= begin ? end - begin : 0;
}

/* Calls the clock only for an active opt-in observation. */
static inline void jce_video_seek_trace_stage(JceVideoSeekTrace *trace,
    uint64_t *total, uint64_t *begin, bool restart)
{
    if (!trace->active) return;
    *total += jce_video_seek_trace_elapsed(*begin, jce_time_ticks_ms());
    if (restart) *begin = jce_time_ticks_ms();
}

static inline void jce_video_seek_trace_report(JceVideoSeekTrace *trace,
    const char *event, uint64_t now, double picture_pts,
    const char *tag, const char *file, int line)
{
    if (!trace || !trace->active) return;
    jce_log_write(JCE_LOG_LEVEL_INFO, tag, file, line,
        "seek-trace event=%s generation=%llu request_pts=%.6f exact=%d keyframe_known=%d "
        "keyframe_pts=%.6f reused=%d pictures=%llu preroll_pictures=%llu "
        "index_ms=%llu flush_ms=%llu decode_ms=%llu worker_elapsed_ms=%llu picture_pts=%.6f",
        event, (unsigned long long)trace->generation, trace->requested_pts,
        trace->exact ? 1 : 0, trace->keyframe_known ? 1 : 0, trace->keyframe_pts, trace->reused ? 1 : 0,
        (unsigned long long)trace->pictures, (unsigned long long)trace->preroll_pictures,
        (unsigned long long)trace->index_ms, (unsigned long long)trace->flush_ms,
        (unsigned long long)trace->decode_ms,
        (unsigned long long)jce_video_seek_trace_elapsed(trace->started_ms, now),
        picture_pts);
    trace->active = false;
}

#endif
