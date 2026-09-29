/*
 * test_jce_profiler.c — Unit tests for jce_profiler.h macros.
 *
 * Layer: L1.  When Tracy is disabled (default test build) every macro
 * compiles to a no-op — verifying that user code still compiles and
 * the macros do not have side effects.
 */

#include "unity.h"

#include <jce/os/core/jce_profiler.h>

void setUp(void)    { }
void tearDown(void) { }

static void test_macros_compile_and_run(void)
{
    int side_effect = 0;
    {
        JCE_PROFILE_ZONE;
        side_effect += 1;
        JCE_PROFILE_ZONE_END;
    }
    {
        JCE_PROFILE_ZONE_N("named");
        side_effect += 1;
        JCE_PROFILE_ZONE_END;
    }
    {
        JCE_PROFILE_ZONE_C(0xFF0000);
        side_effect += 1;
        JCE_PROFILE_ZONE_END;
    }

    JCE_PROFILE_FRAME_MARK;
    JCE_PROFILE_FRAME_MARK_N("custom");
    JCE_PROFILE_ALLOC((void*)0x1234, 64);
    JCE_PROFILE_FREE ((void*)0x1234);
    JCE_PROFILE_PLOT  ("plot",  3.14);
    JCE_PROFILE_PLOT_I("ploti", 42);
    JCE_PROFILE_MSG   ("hi", 2);
    JCE_PROFILE_THREAD_NAME("worker");

    TEST_ASSERT_EQUAL_INT(3, side_effect);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_macros_compile_and_run);
    return UNITY_END();
}
