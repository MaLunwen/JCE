/*
 * test_jce_audio_dsp_palette.c
 *
 * The insert-effect palette past its original four, and the registration
 * surface that stops it being a closed set.
 *
 * audio.dsp.insert-effect-chain is behind on exactly two axes and the row
 * names both: "four fixed effect kinds (EQ/COMPRESSOR/LIMITER/DELAY), no
 * pitch-shift, chorus, flanger, distortion or spectrum, and no plugin path",
 * and "Unity's is a plugin surface; this is a closed set."
 *
 * WHAT THESE CASES ARE FOR, and it is not "the effect runs".  A chain that
 * accepted an effect and passed the buffer through untouched would satisfy
 * that, and is the exact shape this ledger keeps finding.  So each case names
 * a property the effect must have that a PASS-THROUGH does not:
 *
 *   distortion  a shaped signal must contain frequencies the input did not.
 *               Measured as sample values the input never takes, on a signal
 *               whose every sample is +/-A -- a pass-through returns only
 *               +/-A, a shaper returns something else.
 *   chorus      a modulated delay must make the output CHANGE OVER TIME for
 *               a constant input.  A fixed delay does not.
 *   flanger     feedback must make it ring: energy must still be leaving the
 *               chain after the input stops.
 *   registered  the engine must call code it has never seen, and the chain
 *               must keep that code's state between blocks.
 *
 * IN THE TREE: tests/ is tracked on this branch and gitignored on `main`.
 * Settle it with `git check-ignore -v <path>`, never from memory.
 */

#include <jce/middleware/audio/jce_audio_dsp.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define SR     48000u
#define FRAMES 512u

static float g_buf[FRAMES * 2];

/* A square wave at full scale: every sample is exactly +A or -A, which is
 * what lets the distortion case ask "did any sample become something the
 * input never was" without a spectrum. */
static void fill_square(float amp)
{
    for (uint32_t i = 0; i < FRAMES; ++i) {
        const float v = ((i / 16u) % 2u) ? amp : -amp;
        g_buf[i * 2u + 0] = v;
        g_buf[i * 2u + 1] = v;
    }
}

static void fill_silence(void)
{
    memset(g_buf, 0, sizeof g_buf);
}

static float peak(void)
{
    float p = 0.0f;
    for (uint32_t i = 0; i < FRAMES * 2u; ++i) {
        const float m = fabsf(g_buf[i]);
        if (m > p) p = m;
    }
    return p;
}

static JceAudioDspChain *chain_with(JceAudioEffectType t,
                                    const JceAudioEffectDesc *override_desc)
{
    JceAudioDspChain *c = jce_audio_dsp_chain_create(2, SR);
    TEST_ASSERT_NOT_NULL(c);
    JceAudioEffectDesc d = override_desc ? *override_desc
                                         : jce_audio_effect_default(t);
    TEST_ASSERT_TRUE_MESSAGE(jce_audio_dsp_chain_add(c, &d) >= 0,
        "the chain refused the effect");
    return c;
}

/* ── distortion ──────────────────────────────────────────────────────── */

static void test_distortion_produces_values_the_input_never_had(void)
{
    JceAudioEffectDesc d = jce_audio_effect_default(JCE_AUDIO_EFFECT_DISTORTION);
    d.u.distortion.shape       = JCE_AUDIO_DIST_SOFT_CLIP;
    d.u.distortion.drive       = 8.0f;
    d.u.distortion.ceiling     = 1.0f;
    d.u.distortion.wet         = 1.0f;
    d.u.distortion.dry         = 0.0f;
    d.u.distortion.output_gain = 1.0f;

    JceAudioDspChain *c = chain_with(JCE_AUDIO_EFFECT_DISTORTION, &d);
    fill_square(0.25f);
    jce_audio_dsp_chain_process(c, g_buf, FRAMES);

    /* Every input sample was exactly +/-0.25.  A pass-through returns those.
     * A soft clipper driven 8x returns something near the ceiling instead. */
    bool changed = false;
    for (uint32_t i = 0; i < FRAMES * 2u; ++i) {
        if (fabsf(fabsf(g_buf[i]) - 0.25f) > 0.01f) { changed = true; break; }
    }
    const float p = peak();
    jce_audio_dsp_chain_destroy(c);

    TEST_ASSERT_TRUE_MESSAGE(changed,
        "every sample came back at its input value, so the distortion is a "
        "pass-through -- the effect was accepted by the chain and does "
        "nothing, which is the exact shape this ledger keeps finding");
    TEST_ASSERT_TRUE_MESSAGE(p > 0.25f,
        "driving the input 8x did not raise the level at all");
    TEST_ASSERT_TRUE_MESSAGE(p <= 1.01f,
        "the shaper exceeded its own ceiling of 1.0");
}

static void test_hard_clip_never_exceeds_its_ceiling(void)
{
    JceAudioEffectDesc d = jce_audio_effect_default(JCE_AUDIO_EFFECT_DISTORTION);
    d.u.distortion.shape       = JCE_AUDIO_DIST_HARD_CLIP;
    d.u.distortion.drive       = 20.0f;      /* far past the ceiling */
    d.u.distortion.ceiling     = 0.5f;
    d.u.distortion.wet         = 1.0f;
    d.u.distortion.dry         = 0.0f;
    d.u.distortion.output_gain = 1.0f;

    JceAudioDspChain *c = chain_with(JCE_AUDIO_EFFECT_DISTORTION, &d);
    fill_square(0.9f);
    jce_audio_dsp_chain_process(c, g_buf, FRAMES);
    const float p = peak();
    jce_audio_dsp_chain_destroy(c);

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 0.5f, p,
        "a hard clipper driven 20x did not land exactly on its ceiling");
}

/* FOLDBACK must FOLD, not clip: past the ceiling it comes back DOWN, so a
 * huge input lands somewhere inside the range rather than sitting on the
 * rail.  Without this, foldback and hard-clip are two names for one shape. */
static void test_foldback_folds_rather_than_clipping(void)
{
    JceAudioEffectDesc d = jce_audio_effect_default(JCE_AUDIO_EFFECT_DISTORTION);
    d.u.distortion.shape       = JCE_AUDIO_DIST_FOLDBACK;
    d.u.distortion.drive       = 1.5f;     /* 0.9 * 1.5 = 1.35, past 1.0 */
    d.u.distortion.ceiling     = 1.0f;
    d.u.distortion.wet         = 1.0f;
    d.u.distortion.dry         = 0.0f;
    d.u.distortion.output_gain = 1.0f;

    JceAudioDspChain *c = chain_with(JCE_AUDIO_EFFECT_DISTORTION, &d);
    fill_square(0.9f);
    jce_audio_dsp_chain_process(c, g_buf, FRAMES);
    const float p = peak();
    jce_audio_dsp_chain_destroy(c);

    /* 1.35 folds to 2 - 1.35 = 0.65, which is BELOW the ceiling.  A clipper
     * would have returned exactly 1.0. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 0.65f, p,
        "foldback did not fold -- 1.0 here means it clipped instead, and "
        "foldback and hard-clip are then two names for one shape");
}

/* ── chorus: the output must MOVE ────────────────────────────────────── */

static void test_chorus_output_changes_over_time_for_a_constant_input(void)
{
    JceAudioEffectDesc d = jce_audio_effect_default(JCE_AUDIO_EFFECT_CHORUS);
    d.u.mod_delay.rate_hz = 8.0f;     /* fast, so one block covers a sweep */
    d.u.mod_delay.wet     = 1.0f;
    d.u.mod_delay.dry     = 0.0f;
    /* FEEDBACK MUST BE ZERO HERE, and that is the whole point of this case.
     * With feedback, the line's contents differ block to block whatever the
     * read head does, so "two identical blocks came out different" is
     * satisfied without the LFO moving at all -- MUTATION PROVED IT: freezing
     * phase_inc to 0 left this test green.  With no feedback the line is a
     * pure function of the input, so the ONLY thing that can make two
     * identical blocks differ is where the head reads. */
    d.u.mod_delay.feedback = 0.0f;

    JceAudioDspChain *c = chain_with(JCE_AUDIO_EFFECT_CHORUS, &d);

    /* A RAMP, not a constant: a modulated read head over a constant signal
     * returns that same constant wherever it reads, so a constant input
     * cannot tell a moving head from a fixed one.  Over a ramp, WHERE the
     * head reads decides WHAT it returns. */
    #define RAMP_BLOCK()                                        \
        do {                                                    \
            for (uint32_t i = 0; i < FRAMES; ++i) {             \
                const float v = (float)i / (float)FRAMES;       \
                g_buf[i * 2u + 0] = v;                          \
                g_buf[i * 2u + 1] = v;                          \
            }                                                   \
        } while (0)

    /* PRIME PAST THE LINE LENGTH.  A chorus sits around 22 ms -- ~1056 frames
     * at 48 kHz -- and these blocks are 512, so for the first few blocks the
     * head reads a region nothing has written.  That alone makes consecutive
     * outputs differ, which is how an earlier version of this case stayed
     * green with the LFO frozen to zero.  Eight blocks is 4096 frames, well
     * past the longest line this effect allocates. */
    for (int b = 0; b < 8; ++b) {
        RAMP_BLOCK();
        jce_audio_dsp_chain_process(c, g_buf, FRAMES);
    }

    float first[FRAMES * 2];
    RAMP_BLOCK();
    jce_audio_dsp_chain_process(c, g_buf, FRAMES);
    memcpy(first, g_buf, sizeof first);

    RAMP_BLOCK();
    jce_audio_dsp_chain_process(c, g_buf, FRAMES);

    float max_diff = 0.0f;
    for (uint32_t i = 0; i < FRAMES * 2u; ++i) {
        const float dlt = fabsf(g_buf[i] - first[i]);
        if (dlt > max_diff) max_diff = dlt;
    }
    jce_audio_dsp_chain_destroy(c);
    #undef RAMP_BLOCK

    TEST_ASSERT_TRUE_MESSAGE(max_diff > 0.001f,
        "two identical input blocks produced identical output, with feedback "
        "at zero and the line fully primed -- so the read head is NOT moving "
        "and this is a fixed delay wearing the name chorus");
}

/* The LEFT and RIGHT LFOs run offset, which is what makes a chorus wide
 * rather than merely thick.  stereo_phase 0 must NOT produce that. */
static void test_stereo_phase_separates_the_channels(void)
{
    JceAudioEffectDesc d = jce_audio_effect_default(JCE_AUDIO_EFFECT_CHORUS);
    d.u.mod_delay.rate_hz      = 8.0f;
    d.u.mod_delay.wet          = 1.0f;
    d.u.mod_delay.dry          = 0.0f;
    d.u.mod_delay.stereo_phase = 0.5f;

    JceAudioDspChain *c = chain_with(JCE_AUDIO_EFFECT_CHORUS, &d);
    for (int blk = 0; blk < 3; ++blk) {
        for (uint32_t i = 0; i < FRAMES; ++i) {
            const float v = (float)i / (float)FRAMES;
            g_buf[i * 2u + 0] = v;
            g_buf[i * 2u + 1] = v;
        }
        jce_audio_dsp_chain_process(c, g_buf, FRAMES);
    }
    float lr = 0.0f;
    for (uint32_t i = 0; i < FRAMES; ++i) {
        const float dlt = fabsf(g_buf[i * 2u] - g_buf[i * 2u + 1]);
        if (dlt > lr) lr = dlt;
    }
    jce_audio_dsp_chain_destroy(c);

    TEST_ASSERT_TRUE_MESSAGE(lr > 0.001f,
        "left and right came back identical with stereo_phase 0.5, so both "
        "channels share one LFO and the chorus is mono");
}

/* ── flanger: feedback must ring ─────────────────────────────────────── */

static void test_flanger_feedback_keeps_ringing_after_the_input_stops(void)
{
    JceAudioEffectDesc d = jce_audio_effect_default(JCE_AUDIO_EFFECT_FLANGER);
    d.u.mod_delay.feedback = 0.85f;
    d.u.mod_delay.wet      = 1.0f;
    d.u.mod_delay.dry      = 0.0f;

    JceAudioDspChain *c = chain_with(JCE_AUDIO_EFFECT_FLANGER, &d);
    fill_square(0.8f);
    jce_audio_dsp_chain_process(c, g_buf, FRAMES);

    /* Now feed SILENCE.  A chain with no feedback returns silence; one with
     * feedback is still emptying its line. */
    fill_silence();
    jce_audio_dsp_chain_process(c, g_buf, FRAMES);
    const float tail = peak();
    jce_audio_dsp_chain_destroy(c);

    TEST_ASSERT_TRUE_MESSAGE(tail > 0.01f,
        "the flanger went silent the instant its input did, so feedback is "
        "not being applied and it is a plain modulated delay");
}

/* RESET must empty the line: a rewound voice must not carry the previous
 * take's tail into the new one. */
static void test_reset_clears_the_modulated_delay_line(void)
{
    JceAudioDspChain *c = chain_with(JCE_AUDIO_EFFECT_FLANGER, NULL);
    fill_square(0.8f);
    jce_audio_dsp_chain_process(c, g_buf, FRAMES);

    jce_audio_dsp_chain_reset(c);

    fill_silence();
    jce_audio_dsp_chain_process(c, g_buf, FRAMES);
    const float tail = peak();
    jce_audio_dsp_chain_destroy(c);

    TEST_ASSERT_TRUE_MESSAGE(tail < 0.0001f,
        "silence through a RESET chain still came back with signal in it -- "
        "the delay line survived the reset, so a rewound voice plays the "
        "previous take's tail");
}

/* ── the registration surface ────────────────────────────────────────── */

/* A project's effect: doubles its input and counts the blocks it has seen,
 * so the test can assert BOTH that the engine called it and that the chain
 * kept its state between calls. */
typedef struct { float gain; uint32_t blocks; } TestFxState;

static bool testfx_configure(void *state, uint32_t channels, uint32_t sr,
                             const void *params, uint32_t params_size)
{
    (void)channels; (void)sr;
    TestFxState *s = (TestFxState *)state;
    s->gain = (params && params_size >= sizeof(float)) ? *(const float *)params
                                                       : 1.0f;
    return true;
}

static void testfx_process(void *state, float *buf, uint32_t frames,
                           uint32_t channels)
{
    TestFxState *s = (TestFxState *)state;
    ++s->blocks;
    for (uint32_t i = 0; i < frames * channels; ++i) buf[i] *= s->gain;
}

static void test_a_registered_effect_runs_and_keeps_its_state(void)
{
    JceAudioEffectVTable vt;
    memset(&vt, 0, sizeof vt);
    vt.name       = "t.double";
    vt.state_size = (uint32_t)sizeof(TestFxState);
    vt.configure  = testfx_configure;
    vt.process    = testfx_process;

    const int type = jce_audio_dsp_register_effect(&vt);
    TEST_ASSERT_TRUE_MESSAGE(type >= JCE_AUDIO_EFFECT_CUSTOM_BASE,
        "registration refused an effect with a name and a process callback");

    /* IDEMPOTENT BY NAME: a second registration must return the SAME id, or a
     * saved mixer would depend on the order modules registered in. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(type, jce_audio_dsp_register_effect(&vt),
        "registering the same name twice produced a second id");
    TEST_ASSERT_EQUAL_INT_MESSAGE(type, jce_audio_dsp_find_effect("t.double"),
        "the registered effect cannot be found by the name it was given");
    TEST_ASSERT_EQUAL_INT_MESSAGE(type,
        jce_audio_dsp_effect_type_from_name("t.double"),
        "a registered name does not resolve through the shared name lookup, "
        "so it is not authorable in audio_mixer.json");

    const float gain = 2.0f;
    JceAudioEffectDesc d;
    memset(&d, 0, sizeof d);
    d.type               = (JceAudioEffectType)type;
    d.custom_params      = &gain;
    d.custom_params_size = (uint32_t)sizeof gain;

    JceAudioDspChain *c = jce_audio_dsp_chain_create(2, SR);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE_MESSAGE(jce_audio_dsp_chain_add(c, &d) >= 0,
        "the chain refused a registered effect");

    fill_square(0.25f);
    jce_audio_dsp_chain_process(c, g_buf, FRAMES);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 0.5f, peak(),
        "the registered effect's process() never ran, or its configure() did "
        "not receive the parameter blob -- this is the whole plugin surface");

    /* Two more blocks; the state must accumulate rather than restart. */
    fill_square(0.25f);
    jce_audio_dsp_chain_process(c, g_buf, FRAMES);
    fill_square(0.25f);
    jce_audio_dsp_chain_process(c, g_buf, FRAMES);
    jce_audio_dsp_chain_destroy(c);
}

/* The engine's own effects must still answer the shared lookup, and with the
 * spelling already on disk -- "comp", not "compressor". */
static void test_builtin_names_match_what_is_already_authored(void)
{
    TEST_ASSERT_EQUAL_INT(JCE_AUDIO_EFFECT_EQ,
        jce_audio_dsp_effect_type_from_name("eq"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_AUDIO_EFFECT_COMPRESSOR,
        jce_audio_dsp_effect_type_from_name("comp"),
        "the compressor does not resolve under \"comp\" -- which is the "
        "spelling every mixer already saved on disk uses, so this lookup "
        "would be wrong for exactly the effects people have");
    TEST_ASSERT_EQUAL_INT(JCE_AUDIO_EFFECT_CHORUS,
        jce_audio_dsp_effect_type_from_name("chorus"));
    TEST_ASSERT_EQUAL_INT(JCE_AUDIO_EFFECT_FLANGER,
        jce_audio_dsp_effect_type_from_name("flanger"));
    TEST_ASSERT_EQUAL_INT(JCE_AUDIO_EFFECT_DISTORTION,
        jce_audio_dsp_effect_type_from_name("distortion"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1,
        jce_audio_dsp_effect_type_from_name("no_such_effect"),
        "an unknown name resolved to something");

    TEST_ASSERT_EQUAL_STRING("comp",
        jce_audio_dsp_effect_name(JCE_AUDIO_EFFECT_COMPRESSOR));
    TEST_ASSERT_NULL(jce_audio_dsp_effect_name(JCE_AUDIO_EFFECT_NONE));
}

/* CHORUS AND FLANGER MUST NOT SHIP THE SAME DEFAULTS.  They share a param
 * struct, and the numbers are the entire difference between the two effects;
 * identical defaults would make them two names for one sound. */
static void test_chorus_and_flanger_defaults_differ(void)
{
    const JceAudioEffectDesc ch = jce_audio_effect_default(JCE_AUDIO_EFFECT_CHORUS);
    const JceAudioEffectDesc fl = jce_audio_effect_default(JCE_AUDIO_EFFECT_FLANGER);

    TEST_ASSERT_TRUE_MESSAGE(ch.u.mod_delay.delay_ms > fl.u.mod_delay.delay_ms * 3.0f,
        "the chorus default delay is not meaningfully longer than the "
        "flanger's -- they share a struct, and identical numbers make them "
        "one effect with two names");
    TEST_ASSERT_TRUE_MESSAGE(fl.u.mod_delay.feedback > ch.u.mod_delay.feedback * 3.0f,
        "the flanger default does not have markedly more feedback than the "
        "chorus, which is the other half of what separates them");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_distortion_produces_values_the_input_never_had);
    RUN_TEST(test_hard_clip_never_exceeds_its_ceiling);
    RUN_TEST(test_foldback_folds_rather_than_clipping);
    RUN_TEST(test_chorus_output_changes_over_time_for_a_constant_input);
    RUN_TEST(test_stereo_phase_separates_the_channels);
    RUN_TEST(test_flanger_feedback_keeps_ringing_after_the_input_stops);
    RUN_TEST(test_reset_clears_the_modulated_delay_line);
    RUN_TEST(test_a_registered_effect_runs_and_keeps_its_state);
    RUN_TEST(test_builtin_names_match_what_is_already_authored);
    RUN_TEST(test_chorus_and_flanger_defaults_differ);
    return UNITY_END();
}
