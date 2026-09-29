/*
 * test_jce_fixed_clock.c — Unit tests for jce_fixed_clock.h (P3-B.2).
 *
 * Layer: L2.  Deterministic fixed-step accumulator (Glenn Fiedler).
 */

#include "unity.h"

#include <jce/os/core/jce_fixed_clock.h>

#include <math.h>

void setUp(void)    { }
void tearDown(void) { }

/* ---- init -------------------------------------------------------- */

static void test_init_defaults_on_bad_args(void)
{
    JceFixedClock c;
    jce_fixed_clock_init(&c, 0.0, 0.0);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0 / 60.0, c.fixed_dt);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.25,       c.max_frame_dt);
    TEST_ASSERT_EQUAL_UINT64(0, c.tick_count);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, c.fixed_time);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, c.accumulator);
}

static void test_init_honours_explicit_values(void)
{
    JceFixedClock c;
    jce_fixed_clock_init(&c, 1.0 / 60.0, 0.1);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0 / 60.0, c.fixed_dt);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.1,        c.max_frame_dt);
}

/* ---- advance / tick --------------------------------------------- */

static void test_single_frame_no_steps_when_under_dt(void)
{
    JceFixedClock c;
    jce_fixed_clock_init(&c, 1.0 / 50.0, 0.25);
    uint32_t n = jce_fixed_clock_advance(&c, 0.005); /* < 0.02 */
    TEST_ASSERT_EQUAL_UINT32(0u, n);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.005, c.accumulator);
}

static void test_single_frame_one_step(void)
{
    JceFixedClock c;
    jce_fixed_clock_init(&c, 1.0 / 50.0, 0.25);
    uint32_t n = jce_fixed_clock_advance(&c, 0.02);
    TEST_ASSERT_EQUAL_UINT32(1u, n);
    jce_fixed_clock_tick(&c);
    TEST_ASSERT_EQUAL_UINT64(1u, c.tick_count);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0 / 50.0, c.fixed_time);
}

/* 30 FPS feeding a 50 Hz clock: 100 frames * 0.033 = 3.3 s of sim,
 * expect floor(3.3 / 0.02) = 165 ticks (±1 for float slack). */
static void test_30fps_into_50hz_yields_expected_tick_count(void)
{
    JceFixedClock c;
    jce_fixed_clock_init(&c, 1.0 / 50.0, 0.25);
    uint64_t total = 0;
    for (int i = 0; i < 100; ++i) {
        uint32_t n = jce_fixed_clock_advance(&c, 0.033);
        for (uint32_t k = 0; k < n; ++k) jce_fixed_clock_tick(&c);
        total += n;
    }
    TEST_ASSERT_EQUAL_UINT64(total, c.tick_count);
    TEST_ASSERT_TRUE(total >= 164u && total <= 166u);
}

/* Huge stall: a 1.0 s frame_dt at 50 Hz / 0.25 s cap must clamp to
 * at most floor(0.25 / 0.02) = 12 steps — no spiral of death. */
static void test_huge_stall_clamps(void)
{
    JceFixedClock c;
    jce_fixed_clock_init(&c, 1.0 / 50.0, 0.25);
    uint32_t n = jce_fixed_clock_advance(&c, 1.0);
    TEST_ASSERT_TRUE(n <= 12u);
}

static void test_alpha_in_unit_interval(void)
{
    JceFixedClock c;
    jce_fixed_clock_init(&c, 1.0 / 50.0, 0.25);
    (void)jce_fixed_clock_advance(&c, 0.025); /* 1 step, 0.005 remainder */
    double a = jce_fixed_clock_alpha(&c);
    TEST_ASSERT_TRUE(a >= 0.0 && a <= 1.0);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.25, a); /* 0.005 / 0.02 */
}

static void test_negative_dt_is_no_op(void)
{
    JceFixedClock c;
    jce_fixed_clock_init(&c, 1.0 / 50.0, 0.25);
    uint32_t n = jce_fixed_clock_advance(&c, -1.0);
    TEST_ASSERT_EQUAL_UINT32(0u, n);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, c.accumulator);
}

/* ---- default singleton ------------------------------------------ */

static void test_default_clock_is_persistent(void)
{
    JceFixedClock *a = jce_fixed_clock_default();
    JceFixedClock *b = jce_fixed_clock_default();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_PTR(a, b);
    /* Default cadence matches the runtime/physics 60 Hz cadence. */
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0 / 60.0, a->fixed_dt);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_defaults_on_bad_args);
    RUN_TEST(test_init_honours_explicit_values);
    RUN_TEST(test_single_frame_no_steps_when_under_dt);
    RUN_TEST(test_single_frame_one_step);
    RUN_TEST(test_30fps_into_50hz_yields_expected_tick_count);
    RUN_TEST(test_huge_stall_clamps);
    RUN_TEST(test_alpha_in_unit_interval);
    RUN_TEST(test_negative_dt_is_no_op);
    RUN_TEST(test_default_clock_is_persistent);
    return UNITY_END();
}
