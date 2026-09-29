/* test_jce_sr_radix_sort.c
 *
 * The renderer orders its primitive instance batch front-to-back before
 * submitting.  That ordering used to come from qsort with an indirect
 * comparator - 2.73 ms per frame at 26,656 instances, 89% of the whole flush
 * phase - and is now an LSD radix sort.  Replacing a sort is only safe if the
 * ORDER is identical in every respect that the renderer depends on, so these
 * lock the three properties that matter:
 *
 *   1. the emitted order is non-decreasing in the key (correctness),
 *   2. it is a PERMUTATION of the input (nothing dropped or duplicated - a
 *      dropped instance is a silently missing object on screen),
 *   3. it is STABLE (equal depths keep insertion order, so submit order is
 *      reproducible frame to frame; qsort did not guarantee this).
 *
 * The float-key helper gets its own tests: the renderer's key is a SQUARED
 * distance and therefore never negative, but the helper is written to handle
 * signed input, and a future caller passing a signed depth must not silently
 * get an inverted order.
 */
#include "../../../engine/src/middleware/scene/jce_sr_radix_sort.h"

#include "unity.h"
#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define MAXN 4096

static uint32_t g_keys[MAXN], g_idx[MAXN], g_scratch[MAXN * 2];
static uint32_t g_key_of[MAXN];   /* key by ORIGINAL index, for verification */

/* Deterministic PRNG - a fixed seed keeps failures reproducible. */
static uint32_t rng_state = 0x12345678u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void run_sort(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        g_idx[i]    = i;
        g_key_of[i] = g_keys[i];
    }
    jce_radix_sort_u32(g_keys, g_idx, n, g_scratch);
}

static void assert_sorted_permutation_stable(uint32_t n)
{
    /* 1. non-decreasing */
    for (uint32_t i = 1; i < n; i++)
        TEST_ASSERT_TRUE_MESSAGE(g_key_of[g_idx[i - 1]] <= g_key_of[g_idx[i]],
                                 "order is not non-decreasing");

    /* 2. a permutation: every original index appears exactly once */
    static uint8_t seen[MAXN];
    memset(seen, 0, n);
    for (uint32_t i = 0; i < n; i++) {
        TEST_ASSERT_TRUE_MESSAGE(g_idx[i] < n, "index out of range");
        TEST_ASSERT_FALSE_MESSAGE(seen[g_idx[i]], "index emitted twice");
        seen[g_idx[i]] = 1;
    }
    for (uint32_t i = 0; i < n; i++)
        TEST_ASSERT_TRUE_MESSAGE(seen[i], "index dropped");

    /* 3. stable: equal keys keep ascending original index */
    for (uint32_t i = 1; i < n; i++)
        if (g_key_of[g_idx[i - 1]] == g_key_of[g_idx[i]])
            TEST_ASSERT_TRUE_MESSAGE(g_idx[i - 1] < g_idx[i],
                                     "equal keys reordered (not stable)");

    /* 4. the permuted key array the sort maintains agrees with the indices */
    for (uint32_t i = 0; i < n; i++)
        TEST_ASSERT_EQUAL_UINT32(g_key_of[g_idx[i]], g_keys[i]);
}

static void test_random_keys_full_range(void)
{
    const uint32_t n = 4096;
    for (uint32_t i = 0; i < n; i++) g_keys[i] = rng();
    run_sort(n);
    assert_sorted_permutation_stable(n);
}

static void test_ties_everywhere_exercise_stability(void)
{
    /* Only 8 distinct keys over 4096 entries: almost everything is a tie. */
    const uint32_t n = 4096;
    for (uint32_t i = 0; i < n; i++) g_keys[i] = (rng() & 7u) * 1000u;
    run_sort(n);
    assert_sorted_permutation_stable(n);
}

static void test_all_keys_equal_hits_the_uniform_byte_skip(void)
{
    /* Every radix pass sees a uniform byte and is skipped, so the sort must
     * return the input order untouched - and must not leave the result in the
     * scratch buffer (the skip changes the pass parity). */
    const uint32_t n = 1000;
    for (uint32_t i = 0; i < n; i++) g_keys[i] = 0xABCD1234u;
    run_sort(n);
    assert_sorted_permutation_stable(n);
    for (uint32_t i = 0; i < n; i++)
        TEST_ASSERT_EQUAL_UINT32(i, g_idx[i]);
}

static void test_low_byte_only_is_an_odd_pass_count(void)
{
    /* Keys differing ONLY in the low byte execute exactly one pass: an ODD
     * count, so a sort that assumed even parity would leave the answer in the
     * scratch half and return garbage. */
    const uint32_t n = 777;
    for (uint32_t i = 0; i < n; i++) g_keys[i] = 0x11223300u | (rng() & 0xFFu);
    run_sort(n);
    assert_sorted_permutation_stable(n);
}

static void test_high_byte_only(void)
{
    const uint32_t n = 999;
    for (uint32_t i = 0; i < n; i++) g_keys[i] = (rng() & 0xFFu) << 24;
    run_sort(n);
    assert_sorted_permutation_stable(n);
}

static void test_already_sorted_and_reversed(void)
{
    const uint32_t n = 2048;
    for (uint32_t i = 0; i < n; i++) g_keys[i] = i * 3u;
    run_sort(n);
    assert_sorted_permutation_stable(n);

    for (uint32_t i = 0; i < n; i++) g_keys[i] = (n - i) * 3u;
    run_sort(n);
    assert_sorted_permutation_stable(n);
}

static void test_degenerate_sizes_are_no_ops(void)
{
    g_keys[0] = 42u; g_idx[0] = 7u;
    jce_radix_sort_u32(g_keys, g_idx, 0u, g_scratch);
    TEST_ASSERT_EQUAL_UINT32(7u, g_idx[0]);
    jce_radix_sort_u32(g_keys, g_idx, 1u, g_scratch);
    TEST_ASSERT_EQUAL_UINT32(7u, g_idx[0]);

    /* NULL arguments must not crash. */
    jce_radix_sort_u32(NULL, g_idx, 4u, g_scratch);
    jce_radix_sort_u32(g_keys, NULL, 4u, g_scratch);
    jce_radix_sort_u32(g_keys, g_idx, 4u, NULL);
}

/* ── float key helper ─────────────────────────────────────────────────── */

static void test_float_keys_order_like_the_floats(void)
{
    /* Includes the renderer's actual domain (squared distances: 0 and up) and
     * the signed values a future caller might pass. */
    static const float vals[] = {
        -1e30f, -1000.0f, -1.5f, -0.5f, -0.0f, 0.0f, 0.5f, 1.5f,
        3.0f, 1000.0f, 65504.0f, 1e30f,
    };
    const uint32_t n = (uint32_t)(sizeof vals / sizeof vals[0]);
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j < n; j++) {
            const uint32_t a = jce_radix_key_from_float(vals[i]);
            const uint32_t b = jce_radix_key_from_float(vals[j]);
            if (vals[i] < vals[j])
                TEST_ASSERT_TRUE_MESSAGE(a < b, "float order not preserved");
            else if (vals[i] > vals[j])
                TEST_ASSERT_TRUE_MESSAGE(a > b, "float order not preserved");
        }
    /* -0.0 and +0.0 compare equal as floats; the keys must not claim an order
     * that the float comparison does not have. */
    TEST_ASSERT_EQUAL_UINT32(jce_radix_key_from_float(0.0f),
                             jce_radix_key_from_float(0.0f));
}

static void test_sorting_squared_distances_matches_float_order(void)
{
    /* End to end on the renderer's real key: non-negative squared distances. */
    const uint32_t n = 3000;
    static float dist2[MAXN];
    for (uint32_t i = 0; i < n; i++) {
        const float d = (float)(rng() % 100000u) * 0.125f;
        dist2[i] = d * d;
        g_keys[i] = jce_radix_key_from_float(dist2[i]);
    }
    run_sort(n);
    assert_sorted_permutation_stable(n);
    for (uint32_t i = 1; i < n; i++)
        TEST_ASSERT_TRUE_MESSAGE(dist2[g_idx[i - 1]] <= dist2[g_idx[i]],
                                 "float depths not front-to-back");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_random_keys_full_range);
    RUN_TEST(test_ties_everywhere_exercise_stability);
    RUN_TEST(test_all_keys_equal_hits_the_uniform_byte_skip);
    RUN_TEST(test_low_byte_only_is_an_odd_pass_count);
    RUN_TEST(test_high_byte_only);
    RUN_TEST(test_already_sorted_and_reversed);
    RUN_TEST(test_degenerate_sizes_are_no_ops);
    RUN_TEST(test_float_keys_order_like_the_floats);
    RUN_TEST(test_sorting_squared_distances_matches_float_order);
    return UNITY_END();
}
