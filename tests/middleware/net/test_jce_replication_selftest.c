/*
 * test_jce_replication_selftest.c
 *
 * Runs jce_net_replication_self_test().
 *
 * It had no caller anywhere in the tree until 2026-08-31.  Two sibling
 * self-tests in the same state were wired the same day and BOTH failed on
 * their first ever run; one of them (the coroutine scheduler) turned out to
 * be a real production bug in handle encoding.  Verification code that never
 * runs is a claim nobody has checked, not weaker verification.
 *
 * Debug-only on purpose: the self-test reports failure through assert(), and
 * NDEBUG compiles assert() to nothing.  Registering it in Release would
 * produce a test that passes without checking anything -- strictly worse than
 * not having it.  The CMake else-branch says so rather than dropping it
 * silently.
 *
 * The declaration is repeated here because the self-test has no public
 * header: it is non-static only so that something outside its translation
 * unit can call it, and until today nothing did.
 */

#include "unity.h"

void jce_net_replication_self_test(void);

void setUp(void)    {}
void tearDown(void) {}

static void test_replication_self_test_runs(void)
{
    /* Failure arrives as an abort() from a live assert, which ctest reports
     * as a failed test; there is no verdict to return. */
    jce_net_replication_self_test();
    TEST_PASS();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_replication_self_test_runs);
    return UNITY_END();
}
