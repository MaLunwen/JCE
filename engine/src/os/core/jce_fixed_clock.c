/*
 * jce_fixed_clock.c  Deterministic fixed-step accumulator (P3-B.2).
 *
 * Glenn Fiedler "Fix Your Timestep" applied to the JCE PlayerLoop.
 * See jce_fixed_clock.h for the algorithm and determinism contract.
 */

#include <jce/os/core/jce_fixed_clock.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_timer.h>

#define LOG_TAG "fixed_clock"

#define JCE_FIXED_CLOCK_DEFAULT_DT       (1.0 / 50.0)  /* 50 Hz, Unity default */
#define JCE_FIXED_CLOCK_DEFAULT_MAX_DT   (0.25)        /* 250 ms spiral guard  */
/* Hard ceiling on ticks per advance call.  Without this a pathological
 * fixed_dt close to zero combined with the max_frame_dt clamp could
 * still hand back a huge step count.  Mirrors typical engine practice
 * (Bullet: 1000, Unity: 1/maximumDeltaTime). */
#define JCE_FIXED_CLOCK_MAX_STEPS_PER_ADVANCE  ((uint32_t)1000)

void jce_fixed_clock_init(JceFixedClock *c, double fixed_dt, double max_frame_dt)
{
    if (!c) return;
    c->fixed_dt     = (fixed_dt     > 0.0) ? fixed_dt     : JCE_FIXED_CLOCK_DEFAULT_DT;
    c->max_frame_dt = (max_frame_dt > 0.0) ? max_frame_dt : JCE_FIXED_CLOCK_DEFAULT_MAX_DT;
    c->accumulator  = 0.0;
    c->tick_count   = 0;
    c->fixed_time   = 0.0;
    c->alpha        = 0.0;
}

uint32_t jce_fixed_clock_advance(JceFixedClock *c, double frame_dt)
{
    if (!c || c->fixed_dt <= 0.0) return 0;

    /* Negative dt is nonsense (clock skew, paused frame) — coerce to 0. */
    if (frame_dt < 0.0) frame_dt = 0.0;

    /* Spiral-of-death guard.  Rate-limited warning so a long stall does
     * not flood the log ring (1 message / second of wall time). */
    if (frame_dt > c->max_frame_dt) {
        static uint64_t s_last_warn_ms = 0;
        const uint64_t now_ms = jce_time_ticks_ms();
        if (now_ms - s_last_warn_ms >= 1000u) {
            s_last_warn_ms = now_ms;
            LOG_WARN(LOG_TAG,
                "frame_dt=%.3fs exceeded max_frame_dt=%.3fs; clamping to avoid "
                "spiral of death (some simulated time will be lost)",
                frame_dt, c->max_frame_dt);
        }
        frame_dt = c->max_frame_dt;
    }

    c->accumulator += frame_dt;

    uint32_t steps = 0;
    while (c->accumulator >= c->fixed_dt &&
           steps < JCE_FIXED_CLOCK_MAX_STEPS_PER_ADVANCE) {
        c->accumulator -= c->fixed_dt;
        ++steps;
    }

    /* Defensive: drain the remainder if we hit the per-call ceiling so
     * the next frame starts clean instead of compounding. */
    if (steps >= JCE_FIXED_CLOCK_MAX_STEPS_PER_ADVANCE && c->accumulator > 0.0)
        c->accumulator = 0.0;

    c->alpha = (c->fixed_dt > 0.0) ? (c->accumulator / c->fixed_dt) : 0.0;
    if (c->alpha < 0.0) c->alpha = 0.0;
    if (c->alpha > 1.0) c->alpha = 1.0;

    return steps;
}

void jce_fixed_clock_tick(JceFixedClock *c)
{
    if (!c) return;
    c->tick_count += 1u;
    c->fixed_time += c->fixed_dt;
}

double jce_fixed_clock_alpha(const JceFixedClock *c)
{
    if (!c) return 0.0;
    double a = c->alpha;
    if (a < 0.0) a = 0.0;
    if (a > 1.0) a = 1.0;
    return a;
}

JceFixedClock *jce_fixed_clock_default(void)
{
    /* Plain process-global state; PlayerLoop dispatch is main-thread
     * only in v1 so no synchronisation is required.  Lazy initialise on
     * first access via the `initialised` flag. */
    static JceFixedClock s_clock;
    static int           s_initialised = 0;
    if (!s_initialised) {
        jce_fixed_clock_init(&s_clock,
                             JCE_FIXED_CLOCK_DEFAULT_DT,
                             JCE_FIXED_CLOCK_DEFAULT_MAX_DT);
        s_initialised = 1;
    }
    return &s_clock;
}
