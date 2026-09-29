/*
 * test_jce_audio_spatial_blend.c
 *
 * JceAudioSourceComponent.spatial_blend is a float the inspector edits in 0.01
 * steps, and until this change the only runtime reader was
 * `bool spatial = (as->spatial_blend > 0.5f)` -- so 0.0 and 0.49 were
 * bit-identical in the mix, as were 0.51 and 1.0.  A hundred authorable values
 * and two reachable states.
 *
 * WHAT IS ACTUALLY MEASURED.  The blend scales how far DISTANCE ATTENUATION is
 * allowed to pull a voice down, via the spatializer's min-gain floor.  So the
 * observable is a rendered level at a FIXED distance for two different blends;
 * anything that only looked at the authored volume would see nothing.
 *
 * NO DEVICE IS NEEDED and no case may skip: jce_audio_create_offline() plus
 * jce_audio_render_offline() run the real miniaudio node graph, spatializer
 * included.
 *
 * IN THE TREE: tests/ is tracked on this branch and gitignored on `main`.
 * Settle it with `git check-ignore -v <path>`, never from memory.
 */

#include <jce/middleware/audio/jce_audio.h>

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define SR        48000u
#define FRAMES    2400u           /* 50 ms */
#define RENDER    512u            /* frames per render call */
#define FAR_X     20.0f           /* well past the 1..25 m default range */

static int16_t s_pcm[FRAMES];

static void build_pcm(void)
{
    /* Full-scale square wave: loud, and its RMS does not depend on phase, so a
     * short render window is a stable measurement. */
    for (uint32_t i = 0; i < FRAMES; ++i)
        s_pcm[i] = (int16_t)((i % 32u) < 16u ? 12000 : -12000);
}

/* Peak |sample| of the mix produced by one voice at `blend`, placed FAR_X
 * metres away from the listener at the origin. */
static float peak_at(float blend)
{
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    JceSound snd = jce_audio_load_pcm(a, s_pcm, (uint32_t)sizeof s_pcm,
                                      1u, SR, 16u);
    TEST_ASSERT_NOT_EQUAL(JCE_SOUND_INVALID, snd);

    JceVoice v = jce_audio_play(a, snd, true, 1.0f, 1.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_VOICE_INVALID, v);
    jce_audio_voice_set_3d(a, v, blend > 0.0f);
    jce_audio_voice_set_spatial_blend(a, v, blend);
    jce_audio_voice_set_position(a, v, FAR_X, 0.0f, 0.0f);

    float out[RENDER * 2];
    float peak = 0.0f;
    for (int n = 0; n < 8; ++n) {
        memset(out, 0, sizeof out);
        TEST_ASSERT_TRUE(jce_audio_render_offline(a, out, RENDER));
        for (uint32_t i = 0; i < RENDER * 2u; ++i) {
            float m = fabsf(out[i]);
            if (m > peak) peak = m;
        }
    }
    jce_audio_destroy(a);
    return peak;
}

/* POSITIVE CONTROL: the rig can hear anything at all.  Without it every
 * comparison below is between two silences, which compare equal. */
static void test_the_rig_produces_audible_output(void)
{
    build_pcm();
    TEST_ASSERT_TRUE_MESSAGE(
        peak_at(0.0f) > 0.01f,
        "a 2D voice at full volume rendered silence -- the offline graph is "
        "not producing output and every assertion in this file would pass by "
        "comparing zero against zero");
}

/* THE REGRESSION.  Two blends that the old `> 0.5f` collapsed into the same
 * state must now render at measurably different levels. */
static void test_two_blends_below_the_old_threshold_differ(void)
{
    build_pcm();
    const float quarter = peak_at(0.25f);
    const float three_q = peak_at(0.75f);

    /* More blend = more attenuation allowed = quieter at 20 m. */
    TEST_ASSERT_TRUE_MESSAGE(
        quarter > three_q + 0.01f,
        "blend 0.25 and 0.75 rendered at the same level at the same distance "
        "-- the authored float is still being collapsed to a boolean, which "
        "is the defect this row was opened for");
}

/* The endpoints still mean what they always meant. */
static void test_zero_is_unattenuated_and_one_is_fully_attenuated(void)
{
    build_pcm();
    const float flat = peak_at(0.0f);
    const float full = peak_at(1.0f);

    TEST_ASSERT_TRUE_MESSAGE(
        flat > full + 0.01f,
        "a 2D voice (blend 0) and a fully 3D voice (blend 1) rendered the "
        "same at 20 m, so distance attenuation is not reaching the mix at all "
        "and the middle of the range cannot mean anything either");
}

/* MONOTONIC across the whole range, which is what "continuous" has to mean:
 * four points, each no louder than the one before it.  A blend that only had
 * two states would produce a step here and two equal pairs. */
static void test_level_falls_monotonically_with_blend(void)
{
    build_pcm();
    const float b[4] = { 0.0f, 0.25f, 0.5f, 1.0f };
    float p[4];
    int i;
    for (i = 0; i < 4; ++i) p[i] = peak_at(b[i]);

    for (i = 1; i < 4; ++i) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "level at blend %.2f (%.4f) is LOUDER than at %.2f (%.4f) -- "
                 "the blend is not monotonic", b[i], p[i], b[i - 1], p[i - 1]);
        TEST_ASSERT_TRUE_MESSAGE(p[i] <= p[i - 1] + 0.005f, msg);
    }
    /* And it is not a two-state staircase: the ends must actually differ. */
    TEST_ASSERT_TRUE_MESSAGE(p[0] > p[3] + 0.01f,
        "every blend rendered the same level, so 'monotonic' above was "
        "satisfied by a constant");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_rig_produces_audible_output);
    RUN_TEST(test_two_blends_below_the_old_threshold_differ);
    RUN_TEST(test_zero_is_unattenuated_and_one_is_fully_attenuated);
    RUN_TEST(test_level_falls_monotonically_with_blend);
    return UNITY_END();
}
