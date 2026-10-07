#ifndef JCE_VIDEO_PERF_H
#define JCE_VIDEO_PERF_H
#include <jce/os/core/jce_timer.h>
#include <stdbool.h>

/* Disabled diagnostics do not invoke the supplied clock at all. */
static inline uint64_t jce_video_perf_begin(bool enabled, uint64_t (*clock)(void))
{
    return enabled ? clock() : 0u;
}
static inline uint64_t jce_video_perf_frequency(uint64_t *frequency)
{
    if (*frequency == 0u) *frequency = jce_time_perf_freq();
    return *frequency;
}
static inline double jce_video_perf_us(uint64_t *frequency, uint64_t t0, uint64_t t1)
{
    if (t1 <= t0) return 0.0;
    return (double)(t1 - t0) * 1e6 / (double)jce_video_perf_frequency(frequency);
}
static inline void jce_video_perf_ema_update(double *ema, double *last, double sample)
{
    *last = sample;
    if (*ema <= 0.0) *ema = sample;
    else             *ema = (*ema) * 0.9 + sample * 0.1;
}

#endif
