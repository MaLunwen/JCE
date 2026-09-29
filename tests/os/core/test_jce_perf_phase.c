/*
 * test_jce_perf_phase.c - phase accumulator formatting.
 *
 * The formatter writes into a caller-owned char[512] on the stack and the log
 * record it lands in is also char[512], so truncation is the NORMAL case once
 * more than ~30 phases are registered -- and 128 can be. These tests exist
 * because the truncation path wrote past the end of the caller's buffer on
 * every reporting window, in every profiling run, undetected.
 */

#include "unity.h"

#include <jce/os/core/jce_perf_phase.h>

#include <string.h>

void setUp(void)    { jce_perf_phase_set_enabled(1); }
void tearDown(void) { jce_perf_phase_set_enabled(0); }

/* Long-lived names, as the API requires. */
static const char *const kNames[JCE_PERF_PHASE_MAX_SLOTS] = {
#define N16(p) p "00", p "01", p "02", p "03", p "04", p "05", p "06", p "07", \
               p "08", p "09", p "10", p "11", p "12", p "13", p "14", p "15"
    N16("phase_name_a_"), N16("phase_name_b_"), N16("phase_name_c_"),
    N16("phase_name_d_"), N16("phase_name_e_"), N16("phase_name_f_"),
    N16("phase_name_g_"), N16("phase_name_h_"),
#undef N16
};

#define GUARD_LEN 64
#define OUT_LEN   512

/* Report into `out_sz` bytes of a larger buffer whose tail is poisoned, then
 * assert the formatter respected the size it was given. */
static void report_into(int out_sz, char *buf, size_t buf_len, uint32_t frames)
{
    memset(buf, '\xA5', buf_len);
    jce_perf_phase_report_average(buf, out_sz, frames);

    for (size_t i = (size_t)out_sz; i < buf_len; ++i) {
        TEST_ASSERT_EQUAL_MESSAGE('\xA5', buf[i],
            "formatter wrote past the buffer size it was given");
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(memchr(buf, '\0', (size_t)out_sz),
        "result is not NUL-terminated inside the buffer");
}

static void test_full_table_does_not_overrun_the_caller_buffer(void)
{
    char buf[OUT_LEN + GUARD_LEN];

    /* Fill every slot with a value that formats wide, so the concatenation is
     * several times the buffer: 128 x ~20 chars against 512. */
    for (int i = 0; i < JCE_PERF_PHASE_MAX_SLOTS; ++i)
        jce_perf_phase_add(kNames[i], 1234.5 + (double)i);

    report_into(OUT_LEN, buf, sizeof buf, 1u);
}

static void test_tiny_buffer_does_not_overrun(void)
{
    /* One entry cannot even start: the boundary the old code walked past. */
    char buf[8 + GUARD_LEN];
    int  n;

    for (n = 0; n < 8; ++n)
        jce_perf_phase_add(kNames[n], 9.0 + (double)n);

    report_into(8, buf, sizeof buf, 1u);
}

static void test_zero_slots_are_left_out_so_real_ones_fit(void)
{
    char buf[OUT_LEN + GUARD_LEN];

    /* One measurable phase, then a crowd of idle ones -- the editor registers
     * a slot per panel whether or not the panel is open. The measurable one
     * must survive; the idle ones must not consume the budget. */
    jce_perf_phase_add(kNames[0], 3.5);
    for (int i = 1; i < JCE_PERF_PHASE_MAX_SLOTS; ++i)
        jce_perf_phase_add(kNames[i], 0.0);

    report_into(OUT_LEN, buf, sizeof buf, 1u);
    TEST_ASSERT_NOT_NULL(strstr(buf, kNames[0]));
    TEST_ASSERT_NULL_MESSAGE(strstr(buf, kNames[1]),
        "a slot that rounds to 0.00 was spending buffer space");
}

static void test_average_scaling_decides_what_counts_as_zero(void)
{
    char buf[OUT_LEN + GUARD_LEN];

    /* 0.4 ms over 120 frames averages to 0.0033 -- below the print threshold,
     * so the skip has to be applied to the SCALED value, not the accumulator.
     * Judging the raw total would keep an entry that then prints as "0.00". */
    jce_perf_phase_add(kNames[0], 60.0);
    jce_perf_phase_add(kNames[1], 0.4);

    report_into(OUT_LEN, buf, sizeof buf, 120u);
    TEST_ASSERT_NOT_NULL(strstr(buf, kNames[0]));
    TEST_ASSERT_NULL(strstr(buf, kNames[1]));
    TEST_ASSERT_NULL_MESSAGE(strstr(buf, "=0.00"),
        "an entry that rounds to zero reached the output");
}

static void test_report_resets_the_window(void)
{
    char buf[OUT_LEN + GUARD_LEN];

    jce_perf_phase_add(kNames[0], 5.0);
    report_into(OUT_LEN, buf, sizeof buf, 1u);
    TEST_ASSERT_NOT_NULL(strstr(buf, kNames[0]));

    /* Second window with nothing added: the slot is registered but empty, so
     * it must not reappear -- otherwise every phase ever measured accumulates
     * in the log forever. */
    report_into(OUT_LEN, buf, sizeof buf, 1u);
    TEST_ASSERT_NULL(strstr(buf, kNames[0]));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_full_table_does_not_overrun_the_caller_buffer);
    RUN_TEST(test_tiny_buffer_does_not_overrun);
    RUN_TEST(test_zero_slots_are_left_out_so_real_ones_fit);
    RUN_TEST(test_average_scaling_decides_what_counts_as_zero);
    RUN_TEST(test_report_resets_the_window);
    return UNITY_END();
}
