/*
 * test_jce_read_bounds.c  Overflow-safe bounds primitives (L3 / resource).
 *
 * Regression guard for the untrusted-input integer-overflow class:
 * cooked .pak / .jceasset readers validated `offset + size <= limit` and
 * `count * elem` with naive arithmetic that wraps in uint64/size_t, letting a
 * crafted near-max offset/count pass the check and dereference out of bounds
 * (audit 2026-06-13, R2 archive/asset readers).  These primitives must reject
 * the wrapping case.
 */

#include "unity.h"

#include "resource/jce_read_bounds.h"

#include <stdint.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── jce_region_in_bounds: is [off, off+len) within a `limit`-byte buffer ── */

static void test_region_normal_cases(void)
{
    TEST_ASSERT_TRUE (jce_region_in_bounds(0,   100, 1000)); /* well inside   */
    TEST_ASSERT_TRUE (jce_region_in_bounds(900, 100, 1000)); /* exact tail    */
    TEST_ASSERT_TRUE (jce_region_in_bounds(1000,  0, 1000)); /* empty at end  */
    TEST_ASSERT_TRUE (jce_region_in_bounds(0,      0,   0)); /* empty/empty   */
    TEST_ASSERT_FALSE(jce_region_in_bounds(900, 101, 1000)); /* one past end  */
    TEST_ASSERT_FALSE(jce_region_in_bounds(1001,  0, 1000)); /* off past end  */
}

/* The whole point: off+len must not be computed naively (it wraps). */
static void test_region_rejects_additive_overflow(void)
{
    /* off+len wraps to 15 (< 1000) with naive math — must still be rejected. */
    TEST_ASSERT_FALSE(jce_region_in_bounds(UINT64_MAX - 16, 32, 1000));
    TEST_ASSERT_FALSE(jce_region_in_bounds(UINT64_MAX,       1, 1000));
    /* len alone wraps the sum back below limit. */
    TEST_ASSERT_FALSE(jce_region_in_bounds(8, UINT64_MAX, 1000));
}

/* ── jce_count_fits: do `count` elements of `elem` bytes fit in `avail`? ── */

static void test_count_normal_cases(void)
{
    TEST_ASSERT_TRUE (jce_count_fits(10, 32, 1000));  /* 320 <= 1000          */
    TEST_ASSERT_TRUE (jce_count_fits(0,  32, 0));     /* zero elements        */
    TEST_ASSERT_TRUE (jce_count_fits(100, 0, 0));     /* zero-size elements   */
    TEST_ASSERT_TRUE (jce_count_fits(31, 32, 1000));  /* 992 <= 1000 exact-ish*/
    TEST_ASSERT_FALSE(jce_count_fits(32, 32, 1000));  /* 1024 > 1000          */
}

/* count*elem must not be computed naively (it wraps). */
static void test_count_rejects_multiply_overflow(void)
{
    /* On any width, a huge count * elem wraps small with naive math. */
    TEST_ASSERT_FALSE(jce_count_fits(UINT64_MAX, 32, 1000));
    TEST_ASSERT_FALSE(jce_count_fits((UINT64_MAX / 8) + 1, 8, 64));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_region_normal_cases);
    RUN_TEST(test_region_rejects_additive_overflow);
    RUN_TEST(test_count_normal_cases);
    RUN_TEST(test_count_rejects_multiply_overflow);
    return UNITY_END();
}
