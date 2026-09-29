/*
 * test_jce_coroutine_selftest.c
 *
 * Runs jce_coroutine_self_test().
 *
 * 93 lines of verification -- yield_next_frame ordering, wait_seconds,
 * cancellation, slot reuse -- that had no caller anywhere in the tree until
 * 2026-08-31.  It returns a verdict and LOG_ERROR()s the specific case that
 * broke, so wrapping it is all that was missing.
 *
 * The self-test saves and restores the coroutine system around itself, so it
 * does not disturb a live scheduler; nothing here needs setUp/tearDown.
 */

#include <jce/runtime/jce_coroutine.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_coroutine_self_test_passes(void)
{
    TEST_ASSERT_TRUE_MESSAGE(jce_coroutine_self_test(),
                             "jce_coroutine_self_test() reported a failure - "
                             "see the LOG_ERROR lines above for which case");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_coroutine_self_test_passes);
    return UNITY_END();
}
