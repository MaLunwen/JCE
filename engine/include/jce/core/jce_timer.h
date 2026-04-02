/*
 * jce_timer.h  High-precision frame timer with fixed timestep support.
 *
 * Uses SDL_GetPerformanceCounter for sub-millisecond accuracy.
 * Provides delta time, fixed-step accumulator, and interpolation alpha.
 */

#ifndef JCE_TIMER_H
#define JCE_TIMER_H

#include <stdint.h>
#include <stdbool.h>

typedef struct JceTimer JceTimer;

/* Create a timer with the given fixed timestep (seconds).
   Typical values: 1.0/60.0 for 60 Hz, 1.0/120.0 for 120 Hz.
   Pass 0.0 for variable timestep (no fixed-step accumulation). */
JceTimer *jce_timer_create(double fixed_dt);

/* Destroy a timer. */
void jce_timer_destroy(JceTimer *t);

/* Call once per frame at the top of the main loop.
   Updates internal counters and accumulator. */
void jce_timer_tick(JceTimer *t);

/* Get the raw frame delta time (seconds). */
double jce_timer_dt(const JceTimer *t);

/* Get frame delta time in milliseconds (convenience). */
float jce_timer_dt_ms(const JceTimer *t);

/* Fixed timestep: returns true while the accumulator has >= one fixed step.
   Call in a while loop for fixed updates:
     while (jce_timer_consume_fixed(timer))
         physics_update(fixed_dt);
   The fixed dt is the value passed to jce_timer_create(). */
bool jce_timer_consume_fixed(JceTimer *t);

/* Get the fixed timestep (seconds). */
double jce_timer_fixed_dt(const JceTimer *t);

/* Get interpolation alpha (0..1) for rendering between fixed steps.
   alpha = remaining_accumulator / fixed_dt. */
float jce_timer_alpha(const JceTimer *t);

/* Get total elapsed time since timer creation (seconds). */
double jce_timer_elapsed(const JceTimer *t);

/* Get the current smoothed FPS (exponential moving average). */
float jce_timer_fps(const JceTimer *t);

#endif /* JCE_TIMER_H */
