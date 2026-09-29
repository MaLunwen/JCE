/*
 * test_jce_water_shoreline.c
 *
 * Foam derived from how THIN the water is.
 *
 * The term this replaces measured the radius from the centre of the water
 * quad, so it produced foam rings in open water and none where the water met
 * land -- and read as a stylistic ripple rather than as a bug. The assertions
 * below are about the property that makes the new one right: the band is a
 * distance in METRES through the water, so it is the same physical width on a
 * pond and on an ocean and at every viewing angle.
 */

#include "jce_water_shoreline.h"

#include <math.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* ── 1. Foam lives at the waterline and nowhere else ───────────────────  */

static void test_foam_peaks_at_the_waterline(void)
{
    const float band = 2.0f;

    /* At the waterline the column has no thickness: maximum foam. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, jce_water_shore_foam(0.0f, band));
    /* At the band edge: exactly none.  Not "almost none" -- a residue here is
     * a permanent haze over every square metre of the whole body. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_shore_foam(band, band));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_shore_foam(band * 10.0f, band));

    /* Monotone decreasing across the band. */
    float prev = 2.0f;
    for (int i = 0; i <= 100; ++i) {
        const float f = jce_water_shore_foam((float)i * 0.02f, band);
        TEST_ASSERT_TRUE(f <= prev + 1e-6f);
        TEST_ASSERT_TRUE(f >= 0.0f && f <= 1.0f);
        prev = f;
    }
}

/* ── 2. THE POINT: the band is a physical distance ─────────────────────
 *
 * The same depth must give the same foam regardless of how big the water body
 * is -- that is the whole difference from a UV-radius measure, which makes the
 * band a fraction of the mesh and therefore a different physical width on
 * every body that uses the same setting. */

static void test_band_is_metres_not_a_fraction(void)
{
    /* Half a metre down, with a 2 m band, is the same foam whether the body is
     * a puddle or an ocean -- neither appears in the arguments at all, which
     * is exactly the property being asserted. */
    const float a = jce_water_shore_foam(0.5f, 2.0f);
    TEST_ASSERT_TRUE(a > 0.0f && a < 1.0f);

    /* Doubling the band pushes the same depth further up the curve. */
    TEST_ASSERT_TRUE(jce_water_shore_foam(0.5f, 4.0f) > a);
    /* Halving it pushes the same depth out of the band entirely. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_shore_foam(0.5f, 0.4f));
}

/* ── 3. The falloff is not linear ──────────────────────────────────────
 *
 * A linear ramp paints a uniform wedge with a hard outer edge; on a shallow
 * beach that edge sits far offshore and reads as a seam in the water. Surf
 * concentrates at the very edge, so the curve must be steeper near it. */

static void test_falloff_concentrates_at_the_edge(void)
{
    const float band = 2.0f;
    /* Halfway through the band, a linear ramp would read 0.5. */
    const float mid = jce_water_shore_foam(band * 0.5f, band);
    TEST_ASSERT_TRUE_MESSAGE(mid < 0.45f,
        "falloff is linear - the band will have a hard outer edge");

    /* And the slope must be shallower at the far end than near the shore,
     * which is what removes the visible line where it stops. */
    const float near_a = jce_water_shore_foam(0.05f * band, band);
    const float near_b = jce_water_shore_foam(0.15f * band, band);
    const float far_a  = jce_water_shore_foam(0.85f * band, band);
    const float far_b  = jce_water_shore_foam(0.95f * band, band);
    TEST_ASSERT_TRUE((near_a - near_b) > (far_a - far_b));
}

/* ── 4. Degenerate input disables, and does not paint white ────────────  */

static void test_degenerate_input_makes_no_foam(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_shore_foam(1.0f, 0.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_shore_foam(1.0f, -3.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_shore_foam(nanf(""), 2.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_shore_foam(1.0f, nanf("")));
}

/* ── 5. The surge moves the waterline, and stays bounded ───────────────
 *
 * It multiplies the BAND, not the opacity: foam that faded in place would
 * blink rather than run up the sand. So the value has to stay near 1 -- a
 * surge that reached 0 would make the foam vanish entirely once a cycle. */

static void test_surge_is_bounded_and_periodic(void)
{
    const float period = 6.0f;

    float lo = 9.0f, hi = -9.0f;
    for (int i = 0; i <= 600; ++i) {
        const float g = jce_water_shore_surge((float)i * 0.01f, period);
        TEST_ASSERT_TRUE(isfinite(g));
        if (g < lo) lo = g;
        if (g > hi) hi = g;
    }
    /* Never vanishes, never doubles: the waterline breathes, it does not
     * flood and drain. */
    TEST_ASSERT_TRUE(lo > 0.5f);
    TEST_ASSERT_TRUE(hi < 1.5f);
    /* And it actually moves -- a constant 1 would be a surge that never surges,
     * which is indistinguishable from "the artist set the period wrong". */
    TEST_ASSERT_TRUE_MESSAGE(hi - lo > 0.2f, "surge does not move the waterline");

    /* Periodic: one period later is the same place. */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, jce_water_shore_surge(1.25f, period),
                             jce_water_shore_surge(1.25f + period, period));

    /* A nonsense period is neutral, not zero: the band keeps its authored
     * width rather than collapsing and taking the foam with it. */
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_water_shore_surge(3.0f, 0.0f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_water_shore_surge(3.0f, -1.0f));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_foam_peaks_at_the_waterline);
    RUN_TEST(test_band_is_metres_not_a_fraction);
    RUN_TEST(test_falloff_concentrates_at_the_edge);
    RUN_TEST(test_degenerate_input_makes_no_foam);
    RUN_TEST(test_surge_is_bounded_and_periodic);
    return UNITY_END();
}
