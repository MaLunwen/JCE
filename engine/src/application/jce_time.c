/*
 * jce_time.c  Global frame-time singleton.
 *
 * State lives in a single TU with C11 _Atomic where useful.  Most
 * readers are on the main thread and pay no atomic cost; secondary
 * threads that peek dt for telemetry get a consistent (if possibly
 * slightly stale) snapshot via volatile reads.
 */

#include <jce/application/jce_time.h>

static volatile float    s_unscaled_dt          = 0.0f;
static volatile float    s_scaled_dt            = 0.0f;
static volatile float    s_time_scale           = 1.0f;
static volatile float    s_realtime             = 0.0f;
static volatile float    s_total_scaled_time    = 0.0f;
static volatile uint64_t s_frame_count          = 0;

void jce_time_advance(float unscaled_dt)
{
    if (unscaled_dt < 0.0f) unscaled_dt = 0.0f;
    const float scale     = s_time_scale;
    const float scaled_dt = unscaled_dt * scale;

    s_unscaled_dt        = unscaled_dt;
    s_scaled_dt          = scaled_dt;
    s_realtime          += unscaled_dt;
    s_total_scaled_time += scaled_dt;
    s_frame_count       += 1u;
}

void jce_time_reset(void)
{
    s_unscaled_dt       = 0.0f;
    s_scaled_dt         = 0.0f;
    s_time_scale        = 1.0f;
    s_realtime          = 0.0f;
    s_total_scaled_time = 0.0f;
    s_frame_count       = 0;
}

float    jce_time_delta(void)                  { return s_scaled_dt; }
float    jce_time_unscaled_delta(void)         { return s_unscaled_dt; }
float    jce_time_realtime_since_startup(void) { return s_realtime; }
float    jce_time_total(void)                  { return s_total_scaled_time; }
uint64_t jce_time_frame_count(void)            { return s_frame_count; }
float    jce_time_get_scale(void)              { return s_time_scale; }

void jce_time_set_scale(float scale)
{
    if (scale < 0.0f) scale = 0.0f;
    s_time_scale = scale;
}
