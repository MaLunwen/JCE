/*
 * test_jce_audio_occlusion_params.c — the occlusion solver honours the
 * parameters it is handed.
 *
 * All five JceAudioOcclusionComponent fields were inert: the runtime solved
 * with jce_audio_occlusion_default_params() and nothing else, so a designer
 * could set a cutoff, an attenuation, a radius and a layer mask and the audio
 * behaved identically.  The Inspector drew all five with NO unwired badge --
 * unlike the reverb zone right beside it, which badges its two dead fields --
 * so nothing said so either.
 *
 * The runtime side (which probe covers the listener, and the filtered
 * raycast) needs a scene and a physics world.  What is checkable here is the
 * half that has to hold for any of it to matter: that the solver's OUTPUT
 * tracks the parameters, so handing it the authored ones is not a no-op.
 *
 * Asserted:
 *   1. a fully occluded source lands on min_lowpass_hz, so the authored
 *      cutoff is what a muffled source actually gets;
 *   2. it lands on min_direct_volume, so the authored dB cut is what it
 *      actually loses;
 *   3. an UNoccluded source is untouched by either -- otherwise "honouring
 *      the parameters" would just mean "attenuating everything".
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/audio/jce_audio_occlusion.h>

#include <math.h>
#include <string.h>

/* Ray probe: 0 = blocked at the source, 1 = clear line of sight. */
static float ray_blocked(void *ud, jce_vec3 o, jce_vec3 d, float md, float *mat)
{
    (void)ud; (void)o; (void)d; (void)md;
    if (mat) *mat = 1.0f;          /* fully absorbing */
    return 0.0f;
}

static float ray_clear(void *ud, jce_vec3 o, jce_vec3 d, float md, float *mat)
{
    (void)ud; (void)o; (void)d; (void)md;
    if (mat) *mat = 0.0f;
    return 1.0f;
}

static JceAudioOcclusionQuery solve_one(const JceAudioOcclusionParams *p,
                                        bool blocked)
{
    JceAudioOcclusionQuery q;
    memset(&q, 0, sizeof q);
    q.source_position = jce_v3(10.0f, 0.0f, 0.0f);
    jce_audio_occlusion_solve(p, jce_v3(0.0f, 0.0f, 0.0f), &q, 1,
                              blocked ? ray_blocked : ray_clear, NULL);
    return q;
}

static void test_authored_cutoff_is_what_a_muffled_source_gets(void)
{
    JceAudioOcclusionParams p = jce_audio_occlusion_default_params();
    p.min_lowpass_hz = 800.0f;          /* an authored lowpass_cutoff_hz */
    JceAudioOcclusionQuery q = solve_one(&p, true);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(q.lowpass_hz), "cutoff is not finite");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, 800.0f, q.lowpass_hz,
        "a fully occluded source must land on the AUTHORED cutoff, not the "
        "400 Hz default the runtime used to hard-code");
}

static void test_authored_attenuation_is_what_it_loses(void)
{
    JceAudioOcclusionParams p = jce_audio_occlusion_default_params();
    /* -20 dB -> 0.1 linear, which is what the runtime computes from
     * AudioOcclusion.attenuation_db. */
    p.min_direct_volume = powf(10.0f, -20.0f / 20.0f);
    JceAudioOcclusionQuery q = solve_one(&p, true);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 0.1f, q.attenuation,
        "a fully occluded source must land on the AUTHORED floor");
}

static void test_clear_line_of_sight_is_untouched(void)
{
    JceAudioOcclusionParams p = jce_audio_occlusion_default_params();
    p.min_lowpass_hz    = 800.0f;
    p.min_direct_volume = 0.1f;
    JceAudioOcclusionQuery q = solve_one(&p, false);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 1.0f, q.attenuation,
        "an UNoccluded source must keep its volume -- if the authored floor "
        "applied here, honouring the parameters would just mean attenuating "
        "everything");
    TEST_ASSERT_TRUE_MESSAGE(q.lowpass_hz > p.min_lowpass_hz * 2.0f,
        "an UNoccluded source must not be muffled");
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_authored_cutoff_is_what_a_muffled_source_gets);
    RUN_TEST(test_authored_attenuation_is_what_it_loses);
    RUN_TEST(test_clear_line_of_sight_is_untouched);
    return UNITY_END();
}
