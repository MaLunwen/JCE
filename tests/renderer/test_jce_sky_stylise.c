/*
 * test_jce_sky_stylise.c
 *
 * The sky stylisation grade.
 *
 * The design's acceptance criterion 7 is "provably a no-op at identity
 * parameters", and that is the assertion this file exists for. A grade that
 * shifts the sky by a fraction of a unit when nobody enabled stylisation is a
 * permanent global colour change -- and it would be hunted in the scattering
 * code for a long time before anyone suspected a feature that is switched off.
 */

#include "renderer/jce_sky_stylise.h"

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* A spread of radiances: near-black sky, mid cloud, and a sun disc well above
 * 1, since the input is HDR and the bright end is where a grade misbehaves. */
static const float SAMPLES[6][3] = {
    { 0.0f,   0.0f,   0.0f   },
    { 0.001f, 0.002f, 0.004f },
    { 0.12f,  0.18f,  0.31f  },
    { 0.9f,   0.85f,  0.8f   },
    { 4.0f,   3.6f,   3.1f   },
    { 120.0f, 118.0f, 110.0f },
};

/* ── 1. THE CRITERION: identity is bit-identical ───────────────────────  */

static void test_identity_is_exactly_a_no_op(void)
{
    const JceSkyStylise id = jce_sky_stylise_identity();
    TEST_ASSERT_TRUE(jce_sky_stylise_is_identity(&id));

    for (int i = 0; i < 6; ++i)
        for (int r = 0; r <= 4; ++r) {
            float out[3] = { -1.0f, -1.0f, -1.0f };
            jce_sky_stylise_apply(&id, SAMPLES[i], (float)r * 0.25f, out);
            /* EQUAL_FLOAT, not WITHIN: "close enough" across the whole sky is
             * a global colour shift nobody asked for. */
            TEST_ASSERT_EQUAL_FLOAT(SAMPLES[i][0], out[0]);
            TEST_ASSERT_EQUAL_FLOAT(SAMPLES[i][1], out[1]);
            TEST_ASSERT_EQUAL_FLOAT(SAMPLES[i][2], out[2]);
        }

    /* Out-of-range input passes through UNCHANGED at identity.
     *
     * This is what makes the criterion real rather than decorative: the graded
     * path ends in a clamp that drops negatives and non-finite values to zero,
     * which is correct for a grade and wrong for a pass-through.  If identity
     * went through the grade, enabling a feature nobody turned on would
     * silently rewrite whatever the scattering core produced -- and the sky
     * would differ depending on a switch that is off. */
    {
        const float odd[3] = { -0.5f, 0.0f, 1e30f };
        float out[3] = { 9.0f, 9.0f, 9.0f };
        jce_sky_stylise_apply(&id, odd, 0.0f, out);
        TEST_ASSERT_EQUAL_FLOAT(odd[0], out[0]);
        TEST_ASSERT_EQUAL_FLOAT(odd[1], out[1]);
        TEST_ASSERT_EQUAL_FLOAT(odd[2], out[2]);
    }

    /* A NULL grade is also a no-op, not a black sky. */
    for (int i = 0; i < 6; ++i) {
        float out[3] = { -1.0f, -1.0f, -1.0f };
        jce_sky_stylise_apply(NULL, SAMPLES[i], 0.5f, out);
        TEST_ASSERT_EQUAL_FLOAT(SAMPLES[i][0], out[0]);
        TEST_ASSERT_EQUAL_FLOAT(SAMPLES[i][2], out[2]);
    }
}

/* ── 2. Identity must be the constructor, not a zeroed struct ──────────
 *
 * A zeroed JceSkyStylise has zero tints, which is BLACK. Anyone who memsets
 * one and calls this would get a black sky and reasonably blame the sky. */

static void test_zeroed_struct_is_not_identity(void)
{
    JceSkyStylise z;
    memset(&z, 0, sizeof z);
    TEST_ASSERT_FALSE(jce_sky_stylise_is_identity(&z));

    const JceSkyStylise id = jce_sky_stylise_identity();
    TEST_ASSERT_EQUAL_FLOAT(1.0f, id.shadow_tint[0]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, id.saturation);
    TEST_ASSERT_EQUAL_UINT32(0u, id.bands);
}

/* ── 3. A single band is treated as OFF ────────────────────────────────
 *
 * One band is a flat colour over the whole sky. That is not a stylisation
 * anyone wants; it is an off-by-one that renders as a solid dome, and it must
 * not be reachable by setting a slider to its minimum. */

static void test_one_band_is_off(void)
{
    JceSkyStylise s = jce_sky_stylise_identity();
    s.bands = 1u;
    TEST_ASSERT_TRUE(jce_sky_stylise_is_identity(&s));

    float out[3];
    jce_sky_stylise_apply(&s, SAMPLES[3], 0.0f, out);
    TEST_ASSERT_EQUAL_FLOAT(SAMPLES[3][0], out[0]);

    s.bands = 2u;
    TEST_ASSERT_FALSE(jce_sky_stylise_is_identity(&s));
}

/* ── 4. Each knob actually does something, and only its own thing ──────  */

static void test_each_parameter_has_an_effect(void)
{
    const float in[3] = { 0.2f, 0.3f, 0.5f };
    float out[3];

    JceSkyStylise s = jce_sky_stylise_identity();
    s.mid_tint[0] = 2.0f;
    jce_sky_stylise_apply(&s, in, 0.0f, out);
    TEST_ASSERT_TRUE(out[0] > in[0]);

    s = jce_sky_stylise_identity();
    s.saturation = 0.0f;                       /* fully desaturated */
    jce_sky_stylise_apply(&s, in, 0.0f, out);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, out[0], out[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, out[1], out[2]);

    /* Rim only fires where there IS a rim.  A rim term that brightened the
     * interior too would just be an exposure change wearing a different name. */
    s = jce_sky_stylise_identity();
    s.rim_strength = 1.0f;
    float interior[3], edge[3];
    jce_sky_stylise_apply(&s, in, 0.0f, interior);
    jce_sky_stylise_apply(&s, in, 1.0f, edge);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, in[0], interior[0]);
    TEST_ASSERT_TRUE(edge[0] > interior[0]);
}

/* ── 5. Posterisation quantises LUMINANCE, not channels ────────────────
 *
 * Per-channel banding shifts hue at every step: a smooth blue gradient bands
 * into blue, then cyan, then green. That looks like a colour bug rather than
 * like posterisation, and it is the usual way this gets written. */

static void test_posterisation_preserves_hue(void)
{
    JceSkyStylise s = jce_sky_stylise_identity();
    s.bands = 4u;

    /* Walk a gradient of one hue and check the ratios survive. */
    for (int i = 1; i <= 40; ++i) {
        const float k = (float)i * 0.05f;
        const float in[3] = { 0.2f * k, 0.35f * k, 0.7f * k };
        float out[3];
        jce_sky_stylise_apply(&s, in, 0.0f, out);

        TEST_ASSERT_TRUE(isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]));
        TEST_ASSERT_TRUE(out[0] >= 0.0f);
        if (out[2] > 1e-6f && in[2] > 1e-6f) {
            const float hue_in  = in[0]  / in[2];
            const float hue_out = out[0] / out[2];
            TEST_ASSERT_FLOAT_WITHIN(1e-3f, hue_in, hue_out);
        }
    }

    /* And it must actually band -- a "posterisation" that leaves a smooth
     * gradient smooth is a no-op with a slider attached. */
    int distinct = 0;
    float prev = -1.0f;
    for (int i = 1; i <= 60; ++i) {
        const float in[3] = { 0.02f * (float)i, 0.02f * (float)i, 0.02f * (float)i };
        float out[3];
        jce_sky_stylise_apply(&s, in, 0.0f, out);
        if (fabsf(out[0] - prev) > 1e-4f) { distinct++; prev = out[0]; }
    }
    TEST_ASSERT_TRUE_MESSAGE(distinct <= 12, "posterisation did not band the gradient");
    TEST_ASSERT_TRUE(distinct > 1);
}

/* ── 6. HDR input does not collapse the palette ────────────────────────
 *
 * Radiance is unbounded. If the palette position came straight from luminance,
 * a sun disc at 120 would pin every bright pixel to the highlight tint and the
 * three-point palette would render as one colour. */

static void test_bright_input_still_spans_the_palette(void)
{
    JceSkyStylise s = jce_sky_stylise_identity();
    s.shadow_tint[0] = 0.5f;
    s.highlight_tint[0] = 2.0f;

    float dim[3], bright[3];
    const float a[3] = { 0.05f, 0.05f, 0.05f };
    const float b[3] = { 8.0f,  8.0f,  8.0f  };
    jce_sky_stylise_apply(&s, a, 0.0f, dim);
    jce_sky_stylise_apply(&s, b, 0.0f, bright);

    /* The dim sample must be pulled toward the shadow tint and the bright one
     * toward the highlight -- i.e. their RATIOS to input must differ. */
    const float ka = dim[0] / a[0];
    const float kb = bright[0] / b[0];
    TEST_ASSERT_TRUE_MESSAGE(kb > ka * 1.5f,
        "palette collapsed - every bright pixel took the same tint");

    /* The discriminating case, and the reason the position is a tone-mapped
     * proxy rather than a clamp: TWO samples both above 1 must still receive
     * DIFFERENT tints.  A clamp at 1 passes the dim-vs-bright check above and
     * still flattens the entire upper half of an HDR sky to one colour --
     * which reads as a deliberately flat art style. */
    float mid[3], high[3];
    const float m[3] = { 2.0f,  2.0f,  2.0f  };
    const float h[3] = { 60.0f, 60.0f, 60.0f };
    jce_sky_stylise_apply(&s, m, 0.0f, mid);
    jce_sky_stylise_apply(&s, h, 0.0f, high);
    const float km = mid[0]  / m[0];
    const float kh = high[0] / h[0];
    TEST_ASSERT_TRUE_MESSAGE(fabsf(kh - km) > 0.05f,
        "palette is clamped - everything above 1.0 takes an identical tint");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_identity_is_exactly_a_no_op);
    RUN_TEST(test_zeroed_struct_is_not_identity);
    RUN_TEST(test_one_band_is_off);
    RUN_TEST(test_each_parameter_has_an_effect);
    RUN_TEST(test_posterisation_preserves_hue);
    RUN_TEST(test_bright_input_still_spans_the_palette);
    return UNITY_END();
}
