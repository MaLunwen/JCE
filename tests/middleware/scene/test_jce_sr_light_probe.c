/*
 * test_jce_sr_light_probe.c — baked light probes must vary WITH THE POINT
 * BEING SHADED, not with the camera.
 *
 * WHAT THIS IS ABOUT.  sr_gather_lpg_cb used to pick the ONE probe nearest the
 * CAMERA and write it into sr->gi_sh9, so every lit surface in the frame got
 * the same indirect light.  A character in a dark doorway and a wall in open
 * sun were lit identically -- which is the one thing light probes exist to
 * prevent -- and walking the camera past the midpoint between two probes
 * repainted every object in the scene at once, with nothing having moved.
 *
 * THE LOAD-BEARING ASSERTION is that two DIFFERENT sample points give
 * DIFFERENT answers.  Asserting "sampling near the red probe returns red"
 * alone would still pass on an implementation that returned the nearest probe
 * to a fixed camera, because the fixture would have to place the camera
 * somewhere and any single sample agrees with some camera position.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include "jce_sr_light_probe.h"

#include <math.h>
#include <string.h>

/* A probe whose DC term (coefficient 0) is a flat colour, so an assertion can
 * read a single number and say what it means.  The other 8 bands are left at
 * zero: this file is about WHICH probe reaches a point, not about SH maths. */
static void probe_dc(float sh9[9][3], float r, float g, float b)
{
    memset(sh9, 0, 9 * 3 * sizeof(float));
    sh9[0][0] = r;
    sh9[0][1] = g;
    sh9[0][2] = b;
}

static JceSrProbeSet *two_probes(void)
{
    /* RED at x=-10, GREEN at x=+10.  Nothing else in the set. */
    JceSrProbeSet *s = sr_probe_set_create();
    TEST_ASSERT_NOT_NULL(s);
    float red[9][3], green[9][3];
    probe_dc(red,   1.0f, 0.0f, 0.0f);
    probe_dc(green, 0.0f, 1.0f, 0.0f);
    TEST_ASSERT_TRUE(sr_probe_set_add(s, jce_v3(-10, 0, 0), red));
    TEST_ASSERT_TRUE(sr_probe_set_add(s, jce_v3(10, 0, 0), green));
    TEST_ASSERT_EQUAL_INT(2, sr_probe_set_count(s));
    return s;
}

static void test_two_points_get_two_answers(void)
{
    JceSrProbeSet *s = two_probes();
    float near_red[9][3], near_green[9][3];

    TEST_ASSERT_TRUE(sr_probe_set_sample(s, jce_v3(-9, 0, 0), near_red) > 0.0f);
    TEST_ASSERT_TRUE(sr_probe_set_sample(s, jce_v3(9, 0, 0), near_green) > 0.0f);

    TEST_ASSERT_TRUE_MESSAGE(near_red[0][0] > near_red[0][1],
        "a point beside the RED probe must read mostly red");
    TEST_ASSERT_TRUE_MESSAGE(near_green[0][1] > near_green[0][0],
        "a point beside the GREEN probe must read mostly green -- if these two "
        "agree, the sample does not depend on where it was taken, which is the "
        "whole defect this replaces");
}

static void test_standing_on_a_probe_returns_that_probe(void)
{
    /* The epsilon in 1/(d^2+eps) has to make d==0 finite AND dominant.  If it
     * were too large the exact-hit case would be a blend, and a designer who
     * places a probe exactly where an object sits would not get what they
     * baked. */
    JceSrProbeSet *s = two_probes();
    float out[9][3];
    TEST_ASSERT_TRUE(sr_probe_set_sample(s, jce_v3(-10, 0, 0), out) > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 1.0f, out[0][0],
        "on the red probe the answer must BE the red probe");
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, out[0][1]);
    for (int c = 0; c < 9; ++c)
        for (int k = 0; k < 3; ++k)
            TEST_ASSERT_TRUE_MESSAGE(out[c][k] == out[c][k], "NaN in the blend");
}

static void test_the_midpoint_is_a_blend_not_a_switch(void)
{
    /* The other half of the old defect: a hard nearest-probe rule makes the
     * value JUMP as the sample crosses the midpoint.  Here the midpoint must
     * be an even mix, and the two sides of it must be close to each other. */
    JceSrProbeSet *s = two_probes();
    float mid[9][3], just_left[9][3], just_right[9][3];
    TEST_ASSERT_TRUE(sr_probe_set_sample(s, jce_v3(0, 0, 0), mid) > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, mid[0][0], mid[0][1],
        "exactly between two probes the two must contribute equally");

    TEST_ASSERT_TRUE(sr_probe_set_sample(s, jce_v3(-0.05f, 0, 0), just_left) > 0.0f);
    TEST_ASSERT_TRUE(sr_probe_set_sample(s, jce_v3(0.05f, 0, 0), just_right) > 0.0f);
    const float jump = fabsf(just_left[0][0] - just_right[0][0]);
    TEST_ASSERT_TRUE_MESSAGE(jump < 0.05f,
        "crossing the midpoint must be continuous; a nearest-probe rule would "
        "swing the DC term the whole way from 1 to 0 across these 10 cm");
}

static void test_the_far_probe_still_contributes_a_little(void)
{
    /* IDW, not nearest: the far probe is not zero, it is small.  This is what
     * makes the transition smooth, and it is also the honest limitation --
     * a probe on the far side of a wall contributes too, because this sampler
     * does not know about walls. */
    JceSrProbeSet *s = two_probes();
    float out[9][3];
    TEST_ASSERT_TRUE(sr_probe_set_sample(s, jce_v3(-9, 0, 0), out) > 0.0f);
    TEST_ASSERT_TRUE_MESSAGE(out[0][1] > 0.0f,
        "the green probe 19 units away must contribute SOMETHING");
    TEST_ASSERT_TRUE_MESSAGE(out[0][1] < 0.02f,
        "...but 1/d^2 must keep it small: 19x the distance is ~360x less "
        "weight, so it must not wash out the near probe");
}

static void test_one_probe_covers_everywhere(void)
{
    JceSrProbeSet *s = sr_probe_set_create();
    float red[9][3];
    probe_dc(red, 1.0f, 0.0f, 0.0f);
    TEST_ASSERT_TRUE(sr_probe_set_add(s, jce_v3(0, 0, 0), red));
    float out[9][3];
    TEST_ASSERT_TRUE(sr_probe_set_sample(s, jce_v3(500, 500, 500), out) > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, out[0][0]);
    sr_probe_set_destroy(s);
}

static void test_an_empty_set_says_no_data(void)
{
    /* 0 is the "do not apply" signal, and it must leave `out` alone -- a
     * caller that applied a zeroed SH would render the scene BLACK rather than
     * fall back, which is worse than the defect being fixed. */
    JceSrProbeSet *s = sr_probe_set_create();
    float out[9][3];
    probe_dc(out, 7.0f, 7.0f, 7.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, sr_probe_set_sample(s, jce_v3(0, 0, 0), out));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.0f, 7.0f, out[0][0],
        "a refused sample must not touch the caller's buffer");
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, sr_probe_set_sample(NULL, jce_v3(0, 0, 0), out));
    sr_probe_set_destroy(s);
}

static void test_clear_empties_but_keeps_working(void)
{
    JceSrProbeSet *s = two_probes();
    sr_probe_set_clear(s);
    TEST_ASSERT_EQUAL_INT(0, sr_probe_set_count(s));
    float out[9][3];
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, sr_probe_set_sample(s, jce_v3(0, 0, 0), out));

    /* Refilling after a clear is what every frame does. */
    float blue[9][3];
    probe_dc(blue, 0.0f, 0.0f, 1.0f);
    TEST_ASSERT_TRUE(sr_probe_set_add(s, jce_v3(0, 0, 0), blue));
    TEST_ASSERT_TRUE(sr_probe_set_sample(s, jce_v3(0, 0, 0), out) > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, out[0][2]);
    sr_probe_set_destroy(s);
}

static void test_more_probes_than_k_take_the_nearest_k(void)
{
    /* Nine probes in a row; a sample at one end must not be dragged toward
     * the middle by the six it should have ignored. */
    JceSrProbeSet *s = sr_probe_set_create();
    for (int i = 0; i < 9; ++i) {
        float sh[9][3];
        probe_dc(sh, i == 0 ? 1.0f : 0.0f, 0.0f, i == 0 ? 0.0f : 1.0f);
        TEST_ASSERT_TRUE(sr_probe_set_add(s, jce_v3((float)i * 10.0f, 0, 0), sh));
    }
    float out[9][3];
    TEST_ASSERT_TRUE(sr_probe_set_sample(s, jce_v3(0, 0, 0), out) > 0.0f);
    TEST_ASSERT_TRUE_MESSAGE(out[0][0] > 0.99f,
        "standing on probe 0, the eight others must be negligible");
    sr_probe_set_destroy(s);
}

static void test_growth_past_the_initial_capacity(void)
{
    /* The array starts at 64 and doubles; a scene with several groups exceeds
     * that immediately, and a realloc that lost the earlier probes would show
     * up as ambient that changes when an unrelated group is added. */
    JceSrProbeSet *s = sr_probe_set_create();
    float first[9][3], rest[9][3];
    probe_dc(first, 1.0f, 0.0f, 0.0f);
    probe_dc(rest,  0.0f, 0.0f, 1.0f);
    TEST_ASSERT_TRUE(sr_probe_set_add(s, jce_v3(0, 0, 0), first));
    for (int i = 1; i < 300; ++i)
        TEST_ASSERT_TRUE(sr_probe_set_add(s, jce_v3(1000.0f + (float)i, 0, 0), rest));
    TEST_ASSERT_EQUAL_INT(300, sr_probe_set_count(s));

    float out[9][3];
    TEST_ASSERT_TRUE(sr_probe_set_sample(s, jce_v3(0, 0, 0), out) > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, 1.0f, out[0][0],
        "probe 0 must survive 4 reallocations");
    sr_probe_set_destroy(s);
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_two_points_get_two_answers);
    RUN_TEST(test_standing_on_a_probe_returns_that_probe);
    RUN_TEST(test_the_midpoint_is_a_blend_not_a_switch);
    RUN_TEST(test_the_far_probe_still_contributes_a_little);
    RUN_TEST(test_one_probe_covers_everywhere);
    RUN_TEST(test_an_empty_set_says_no_data);
    RUN_TEST(test_clear_empties_but_keeps_working);
    RUN_TEST(test_more_probes_than_k_take_the_nearest_k);
    RUN_TEST(test_growth_past_the_initial_capacity);
    return UNITY_END();
}
