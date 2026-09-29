/*
 * test_jce_auto_exposure.c — the adaptation law, as arithmetic.
 *
 * Every one of these fails on screen as "the exposure feels wrong", which is
 * not a description anybody can act on and not something a screenshot
 * comparison localises.  So they are asserted on the number:
 *
 *   direction ....... a brighter scene must produce a SMALLER multiplier.
 *                     Get the sign wrong and walking into daylight makes the
 *                     image brighter, which reads as a lighting bug.
 *   convergence ..... stepping repeatedly must reach the target and stay.
 *   no overshoot .... a dt large enough to cross the target must land ON it.
 *                     A dropped frame produces exactly that dt, and an
 *                     overshoot there is a flash on a frame the player has
 *                     already noticed.
 *   asymmetry ....... dark->bright is faster than bright->dark, in the eye and
 *                     in every engine that models it.  One speed makes one of
 *                     the two directions feel wrong.
 *   the clamp ....... an almost-black frame must not drive the target to
 *                     infinity, or the next bright frame arrives as a white
 *                     flash that takes seconds to recover from.
 *   log(0) .......... a black frame is a REAL frame: the first one, a fade, a
 *                     fully occluded shot.  log2(0) is -inf and it propagates
 *                     into every pixel.
 *   NaN ............. a readback that arrived short produces one, and NaN
 *                     compares false against every bound, so a plain range
 *                     check lets it through.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/renderer/jce_auto_exposure.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static JceAutoExposureDesc D(void) { return jce_auto_exposure_desc_default(); }

static float settle(float ev, float target, float dt, int steps)
{
    JceAutoExposureDesc d = D();
    for (int i = 0; i < steps; ++i)
        ev = jce_auto_exposure_step(ev, target, dt, &d);
    return ev;
}

/* ── The metering half ────────────────────────────────────────────────
 *
 * float -> IEEE binary16, so a test can build the exact pixel buffer
 * bgfx_read_texture() would hand back.  Only the normal range is needed here;
 * the special encodings are written as literals below where they are meant. */
static uint16_t h16(float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t  exp  = (int32_t)((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = (bits >> 13) & 0x3FFu;
    if (f == 0.0f)  return (uint16_t)sign;
    if (exp <= 0)   return (uint16_t)sign;              /* flush to zero */
    if (exp >= 31)  return (uint16_t)(sign | 0x7C00u);  /* inf */
    return (uint16_t)(sign | ((uint32_t)exp << 10) | mant);
}

static void fill_grey(uint16_t *px, uint32_t n, float v)
{
    for (uint32_t i = 0; i < n; ++i) {
        px[i * 4 + 0] = h16(v);
        px[i * 4 + 1] = h16(v);
        px[i * 4 + 2] = h16(v);
        px[i * 4 + 3] = h16(1.0f);
    }
}

/* The linear mean, written out here ON PURPOSE: it is the implementation this
 * function must NOT be, and the firefly case below is only meaningful as a
 * comparison against it. */
static float linear_mean(const uint16_t *px, uint32_t n);

#define AE_N 1024u

static void test_a_flat_frame_meters_its_own_luminance(void)
{
    uint16_t px[AE_N * 4];
    fill_grey(px, AE_N, 0.25f);
    /* Rec.709 luma of a neutral grey is the grey itself: the weights sum to 1,
     * and if they did not every neutral scene would meter wrong. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.002f, 0.25f,
        jce_auto_exposure_log_average_rgba16f(px, AE_N),
        "a flat neutral frame must meter as its own luminance");
}

static void test_one_blown_pixel_does_not_move_the_exposure(void)
{
    uint16_t px[AE_N * 4];
    fill_grey(px, AE_N, 0.18f);
    /* Four fireflies out of 1024 -- a sun glint, a filament, two speculars. */
    for (uint32_t i = 0; i < 4u; ++i) fill_grey(px + i * 4u, 1u, 400.0f);

    const float geo = jce_auto_exposure_log_average_rgba16f(px, AE_N);
    const float lin = linear_mean(px, AE_N);

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.18f * 0.05f, 0.18f, geo,
        "four blown pixels in a thousand moved the metered luminance by more "
        "than 5%; the exposure would visibly dip whenever a specular enters "
        "frame, which reads on screen as the lighting flickering");

    /* The control.  If this ever stops holding, the function above has
     * silently become a linear mean and the assertion before it is vacuous. */
    TEST_ASSERT_TRUE_MESSAGE(lin > geo * 4.0f,
        "the linear mean was supposed to be dragged far off by the same four "
        "pixels; if it is not, this test is no longer testing anything");
}

static void test_an_unrendered_frame_is_not_something_to_steer_from(void)
{
    /* THE CASE THIS EXISTS FOR, measured on a GPU before it was written:
     * the first read-back landed on frame 10 of a real scene, before anything
     * had drawn into the metering target.  It read 1.0e-5 -- the floor -- and
     * the floor asks for max_ev, so the exposure snapped to +8 EV, a 256x
     * multiplier, and was STILL six stops away from the correct -1.37 at
     * frame 240.  Every level would have opened uniformly white for three
     * seconds, which on screen is indistinguishable from a broken tonemap. */
    uint16_t px[64 * 4];
    memset(px, 0, sizeof px);
    const float empty = jce_auto_exposure_log_average_rgba16f(px, 64u);
    TEST_ASSERT_FALSE_MESSAGE(jce_auto_exposure_measurement_is_usable(empty),
        "an all-zero read-back was accepted as a measurement; it asks for "
        "max_ev, so it opens every level at the top of the clamp");

    /* And the frame right after it, which IS a scene, must be accepted --
     * or the rejection has simply turned the feature off. */
    fill_grey(px, 64u, 0.02f);   /* a dim interior, ~3 stops under the key */
    TEST_ASSERT_TRUE_MESSAGE(jce_auto_exposure_measurement_is_usable(
                                 jce_auto_exposure_log_average_rgba16f(px, 64u)),
        "a genuinely dim but rendered frame was rejected; the threshold is "
        "supposed to separate 'nothing drew' from 'the scene is dark'");

    /* The threshold has to sit clear of both: an order of magnitude above the
     * floor, and many stops below anything lit. */
    TEST_ASSERT_FALSE(jce_auto_exposure_measurement_is_usable(0.0f));
    TEST_ASSERT_FALSE_MESSAGE(jce_auto_exposure_measurement_is_usable((float)NAN),
        "a NaN measurement was called usable; it would reach log2");
    TEST_ASSERT_TRUE(jce_auto_exposure_measurement_is_usable(0.001f));
}

static void test_a_black_frame_meters_finite_not_zero(void)
{
    uint16_t px[64 * 4];
    memset(px, 0, sizeof px);
    const float v = jce_auto_exposure_log_average_rgba16f(px, 64u);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(v) && v > 0.0f,
        "an all-black frame produced 0 or -inf; log2 of it becomes the "
        "exposure and then every pixel");

    /* And it must still be a NUMBER the law accepts. */
    TEST_ASSERT_TRUE(isfinite(jce_auto_exposure_target_ev(v, NULL)));
}

static void test_a_short_or_corrupt_readback_cannot_poison_the_frame(void)
{
    uint16_t px[64 * 4];
    fill_grey(px, 64u, 0.5f);
    px[0]  = 0x7E00u;   /* NaN  */
    px[5]  = 0x7C00u;   /* +inf */
    px[10] = 0xFC00u;   /* -inf */
    const float v = jce_auto_exposure_log_average_rgba16f(px, 64u);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(v) && v > 0.0f,
        "a NaN/inf texel reached the average; NaN fails every range check, so "
        "a bare `l < floor` lets it through and one bad texel decides the "
        "exposure of the whole frame");

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f,
        jce_auto_exposure_log_average_rgba16f(NULL, 64u),
        "a NULL buffer must read as 'nothing measured', not as black");
    TEST_ASSERT_EQUAL_FLOAT(0.0f,
        jce_auto_exposure_log_average_rgba16f(px, 0u));
}

static float linear_mean(const uint16_t *px, uint32_t n)
{
    /* Decodes the same way the function under test does, so the comparison is
     * about the AVERAGE and not about the decode. */
    double sum = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint16_t *h = px + (size_t)i * 4u;
        float c[3];
        for (int k = 0; k < 3; ++k) {
            const uint32_t s2 = (uint32_t)(h[k] >> 15) & 1u;
            const uint32_t e  = (uint32_t)(h[k] >> 10) & 0x1Fu;
            const uint32_t m  = (uint32_t)h[k] & 0x3FFu;
            c[k] = (e == 0u) ? 0.0f
                 : ldexpf(1.0f + (float)m / 1024.0f, (int)e - 15);
            if (s2) c[k] = -c[k];
        }
        sum += (double)(0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]);
    }
    return (float)(sum / (double)n);
}

static void test_a_brighter_scene_gets_a_smaller_multiplier(void)
{
    JceAutoExposureDesc d = D();
    const float dim    = jce_auto_exposure_target_ev(0.05f, &d);
    const float mid    = jce_auto_exposure_target_ev(0.18f, &d);
    const float bright = jce_auto_exposure_target_ev(4.00f, &d);

    TEST_ASSERT_TRUE_MESSAGE(dim > mid && mid > bright,
        "EV must FALL as the scene brightens; the other sign makes walking "
        "into daylight brighten the image, which reads as a lighting bug");

    TEST_ASSERT_TRUE_MESSAGE(
        jce_auto_exposure_multiplier(bright) < jce_auto_exposure_multiplier(mid),
        "a brighter scene must end up with a smaller exposure multiplier");

    /* At the key itself the scene is already correctly exposed, so the
     * multiplier is 1: a scene that needs no adaptation must not get any. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 1.0f,
        jce_auto_exposure_multiplier(mid),
        "average luminance == key must mean a multiplier of exactly 1");
}

static void test_it_converges_and_stays(void)
{
    JceAutoExposureDesc d = D();
    const float target = jce_auto_exposure_target_ev(1.0f, &d);

    const float ev = settle(4.0f, target, 1.0f / 60.0f, 2000);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, target, ev,
        "adaptation did not converge on the target");

    /* And it STAYS: stepping an already-settled value must not drift. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.0001f, target,
        settle(ev, target, 1.0f / 60.0f, 600),
        "a settled exposure drifts; the image would breathe while nothing "
        "in the scene is changing");
}

static void test_a_long_frame_lands_on_the_target_not_past_it(void)
{
    JceAutoExposureDesc d = D();
    /* A dropped frame.  Half a second at speed_up 3 EV/s is 1.5 EV of travel
     * against a 1.0 EV gap, so an implementation that just adds speed*dt goes
     * straight past. */
    const float ev = jce_auto_exposure_step(1.0f, 0.0f, 0.5f, &d);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.0001f, 0.0f, ev,
        "a long frame overshot the target; that is a visible flash on a frame "
        "the player already noticed because it hitched");

    /* The same in the other direction, or the guard only covers one sign. */
    const float up = jce_auto_exposure_step(0.0f, 0.4f, 10.0f, &d);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.0001f, 0.4f, up,
        "overshoot is guarded in one direction only");
}

static void test_adaptation_is_asymmetric(void)
{
    JceAutoExposureDesc d = D();
    TEST_ASSERT_TRUE_MESSAGE(d.speed_up > d.speed_down,
        "the default must be faster into brightness than out of it");

    /* Equal gaps, one in each direction, one frame each. */
    const float to_bright = 2.0f - jce_auto_exposure_step(2.0f, 0.0f, 0.1f, &d);
    const float to_dark   = jce_auto_exposure_step(0.0f, 2.0f, 0.1f, &d) - 0.0f;

    /* A RATIO, not `>`.  The first version of this assertion was
     * `to_bright > to_dark`, and with the direction branch removed it still
     * PASSED: 2.0f - 1.9f is 0.10000002 while 0.1f - 0.0f is 0.1, so a
     * float rounding artefact satisfied it.  Caught by running that removal
     * as a control and getting one failure instead of two.  Asymmetry means
     * the fast direction is MEANINGFULLY faster, so that is what is asked. */
    TEST_ASSERT_TRUE_MESSAGE(to_bright > to_dark * 1.5f,
        "dark->bright must move substantially further per frame than "
        "bright->dark; with one speed, whichever value you pick makes the "
        "other direction wrong");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.0001f,
        d.speed_up / d.speed_down, to_bright / to_dark,
        "the per-frame travel ratio must be the speed ratio; if it is not, "
        "one of the two directions is not using its own speed");
}

static void test_the_clamp_holds_at_both_ends(void)
{
    JceAutoExposureDesc d = D();
    const float from_black = jce_auto_exposure_target_ev(0.0f, &d);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.0001f, d.max_ev, from_black,
        "an almost-black frame must clamp, not run to infinity -- the next "
        "bright frame would arrive as a white flash");

    const float from_sun = jce_auto_exposure_target_ev(1.0e9f, &d);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.0001f, d.min_ev, from_sun,
        "an extremely bright frame must clamp at min_ev");

    /* Bounds passed the wrong way round must still bracket something. */
    JceAutoExposureDesc swapped = d;
    swapped.min_ev = 8.0f;
    swapped.max_ev = -8.0f;
    const float ev = jce_auto_exposure_target_ev(0.18f, &swapped);
    TEST_ASSERT_TRUE_MESSAGE(ev >= -8.0f && ev <= 8.0f,
        "reversed bounds produced a value outside both of them");
}

static void test_black_frames_and_nan_do_not_become_infinities(void)
{
    JceAutoExposureDesc d = D();
    /* A black frame is a REAL frame: the first one, a fade, an occluded shot. */
    const float zero = jce_auto_exposure_target_ev(0.0f, &d);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(zero), "log2(0) escaped as -inf");

    const float neg = jce_auto_exposure_target_ev(-3.0f, &d);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(neg), "a negative average produced NaN");

    /* NaN compares FALSE against every bound, so a plain `if (l < floor)` lets
     * it through untouched -- which is the trap this orders isfinite first
     * to avoid.  A short readback produces exactly this input. */
    const float nan_in = jce_auto_exposure_target_ev((float)NAN, &d);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(nan_in),
        "a NaN average reached log2; NaN fails every range check, so the "
        "finite test has to come first");

    TEST_ASSERT_TRUE(isfinite(jce_auto_exposure_step((float)NAN, 1.0f, 0.016f, &d)));
    TEST_ASSERT_TRUE(isfinite(jce_auto_exposure_step(1.0f, (float)NAN, 0.016f, &d)));
    TEST_ASSERT_TRUE(isfinite(jce_auto_exposure_step(1.0f, 2.0f, (float)NAN, &d)));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 1.0f,
        jce_auto_exposure_multiplier((float)NAN),
        "a non-finite EV must yield a neutral multiplier, not a NaN scale on "
        "every pixel");
}

static void test_zero_dt_and_frozen_speeds_do_nothing(void)
{
    JceAutoExposureDesc d = D();
    TEST_ASSERT_EQUAL_FLOAT(3.0f, jce_auto_exposure_step(3.0f, 0.0f, 0.0f, &d));
    TEST_ASSERT_EQUAL_FLOAT(3.0f, jce_auto_exposure_step(3.0f, 0.0f, -1.0f, &d));

    /* speed 0 is how a caller pins the exposure without turning the feature
     * off; it must hold, not divide by zero or snap. */
    JceAutoExposureDesc frozen = d;
    frozen.speed_up = frozen.speed_down = 0.0f;
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(3.0f,
        jce_auto_exposure_step(3.0f, -5.0f, 1.0f, &frozen),
        "speed 0 must freeze the exposure where it is");
}

static void test_the_bias_is_a_stop_offset(void)
{
    JceAutoExposureDesc d = D();
    JceAutoExposureDesc up = d;
    up.exposure_bias = 1.0f;   /* one stop brighter */

    const float base  = jce_auto_exposure_target_ev(0.5f, &d);
    const float lifted = jce_auto_exposure_target_ev(0.5f, &up);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.0001f, base + 1.0f, lifted,
        "the bias must be in STOPS: +1 has to double the multiplier, which is "
        "the only reason to express it in EV rather than as a scale");
    TEST_ASSERT_FLOAT_WITHIN(0.001f,
        2.0f * jce_auto_exposure_multiplier(base),
        jce_auto_exposure_multiplier(lifted));
}

static void test_a_null_desc_is_the_documented_default(void)
{
    JceAutoExposureDesc d = D();
    TEST_ASSERT_EQUAL_FLOAT(jce_auto_exposure_target_ev(0.4f, &d),
                            jce_auto_exposure_target_ev(0.4f, NULL));
    TEST_ASSERT_EQUAL_FLOAT(jce_auto_exposure_step(1.0f, 0.0f, 0.1f, &d),
                            jce_auto_exposure_step(1.0f, 0.0f, 0.1f, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_brighter_scene_gets_a_smaller_multiplier);
    RUN_TEST(test_it_converges_and_stays);
    RUN_TEST(test_a_long_frame_lands_on_the_target_not_past_it);
    RUN_TEST(test_adaptation_is_asymmetric);
    RUN_TEST(test_the_clamp_holds_at_both_ends);
    RUN_TEST(test_black_frames_and_nan_do_not_become_infinities);
    RUN_TEST(test_zero_dt_and_frozen_speeds_do_nothing);
    RUN_TEST(test_the_bias_is_a_stop_offset);
    RUN_TEST(test_a_null_desc_is_the_documented_default);
    RUN_TEST(test_a_flat_frame_meters_its_own_luminance);
    RUN_TEST(test_one_blown_pixel_does_not_move_the_exposure);
    RUN_TEST(test_an_unrendered_frame_is_not_something_to_steer_from);
    RUN_TEST(test_a_black_frame_meters_finite_not_zero);
    RUN_TEST(test_a_short_or_corrupt_readback_cannot_poison_the_frame);
    return UNITY_END();
}
