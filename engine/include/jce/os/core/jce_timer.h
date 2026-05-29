/*
 * jce_timer.h  High-precision frame timer with fixed timestep support.
 *
 * Uses SDL_GetPerformanceCounter for sub-millisecond accuracy.
 * Provides delta time, fixed-step accumulator, and interpolation alpha.
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

/* Fixed timestep: returns true while the accumulator has >= one fixed step.
   Call in a while loop for fixed updates:
     while (jce_timer_consume_fixed(timer))
         physics_update(fixed_dt);
   The fixed dt is the value passed to jce_timer_create(). */
JCE_API bool JCE_CALL jce_timer_consume_fixed(JceTimer *t);

/* Get the fixed timestep (seconds). */
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

/* High-resolution monotonic counter (wraps SDL_GetPerformanceCounter).
   Uniform implementation across Windows / macOS / Linux / Android /
   iOS / Web — prefer this over std::chrono::steady_clock or platform
   clocks for consistent behavior.                                    */
JCE_API uint64_t JCE_CALL jce_time_perf_counter(void);

/* Ticks of jce_time_perf_counter() per second (cached). */
JCE_API uint64_t JCE_CALL jce_time_perf_freq(void);

/* Milliseconds since some unspecified monotonic origin
   (wraps SDL_GetTicks).  Suitable for coarse deadlines and timestamps. */
JCE_API uint64_t JCE_CALL jce_time_ticks_ms(void);

/* Nanoseconds since some unspecified monotonic origin
   (wraps SDL_GetTicksNS). */
JCE_API uint64_t JCE_CALL jce_time_ticks_ns(void);

/* Wall-clock seconds since the Unix epoch (UTC).  Wraps the platform
   real-time clock; centralized so callers do not include <time.h>. */
JCE_API int64_t JCE_CALL jce_time_now_epoch_seconds(void);

/* Convert two perf-counter samples to elapsed seconds. */
JCE_API double JCE_CALL jce_time_perf_to_seconds(uint64_t start, uint64_t end);

/* Convert two perf-counter samples to elapsed milliseconds. */
JCE_API double JCE_CALL jce_time_perf_to_ms(uint64_t start, uint64_t end);

JCE_EXTERN_C_END

#endif /* JCE_TIMER_H */
