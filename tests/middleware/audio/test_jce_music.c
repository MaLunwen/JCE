/*
 * test_jce_music.c -- Unit tests for FEATURE 5.3 (L4 audio):
 *   * vertical layering: intensity fades the right stems in and others out
 *     over each layer's fade time (deterministic linear gain ramp),
 *   * horizontal transition: a switch is QUANTIZED to the next beat/bar
 *     boundary given a tempo (the computed switch time is exact), not immediate,
 *   * stinger: a one-shot overlay that does not disturb the layer gains.
 *
 * This drives the REAL director math (jce_music_update / set_intensity /
 * request_transition / next_boundary) consumed by the runtime -- never a mock.
 *
 * In addition the resolved layer gains are proven to run HEADLESSLY through
 * miniaudio's offline read path: known float PCM stems are read block-by-block
 * via ma_data_source_read_pcm_frames (NO audio device), each scaled by the
 * director's live per-layer gain, summed, and the mixed buffer's energy is
 * asserted to follow the intensity-driven fade -- exactly as the live node mix
 * would behave.
 */

#include "unity.h"

#include <jce/middleware/audio/jce_music.h>
#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/os/core/jce_str.h>

#include <miniaudio.h>

#include <math.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define SR 48000u

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static float buf_rms(const float *b, uint32_t n)
{
    double s = 0.0;
    for (uint32_t i = 0; i < n; ++i) s += (double)b[i] * (double)b[i];
    return n ? (float)sqrt(s / (double)n) : 0.0f;
}

/* A 3-stem track: base (always on), mid (enters at 0.3), high (enters at 0.7),
 * each with a 1.0 s fade.  Returns by value. */
static JceMusicTrackDesc make_three_stem_track(void)
{
    JceMusicTrackDesc t;
    jce_music_track_desc_default(&t);
    t.tempo_bpm        = 120.0f;     /* 0.5 s per beat */
    t.beats_per_bar    = 4u;         /* 2.0 s per bar  */
    t.bars_per_segment = 4u;
    t.layer_count      = 3u;

    /* base: always on (enter=full=0). */
    jce_strlcpy(t.layers[0].name, "base", JCE_MUSIC_NAME);
    t.layers[0].intensity_enter = 0.0f;
    t.layers[0].intensity_full  = 0.0f;
    t.layers[0].fade_time       = 1.0f;
    /* mid: cross-fades in over [0.3, 0.5]. */
    jce_strlcpy(t.layers[1].name, "mid", JCE_MUSIC_NAME);
    t.layers[1].intensity_enter = 0.3f;
    t.layers[1].intensity_full  = 0.5f;
    t.layers[1].fade_time       = 1.0f;
    /* high: cross-fades in over [0.7, 1.0]. */
    jce_strlcpy(t.layers[2].name, "high", JCE_MUSIC_NAME);
    t.layers[2].intensity_enter = 0.7f;
    t.layers[2].intensity_full  = 1.0f;
    t.layers[2].fade_time       = 1.0f;
    return t;
}

/* ================================================================== */
/* VERTICAL LAYERING (intensity -> targets -> linear fade)             */
/* ================================================================== */

static void test_intensity_sets_layer_targets(void)
{
    JceMusicTrackDesc t = make_three_stem_track();
    JceMusicDirector *d = jce_music_create(&t, NULL);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT32(3u, jce_music_layer_count(d));

    int base = jce_music_find_layer(d, "base");
    int mid  = jce_music_find_layer(d, "mid");
    int high = jce_music_find_layer(d, "high");
    TEST_ASSERT_TRUE(base >= 0 && mid >= 0 && high >= 0);

    /* intensity 0: base full on, mid/high silent (targets). */
    jce_music_set_intensity(d, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, jce_music_layer_target(d, base));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, jce_music_layer_target(d, mid));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, jce_music_layer_target(d, high));

    /* intensity 0.5: base full, mid full (>= its full=0.5), high still silent. */
    jce_music_set_intensity(d, 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, jce_music_layer_target(d, base));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, jce_music_layer_target(d, mid));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, jce_music_layer_target(d, high));

    /* intensity 0.4: mid is halfway up its [0.3,0.5] band. */
    jce_music_set_intensity(d, 0.4f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, jce_music_layer_target(d, mid));

    /* intensity 1.0: everything on. */
    jce_music_set_intensity(d, 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, jce_music_layer_target(d, high));

    jce_music_destroy(d);
}

static void test_raising_intensity_fades_layers_in_over_fade_time(void)
{
    JceMusicTrackDesc t = make_three_stem_track();
    JceMusicDirector *d = jce_music_create(&t, NULL);
    int mid  = jce_music_find_layer(d, "mid");
    int high = jce_music_find_layer(d, "high");

    /* Start silent for mid/high. */
    jce_music_set_intensity(d, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, jce_music_layer_gain(d, mid));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, jce_music_layer_gain(d, high));

    /* Crank intensity to full: mid AND high targets go to 1, but the gains
     * must RAMP over the 1.0 s fade, not jump. */
    jce_music_set_intensity(d, 1.0f);
    TEST_ASSERT_TRUE(jce_music_is_fading(d));

    /* 0.25 s in: linear ramp from 0 -> 1 over 1 s => 0.25. */
    jce_music_update(d, 0.25f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.25f, jce_music_layer_gain(d, mid));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.25f, jce_music_layer_gain(d, high));

    /* +0.5 s (0.75 total) => 0.75. */
    jce_music_update(d, 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.75f, jce_music_layer_gain(d, mid));

    /* +0.5 s (overshoot end) clamps exactly at the target, fade ends. */
    jce_music_update(d, 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, jce_music_layer_gain(d, mid));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, jce_music_layer_gain(d, high));
    TEST_ASSERT_FALSE(jce_music_is_fading(d));

    jce_music_destroy(d);
}

/* The DISCRIMINATING test: raising intensity fades the RIGHT layers IN while
 * the others (a layer that should leave) fade OUT, deterministically. */
static void test_intensity_fades_some_in_and_others_out(void)
{
    JceMusicTrackDesc t = make_three_stem_track();
    JceMusicDirector *d = jce_music_create(&t, NULL);
    int mid  = jce_music_find_layer(d, "mid");
    int high = jce_music_find_layer(d, "high");

    /* Reach full intensity (mid+high fully on) instantly by letting the fades
     * complete. */
    jce_music_set_intensity(d, 1.0f);
    jce_music_update(d, 2.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, jce_music_layer_gain(d, mid));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, jce_music_layer_gain(d, high));

    /* Now DROP intensity to 0.4: mid stays up (target 0.5) but HIGH must fade
     * OUT (target 0).  At 0.4: mid target = 0.5, high target = 0. */
    jce_music_set_intensity(d, 0.4f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, jce_music_layer_target(d, mid));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, jce_music_layer_target(d, high));

    /* Constant-rate slew at 1/fade_time = 1.0 gain-units/s.  After 0.25 s both
     * have moved 0.25 down: high 1.0 -> 0.75 (heading to 0), mid 1.0 -> 0.75
     * (heading to 0.5). */
    jce_music_update(d, 0.25f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.75f, jce_music_layer_gain(d, high)); /* fading OUT */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.75f, jce_music_layer_gain(d, mid));  /* fading down */

    /* After another 0.5 s (0.75 total) mid has hit its 0.5 floor and STOPPED,
     * while high keeps falling toward 0 (now 0.25): the right layer settled,
     * the other is still fading out. */
    jce_music_update(d, 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.25f, jce_music_layer_gain(d, high)); /* still OUT  */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.50f, jce_music_layer_gain(d, mid));  /* settled    */

    /* Finish: high reaches 0, mid stays at its 0.5 target. */
    jce_music_update(d, 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, jce_music_layer_gain(d, high));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, jce_music_layer_gain(d, mid));

    jce_music_destroy(d);
}

/* When a mixer is attached, the resolved layer gains are pushed onto the bound
 * buses, so the live mix follows the fade through the existing bus path. */
static void test_layers_drive_mixer_buses(void)
{
    JceAudioMixer *mx = jce_audio_mixer_create();
    JceAudioBusId music = jce_audio_mixer_add_bus(mx, JCE_AUDIO_BUS_MASTER, "Music", 1.0f);
    JceAudioBusId b_base = jce_audio_mixer_add_bus(mx, music, "Mus.Base", 0.0f);
    JceAudioBusId b_mid  = jce_audio_mixer_add_bus(mx, music, "Mus.Mid",  0.0f);

    JceMusicTrackDesc t;
    jce_music_track_desc_default(&t);
    t.layer_count = 2u;
    jce_strlcpy(t.layers[0].name, "base", JCE_MUSIC_NAME);
    jce_strlcpy(t.layers[0].bus_name, "Mus.Base", JCE_MUSIC_NAME);
    t.layers[0].intensity_enter = 0.0f; t.layers[0].intensity_full = 0.0f;
    t.layers[0].fade_time = 1.0f;
    jce_strlcpy(t.layers[1].name, "mid", JCE_MUSIC_NAME);
    jce_strlcpy(t.layers[1].bus_name, "Mus.Mid", JCE_MUSIC_NAME);
    t.layers[1].intensity_enter = 0.3f; t.layers[1].intensity_full = 0.5f;
    t.layers[1].fade_time = 1.0f;

    JceMusicDirector *d = jce_music_create(&t, mx);
    TEST_ASSERT_NOT_NULL(d);
    /* The director resolved the bus ids at build time. */
    TEST_ASSERT_EQUAL_UINT16(b_base, jce_music_layer_bus(d, 0));
    TEST_ASSERT_EQUAL_UINT16(b_mid,  jce_music_layer_bus(d, 1));

    /* base is always-on; its bus was set to 1.0 at create time. */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, jce_audio_mixer_get_volume(mx, b_base));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, jce_audio_mixer_get_volume(mx, b_mid));

    /* Raise intensity; after a half-fade the Mid bus volume tracks the layer. */
    jce_music_set_intensity(d, 1.0f);
    jce_music_update(d, 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, jce_audio_mixer_get_volume(mx, b_mid));
    /* And the resolved (Master*Music*Mid) gain follows it. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, jce_audio_mixer_resolve_volume(mx, b_mid));

    jce_music_update(d, 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, jce_audio_mixer_get_volume(mx, b_mid));

    jce_music_destroy(d);
    jce_audio_mixer_destroy(mx);
}

/* ================================================================== */
/* HORIZONTAL TRANSITION (beat / bar quantize)                         */
/* ================================================================== */

static void test_next_boundary_math_is_exact(void)
{
    /* 120 BPM => 0.5 s/beat, 4/4 => 2.0 s/bar. */
    const float bpm = 120.0f; const uint32_t bpb = 4u;

    /* From t=0 the next beat IS 0 (already on a boundary). */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f,
        jce_music_next_boundary(0.0f, bpm, bpb, JCE_MUSIC_QUANT_BEAT));
    /* From t=0.1 the next beat is 0.5. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.5f,
        jce_music_next_boundary(0.1f, bpm, bpb, JCE_MUSIC_QUANT_BEAT));
    /* From t=0.5 (exactly on beat 1) the next beat is itself, 0.5. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.5f,
        jce_music_next_boundary(0.5f, bpm, bpb, JCE_MUSIC_QUANT_BEAT));
    /* From t=0.6 the next beat is 1.0. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f,
        jce_music_next_boundary(0.6f, bpm, bpb, JCE_MUSIC_QUANT_BEAT));

    /* Bars: from t=0.1 the next bar is 2.0 (not the next beat 0.5). */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 2.0f,
        jce_music_next_boundary(0.1f, bpm, bpb, JCE_MUSIC_QUANT_BAR));
    /* From t=2.0 (on bar 1) the next bar is itself. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 2.0f,
        jce_music_next_boundary(2.0f, bpm, bpb, JCE_MUSIC_QUANT_BAR));
    /* From t=2.3 the next bar is 4.0. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 4.0f,
        jce_music_next_boundary(2.3f, bpm, bpb, JCE_MUSIC_QUANT_BAR));

    /* IMMEDIATE returns the time unchanged. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.37f,
        jce_music_next_boundary(0.37f, bpm, bpb, JCE_MUSIC_QUANT_IMMEDIATE));
}

static void test_transition_is_scheduled_to_next_boundary_not_immediate(void)
{
    JceMusicTrackDesc t = make_three_stem_track();   /* 120 BPM, 4/4 */
    JceMusicDirector *d = jce_music_create(&t, NULL);

    /* Advance the playhead to t=0.6 s (mid-bar, mid-beat). */
    jce_music_update(d, 0.6f);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.6f, jce_music_playhead(d));
    TEST_ASSERT_EQUAL_INT(0, jce_music_current_segment(d));

    /* Request a switch to segment 1 quantized to the next BAR.  From t=0.6 the
     * next bar boundary is 2.0 s -- NOT now. */
    float when = jce_music_request_transition(d, 1, JCE_MUSIC_QUANT_BAR);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 2.0f, when);
    TEST_ASSERT_TRUE(jce_music_transition_pending(d));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 2.0f, jce_music_transition_time(d));
    TEST_ASSERT_EQUAL_INT(1, jce_music_transition_target(d));

    /* It must NOT fire before the boundary: advance to t=1.6 (still seg 0). */
    jce_music_update(d, 1.0f);
    TEST_ASSERT_TRUE(jce_music_transition_pending(d));
    TEST_ASSERT_EQUAL_INT(0, jce_music_current_segment(d));

    /* Cross the boundary (t=2.1): the switch fires, segment advances. */
    jce_music_update(d, 0.5f);
    TEST_ASSERT_FALSE(jce_music_transition_pending(d));
    TEST_ASSERT_EQUAL_INT(1, jce_music_current_segment(d));

    jce_music_destroy(d);
}

/* The scheduled switch fires exactly ONCE and invokes the host callback with
 * the right from/to segment ids. */
static int g_cb_calls = 0;
static int g_cb_from  = -99;
static int g_cb_to    = -99;
static void JCE_CALL on_transition(JceMusicDirector *d, int from, int to, void *user)
{
    (void)d; (void)user;
    g_cb_calls++;
    g_cb_from = from;
    g_cb_to   = to;
}

static void test_transition_callback_fires_once_on_boundary(void)
{
    JceMusicTrackDesc t = make_three_stem_track();
    JceMusicDirector *d = jce_music_create(&t, NULL);
    g_cb_calls = 0; g_cb_from = -99; g_cb_to = -99;
    jce_music_set_transition_cb(d, on_transition, NULL);

    /* Advance off the t=0 boundary (to t=0.1) so the next beat is a genuine
     * FUTURE boundary at 0.5 -- proving the switch is quantized, not immediate. */
    jce_music_update(d, 0.1f);
    float when = jce_music_request_transition(d, 2, JCE_MUSIC_QUANT_BEAT);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.5f, when);

    jce_music_update(d, 0.25f);                 /* t=0.35, before boundary */
    TEST_ASSERT_EQUAL_INT(0, g_cb_calls);

    jce_music_update(d, 0.30f);                 /* t=0.65, crosses 0.5 */
    TEST_ASSERT_EQUAL_INT(1, g_cb_calls);
    TEST_ASSERT_EQUAL_INT(0, g_cb_from);
    TEST_ASSERT_EQUAL_INT(2, g_cb_to);

    /* No further fires once consumed. */
    jce_music_update(d, 1.0f);
    TEST_ASSERT_EQUAL_INT(1, g_cb_calls);
    TEST_ASSERT_EQUAL_INT(2, jce_music_current_segment(d));

    jce_music_destroy(d);
}

/* ================================================================== */
/* STINGER (one-shot overlay, must not disturb layers)                 */
/* ================================================================== */

static void test_stinger_is_one_shot_and_leaves_layers_untouched(void)
{
    JceMusicTrackDesc t = make_three_stem_track();
    JceMusicDirector *d = jce_music_create(&t, NULL);

    /* Settle a known layer state: intensity 0.4 fully faded. */
    jce_music_set_intensity(d, 0.4f);
    jce_music_update(d, 2.0f);
    float g_base = jce_music_layer_gain(d, jce_music_find_layer(d, "base"));
    float g_mid  = jce_music_layer_gain(d, jce_music_find_layer(d, "mid"));
    float g_high = jce_music_layer_gain(d, jce_music_find_layer(d, "high"));
    TEST_ASSERT_FALSE(jce_music_stinger_active(d));

    /* Fire a 0.5 s stinger. */
    jce_music_fire_stinger(d, 0.5f);
    TEST_ASSERT_TRUE(jce_music_stinger_active(d));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, jce_music_stinger_remaining(d));

    /* Tick 0.2 s: stinger counts down; layer gains must be UNCHANGED. */
    jce_music_update(d, 0.2f);
    TEST_ASSERT_TRUE(jce_music_stinger_active(d));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.3f, jce_music_stinger_remaining(d));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, g_base, jce_music_layer_gain(d, 0));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, g_mid,  jce_music_layer_gain(d, 1));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, g_high, jce_music_layer_gain(d, 2));

    /* Tick past the end: stinger expires (one-shot), layers still unchanged. */
    jce_music_update(d, 0.4f);
    TEST_ASSERT_FALSE(jce_music_stinger_active(d));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, jce_music_stinger_remaining(d));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, g_base, jce_music_layer_gain(d, 0));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, g_mid,  jce_music_layer_gain(d, 1));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, g_high, jce_music_layer_gain(d, 2));

    jce_music_destroy(d);
}

/* A stinger must not perturb a PENDING transition either. */
static void test_stinger_does_not_disturb_transition_schedule(void)
{
    JceMusicTrackDesc t = make_three_stem_track();
    JceMusicDirector *d = jce_music_create(&t, NULL);

    float when = jce_music_request_transition(d, 1, JCE_MUSIC_QUANT_BAR); /* 2.0 */
    jce_music_fire_stinger(d, 0.25f);
    /* Firing the stinger leaves the transition time and target intact. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, when, jce_music_transition_time(d));
    TEST_ASSERT_EQUAL_INT(1, jce_music_transition_target(d));
    TEST_ASSERT_TRUE(jce_music_transition_pending(d));
    jce_music_destroy(d);
}

/* ================================================================== */
/* HEADLESS DSP: resolved layer gains applied to real PCM via miniaudio */
/* ================================================================== */

/* Two stems (base + lead) read from real ma_audio_buffer data sources block by
 * block with NO device.  As intensity rises the lead must fade IN, so the mixed
 * RMS during the high-intensity window must exceed the RMS while the lead is
 * silent -- proving the director's gain math drives a real summed mix. */
static void test_layer_gains_drive_offline_mix(void)
{
    enum { TOTAL = SR };            /* 1 s */
    enum { BLOCK = 480 };           /* 10 ms */
    static float base_pcm[TOTAL];
    static float lead_pcm[TOTAL];
    for (uint32_t i = 0; i < TOTAL; ++i) {
        double tt = (double)i / (double)SR;
        base_pcm[i] = 0.25f * (float)sin(2.0 * 3.14159265358979 * 110.0 * tt);
        lead_pcm[i] = 0.50f * (float)sin(2.0 * 3.14159265358979 * 660.0 * tt);
    }

    ma_audio_buffer_config bc = ma_audio_buffer_config_init(
        ma_format_f32, 1, (ma_uint64)TOTAL, base_pcm, NULL);
    bc.sampleRate = SR;
    ma_audio_buffer bbuf;
    TEST_ASSERT_EQUAL_INT(MA_SUCCESS, ma_audio_buffer_init(&bc, &bbuf));
    ma_audio_buffer_config lc = ma_audio_buffer_config_init(
        ma_format_f32, 1, (ma_uint64)TOTAL, lead_pcm, NULL);
    lc.sampleRate = SR;
    ma_audio_buffer lbuf;
    TEST_ASSERT_EQUAL_INT(MA_SUCCESS, ma_audio_buffer_init(&lc, &lbuf));

    /* base always-on; lead enters over [0.0,0.2] with a short 0.1 s fade so it
     * settles quickly once intensity is raised. */
    JceMusicTrackDesc t;
    jce_music_track_desc_default(&t);
    t.layer_count = 2u;
    jce_strlcpy(t.layers[0].name, "base", JCE_MUSIC_NAME);
    t.layers[0].intensity_enter = 0.0f; t.layers[0].intensity_full = 0.0f;
    t.layers[0].fade_time = 0.05f;
    jce_strlcpy(t.layers[1].name, "lead", JCE_MUSIC_NAME);
    t.layers[1].intensity_enter = 0.0f; t.layers[1].intensity_full = 0.2f;
    t.layers[1].fade_time = 0.05f;
    JceMusicDirector *d = jce_music_create(&t, NULL);

    /* Phase A (first 0.5 s): intensity 0 -> lead silent.  Phase B (next 0.5 s):
     * intensity 1 -> lead full.  Measure mixed RMS in each phase. */
    double rmsA = 0.0; uint32_t nA = 0;
    double rmsB = 0.0; uint32_t nB = 0;
    float outblk[BLOCK];
    uint32_t pos = 0;
    bool switched = false;

    while (pos < TOTAL) {
        if (!switched && pos >= TOTAL / 2) {
            jce_music_set_intensity(d, 1.0f);   /* raise intensity at the half */
            switched = true;
        }
        ma_uint64 want = (TOTAL - pos) < BLOCK ? (TOTAL - pos) : BLOCK;
        float blk_base[BLOCK];
        float blk_lead[BLOCK];
        ma_uint64 gb = 0, gl = 0;
        TEST_ASSERT_EQUAL_INT(MA_SUCCESS,
            ma_data_source_read_pcm_frames(&bbuf, blk_base, want, &gb));
        TEST_ASSERT_EQUAL_INT(MA_SUCCESS,
            ma_data_source_read_pcm_frames(&lbuf, blk_lead, want, &gl));
        ma_uint64 got = gb < gl ? gb : gl;
        if (got == 0) break;

        /* Advance the director for this block's duration, then read the live
         * resolved per-layer gains and sum the stems. */
        jce_music_update(d, (float)got / (float)SR);
        float g_base = jce_music_layer_gain(d, 0);
        float g_lead = jce_music_layer_gain(d, 1);
        for (ma_uint64 i = 0; i < got; ++i)
            outblk[i] = blk_base[i] * g_base + blk_lead[i] * g_lead;

        float r = buf_rms(outblk, (uint32_t)got);
        if (pos + got <= TOTAL / 2) { rmsA += r; nA++; }
        else if (pos >= TOTAL / 2 + SR / 10) { rmsB += r; nB++; }  /* skip the fade */
        pos += (uint32_t)got;
    }

    TEST_ASSERT_TRUE(nA > 0 && nB > 0);
    float avgA = (float)(rmsA / nA);
    float avgB = (float)(rmsB / nB);
    /* With the lead faded in, the mix must be measurably louder. */
    TEST_ASSERT_TRUE(avgB > avgA + 0.02f);

    ma_audio_buffer_uninit(&bbuf);
    ma_audio_buffer_uninit(&lbuf);
    jce_music_destroy(d);
}

/* ================================================================== */
/* arg / edge handling                                                 */
/* ================================================================== */

static void test_create_rejects_bad_args(void)
{
    TEST_ASSERT_NULL(jce_music_create(NULL, NULL));
    JceMusicTrackDesc t;
    jce_music_track_desc_default(&t);
    t.tempo_bpm = 0.0f;                       /* invalid tempo */
    TEST_ASSERT_NULL(jce_music_create(&t, NULL));
    jce_music_track_desc_default(&t);
    t.layer_count = JCE_MUSIC_MAX_LAYERS + 1; /* too many */
    TEST_ASSERT_NULL(jce_music_create(&t, NULL));

    /* null-safe accessors. */
    TEST_ASSERT_EQUAL_UINT32(0u, jce_music_layer_count(NULL));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -1.0f, jce_music_layer_gain(NULL, 0));
    jce_music_destroy(NULL);
}

/* ------------------------------------------------------------------ */
/* runner                                                             */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    /* vertical layering */
    RUN_TEST(test_intensity_sets_layer_targets);
    RUN_TEST(test_raising_intensity_fades_layers_in_over_fade_time);
    RUN_TEST(test_intensity_fades_some_in_and_others_out);
    RUN_TEST(test_layers_drive_mixer_buses);
    /* horizontal transition */
    RUN_TEST(test_next_boundary_math_is_exact);
    RUN_TEST(test_transition_is_scheduled_to_next_boundary_not_immediate);
    RUN_TEST(test_transition_callback_fires_once_on_boundary);
    /* stinger */
    RUN_TEST(test_stinger_is_one_shot_and_leaves_layers_untouched);
    RUN_TEST(test_stinger_does_not_disturb_transition_schedule);
    /* headless DSP mix */
    RUN_TEST(test_layer_gains_drive_offline_mix);
    /* edge handling */
    RUN_TEST(test_create_rejects_bad_args);
    return UNITY_END();
}
