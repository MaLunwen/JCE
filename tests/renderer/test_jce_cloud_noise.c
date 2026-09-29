/* test_jce_cloud_noise.c
 *
 * Property tests for the CPU cloud density field.
 *
 * These assert INVARIANTS, not golden numbers, so the field can be re-tuned
 * (different bands, different default frequencies) without touching the tests,
 * while a broken implementation still fails:
 *   - density is always a finite value in [0,1], for hostile input included;
 *   - zero coverage is exactly empty, full coverage is substantially fuller;
 *   - the height gradient really bands by cloud type and never pops;
 *   - the field tiles seamlessly in X and Z;
 *   - the same inputs give bit-identical output;
 *   - detail erosion only removes density, and never touches the core;
 *   - bake writes every element and rejects degenerate dimensions;
 *   - NULL is tolerated everywhere.
 */

#include "jce_cloud_noise.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define GRID 8
#define GRID_VOXELS (GRID * GRID * GRID)

/* ── 1. defaults + NULL tolerance ───────────────────────────────────────── */

static void test_defaults_and_null_safety(void)
{
    JceCloudNoiseParams p;
    JceCloudWeather w;
    float d;

    jce_cloud_noise_params_default(NULL); /* must not crash */

    jce_cloud_noise_params_default(&p);
    TEST_ASSERT_TRUE(p.period_x > 0.0f);
    TEST_ASSERT_TRUE(p.period_y > 0.0f);
    TEST_ASSERT_TRUE(p.period_z > 0.0f);
    TEST_ASSERT_TRUE(p.base_octaves >= 1);
    TEST_ASSERT_TRUE(p.gain > 0.0f && p.gain < 1.0f);
    TEST_ASSERT_TRUE(p.lacunarity >= 1.0f);
    TEST_ASSERT_NULL(p.weather_fn);

    /* NULL params fall back to defaults instead of crashing. */
    d = jce_cloud_density(NULL, 12.0f, 300.0f, 7.0f, 0.3f, 1.0f, 0.5f);
    TEST_ASSERT_TRUE(d >= 0.0f && d <= 1.0f);
    TEST_ASSERT_TRUE(isfinite(d));

    d = jce_cloud_density_weather(NULL, 12.0f, 300.0f, 7.0f, 0.3f);
    TEST_ASSERT_TRUE(d >= 0.0f && d <= 1.0f);

    jce_cloud_weather_sample(NULL, 0.0f, 0.0f, &w);
    TEST_ASSERT_TRUE(w.coverage >= 0.0f && w.coverage <= 1.0f);
    TEST_ASSERT_TRUE(w.cloud_type >= 0.0f && w.cloud_type <= 1.0f);
    TEST_ASSERT_TRUE(w.precipitation >= 0.0f && w.precipitation <= 1.0f);

    jce_cloud_weather_sample(&p, 0.0f, 0.0f, NULL); /* must not crash */

    TEST_ASSERT_FALSE(jce_cloud_noise_bake(&p, NULL, 4u, 4u, 4u));

    /* remap must not divide by a collapsed window. */
    TEST_ASSERT_TRUE(isfinite(jce_cloud_remap(0.5f, 1.0f, 1.0f, 0.0f, 1.0f)));
}

/* ── 2. output is always finite and unit-ranged ─────────────────────────── */

static void test_density_is_finite_and_unit_range(void)
{
    JceCloudNoiseParams p, garbage;
    const float covs[3] = { 0.0f, 0.35f, 1.0f };
    const float types[3] = { 0.0f, 0.5f, 1.0f };
    int i, j, k, ci, ti;

    jce_cloud_noise_params_default(&p);

    for (k = 0; k < GRID; ++k) {
        /* Deliberately straddle the origin so negative coordinates are covered. */
        const float z = (((float)k + 0.5f) / (float)GRID - 0.5f) * p.period_z * 2.0f;
        for (j = 0; j < GRID; ++j) {
            const float h = ((float)j + 0.5f) / (float)GRID;
            const float y = h * p.period_y;
            for (i = 0; i < GRID; ++i) {
                const float x = (((float)i + 0.5f) / (float)GRID - 0.5f) * p.period_x * 2.0f;
                for (ci = 0; ci < 3; ++ci) {
                    for (ti = 0; ti < 3; ++ti) {
                        const float d = jce_cloud_density(&p, x, y, z, h, covs[ci], types[ti]);
                        TEST_ASSERT_TRUE(isfinite(d));
                        TEST_ASSERT_TRUE(d >= 0.0f && d <= 1.0f);
                    }
                }
            }
        }
    }

    /* Non-finite coordinates must be rejected, not propagated. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_cloud_density(&p, (float)INFINITY, 0.0f, 0.0f,
                                                    0.5f, 1.0f, 0.5f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_cloud_density(&p, 0.0f, (float)NAN, 0.0f,
                                                    0.5f, 1.0f, 0.5f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_cloud_density(&p, 0.0f, 0.0f, 0.0f,
                                                    (float)NAN, 1.0f, 0.5f));

    /* Out-of-range knobs are clamped, not rejected. */
    {
        const float d = jce_cloud_density(&p, 10.0f, 200.0f, 30.0f, -5.0f, 17.0f, -3.0f);
        TEST_ASSERT_TRUE(isfinite(d) && d >= 0.0f && d <= 1.0f);
    }

    /* An all-zero param block (zeroed periods, zero octaves, zero gain) must be
     * sanitised into something usable rather than dividing by zero. */
    memset(&garbage, 0, sizeof(garbage));
    {
        const float d = jce_cloud_density(&garbage, 10.0f, 200.0f, 30.0f, 0.3f, 1.0f, 0.5f);
        TEST_ASSERT_TRUE(isfinite(d) && d >= 0.0f && d <= 1.0f);
    }
    garbage.period_x = (float)NAN;
    garbage.lacunarity = (float)INFINITY;
    garbage.gain = -12.0f;
    garbage.base_octaves = 1000000;
    garbage.detail_strength = 5.0f;
    garbage.density_scale = (float)NAN;
    {
        const float d = jce_cloud_density(&garbage, 10.0f, 200.0f, 30.0f, 0.3f, 1.0f, 0.5f);
        TEST_ASSERT_TRUE(isfinite(d) && d >= 0.0f && d <= 1.0f);
    }
}

/* ── 3. zero coverage is exactly empty ──────────────────────────────────── */

static void test_zero_coverage_is_empty(void)
{
    JceCloudNoiseParams p;
    int i, j, k;

    jce_cloud_noise_params_default(&p);
    for (k = 0; k < GRID; ++k) {
        const float z = ((float)k / (float)GRID) * p.period_z;
        for (j = 0; j < GRID; ++j) {
            const float h = ((float)j + 0.5f) / (float)GRID;
            for (i = 0; i < GRID; ++i) {
                const float x = ((float)i / (float)GRID) * p.period_x;
                TEST_ASSERT_EQUAL_FLOAT(0.0f,
                    jce_cloud_density(&p, x, h * p.period_y, z, h, 0.0f, 0.5f));
                TEST_ASSERT_EQUAL_FLOAT(0.0f,
                    jce_cloud_density(&p, x, h * p.period_y, z, h, 0.0f, 1.0f));
            }
        }
    }
}

/* ── 4. coverage monotonically fills the sky ────────────────────────────── */

static void test_full_coverage_is_substantially_denser(void)
{
    JceCloudNoiseParams p;
    float sum_full = 0.0f, sum_low = 0.0f;
    int i, j, k, n = 0;

    jce_cloud_noise_params_default(&p);
    for (k = 0; k < GRID; ++k) {
        const float z = ((float)k / (float)GRID) * p.period_z;
        for (j = 0; j < GRID; ++j) {
            /* Restricted to the cumulus band so the height gradient is not the
             * thing being measured here. */
            const float h = 0.15f + 0.30f * ((float)j / (float)GRID);
            for (i = 0; i < GRID; ++i) {
                const float x = ((float)i / (float)GRID) * p.period_x;
                const float y = h * p.period_y;
                sum_full += jce_cloud_density(&p, x, y, z, h, 1.0f, 0.5f);
                sum_low  += jce_cloud_density(&p, x, y, z, h, 0.25f, 0.5f);
                ++n;
            }
        }
    }
    TEST_ASSERT_TRUE(n > 0);
    sum_full /= (float)n;
    sum_low  /= (float)n;

    TEST_ASSERT_TRUE(sum_full > 0.05f);          /* full coverage produces cloud */
    TEST_ASSERT_TRUE(sum_full > sum_low + 0.05f); /* and substantially more of it */
    TEST_ASSERT_TRUE(sum_low >= 0.0f);
}

/* ── 5. the height gradient bands by cloud type ─────────────────────────── */

static void test_height_gradient_bands_by_type(void)
{
    const float anvil = 0.5f;
    int i;

    /* Range and finiteness over the whole (height, type) domain. */
    for (i = 0; i <= 64; ++i) {
        const float h = (float)i / 64.0f;
        int t;
        for (t = 0; t <= 4; ++t) {
            const float g = jce_cloud_height_gradient(h, (float)t / 4.0f, anvil);
            TEST_ASSERT_TRUE(isfinite(g));
            TEST_ASSERT_TRUE(g >= 0.0f && g <= 1.0f);
        }
    }

    /* Stratus lives low and is gone by mid-layer. */
    TEST_ASSERT_TRUE(jce_cloud_height_gradient(0.10f, 0.0f, anvil) > 0.3f);
    TEST_ASSERT_TRUE(jce_cloud_height_gradient(0.50f, 0.0f, anvil) < 0.01f);
    TEST_ASSERT_TRUE(jce_cloud_height_gradient(0.90f, 0.0f, anvil) < 0.01f);

    /* Cumulus occupies the middle and does not reach the top. */
    TEST_ASSERT_TRUE(jce_cloud_height_gradient(0.35f, 0.5f, anvil) > 0.5f);
    TEST_ASSERT_TRUE(jce_cloud_height_gradient(0.95f, 0.5f, anvil) < 0.01f);

    /* Cumulonimbus is tall: still present near the top of the layer. */
    TEST_ASSERT_TRUE(jce_cloud_height_gradient(0.30f, 1.0f, anvil) > 0.5f);
    TEST_ASSERT_TRUE(jce_cloud_height_gradient(0.90f, 1.0f, anvil) > 0.3f);

    /* Blending across the type axis must be continuous -- an authored weather
     * map animates this value, and a step here shows up as a pop. */
    for (i = 1; i <= 256; ++i) {
        const float t0 = (float)(i - 1) / 256.0f;
        const float t1 = (float)i / 256.0f;
        int hi;
        for (hi = 0; hi <= 10; ++hi) {
            const float h = (float)hi / 10.0f;
            const float a = jce_cloud_height_gradient(h, t0, anvil);
            const float b = jce_cloud_height_gradient(h, t1, anvil);
            TEST_ASSERT_TRUE(fabsf(a - b) < 0.05f);
        }
    }

    /* The banding is visible in the density itself, not just the gradient. */
    {
        JceCloudNoiseParams p;
        float low_stratus = 0.0f, high_stratus = 0.0f, high_cb = 0.0f;
        int j;
        jce_cloud_noise_params_default(&p);
        for (j = 0; j < 32; ++j) {
            const float x = ((float)j / 32.0f) * p.period_x;
            const float z = ((float)((j * 5) % 32) / 32.0f) * p.period_z;
            low_stratus  += jce_cloud_density(&p, x, 0.10f * p.period_y, z, 0.10f, 1.0f, 0.0f);
            high_stratus += jce_cloud_density(&p, x, 0.85f * p.period_y, z, 0.85f, 1.0f, 0.0f);
            high_cb      += jce_cloud_density(&p, x, 0.85f * p.period_y, z, 0.85f, 1.0f, 1.0f);
        }
        TEST_ASSERT_TRUE(low_stratus > 0.0f);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, high_stratus);
        TEST_ASSERT_TRUE(high_cb > low_stratus * 0.25f);
    }
}

/* ── 6. seamless tiling in X and Z ──────────────────────────────────────── */

static void test_tiling_is_seamless(void)
{
    JceCloudNoiseParams p;
    int n;

    jce_cloud_noise_params_default(&p);
    for (n = 0; n < 24; ++n) {
        const float h = 0.15f + 0.6f * ((float)(n % 5) / 5.0f);
        const float x = ((float)n / 24.0f) * p.period_x + 3.5f;
        const float z = ((float)((n * 7) % 24) / 24.0f) * p.period_z + 11.25f;
        const float y = h * p.period_y;
        const float base  = jce_cloud_density(&p, x, y, z, h, 1.0f, 0.5f);
        const float wrapx = jce_cloud_density(&p, x + p.period_x, y, z, h, 1.0f, 0.5f);
        const float wrapz = jce_cloud_density(&p, x, y, z + p.period_z, h, 1.0f, 0.5f);
        const float farx  = jce_cloud_density(&p, x - 3.0f * p.period_x, y, z, h, 1.0f, 0.5f);

        /* Only float rounding in (x + period)/period may differ. */
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, base, wrapx);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, base, wrapz);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, base, farx);

        /* The components tile too, not just the composed density. */
        TEST_ASSERT_FLOAT_WITHIN(1e-3f,
            jce_cloud_perlin_worley(&p, x, y, z),
            jce_cloud_perlin_worley(&p, x + p.period_x, y, z + p.period_z));
    }
}

/* ── 7. determinism ─────────────────────────────────────────────────────── */

static void test_determinism_is_bit_exact(void)
{
    JceCloudNoiseParams p1, p2;
    float a[GRID_VOXELS];
    float b[GRID_VOXELS];
    float d0, d1;

    jce_cloud_noise_params_default(&p1);
    jce_cloud_noise_params_default(&p2);

    TEST_ASSERT_TRUE(jce_cloud_noise_bake(&p1, a, GRID, GRID, GRID));
    TEST_ASSERT_TRUE(jce_cloud_noise_bake(&p2, b, GRID, GRID, GRID));
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(a));

    /* A different seed must actually move the field -- otherwise the "identical"
     * result above would be vacuous. */
    p2.seed = p1.seed ^ 0x1234abcdu;
    TEST_ASSERT_TRUE(jce_cloud_noise_bake(&p2, b, GRID, GRID, GRID));
    TEST_ASSERT_TRUE(memcmp(a, b, sizeof(a)) != 0);

    /* Point sampling carries no state: interleaving other samples changes nothing. */
    d0 = jce_cloud_density(&p1, 101.0f, 250.0f, 37.0f, 0.4f, 0.7f, 0.3f);
    (void)jce_cloud_density(&p1, -9.0f, 12.0f, 3.0f, 0.9f, 0.2f, 0.8f);
    (void)jce_cloud_density_weather(&p1, 500.0f, 400.0f, 900.0f, 0.5f);
    d1 = jce_cloud_density(&p1, 101.0f, 250.0f, 37.0f, 0.4f, 0.7f, 0.3f);
    TEST_ASSERT_EQUAL_MEMORY(&d0, &d1, sizeof(d0));
}

/* ── 8. bake writes everything, rejects degenerate dims ─────────────────── */

static void test_bake_fills_buffer_and_rejects_bad_dims(void)
{
    JceCloudNoiseParams p;
    float vol[4 * 5 * 6];
    float atlas[12 * 10];
    uint32_t w = 0u, h = 0u;
    size_t i;

    jce_cloud_noise_params_default(&p);

    for (i = 0; i < sizeof(vol) / sizeof(vol[0]); ++i) {
        vol[i] = -1.0f; /* sentinel: density can never be negative */
    }
    TEST_ASSERT_TRUE(jce_cloud_noise_bake(&p, vol, 4u, 5u, 6u));
    for (i = 0; i < sizeof(vol) / sizeof(vol[0]); ++i) {
        TEST_ASSERT_TRUE(vol[i] >= 0.0f && vol[i] <= 1.0f);
    }

    TEST_ASSERT_FALSE(jce_cloud_noise_bake(&p, vol, 0u, 5u, 6u));
    TEST_ASSERT_FALSE(jce_cloud_noise_bake(&p, vol, 4u, 0u, 6u));
    TEST_ASSERT_FALSE(jce_cloud_noise_bake(&p, vol, 4u, 5u, 0u));
    TEST_ASSERT_FALSE(jce_cloud_noise_bake(&p, vol, 4096u, 4096u, 4096u));
    TEST_ASSERT_TRUE(jce_cloud_noise_bake(NULL, vol, 4u, 5u, 6u)); /* defaults */

    /* 2-D slice atlas: 6 slices, 3 per row => 12x10 with no padding. */
    TEST_ASSERT_TRUE(jce_cloud_noise_atlas_size(4u, 5u, 6u, 3u, &w, &h));
    TEST_ASSERT_EQUAL_UINT32(12u, w);
    TEST_ASSERT_EQUAL_UINT32(10u, h);
    for (i = 0; i < sizeof(atlas) / sizeof(atlas[0]); ++i) {
        atlas[i] = -1.0f;
    }
    TEST_ASSERT_TRUE(jce_cloud_noise_bake_atlas(&p, atlas, 4u, 5u, 6u, 3u));
    for (i = 0; i < sizeof(atlas) / sizeof(atlas[0]); ++i) {
        TEST_ASSERT_TRUE(atlas[i] >= 0.0f && atlas[i] <= 1.0f);
    }
    TEST_ASSERT_FALSE(jce_cloud_noise_bake_atlas(&p, atlas, 4u, 5u, 6u, 0u));
    TEST_ASSERT_FALSE(jce_cloud_noise_bake_atlas(&p, NULL, 4u, 5u, 6u, 3u));
    TEST_ASSERT_FALSE(jce_cloud_noise_atlas_size(4u, 5u, 6u, 3u, &w, NULL));
}

/* ── 9. erosion carves edges and leaves the core alone ──────────────────── */

static void test_detail_erosion_spares_the_core(void)
{
    JceCloudNoiseParams solid, eroded;
    int i, j, k;
    int core_samples = 0, eroded_samples = 0;

    jce_cloud_noise_params_default(&solid);
    solid.detail_strength = 0.0f;
    eroded = solid;
    eroded.detail_strength = 0.6f;

    for (k = 0; k < GRID; ++k) {
        const float z = ((float)k / (float)GRID) * solid.period_z;
        for (j = 0; j < GRID; ++j) {
            const float h = 0.15f + 0.35f * ((float)j / (float)GRID);
            for (i = 0; i < GRID; ++i) {
                const float x = ((float)i / (float)GRID) * solid.period_x;
                const float y = h * solid.period_y;
                const float a = jce_cloud_density(&solid, x, y, z, h, 1.0f, 0.5f);
                const float b = jce_cloud_density(&eroded, x, y, z, h, 1.0f, 0.5f);

                /* Erosion is subtractive: it can never add density. */
                TEST_ASSERT_TRUE(b <= a + 1e-6f);

                if (a >= 0.25f) {
                    /* Above the core onset the erosion weight is exactly zero,
                     * so the core must be untouched -- a uniform erosion would
                     * hollow the cloud out and this would fail. */
                    TEST_ASSERT_EQUAL_FLOAT(a, b);
                    ++core_samples;
                } else if (a > 0.01f && b < a - 1e-4f) {
                    ++eroded_samples;
                }
            }
        }
    }
    /* Both halves of the invariant must actually have been exercised. */
    TEST_ASSERT_TRUE(core_samples > 0);
    TEST_ASSERT_TRUE(eroded_samples > 0);
}

/* ── 10. caller weather maps, including hostile ones ────────────────────── */

static void weather_zero(void *user, float x, float z, JceCloudWeather *out)
{
    (void)user; (void)x; (void)z;
    out->coverage = 0.0f;
    out->cloud_type = 0.5f;
    out->precipitation = 0.0f;
}

static void weather_hostile(void *user, float x, float z, JceCloudWeather *out)
{
    (void)x; (void)z;
    /* The user pointer must arrive intact; the values must not. */
    if (user) {
        *(int *)user += 1;
    }
    out->coverage = 1e30f;
    out->cloud_type = -7.0f;
    out->precipitation = (float)NAN;
}

static void test_caller_weather_map_is_used_and_clamped(void)
{
    JceCloudNoiseParams p;
    JceCloudWeather w;
    int calls = 0;
    int i;

    jce_cloud_noise_params_default(&p);

    p.weather_fn = weather_zero;
    p.weather_user = NULL;
    for (i = 0; i < 16; ++i) {
        const float x = ((float)i / 16.0f) * p.period_x;
        TEST_ASSERT_EQUAL_FLOAT(0.0f,
            jce_cloud_density_weather(&p, x, 0.3f * p.period_y, x, 0.3f));
    }

    p.weather_fn = weather_hostile;
    p.weather_user = &calls;
    jce_cloud_weather_sample(&p, 12.0f, 34.0f, &w);
    TEST_ASSERT_TRUE(calls > 0);
    TEST_ASSERT_TRUE(w.coverage >= 0.0f && w.coverage <= 1.0f);
    TEST_ASSERT_TRUE(w.cloud_type >= 0.0f && w.cloud_type <= 1.0f);
    TEST_ASSERT_TRUE(w.precipitation >= 0.0f && w.precipitation <= 1.0f);

    for (i = 0; i < 16; ++i) {
        const float x = ((float)i / 16.0f) * p.period_x;
        const float d = jce_cloud_density_weather(&p, x, 0.3f * p.period_y, x, 0.3f);
        TEST_ASSERT_TRUE(isfinite(d));
        TEST_ASSERT_TRUE(d >= 0.0f && d <= 1.0f);
    }
}

/* ── Atlas addressing agrees with the volume it was folded from ────────
 *
 * A slice atlas is a 3-D volume folded into a 2-D image.  A consumer that
 * mis-addresses the fold still gets clouds -- just not the ones that were
 * baked -- and no screenshot reveals it.  So the check is the same shape as
 * the atmosphere LUT's: walk the atlas the way a shader will and require it to
 * match the volume bake.
 */

#define CA_X 16u
#define CA_Y 8u
#define CA_Z 8u
#define CA_TILES 4u

/* ── precipitation is read, and reads as rain ──────────────────────────
 *
 * JceCloudWeather.precipitation was written by cloud_weather_default and
 * consumed by NOTHING: every weather-sampling path passed coverage and type
 * down and let the third channel fall off the end. A carrier with no writer is
 * easy to spot; a carrier with a writer and no READER looks exactly like a
 * working feature from both sides.
 *
 * None of the other fourteen tests here can fail on that, because none of them
 * varies precipitation while holding the rest fixed. This one does exactly
 * that and nothing else: two maps identical in coverage and type, differing
 * only in rain. */

static void weather_dry(void *user, float x, float z, JceCloudWeather *out)
{
    (void)user; (void)x; (void)z;
    out->coverage = 0.65f;
    out->cloud_type = 0.35f;
    out->precipitation = 0.0f;
}

static void weather_wet(void *user, float x, float z, JceCloudWeather *out)
{
    (void)user; (void)x; (void)z;
    out->coverage = 0.65f;
    out->cloud_type = 0.35f;
    out->precipitation = 1.0f;      /* the ONLY difference */
}

static void test_precipitation_is_consumed(void)
{
    JceCloudNoiseParams p;
    jce_cloud_noise_params_default(&p);

    /* Sampled across a spread of altitudes and positions rather than at one
     * point: a rain cell is a change of SHAPE (the type axis moves toward
     * cumulonimbus, which is the tall gradient) as well as of density, so a
     * single probe can legitimately land where the two agree. */
    int denser = 0, sampled = 0, thinner = 0;
    double sum_dry = 0.0, sum_wet = 0.0;

    for (int i = 0; i < 30; i++) {
        const float x = 137.0f * (float)i;   /* 30 positions x 8 altitudes: the
                                              * default field at coverage 0.65
                                              * is sparse enough that 96 probes
                                              * found cloud at only 19. */
        const float z = 311.0f * (float)i;
        for (int j = 1; j < 9; j++) {
            const float h = (float)j / 9.0f;
            float dry, wet;

            p.weather_fn = weather_dry;
            dry = jce_cloud_density_weather(&p, x, h * p.period_y, z, h);
            p.weather_fn = weather_wet;
            wet = jce_cloud_density_weather(&p, x, h * p.period_y, z, h);

            TEST_ASSERT_TRUE(dry >= 0.0f && dry <= 1.0f);
            TEST_ASSERT_TRUE(wet >= 0.0f && wet <= 1.0f);

            sum_dry += dry;
            sum_wet += wet;
            if (dry > 0.0f) {
                sampled++;
                if (wet > dry) denser++;
                if (wet < dry) thinner++;
            }
        }
    }

    /* There has to BE cloud to compare, or the test proves nothing. */
    TEST_ASSERT_TRUE(sampled > 20);

    /* Rain makes the cell heavier on the whole. The per-sample count is the
     * part that would catch a sign error; the sum is the part that would catch
     * a change too small to matter. */
    TEST_ASSERT_TRUE(denser > thinner);
    TEST_ASSERT_TRUE(sum_wet > sum_dry * 1.10);

    p.weather_fn = NULL;
}

/* Zero precipitation must be the IDENTITY, not merely "close".
 *
 * Every scene that authors no rain -- which is every scene today -- has to
 * bake the field it baked before. A new parameter that shifts an untouched
 * default by a fraction of a percent is how a golden image stops being golden
 * for reasons nobody can name. */
static void test_no_rain_is_bit_identical(void)
{
    JceCloudNoiseParams p;
    jce_cloud_noise_params_default(&p);

    for (int i = 0; i < 16; i++) {
        const float x = 91.0f * (float)i;
        const float z = 173.0f * (float)i;
        const float h = (float)(i % 8 + 1) / 9.0f;

        /* The explicit-weather entry point takes no rain at all, so it is the
         * reference: whatever it returns is what the weather path must return
         * when the map says it is not raining. */
        const float ref = jce_cloud_density(&p, x, h * p.period_y, z, h,
                                            0.65f, 0.35f);
        p.weather_fn = weather_dry;
        const float got = jce_cloud_density_weather(&p, x, h * p.period_y, z, h);
        p.weather_fn = NULL;

        TEST_ASSERT_EQUAL_FLOAT(ref, got);
    }
}

static void test_atlas_addressing_matches_the_volume(void)
{
    JceCloudNoiseParams p;
    jce_cloud_noise_params_default(&p);

    float *vol = (float *)malloc(sizeof(float) * CA_X * CA_Y * CA_Z);
    TEST_ASSERT_NOT_NULL(vol);
    TEST_ASSERT_TRUE(jce_cloud_noise_bake(&p, vol, CA_X, CA_Y, CA_Z));

    uint32_t aw = 0u, ah = 0u;
    TEST_ASSERT_TRUE(jce_cloud_noise_atlas_size(CA_X, CA_Y, CA_Z, CA_TILES,
                                                &aw, &ah));
    float *atlas = (float *)malloc(sizeof(float) * aw * ah);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_TRUE(jce_cloud_noise_bake_atlas(&p, atlas, CA_X, CA_Y, CA_Z,
                                                CA_TILES));

    /* Every voxel, addressed through the atlas UV, must return that voxel.
     * Sampling at texel centres means the bilinear tap lands exactly on one
     * texel, so this is an equality test, not a tolerance one. */
    double worst = 0.0;
    for (uint32_t z = 0; z < CA_Z; z++) {
        for (uint32_t y = 0; y < CA_Y; y++) {
            for (uint32_t x = 0; x < CA_X; x++) {
                const float x01 = (CA_X > 1u) ? (float)x / (float)(CA_X - 1u) : 0.0f;
                const float y01 = (CA_Y > 1u) ? (float)y / (float)(CA_Y - 1u) : 0.0f;
                const float z01 = ((float)z + 0.5f) / (float)CA_Z;

                float u, v;
                jce_cloud_atlas_uv(CA_X, CA_Y, CA_Z, CA_TILES, x01, y01, z01,
                                   &u, &v);
                const float got = jce_cloud_atlas_sample(atlas, aw, ah, u, v);
                const float want = vol[(size_t)(z * CA_Y + y) * CA_X + x];
                const double d = fabs((double)got - want);
                if (d > worst) worst = d;
            }
        }
    }
    TEST_ASSERT_TRUE(worst < 1e-5);

    /* The field must not be constant, or the agreement above would be
     * satisfied by any addressing at all. */
    float lo = 1e9f, hi = -1e9f;
    for (uint32_t i = 0; i < CA_X * CA_Y * CA_Z; i++) {
        if (vol[i] < lo) lo = vol[i];
        if (vol[i] > hi) hi = vol[i];
    }
    TEST_ASSERT_TRUE(hi - lo > 0.01f);

    free(atlas);
    free(vol);
}

/* Slices must land in DIFFERENT tiles: a fold that put every slice in the same
 * place would still satisfy a per-slice equality test if the sampler made the
 * same mistake, so pin the layout itself. */
static void test_slices_occupy_distinct_tiles(void)
{
    float u0, v0, u1, v1, u4, v4;
    jce_cloud_atlas_uv(CA_X, CA_Y, CA_Z, CA_TILES, 0.5f, 0.5f,
                       0.5f / (float)CA_Z, &u0, &v0);      /* slice 0 */
    jce_cloud_atlas_uv(CA_X, CA_Y, CA_Z, CA_TILES, 0.5f, 0.5f,
                       1.5f / (float)CA_Z, &u1, &v1);      /* slice 1 */
    jce_cloud_atlas_uv(CA_X, CA_Y, CA_Z, CA_TILES, 0.5f, 0.5f,
                       4.5f / (float)CA_Z, &u4, &v4);      /* slice 4 */

    /* Slice 1 is the next COLUMN; slice 4 wraps to the next ROW (tiles_x=4). */
    TEST_ASSERT_TRUE(u1 > u0);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, v0, v1);
    TEST_ASSERT_TRUE(v4 > v0);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, u0, u4);
}

static void test_atlas_uv_null_safety(void)
{
    float u = -1.0f, v = -1.0f;
    jce_cloud_atlas_uv(0u, 8u, 8u, 4u, 0.5f, 0.5f, 0.5f, &u, &v);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, u);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v);
    jce_cloud_atlas_uv(8u, 8u, 8u, 4u, 0.5f, 0.5f, 0.5f, NULL, NULL);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_cloud_atlas_sample(NULL, 4u, 4u, 0.5f, 0.5f));
}

typedef struct ControlledBakeProbe {
    uint32_t calls;
    uint32_t completed;
    uint32_t total;
    uint32_t stop_after;
} ControlledBakeProbe;

static bool controlled_bake_progress(void *user, uint32_t completed,
                                     uint32_t total)
{
    ControlledBakeProbe *probe = (ControlledBakeProbe *)user;
    probe->calls++;
    probe->completed = completed;
    probe->total = total;
    return completed < probe->stop_after;
}

static void test_controlled_atlas_bake_cancels_at_slice_boundary(void)
{
    enum { X = 4, Y = 3, Z = 5, TILES = 2, W = 8, H = 9 };
    JceCloudNoiseParams p;
    ControlledBakeProbe probe = { 0u, 0u, 0u, 2u };
    float storage[W * H + 2];
    float *atlas = &storage[1];
    size_t i;

    jce_cloud_noise_params_default(&p);
    for (i = 0; i < W * H + 2u; ++i)
        storage[i] = -7.0f;

    TEST_ASSERT_FALSE(jce_cloud_noise_bake_atlas_controlled(
        &p, atlas, X, Y, Z, TILES, controlled_bake_progress, &probe));
    TEST_ASSERT_EQUAL_UINT32(3u, probe.calls);
    TEST_ASSERT_EQUAL_UINT32(2u, probe.completed);
    TEST_ASSERT_EQUAL_UINT32(Z, probe.total);
    TEST_ASSERT_EQUAL_FLOAT(-7.0f, storage[0]);
    TEST_ASSERT_EQUAL_FLOAT(-7.0f, storage[W * H + 1]);
    for (i = 0; i < W * H; ++i)
        TEST_ASSERT_TRUE(atlas[i] >= 0.0f && atlas[i] <= 1.0f);

    memset(&probe, 0, sizeof(probe));
    probe.stop_after = Z + 1u;
    TEST_ASSERT_TRUE(jce_cloud_noise_bake_atlas_controlled(
        &p, atlas, X, Y, Z, TILES, controlled_bake_progress, &probe));
    TEST_ASSERT_EQUAL_UINT32(Z + 1u, probe.calls);
    TEST_ASSERT_EQUAL_UINT32(Z, probe.completed);
}

/* ── March cost scales with the tier, and never exceeds the loop bound ──
 *
 * The step count is the one knob that decides whether clouds are affordable.
 * Two properties matter and neither is visible in a screenshot: a weaker tier
 * must get FEWER steps, and no tier may exceed the shader's compile-time loop
 * bound -- a budget above it would silently be ignored, so the machine that
 * asked for more quality would get exactly the bound and no warning. */

static void test_march_steps_scale_with_tier(void)
{
    const uint32_t low    = jce_cloud_march_steps(0);
    const uint32_t medium = jce_cloud_march_steps(1);
    const uint32_t high   = jce_cloud_march_steps(2);
    const uint32_t ultra  = jce_cloud_march_steps(3);

    TEST_ASSERT_TRUE(low < medium);
    TEST_ASSERT_TRUE(medium < high);
    TEST_ASSERT_TRUE(ultra >= high);

    /* Never above the shader's loop bound. */
    TEST_ASSERT_TRUE(high  <= JCE_CLOUD_MARCH_MAX_STEPS);
    TEST_ASSERT_TRUE(ultra <= JCE_CLOUD_MARCH_MAX_STEPS);

    /* Always at least one step: a zero budget would divide the slab by zero
     * when deriving the step length. */
    TEST_ASSERT_TRUE(low >= 1u);

    /* An unknown or corrupt tier must degrade to the CHEAPEST option, not the
     * most expensive -- a failed caps query should not hand a weak machine an
     * ultra-quality march. */
    TEST_ASSERT_EQUAL_UINT32(low, jce_cloud_march_steps(-1));
    TEST_ASSERT_EQUAL_UINT32(low, jce_cloud_march_steps(99));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_and_null_safety);
    RUN_TEST(test_density_is_finite_and_unit_range);
    RUN_TEST(test_zero_coverage_is_empty);
    RUN_TEST(test_full_coverage_is_substantially_denser);
    RUN_TEST(test_height_gradient_bands_by_type);
    RUN_TEST(test_tiling_is_seamless);
    RUN_TEST(test_determinism_is_bit_exact);
    RUN_TEST(test_bake_fills_buffer_and_rejects_bad_dims);
    RUN_TEST(test_detail_erosion_spares_the_core);
    RUN_TEST(test_caller_weather_map_is_used_and_clamped);
    RUN_TEST(test_precipitation_is_consumed);
    RUN_TEST(test_no_rain_is_bit_identical);
    RUN_TEST(test_atlas_addressing_matches_the_volume);
    RUN_TEST(test_slices_occupy_distinct_tiles);
    RUN_TEST(test_atlas_uv_null_safety);
    RUN_TEST(test_controlled_atlas_bake_cancels_at_slice_boundary);
    RUN_TEST(test_march_steps_scale_with_tier);
    return UNITY_END();
}
