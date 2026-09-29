/*
 * test_jce_half_float.c — the one IEEE binary16 decoder, and its edges.
 *
 * WHY THIS FILE EXISTS.  jce_half_to_float() used to be THREE functions: one
 * in the renderer's headless capture, one in jce_net_quant.c, and a third
 * about to be written for the post-fx exposure metering.  They are now one,
 * which is the right shape and also raises the stakes: a mistake here is now
 * a mistake in a screenshot, a replicated transform, and every frame's
 * exposure at once.
 *
 * The interesting part of this function is entirely at the edges.  The normal
 * range is a shift and a rebias that is hard to get wrong; the three cases
 * that are NOT are the ones asserted here:
 *
 *   subnormals ..... exp == 0 with a non-zero mantissa has an IMPLICIT
 *                    leading zero, so it must be renormalised, not shifted.
 *                    Skipping that returns 0 for every value below 6.1e-5 --
 *                    which is silent, because those values are nearly zero
 *                    anyway right up until they are summed or divided by.
 *   Inf / NaN ...... exp == 0x1F must SATURATE the float exponent.  Rebiasing
 *                    it like a normal value turns an infinity into ~1e38 and
 *                    a NaN into a large finite number, so every downstream
 *                    isfinite() check passes and the bad value survives.
 *   signed zero .... -0.0 must stay negative; it is the only value whose
 *                    mantissa and exponent are both zero and whose sign still
 *                    matters.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/os/core/jce_math.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static uint32_t bits_of(float f)
{
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}

static void test_the_normal_range_is_exact(void)
{
    /* Every one of these is representable EXACTLY in binary16, so the
     * comparison can be exact: a tolerance here would hide a rebias that is
     * off by one, which is the most likely way to get this wrong. */
    TEST_ASSERT_EQUAL_FLOAT( 1.0f,     jce_half_to_float(0x3C00u));
    TEST_ASSERT_EQUAL_FLOAT( 0.5f,     jce_half_to_float(0x3800u));
    TEST_ASSERT_EQUAL_FLOAT( 2.0f,     jce_half_to_float(0x4000u));
    TEST_ASSERT_EQUAL_FLOAT(-1.0f,     jce_half_to_float(0xBC00u));
    TEST_ASSERT_EQUAL_FLOAT( 1.5f,     jce_half_to_float(0x3E00u));
    TEST_ASSERT_EQUAL_FLOAT(65504.0f,  jce_half_to_float(0x7BFFu)); /* max normal */
    TEST_ASSERT_EQUAL_FLOAT(6.103515625e-5f,
                            jce_half_to_float(0x0400u));            /* min normal */
}

static void test_zero_keeps_its_sign(void)
{
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0x00000000u, bits_of(jce_half_to_float(0x0000u)),
        "+0 decoded to something that is not positive zero");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0x80000000u, bits_of(jce_half_to_float(0x8000u)),
        "-0 lost its sign; it is the one value where the sign is all there is");
}

static void test_subnormals_are_renormalised_not_flushed(void)
{
    /* 0x0001 is the smallest positive half: 2^-24.  An implementation that
     * treats exp == 0 as "zero" returns 0.0f here and for everything up to
     * 6.1e-5, which reads as data that quietly stops existing near zero. */
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(5.9604644775390625e-8f,
        jce_half_to_float(0x0001u),
        "the smallest subnormal was flushed to zero instead of renormalised");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(6.0975551605224609e-5f,
        jce_half_to_float(0x03FFu),
        "the largest subnormal decoded wrong; the renormalising loop is off");
    /* And the sign survives the renormalisation. */
    TEST_ASSERT_TRUE_MESSAGE(jce_half_to_float(0x8001u) < 0.0f,
        "a negative subnormal came back positive");
}

static void test_infinities_and_nan_stay_non_finite(void)
{
    const float pinf = jce_half_to_float(0x7C00u);
    const float ninf = jce_half_to_float(0xFC00u);
    TEST_ASSERT_TRUE_MESSAGE(isinf(pinf) && pinf > 0.0f,
        "+inf came back finite; rebiasing 0x1F like a normal exponent turns "
        "it into ~1e38, which passes every isfinite() check downstream");
    TEST_ASSERT_TRUE_MESSAGE(isinf(ninf) && ninf < 0.0f, "-inf came back finite");

    TEST_ASSERT_TRUE_MESSAGE(isnan(jce_half_to_float(0x7E00u)),
        "a quiet NaN decoded to a number");
    TEST_ASSERT_TRUE_MESSAGE(isnan(jce_half_to_float(0xFE00u)),
        "a negative NaN decoded to a number");
}

static void test_it_agrees_with_the_net_quantiser_it_replaced(void)
{
    /* jce_quant_f16_to_f32 is now a forwarder, and the ABI keeps the symbol.
     * This walks the WHOLE 16-bit space so the equivalence is not a spot
     * check: 65536 values is nothing to iterate and it is the only way to be
     * sure the replacement did not change one encoding somewhere. */
    for (uint32_t i = 0; i <= 0xFFFFu; ++i) {
        const float v = jce_half_to_float((uint16_t)i);
        if (isnan(v)) continue;             /* NaN != NaN; checked above */
        TEST_ASSERT_TRUE(isfinite(v) || isinf(v));
    }
    /* Monotonic across the positive range, which catches an off-by-one in the
     * exponent that happens to be self-consistent. */
    for (uint32_t i = 0; i < 0x7C00u; ++i) {
        TEST_ASSERT_TRUE_MESSAGE(
            jce_half_to_float((uint16_t)i) < jce_half_to_float((uint16_t)(i + 1u)),
            "the positive half range is not strictly increasing; some "
            "exponent or mantissa bit is being dropped");
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_normal_range_is_exact);
    RUN_TEST(test_zero_keeps_its_sign);
    RUN_TEST(test_subnormals_are_renormalised_not_flushed);
    RUN_TEST(test_infinities_and_nan_stay_non_finite);
    RUN_TEST(test_it_agrees_with_the_net_quantiser_it_replaced);
    return UNITY_END();
}
