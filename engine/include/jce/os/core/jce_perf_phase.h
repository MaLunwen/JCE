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

/* Number of distinct phase names that can be registered.  Public because
 * consumers size their own row arrays from it -- the editor's profiler panel
 * used a hardcoded 64 against this 128, and applied the cap while COLLECTING,
 * before sorting, so it dropped whichever phases happened to register last
 * rather than the smallest ones. */
#define JCE_PERF_PHASE_MAX_SLOTS 128

/* Accumulate `ms` into the named slot (first-seen name registers the slot).
 * Up to JCE_PERF_PHASE_MAX_SLOTS distinct names; extras warn once then drop.
 * `name` must be a long-lived pointer (string literal is fine). */
JCE_API void jce_perf_phase_add(const char *name, double ms);

/* Format all slots as "name=ms name=ms ..." sorted descending by ms,
 * then RESET all accumulators to 0.  Name registrations persist. */
JCE_API void jce_perf_phase_report(char *out, int out_sz);
/* Same reset/report operation, but emits average milliseconds per frame over
 * `frame_count`. This is the preferred periodic-log form. */
JCE_API void jce_perf_phase_report_average(char *out, int out_sz,
                                           uint32_t frame_count);

/* Gate: when off, jce_perf_phase_add is a no-op (zero overhead). */
JCE_API void jce_perf_phase_set_enabled(int on);
JCE_API int  jce_perf_phase_enabled(void);

/* Non-destructive access (editor profiler UI, engine hitch attribution).
 * frame_tick() rotates the window accumulators into a per-frame snapshot
 * (delta since the previous tick); peek_frame() reads that snapshot without
 * touching the window that jce_perf_phase_report() owns.  Called once per
 * frame by the engine's player loop; idempotent, so a host that also calls it
 * (the editor does) costs nothing and clobbers nothing.
 *
 * frame_has_data() distinguishes "the snapshot says every phase was cheap"
 * from "nothing ever populated the snapshot".  Those are not the same claim,
 * and for the whole life of the hitch reporter they were reported as one: the
 * tick was called only by the editor, so every standalone run attributed its
 * hitches to "outside every instrumented phase" -- an assertion about the
 * frame, made from an array of zeros. */
JCE_API void jce_perf_phase_frame_tick(void);
JCE_API int  jce_perf_phase_count(void);
JCE_API int  jce_perf_phase_peek_frame(int idx, const char **out_name,
                                       double *out_ms);
JCE_API int  jce_perf_phase_frame_has_data(void);

JCE_EXTERN_C_END

#endif /* JCE_PERF_PHASE_H */
