/* test_jce_foliage_density_mask.c
 *
 * Foliage density brush (large-world #8a): jce_foliage_scatter honours an
 * optional density mask — each candidate is kept with probability = its mask
 * cell, so a brush-painted mask thins/keeps regions.  Verifies the scatter-side
 * core (the brush UI paints this same mask).  Pure scatter math; no GPU/editor.
 */

#include <jce/middleware/scene/jce_foliage.h>

#include "unity.h"

#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static JceFoliageScatterParams base_params(void)
{
    JceFoliageScatterParams p;
    memset(&p, 0, sizeof p);
    p.seed = 123;
    p.density = 0.5f;          /* 0.5 * 100 * 100 = 2500 candidates */
    p.area_x = 100.0f;
    p.area_z = 100.0f;
    p.max_slope_deg = 90.0f;   /* no slope test (terrain NULL anyway) */
    p.scale_min = 1.0f;
    p.scale_max = 1.0f;
    return p;
}

/* A left-half mask (cols 0..1 = 1, cols 2..3 = 0) keeps only instances whose
 * candidate UV.x < 0.5, i.e. world X < origin.X. */
static void test_left_half_mask_confines_instances(void)
{
    float mask[4 * 4];
    for (int z = 0; z < 4; ++z)
        for (int x = 0; x < 4; ++x)
            mask[z * 4 + x] = (x < 2) ? 1.0f : 0.0f;

    JceFoliageScatterParams p = base_params();
    p.density_mask = mask;
    p.mask_dim = 4;

    static JceFoliageInstance out[8192];
    jce_vec3 origin = { 0.0f, 0.0f, 0.0f };
    uint32_t n = jce_foliage_scatter(&p, NULL, &origin, out, 8192);

    TEST_ASSERT_TRUE(n > 100);                 /* a meaningful number survived */
    for (uint32_t i = 0; i < n; ++i)
        TEST_ASSERT_TRUE(out[i].pos[0] < 0.0f); /* all in the painted left half */
}

/* All-ones mask keeps every candidate -> same count as the target; all-zeros
 * keeps none. */
static void test_uniform_masks_bound_count(void)
{
    static JceFoliageInstance out[8192];
    jce_vec3 origin = { 0.0f, 0.0f, 0.0f };

    float ones[16], zeros[16];
    for (int i = 0; i < 16; ++i) { ones[i] = 1.0f; zeros[i] = 0.0f; }

    JceFoliageScatterParams p = base_params();
    p.mask_dim = 4;

    p.density_mask = ones;
    uint32_t n_ones = jce_foliage_scatter(&p, NULL, &origin, out, 8192);
    TEST_ASSERT_EQUAL_UINT32(5000u, n_ones);   /* density*area, all kept */

    p.density_mask = zeros;
    uint32_t n_zero = jce_foliage_scatter(&p, NULL, &origin, out, 8192);
    TEST_ASSERT_EQUAL_UINT32(0u, n_zero);      /* all rejected */
}

/* A NULL mask is byte-identical to the pre-brush scatter (no extra RNG draw):
 * count == target and the maskless path is unchanged. */
static void test_null_mask_is_full_density(void)
{
    static JceFoliageInstance out[8192];
    jce_vec3 origin = { 0.0f, 0.0f, 0.0f };
    JceFoliageScatterParams p = base_params();   /* density_mask NULL, mask_dim 0 */
    uint32_t n = jce_foliage_scatter(&p, NULL, &origin, out, 8192);
    TEST_ASSERT_EQUAL_UINT32(5000u, n);
}

/* A half-density mask (all cells 0.5) keeps roughly half the candidates. */
static void test_half_mask_thins_to_about_half(void)
{
    static JceFoliageInstance out[8192];
    jce_vec3 origin = { 0.0f, 0.0f, 0.0f };
    float half[16];
    for (int i = 0; i < 16; ++i) half[i] = 0.5f;

    JceFoliageScatterParams p = base_params();
    p.density_mask = half;
    p.mask_dim = 4;
    uint32_t n = jce_foliage_scatter(&p, NULL, &origin, out, 8192);

    /* 5000 candidates each kept ~50% -> expect ~2500, allow generous slack. */
    TEST_ASSERT_TRUE(n > 2200 && n < 2800);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_left_half_mask_confines_instances);
    RUN_TEST(test_uniform_masks_bound_count);
    RUN_TEST(test_null_mask_is_full_density);
    RUN_TEST(test_half_mask_thins_to_about_half);
    return UNITY_END();
}
