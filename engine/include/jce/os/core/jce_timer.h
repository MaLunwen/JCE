/*
 * jce_timer.h  High-precision frame timer.
 *
 * Uses the shared high-resolution monotonic clock. Actual resolution is
 * platform dependent; consecutive readings may be equal.
 * Provides delta time, elapsed time, smoothed FPS and an interpolation
 * alpha.  The engine's fixed-step simulation cadence is owned by
 * JceFixedClock (jce_fixed_clock.h) — this timer does not drive it.
 */

#ifndef JCE_TIMER_H
#define JCE_TIMER_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceTimer JceTimer;

/* Create a timer with the given fixed timestep (seconds).
   Typical values: 1.0/60.0 for 60 Hz, 1.0/120.0 for 120 Hz.
   Pass 0.0 for variable timestep (no fixed-step accumulation). */
JCE_API JceTimer *jce_timer_create(double fixed_dt);

/* Destroy a timer. */
JCE_API void JCE_CALL jce_timer_destroy(JceTimer *t);

/* Call once per frame at the top of the main loop.
   Updates internal counters and accumulator. */
JCE_API void JCE_CALL jce_timer_tick(JceTimer *t);

/* Get the raw frame delta time (seconds). */
JCE_API double JCE_CALL jce_timer_dt(const JceTimer *t);

/* Get frame delta time in milliseconds (convenience). */
JCE_API float JCE_CALL jce_timer_dt_ms(const JceTimer *t);

/* Get the fixed timestep (seconds) passed to jce_timer_create(). */
JCE_API double JCE_CALL jce_timer_fixed_dt(const JceTimer *t);

/* Get interpolation alpha (0..1) for rendering between fixed steps.
   alpha = remaining_accumulator / fixed_dt. */
JCE_API float JCE_CALL jce_timer_alpha(const JceTimer *t);

/* Get total elapsed time since timer creation (seconds). */
JCE_API double JCE_CALL jce_timer_elapsed(const JceTimer *t);

/* Get the current smoothed FPS (exponential moving average). */
JCE_API float JCE_CALL jce_timer_fps(const JceTimer *t);

/* ================================================================== */
/* Local-time formatting                                               */
/* ================================================================== */

/* Format `epoch_seconds` as local time using a strftime-style format
   string.  Writes a NUL-terminated string into `out` of capacity
   `out_size`.  Returns the number of characters written (excluding NUL),
   or 0 on failure (out is set to "" on failure when out_size > 0).
   The platform-specific reentrant call (localtime_r vs localtime_s)
   is hidden inside the engine. */
JCE_API size_t JCE_CALL jce_time_format_local(int64_t epoch_seconds,
                             const char *fmt,
                             char *out,
                             size_t out_size);

/* Same as jce_time_format_local but formats UTC (gmtime_r/gmtime_s).
   Centralizes the reentrant-call portability fork. */
JCE_API size_t JCE_CALL jce_time_format_utc(int64_t epoch_seconds,
                             const char *fmt,
                             char *out,
                             size_t out_size);

/* ================================================================== */
/* Cross-platform clock primitives                                     */
/* ================================================================== */

/* High-resolution monotonic counter with an unspecified origin. Native
   hosts use SDL performance time; Web uses Emscripten performance time.
   Counter units and actual resolution differ: browser privacy controls
   may coarsen readings, and consecutive readings may be equal. Use the
   conversion helpers instead of assuming a particular counter unit. */
JCE_API uint64_t JCE_CALL jce_time_perf_counter(void);

/* Ticks of jce_time_perf_counter() per second; stable for the provider. */
JCE_API uint64_t JCE_CALL jce_time_perf_freq(void);

/* Milliseconds since an unspecified monotonic origin. Suitable for coarse
   deadlines and timestamps. Native hosts retain SDL ticks; Web derives
   both tick APIs from the shared performance clock. */
JCE_API uint64_t JCE_CALL jce_time_ticks_ms(void);

/* Nanoseconds since an unspecified monotonic origin. Units do not imply
   nanosecond resolution; consecutive readings may be equal. */
JCE_API uint64_t JCE_CALL jce_time_ticks_ns(void);

/* Wall-clock seconds since the Unix epoch (UTC).  Wraps the platform
   real-time clock; centralized so callers do not include <time.h>. */
JCE_API int64_t JCE_CALL jce_time_now_epoch_seconds(void);

/* Realtime UTC milliseconds since the Unix epoch; may adjust with the system
 * clock. Returns 0 if unavailable. Use the perf counter for elapsed durations. */
JCE_API int64_t JCE_CALL jce_time_now_epoch_milliseconds(void);

/* Convert two perf-counter samples to elapsed seconds. */
JCE_API double JCE_CALL jce_time_perf_to_seconds(uint64_t start, uint64_t end);

/* Convert two perf-counter samples to elapsed milliseconds. */
JCE_API double JCE_CALL jce_time_perf_to_ms(uint64_t start, uint64_t end);

JCE_EXTERN_C_END

#endif /* JCE_TIMER_H */
