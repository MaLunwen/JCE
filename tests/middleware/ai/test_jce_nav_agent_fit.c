/* test_jce_nav_agent_fit.c
 *
 * A navmesh belongs to the agent it was carved for.
 *
 * JceNavAgentComponent.height was documented "editor gizmo only" and read by
 * nothing, so an agent authored at 2.4m walked a mesh built for 1.8m of
 * clearance exactly as if it fitted -- under geometry it could not fit under,
 * with nothing about the steering looking wrong.  Both navmesh backends now
 * answer through this one rule, so the grid mesh and the Detour mesh cannot
 * drift into different ideas of "too tall".
 *
 * The interesting half is the two UNKNOWNS.  A check that fired on missing
 * data would reject every scene authored before it existed, which is a worse
 * outcome than the bug it fixes.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include <jce/middleware/ai/jce_nav_agent.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_a_taller_agent_does_not_fit(void)
{
    TEST_ASSERT_FALSE_MESSAGE(jce_nav_agent_fits(2.4f, 1.8f),
        "a 2.4 m agent was accepted on a mesh carved for 1.8 m");
}

static void test_an_agent_that_fits_exactly_fits(void)
{
    /* The boundary is inclusive: a mesh carved for 1.8 m is carved FOR a
     * 1.8 m agent, so rejecting it would reject the intended case. */
    TEST_ASSERT_TRUE_MESSAGE(jce_nav_agent_fits(1.8f, 1.8f),
        "the exact height the mesh was built for was rejected");
    TEST_ASSERT_TRUE(jce_nav_agent_fits(1.7f, 1.8f));
}

static void test_an_unset_agent_height_never_rejects(void)
{
    /* Every JceNavAgentDesc written before the field existed holds 0 here. */
    TEST_ASSERT_TRUE_MESSAGE(jce_nav_agent_fits(0.0f, 1.8f),
        "an agent with no authored height was rejected");
    TEST_ASSERT_TRUE(jce_nav_agent_fits(-1.0f, 1.8f));
}

static void test_an_unknown_clearance_never_rejects(void)
{
    /* A mesh baked before the clearance was read back reports 0.  That is
     * UNKNOWN, not zero -- treating it as zero would reject everybody. */
    TEST_ASSERT_TRUE_MESSAGE(jce_nav_agent_fits(2.4f, 0.0f),
        "a mesh that does not state its clearance rejected an agent");
    TEST_ASSERT_TRUE(jce_nav_agent_fits(2.4f, -1.0f));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_taller_agent_does_not_fit);
    RUN_TEST(test_an_agent_that_fits_exactly_fits);
    RUN_TEST(test_an_unset_agent_height_never_rejects);
    RUN_TEST(test_an_unknown_clearance_never_rejects);
    return UNITY_END();
}
