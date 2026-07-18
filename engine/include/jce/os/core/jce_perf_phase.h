/*
 * jce_perf_phase.h  Lightweight per-frame CPU-phase accumulator.
 *
 * Named phase buckets accumulate ms over a reporting window (e.g. 120 frames).
 * Call jce_perf_phase_report() to get a sorted string and reset all buckets.
 * All calls are no-ops when disabled (jce_perf_phase_set_enabled(0)).
 *
 * Main-thread only — not thread-safe.
 */

#ifndef JCE_PERF_PHASE_H
#define JCE_PERF_PHASE_H

#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

/* Accumulate `ms` into the named slot (first-seen name registers the slot).
 * Up to 24 distinct names; extras are silently dropped.
 * `name` must be a long-lived pointer (string literal is fine). */
JCE_API void jce_perf_phase_add(const char *name, double ms);

/* Format all slots as "name=ms name=ms ..." sorted descending by ms,
 * then RESET all accumulators to 0.  Name registrations persist. */
JCE_API void jce_perf_phase_report(char *out, int out_sz);

/* Gate: when off, jce_perf_phase_add is a no-op (zero overhead). */
JCE_API void jce_perf_phase_set_enabled(int on);
JCE_API int  jce_perf_phase_enabled(void);

/* Non-destructive access (editor profiler UI).  frame_tick() rotates the
 * window accumulators into a per-frame snapshot (delta since the previous
 * tick); peek_frame() reads that snapshot without touching the window that
 * jce_perf_phase_report() owns.  Call frame_tick once per frame. */
JCE_API void jce_perf_phase_frame_tick(void);
JCE_API int  jce_perf_phase_count(void);
JCE_API int  jce_perf_phase_peek_frame(int idx, const char **out_name,
                                       double *out_ms);

JCE_EXTERN_C_END

#endif /* JCE_PERF_PHASE_H */
