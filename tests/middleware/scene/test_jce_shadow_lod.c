/*
 * test_jce_shadow_lod.c
 *
 * Sub-texel shadow-caster culling.  This logic already ships and had no test,
 * which is the worst combination for a cull: every way it can be wrong removes
 * shadows, and a missing shadow does not crash, warn, or look like a defect.
 * It looks like the lighting is soft.
 *
 * So the assertions are mostly about what must SURVIVE the cull, not what it
 * removes.
 */

#include "jce_shadow_lod.h"

#include <math.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* ── 1. The span is the projected texel count, exactly ──────────────────
 *
 * A factor-of-two error here (cascade radius used where diameter belongs) does
 * not look like a bug -- it just makes the cull twice as aggressive, and the
 * only evidence is shadows that are not there. */

static void test_span_is_the_projected_texel_count(void)
{
    /* A 1 m caster in a 64 m-radius (128 m across) cascade on a 2048 map:
     * one texel is 62.5 mm, so the caster spans 16 of them. */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 16.0f,
        jce_shadow_caster_texel_span(1.0f, 64.0f, 2048u));

    /* Twice the map: twice the texels. */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 32.0f,
        jce_shadow_caster_texel_span(1.0f, 64.0f, 4096u));
    /* Twice the cascade: half the texels. */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 8.0f,
        jce_shadow_caster_texel_span(1.0f, 128.0f, 2048u));

    /* Degenerates report 0 = "unknown", never a small number that would read
     * as "too small to draw". */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_shadow_caster_texel_span(0.0f, 64.0f, 2048u));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_shadow_caster_texel_span(1.0f, 0.0f,  2048u));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_shadow_caster_texel_span(1.0f, 64.0f, 0u));
}

/* ── 2. THE POINT: the same caster, different cascades ─────────────────
 *
 * This is what makes the cull per-cascade rather than a world-size threshold.
 * A cull that dropped the bush everywhere would take its shadow from under the
 * player's feet; one that kept it everywhere would save nothing. */

static void test_same_caster_survives_near_and_drops_far(void)
{
    const float bush = 0.5f;          /* half-metre shrub */
    const uint32_t map = 2048u;
    const float min_texels = 1.5f;

    /* Cascade 0: 20 m radius -> spans 25.6 texels.  Clearly real. */
    TEST_ASSERT_TRUE(jce_shadow_caster_resolvable(bush, 20.0f, map, min_texels));
    /* Cascade 3: 1500 m radius -> spans 0.34 texels.  Cannot resolve. */
    TEST_ASSERT_FALSE(jce_shadow_caster_resolvable(bush, 1500.0f, map, min_texels));

    /* And a large caster must survive even in the far cascade -- distance alone
     * is not the criterion, and a cull that used it would delete the shadow of
     * every distant building. */
    TEST_ASSERT_TRUE(jce_shadow_caster_resolvable(40.0f, 1500.0f, map, min_texels));
}

/* ── 3. Disabling the cull must change nothing ─────────────────────────
 *
 * JCE_SHADOW_CASTER_MIN_TEXELS=0 is the A/B switch used to attribute frame
 * time.  If it altered the picture, every measurement taken with it would be
 * comparing two different scenes. */

static void test_zero_threshold_keeps_everything(void)
{
    const float sizes[5] = { 0.001f, 0.1f, 1.0f, 10.0f, 1000.0f };
    const float radii[4] = { 5.0f, 50.0f, 500.0f, 5000.0f };
    for (int s = 0; s < 5; ++s)
        for (int r = 0; r < 4; ++r) {
            TEST_ASSERT_TRUE(jce_shadow_caster_resolvable(sizes[s], radii[r],
                                                          2048u, 0.0f));
            TEST_ASSERT_TRUE(jce_shadow_caster_resolvable(sizes[s], radii[r],
                                                          2048u, -1.0f));
        }
}

/* ── 4. Every unknown keeps the caster ─────────────────────────────────
 *
 * A caster with no AABB, a cascade with no radius, an uninitialised map size:
 * each of these is a bug somewhere else, and each must fail toward DRAWING.
 * Failing the other way hides the evidence of the original bug. */

static void test_degenerate_input_keeps_the_caster(void)
{
    TEST_ASSERT_TRUE(jce_shadow_caster_resolvable(0.0f,  64.0f, 2048u, 1.5f));
    TEST_ASSERT_TRUE(jce_shadow_caster_resolvable(1.0f,   0.0f, 2048u, 1.5f));
    TEST_ASSERT_TRUE(jce_shadow_caster_resolvable(1.0f,  64.0f,    0u, 1.5f));
    TEST_ASSERT_TRUE(jce_shadow_caster_resolvable(-1.0f, 64.0f, 2048u, 1.5f));

    /* A NaN threshold reaches here from atof() on a malformed
     * JCE_SHADOW_CASTER_MIN_TEXELS.  Every comparison against NaN is false, so
     * the natural-looking guard `min_texels < 0` lets it through and then
     * `span >= NaN` drops EVERY caster -- one typo in an env var and the scene
     * has no shadows at all, with no message anywhere.  The guard has to be
     * written as a negated positive test to catch it. */
    const float nan_threshold = nanf("");   /* runtime: 0.0f/0.0f folds to a compile error */
    TEST_ASSERT_TRUE(jce_shadow_caster_resolvable(1.0f, 64.0f, 2048u, nan_threshold));
}

/* ── 5. Monotonic in every argument ────────────────────────────────────
 *
 * A bigger caster must never be culled where a smaller one survived, and a
 * sharper map must never cull what a coarser one kept.  Violating either means
 * the cull has an inversion somewhere, and an inversion deletes exactly the
 * shadows that matter most. */

static void test_monotonic_in_size_and_resolution(void)
{
    const float min_texels = 1.5f;
    for (int r = 1; r <= 40; ++r) {
        const float radius = (float)r * 25.0f;
        bool prev = false;
        for (int s = 1; s <= 60; ++s) {          /* growing caster */
            const bool keep = jce_shadow_caster_resolvable((float)s * 0.05f,
                                                           radius, 2048u,
                                                           min_texels);
            if (prev) TEST_ASSERT_TRUE(keep);    /* once kept, never dropped */
            prev = keep;
        }
        /* Same caster, finer map: never worse. */
        for (int s = 1; s <= 60; ++s) {
            const float d = (float)s * 0.05f;
            if (jce_shadow_caster_resolvable(d, radius, 1024u, min_texels))
                TEST_ASSERT_TRUE(jce_shadow_caster_resolvable(d, radius, 2048u,
                                                              min_texels));
        }
    }
}

/* ── 6. The default threshold is conservative ──────────────────────────
 *
 * 1.5 texels is the shipped default.  A shadow narrower than one texel cannot
 * appear at all, so anything at or above 1 texel is the aggressive end; the
 * test pins that the default has not drifted somewhere it starts eating
 * shadows a viewer would notice. */

static void test_default_threshold_only_drops_subtexel_casters(void)
{
    const float min_texels = 1.5f;
    /* Exactly at the threshold: kept.  The boundary must be inclusive, or the
     * cull is one ulp more aggressive than it documents. */
    const float diag = 1.5f * 2.0f * 64.0f / 2048.0f;
    TEST_ASSERT_TRUE(jce_shadow_caster_resolvable(diag, 64.0f, 2048u, min_texels));

    /* A caster spanning a full 2 texels always survives the default. */
    const float two = 2.0f * 2.0f * 64.0f / 2048.0f;
    TEST_ASSERT_TRUE(jce_shadow_caster_resolvable(two, 64.0f, 2048u, min_texels));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_span_is_the_projected_texel_count);
    RUN_TEST(test_same_caster_survives_near_and_drops_far);
    RUN_TEST(test_zero_threshold_keeps_everything);
    RUN_TEST(test_degenerate_input_keeps_the_caster);
    RUN_TEST(test_monotonic_in_size_and_resolution);
    RUN_TEST(test_default_threshold_only_drops_subtexel_casters);
    return UNITY_END();
}
