/*
 * test_jce_sh9_dering.c — JceLightProbeGroupComponent.dering.
 *
 * `dering` ("enable ring artifact reduction") was authored, serialised, shown
 * in the Inspector as a checkbox with an "unwired" badge, and read by nothing.
 *
 * Ringing is what a hard cutoff does: projecting a sharp lighting environment
 * onto L0+L1+L2 and reconstructing gives irradiance that overshoots near the
 * transition and swings NEGATIVE on the far side.  On a probe-lit object that
 * is dark banding where nothing casts a shadow.
 *
 * Four properties, and the last three matter as much as the first -- a
 * deringer that removes ringing by dimming the probe has traded a visible
 * artifact for an invisible one:
 *   1. no direction reconstructs negative afterwards
 *   2. the L0 term is EXACTLY unchanged (average irradiance preserved)
 *   3. the three channels are windowed together (no hue shift)
 *   4. a probe that does not ring is not touched at all
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_lightmapper.h>

#include <math.h>
#include <string.h>

/* Same basis the baker projects with. */
static void sh9_basis(float *sh, float x, float y, float z)
{
    sh[0] =  0.282095f;
    sh[1] =  0.488603f * y;
    sh[2] =  0.488603f * z;
    sh[3] =  0.488603f * x;
    sh[4] =  1.092548f * x * y;
    sh[5] =  1.092548f * y * z;
    sh[6] =  0.315392f * (3.0f * z * z - 1.0f);
    sh[7] =  1.092548f * x * z;
    sh[8] =  0.546274f * (x * x - y * y);
}

/* A dense, INDEPENDENT direction set: 4096 directions on a lat/long grid, not
 * the 256-point Fibonacci set the implementation optimises against.  Checking
 * non-negativity on the very samples the solver minimised over would only
 * prove the solver ran, not that the result holds. */
static float min_over_sphere(const float sh[9][3], int ch)
{
    float lo = 3.0e38f;
    for (int i = 0; i < 64; ++i) {
        const float theta = 3.14159265f * ((float)i + 0.5f) / 64.0f;
        for (int j = 0; j < 64; ++j) {
            const float phi = 6.2831853f * ((float)j + 0.5f) / 64.0f;
            const float st = sinf(theta);
            float b[9];
            sh9_basis(b, st * cosf(phi), st * sinf(phi), cosf(theta));
            float v = 0.0f;
            for (int c = 0; c < 9; ++c) v += sh[c][ch] * b[c];
            if (v < lo) lo = v;
        }
    }
    return lo;
}

/* A probe that rings: a bright directional lobe projected onto L2.  The DC
 * term is the lobe's average; the directional terms are strong enough that the
 * truncated reconstruction dips below zero opposite the lobe. */
static void make_ringing_probe(float sh[9][3])
{
    memset(sh, 0, 9 * 3 * sizeof(float));
    for (int ch = 0; ch < 3; ++ch) {
        sh[0][ch] = 1.0f;
        sh[2][ch] = 2.2f;    /* L1,0  — strong +Z lobe */
        sh[6][ch] = 1.6f;    /* L2,0 */
    }
}

static void test_a_ringing_probe_really_does_ring(void)
{
    float sh[9][3];
    make_ringing_probe(sh);
    TEST_ASSERT_TRUE_MESSAGE(min_over_sphere((const float (*)[3])sh, 0) < 0.0f,
        "the fixture must actually ring, or every assertion below is vacuous");
}

static void test_dering_removes_the_negative_lobe(void)
{
    float sh[9][3];
    make_ringing_probe(sh);
    jce_lightmapper_sh9_dering(&sh, 1);
    for (int ch = 0; ch < 3; ++ch) {
        char msg[96];
        snprintf(msg, sizeof msg,
                 "channel %d still reconstructs negative after deringing", ch);
        TEST_ASSERT_TRUE_MESSAGE(
            min_over_sphere((const float (*)[3])sh, ch) >= -1.0e-4f, msg);
    }
}

static void test_dering_preserves_average_irradiance_exactly(void)
{
    float sh[9][3];
    make_ringing_probe(sh);
    const float dc0 = sh[0][0];
    jce_lightmapper_sh9_dering(&sh, 1);
    for (int ch = 0; ch < 3; ++ch)
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(dc0, sh[0][ch],
            "the L0 band must be untouched -- windowing it would let "
            "'remove ringing' silently darken or brighten the whole scene");
}

static void test_dering_does_not_shift_hue(void)
{
    /* Red rings hardest, blue least: windowing per channel would scale them by
     * different amounts and turn a white lobe blue. */
    float sh[9][3];
    memset(sh, 0, sizeof sh);
    sh[0][0] = 1.0f; sh[0][1] = 1.0f; sh[0][2] = 1.0f;
    sh[2][0] = 2.6f; sh[2][1] = 2.2f; sh[2][2] = 1.8f;
    sh[6][0] = 1.9f; sh[6][1] = 1.6f; sh[6][2] = 1.3f;

    const float before[3] = { sh[2][0] / sh[2][1], sh[2][1] / sh[2][2],
                              sh[6][0] / sh[6][1] };
    jce_lightmapper_sh9_dering(&sh, 1);
    const float after[3]  = { sh[2][0] / sh[2][1], sh[2][1] / sh[2][2],
                              sh[6][0] / sh[6][1] };
    for (int i = 0; i < 3; ++i)
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0e-5f, before[i], after[i],
            "channel ratios must survive: one window per PROBE, taken as the "
            "max over channels, not one window per channel");
}

static void test_a_probe_that_does_not_ring_is_untouched(void)
{
    /* Ambient-dominant: a weak lobe on a strong DC term never goes negative. */
    float sh[9][3], before[9][3];
    memset(sh, 0, sizeof sh);
    for (int ch = 0; ch < 3; ++ch) { sh[0][ch] = 4.0f; sh[2][ch] = 0.3f; }
    memcpy(before, sh, sizeof sh);

    jce_lightmapper_sh9_dering(&sh, 1);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(before, sh, sizeof sh,
        "a probe that does not ring must come back bit-identical -- otherwise "
        "turning the checkbox on quietly softens every probe in the scene");
}

static void test_negative_dc_is_left_alone(void)
{
    /* A negative average is a bake defect, not ringing, and no amount of
     * windowing can fix it: leave it visible rather than rewrite it into
     * something that looks plausible. */
    float sh[9][3], before[9][3];
    memset(sh, 0, sizeof sh);
    for (int ch = 0; ch < 3; ++ch) { sh[0][ch] = -0.5f; sh[2][ch] = 2.0f; }
    memcpy(before, sh, sizeof sh);

    jce_lightmapper_sh9_dering(&sh, 1);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(before, sh, sizeof sh,
        "a probe with negative average irradiance must be left as it is");
}

static void test_every_probe_in_a_group_is_processed(void)
{
    float sh[3][9][3];
    for (int p = 0; p < 3; ++p) make_ringing_probe(sh[p]);
    jce_lightmapper_sh9_dering(sh, 3);
    for (int p = 0; p < 3; ++p) {
        char msg[64];
        snprintf(msg, sizeof msg, "probe %d was skipped", p);
        TEST_ASSERT_TRUE_MESSAGE(
            min_over_sphere((const float (*)[3])sh[p], 0) >= -1.0e-4f, msg);
    }
}

/* ── the engine-side policy: which groups get windowed at all ────────── */

/* Returns what jce_scene_light_probe_group_dering did, and whether the
 * coefficients actually moved.  The two can disagree -- that is the bug this
 * catches: a policy that reports "applied" while changing nothing. */
static void probe_group_case(bool dering, bool baked, int probe_count,
                             bool *out_applied, bool *out_changed)
{
    JceScene *s = jce_scene_create();
    JceEntity e = jce_scene_create_entity(s, "Probes");

    JceLightProbeGroupComponent g;
    memset(&g, 0, sizeof g);
    g.probe_count = probe_count;
    g.dering      = dering;
    g.sh9_baked   = baked;
    make_ringing_probe(g.sh9[0]);
    jce_scene_set_light_probe_group(s, e, &g);

    float before[9][3];
    memcpy(before, g.sh9[0], sizeof before);

    *out_applied = jce_scene_light_probe_group_dering(s, e);

    JceLightProbeGroupComponent *out = jce_scene_get_light_probe_group(s, e);
    *out_changed = out && memcmp(before, out->sh9[0], sizeof before) != 0;
    jce_scene_destroy(s);
}

static void test_policy_applies_when_asked_and_baked(void)
{
    bool applied, changed;
    probe_group_case(true, true, 1, &applied, &changed);
    TEST_ASSERT_TRUE_MESSAGE(applied, "dering + baked must apply");
    TEST_ASSERT_TRUE_MESSAGE(changed,
        "reporting applied while the coefficients did not move would make the "
        "return value a lie");
}

static void test_policy_skips_a_group_that_did_not_ask(void)
{
    bool applied, changed;
    probe_group_case(false, true, 1, &applied, &changed);
    TEST_ASSERT_FALSE_MESSAGE(applied, "dering off must not apply");
    TEST_ASSERT_FALSE_MESSAGE(changed,
        "`dering` is the field this exists for -- with it off the "
        "coefficients must come back untouched");
}

static void test_policy_skips_an_unbaked_group(void)
{
    bool applied, changed;
    probe_group_case(true, false, 1, &applied, &changed);
    TEST_ASSERT_FALSE_MESSAGE(applied, "there is nothing to window until a bake");
    TEST_ASSERT_FALSE_MESSAGE(changed, "an unbaked group must not be rewritten");
}

static void test_policy_skips_an_empty_group(void)
{
    bool applied, changed;
    probe_group_case(true, true, 0, &applied, &changed);
    TEST_ASSERT_FALSE_MESSAGE(applied, "a group with no probes has nothing to do");
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_ringing_probe_really_does_ring);
    RUN_TEST(test_dering_removes_the_negative_lobe);
    RUN_TEST(test_dering_preserves_average_irradiance_exactly);
    RUN_TEST(test_dering_does_not_shift_hue);
    RUN_TEST(test_a_probe_that_does_not_ring_is_untouched);
    RUN_TEST(test_negative_dc_is_left_alone);
    RUN_TEST(test_every_probe_in_a_group_is_processed);
    RUN_TEST(test_policy_applies_when_asked_and_baked);
    RUN_TEST(test_policy_skips_a_group_that_did_not_ask);
    RUN_TEST(test_policy_skips_an_unbaked_group);
    RUN_TEST(test_policy_skips_an_empty_group);
    return UNITY_END();
}
