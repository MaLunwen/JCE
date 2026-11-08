/*
 * jce_timer.c  High-precision frame timer implementation.
 *
 * Uses the shared JCE monotonic clock. Native hosts retain SDL; Web uses
 * Emscripten performance time without the SDL realtime fallback.
 */

#include <jce/os/core/jce_timer.h>

#include "jce_memory.h"

#include <SDL3/SDL.h>

#if JCE_PLATFORM_WEB
#include <emscripten/emscripten.h>
#endif

/* Clamp frame delta to avoid spiral of death after breakpoints / sleep.
   Same cap as JCE_FIXED_CLOCK_DEFAULT_MAX_DT in jce_fixed_clock.c. */
#define MAX_FRAME_DT 0.25  /* 250 ms (4 FPS minimum) */

struct JceTimer {
    uint64_t freq;          /* performance-counter ticks per second */
    uint64_t last_counter;  /* counter value at previous tick */
    uint64_t start_counter; /* counter value at creation */

    double   dt;            /* raw frame delta (seconds) */
    double   elapsed;       /* total elapsed time (seconds) */

    /* Fixed timestep state — feeds jce_timer_alpha() only.  The engine's
       simulation cadence is driven by JceFixedClock, not by this timer. */
    double   fixed_dt;      /* target fixed step (0 = variable) */
    double   accumulator;   /* accumulated time (seconds) */

    /* FPS smoothing. */
    float    fps_smoothed;
};

JceTimer *jce_timer_create(double fixed_dt)
{
    JceTimer *t = (JceTimer *)JCE_CALLOC(1, sizeof(*t));
    if (!t) return NULL;

    t->freq          = jce_time_perf_freq();
    t->last_counter  = jce_time_perf_counter();
    t->start_counter = t->last_counter;
    t->fixed_dt      = fixed_dt > 0.0 ? fixed_dt : 0.0;
    t->fps_smoothed  = fixed_dt > 0.0 ? (float)(1.0 / fixed_dt) : 60.0f;

    return t;
}

void jce_timer_destroy(JceTimer *t)
{
    JCE_FREE(t);
}

void jce_timer_tick(JceTimer *t)
{
    if (!t) return;

    uint64_t now = jce_time_perf_counter();
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

/* ── Local-time formatting ────────────────────────────────────────── */

#include <time.h>

size_t jce_time_format_local(int64_t epoch_seconds, const char *fmt,
                             char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;
    if (!fmt) { out[0] = '\0'; return 0; }

    time_t t = (time_t)epoch_seconds;
    struct tm local_tm;

#if JCE_COMPILER_MSVC
    if (localtime_s(&local_tm, &t) != 0) {
        out[0] = '\0';
        return 0;
    }
#elif JCE_PLATFORM_WINDOWS
    /* MinGW: localtime_r may be missing.  localtime() is not thread-
       safe, but on Windows the underlying CRT call uses thread-local
       storage (_localtime64), making it safe in practice. */
    struct tm *lp = localtime(&t);
    if (!lp) { out[0] = '\0'; return 0; }
    local_tm = *lp;
#else
    if (!localtime_r(&t, &local_tm)) {
        out[0] = '\0';
        return 0;
    }
#endif

    size_t n = strftime(out, out_size, fmt, &local_tm);
    if (n == 0 && out_size > 0) out[0] = '\0';
    return n;
}

size_t jce_time_format_utc(int64_t epoch_seconds, const char *fmt,
                           char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;
    if (!fmt) { out[0] = '\0'; return 0; }

    time_t t = (time_t)epoch_seconds;
    struct tm utc_tm;

#if JCE_COMPILER_MSVC
    if (gmtime_s(&utc_tm, &t) != 0) {
        out[0] = '\0';
        return 0;
    }
#elif JCE_PLATFORM_WINDOWS
    /* MinGW: gmtime_r may be missing.  gmtime() is thread-safe on
       Windows due to thread-local storage in the CRT. */
    struct tm *gp = gmtime(&t);
    if (!gp) { out[0] = '\0'; return 0; }
    utc_tm = *gp;
#else
    if (!gmtime_r(&t, &utc_tm)) {
        out[0] = '\0';
        return 0;
    }
#endif

    size_t n = strftime(out, out_size, fmt, &utc_tm);
    if (n == 0 && out_size > 0) out[0] = '\0';
    return n;
}

/* ── Cross-platform clock primitives ─────────────────────────────── */

uint64_t jce_time_perf_counter(void)
{
#if JCE_PLATFORM_WEB
    /* Units are nanoseconds, not a promise of nanosecond resolution. The
     * browser may coarsen performance time; equal readings are legitimate.
     * Clamp invalid/extreme values before the float-to-integer conversion. */
    const double ticks = emscripten_get_now() * 1000000.0;
    if (!(ticks > 0.0)) return 0;
    if (ticks >= (double)UINT64_MAX) return UINT64_MAX;
    return (uint64_t)ticks;
#else
    return SDL_GetPerformanceCounter();
#endif
}

uint64_t jce_time_perf_freq(void)
{
#if JCE_PLATFORM_WEB
    return 1000000000;
#else
    /* SDL caches the native frequency internally. */
    return SDL_GetPerformanceFrequency();
#endif
}

uint64_t jce_time_ticks_ms(void)
{
#if JCE_PLATFORM_WEB
    return jce_time_ticks_ns() / 1000000;
#else
    return SDL_GetTicks();
#endif
}

uint64_t jce_time_ticks_ns(void)
{
#if JCE_PLATFORM_WEB
    return jce_time_perf_counter();
#else
    return SDL_GetTicksNS();
#endif
}

int64_t jce_time_now_epoch_seconds(void)
{
    /* time(NULL) is portable POSIX/C standard.  Centralizing here so
       call sites do not include <time.h> directly. */
    return (int64_t)time(NULL);
}

double jce_time_perf_to_seconds(uint64_t start, uint64_t end)
{
    const uint64_t freq = jce_time_perf_freq();
    if (freq == 0) return 0.0;
    if (end < start) return 0.0;
    return (double)(end - start) / (double)freq;
}

double jce_time_perf_to_ms(uint64_t start, uint64_t end)
{
    return jce_time_perf_to_seconds(start, end) * 1000.0;
}

int64_t jce_time_now_epoch_milliseconds(void)
{
    SDL_Time current = 0;
    if (!SDL_GetCurrentTime(&current)) return 0;
    return (int64_t)(current / 1000000);
}
