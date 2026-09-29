/* Bus metering and the sidechain-duck loop it closes.
 *
 * FEATURE 5.2's sidechain ducking had a wired TAIL and no HEAD: the runtime
 * called jce_audio_mixer_resolve_volume_ducked() every frame and pushed the
 * result onto the device, while jce_audio_mixer_duck_advance() -- the thing
 * that makes the duck gain anything other than 1.0 -- had no caller outside
 * the tests.  The public header even claimed "the exact same envelope+curve
 * math runs in the live miniaudio node", which was false and was the sentence
 * that stopped anyone looking.
 *
 * What was missing is the MEASUREMENT: a live key-bus level.  That is
 * jce_audio_bus_get_peak(), metered on the bus's own insert node.  These
 * cases prove the meter reads the real graph, that it reads it POST-FADER,
 * and that the follower driven from it actually ducks and recovers.
 *
 * NO DEVICE IS NEEDED and no case may skip: jce_audio_create_offline() plus
 * jce_audio_render_offline() run the real miniaudio node graph -- voices, bus
 * groups, insert nodes -- with the caller as the only pump.  A test that
 * self-ignores for want of sound hardware would have measured nothing on
 * every machine that matters. */

#include "unity.h"

#include <jce/middleware/audio/jce_audio.h>
#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/middleware/audio/jce_audio_duck_pump.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define SR        48000u
#define PCM_FRAMES 4800u      /* 0.1 s, looped */
#define AMPLITUDE  16384      /* s16 half scale -> 0.5f */

/* A 500 Hz square wave: |sample| == AMPLITUDE on every frame but the ones at
 * a sign flip, so the block peak is the amplitude regardless of where a block
 * boundary lands.  A sine would make every assertion below depend on block
 * alignment for no gain. */
static int16_t s_pcm[PCM_FRAMES];

static void build_pcm(void)
{
    for (uint32_t i = 0; i < PCM_FRAMES; ++i)
        s_pcm[i] = ((i / 48u) & 1u) ? (int16_t)-AMPLITUDE : (int16_t)AMPLITUDE;
}

/* Play `s_pcm` on `bus`, looping, non-spatial.  jce_audio_voice_set_3d(false)
 * is called EXPLICITLY rather than trusted to be the default: miniaudio
 * enables spatialisation unless a sound is created with
 * MA_SOUND_FLAG_NO_SPATIALIZATION, and jce_audio_play passes no flags -- so a
 * voice at the listener's own position would be attenuated and panned by a
 * model this test has no business depending on.  (jce_audio.h:391 says
 * "Voices default to non-spatial".  Whether that is true is a separate
 * question from this test, and asking it here would make every assertion
 * below depend on the answer.) */
static JceVoice play_on_bus(JceAudio *a, JceSound snd, const char *bus)
{
    JceVoice v = jce_audio_play(a, snd, true, 1.0f, 1.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_VOICE_INVALID, v);
    jce_audio_voice_set_3d(a, v, false);
    jce_audio_voice_set_bus(a, v, bus);
    return v;
}

/* Pump `frames` frames through the graph and throw the audio away; the peak
 * accumulates on the bus node while we do. */
static void render(JceAudio *a, uint32_t frames)
{
    static float out[2048 * 2];
    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = frames - done;
        if (n > 2048u) n = 2048u;
        TEST_ASSERT_TRUE(jce_audio_render_offline(a, out, n));
        done += n;
    }
}

/* ---- The meter reads the real graph ------------------------------- */

/* NEGATIVE CONTROL for everything below.  If this ever passed while nothing
 * was playing AND the positive case passed, the meter would be reporting a
 * constant and both would look like success. */
static void test_meter_reads_zero_with_nothing_playing(void)
{
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(jce_audio_bus_create(a, "SFX"));

    (void)jce_audio_bus_get_peak(a, "SFX");   /* first call arms the meter */
    render(a, 4800u);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_bus_get_peak(a, "SFX"));

    jce_audio_destroy(a);
}

/* POSITIVE CONTROL, and its own negative half: the same bus, same meter,
 * reads a level while a voice plays and returns to zero after it stops. */
static void test_meter_rises_with_a_voice_and_falls_when_it_stops(void)
{
    build_pcm();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(jce_audio_bus_create(a, "SFX"));
    jce_audio_bus_set_volume(a, "SFX", 1.0f);

    JceSound snd = jce_audio_load_pcm(a, s_pcm, (uint32_t)sizeof s_pcm,
                                      1u, SR, 16u);
    TEST_ASSERT_NOT_EQUAL(JCE_SOUND_INVALID, snd);
    JceVoice v = play_on_bus(a, snd, "SFX");

    (void)jce_audio_bus_get_peak(a, "SFX");
    render(a, 4800u);
    float loud = jce_audio_bus_get_peak(a, "SFX");
    TEST_ASSERT_TRUE_MESSAGE(loud > 0.1f, "meter saw no signal from a voice");

    /* Stop, drain one window so the graph goes quiet, then measure a clean
     * window.  The drain is what separates "the meter cleared" from "the
     * meter is stuck". */
    jce_audio_stop(a, v);
    render(a, 4800u);
    (void)jce_audio_bus_get_peak(a, "SFX");
    render(a, 4800u);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_bus_get_peak(a, "SFX"));

    jce_audio_destroy(a);
}

/* THE CONTRACT duck_advance depends on: the number is post-fader.  Asserted
 * as a RATIO, so whatever constant factor the engine's channel conversion
 * applies cancels and the case is about the fader alone. */
static void test_meter_is_post_fader(void)
{
    build_pcm();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(jce_audio_bus_create(a, "SFX"));

    JceSound snd = jce_audio_load_pcm(a, s_pcm, (uint32_t)sizeof s_pcm,
                                      1u, SR, 16u);
    TEST_ASSERT_NOT_EQUAL(JCE_SOUND_INVALID, snd);
    play_on_bus(a, snd, "SFX");

    jce_audio_bus_set_volume(a, "SFX", 1.0f);
    render(a, 2400u);
    (void)jce_audio_bus_get_peak(a, "SFX");
    render(a, 4800u);
    float full = jce_audio_bus_get_peak(a, "SFX");
    TEST_ASSERT_TRUE(full > 0.1f);

    jce_audio_bus_set_volume(a, "SFX", 0.5f);
    render(a, 2400u);                       /* let the fader change settle */
    (void)jce_audio_bus_get_peak(a, "SFX");
    render(a, 4800u);
    float half = jce_audio_bus_get_peak(a, "SFX");

    /* Half the gain, half the peak.  A PRE-fader meter would read the same
     * number twice -- which is exactly the failure that makes a sidechain
     * mis-calibrated at non-unity key volume. */
    TEST_ASSERT_TRUE_MESSAGE(half > 0.0f, "meter went silent at 0.5 gain");
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.5f, half / full);

    jce_audio_destroy(a);
}

/* Read-and-clear: a second read with no audio in between is zero, so a caller
 * polling once per frame can never be handed a stale hold. */
static void test_meter_read_clears(void)
{
    build_pcm();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(jce_audio_bus_create(a, "SFX"));
    JceSound snd = jce_audio_load_pcm(a, s_pcm, (uint32_t)sizeof s_pcm,
                                      1u, SR, 16u);
    play_on_bus(a, snd, "SFX");

    (void)jce_audio_bus_get_peak(a, "SFX");
    render(a, 4800u);
    TEST_ASSERT_TRUE(jce_audio_bus_get_peak(a, "SFX") > 0.1f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_bus_get_peak(a, "SFX"));

    jce_audio_destroy(a);
}

static void test_meter_rejects_nonsense(void)
{
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_bus_get_peak(NULL, "SFX"));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_bus_get_peak(a, "NoSuchBus"));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_bus_get_peak(a, "Master"));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_bus_get_peak(a, NULL));
    jce_audio_destroy(a);
}

/* The offline pump is refused on a device-pumped engine.  This is the reason
 * jce_audio_create_offline() exists as a separate call instead of a sentence
 * in a comment: two pumps on one graph race for the same read cursors, and
 * the caller would get a plausible buffer built from frames the device also
 * consumed.  Uses jce_audio_create(), which succeeds with or without sound
 * hardware (the engine is device-less either way), so this does not skip. */
static void test_render_offline_refuses_a_device_pumped_engine(void)
{
    enum { N = 64 };
    float out[N * 2];
    for (int i = 0; i < N * 2; ++i) out[i] = 7.0f;   /* poison */

    JceAudio *a = jce_audio_create();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_FALSE(jce_audio_render_offline(a, out, (uint32_t)N));
    TEST_ASSERT_EQUAL_FLOAT(7.0f, out[0]);  /* refused means UNTOUCHED */
    jce_audio_destroy(a);

    /* An offline engine with NOTHING attached to its endpoint.  miniaudio has
     * no silence to mix and reads zero frames, so this is the case that
     * proves the silence-fill rather than the read -- and it is the case that
     * caught it: before the fill, this block came back still holding the
     * poison and the frames-written count said 0. */
    JceAudio *b = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(jce_audio_render_offline(b, out, (uint32_t)N));
    for (int i = 0; i < N * 2; ++i)
        TEST_ASSERT_EQUAL_FLOAT(0.0f, out[i]);
    TEST_ASSERT_FALSE(jce_audio_render_offline(b, NULL, (uint32_t)N));
    TEST_ASSERT_FALSE(jce_audio_render_offline(b, out, 0u));
    jce_audio_destroy(b);
}

/* ---- The loop the row is about ------------------------------------ */

/* END TO END: a loud voice on the key bus pulls the target bus down through
 * the LIVE meter, and the target recovers when the key falls silent.
 *
 * It calls jce_audio_duck_pump -- THE function the runtime calls, not a model
 * of it.  rt_apply_mixer is three lines around this same call, and an earlier
 * draft of this file had its own copy of the sampler describing itself as a
 * model; that arrangement cannot catch a defect in the sampler, because the
 * copy can be correct while the runtime's is not.  The two calls after it are
 * the rest of rt_apply_mixer's push, which is what carries the reduction to
 * the device.
 *
 * Before this commit the gain could not move at all: duck_advance had no
 * caller, and resolve_volume_ducked multiplied a permanent 1.0 in every
 * frame. */
static void test_sidechain_ducks_from_the_live_bus_meter(void)
{
    build_pcm();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(jce_audio_bus_create(a, "Music"));
    TEST_ASSERT_TRUE(jce_audio_bus_create(a, "Voice"));

    JceAudioMixer *m = jce_audio_mixer_create();
    TEST_ASSERT_NOT_NULL(m);
    JceAudioBusId music = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER,
                                                  "Music", 1.0f);
    JceAudioBusId voice = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER,
                                                  "Voice", 1.0f);
    JceAudioDuckParams d = jce_audio_duck_default_params();
    d.key = voice;
    d.threshold_db       = -35.0f;
    d.ratio              = 8.0f;
    d.attack_ms          = 5.0f;
    d.release_ms         = 60.0f;
    d.max_attenuation_db = -24.0f;
    TEST_ASSERT_TRUE(jce_audio_mixer_set_sidechain(m, music, &d, SR));

    JceSound snd = jce_audio_load_pcm(a, s_pcm, (uint32_t)sizeof s_pcm,
                                      1u, SR, 16u);
    TEST_ASSERT_NOT_EQUAL(JCE_SOUND_INVALID, snd);
    JceVoice music_v = play_on_bus(a, snd, "Music");
    JceVoice key_v   = play_on_bus(a, snd, "Voice");
    (void)music_v;

    const uint32_t BLOCK = 480u;            /* 10 ms, as one frame would be */
    const float    STEP  = (float)BLOCK / (float)SR;

    /* Idle first: the key is playing but nothing has advanced yet. */
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_audio_mixer_duck_gain(m, music));

    for (int i = 0; i < 100; ++i) {         /* 1 s of key audio */
        render(a, BLOCK);
        jce_audio_duck_pump(a, m, STEP);
        jce_audio_bus_set_volume(a, "Music",
            jce_audio_mixer_resolve_volume_ducked(m, music));
        jce_audio_bus_set_volume(a, "Voice",
            jce_audio_mixer_resolve_volume_ducked(m, voice));
    }
    float ducked = jce_audio_mixer_duck_gain(m, music);
    TEST_ASSERT_TRUE_MESSAGE(ducked < 0.9f,
        "loud key did not duck the target through the live meter");
    TEST_ASSERT_TRUE_MESSAGE(ducked >= 0.05f, "duck blew past its floor");
    /* And the reduction reached the device-side bus, not just the ledger. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, ducked,
                             jce_audio_mixer_resolve_volume_ducked(m, music));

    /* Key falls silent: the follower must release back toward unity.  This is
     * the half that a meter stuck at its maximum would fail. */
    jce_audio_stop(a, key_v);
    for (int i = 0; i < 200; ++i) {
        render(a, BLOCK);
        jce_audio_duck_pump(a, m, STEP);
    }
    float recovered = jce_audio_mixer_duck_gain(m, music);
    TEST_ASSERT_TRUE_MESSAGE(recovered > ducked, "duck never released");
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 1.0f, recovered);

    jce_audio_mixer_destroy(m);
    jce_audio_destroy(a);
}

/* TWO TARGETS, ONE KEY -- the ordinary case (Music and SFX both ducking under
 * Voice), and the one that the read-and-clear meter makes dangerous.  Without
 * a per-pump memo the first callback consumes the key's peak and the SECOND
 * target is handed whatever landed in the microseconds after it: to a first
 * approximation, nothing.  The failure is silent, is confined to projects
 * that configure a second sidechain on the same key, and looks like "that one
 * bus just doesn't duck".
 *
 * This runs against jce_audio_duck_pump itself, so deleting the memo inside
 * it is what this case rejects -- not a copy of the memo living in this
 * file. */
static void test_two_targets_can_share_one_key(void)
{
    build_pcm();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(jce_audio_bus_create(a, "Music"));
    TEST_ASSERT_TRUE(jce_audio_bus_create(a, "SFX"));
    TEST_ASSERT_TRUE(jce_audio_bus_create(a, "Voice"));

    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId music = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 1.0f);
    JceAudioBusId sfx   = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX",   1.0f);
    JceAudioBusId voice = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Voice", 1.0f);

    JceAudioDuckParams d = jce_audio_duck_default_params();
    d.key = voice; d.threshold_db = -35.0f; d.ratio = 8.0f;
    d.attack_ms = 5.0f; d.release_ms = 60.0f; d.max_attenuation_db = -24.0f;
    TEST_ASSERT_TRUE(jce_audio_mixer_set_sidechain(m, music, &d, SR));
    TEST_ASSERT_TRUE(jce_audio_mixer_set_sidechain(m, sfx,   &d, SR));

    JceSound snd = jce_audio_load_pcm(a, s_pcm, (uint32_t)sizeof s_pcm,
                                      1u, SR, 16u);
    play_on_bus(a, snd, "Voice");

    for (int i = 0; i < 100; ++i) {
        render(a, 480u);
        jce_audio_duck_pump(a, m, 480.0f / (float)SR);
    }

    float gm = jce_audio_mixer_duck_gain(m, music);
    float gs = jce_audio_mixer_duck_gain(m, sfx);
    TEST_ASSERT_TRUE_MESSAGE(gm < 0.9f, "first target did not duck");
    TEST_ASSERT_TRUE_MESSAGE(gs < 0.9f, "SECOND target on the same key did "
                                        "not duck -- the key meter was "
                                        "consumed by the first");
    /* Same key, same params: the two must agree, not merely both be < 1. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, gm, gs);

    jce_audio_mixer_destroy(m);
    jce_audio_destroy(a);
}

/* ---- What duck_advance folds in, and what it must not ------------- */

static float s_synth_peak;
static float synth_key_peak(JceAudioBusId key, void *user)
{ (void)key; (void)user; return s_synth_peak; }

static float settle(JceAudioMixer *m, JceAudioBusId target, int steps)
{
    for (int i = 0; i < steps; ++i)
        jce_audio_mixer_duck_advance(m, 256u, synth_key_peak, NULL);
    return jce_audio_mixer_duck_gain(m, target);
}

/* MUTATION CONTROL for the one behaviour change in this commit.
 *
 * duck_advance used to multiply the reported key level by the key's full
 * resolved volume.  Against a POST-fader meter that squares the key's gain:
 * exact at 1.0 and 6 dB wrong at 0.5 -- which every test anyone writes at
 * volume 1.0 would pass.  So the case is built at a volume that is NOT 1.0:
 * the same measured level must produce the same duck whatever the key fader
 * says, because the fader is already inside the measurement.
 *
 * Restore the `key_peak_lin *= resolve_volume(key)` line and this fails. */
static void test_duck_uses_the_measured_level_not_its_square(void)
{
    JceAudioMixer *a = jce_audio_mixer_create();
    JceAudioBusId a_mus = jce_audio_mixer_add_bus(a, JCE_AUDIO_BUS_MASTER, "Music", 1.0f);
    JceAudioBusId a_key = jce_audio_mixer_add_bus(a, JCE_AUDIO_BUS_MASTER, "Voice", 1.0f);

    JceAudioMixer *b = jce_audio_mixer_create();
    JceAudioBusId b_mus = jce_audio_mixer_add_bus(b, JCE_AUDIO_BUS_MASTER, "Music", 1.0f);
    JceAudioBusId b_key = jce_audio_mixer_add_bus(b, JCE_AUDIO_BUS_MASTER, "Voice", 0.25f);

    JceAudioDuckParams d = jce_audio_duck_default_params();
    d.threshold_db = -35.0f; d.ratio = 8.0f;
    d.attack_ms = 5.0f; d.release_ms = 60.0f; d.max_attenuation_db = -24.0f;
    JceAudioDuckParams da = d; da.key = a_key;
    JceAudioDuckParams db = d; db.key = b_key;
    TEST_ASSERT_TRUE(jce_audio_mixer_set_sidechain(a, a_mus, &da, SR));
    TEST_ASSERT_TRUE(jce_audio_mixer_set_sidechain(b, b_mus, &db, SR));

    s_synth_peak = 0.45f;                   /* the METERED level, both sides */
    float ga = settle(a, a_mus, 400);
    float gb = settle(b, b_mus, 400);

    TEST_ASSERT_TRUE_MESSAGE(ga < 0.9f, "control did not duck at all");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, ga, gb,
        "key fader changed the duck although the meter is post-fader");

    jce_audio_mixer_destroy(b);
    jce_audio_mixer_destroy(a);
}

/* The half of the old behaviour that was KEPT: mute/solo still gates the key,
 * because a muted bus is still metered -- the meter sees whatever the fader
 * last carried, and mute is not a fader. */
static void test_muted_or_unsoloed_key_cannot_duck(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId mus = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 1.0f);
    JceAudioBusId key = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Voice", 1.0f);
    JceAudioBusId oth = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX",   1.0f);
    JceAudioDuckParams d = jce_audio_duck_default_params();
    d.key = key; d.threshold_db = -35.0f; d.ratio = 8.0f;
    d.attack_ms = 5.0f; d.release_ms = 60.0f;
    TEST_ASSERT_TRUE(jce_audio_mixer_set_sidechain(m, mus, &d, SR));

    s_synth_peak = 0.9f;
    TEST_ASSERT_TRUE_MESSAGE(settle(m, mus, 400) < 0.9f,
                             "positive control: an audible key must duck");

    /* Muted key: no duck, and it releases all the way back. */
    jce_audio_mixer_set_muted(m, key, true);
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 1.0f, settle(m, mus, 2000));
    jce_audio_mixer_set_muted(m, key, false);
    TEST_ASSERT_TRUE(settle(m, mus, 400) < 0.9f);

    /* Another bus solo'd: the key is off the solo path, so it is inaudible
     * and must not duck either. */
    jce_audio_mixer_set_solo(m, oth, true);
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 1.0f, settle(m, mus, 2000));

    jce_audio_mixer_destroy(m);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_meter_reads_zero_with_nothing_playing);
    RUN_TEST(test_meter_rises_with_a_voice_and_falls_when_it_stops);
    RUN_TEST(test_meter_is_post_fader);
    RUN_TEST(test_meter_read_clears);
    RUN_TEST(test_meter_rejects_nonsense);
    RUN_TEST(test_render_offline_refuses_a_device_pumped_engine);
    RUN_TEST(test_sidechain_ducks_from_the_live_bus_meter);
    RUN_TEST(test_two_targets_can_share_one_key);
    RUN_TEST(test_duck_uses_the_measured_level_not_its_square);
    RUN_TEST(test_muted_or_unsoloed_key_cannot_duck);
    return UNITY_END();
}
