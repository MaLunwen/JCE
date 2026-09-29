/*
 * test_jce_shadow_blend.c
 *
 * Which cascade-blend mode a tier gets.
 *
 * The rule that matters is the one that is easy to get wrong in the direction
 * that still looks like it works: DITHER without a temporal resolve compiles,
 * runs, hits its frame budget, and renders screen-door noise along every
 * cascade boundary.  Nothing reports it, and it is worse than the banding it
 * was added to remove.
 */

#include "jce_shadow_blend.h"

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* ── 1. THE RULE: no dither without something to resolve it ────────────  */

static void test_dither_never_ships_without_temporal_resolve(void)
{
    const JceGpuTier tiers[4] = {
        JCE_GPU_TIER_LOW, JCE_GPU_TIER_MEDIUM,
        JCE_GPU_TIER_HIGH, JCE_GPU_TIER_ULTRA
    };
    for (int i = 0; i < 4; ++i)
        TEST_ASSERT_NOT_EQUAL_INT(JCE_CASCADE_BLEND_DITHER,
                                  jce_cascade_blend_mode(tiers[i], false));
}

/* ── 2. And the fallback is HARD, not LERP ─────────────────────────────
 *
 * LERP is the tempting fallback: it is the correct-looking one.  It also puts a
 * second PCF in the band on the only hardware that reached this branch, which
 * is the hardware with no headroom -- a "safe" fallback that costs more than
 * the tier above it. */

static void test_medium_without_taa_falls_to_hard_not_lerp(void)
{
    TEST_ASSERT_EQUAL_INT(JCE_CASCADE_BLEND_HARD,
        jce_cascade_blend_mode(JCE_GPU_TIER_MEDIUM, false));
    /* With a resolve, MEDIUM gets what the design specifies. */
    TEST_ASSERT_EQUAL_INT(JCE_CASCADE_BLEND_DITHER,
        jce_cascade_blend_mode(JCE_GPU_TIER_MEDIUM, true));
}

/* ── 3. The tier ladder itself ─────────────────────────────────────────  */

static void test_tier_ladder_matches_the_design(void)
{
    TEST_ASSERT_EQUAL_INT(JCE_CASCADE_BLEND_HARD,
        jce_cascade_blend_mode(JCE_GPU_TIER_LOW, true));
    TEST_ASSERT_EQUAL_INT(JCE_CASCADE_BLEND_LERP,
        jce_cascade_blend_mode(JCE_GPU_TIER_HIGH, true));
    TEST_ASSERT_EQUAL_INT(JCE_CASCADE_BLEND_LERP,
        jce_cascade_blend_mode(JCE_GPU_TIER_ULTRA, true));

    /* LOW stays hard even with a resolve available: the tier's problem is
     * fill rate, not noise, and a temporal resolve does not buy it a band. */
    TEST_ASSERT_EQUAL_INT(JCE_CASCADE_BLEND_HARD,
        jce_cascade_blend_mode(JCE_GPU_TIER_LOW, false));
}

/* ── 4. An unknown tier must not be guessed upward ─────────────────────
 *
 * A tier this function does not recognise means detection failed somewhere.
 * Resolving that toward the expensive mode puts a second PCF on hardware
 * nobody has identified -- the one machine least able to absorb a surprise. */

static void test_unknown_tier_resolves_downward(void)
{
    const JceGpuTier bogus = (JceGpuTier)99;
    TEST_ASSERT_EQUAL_INT(JCE_CASCADE_BLEND_HARD,
                          jce_cascade_blend_mode(bogus, true));
    TEST_ASSERT_EQUAL_INT(JCE_CASCADE_BLEND_HARD,
                          jce_cascade_blend_mode(bogus, false));
}

/* ── 5. The band is consistent with the mode ───────────────────────────
 *
 * A HARD mode with a non-zero band would make the shader do the blend work it
 * was chosen to avoid, and the tier's whole cost argument would be wrong while
 * the picture looked fine. */

static void test_band_matches_mode(void)
{
    const JceGpuTier tiers[4] = {
        JCE_GPU_TIER_LOW, JCE_GPU_TIER_MEDIUM,
        JCE_GPU_TIER_HIGH, JCE_GPU_TIER_ULTRA
    };
    for (int i = 0; i < 4; ++i) {
        /* HARD must cost nothing.  A non-zero band here would make the shader
         * do the blend work the mode was chosen to avoid, and the tier's whole
         * cost argument would be wrong while the picture looked fine. */
        TEST_ASSERT_EQUAL_FLOAT(0.0f,
            jce_cascade_blend_band(JCE_CASCADE_BLEND_HARD, tiers[i]));
        /* Dither's band is narrow everywhere: the band IS the noisy region. */
        TEST_ASSERT_EQUAL_FLOAT(0.05f,
            jce_cascade_blend_band(JCE_CASCADE_BLEND_DITHER, tiers[i]));
        /* Every band the shader can receive stays inside its 0..0.35 clamp,
         * or the uniform is silently clipped and this policy is fiction. */
        TEST_ASSERT_TRUE(
            jce_cascade_blend_band(JCE_CASCADE_BLEND_LERP, tiers[i]) <= 0.35f);
        TEST_ASSERT_TRUE(
            jce_cascade_blend_band(JCE_CASCADE_BLEND_LERP, tiers[i]) > 0.0f);
    }

    /* The shipped per-tier LERP widths, pinned so a later edit to the tier
     * table has to come through here and be seen. */
    TEST_ASSERT_EQUAL_FLOAT(0.22f,
        jce_cascade_blend_band(JCE_CASCADE_BLEND_LERP, JCE_GPU_TIER_HIGH));
    TEST_ASSERT_EQUAL_FLOAT(0.20f,
        jce_cascade_blend_band(JCE_CASCADE_BLEND_LERP, JCE_GPU_TIER_MEDIUM));
    TEST_ASSERT_EQUAL_FLOAT(0.18f,
        jce_cascade_blend_band(JCE_CASCADE_BLEND_LERP, JCE_GPU_TIER_LOW));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dither_never_ships_without_temporal_resolve);
    RUN_TEST(test_medium_without_taa_falls_to_hard_not_lerp);
    RUN_TEST(test_tier_ladder_matches_the_design);
    RUN_TEST(test_unknown_tier_resolves_downward);
    RUN_TEST(test_band_matches_mode);
    return UNITY_END();
}
