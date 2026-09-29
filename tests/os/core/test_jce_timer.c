/*
 * test_jce_timer.c — Unit tests for jce_timer.h
 *
 * Layer: L1.  High-resolution clock + fixed-step accumulator.
 */

#include "unity.h"

#include <jce/os/core/jce_timer.h>

void setUp(void)    { }
void tearDown(void) { }

/* ---- clock primitives ------------------------------------------------ */

static void test_perf_freq_nonzero(void)
{
    TEST_ASSERT_TRUE(jce_time_perf_freq() > 0u);
}

static void test_perf_counter_monotonic(void)
{
    uint64_t a = jce_time_perf_counter();
    uint64_t b = jce_time_perf_counter();
    TEST_ASSERT_TRUE(b >= a);
}

static void test_ticks_ms_monotonic(void)
{
    uint64_t a = jce_time_ticks_ms();
    uint64_t b = jce_time_ticks_ms();
    TEST_ASSERT_TRUE(b >= a);
}

static void test_perf_to_seconds_consistent(void)
{
    uint64_t start = jce_time_perf_counter();
    /* Burn a tiny interval. */
    for (volatile int i = 0; i < 10000; ++i) { }
    uint64_t end = jce_time_perf_counter();
    double sec = jce_time_perf_to_seconds(start, end);
    double ms  = jce_time_perf_to_ms(start, end);
    TEST_ASSERT_TRUE(sec >= 0.0);
    TEST_ASSERT_TRUE(ms  >= 0.0);
    /* ms / 1000 ≈ sec (allow large slack). */
    double diff = (sec * 1000.0) - ms;
    if (diff < 0) diff = -diff;
    TEST_ASSERT_TRUE(diff < 1.0);
}

static void test_epoch_seconds_positive(void)
{
    int64_t now = jce_time_now_epoch_seconds();
    /* Sanity: after the year 2000 (946684800). */
    TEST_ASSERT_TRUE(now > 946684800);
}

/* ---- JceTimer object ------------------------------------------------- */

static void test_timer_create_destroy(void)
{
    JceTimer *t = jce_timer_create(1.0 / 60.0);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_DOUBLE(1.0 / 60.0, jce_timer_fixed_dt(t));
    jce_timer_destroy(t);
}

static void test_timer_tick_progresses_state(void)
{
    JceTimer *t = jce_timer_create(1.0 / 60.0);
    jce_timer_tick(t);
    jce_timer_tick(t);
    TEST_ASSERT_TRUE(jce_timer_dt(t)      >= 0.0);
    TEST_ASSERT_TRUE(jce_timer_dt_ms(t)   >= 0.0f);
    TEST_ASSERT_TRUE(jce_timer_elapsed(t) >= 0.0);
    TEST_ASSERT_TRUE(jce_timer_alpha(t)   >= 0.0f);
    TEST_ASSERT_TRUE(jce_timer_alpha(t)   <= 1.0f);
    jce_timer_destroy(t);
}

/* ---- jce_time_format_local ------------------------------------------ */

static void test_format_local_writes_buffer(void)
{
    char buf[64] = { 0 };
    size_t n = jce_time_format_local(0, "%Y", buf, sizeof(buf));
    TEST_ASSERT_TRUE(n >= 4);                 /* "1970" or local equiv. */
    TEST_ASSERT_EQUAL_size_t(n, jce_time_format_local(0, "%Y", buf, sizeof(buf)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_perf_freq_nonzero);
    RUN_TEST(test_perf_counter_monotonic);
    RUN_TEST(test_ticks_ms_monotonic);
    RUN_TEST(test_perf_to_seconds_consistent);
    RUN_TEST(test_epoch_seconds_positive);
    RUN_TEST(test_timer_create_destroy);
    RUN_TEST(test_timer_tick_progresses_state);
    RUN_TEST(test_format_local_writes_buffer);
    return UNITY_END();
}
