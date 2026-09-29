/*
 * test_jce_audio_dsp.c — Unit tests for jce_audio_dsp.h (FEATURE 5.1).
 *
 * Exercises the REAL insert-effect DSP math offline: a known PCM buffer (unit
 * impulse / 1 kHz sine) is run through each effect via jce_audio_dsp_chain_*
 * and the output is asserted against the expected spectral/dynamic behaviour.
 * No miniaudio, no audio device — the same chain code that runs live in the
 * node graph is driven here over a plain float buffer.
 */

#include "unity.h"

#include <jce/middleware/audio/jce_audio_dsp.h>

#include <math.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define SR        48000u
#define PI        3.14159265358979323846

/* ── helpers ───────────────────────────────────────────────────────── */

/* Peak absolute amplitude of an interleaved mono buffer. */
static float buf_peak(const float *b, uint32_t n)
{
    float p = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        float a = b[i] < 0.0f ? -b[i] : b[i];
        if (a > p) p = a;
    }
    return p;
}

/* RMS of an interleaved mono buffer. */
static float buf_rms(const float *b, uint32_t n)
{
    double acc = 0.0;
    for (uint32_t i = 0; i < n; ++i) acc += (double)b[i] * (double)b[i];
    return (float)sqrt(acc / (double)(n ? n : 1));
}

/* Fill `b` with a mono sine of `freq` Hz at amplitude `amp`. */
static void fill_sine(float *b, uint32_t n, double freq, float amp)
{
    for (uint32_t i = 0; i < n; ++i)
        b[i] = amp * (float)sin(2.0 * PI * freq * (double)i / (double)SR);
}

/* Goertzel single-bin magnitude (RMS-normalised) of `freq` in a mono buffer. */
static float goertzel_mag(const float *b, uint32_t n, double freq)
{
    double w = 2.0 * PI * freq / (double)SR;
    double coeff = 2.0 * cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        s0 = (double)b[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    if (power < 0.0) power = 0.0;
    return (float)(sqrt(power) / (double)(n / 2));
}

/* ── chain lifecycle ───────────────────────────────────────────────── */

static void test_chain_create_destroy(void)
{
    JceAudioDspChain *c = jce_audio_dsp_chain_create(2, SR);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_audio_dsp_chain_count(c));
    jce_audio_dsp_chain_destroy(c);
    jce_audio_dsp_chain_destroy(NULL);                 /* null-safe */

    TEST_ASSERT_NULL(jce_audio_dsp_chain_create(0, SR));  /* bad channels */
    TEST_ASSERT_NULL(jce_audio_dsp_chain_create(3, SR));  /* > stereo     */
    TEST_ASSERT_NULL(jce_audio_dsp_chain_create(1, 0));   /* bad rate     */
}

static void test_empty_chain_is_passthrough(void)
{
    JceAudioDspChain *c = jce_audio_dsp_chain_create(1, SR);
    enum { N = 256 };
    float buf[N], orig[N];
    fill_sine(buf, N, 1000.0, 0.5f);
    memcpy(orig, buf, sizeof(buf));
    jce_audio_dsp_chain_process(c, buf, N);
    for (int i = 0; i < N; ++i)
        TEST_ASSERT_FLOAT_WITHIN(1e-6f, orig[i], buf[i]);
    jce_audio_dsp_chain_destroy(c);
}

static void test_add_remove_reorder(void)
{
    JceAudioDspChain *c = jce_audio_dsp_chain_create(1, SR);
    JceAudioEffectDesc eq  = jce_audio_effect_default(JCE_AUDIO_EFFECT_EQ);
    JceAudioEffectDesc cmp = jce_audio_effect_default(JCE_AUDIO_EFFECT_COMPRESSOR);
    JceAudioEffectDesc lim = jce_audio_effect_default(JCE_AUDIO_EFFECT_LIMITER);

    TEST_ASSERT_EQUAL_INT(0, jce_audio_dsp_chain_add(c, &eq));
    TEST_ASSERT_EQUAL_INT(1, jce_audio_dsp_chain_add(c, &cmp));
    TEST_ASSERT_EQUAL_INT(2, jce_audio_dsp_chain_add(c, &lim));
    TEST_ASSERT_EQUAL_UINT32(3u, jce_audio_dsp_chain_count(c));
    TEST_ASSERT_EQUAL_INT(JCE_AUDIO_EFFECT_EQ,         jce_audio_dsp_chain_type(c, 0));
    TEST_ASSERT_EQUAL_INT(JCE_AUDIO_EFFECT_COMPRESSOR, jce_audio_dsp_chain_type(c, 1));
    TEST_ASSERT_EQUAL_INT(JCE_AUDIO_EFFECT_LIMITER,    jce_audio_dsp_chain_type(c, 2));

    /* Removing the middle effect shifts the limiter down. */
    TEST_ASSERT_TRUE(jce_audio_dsp_chain_remove(c, 1));
    TEST_ASSERT_EQUAL_UINT32(2u, jce_audio_dsp_chain_count(c));
    TEST_ASSERT_EQUAL_INT(JCE_AUDIO_EFFECT_EQ,      jce_audio_dsp_chain_type(c, 0));
    TEST_ASSERT_EQUAL_INT(JCE_AUDIO_EFFECT_LIMITER, jce_audio_dsp_chain_type(c, 1));
    TEST_ASSERT_FALSE(jce_audio_dsp_chain_remove(c, 9));   /* out of range */

    jce_audio_dsp_chain_clear(c);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_audio_dsp_chain_count(c));
    jce_audio_dsp_chain_destroy(c);
}

static void test_chain_full_rejects_extra(void)
{
    JceAudioDspChain *c = jce_audio_dsp_chain_create(1, SR);
    JceAudioEffectDesc eq = jce_audio_effect_default(JCE_AUDIO_EFFECT_EQ);
    int added = 0;
    for (int i = 0; i < JCE_AUDIO_DSP_MAX_EFFECTS + 4; ++i)
        if (jce_audio_dsp_chain_add(c, &eq) >= 0) added++;
    TEST_ASSERT_EQUAL_INT(JCE_AUDIO_DSP_MAX_EFFECTS, added);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_AUDIO_DSP_MAX_EFFECTS,
                             jce_audio_dsp_chain_count(c));
    jce_audio_dsp_chain_destroy(c);
}

/* ── EQ: changes spectrum/amplitude as expected ────────────────────── */

static void test_eq_peaking_boosts_target_band(void)
{
    JceAudioDspChain *c = jce_audio_dsp_chain_create(1, SR);
    JceAudioEffectDesc eq = jce_audio_effect_default(JCE_AUDIO_EFFECT_EQ);
    eq.u.eq.shape        = JCE_AUDIO_EQ_PEAKING;
    eq.u.eq.frequency_hz = 1000.0f;
    eq.u.eq.gain_db      = 12.0f;          /* +12 dB ≈ ×3.98 */
    eq.u.eq.q            = 1.0f;
    jce_audio_dsp_chain_add(c, &eq);

    enum { N = 8192 };
    static float buf[N];
    fill_sine(buf, N, 1000.0, 0.2f);
    float in_mag = goertzel_mag(buf, N, 1000.0);
    jce_audio_dsp_chain_process(c, buf, N);
    float out_mag = goertzel_mag(buf, N, 1000.0);

    float ratio = out_mag / in_mag;        /* expect ≈ 3.98 (+12 dB) */
    TEST_ASSERT_FLOAT_WITHIN(0.4f, 3.98f, ratio);
    jce_audio_dsp_chain_destroy(c);
}

static void test_eq_peaking_leaves_far_band_alone(void)
{
    JceAudioDspChain *c = jce_audio_dsp_chain_create(1, SR);
    JceAudioEffectDesc eq = jce_audio_effect_default(JCE_AUDIO_EFFECT_EQ);
    eq.u.eq.shape        = JCE_AUDIO_EQ_PEAKING;
    eq.u.eq.frequency_hz = 8000.0f;        /* boost up high … */
    eq.u.eq.gain_db      = 12.0f;
    eq.u.eq.q            = 2.0f;
    jce_audio_dsp_chain_add(c, &eq);

    enum { N = 8192 };
    static float buf[N];
    fill_sine(buf, N, 200.0, 0.2f);        /* … probe down low */
    float in_mag = goertzel_mag(buf, N, 200.0);
    jce_audio_dsp_chain_process(c, buf, N);
    float out_mag = goertzel_mag(buf, N, 200.0);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.0f, out_mag / in_mag);  /* ~unchanged */
    jce_audio_dsp_chain_destroy(c);
}

static void test_eq_lowpass_kills_highs_keeps_lows(void)
{
    enum { N = 8192 };
    static float low[N], high[N];

    /* Low-pass at 1 kHz: a 200 Hz tone passes, a 12 kHz tone is crushed. */
    JceAudioDspChain *c = jce_audio_dsp_chain_create(1, SR);
    JceAudioEffectDesc eq = jce_audio_effect_default(JCE_AUDIO_EFFECT_EQ);
    eq.u.eq.shape        = JCE_AUDIO_EQ_LOW_PASS;
    eq.u.eq.frequency_hz = 1000.0f;
    eq.u.eq.q            = 0.707f;
    jce_audio_dsp_chain_add(c, &eq);

    fill_sine(low, N, 200.0, 0.3f);
    float low_in = goertzel_mag(low, N, 200.0);
    jce_audio_dsp_chain_process(c, low, N);
    float low_out = goertzel_mag(low, N, 200.0);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.0f, low_out / low_in);  /* passes */

    jce_audio_dsp_chain_reset(c);                              /* clear state */
    fill_sine(high, N, 12000.0, 0.3f);
    float high_in = goertzel_mag(high, N, 12000.0);
    jce_audio_dsp_chain_process(c, high, N);
    float high_out = goertzel_mag(high, N, 12000.0);
    TEST_ASSERT_TRUE(high_out < high_in * 0.1f);              /* >20 dB down */
    jce_audio_dsp_chain_destroy(c);
}

/* ── Compressor: above threshold reduces gain ≈ by ratio ───────────── */

static void test_compressor_reduces_gain_above_threshold(void)
{
    JceAudioDspChain *c = jce_audio_dsp_chain_create(1, SR);
    JceAudioEffectDesc cmp = jce_audio_effect_default(JCE_AUDIO_EFFECT_COMPRESSOR);
    cmp.u.comp.threshold_db = -20.0f;
    cmp.u.comp.ratio        = 4.0f;
    cmp.u.comp.attack_ms    = 1.0f;
    cmp.u.comp.release_ms   = 50.0f;
    cmp.u.comp.makeup_db    = 0.0f;
    cmp.u.comp.knee_db      = 0.0f;
    jce_audio_dsp_chain_add(c, &cmp);

    /* Input at 0 dBFS (amp 1.0) => 20 dB over a -20 dB threshold.
     * With 4:1, output should be ~ -20 + 20/4 = -15 dBFS (amp ≈ 0.178). */
    enum { N = SR };                        /* 1 s — long enough to settle */
    static float buf[N];
    fill_sine(buf, N, 1000.0, 1.0f);
    jce_audio_dsp_chain_process(c, buf, N);

    /* Measure the settled tail (last quarter) to skip the attack ramp. */
    const float *tail = buf + (N - N / 4);
    float pk = buf_peak(tail, N / 4);
    float pk_db = 20.0f * (float)log10((double)pk);
    TEST_ASSERT_FLOAT_WITHIN(2.0f, -15.0f, pk_db);
    jce_audio_dsp_chain_destroy(c);
}

static void test_compressor_leaves_quiet_signal_alone(void)
{
    JceAudioDspChain *c = jce_audio_dsp_chain_create(1, SR);
    JceAudioEffectDesc cmp = jce_audio_effect_default(JCE_AUDIO_EFFECT_COMPRESSOR);
    cmp.u.comp.threshold_db = -6.0f;       /* high threshold */
    cmp.u.comp.ratio        = 8.0f;
    cmp.u.comp.attack_ms    = 1.0f;
    cmp.u.comp.release_ms   = 50.0f;
    jce_audio_dsp_chain_add(c, &cmp);

    enum { N = 4096 };
    static float buf[N];
    fill_sine(buf, N, 1000.0, 0.1f);       /* -20 dBFS, below threshold */
    float in_pk = buf_peak(buf, N);
    jce_audio_dsp_chain_process(c, buf, N);
    float out_pk = buf_peak(buf, N);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, in_pk, out_pk);  /* untouched */
    jce_audio_dsp_chain_destroy(c);
}

static void test_compressor_makeup_restores_level(void)
{
    JceAudioDspChain *c = jce_audio_dsp_chain_create(1, SR);
    JceAudioEffectDesc cmp = jce_audio_effect_default(JCE_AUDIO_EFFECT_COMPRESSOR);
    cmp.u.comp.threshold_db = -20.0f;
    cmp.u.comp.ratio        = 4.0f;
    cmp.u.comp.attack_ms    = 1.0f;
    cmp.u.comp.release_ms   = 50.0f;
    cmp.u.comp.makeup_db    = 5.0f;        /* +5 dB makeup */
    jce_audio_dsp_chain_add(c, &cmp);

    enum { N = SR };
    static float buf[N];
    fill_sine(buf, N, 1000.0, 1.0f);
    jce_audio_dsp_chain_process(c, buf, N);
    const float *tail = buf + (N - N / 4);
    float pk_db = 20.0f * (float)log10((double)buf_peak(tail, N / 4));
    /* -15 dB compressed + 5 dB makeup ≈ -10 dBFS. */
    TEST_ASSERT_FLOAT_WITHIN(2.0f, -10.0f, pk_db);
    jce_audio_dsp_chain_destroy(c);
}

/* ── Limiter: clamps peaks to the ceiling ──────────────────────────── */

static void test_limiter_clamps_to_ceiling(void)
{
    JceAudioDspChain *c = jce_audio_dsp_chain_create(1, SR);
    JceAudioEffectDesc lim = jce_audio_effect_default(JCE_AUDIO_EFFECT_LIMITER);
    lim.u.limiter.ceiling_db = -6.0f;      /* ceiling ≈ 0.501 linear */
    lim.u.limiter.release_ms = 20.0f;
    jce_audio_dsp_chain_add(c, &lim);

    enum { N = 4096 };
    static float buf[N];
    fill_sine(buf, N, 500.0, 1.0f);        /* full-scale input */
    jce_audio_dsp_chain_process(c, buf, N);

    float ceil_lin = (float)pow(10.0, -6.0 / 20.0);  /* ≈ 0.5012 */
    float pk = buf_peak(buf, N);
    /* Output peak must not exceed the ceiling (allow tiny FP slack). */
    TEST_ASSERT_TRUE(pk <= ceil_lin + 1e-3f);
    /* And the limiter must actually be working near the ceiling, not silence. */
    float tail_pk = buf_peak(buf + N / 2, N / 2);
    TEST_ASSERT_TRUE(tail_pk > ceil_lin * 0.8f);
    jce_audio_dsp_chain_destroy(c);
}

static void test_limiter_passes_signal_below_ceiling(void)
{
    JceAudioDspChain *c = jce_audio_dsp_chain_create(1, SR);
    JceAudioEffectDesc lim = jce_audio_effect_default(JCE_AUDIO_EFFECT_LIMITER);
    lim.u.limiter.ceiling_db = 0.0f;       /* ceiling = 1.0 */
    lim.u.limiter.release_ms = 20.0f;
    jce_audio_dsp_chain_add(c, &lim);

    enum { N = 2048 };
    static float buf[N], orig[N];
    fill_sine(buf, N, 500.0, 0.4f);        /* well below 1.0 */
    memcpy(orig, buf, sizeof(buf));
    jce_audio_dsp_chain_process(c, buf, N);
    for (int i = 0; i < N; ++i)
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, orig[i], buf[i]);  /* untouched */
    jce_audio_dsp_chain_destroy(c);
}

/* ── Delay: reproduces the input shifted by delay samples ──────────── */

static void test_delay_reproduces_shifted_impulse(void)
{
    /* 10 ms delay @ 48 kHz = 480 samples. */
    const float delay_ms = 10.0f;
    const int   delay_samples = (int)(delay_ms / 1000.0f * (float)SR + 0.5f);

    JceAudioDspChain *c = jce_audio_dsp_chain_create(1, SR);
    JceAudioEffectDesc dl = jce_audio_effect_default(JCE_AUDIO_EFFECT_DELAY);
    dl.u.delay.delay_ms = delay_ms;
    dl.u.delay.feedback = 0.0f;            /* single echo */
    dl.u.delay.wet      = 1.0f;
    dl.u.delay.dry      = 1.0f;
    jce_audio_dsp_chain_add(c, &dl);

    enum { N = 2048 };
    static float buf[N];
    memset(buf, 0, sizeof(buf));
    buf[0] = 1.0f;                          /* unit impulse */
    jce_audio_dsp_chain_process(c, buf, N);

    /* Dry impulse at 0; wet copy exactly delay_samples later. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, buf[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, buf[delay_samples]);
    /* Nothing in between. */
    for (int i = 1; i < delay_samples; ++i)
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, buf[i]);
    jce_audio_dsp_chain_destroy(c);
}

static void test_delay_feedback_decays(void)
{
    const float delay_ms = 5.0f;
    const int   d = (int)(delay_ms / 1000.0f * (float)SR + 0.5f);

    JceAudioDspChain *c = jce_audio_dsp_chain_create(1, SR);
    JceAudioEffectDesc dl = jce_audio_effect_default(JCE_AUDIO_EFFECT_DELAY);
    dl.u.delay.delay_ms = delay_ms;
    dl.u.delay.feedback = 0.5f;            /* each echo half the last */
    dl.u.delay.wet      = 1.0f;
    dl.u.delay.dry      = 0.0f;            /* hear only the echoes */
    jce_audio_dsp_chain_add(c, &dl);

    enum { N = 4096 };
    static float buf[N];
    memset(buf, 0, sizeof(buf));
    buf[0] = 1.0f;
    jce_audio_dsp_chain_process(c, buf, N);

    /* Echo at d ≈ 1.0, at 2d ≈ 0.5, at 3d ≈ 0.25 (geometric by feedback). */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f,  buf[d]);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.5f,  buf[2 * d]);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.25f, buf[3 * d]);
    jce_audio_dsp_chain_destroy(c);
}

/* ── Stereo: per-channel state stays independent ───────────────────── */

static void test_stereo_delay_independent_channels(void)
{
    const float delay_ms = 4.0f;
    const int   d = (int)(delay_ms / 1000.0f * (float)SR + 0.5f);

    JceAudioDspChain *c = jce_audio_dsp_chain_create(2, SR);
    JceAudioEffectDesc dl = jce_audio_effect_default(JCE_AUDIO_EFFECT_DELAY);
    dl.u.delay.delay_ms = delay_ms;
    dl.u.delay.feedback = 0.0f;
    dl.u.delay.wet      = 1.0f;
    dl.u.delay.dry      = 1.0f;
    jce_audio_dsp_chain_add(c, &dl);

    enum { FRAMES = 1024 };
    static float buf[FRAMES * 2];
    memset(buf, 0, sizeof(buf));
    buf[0] = 1.0f;   /* left impulse only (frame 0, channel 0) */
    jce_audio_dsp_chain_process(c, buf, FRAMES);

    /* Left echoes at frame d (interleaved index 2*d); right stays silent. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, buf[2 * d]);       /* L delayed */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, buf[2 * d + 1]);   /* R silent  */
    jce_audio_dsp_chain_destroy(c);
}

/* ── Ordering: EQ then limiter — order is honoured ─────────────────── */

static void test_chain_order_eq_then_limiter(void)
{
    /* Boost a band hard with EQ, then clamp with the limiter: the output
     * peak must respect the limiter ceiling regardless of the EQ boost. */
    JceAudioDspChain *c = jce_audio_dsp_chain_create(1, SR);
    JceAudioEffectDesc eq = jce_audio_effect_default(JCE_AUDIO_EFFECT_EQ);
    eq.u.eq.shape        = JCE_AUDIO_EQ_PEAKING;
    eq.u.eq.frequency_hz = 1000.0f;
    eq.u.eq.gain_db      = 18.0f;
    eq.u.eq.q            = 1.0f;
    JceAudioEffectDesc lim = jce_audio_effect_default(JCE_AUDIO_EFFECT_LIMITER);
    lim.u.limiter.ceiling_db = -3.0f;
    lim.u.limiter.release_ms = 10.0f;
    jce_audio_dsp_chain_add(c, &eq);
    jce_audio_dsp_chain_add(c, &lim);

    enum { N = 8192 };
    static float buf[N];
    fill_sine(buf, N, 1000.0, 0.5f);
    jce_audio_dsp_chain_process(c, buf, N);

    float ceil_lin = (float)pow(10.0, -3.0 / 20.0);
    float tail_pk = buf_peak(buf + N / 2, N / 2);
    TEST_ASSERT_TRUE(tail_pk <= ceil_lin + 1e-2f);
    (void)buf_rms;
    jce_audio_dsp_chain_destroy(c);
}

/* ── null safety ───────────────────────────────────────────────────── */

static void test_null_safety(void)
{
    jce_audio_dsp_chain_destroy(NULL);
    jce_audio_dsp_chain_reset(NULL);
    jce_audio_dsp_chain_clear(NULL);
    jce_audio_dsp_chain_process(NULL, NULL, 0);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_audio_dsp_chain_count(NULL));
    TEST_ASSERT_EQUAL_INT(-1, jce_audio_dsp_chain_add(NULL, NULL));
    TEST_ASSERT_FALSE(jce_audio_dsp_chain_remove(NULL, 0));
    TEST_ASSERT_FALSE(jce_audio_dsp_chain_set(NULL, 0, NULL));
    TEST_ASSERT_EQUAL_INT(JCE_AUDIO_EFFECT_NONE, jce_audio_dsp_chain_type(NULL, 0));
}

/* ── runner ────────────────────────────────────────────────────────── */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_chain_create_destroy);
    RUN_TEST(test_empty_chain_is_passthrough);
    RUN_TEST(test_add_remove_reorder);
    RUN_TEST(test_chain_full_rejects_extra);
    RUN_TEST(test_eq_peaking_boosts_target_band);
    RUN_TEST(test_eq_peaking_leaves_far_band_alone);
    RUN_TEST(test_eq_lowpass_kills_highs_keeps_lows);
    RUN_TEST(test_compressor_reduces_gain_above_threshold);
    RUN_TEST(test_compressor_leaves_quiet_signal_alone);
    RUN_TEST(test_compressor_makeup_restores_level);
    RUN_TEST(test_limiter_clamps_to_ceiling);
    RUN_TEST(test_limiter_passes_signal_below_ceiling);
    RUN_TEST(test_delay_reproduces_shifted_impulse);
    RUN_TEST(test_delay_feedback_decays);
    RUN_TEST(test_stereo_delay_independent_channels);
    RUN_TEST(test_chain_order_eq_then_limiter);
    RUN_TEST(test_null_safety);
    return UNITY_END();
}
