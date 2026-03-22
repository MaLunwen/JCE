/*
 * jce_timer.c  High-precision frame timer implementation.
 *
 * Uses SDL_GetPerformanceCounter / SDL_GetPerformanceFrequency
 * for cross-platform sub-millisecond accuracy.
 */

#include "jce_timer.h"
#include <SDL3/SDL.h>

/* Clamp frame delta to avoid spiral of death after breakpoints / sleep. */
#define MAX_FRAME_DT 0.25  /* 250 ms (4 FPS minimum) */

struct JceTimer {
    uint64_t freq;          /* SDL performance counter frequency */
    uint64_t last_counter;  /* counter value at previous tick */
    uint64_t start_counter; /* counter value at creation */

    double   dt;            /* raw frame delta (seconds) */
    double   elapsed;       /* total elapsed time (seconds) */

    /* Fixed timestep state. */
    double   fixed_dt;      /* target fixed step (0 = variable) */
    double   accumulator;   /* unconsumed time (seconds) */

    /* FPS smoothing. */
    float    fps_smoothed;
};

JceTimer *jce_timer_create(double fixed_dt)
{
    JceTimer *t = (JceTimer *)SDL_calloc(1, sizeof(*t));
    if (!t) return NULL;

    t->freq          = SDL_GetPerformanceFrequency();
    t->last_counter  = SDL_GetPerformanceCounter();
    t->start_counter = t->last_counter;
    t->fixed_dt      = fixed_dt > 0.0 ? fixed_dt : 0.0;
    t->fps_smoothed  = fixed_dt > 0.0 ? (float)(1.0 / fixed_dt) : 60.0f;

    return t;
}

void jce_timer_destroy(JceTimer *t)
{
    SDL_free(t);
}

void jce_timer_tick(JceTimer *t)
{
    if (!t) return;

    uint64_t now = SDL_GetPerformanceCounter();
    double raw_dt = (double)(now - t->last_counter) / (double)t->freq;
    t->last_counter = now;

    /* Clamp to avoid spiral of death. */
    if (raw_dt > MAX_FRAME_DT)
        raw_dt = MAX_FRAME_DT;

    t->dt = raw_dt;
    t->elapsed = (double)(now - t->start_counter) / (double)t->freq;

    /* Accumulate for fixed timestep. */
    if (t->fixed_dt > 0.0)
        t->accumulator += raw_dt;

    /* Smooth FPS (EMA). */
    if (raw_dt > 0.0) {
        float instant_fps = (float)(1.0 / raw_dt);
        t->fps_smoothed = t->fps_smoothed * 0.95f + instant_fps * 0.05f;
    }
}

double jce_timer_dt(const JceTimer *t)
{
    return t ? t->dt : 0.0;
}

float jce_timer_dt_ms(const JceTimer *t)
{
    return t ? (float)(t->dt * 1000.0) : 0.0f;
}

bool jce_timer_consume_fixed(JceTimer *t)
{
    if (!t || t->fixed_dt <= 0.0) return false;
    if (t->accumulator >= t->fixed_dt) {
        t->accumulator -= t->fixed_dt;
        return true;
    }
    return false;
}

double jce_timer_fixed_dt(const JceTimer *t)
{
    return t ? t->fixed_dt : 0.0;
}

float jce_timer_alpha(const JceTimer *t)
{
    if (!t || t->fixed_dt <= 0.0) return 1.0f;
    float a = (float)(t->accumulator / t->fixed_dt);
    if (a > 1.0f) a = 1.0f;
    return a;
}

double jce_timer_elapsed(const JceTimer *t)
{
    return t ? t->elapsed : 0.0;
}

float jce_timer_fps(const JceTimer *t)
{
    return t ? t->fps_smoothed : 0.0f;
}
