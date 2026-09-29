/*
 * test_jce_render_graph_selftest.c
 *
 * Runs jce_rg_self_test().
 *
 * The render graph ships 524 lines and a self-test that checks pass
 * ordering, resource lifetimes and cycle rejection -- and until 2026-08-31
 * nothing called either.  jce_rg_create() had exactly two call sites in the
 * whole tree, both inside jce_rg_self_test(), and jce_rg_self_test() had
 * none.  Verification code that never runs is not weaker verification; it
 * is a claim nobody has ever checked.
 *
 * This test is deliberately thin: the assertions live in the self-test,
 * which returns a verdict and LOG_ERROR()s the specific failure.  Wrapping
 * it is all that was missing.
 */

#include "renderer/jce_render_graph.h"

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_render_graph_self_test_passes(void)
{
    /* On failure the self-test logs which of its cases broke; read the
     * test's stdout rather than guessing from this line. */
    TEST_ASSERT_TRUE_MESSAGE(jce_rg_self_test(),
                             "jce_rg_self_test() reported a failure - see the "
                             "LOG_ERROR lines above for which case");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_render_graph_self_test_passes);
    return UNITY_END();
}
