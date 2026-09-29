/*
 * test_jce_hashed_alpha.c
 *
 * Stochastic alpha testing.
 *
 * The property that makes this correct is STATISTICAL: the fraction of
 * fragments surviving the threshold must equal the alpha value. A shader
 * cannot be asked that question, and every way of getting it wrong still
 * renders foliage -- slightly too thin, slightly too thick, or crawling --
 * which reads as an art problem. So it is pinned here.
 */

#include "jce_hashed_alpha.h"

#include <math.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* ── 1. The hash is uniform in [0,1) ───────────────────────────────────
 *
 * Everything downstream assumes it. A hash biased toward either end makes the
 * surviving fraction stop matching alpha, and the canopy gains or loses
 * density with distance. */

static void test_hash_is_uniform(void)
{
    enum { BUCKETS = 10, N = 40 };
    int hist[BUCKETS] = { 0 };
    int total = 0;

    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j)
            for (int k = 0; k < N; ++k) {
                const float v = jce_hashed_alpha_hash3((float)i * 1.37f,
                                                       (float)j * 0.91f,
                                                       (float)k * 2.13f);
                TEST_ASSERT_TRUE(v >= 0.0f && v < 1.0f);
                int b = (int)(v * BUCKETS);
                if (b >= BUCKETS) b = BUCKETS - 1;
                hist[b]++;
                total++;
            }

    const double expect = (double)total / BUCKETS;
    for (int b = 0; b < BUCKETS; ++b) {
        const double ratio = hist[b] / expect;
        TEST_ASSERT_TRUE_MESSAGE(ratio > 0.75 && ratio < 1.25,
                                 "hash is not uniform - coverage will not match alpha");
    }
}

/* ── 2. THE POINT: surviving coverage equals alpha ─────────────────────
 *
 * This is the whole technique. If the threshold distribution is not uniform,
 * the surviving fraction drifts from alpha and foliage systematically thins or
 * thickens -- smoothly, everywhere, in a way no one attributes to the alpha
 * test. The triangular-to-uniform CDF in the implementation exists only for
 * this, and without it the error peaks near lerp_t = 0.5. */

static void test_coverage_matches_alpha(void)
{
    const float alphas[5] = { 0.15f, 0.35f, 0.5f, 0.7f, 0.9f };

    /* Sweep the pixel derivative so lerp_t sweeps its whole range, including
     * the midpoint where an uncorrected distribution is worst. */
    for (int ai = 0; ai < 5; ++ai) {
        int pass = 0, total = 0;
        for (int d = 0; d < 12; ++d) {
            const float deriv = 0.01f * powf(1.7f, (float)d);
            for (int i = 0; i < 24; ++i)
                for (int j = 0; j < 24; ++j) {
                    const float t = jce_hashed_alpha_threshold(
                        (float)i * 0.31f, (float)j * 0.27f, 3.5f, deriv, 1.0f);
                    TEST_ASSERT_TRUE(t > 0.0f && t <= 1.0f);
                    if (alphas[ai] >= t) pass++;
                    total++;
                }
        }
        const double coverage = (double)pass / (double)total;
        TEST_ASSERT_TRUE_MESSAGE(fabs(coverage - alphas[ai]) < 0.06,
            "surviving coverage does not match alpha - foliage will thin or thicken");
    }
}

/* ── 3. The threshold is stable under object motion ────────────────────
 *
 * The hash keys on OBJECT space precisely so the wind cannot move it. Sampling
 * the same object-space point must give the same threshold no matter where the
 * object is in the world -- a world-space hash would resample as the leaf
 * sways, and the stipple would crawl across the surface. */

static void test_threshold_is_object_space_stable(void)
{
    const float deriv = 0.05f;
    const float a = jce_hashed_alpha_threshold(1.25f, 0.5f, -2.0f, deriv, 1.0f);
    const float b = jce_hashed_alpha_threshold(1.25f, 0.5f, -2.0f, deriv, 1.0f);
    TEST_ASSERT_EQUAL_FLOAT(a, b);

    /* A different point gives a different threshold -- otherwise the "hash"
     * is a constant and the whole surface flips at once, which is the popping
     * this replaces. */
    const float c = jce_hashed_alpha_threshold(1.30f, 0.5f, -2.0f, deriv, 1.0f);
    TEST_ASSERT_TRUE(fabsf(a - c) > 1e-6f);
}

/* ── 4. Never exactly zero, never above one ────────────────────────────
 *
 * A zero threshold passes a fully transparent fragment: one opaque dot in a
 * leaf's empty margin, scattered across the canopy, attributed to anything but
 * the alpha test. */

static void test_threshold_bounds(void)
{
    for (int d = 0; d < 40; ++d) {
        const float deriv = 1e-4f * powf(2.0f, (float)d * 0.5f);
        for (int i = 0; i < 30; ++i) {
            const float t = jce_hashed_alpha_threshold((float)i * 7.3f, 2.0f,
                                                       -11.0f, deriv, 0.7f);
            TEST_ASSERT_TRUE(isfinite(t));
            TEST_ASSERT_TRUE_MESSAGE(t > 0.0f, "zero threshold passes a transparent fragment");
            TEST_ASSERT_TRUE(t <= 1.0f);
        }
    }

    /* The reachable route to a zero threshold: a non-finite object-space
     * position -- a corrupt transform, or a degenerate vertex.  The hash
     * resolves NaN to 0, both discretised samples come back 0, and the CDF's
     * lower branch takes that straight to 0.  Random sampling never lands
     * there, which is exactly why the clamp needs an explicit test: without
     * it, one bad vertex draws opaque dots through a leaf's empty margin and
     * the alpha test is the last thing anyone would suspect. */
    const float bad = nanf("");
    float t = jce_hashed_alpha_threshold(bad, bad, bad, 0.05f, 1.0f);
    TEST_ASSERT_TRUE(isfinite(t));
    TEST_ASSERT_TRUE_MESSAGE(t > 0.0f,
        "a non-finite position yields a zero threshold - transparent fragments survive");

    t = jce_hashed_alpha_threshold(HUGE_VALF, 0.0f, 0.0f, 0.05f, 1.0f);
    TEST_ASSERT_TRUE(isfinite(t));
    TEST_ASSERT_TRUE(t > 0.0f && t <= 1.0f);
}

/* ── 5. Degenerate input falls back to a plain alpha test ──────────────
 *
 * No usable derivative means no way to hold the noise at a constant screen
 * size, and unscaled object-space noise aliases into per-pixel static -- which
 * looks like a broken texture. A 0.5 cutoff is a defensible picture. */

static void test_degenerate_input_is_a_plain_cutoff(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.5f,
        jce_hashed_alpha_threshold(1.0f, 2.0f, 3.0f, 0.0f, 1.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.5f,
        jce_hashed_alpha_threshold(1.0f, 2.0f, 3.0f, -1.0f, 1.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.5f,
        jce_hashed_alpha_threshold(1.0f, 2.0f, 3.0f, 0.05f, 0.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.5f,
        jce_hashed_alpha_threshold(1.0f, 2.0f, 3.0f, nanf(""), 1.0f));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hash_is_uniform);
    RUN_TEST(test_coverage_matches_alpha);
    RUN_TEST(test_threshold_is_object_space_stable);
    RUN_TEST(test_threshold_bounds);
    RUN_TEST(test_degenerate_input_is_a_plain_cutoff);
    return UNITY_END();
}
