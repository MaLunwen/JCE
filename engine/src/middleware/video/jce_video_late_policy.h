#ifndef JCE_VIDEO_LATE_POLICY_H
#define JCE_VIDEO_LATE_POLICY_H
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <jce/os/core/jce_log.h>

typedef struct JceVideoPresentationTarget {
    double seconds;
    uint64_t serial;
    uint64_t skipped;
    bool active;
    uint32_t consecutive;
} JceVideoPresentationTarget;

/* Decode references normally; omit conversion only for obsolete presentation.
 * Unknown duration cannot certify the terminal picture, so remains untouched. */
static inline bool jce_video_presentation_obsolete(
    const JceVideoPresentationTarget *target, uint64_t serial,
    double pts, double interval, double duration, bool seek_pending)
{
    return target && target->active && target->serial == serial && !seek_pending
        && isfinite(target->seconds) && isfinite(pts) && isfinite(interval)
        && isfinite(duration) && interval > 0.0 && duration > 0.0
        && pts >= 0.0 && pts + interval < duration
        && pts + interval < target->seconds;
}
/* Bound skipped presentation even when reference decoding cannot catch up. */
static inline bool jce_video_presentation_skip(JceVideoPresentationTarget *t,
    uint64_t serial, double pts, double interval, double duration, bool pending)
{
    if (!t) return false;
    if (!jce_video_presentation_obsolete(t, serial, pts, interval, duration, pending)
        || t->consecutive >= 2u) {
        t->consecutive = 0u;
        return false;
    }
    ++t->consecutive;
    ++t->skipped;
    return true;
}

/* Separate from consumer-drop counters and seek-trace record schema. */
static inline void jce_video_presentation_trace(const JceVideoPresentationTarget *t,
    bool enabled, double pts, const char *tag, const char *file, int line)
{
    if (!enabled || (t->skipped != 1 && t->skipped % 120 != 0)) return;
    jce_log_write(JCE_LOG_LEVEL_INFO, tag, file, line,
        "presentation-skip generation=%llu skipped_conversion=%llu pts=%.6f target=%.6f",
        (unsigned long long)t->serial, (unsigned long long)t->skipped, pts, t->seconds);
}
#endif
