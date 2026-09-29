/*
 * test_jce_cloud_shadow.c
 *
 * Top-down cloud transmittance.
 *
 * This term multiplies the sun over the entire world, so its failures are
 * global and none of them look like failures: a map of zeros is night, a map
 * of ones is a clear sky, and a half-texel offset is wind. The assertions
 * below are chosen so that each of those reads as a test failure instead.
 */

#include "renderer/jce_cloud_shadow.h"
#include "renderer/jce_cloud_noise.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define RES 32u

/* Coverage is no longer a scalar on the desc: it comes from the field's own
 * 2-D weather map, biased by `weather_coverage_bias`, because that is what the
 * SKY's atlas is baked from and this map has to agree with it. The tests
 * therefore drive the bias, which is the control the renderer drives too. */
static JceCloudNoiseParams noise_with_bias(float bias)
{
    JceCloudNoiseParams p;
    jce_cloud_noise_params_default(&p);
    p.weather_coverage_bias = bias;
    return p;
}

static JceCloudShadowDesc base_desc(void)
{
    JceCloudShadowDesc d;
    memset(&d, 0, sizeof d);
    d.noise          = NULL;              /* defaults: bias 0 */
    d.sun_dir[0]     = 0.0f;
    d.sun_dir[1]     = 1.0f;              /* straight up */
    d.sun_dir[2]     = 0.0f;
    d.layer_bottom_m = 1500.0f;
    d.layer_top_m    = 4000.0f;
    d.extinction     = 0.05f;
    d.world_extent_m = 4000.0f;
    d.center_x       = 0.0f;
    d.center_z       = 0.0f;
    d.resolution     = RES;
    return d;
}

/* ── 1. A bake produces varying transmittance in [0,1] ─────────────────
 *
 * A CONSTANT map is the failure that hides best: it is a perfectly valid
 * texture, it multiplies cleanly, and it renders as "the clouds happen not to
 * be casting today". */

static void test_bake_varies_and_stays_in_range(void)
{
    JceCloudShadowDesc d = base_desc();
    float *map = malloc(sizeof(float) * RES * RES);
    TEST_ASSERT_NOT_NULL(map);
    TEST_ASSERT_TRUE(jce_cloud_shadow_bake(&d, map));

    float lo = 2.0f, hi = -1.0f;
    for (uint32_t i = 0; i < RES * RES; ++i) {
        TEST_ASSERT_TRUE(isfinite(map[i]));
        TEST_ASSERT_TRUE(map[i] >= 0.0f && map[i] <= 1.0f);
        if (map[i] < lo) lo = map[i];
        if (map[i] > hi) hi = map[i];
    }
    /* Real structure, not a flat field. */
    TEST_ASSERT_TRUE_MESSAGE(hi - lo > 0.01f, "map is constant - no cloud shadow");

    free(map);
}

/* ── 2. THE POINT: more cloud means less sun ───────────────────────────
 *
 * Monotone in coverage. If this inverted, overcast skies would brighten the
 * ground -- and because everything scales together, it would look like an
 * exposure choice rather than a sign error. */

static void test_more_coverage_means_less_light(void)
{
    float *a = malloc(sizeof(float) * RES * RES);
    float *b = malloc(sizeof(float) * RES * RES);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);

    JceCloudShadowDesc d = base_desc();
    JceCloudNoiseParams light = noise_with_bias(-0.04f);   /* coverage 0.2 */
    JceCloudNoiseParams heavy = noise_with_bias(+0.52f);   /* coverage 0.9 */
    d.noise = &light;
    TEST_ASSERT_TRUE(jce_cloud_shadow_bake(&d, a));
    d.noise = &heavy;
    TEST_ASSERT_TRUE(jce_cloud_shadow_bake(&d, b));

    double sa = 0.0, sb = 0.0;
    for (uint32_t i = 0; i < RES * RES; ++i) { sa += a[i]; sb += b[i]; }
    TEST_ASSERT_TRUE_MESSAGE(sb < sa, "heavier coverage did not darken the ground");

    /* Zero coverage must be exactly clear: the bias goes to -1, the weather
     * map clamps to nothing, the density field returns 0, the integral is 0
     * and transmittance is 1. Anything less is a constant dimming applied to
     * every scene that has clouds switched off. */
    JceCloudNoiseParams clear = noise_with_bias(-1.0f);
    d.noise = &clear;
    TEST_ASSERT_TRUE(jce_cloud_shadow_bake(&d, a));
    for (uint32_t i = 0; i < RES * RES; ++i)
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, a[i]);

    free(a);
    free(b);
}

/* ── 3. A low sun casts through MORE cloud ─────────────────────────────
 *
 * The ray crosses the slab at an angle, so its path length is thickness/sin,
 * not thickness. Using the thickness directly gives identical shadows at noon
 * and at sunset -- which is exactly backwards, and reads as the clouds being
 * lit oddly rather than as the geometry being wrong. */

static void test_low_sun_travels_through_more_cloud(void)
{
    float *high = malloc(sizeof(float) * RES * RES);
    float *low  = malloc(sizeof(float) * RES * RES);
    TEST_ASSERT_NOT_NULL(high);
    TEST_ASSERT_NOT_NULL(low);

    JceCloudShadowDesc d = base_desc();
    TEST_ASSERT_TRUE(jce_cloud_shadow_bake(&d, high));   /* straight up */

    const float elev = 0.35f;                            /* ~20 degrees */
    d.sun_dir[0] = cosf(elev);
    d.sun_dir[1] = sinf(elev);
    d.sun_dir[2] = 0.0f;
    TEST_ASSERT_TRUE(jce_cloud_shadow_bake(&d, low));

    /* Compare OPTICAL DEPTH, not transmittance, and compare its RATIO.
     *
     * "the low sun is darker" is too weak: a slanted ray also samples
     * different noise, so it can come out darker for the wrong reason and the
     * test passes while the geometry is wrong.  The path through a slab of
     * thickness t at elevation e is t/sin(e), so the mean optical depth must
     * scale by 1/sin(e) -- 2.92x here.  A version that used the thickness
     * directly gives a ratio near 1, and no accident of the noise field
     * produces 2.9. */
    double th = 0.0, tl = 0.0;
    for (uint32_t i = 0; i < RES * RES; ++i) {
        th += -log((double)high[i] + 1e-9);
        tl += -log((double)low[i]  + 1e-9);
    }
    TEST_ASSERT_TRUE(th > 1e-6);
    const double ratio    = tl / th;
    const double expected = 1.0 / (double)sinf(elev);
    TEST_ASSERT_TRUE_MESSAGE(ratio > expected * 0.6,
        "low-sun path length does not scale with 1/sin(elevation)");
    TEST_ASSERT_TRUE_MESSAGE(ratio < expected * 1.6, "path length overshoots");

    free(high);
    free(low);
}

/* ── 3b. Texels are sampled at their CENTRES ───────────────────────────
 *
 * A half-texel offset shifts the whole map by metres on the ground: the cloud
 * and its shadow stay a constant distance apart, which reads as wind rather
 * than as an indexing error, and it survives every check that only looks at
 * the map's statistics.
 *
 * The discriminator: a 1x1 bake sampled at the CENTRE reads the same world
 * point no matter how wide the map is, so two bakes at different extents must
 * agree exactly.  Sampled at the corner it reads (centre - extent/2), which
 * moves when the extent changes. */

static void test_bake_samples_texel_centres(void)
{
    JceCloudShadowDesc d = base_desc();
    d.resolution = 1u;
    d.center_x   = 1000.0f;
    d.center_z   = 2000.0f;

    float narrow = 0.0f, wide = 0.0f;
    d.world_extent_m = 4000.0f;
    TEST_ASSERT_TRUE(jce_cloud_shadow_bake(&d, &narrow));
    d.world_extent_m = 9000.0f;
    TEST_ASSERT_TRUE(jce_cloud_shadow_bake(&d, &wide));

    TEST_ASSERT_FLOAT_WITHIN(1e-5f, narrow, wide);
}

/* ── 4. Degenerate input is REFUSED, and leaves the buffer alone ───────
 *
 * The caller keeps last frame's map. Writing zeros would black out the world;
 * writing ones would claim clear sky under an overcast one. Both are valid
 * pictures, which is why neither would be reported. */

static void test_degenerate_input_refuses_without_writing(void)
{
    float *map = malloc(sizeof(float) * RES * RES);
    TEST_ASSERT_NOT_NULL(map);
    for (uint32_t i = 0; i < RES * RES; ++i) map[i] = 0.5f;

    JceCloudShadowDesc d = base_desc();

    d.resolution = 0u;
    TEST_ASSERT_FALSE(jce_cloud_shadow_bake(&d, map));
    d = base_desc();

    d.world_extent_m = 0.0f;
    TEST_ASSERT_FALSE(jce_cloud_shadow_bake(&d, map));
    d = base_desc();

    d.layer_top_m = d.layer_bottom_m;          /* zero-thickness slab */
    TEST_ASSERT_FALSE(jce_cloud_shadow_bake(&d, map));
    d = base_desc();

    d.sun_dir[0] = d.sun_dir[1] = d.sun_dir[2] = 0.0f;
    TEST_ASSERT_FALSE(jce_cloud_shadow_bake(&d, map));
    d = base_desc();

    /* Sun at/below the horizon: the ray runs sideways through an unbounded
     * slab and the integral diverges toward total darkness.  Refusing is what
     * stops the world staying black at dawn. */
    d.sun_dir[0] = 1.0f; d.sun_dir[1] = 0.0f; d.sun_dir[2] = 0.0f;
    TEST_ASSERT_FALSE(jce_cloud_shadow_bake(&d, map));

    TEST_ASSERT_FALSE(jce_cloud_shadow_bake(NULL, map));
    d = base_desc();
    TEST_ASSERT_FALSE(jce_cloud_shadow_bake(&d, NULL));

    /* Nothing was written by any of those. */
    for (uint32_t i = 0; i < RES * RES; ++i)
        TEST_ASSERT_EQUAL_FLOAT(0.5f, map[i]);

    free(map);
}

/* ── 5. Sampling: inside interpolates, outside is full sun ─────────────  */

static void test_sample_interpolates_inside_and_is_clear_outside(void)
{
    /* A hand-built 2x2 map so the expected values are arithmetic, not a
     * property of the noise field. */
    float m[4] = { 0.0f, 1.0f,
                   0.0f, 1.0f };
    const float ext = 100.0f;   /* -50..50, texel 50, centres at -25 and +25 */

    /* At a texel centre the value is exact. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f,
        jce_cloud_shadow_sample(m, 2u, ext, 0.0f, 0.0f, -25.0f, -25.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f,
        jce_cloud_shadow_sample(m, 2u, ext, 0.0f, 0.0f, 25.0f, -25.0f));
    /* Halfway between them: the average. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f,
        jce_cloud_shadow_sample(m, 2u, ext, 0.0f, 0.0f, 0.0f, -25.0f));

    /* Outside is FULL SUN, not the clamped edge value.  With a dark edge
     * texel, clamping would shadow the entire rest of the world. */
    TEST_ASSERT_EQUAL_FLOAT(1.0f,
        jce_cloud_shadow_sample(m, 2u, ext, 0.0f, 0.0f, -5000.0f, 0.0f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f,
        jce_cloud_shadow_sample(m, 2u, ext, 0.0f, 0.0f, 0.0f, 5000.0f));

    /* And the map follows its centre: the same world point reads differently
     * once the map moves, or the shadow would be pinned to the origin while
     * the camera flew away from it. */
    const float at_origin = jce_cloud_shadow_sample(m, 2u, ext, 0.0f, 0.0f,
                                                    -25.0f, 0.0f);
    const float shifted   = jce_cloud_shadow_sample(m, 2u, ext, 50.0f, 0.0f,
                                                    -25.0f, 0.0f);
    TEST_ASSERT_TRUE(fabsf(at_origin - shifted) > 0.1f);

    TEST_ASSERT_EQUAL_FLOAT(1.0f,
        jce_cloud_shadow_sample(NULL, 2u, ext, 0.0f, 0.0f, 0.0f, 0.0f));
}

/* ── The map must not saturate at the SHIPPING parameters ──────────────
 *
 * A binary map is a valid texture, it multiplies cleanly, and it renders as
 * "the clouds are thick today". It is also the failure this whole term is
 * most likely to have, because extinction enters as exp(-sigma * density *
 * path) and the path through a 2.8 km slab at a 34-degree sun is 5 km: an
 * extinction off by even one order of magnitude sends every non-empty column
 * to exactly zero and every empty one to exactly one.
 *
 * It happened. The renderer multiplied the authored cloudDensity by an
 * undocumented 0.46 to reach a per-metre extinction, while the sky march
 * integrates the same density per KILOMETRE -- a factor of 460. At the
 * shipped cloudDensity of 1.15 that gave an optical depth of 132 at the
 * THINNEST density the noise field produces, and exp(-132) is zero in float.
 * The symptom was not a black world: it was that replacing the shadow's
 * coverage with the sky's full 2-D weather field moved the ground by 0.0048
 * grey levels, which is the build-to-build compiler noise. A saturated map
 * cannot show what it is saturated by, so the defect hid inside a
 * measurement that looked like "no visible difference".
 *
 * The assertion is on the DISTRIBUTION, not on any single value, because the
 * correct extinction is a physical quantity and this test must not become a
 * second place that defines it. */
static void test_shipping_parameters_do_not_saturate(void)
{
    JceCloudShadowDesc d = base_desc();
    /* hidden_cove: cloudDensity 1.15, layer 1.4-4.2 km, sun 34 degrees up. */
    JceCloudNoiseParams np = noise_with_bias(-0.2f + 0.82f * 0.8f);
    d.noise          = &np;
    d.extinction     = 1.15f * 0.001f;
    d.layer_bottom_m = 1400.0f;
    d.layer_top_m    = 4200.0f;
    d.sun_dir[0]     = -0.1154f;
    d.sun_dir[1]     =  0.5592f;
    d.sun_dir[2]     = -0.8210f;
    d.world_extent_m = 1040.0f;

    float *map = malloc(sizeof(float) * RES * RES);
    TEST_ASSERT_NOT_NULL(map);
    TEST_ASSERT_TRUE(jce_cloud_shadow_bake(&d, map));

    uint32_t mid = 0;
    for (uint32_t i = 0; i < RES * RES; ++i)
        if (map[i] > 0.02f && map[i] < 0.98f) mid++;

    /* A tenth of the map in the middle of the range is a low bar that a
     * saturated map cannot clear: at 460x too much extinction, `mid` is 0. */
    char msg[128];
    snprintf(msg, sizeof msg,
             "cloud shadow saturated: only %u of %u texels are partial",
             mid, RES * RES);
    TEST_ASSERT_TRUE_MESSAGE(mid * 10u >= RES * RES, msg);

    free(map);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bake_varies_and_stays_in_range);
    RUN_TEST(test_more_coverage_means_less_light);
    RUN_TEST(test_shipping_parameters_do_not_saturate);
    RUN_TEST(test_low_sun_travels_through_more_cloud);
    RUN_TEST(test_bake_samples_texel_centres);
    RUN_TEST(test_degenerate_input_refuses_without_writing);
    RUN_TEST(test_sample_interpolates_inside_and_is_clear_outside);
    return UNITY_END();
}
