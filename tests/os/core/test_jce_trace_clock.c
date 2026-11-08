/* Actual trace producer across real SDL lifetimes; clock ports also exercise
 * exact conversion refusal without substituting a copied event producer. */
#include "unity.h"
#include <jce/os/core/jce_trace.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_filesystem.h>
#include <SDL3/SDL.h>
#include <string.h>

static bool clock_controlled;
static uint64_t clock_counter, clock_frequency;
static uint64_t clock_counter_port(void)
{ return clock_controlled ? clock_counter : jce_time_perf_counter(); }
static uint64_t clock_frequency_port(void)
{ return clock_controlled ? clock_frequency : jce_time_perf_freq(); }
#define jce_time_perf_counter clock_counter_port
#define jce_time_perf_freq clock_frequency_port
#ifndef JCE_TEST_TRACE_CLOCK_OWNER
#define JCE_TEST_TRACE_CLOCK_OWNER "os/core/jce_trace.c"
#endif
#include JCE_TEST_TRACE_CLOCK_OWNER
#undef jce_time_perf_counter
#undef jce_time_perf_freq

void setUp(void)
{
    clock_controlled = false;
    TEST_ASSERT_TRUE(SDL_Init(SDL_INIT_EVENTS));
    jce_trace_set_enabled(true);
    jce_trace_reset();
}
void tearDown(void)
{
    clock_controlled = false;
    jce_trace_set_enabled(false);
    SDL_Quit();
}

static uint64_t clock_real_ns(void)
{
    uint64_t value = 0;
    TEST_ASSERT_TRUE(jce_trace_clock_convert_internal(
        jce_time_perf_counter(), jce_time_perf_freq(), &value));
    return value;
}

static void test_real_sdl_restart_retains_prefix_on_one_clock(void)
{
    JceTraceEvent events[4];
    JceTraceCursor cursor;
    JceTraceStats stats;
    uint64_t before[2], after[2];
    SDL_Delay(25);
    before[0] = clock_real_ns();
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "before-sdl-restart", 1);
    after[0] = clock_real_ns();
    SDL_Quit();
    TEST_ASSERT_TRUE(SDL_Init(SDL_INIT_EVENTS));
    before[1] = clock_real_ns();
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "after-sdl-restart", 2);
    after[1] = clock_real_ns();
    /* No trace reset or generation change hides the pre-start prefix. */
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(2, jce_trace_read(&cursor, events, 4));
    TEST_ASSERT_EQUAL_STRING("before-sdl-restart", events[0].name);
    TEST_ASSERT_EQUAL_STRING("after-sdl-restart", events[1].name);
    TEST_ASSERT_EQUAL_UINT64(events[0].thread_id, events[1].thread_id);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(events[0].timestamp_ns, events[1].timestamp_ns);
    for (unsigned i = 0; i < 2; ++i) {
        TEST_ASSERT_GREATER_OR_EQUAL_UINT64(before[i], events[i].timestamp_ns);
        TEST_ASSERT_LESS_OR_EQUAL_UINT64(after[i], events[i].timestamp_ns);
    }
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(2, stats.recorded_events);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
}

static void test_exact_conversion_covers_wide_products_and_refuses_overflow(void)
{
    struct ClockCase { uint64_t counter, frequency, expected; } cases[] = {
        {0, 1, 0}, {1, 3, 333333333}, {2, 3, 666666666},
        {27, 3, UINT64_C(9000000000)},
        {123456789, 10000000, UINT64_C(12345678900)},
        {UINT64_C(9007199254740993), 1000000000, UINT64_C(9007199254740993)},
        {UINT64_MAX, 1000000000, UINT64_MAX},
        {UINT64_MAX, UINT64_MAX, 1000000000},
        {UINT64_MAX - 1, UINT64_MAX, 999999999},
        {(UINT64_C(1) << 63) - 1, UINT64_C(1) << 63, 999999999},
        {UINT64_MAX, UINT64_C(1) << 63, 1999999999}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint64_t value = 17;
        TEST_ASSERT_TRUE(jce_trace_clock_convert_internal(cases[i].counter,
                                                         cases[i].frequency, &value));
        TEST_ASSERT_EQUAL_UINT64(cases[i].expected, value);
    }
    uint64_t value = 17;
    TEST_ASSERT_FALSE(jce_trace_clock_convert_internal(1, 0, &value));
    TEST_ASSERT_EQUAL_UINT64(17, value);
    TEST_ASSERT_FALSE(jce_trace_clock_convert_internal(UINT64_MAX, 1, &value));
    TEST_ASSERT_FALSE(jce_trace_clock_convert_internal(UINT64_MAX, 999999999, &value));
    TEST_ASSERT_EQUAL_UINT64(17, value);
    TEST_ASSERT_FALSE(jce_trace_clock_convert_internal(1, 1, NULL));
}

static void test_invalid_provider_leaves_accounted_pending_sequence(void)
{
    JceTraceCursor cursor;
    JceTraceEvent events[4];
    JceTraceStats stats;
    clock_controlled = true;
    clock_counter = 1;
    clock_frequency = 0;
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "invalid-clock", 1);
    clock_frequency = 1;
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "later-valid-clock", 2);
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(0, jce_trace_read(&cursor, events, 4));
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(2, stats.recorded_events);
    /* A capture cannot pass recorded==captured; the gap is not fabricated time. */
    TEST_ASSERT_EQUAL_UINT32(1, cursor.next_sequence);
    jce_trace_reset();
    clock_counter = UINT64_MAX;
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "overflow-clock", 3);
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(0, jce_trace_read(&cursor, events, 4));
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(1, stats.recorded_events);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_real_sdl_restart_retains_prefix_on_one_clock);
    RUN_TEST(test_exact_conversion_covers_wide_products_and_refuses_overflow);
    RUN_TEST(test_invalid_provider_leaves_accounted_pending_sequence);
    return UNITY_END();
}
