/*
 * test_jce_cloud_coverage.c
 *
 * The coverage control has to reach the field.
 *
 * There are two coverage-shaped knobs in this system: the bake's own
 * weather_coverage_bias, which decides how much cloud EXISTS, and a threshold
 * the sky shader applies afterwards. The renderer wired only the second, baking
 * every scene at the default bias of 0.0 -- which leaves 13.8% of the volume
 * non-zero with a peak density of 0.46. Thresholding that almost-empty field
 * then removed most of what was left, so the slider did nothing until it
 * approached 1 and then produced wisps. Raising density could not compensate:
 * 10x density measured 2.7x effect, because there was nothing there to multiply.
 *
 * These assertions are about the bias, because that is the end of the chain
 * that was never connected.
 */

#include "renderer/jce_cloud_noise.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

/* The renderer's atlas shape (SR_CLOUD_* in jce_sr_environment.c). */
#define DIM_X 64u
#define DIM_Y 32u
#define DIM_Z 32u
#define TILES_X 8u

void setUp(void)    {}
void tearDown(void) {}

/* Bake at a bias and report what fraction of the volume holds any cloud. */
static double occupancy_at(float bias, double *out_mean, double *out_max)
{
    uint32_t aw = 0, ah = 0;
    TEST_ASSERT_TRUE(jce_cloud_noise_atlas_size(DIM_X, DIM_Y, DIM_Z, TILES_X, &aw, &ah));
    float *atlas = (float *)malloc((size_t)aw * ah * sizeof(float));
    TEST_ASSERT_NOT_NULL(atlas);
    memset(atlas, 0, (size_t)aw * ah * sizeof(float));

    JceCloudNoiseParams cp;
    jce_cloud_noise_params_default(&cp);
    cp.weather_coverage_bias = bias;
    TEST_ASSERT_TRUE(jce_cloud_noise_bake_atlas(&cp, atlas, DIM_X, DIM_Y, DIM_Z, TILES_X));

    size_t hit = 0, total = 0;
    double sum = 0.0, mx = 0.0;
    for (uint32_t k = 0; k < DIM_Z; ++k) {
        const uint32_t tx = k % TILES_X, ty = k / TILES_X;
        const float *s = atlas + (size_t)ty * DIM_Y * aw + (size_t)tx * DIM_X;
        for (uint32_t y = 0; y < DIM_Y; ++y)
            for (uint32_t x = 0; x < DIM_X; ++x) {
                const float v = s[(size_t)y * aw + x];
                TEST_ASSERT_TRUE(isfinite(v));
                TEST_ASSERT_TRUE(v >= 0.0f && v <= 1.0f);
                if (v > 0.001f) hit++;
                sum += v; if (v > mx) mx = v;
                total++;
            }
    }
    free(atlas);
    if (out_mean) *out_mean = sum / (double)total;
    if (out_max)  *out_max  = mx;
    return (double)hit / (double)total;
}

/* ── 1. THE POINT: the bias actually changes how much cloud there is ───
 *
 * If this ever goes flat, the coverage slider is disconnected again and the
 * only symptom is a sky that ignores it -- which reads as "the clouds are
 * broken" rather than as "one parameter is not being passed". */

static void test_bias_controls_occupancy(void)
{
    double m_lo = 0, m_hi = 0, x_lo = 0, x_hi = 0;
    const double lo = occupancy_at(-0.2f, &m_lo, &x_lo);
    const double hi = occupancy_at( 0.4f, &m_hi, &x_hi);

    TEST_ASSERT_TRUE_MESSAGE(hi > lo * 5.0,
        "coverage bias barely moves the field - the control is not connected");
    TEST_ASSERT_TRUE_MESSAGE(m_hi > m_lo * 5.0, "mean density does not follow bias");
    /* And the high end must produce genuinely dense cloud, not just more haze:
     * a peak of 0.46 (the old default) marches to a barely-visible layer. */
    TEST_ASSERT_TRUE_MESSAGE(x_hi > 0.7,
        "even at high coverage the field never gets dense - clouds stay wispy");
}

/* ── 2. Monotone across the useful span ────────────────────────────────
 *
 * A slider that increases cloud, then decreases it, is worse than one that does
 * nothing: it makes the parameter look random. */

static void test_occupancy_is_monotone(void)
{
    const float biases[] = { -0.2f, 0.0f, 0.2f, 0.4f };
    double prev = -1.0;
    for (int i = 0; i < 4; ++i) {
        const double occ = occupancy_at(biases[i], NULL, NULL);
        TEST_ASSERT_TRUE_MESSAGE(occ >= prev - 1e-6,
            "occupancy is not monotone in the coverage bias");
        prev = occ;
    }
}

/* ── 3. Fully clear stays clear ────────────────────────────────────────
 *
 * The renderer maps coverage 0 to bias -1.  If that still produced cloud, a
 * scene that asked for a clear sky would get weather. */

static void test_minimum_bias_is_clear(void)
{
    double mean = 0;
    const double occ = occupancy_at(-1.0f, &mean, NULL);
    TEST_ASSERT_TRUE_MESSAGE(occ < 0.01, "coverage 0 still produces cloud");
    TEST_ASSERT_TRUE(mean < 0.005);
}

/* ── 4. Still deterministic ────────────────────────────────────────────
 *
 * Same seed, same sky: recorded playbacks compare frames, so a bake that
 * varied run to run would show up as a flickering sky in a diff and nowhere
 * else. */

static void test_bake_is_deterministic(void)
{
    uint32_t aw = 0, ah = 0;
    TEST_ASSERT_TRUE(jce_cloud_noise_atlas_size(DIM_X, DIM_Y, DIM_Z, TILES_X, &aw, &ah));
    const size_t n = (size_t)aw * ah;

    float *a = (float *)malloc(n * sizeof(float));
    float *b = (float *)malloc(n * sizeof(float));
    TEST_ASSERT_NOT_NULL(a); TEST_ASSERT_NOT_NULL(b);
    memset(a, 0, n * sizeof(float));
    memset(b, 0, n * sizeof(float));

    JceCloudNoiseParams cp;
    jce_cloud_noise_params_default(&cp);
    cp.weather_coverage_bias = 0.3f;
    TEST_ASSERT_TRUE(jce_cloud_noise_bake_atlas(&cp, a, DIM_X, DIM_Y, DIM_Z, TILES_X));
    TEST_ASSERT_TRUE(jce_cloud_noise_bake_atlas(&cp, b, DIM_X, DIM_Y, DIM_Z, TILES_X));

    for (size_t i = 0; i < n; ++i)
        TEST_ASSERT_EQUAL_FLOAT(a[i], b[i]);
    free(a); free(b);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bias_controls_occupancy);
    RUN_TEST(test_occupancy_is_monotone);
    RUN_TEST(test_minimum_bias_is_clear);
    RUN_TEST(test_bake_is_deterministic);
    return UNITY_END();
}
