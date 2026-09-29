/*
 * test_jce_sysinfo.c — Unit tests for jce_sysinfo.h
 *
 * Layer: L1.  Cross-platform CPU/RAM probe via SDL3.
 */

#include "unity.h"

#include <jce/os/core/jce_sysinfo.h>

#include <string.h>

void setUp(void)    { }
void tearDown(void) { }

static void test_init_fills_static_fields(void)
{
    JceSysInfo s;
    memset(&s, 0, sizeof(s));
    jce_sysinfo_init(&s);

    TEST_ASSERT_TRUE(s.cpu_cores    >= 1);
    TEST_ASSERT_TRUE(s.ram_total_mb >= 1);
}

static void test_update_is_safe_after_init(void)
{
    JceSysInfo s;
    memset(&s, 0, sizeof(s));
    jce_sysinfo_init(&s);
    jce_sysinfo_update(&s);   /* must not crash */
    /* CPU usage is in [0, 100] (approximate; first sample may be 0). */
    TEST_ASSERT_TRUE(s.cpu_usage >= 0.0f);
    TEST_ASSERT_TRUE(s.cpu_usage <= 100.0f + 1.0f);
}

static void test_process_mem_returns_some_value_or_false(void)
{
    uint64_t ws = 0, pb = 0, peak = 0;
    bool ok = jce_sysinfo_process_mem(&ws, &pb, &peak);
    /* Either succeeded (at least working_set non-zero) or platform-skipped. */
    if (ok) {
        TEST_ASSERT_TRUE(ws > 0u || pb > 0u || peak > 0u);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_fills_static_fields);
    RUN_TEST(test_update_is_safe_after_init);
    RUN_TEST(test_process_mem_returns_some_value_or_false);
    return UNITY_END();
}
