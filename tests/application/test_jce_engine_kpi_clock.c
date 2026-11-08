/* Exact private ledger; no window, renderer, timer replacement or CSV copy. */
#include "unity.h"
#ifndef JCE_TEST_ENGINE_KPI_CLOCK_HEADER
#define JCE_TEST_ENGINE_KPI_CLOCK_HEADER "application/jce_engine_kpi_clock.h"
#endif
#include JCE_TEST_ENGINE_KPI_CLOCK_HEADER
void setUp(void) {}
void tearDown(void) {}

static void test_disabled_ledger_is_inert(void)
{
    JceEngineKpiClock clock;
    jce_engine_kpi_clock_init(&clock, false, 0, 0);
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_record(&clock, 10, 0, 10, 0));
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_check(&clock, 10, 10));
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_finish(&clock, 0));
    TEST_ASSERT_EQUAL_UINT64(0, clock.render_ticks);
    TEST_ASSERT_EQUAL_UINT64(0, clock.render_intervals);
    TEST_ASSERT_EQUAL_UINT(0, clock.errors);
}
static void test_service_init_timer_and_render_span_remain_distinct(void)
{
    JceEngineKpiClock clock;
    jce_engine_kpi_clock_init(&clock, true, 1000, 100);
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_record(&clock, 1000, 2500, 10000, 0));
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_record(&clock, 1000, 10000, 20000, 1));
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_check(&clock, 1000, 20100));
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_record(&clock, 1000, 20000, 20101, 2));
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_finish(&clock, 3));
    TEST_ASSERT_EQUAL_UINT64(100, clock.timer_start);
    TEST_ASSERT_EQUAL_UINT64(20100, clock.timer_checked);
    TEST_ASSERT_EQUAL_UINT64(2500, clock.render_start);
    TEST_ASSERT_EQUAL_UINT64(20101, clock.render_end);
    TEST_ASSERT_EQUAL_UINT64(17601, clock.render_ticks);
    TEST_ASSERT_EQUAL_UINT64(3, clock.render_intervals);
    TEST_ASSERT_EQUAL_UINT64(0, clock.omitted_ticks);
}
static void test_zero_intervals_and_zero_origin_are_real_records(void)
{
    JceEngineKpiClock clock;
    jce_engine_kpi_clock_init(&clock, true, 10, 0);
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_record(&clock, 10, 0, 0, 0));
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_record(&clock, 10, 0, 1, 1));
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_check(&clock, 10, 1));
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_finish(&clock, 2));
    TEST_ASSERT_EQUAL_UINT64(2, clock.render_intervals);
    TEST_ASSERT_EQUAL_UINT64(1, clock.render_ticks);
}
static void test_gaps_are_counted_and_cannot_certify_continuity(void)
{
    JceEngineKpiClock clock;
    jce_engine_kpi_clock_init(&clock, true, 10, 0);
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_record(&clock, 10, 1, 4, 0));
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_record(&clock, 10, 6, 8, 1));
    TEST_ASSERT_EQUAL_UINT64(2, clock.omitted_ticks);
    TEST_ASSERT_TRUE(clock.errors & JCE_KPI_CLOCK_GAP);
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_finish(&clock, 2));
}
static void test_frequency_and_reversed_or_overlapping_counters_fail(void)
{
    JceEngineKpiClock clock;
    jce_engine_kpi_clock_init(&clock, true, 0, 0);
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_record(&clock, 10, 1, 2, 0));
    TEST_ASSERT_TRUE(clock.errors & JCE_KPI_CLOCK_FREQUENCY);
    jce_engine_kpi_clock_init(&clock, true, 10, 5);
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_record(&clock, 10, 4, 6, 0));
    TEST_ASSERT_TRUE(clock.errors & JCE_KPI_CLOCK_COUNTER);
    jce_engine_kpi_clock_init(&clock, true, 10, 0);
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_record(&clock, 10, 3, 2, 0));
    jce_engine_kpi_clock_init(&clock, true, 10, 0);
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_record(&clock, 10, 1, 3, 0));
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_record(&clock, 10, 2, 4, 1));
    jce_engine_kpi_clock_init(&clock, true, 10, 0);
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_record(&clock, 10, 0, 1, 0));
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_check(&clock, 20, 1));
    TEST_ASSERT_TRUE(clock.errors & JCE_KPI_CLOCK_FREQUENCY);
}
static void test_row_gaps_duplicates_and_final_count_mismatch_fail(void)
{
    JceEngineKpiClock clock;
    jce_engine_kpi_clock_init(&clock, true, 10, 0);
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_record(&clock, 10, 0, 1, 1));
    TEST_ASSERT_TRUE(clock.errors & JCE_KPI_CLOCK_SEQUENCE);
    jce_engine_kpi_clock_init(&clock, true, 10, 0);
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_record(&clock, 10, 0, 1, 0));
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_record(&clock, 10, 1, 2, 0));
    jce_engine_kpi_clock_init(&clock, true, 10, 0);
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_record(&clock, 10, 0, 1, 0));
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_check(&clock, 10, 1));
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_finish(&clock, 2));
}
static void test_counter_and_record_accumulation_cannot_overflow(void)
{
    JceEngineKpiClock clock;
    jce_engine_kpi_clock_init(&clock, true, 10, 0);
    /* Exercise actual private arithmetic boundary without wrapping a host clock. */
    clock.render_ticks = UINT64_MAX;
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_record(&clock, 10, 0, 1, 0));
    TEST_ASSERT_TRUE(clock.errors & JCE_KPI_CLOCK_OVERFLOW);
    jce_engine_kpi_clock_init(&clock, true, 10, 0);
    clock.render_intervals = UINT64_MAX;
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_record(&clock, 10, 0, 0, UINT64_MAX));
    TEST_ASSERT_TRUE(clock.errors & JCE_KPI_CLOCK_OVERFLOW);
}
static void test_terminal_deadline_and_final_interval_are_both_required(void)
{
    JceEngineKpiClock clock;
    jce_engine_kpi_clock_init(&clock, true, 10, 0);
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_record(&clock, 10, 0, 1, 0));
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_finish(&clock, 1));
    TEST_ASSERT_TRUE(clock.errors & JCE_KPI_CLOCK_TERMINAL);
    jce_engine_kpi_clock_init(&clock, true, 10, 0);
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_record(&clock, 10, 0, 1, 0));
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_check(&clock, 10, 2));
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_finish(&clock, 1));
    jce_engine_kpi_clock_init(&clock, true, 10, 0);
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_record(&clock, 10, 0, 1, 0));
    TEST_ASSERT_TRUE(jce_engine_kpi_clock_check(&clock, 10, 1));
    TEST_ASSERT_FALSE(jce_engine_kpi_clock_check(&clock, 10, 1));
}
static void test_wall_calculation_uses_zero_boundary_and_admits_subtraction_before_scaling(void)
{
    double ms = 99.0;
    TEST_ASSERT_FALSE(jce_engine_frame_clock_wall_ms(10, 0, 1, NULL));
    TEST_ASSERT_FALSE(jce_engine_frame_clock_wall_ms(0, 0, 1, &ms));
    TEST_ASSERT_EQUAL_DOUBLE(99.0, ms);
    TEST_ASSERT_FALSE(jce_engine_frame_clock_wall_ms(10, 4, 3, &ms));
    TEST_ASSERT_EQUAL_DOUBLE(99.0, ms);
    TEST_ASSERT_TRUE(jce_engine_frame_clock_wall_ms(1000, 0, 0, &ms));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, ms);
    TEST_ASSERT_TRUE(jce_engine_frame_clock_wall_ms(1000, 0, 200, &ms));
    TEST_ASSERT_EQUAL_DOUBLE(200.0, ms);
    TEST_ASSERT_TRUE(jce_engine_frame_clock_wall_ms(1000, 7, 11, &ms));
    TEST_ASSERT_EQUAL_DOUBLE(4.0, ms);
    TEST_ASSERT_TRUE(jce_engine_frame_clock_wall_ms(1000, UINT64_MAX - 4,
                                                  UINT64_MAX, &ms));
    TEST_ASSERT_EQUAL_DOUBLE(4.0, ms);
    TEST_ASSERT_TRUE(jce_engine_frame_clock_wall_ms(UINT64_MAX, 0,
                                                  UINT64_MAX, &ms));
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1000.0, ms);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_disabled_ledger_is_inert);
    RUN_TEST(test_service_init_timer_and_render_span_remain_distinct);
    RUN_TEST(test_zero_intervals_and_zero_origin_are_real_records);
    RUN_TEST(test_gaps_are_counted_and_cannot_certify_continuity);
    RUN_TEST(test_frequency_and_reversed_or_overlapping_counters_fail);
    RUN_TEST(test_row_gaps_duplicates_and_final_count_mismatch_fail);
    RUN_TEST(test_counter_and_record_accumulation_cannot_overflow);
    RUN_TEST(test_terminal_deadline_and_final_interval_are_both_required);
    RUN_TEST(test_wall_calculation_uses_zero_boundary_and_admits_subtraction_before_scaling);
    return UNITY_END();
}
