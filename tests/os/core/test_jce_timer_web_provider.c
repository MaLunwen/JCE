/* Deterministic fractional Web readings against the actual timer owner. */
#include "unity.h"
#include <jce/os/core/jce_defs.h>

#include <float.h>
#include <math.h>

/* Keep the host/compiler ABI native while selecting only the private clock
 * branch. All owner exports are isolated from the linked real core library. */
#undef JCE_PLATFORM_WEB
#define JCE_PLATFORM_WEB 1
#undef JCE_API
#define JCE_API
#define jce_timer_create clock_control_timer_create
#define jce_timer_destroy clock_control_timer_destroy
#define jce_timer_tick clock_control_timer_tick
#define jce_timer_dt clock_control_timer_dt
#define jce_timer_dt_ms clock_control_timer_dt_ms
#define jce_timer_fixed_dt clock_control_timer_fixed_dt
#define jce_timer_alpha clock_control_timer_alpha
#define jce_timer_elapsed clock_control_timer_elapsed
#define jce_timer_fps clock_control_timer_fps
#define jce_time_format_local clock_control_format_local
#define jce_time_format_utc clock_control_format_utc
#define jce_time_perf_counter clock_control_counter
#define jce_time_perf_freq clock_control_frequency
#define jce_time_ticks_ms clock_control_ticks_ms
#define jce_time_ticks_ns clock_control_ticks_ns
#define jce_time_now_epoch_seconds clock_control_epoch_seconds
#define jce_time_now_epoch_milliseconds clock_control_epoch_milliseconds
#define jce_time_perf_to_seconds clock_control_to_seconds
#define jce_time_perf_to_ms clock_control_to_ms
#define SDL_GetPerformanceCounter clock_control_sdl_counter
#define SDL_GetPerformanceFrequency clock_control_sdl_frequency
#define SDL_GetTicks clock_control_sdl_ticks_ms
#define SDL_GetTicksNS clock_control_sdl_ticks_ns
#define emscripten_get_now clock_control_web_now
#include "os/core/jce_timer.c"

static double s_now_ms;

/* Model the pinned SDL fallback: microsecond units, integer-ms readings. */
Uint64 SDLCALL clock_control_sdl_counter(void)
{
    if (!(s_now_ms > 0.0)) return 0;
    if (s_now_ms >= (double)UINT64_MAX / 1000.0) return UINT64_MAX;
    return (Uint64)s_now_ms * 1000;
}
Uint64 SDLCALL clock_control_sdl_frequency(void) { return 1000000; }
Uint64 SDLCALL clock_control_sdl_ticks_ms(void)
{
    return clock_control_sdl_counter() / 1000;
}
Uint64 SDLCALL clock_control_sdl_ticks_ns(void)
{
    return clock_control_sdl_ticks_ms() * 1000000;
}
double clock_control_web_now(void) { return s_now_ms; }

void setUp(void) { s_now_ms = 1000.0; }
void tearDown(void) { }

static void test_fractional_counter_and_stable_frequency(void)
{
    uint64_t a = clock_control_counter();
    s_now_ms += 0.125;
    uint64_t b = clock_control_counter();
    TEST_ASSERT_EQUAL_UINT64(1000000000, clock_control_frequency());
    TEST_ASSERT_EQUAL_UINT64(125000, b - a);
    TEST_ASSERT_EQUAL_UINT64(b, clock_control_counter());
    TEST_ASSERT_EQUAL_UINT64(1000000000, clock_control_frequency());
}

static void test_conversion_uses_the_selected_counter_frequency(void)
{
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.125,
        clock_control_to_ms(1000000000, 1000125000));
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.000125,
        clock_control_to_seconds(1000000000, 1000125000));
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.0, clock_control_to_ms(2, 1));
}

static void test_timer_retains_fractional_dt_elapsed_and_existing_cap(void)
{
    JceTimer *timer = clock_control_timer_create(0.001);
    TEST_ASSERT_NOT_NULL(timer);
    s_now_ms += 0.125;
    clock_control_timer_tick(timer);
    double dt = clock_control_timer_dt(timer);
    s_now_ms += 0.25;
    clock_control_timer_tick(timer);
    double elapsed = clock_control_timer_elapsed(timer);
    float alpha = clock_control_timer_alpha(timer);
    s_now_ms += 1000.0;
    clock_control_timer_tick(timer);
    double capped = clock_control_timer_dt(timer);
    double uncapped_elapsed = clock_control_timer_elapsed(timer);
    clock_control_timer_destroy(timer);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.000125, dt);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.000375, elapsed);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.375f, alpha);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.25, capped);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.000375, uncapped_elapsed);
}

static void test_ticks_share_units_origin_and_fractional_samples(void)
{
    s_now_ms = 1000.125;
    uint64_t ns = clock_control_ticks_ns();
    TEST_ASSERT_EQUAL_UINT64(1000125000, ns);
    TEST_ASSERT_EQUAL_UINT64(ns, clock_control_counter());
    TEST_ASSERT_EQUAL_UINT64(ns / 1000000, clock_control_ticks_ms());
    TEST_ASSERT_EQUAL_UINT64(ns, clock_control_ticks_ns());
    s_now_ms = 1000.875;
    TEST_ASSERT_EQUAL_UINT64(750000, clock_control_ticks_ns() - ns);
}

static void test_counter_cast_is_bounded_for_invalid_or_extreme_readings(void)
{
    s_now_ms = -1.0;
    TEST_ASSERT_EQUAL_UINT64(0, clock_control_counter());
    s_now_ms = NAN;
    TEST_ASSERT_EQUAL_UINT64(0, clock_control_counter());
    s_now_ms = DBL_MAX;
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, clock_control_counter());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fractional_counter_and_stable_frequency);
    RUN_TEST(test_conversion_uses_the_selected_counter_frequency);
    RUN_TEST(test_timer_retains_fractional_dt_elapsed_and_existing_cap);
    RUN_TEST(test_ticks_share_units_origin_and_fractional_samples);
    RUN_TEST(test_counter_cast_is_bounded_for_invalid_or_extreme_readings);
    return UNITY_END();
}
