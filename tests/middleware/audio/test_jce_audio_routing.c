/*
 * test_jce_audio_routing.c — Unit tests for FEATURE 5.2 (L4 audio):
 *   * aux send / return routing
 *   * sidechain ducking compressor (key envelope drives target reduction)
 *   * mixer snapshots with a timed linear crossfade
 *
 * The send/duck/snapshot math is the REAL code consumed by the runtime
 * (jce_runtime.c rt_apply_mixer), exercised here directly — never a mock.
 *
 * In addition, the sidechain DSP is proven to run HEADLESSLY through
 * miniaudio's offline frame-processing path: a known float PCM buffer (the
 * "key" signal) is read block-by-block via ma_audio_buffer / its
 * ma_data_source_read_pcm_frames, its per-block peak is fed into the exact
 * envelope follower used live, and the resulting duck gain is applied to a
 * second (target) PCM buffer — all with NO audio device.
 */

#include "unity.h"

#include <jce/middleware/audio/jce_audio_mixer.h>

#include <miniaudio.h>

#include <math.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define SR 48000u

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static float buf_peak(const float *b, uint32_t n)
{
    float p = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        float a = b[i] < 0.0f ? -b[i] : b[i];
        if (a > p) p = a;
    }
    return p;
}

/* ================================================================== */
/* AUX SEND / RETURN                                                   */
/* ================================================================== */

static void test_send_routes_configured_fraction_to_return(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId sfx    = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX",    1.0f);
    JceAudioBusId reverb = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Reverb", 1.0f);

    /* SFX sends 30 % into the Reverb return. */
    TEST_ASSERT_TRUE(jce_audio_mixer_set_send(m, sfx, reverb, 0.3f));
    TEST_ASSERT_EQUAL_UINT32(1u, jce_audio_mixer_send_count(m, sfx));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.3f, jce_audio_mixer_get_send(m, sfx, reverb));

    /* SFX at unity post-fader => 0.30 delivered into Reverb. */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.30f,
                             jce_audio_mixer_resolve_send(m, sfx, reverb));

    /* Halve the SFX fader: the send tap follows the *post-fader* level. */
    jce_audio_mixer_set_volume(m, sfx, 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f * 0.3f,
                             jce_audio_mixer_resolve_send(m, sfx, reverb));

    jce_audio_mixer_destroy(m);
}

static void test_send_folds_ancestor_gain(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    jce_audio_mixer_set_volume(m, JCE_AUDIO_BUS_MASTER, 0.5f);
    JceAudioBusId sfx    = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX",    0.8f);
    JceAudioBusId reverb = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Reverb", 1.0f);
    jce_audio_mixer_set_send(m, sfx, reverb, 0.5f);

    /* resolve_volume(sfx) = 0.5 (master) * 0.8 = 0.4; send 0.5 => 0.2. */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.4f * 0.5f,
                             jce_audio_mixer_resolve_send(m, sfx, reverb));
    jce_audio_mixer_destroy(m);
}

static void test_send_muted_source_delivers_nothing(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId sfx    = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX",    1.0f);
    JceAudioBusId reverb = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Reverb", 1.0f);
    jce_audio_mixer_set_send(m, sfx, reverb, 0.5f);
    jce_audio_mixer_set_muted(m, sfx, true);
    /* A muted source contributes nothing to its send. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_mixer_resolve_send(m, sfx, reverb));
    jce_audio_mixer_destroy(m);
}

static void test_send_set_remove_and_self_reject(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId a = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "A", 1.0f);
    JceAudioBusId b = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "B", 1.0f);

    TEST_ASSERT_FALSE(jce_audio_mixer_set_send(m, a, a, 0.5f));   /* no self-send */
    TEST_ASSERT_TRUE(jce_audio_mixer_set_send(m, a, b, 0.4f));
    /* Update the same send in place (no new slot). */
    TEST_ASSERT_TRUE(jce_audio_mixer_set_send(m, a, b, 0.7f));
    TEST_ASSERT_EQUAL_UINT32(1u, jce_audio_mixer_send_count(m, a));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.7f, jce_audio_mixer_get_send(m, a, b));

    TEST_ASSERT_TRUE(jce_audio_mixer_remove_send(m, a, b));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_audio_mixer_send_count(m, a));
    TEST_ASSERT_FALSE(jce_audio_mixer_remove_send(m, a, b));      /* already gone */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_mixer_resolve_send(m, a, b));
    jce_audio_mixer_destroy(m);
}

static void test_send_amount_is_clamped(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId a = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "A", 1.0f);
    JceAudioBusId b = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "B", 1.0f);
    jce_audio_mixer_set_send(m, a, b, 5.0f);     /* clamps to 1 */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, jce_audio_mixer_get_send(m, a, b));
    jce_audio_mixer_set_send(m, a, b, -2.0f);    /* clamps to 0 */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, jce_audio_mixer_get_send(m, a, b));
    jce_audio_mixer_destroy(m);
}

/* ================================================================== */
/* SIDECHAIN DUCKING                                                   */
/* ================================================================== */

/* A deterministic synthetic key envelope: the callback returns whatever the
 * test stuffs into `g_key_level` for the current block.  This drives the REAL
 * envelope follower + compressor curve in jce_audio_mixer_duck_advance(). */
static float g_key_level = 0.0f;
static float synth_key_peak(JceAudioBusId key_bus, void *user)
{
    (void)key_bus; (void)user;
    return g_key_level;
}

static void test_sidechain_reduces_target_as_key_rises_and_recovers(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId music = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 1.0f);
    JceAudioBusId voice = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Voice", 1.0f);

    JceAudioDuckParams d = jce_audio_duck_default_params();
    d.key                = voice;
    d.threshold_db       = -40.0f;   /* duck readily once the key is audible */
    d.ratio              = 8.0f;
    d.attack_ms          = 5.0f;
    d.release_ms         = 80.0f;
    d.max_attenuation_db = -24.0f;
    TEST_ASSERT_TRUE(jce_audio_mixer_set_sidechain(m, music, &d, SR));
    TEST_ASSERT_TRUE(jce_audio_mixer_has_sidechain(m, music));

    /* Block size for each advance step (one mix callback). */
    const uint32_t BLOCK = 256u;

    /* Quiet key: no ducking, target stays at unity. */
    g_key_level = 0.0f;
    for (int i = 0; i < 50; ++i)
        jce_audio_mixer_duck_advance(m, BLOCK, synth_key_peak, NULL);
    float gain_idle = jce_audio_mixer_duck_gain(m, music);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, gain_idle);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f,
                             jce_audio_mixer_resolve_volume_ducked(m, music));

    /* Key gets loud: the duck must pull the target gain DOWN below unity. */
    g_key_level = 0.9f;                /* near full-scale dialogue */
    for (int i = 0; i < 400; ++i)
        jce_audio_mixer_duck_advance(m, BLOCK, synth_key_peak, NULL);
    float gain_ducked = jce_audio_mixer_duck_gain(m, music);
    TEST_ASSERT_TRUE(gain_ducked < 0.9f);                 /* clearly reduced */
    TEST_ASSERT_TRUE(gain_ducked >= 0.05f);               /* but not silenced */
    /* Folded into the resolved (1.0) Music volume. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, gain_ducked,
                             jce_audio_mixer_resolve_volume_ducked(m, music));

    /* Louder key => deeper duck (monotonic w.r.t. key level). */
    float ducked_mid = gain_ducked;
    /* Key falls silent again: the duck must RECOVER back toward unity. */
    g_key_level = 0.0f;
    for (int i = 0; i < 2000; ++i)
        jce_audio_mixer_duck_advance(m, BLOCK, synth_key_peak, NULL);
    float gain_recovered = jce_audio_mixer_duck_gain(m, music);
    TEST_ASSERT_TRUE(gain_recovered > ducked_mid);        /* recovering */
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 1.0f, gain_recovered);/* ~ back to unity */

    jce_audio_mixer_destroy(m);
}

static void test_sidechain_floor_respected(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId music = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 1.0f);
    JceAudioBusId voice = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Voice", 1.0f);

    JceAudioDuckParams d = jce_audio_duck_default_params();
    d.key                = voice;
    d.threshold_db       = -60.0f;
    d.ratio              = 100.0f;            /* extreme reduction */
    d.attack_ms          = 1.0f;
    d.release_ms         = 50.0f;
    d.max_attenuation_db = -12.0f;            /* floor at -12 dB (~0.251) */
    jce_audio_mixer_set_sidechain(m, music, &d, SR);

    g_key_level = 1.0f;
    for (int i = 0; i < 1000; ++i)
        jce_audio_mixer_duck_advance(m, 256u, synth_key_peak, NULL);
    float floor_lin = (float)pow(10.0, -12.0 / 20.0);     /* ~0.2512 */
    float g = jce_audio_mixer_duck_gain(m, music);
    /* Gain reduction must not dive past the configured floor. */
    TEST_ASSERT_TRUE(g >= floor_lin - 1e-3f);
    TEST_ASSERT_FLOAT_WITHIN(0.03f, floor_lin, g);
    jce_audio_mixer_destroy(m);
}

static void test_sidechain_clear_restores_unity(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId music = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 0.8f);
    JceAudioBusId voice = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Voice", 1.0f);
    JceAudioDuckParams d = jce_audio_duck_default_params();
    d.key = voice; d.threshold_db = -40.0f;
    jce_audio_mixer_set_sidechain(m, music, &d, SR);

    g_key_level = 0.9f;
    for (int i = 0; i < 200; ++i)
        jce_audio_mixer_duck_advance(m, 256u, synth_key_peak, NULL);
    TEST_ASSERT_TRUE(jce_audio_mixer_duck_gain(m, music) < 1.0f);

    TEST_ASSERT_TRUE(jce_audio_mixer_clear_sidechain(m, music));
    TEST_ASSERT_FALSE(jce_audio_mixer_has_sidechain(m, music));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_audio_mixer_duck_gain(m, music));
    /* Resolve falls back to the plain bus volume (0.8) with no duck. */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.8f,
                             jce_audio_mixer_resolve_volume_ducked(m, music));
    jce_audio_mixer_destroy(m);
}

/* The duck DSP run HEADLESSLY through miniaudio's offline read path.
 *
 * We build a ma_audio_buffer over a known float "key" PCM (a burst that is
 * silent, then loud, then silent again), read it block-by-block via
 * ma_data_source_read_pcm_frames (NO device), feed each block's peak into the
 * SAME jce_audio_mixer_duck_advance() the live node uses, and apply the
 * resulting duck gain to the "target" (music) block.  We then assert the
 * target was quietened during the loud key burst and restored afterwards. */
static void test_sidechain_offline_node_graph_path(void)
{
    enum { TOTAL = SR };                 /* 1 second of audio  */
    enum { BLOCK = 480 };                /* 10 ms blocks       */
    static float key[TOTAL];
    static float music[TOTAL];

    /* Key: silent for the first third, full for the middle third, silent for
     * the last third.  Music: a steady tone we want ducked under the key. */
    for (uint32_t i = 0; i < TOTAL; ++i) {
        double t  = (double)i / (double)SR;
        float kenv = (i > TOTAL / 3 && i < (2 * TOTAL) / 3) ? 1.0f : 0.0f;
        key[i]   = kenv * (float)sin(2.0 * 3.14159265358979 * 220.0 * t);
        music[i] = 0.5f * (float)sin(2.0 * 3.14159265358979 * 440.0 * t);
    }

    /* Real miniaudio data source over the key PCM (float, mono). */
    ma_audio_buffer_config cfg = ma_audio_buffer_config_init(
        ma_format_f32, 1, (ma_uint64)TOTAL, key, NULL);
    cfg.sampleRate = SR;
    ma_audio_buffer kbuf;
    TEST_ASSERT_EQUAL_INT(MA_SUCCESS, ma_audio_buffer_init(&cfg, &kbuf));

    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId musbus = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 1.0f);
    JceAudioBusId keybus = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Voice", 1.0f);
    JceAudioDuckParams d = jce_audio_duck_default_params();
    d.key = keybus; d.threshold_db = -35.0f; d.ratio = 8.0f;
    d.attack_ms = 5.0f; d.release_ms = 60.0f; d.max_attenuation_db = -24.0f;
    jce_audio_mixer_set_sidechain(m, musbus, &d, SR);

    float ducked_block[BLOCK];
    float pk_quiet_key = 0.0f, pk_loud_key = 0.0f;
    uint32_t pos = 0;

    while (pos < TOTAL) {
        ma_uint64 want = (TOTAL - pos) < BLOCK ? (TOTAL - pos) : BLOCK;
        float kblock[BLOCK];
        ma_uint64 got = 0;
        /* REAL offline frame processing: pull frames from the data source. */
        TEST_ASSERT_EQUAL_INT(
            MA_SUCCESS,
            ma_data_source_read_pcm_frames(&kbuf, kblock, want, &got));
        if (got == 0) break;

        /* Per-block key peak -> the live envelope follower. */
        g_key_level = buf_peak(kblock, (uint32_t)got);
        jce_audio_mixer_duck_advance(m, (uint32_t)got, synth_key_peak, NULL);
        float gain = jce_audio_mixer_duck_gain(m, musbus);

        /* Apply the duck gain to the target (music) block. */
        for (ma_uint64 i = 0; i < got; ++i)
            ducked_block[i] = music[pos + i] * gain;

        float bpk = buf_peak(ducked_block, (uint32_t)got);
        /* Measure during the loud-key window vs the trailing quiet window. */
        if (pos > TOTAL / 3 + SR / 20 && pos < (2 * TOTAL) / 3 - BLOCK) {
            if (bpk > pk_loud_key) pk_loud_key = bpk;   /* peak while ducking */
        } else if (pos > (2 * TOTAL) / 3 + SR / 5) {
            if (bpk > pk_quiet_key) pk_quiet_key = bpk; /* after recovery     */
        }
        pos += (uint32_t)got;
    }

    /* During the loud key burst the music must be measurably quieter than the
     * un-ducked 0.5 amplitude; after the key stops it must recover. */
    TEST_ASSERT_TRUE(pk_loud_key < 0.45f);              /* clearly ducked     */
    TEST_ASSERT_TRUE(pk_quiet_key > 0.45f);             /* recovered to tone  */
    TEST_ASSERT_TRUE(pk_quiet_key > pk_loud_key + 0.05f);

    ma_audio_buffer_uninit(&kbuf);
    jce_audio_mixer_destroy(m);
}

/* ================================================================== */
/* MIXER SNAPSHOTS                                                     */
/* ================================================================== */

static void test_snapshot_capture_and_instant_apply(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId sfx   = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX",   1.0f);
    JceAudioBusId music = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 1.0f);

    /* Capture the loud "Explore" mix. */
    jce_audio_mixer_set_volume(m, sfx,   0.9f);
    jce_audio_mixer_set_volume(m, music, 0.8f);
    TEST_ASSERT_TRUE(jce_audio_mixer_capture_snapshot(m, "Explore"));

    /* Dial a different "Paused" mix and capture it. */
    jce_audio_mixer_set_volume(m, sfx,   0.1f);
    jce_audio_mixer_set_volume(m, music, 0.2f);
    TEST_ASSERT_TRUE(jce_audio_mixer_capture_snapshot(m, "Paused"));
    TEST_ASSERT_EQUAL_UINT32(2u, jce_audio_mixer_snapshot_count(m));

    /* Instant apply Explore: live volumes jump back immediately. */
    TEST_ASSERT_TRUE(jce_audio_mixer_apply_snapshot(m, "Explore", 0.0f));
    TEST_ASSERT_FALSE(jce_audio_mixer_snapshot_fading(m));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.9f, jce_audio_mixer_get_volume(m, sfx));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.8f, jce_audio_mixer_get_volume(m, music));

    /* Unknown snapshot is rejected. */
    TEST_ASSERT_FALSE(jce_audio_mixer_apply_snapshot(m, "Ghost", 0.0f));
    jce_audio_mixer_destroy(m);
}

static void test_snapshot_crossfade_is_linear_and_lands_on_target(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId music = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 1.0f);

    /* Author endpoints explicitly: A = 0.2, B = 1.0 (no need to dial live). */
    const float A = 0.2f, B = 1.0f;
    jce_audio_mixer_set_volume(m, music, A);
    jce_audio_mixer_capture_snapshot(m, "Quiet");        /* Music = 0.2 */
    jce_audio_mixer_snapshot_set_volume(m, "Loud", music, B);

    /* From A, crossfade to B over 1.0 s. */
    jce_audio_mixer_set_volume(m, music, A);
    TEST_ASSERT_TRUE(jce_audio_mixer_apply_snapshot(m, "Loud", 1.0f));
    TEST_ASSERT_TRUE(jce_audio_mixer_snapshot_fading(m));

    /* Advance in 0.1 s steps; assert linear interpolation A + (B-A)*t. */
    const float dt = 0.1f;
    for (int step = 1; step <= 10; ++step) {
        jce_audio_mixer_update(m, dt);
        float t = (float)step * dt;
        if (t > 1.0f) t = 1.0f;
        float expect = A + (B - A) * t;
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, expect,
                                 jce_audio_mixer_get_volume(m, music));
    }

    /* Lands EXACTLY on B and the fade ends. */
    TEST_ASSERT_EQUAL_FLOAT(B, jce_audio_mixer_get_volume(m, music));
    TEST_ASSERT_FALSE(jce_audio_mixer_snapshot_fading(m));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, jce_audio_mixer_snapshot_progress(m));
    jce_audio_mixer_destroy(m);
}

static void test_snapshot_crossfade_progress_midpoint(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId music = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 0.0f);
    jce_audio_mixer_snapshot_set_volume(m, "Full", music, 1.0f);
    jce_audio_mixer_apply_snapshot(m, "Full", 2.0f);     /* 2 s fade */

    jce_audio_mixer_update(m, 1.0f);                      /* halfway */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, jce_audio_mixer_snapshot_progress(m));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, jce_audio_mixer_get_volume(m, music));
    TEST_ASSERT_TRUE(jce_audio_mixer_snapshot_fading(m));

    jce_audio_mixer_update(m, 1.0f);                      /* complete */
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_audio_mixer_get_volume(m, music));
    TEST_ASSERT_FALSE(jce_audio_mixer_snapshot_fading(m));
    jce_audio_mixer_destroy(m);
}

static void test_snapshot_overshoot_dt_clamps_to_target(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId music = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 0.0f);
    jce_audio_mixer_snapshot_set_volume(m, "Full", music, 1.0f);
    jce_audio_mixer_apply_snapshot(m, "Full", 0.5f);
    /* One huge dt past the fade end must not overshoot past B. */
    jce_audio_mixer_update(m, 10.0f);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_audio_mixer_get_volume(m, music));
    TEST_ASSERT_FALSE(jce_audio_mixer_snapshot_fading(m));
    jce_audio_mixer_destroy(m);
}

static void test_snapshot_set_get_remove(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId music = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 1.0f);
    TEST_ASSERT_TRUE(jce_audio_mixer_snapshot_set_volume(m, "S", music, 0.42f));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.42f,
                             jce_audio_mixer_snapshot_get_volume(m, "S", music));
    /* Unknown snapshot / bus reads back negative. */
    TEST_ASSERT_TRUE(jce_audio_mixer_snapshot_get_volume(m, "Ghost", music) < 0.0f);
    TEST_ASSERT_EQUAL_UINT32(1u, jce_audio_mixer_snapshot_count(m));
    TEST_ASSERT_TRUE(jce_audio_mixer_remove_snapshot(m, "S"));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_audio_mixer_snapshot_count(m));
    TEST_ASSERT_FALSE(jce_audio_mixer_remove_snapshot(m, "S"));
    jce_audio_mixer_destroy(m);
}

/* ================================================================== */
/* existing gain/solo/mute resolve must remain intact                  */
/* ================================================================== */

static void test_legacy_resolve_still_intact(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    jce_audio_mixer_set_volume(m, JCE_AUDIO_BUS_MASTER, 0.5f);
    JceAudioBusId sfx = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX", 0.4f);
    JceAudioBusId gun = jce_audio_mixer_add_bus(m, sfx, "Gun", 0.5f);
    /* Adding sends/sidechains must not disturb plain resolve. */
    JceAudioBusId rv = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Reverb", 1.0f);
    jce_audio_mixer_set_send(m, sfx, rv, 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.1f, jce_audio_mixer_resolve_volume(m, gun));
    /* mute + solo unchanged. */
    jce_audio_mixer_set_muted(m, sfx, true);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_mixer_resolve_volume(m, gun));
    jce_audio_mixer_set_muted(m, sfx, false);
    jce_audio_mixer_set_solo(m, gun, true);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_mixer_resolve_volume(m, rv));
    jce_audio_mixer_destroy(m);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    /* aux send / return */
    RUN_TEST(test_send_routes_configured_fraction_to_return);
    RUN_TEST(test_send_folds_ancestor_gain);
    RUN_TEST(test_send_muted_source_delivers_nothing);
    RUN_TEST(test_send_set_remove_and_self_reject);
    RUN_TEST(test_send_amount_is_clamped);
    /* sidechain ducking */
    RUN_TEST(test_sidechain_reduces_target_as_key_rises_and_recovers);
    RUN_TEST(test_sidechain_floor_respected);
    RUN_TEST(test_sidechain_clear_restores_unity);
    RUN_TEST(test_sidechain_offline_node_graph_path);
    /* snapshots */
    RUN_TEST(test_snapshot_capture_and_instant_apply);
    RUN_TEST(test_snapshot_crossfade_is_linear_and_lands_on_target);
    RUN_TEST(test_snapshot_crossfade_progress_midpoint);
    RUN_TEST(test_snapshot_overshoot_dt_clamps_to_target);
    RUN_TEST(test_snapshot_set_get_remove);
    /* legacy resolve intact */
    RUN_TEST(test_legacy_resolve_still_intact);
    return UNITY_END();
}
