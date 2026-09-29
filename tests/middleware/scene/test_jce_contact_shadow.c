/*
 * test_jce_contact_shadow.c
 *
 * Contact-shadow march parameters.
 *
 * A screen-space ray only knows about the first depth layer, so both ends of
 * the length range fail quietly and in opposite directions: too short and the
 * feature does nothing while costing its full fetch count, too long and it
 * invents occlusion behind every silhouette -- dark halos that read as an
 * artistic choice rather than as a ray running off the end of its information.
 */

#include "jce_contact_shadow.h"

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* ── 1. Steps never exceed the shader's constant loop bound ────────────
 *
 * The shader unrolls to JCE_CONTACT_SHADOW_MAX_STEPS.  A tier asking for more
 * would not march further -- it would just be a number that quietly means
 * something other than what it says. */

static void test_steps_fit_the_shader_loop(void)
{
    const JceGpuTier tiers[4] = {
        JCE_GPU_TIER_LOW, JCE_GPU_TIER_MEDIUM,
        JCE_GPU_TIER_HIGH, JCE_GPU_TIER_ULTRA
    };
    for (int i = 0; i < 4; ++i)
        TEST_ASSERT_TRUE(jce_contact_shadow_steps(tiers[i])
                         <= JCE_CONTACT_SHADOW_MAX_STEPS);

    TEST_ASSERT_EQUAL_UINT32(JCE_CONTACT_SHADOW_MAX_STEPS,
                             jce_contact_shadow_steps(JCE_GPU_TIER_HIGH));
    TEST_ASSERT_EQUAL_UINT32(8u, jce_contact_shadow_steps(JCE_GPU_TIER_MEDIUM));
}

/* ── 2. LOW is off, and unknown hardware is treated as LOW ─────────────  */

static void test_low_and_unknown_do_not_march(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, jce_contact_shadow_steps(JCE_GPU_TIER_LOW));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_contact_shadow_steps((JceGpuTier)77));

    /* Monotonic in tier: a stronger tier never marches less. */
    TEST_ASSERT_TRUE(jce_contact_shadow_steps(JCE_GPU_TIER_MEDIUM) >
                     jce_contact_shadow_steps(JCE_GPU_TIER_LOW));
    TEST_ASSERT_TRUE(jce_contact_shadow_steps(JCE_GPU_TIER_HIGH) >=
                     jce_contact_shadow_steps(JCE_GPU_TIER_MEDIUM));
}

/* ── 3. Jitter follows the temporal resolve, nothing else ──────────────
 *
 * Same rule as cascade dither.  Jittering into a buffer nothing averages
 * replaces banding with noise, and noise is the one that looks broken. */

static void test_jitter_requires_a_temporal_resolve(void)
{
    TEST_ASSERT_FALSE(jce_contact_shadow_jitter(false));
    TEST_ASSERT_TRUE(jce_contact_shadow_jitter(true));
}

/* ── 4. Ray length scales with texel size, not with metres ─────────────
 *
 * The error being corrected is a shadow-map texel.  A length in fixed metres
 * would be tuned at one resolution and wrong at every other -- and the failure
 * would show up as "contact shadows stopped working when I raised the shadow
 * resolution", which nobody attributes to the ray. */

static void test_ray_length_tracks_texel_size(void)
{
    /* 20 m cascade 0 at 1024: texel 39 mm, ray 8 texels = 312 mm. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.3125f,
        jce_contact_shadow_ray_length(20.0f, 1024u));

    /* Double the map: half the texel, half the ray. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.15625f,
        jce_contact_shadow_ray_length(20.0f, 2048u));
    /* Double the cascade: double the texel, double the ray.  Checked at 2048
     * so both points stay inside the clamp -- at 1024 the doubled ray is
     * 625 mm, which the 0.5 m cap truncates, and asserting the doubling there
     * would be asserting against a saturated value. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.3125f,
        jce_contact_shadow_ray_length(40.0f, 2048u));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f,
        2.0f * jce_contact_shadow_ray_length(20.0f, 2048u),
        jce_contact_shadow_ray_length(40.0f, 2048u));
}

/* ── 5. Clamped at both ends ───────────────────────────────────────────  */

static void test_ray_length_is_clamped_both_ways(void)
{
    /* A huge cascade would derive a metres-long ray.  Beyond ~0.5 m the march
     * is asserting occlusion from geometry it cannot see behind. */
    TEST_ASSERT_TRUE(jce_contact_shadow_ray_length(4000.0f, 512u) <= 0.50f);
    /* A tiny cascade on a sharp map would derive a sub-millimetre ray: all
     * the cost, none of the effect. */
    TEST_ASSERT_TRUE(jce_contact_shadow_ray_length(2.0f, 4096u) >= 0.02f);

    /* The clamps must not invert the relationship inside the valid range. */
    const float a = jce_contact_shadow_ray_length(20.0f, 2048u);
    const float b = jce_contact_shadow_ray_length(40.0f, 2048u);
    TEST_ASSERT_TRUE(b > a);
}

/* ── 6. Degenerate input disables the march ────────────────────────────
 *
 * A ray of unknown length is worse than no ray: it would darken contacts by an
 * arbitrary amount that looks deliberate. */

static void test_degenerate_input_disables_the_march(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_contact_shadow_ray_length(0.0f,  1024u));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_contact_shadow_ray_length(20.0f,    0u));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_contact_shadow_ray_length(-5.0f, 1024u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_steps_fit_the_shader_loop);
    RUN_TEST(test_low_and_unknown_do_not_march);
    RUN_TEST(test_jitter_requires_a_temporal_resolve);
    RUN_TEST(test_ray_length_tracks_texel_size);
    RUN_TEST(test_ray_length_is_clamped_both_ways);
    RUN_TEST(test_degenerate_input_disables_the_march);
    return UNITY_END();
}
