/* The view-band overlap guard.
 *
 * bgfx view state is last-write-wins, so two subsystems binding different
 * framebuffers to the same view id do not fail loudly — one of them silently
 * stops rendering. That shipped twice: the point-shadow cube band had to
 * relocate away from the editor's overlay/preview/pick ids, and the
 * dual-shadow dynamic atlas then took base+21..25, which were
 * PostFX/TAA_Resolve, TAA_HistoryCopy and Composite. The viewport went black
 * and the feature was written off as unmeasurable on discrete GPUs.
 *
 * These assert the guard's actual contract, including the two cases that
 * would make it useless in practice: adjacent-but-disjoint bands must not
 * false-positive (the bands in this engine are packed tight), and the same
 * subsystem re-claiming its own range must not either (a frame renders the
 * scene several times — editor viewport, game view, thumbnails). */

#include "unity.h"

#include "renderer/jce_view_bands.h"

void setUp(void) { jce_view_bands_begin_frame(); }
void tearDown(void) {}

static void test_disjoint_bands_do_not_conflict(void)
{
    TEST_ASSERT_TRUE(jce_view_bands_claim("scene", 0u, 20u));
    TEST_ASSERT_TRUE(jce_view_bands_claim("postfx", 20u, 21u));
    TEST_ASSERT_TRUE(jce_view_bands_claim("overlay", 50u, 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_view_bands_conflict_count());
}

static void test_adjacent_bands_do_not_conflict(void)
{
    /* [10..19] and [20..29] touch but do not overlap. An off-by-one here
     * would make the guard cry wolf on every frame and get switched off. */
    TEST_ASSERT_TRUE(jce_view_bands_claim("a", 10u, 10u));
    TEST_ASSERT_TRUE(jce_view_bands_claim("b", 20u, 10u));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_view_bands_conflict_count());
}

static void test_the_shipped_collision_is_caught(void)
{
    /* The real one: postfx re-based to base+20 (21 views from there) against
     * the dual-shadow atlas at base+21..25, scene base 3. */
    TEST_ASSERT_TRUE(jce_view_bands_claim("postfx", 23u, 21u));
    TEST_ASSERT_FALSE_MESSAGE(
        jce_view_bands_claim("scene-renderer", 24u, 5u),
        "the atlas band inside the postfx range must be reported");
    TEST_ASSERT_EQUAL_UINT32(1u, jce_view_bands_conflict_count());
}

static void test_same_owner_may_reclaim(void)
{
    /* One frame renders the scene more than once, each with its own base;
     * the same subsystem re-declaring a range it already holds is normal. */
    TEST_ASSERT_TRUE(jce_view_bands_claim("scene-renderer", 3u, 20u));
    TEST_ASSERT_TRUE(jce_view_bands_claim("scene-renderer", 3u, 20u));
    TEST_ASSERT_TRUE(jce_view_bands_claim("scene-renderer", 10u, 5u));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_view_bands_conflict_count());
}

static void test_begin_frame_clears_state(void)
{
    TEST_ASSERT_TRUE(jce_view_bands_claim("x", 0u, 10u));
    TEST_ASSERT_FALSE(jce_view_bands_claim("y", 5u, 10u));
    TEST_ASSERT_EQUAL_UINT32(1u, jce_view_bands_conflict_count());
    jce_view_bands_begin_frame();
    TEST_ASSERT_EQUAL_UINT32(0u, jce_view_bands_conflict_count());
    TEST_ASSERT_TRUE(jce_view_bands_claim("y", 5u, 10u));
}

static void test_partial_overlap_at_either_end(void)
{
    TEST_ASSERT_TRUE(jce_view_bands_claim("mid", 100u, 10u));   /* 100..109 */
    TEST_ASSERT_FALSE(jce_view_bands_claim("low", 95u, 6u));    /* ..100    */
    TEST_ASSERT_FALSE(jce_view_bands_claim("high", 109u, 4u));  /* 109..    */
    TEST_ASSERT_EQUAL_UINT32(2u, jce_view_bands_conflict_count());
}

int main(void)
{
    if (!jce_view_bands_enabled()) {
        /* JCE_VIEW_BAND_CHECK=0 in the environment: claim() is a no-op by
         * design and there is nothing to assert. */
        UNITY_BEGIN();
        UNITY_END();
        return 0;
    }
    UNITY_BEGIN();
    RUN_TEST(test_disjoint_bands_do_not_conflict);
    RUN_TEST(test_adjacent_bands_do_not_conflict);
    RUN_TEST(test_the_shipped_collision_is_caught);
    RUN_TEST(test_same_owner_may_reclaim);
    RUN_TEST(test_begin_frame_clears_state);
    RUN_TEST(test_partial_overlap_at_either_end);
    return UNITY_END();
}
