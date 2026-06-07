/*
 * jce_fixed_clock.h  Deterministic fixed-step accumulator (P3-B.2).
 *
 * Unity-parity: equivalent to Time.fixedDeltaTime / Time.fixedTime.
 * Drives the JCE_PHASE_FIXED_UPDATE dispatch in jce_engine_iterate so
 * physics and (future) network simulation run on a stable timeline that
 * is decoupled from the renderer's variable frame rate.
 *
 * Algorithm (Glenn Fiedler "Fix Your Timestep"):
 *   1. Each frame, add the (clamped) frame dt to an accumulator.
 *   2. While accumulator >= fixed_dt: run one tick, subtract fixed_dt,
 *      increment tick_count and fixed_time.
 *   3. The remainder / fixed_dt is the renderer interpolation alpha.
 *
 * Spiral-of-death guard: frame_dt is clamped to `max_frame_dt`
 * (default 0.25 s) inside jce_fixed_clock_advance so a long stall (load
 * spike, breakpoint, OS swap) cannot enqueue an unbounded burst of
 * fixed ticks.  A rate-limited warning is logged when clamping fires.
 *
 * Determinism notes (caller responsibility):
 *   - The clock guarantees a stable cadence (N * fixed_dt) but NOT
 *     bit-identical simulation across machines.  Callers that need
 *     cross-platform position equivalence (replays, lockstep netcode,
 *     P3-D rollback) must use deterministic math themselves: no libm
 *     identities that differ by vendor (sinf/cosf), no float reduction
 *     ordering races, no RNG that depends on wall-clock time.
 *   - This module paves the way for P3-D networking replay/rollback by
 *     exposing `tick_count` as the canonical simulation timeline index.
 *
 * Threading: main thread only in v1.  Phase dispatch (and therefore
 * tick) is single-threaded; do not call advance/tick concurrently.
 *
 * Layer: L2 (os/core).  Consumed via <jce/api_core.h>.
 */

#ifndef JCE_FIXED_CLOCK_H
#define JCE_FIXED_CLOCK_H

#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Public state; layout is part of the API for cheap embedding and
 * profiler/inspector readouts.  Treat fields as read-only outside the
 * jce_fixed_clock_* functions. */
typedef struct JceFixedClock {
    double   accumulator;   /* unconsumed time, seconds (always < fixed_dt
                             * after a successful advance loop)        */
    double   fixed_dt;      /* simulation step, seconds (e.g. 1.0/60.0) */
    double   max_frame_dt;  /* spiral-of-death cap, seconds (e.g. 0.25) */
    uint64_t tick_count;    /* monotonic count of executed fixed ticks  */
    double   fixed_time;    /* tick_count * fixed_dt (sim wall clock)   */
    double   alpha;         /* [0,1] = accumulator / fixed_dt after the
                             * last advance — renderer interp factor   */
} JceFixedClock;

/* Initialise in place.  `fixed_dt` <= 0 falls back to 1.0/60.0 (the
 * engine-wide default that matches the physics step).  `max_frame_dt`
 * <= 0 falls back to 0.25 s.  Resets all counters to zero. */
JCE_API void JCE_CALL
jce_fixed_clock_init(JceFixedClock *c, double fixed_dt, double max_frame_dt);

/* Add `frame_dt` (seconds) to the accumulator and return how many
 * fixed ticks the caller must execute this frame.  Updates `alpha`
 * as a side effect.  The caller MUST call jce_fixed_clock_tick()
 * exactly that many times — the function does not advance tick_count
 * itself so callers may interleave per-tick work between bumps. */
JCE_API uint32_t JCE_CALL
jce_fixed_clock_advance(JceFixedClock *c, double frame_dt);

/* Advance tick_count and fixed_time by exactly one fixed step.  Call
 * this once per fixed-update execution after jce_fixed_clock_advance
 * reports a non-zero step count.  Cheap; pure bookkeeping. */
JCE_API void JCE_CALL
jce_fixed_clock_tick(JceFixedClock *c);

/* Convenience accessor: same value as the `alpha` field, clamped to
 * [0, 1].  Renderers blend the previous and current simulated state
 * with this factor to hide the fixed/render rate mismatch. */
JCE_API double JCE_CALL
jce_fixed_clock_alpha(const JceFixedClock *c);

/* Process-global fixed clock used by jce_engine_iterate to drive
 * JCE_PHASE_FIXED_UPDATE.  This is the SINGLE source of truth for the
 * fixed cadence: the per-runtime physics clock (rt->clock) adopts this
 * clock's fixed_dt every step so the two cannot desync, and the
 * net-transform tick conversion reads it too.  Lazily initialised on
 * first call with the engine-default 60 Hz / 0.25 s spiral cap.
 * Configure cadence via jce_engine_set_fixed_hz() (or, for a specific
 * runtime, JceRuntimeDesc.fixed_timestep, which also retunes this clock)
 * rather than mutating the struct directly. */
JCE_API JceFixedClock * JCE_CALL
jce_fixed_clock_default(void);

JCE_EXTERN_C_END

#endif /* JCE_FIXED_CLOCK_H */
