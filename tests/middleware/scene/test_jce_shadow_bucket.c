/*
 * test_jce_shadow_bucket.c
 *
 * Ordering alpha-tested shadow casters after opaque ones.
 *
 * The thing under test is not the optimisation -- that is a measurement on a
 * specific machine. It is the SWITCH: a performance flag whose "off" state is
 * not byte-identical to shipping makes every A/B taken with it a comparison
 * between two things that both differ from what users run, and the resulting
 * number is confidently wrong rather than absent.
 */

#include "jce_shadow_bucket.h"

#include <stdlib.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* ── 1. Opaque sorts before masked ─────────────────────────────────────
 *
 * The whole point. If the order inverted, masked casters would run first with
 * nothing to depth-reject against -- the pass would keep its cost and lose its
 * batching, which is strictly worse than not doing this at all. */

static void test_opaque_sorts_before_masked(void)
{
    TEST_ASSERT_TRUE(jce_shadow_bucket_key(JCE_SHADOW_BUCKET_OPAQUE) <
                     jce_shadow_bucket_key(JCE_SHADOW_BUCKET_MASKED));
}

/* ── 2. The key is the BUCKET, not the caster ──────────────────────────
 *
 * Two different masked programs -- foliage cards and, later, anything else
 * that discards -- must land in the same bucket. Keying on the program instead
 * would silently re-interleave the pass the moment a second alpha-tested
 * caster type appeared, and the regression would be a performance one with no
 * visual signature at all. */

static void test_same_bucket_gives_the_same_key(void)
{
    TEST_ASSERT_EQUAL_UINT32(jce_shadow_bucket_key(JCE_SHADOW_BUCKET_MASKED),
                             jce_shadow_bucket_key(JCE_SHADOW_BUCKET_MASKED));
    TEST_ASSERT_EQUAL_UINT32(jce_shadow_bucket_key(JCE_SHADOW_BUCKET_OPAQUE),
                             jce_shadow_bucket_key(JCE_SHADOW_BUCKET_OPAQUE));

    /* Room between them for a future bucket, so inserting one does not
     * renumber -- and therefore does not silently reorder -- the two that
     * already exist. */
    const uint32_t gap = jce_shadow_bucket_key(JCE_SHADOW_BUCKET_MASKED)
                       - jce_shadow_bucket_key(JCE_SHADOW_BUCKET_OPAQUE);
    TEST_ASSERT_TRUE(gap > 1u);
}

/* ── 3. THE POINT: the switch defaults OFF ─────────────────────────────
 *
 * This ships disabled because the trade -- early-Z rejection against lost
 * state sorting in the highest-draw-count pass in the engine -- has not been
 * measured on any machine. A default of ON would mean shipping a performance
 * change justified by a paper rather than by this renderer. */

static void test_default_is_off(void)
{
    /* Unset in the test environment; the cached read must report disabled. */
    TEST_ASSERT_FALSE(jce_shadow_masked_last_enabled());

    /* And it is stable across calls -- the value is cached, so a caller that
     * checked it per draw and a caller that checked it per frame must agree.
     * A flag that could change mid-frame would reorder half a pass. */
    for (int i = 0; i < 8; ++i)
        TEST_ASSERT_FALSE(jce_shadow_masked_last_enabled());
}

int main(void)
{
    /* Make the default explicit rather than inherited: if the surrounding
     * environment happened to set this, the test above would be asserting the
     * environment rather than the default. */
#if defined(_WIN32)
    _putenv("JCE_SHADOW_MASKED_LAST=");
#else
    unsetenv("JCE_SHADOW_MASKED_LAST");
#endif

    UNITY_BEGIN();
    RUN_TEST(test_opaque_sorts_before_masked);
    RUN_TEST(test_same_bucket_gives_the_same_key);
    RUN_TEST(test_default_is_off);
    return UNITY_END();
}
