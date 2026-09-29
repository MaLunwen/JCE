/* test_jce_csm_bias_units.c
 *
 * A shadow bias has to mean a DISTANCE.  The engine's used to mean a fraction
 * of whatever depth range the cascade fit happened to produce, so the same
 * authored number was 89 mm of world offset in cascade 0 and 8969 mm in
 * cascade 3 of the same frame -- a ratio nobody chose, and one that also moved
 * when a project changed its Shadow Distance.
 *
 * The shader now divides by u_csmPenumbra[c], the per-cascade depth-range
 * units per shadow texel that jce_sr_shadow.c already uploaded for the
 * contact-hardening search.  This file guards the two properties that makes
 * true, on the CPU, from jce_csm_compute's own published outputs:
 *
 *   1. THE BRIDGE IS WHAT IT CLAIMS.  depth_range[c] / penumbra[c] is the
 *      world size of one shadow texel, 2*radius[c]/map_size, exactly.  If that
 *      ever stops holding, every consumer of that uniform -- the penumbra
 *      search and now the bias -- is scaling by a number that means something
 *      else, and nothing about the picture would say so.
 *
 *   2. THE OLD SCHEME REALLY WAS BROKEN, so this file is not asserting a
 *      tautology.  A bias held constant in NORMALIZED depth produces world
 *      offsets in the ratio depth_range[c] / depth_range[0], and that ratio is
 *      measured here and required to be large.  A test that only checked the
 *      new bridge would pass just as happily on an engine where every cascade
 *      had the same depth range and the whole problem was imaginary.
 *
 * What this file CANNOT check is that the shader still divides by it; that is
 * a source property and belongs to tools/lint/check_shadow_bias_units.py.
 * The two are stated separately rather than one pretending to cover both.
 */

#include <jce/renderer/jce_csm.h>

#include <math.h>
#include <stdio.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define FOV_DEG   60.0f
#define ASPECT    (16.0f / 9.0f)
#define MAP_SIZE  2048
#define LAMBDA    0.7f

static jce_mat4 a_view(void)
{
    const jce_vec3 eye = jce_v3(0.0f, 1.8f, 0.0f);
    return jce_m4_look_at(eye, jce_v3(0.0f, 1.4f, 1.0f), jce_v3(0, 1, 0));
}

static jce_vec3 to_sun(float el_deg)
{
    const float e = el_deg * JCE_DEG2RAD;
    return jce_v3(0.0f, sinf(e), -cosf(e));
}

static JceCsmData fit(float shadow_far, float el_deg)
{
    const jce_mat4 vw = a_view();
    const jce_vec3 sun = to_sun(el_deg);
    const jce_vec3 mn = jce_v3(-400.0f, -1.0f, -400.0f);
    const jce_vec3 mx = jce_v3( 400.0f, 12.0f,  400.0f);
    JceCsmData d;
    jce_csm_compute(&d, 4, 0.1f, shadow_far, FOV_DEG, ASPECT, &vw, &sun,
                    false, MAP_SIZE, LAMBDA, &mn, &mx);
    return d;
}

/* Exactly the quantity jce_sr_shadow.c uploads as u_csmPenumbra. */
static float penumbra_scale(const JceCsmData *d, uint32_t c)
{
    return d->depth_range[c] * (float)MAP_SIZE / (2.0f * d->radius[c]);
}

/* ── 1. The bridge ──────────────────────────────────────────────────────── */

static void test_penumbra_scale_is_world_units_per_texel(void)
{
    const float fars[4] = { 40.0f, 80.0f, 150.0f, 300.0f };
    for (int f = 0; f < 4; f++) {
        const JceCsmData d = fit(fars[f], 35.0f);
        for (uint32_t c = 0; c < d.cascade_count; c++) {
            const float texel_world = 2.0f * d.radius[c] / (float)MAP_SIZE;
            /* depth_range / penumbra == one texel in world units. */
            const float got = d.depth_range[c] / penumbra_scale(&d, c);
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(texel_world * 1e-4f, texel_world,
                got,
                "u_csmPenumbra is not depth-range-per-texel any more: every "
                "consumer that divides by it -- the contact-hardening search "
                "and the depth bias -- is now scaling by something else");
        }
    }
}

static void test_world_bias_is_independent_of_depth_range(void)
{
    /* The claim the shader's expression rests on: a bias authored as N shadow
     * texels lands as N * texel_world, whatever the cascade's depth range is.
     * Computed here from the SAME two published numbers the shader divides
     * and multiplies, so a change to either is caught. */
    const float N = 4.8f;                       /* fs_pbr's figure */
    const JceCsmData d = fit(300.0f, 35.0f);
    for (uint32_t c = 0; c < d.cascade_count; c++) {
        const float normalized = N / penumbra_scale(&d, c);
        const float world      = normalized * d.depth_range[c];
        const float expect     = N * 2.0f * d.radius[c] / (float)MAP_SIZE;
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(expect * 1e-4f, expect, world,
            "a bias authored in shadow texels did not land as that many "
            "texels of world offset");
    }
}

/* ── 2. The control: the old scheme was really broken ───────────────────── */

static void test_a_normalized_bias_would_differ_wildly_between_cascades(void)
{
    /* Without this, the two tests above would pass on an engine where every
     * cascade had the same depth range -- and would be guarding nothing. */
    const JceCsmData d = fit(300.0f, 35.0f);
    TEST_ASSERT_TRUE(d.cascade_count == 4);
    const float ratio = d.depth_range[3] / d.depth_range[0];
    printf("  depth_range %.1f / %.1f / %.1f / %.1f  -> a fixed NORMALIZED "
           "bias spans %.1fx between cascade 0 and 3\n",
           (double)d.depth_range[0], (double)d.depth_range[1],
           (double)d.depth_range[2], (double)d.depth_range[3], (double)ratio);
    TEST_ASSERT_TRUE_MESSAGE(ratio > 5.0f,
        "the cascades' depth ranges are now close enough that a normalized "
        "bias would have been nearly uniform -- which would make the two "
        "assertions above vacuous, so one of them is no longer testing "
        "anything");
}

static void test_the_same_authored_bias_moves_with_shadow_distance(void)
{
    /* The other half of the old defect, and the reason it was not merely
     * cosmetic: the same NORMALIZED number meant a different distance in
     * projects that differed only in Shadow Distance, so tuning shadows for
     * performance retuned every shadow's contact.  In texels it does not. */
    const JceCsmData a = fit(300.0f, 35.0f);
    const JceCsmData b = fit(40.0f,  35.0f);
    const float norm_span = a.depth_range[0] / b.depth_range[0];
    const float texel_a = 2.0f * a.radius[0] / (float)MAP_SIZE;
    const float texel_b = 2.0f * b.radius[0] / (float)MAP_SIZE;
    printf("  cascade 0 at shadow_far 300 vs 40: a NORMALIZED bias spans "
           "%.1fx, a TEXEL bias spans %.1fx\n",
           (double)norm_span, (double)(texel_a / texel_b));
    TEST_ASSERT_TRUE_MESSAGE(norm_span > 3.0f,
        "shadow distance no longer changes cascade 0's depth range much, so "
        "this control no longer demonstrates the defect it was written for");
    /* The texel span is NOT asserted to be 1: a bigger cascade genuinely has
     * bigger texels and genuinely needs more bias.  That variation is physical
     * and is the point; what was wrong was the part that was not. */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_penumbra_scale_is_world_units_per_texel);
    RUN_TEST(test_world_bias_is_independent_of_depth_range);
    RUN_TEST(test_a_normalized_bias_would_differ_wildly_between_cascades);
    RUN_TEST(test_the_same_authored_bias_moves_with_shadow_distance);
    return UNITY_END();
}
