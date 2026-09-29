/* test_jce_rand.c
 *
 * Unit tests for the PCG32 RNG (jce_rand):
 *   - determinism: same (seed,seq) reproduces the same stream
 *   - distinct seeds / streams diverge
 *   - f32 in [0,1); range_f within bounds
 *   - range_i inclusive bounds covered, never out of range
 *   - chance(0)/chance(1) edge behaviour
 *   - rough uniformity (mean ~0.5)
 */

#include <jce/os/core/jce_rand.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_determinism(void)
{
    JceRng a, b;
    jce_rng_seed(&a, 12345u, 1u);
    jce_rng_seed(&b, 12345u, 1u);
    for (int i = 0; i < 100; ++i)
        TEST_ASSERT_EQUAL_UINT32(jce_rng_u32(&a), jce_rng_u32(&b));
}

static void test_streams_diverge(void)
{
    JceRng a, b, c;
    jce_rng_seed(&a, 1u, 1u);
    jce_rng_seed(&b, 2u, 1u);   /* different seed */
    jce_rng_seed(&c, 1u, 2u);   /* different stream */
    uint32_t va = jce_rng_u32(&a), vb = jce_rng_u32(&b), vc = jce_rng_u32(&c);
    TEST_ASSERT_TRUE(va != vb);
    TEST_ASSERT_TRUE(va != vc);
}

static void test_f32_range(void)
{
    JceRng r; jce_rng_seed(&r, 7u, 1u);
    for (int i = 0; i < 10000; ++i) {
        float f = jce_rng_f32(&r);
        TEST_ASSERT_TRUE(f >= 0.0f && f < 1.0f);
    }
    jce_rng_seed(&r, 7u, 1u);
    for (int i = 0; i < 10000; ++i) {
        float f = jce_rng_range_f(&r, -3.0f, 5.0f);
        TEST_ASSERT_TRUE(f >= -3.0f && f < 5.0f);
    }
}

static void test_range_i(void)
{
    JceRng r; jce_rng_seed(&r, 99u, 3u);
    bool seen_lo = false, seen_hi = false;
    for (int i = 0; i < 5000; ++i) {
        int v = jce_rng_range_i(&r, 10, 12);   /* {10,11,12} */
        TEST_ASSERT_TRUE(v >= 10 && v <= 12);
        if (v == 10) seen_lo = true;
        if (v == 12) seen_hi = true;
    }
    TEST_ASSERT_TRUE(seen_lo);                  /* inclusive low  */
    TEST_ASSERT_TRUE(seen_hi);                  /* inclusive high */
    /* Degenerate range returns lo. */
    TEST_ASSERT_EQUAL_INT(5, jce_rng_range_i(&r, 5, 5));
    TEST_ASSERT_EQUAL_INT(5, jce_rng_range_i(&r, 5, 1));
}

static void test_chance_edges(void)
{
    JceRng r; jce_rng_seed(&r, 4u, 4u);
    for (int i = 0; i < 100; ++i) {
        TEST_ASSERT_FALSE(jce_rng_chance(&r, 0.0f));
        TEST_ASSERT_TRUE(jce_rng_chance(&r, 1.0f));
    }
}

static void test_rough_uniformity(void)
{
    JceRng r; jce_rng_seed(&r, 2024u, 1u);
    double sum = 0.0;
    const int N = 200000;
    for (int i = 0; i < N; ++i) sum += jce_rng_f32(&r);
    double mean = sum / N;
    TEST_ASSERT_TRUE(mean > 0.48 && mean < 0.52);   /* ~0.5 */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_determinism);
    RUN_TEST(test_streams_diverge);
    RUN_TEST(test_f32_range);
    RUN_TEST(test_range_i);
    RUN_TEST(test_chance_edges);
    RUN_TEST(test_rough_uniformity);
    return UNITY_END();
}
