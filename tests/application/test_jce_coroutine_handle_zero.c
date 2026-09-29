/*
 * test_jce_coroutine_handle_zero.c
 *
 * Regression guard for the handle-vs-sentinel collision fixed 2026-08-31.
 *
 * The public handle is (generation << 32) | slot and JCE_COROUTINE_INVALID
 * is 0.  jce_coroutine_system_init() memset the slot pool, so slot 0 carried
 * generation 0 and the FIRST coroutine started after init packed to 0 -- a
 * successful start that is bit-identical to the documented failure value.
 * resolve() short-circuits on that sentinel, so the coroutine was also
 * reported dead by jce_coroutine_is_alive() while it ran, and
 * jce_coroutine_cancel() silently did nothing to it.
 *
 * jce_coroutine_self_test() catches this on its first assertion, but it lives
 * behind `#ifndef NDEBUG` and so does not exist in the configuration that
 * ships.  This test is deliberately plain C against the public header alone,
 * so it runs in the Release suite where the fix actually has to hold.
 */

#include <jce/runtime/jce_coroutine.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static bool never_finishes(void *user, JceCoroutineWait *next)
{
    int *ticks = (int *)user;
    (*ticks)++;
    jce_coroutine_yield_next_frame(next);
    return true;                       /* stay alive */
}

static void test_first_handle_after_init_is_not_the_invalid_sentinel(void)
{
    jce_coroutine_system_shutdown();   /* force a pristine pool */
    jce_coroutine_system_init();

    int ticks = 0;
    const JceCoroutineHandle h = jce_coroutine_start(never_finishes, &ticks);

    TEST_ASSERT_TRUE_MESSAGE(h != JCE_COROUTINE_INVALID,
        "the first coroutine after init packed to JCE_COROUTINE_INVALID: a "
        "successful start that every documented caller reads as a failure");

    /* The two consequences of the collision, asserted separately so a
     * regression says which half came back. */
    TEST_ASSERT_TRUE_MESSAGE(jce_coroutine_is_alive(h),
        "the first coroutine reports not-alive while it is running");

    jce_coroutine_cancel(h);
    TEST_ASSERT_FALSE_MESSAGE(jce_coroutine_is_alive(h),
        "cancel() did not take: the first coroutine cannot be stopped");

    jce_coroutine_system_shutdown();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_handle_after_init_is_not_the_invalid_sentinel);
    return UNITY_END();
}
