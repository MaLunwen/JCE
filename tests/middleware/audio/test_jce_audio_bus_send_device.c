/*
 * test_jce_audio_bus_send_device.c
 *
 * The DEVICE side of an aux send: does a scaled copy of one bus's audio
 * actually arrive at another bus?
 *
 * jce_audio_mixer has modelled sends for a long time -- set_send, send_at,
 * resolve_send -- the mixer panel authors them per bus and the config parser
 * reads them back at Play start.  And nothing routed any audio:
 * jce_audio_mixer_resolve_send had no caller outside its own header and
 * implementation, because the device side offered only bus_create,
 * bus_set_volume and voice_set_bus.  Every bus was a flat child of Master
 * with one gain and there was no parallel tap to send into.
 *
 * WHAT IS MEASURED, AND WHY IT IS THE DESTINATION BUS'S METER.  A send is a
 * PARALLEL tap: the source keeps feeding its own output either way, so the
 * source's level says nothing about whether a send happened.  The observable
 * has to be the DESTINATION reading a level it has no voice of its own to
 * explain.  jce_audio_bus_get_peak is read-and-clear, so each case reads it
 * exactly once.
 *
 * NO DEVICE IS NEEDED and no case may skip: jce_audio_create_offline() plus
 * jce_audio_render_offline() run the real miniaudio node graph, splitter
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

#define SR      48000u
#define FRAMES  2400u
#define RENDER  512u

static int16_t s_pcm[FRAMES];

static void build_pcm(void)
{
    for (uint32_t i = 0; i < FRAMES; ++i)
        s_pcm[i] = (int16_t)((i % 32u) < 16u ? 12000 : -12000);
}

/* Play a voice on "SFX" with an optional send into "Reverb", render, and
 * return the peak the DESTINATION bus saw. */
static float dest_peak_with_send(float amount, bool make_send)
{
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(jce_audio_bus_create(a, "SFX"));
    TEST_ASSERT_TRUE(jce_audio_bus_create(a, "Reverb"));
    jce_audio_bus_set_volume(a, "SFX", 1.0f);
    jce_audio_bus_set_volume(a, "Reverb", 1.0f);

    if (make_send)
        TEST_ASSERT_TRUE_MESSAGE(
            jce_audio_bus_set_send(a, "SFX", "Reverb", amount),
            "jce_audio_bus_set_send refused a legitimate send");

    JceSound snd = jce_audio_load_pcm(a, s_pcm, (uint32_t)sizeof s_pcm,
                                      1u, SR, 16u);
    TEST_ASSERT_NOT_EQUAL(JCE_SOUND_INVALID, snd);
    JceVoice v = jce_audio_play(a, snd, true, 1.0f, 1.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_VOICE_INVALID, v);
    jce_audio_voice_set_bus(a, v, "SFX");

    /* ARM THE METER BEFORE RENDERING.  jce_audio_bus_get_peak stands the
     * bus's insert node up on first call and that node IS the meter, so
     * reading it only after the render measures a node that did not exist
     * while the audio flowed -- every case would read 0 and the negative
     * control would pass for the wrong reason. */
    (void)jce_audio_bus_get_peak(a, "Reverb");

    float out[RENDER * 2];
    for (int n = 0; n < 8; ++n) {
        memset(out, 0, sizeof out);
        TEST_ASSERT_TRUE(jce_audio_render_offline(a, out, RENDER));
    }
    /* Read-and-clear, once. */
    float peak = jce_audio_bus_get_peak(a, "Reverb");
    jce_audio_destroy(a);
    return peak;
}

/* NEGATIVE CONTROL FIRST.  With NO send, the destination bus has no voice of
 * its own and must read silence.  Without this, every case below is satisfied
 * by a build where every bus hears everything. */
static void test_the_destination_is_silent_without_a_send(void)
{
    build_pcm();
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
        0.001f, 0.0f, dest_peak_with_send(0.0f, false),
        "a bus with no voice and no send into it reported a level -- buses "
        "are not isolated, so 'the send arrived' cannot be measured here");
}

/* THE REGRESSION: audio arrives where the send points. */
static void test_a_send_delivers_audio_to_the_destination_bus(void)
{
    build_pcm();
    const float got = dest_peak_with_send(1.0f, true);
    TEST_ASSERT_TRUE_MESSAGE(
        got > 0.01f,
        "the destination bus heard nothing from an aux send at amount 1.0 -- "
        "the send is authored and modelled but still routes no audio, which "
        "is the defect this row was opened for");
}

/* The amount is a GAIN, not a switch: half the send is quieter than all of it
 * and both are louder than none.  A boolean implementation passes the case
 * above and fails this one. */
static void test_the_send_amount_scales_the_delivered_level(void)
{
    build_pcm();
    const float full = dest_peak_with_send(1.0f, true);
    const float half = dest_peak_with_send(0.25f, true);

    TEST_ASSERT_TRUE_MESSAGE(half > 0.001f,
        "a send at 0.25 delivered nothing at all");
    TEST_ASSERT_TRUE_MESSAGE(full > half + 0.01f,
        "sends at 1.0 and 0.25 delivered the same level -- the amount is "
        "being treated as on/off rather than as a gain");
}

/* A bus may not send to itself: that edge feeds the node its own input comes
 * from, and pulling frames through it recurses with no bottom.  Refused at the
 * device layer, not merely at the authoring layer. */
static void test_a_self_send_is_refused(void)
{
    build_pcm();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(jce_audio_bus_create(a, "SFX"));

    TEST_ASSERT_FALSE_MESSAGE(
        jce_audio_bus_set_send(a, "SFX", "SFX", 0.5f),
        "a bus was allowed to send to itself -- that is a ring in the node "
        "graph, and the first pull through it does not return");
    TEST_ASSERT_FALSE_MESSAGE(
        jce_audio_bus_set_send(a, "SFX", "NoSuchBus", 0.5f),
        "a send to an unknown bus was accepted");
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_bus_get_send(a, "SFX", "SFX"));

    jce_audio_destroy(a);
}

/* The read-back reports what was set, and reports 0 for a bus that is not the
 * send's destination -- so a serializer cannot mistake "sends to B" for
 * "sends to C". */
static void test_get_send_round_trips_and_is_destination_specific(void)
{
    build_pcm();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(jce_audio_bus_create(a, "SFX"));
    TEST_ASSERT_TRUE(jce_audio_bus_create(a, "Reverb"));
    TEST_ASSERT_TRUE(jce_audio_bus_create(a, "Delay"));

    TEST_ASSERT_TRUE(jce_audio_bus_set_send(a, "SFX", "Reverb", 0.6f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.6f,
                             jce_audio_bus_get_send(a, "SFX", "Reverb"));
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(
        0.0f, jce_audio_bus_get_send(a, "SFX", "Delay"),
        "get_send reported a send to a bus that is not its destination");

    /* A SECOND destination ADDS a send; it does not re-point the first.  A
     * console strip feeds several returns at once, and jce_audio_mixer has
     * always modelled that (send_count / send_at, capped at 8). */
    TEST_ASSERT_TRUE(jce_audio_bus_set_send(a, "SFX", "Delay", 0.3f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.3f,
                             jce_audio_bus_get_send(a, "SFX", "Delay"));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
        0.001f, 0.6f, jce_audio_bus_get_send(a, "SFX", "Reverb"),
        "adding a send to a second bus silently dropped the first -- the "
        "device routes fewer sends than the mixer authors, which is the "
        "partial wiring this row is about");

    /* Re-scaling the SAME destination reuses its slot rather than consuming
     * a second one, or eight repeated calls would exhaust the bus. */
    TEST_ASSERT_TRUE(jce_audio_bus_set_send(a, "SFX", "Reverb", 0.2f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.2f,
                             jce_audio_bus_get_send(a, "SFX", "Reverb"));

    /* Zero retires it. */
    TEST_ASSERT_TRUE(jce_audio_bus_set_send(a, "SFX", "Reverb", 0.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_bus_get_send(a, "SFX", "Reverb"));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
        0.001f, 0.3f, jce_audio_bus_get_send(a, "SFX", "Delay"),
        "retiring one send disturbed another");

    jce_audio_destroy(a);
}

/* ORDER INDEPENDENCE.  Standing up the SOURCE bus's insert chain after a send
 * already exists must not detach the send.  It did: bus_chain_ensure spliced
 * group -> dsp -> audio_output_node() unconditionally, which re-pointed the
 * bus straight at the endpoint and orphaned the splitter -- and get_send kept
 * reporting the amount, so the send looked configured and carried nothing.
 * The dsp now targets the splitter when one exists.
 *
 * This is the only case that creates the source's dsp at all; the others arm
 * the DESTINATION meter, which stands up a node on the wrong bus to see it. */
static void test_a_later_insert_chain_does_not_detach_the_send(void)
{
    build_pcm();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(jce_audio_bus_create(a, "SFX"));
    TEST_ASSERT_TRUE(jce_audio_bus_create(a, "Reverb"));
    jce_audio_bus_set_volume(a, "SFX", 1.0f);
    jce_audio_bus_set_volume(a, "Reverb", 1.0f);

    TEST_ASSERT_TRUE(jce_audio_bus_set_send(a, "SFX", "Reverb", 1.0f));
    /* AFTER the send: this stands up the SOURCE bus's insert node. */
    (void)jce_audio_bus_get_peak(a, "SFX");
    (void)jce_audio_bus_get_peak(a, "Reverb");

    JceSound snd = jce_audio_load_pcm(a, s_pcm, (uint32_t)sizeof s_pcm,
                                      1u, SR, 16u);
    TEST_ASSERT_NOT_EQUAL(JCE_SOUND_INVALID, snd);
    JceVoice v = jce_audio_play(a, snd, true, 1.0f, 1.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_VOICE_INVALID, v);
    jce_audio_voice_set_bus(a, v, "SFX");

    float out[RENDER * 2];
    for (int n = 0; n < 8; ++n) {
        memset(out, 0, sizeof out);
        TEST_ASSERT_TRUE(jce_audio_render_offline(a, out, RENDER));
    }
    const float dest = jce_audio_bus_get_peak(a, "Reverb");
    jce_audio_destroy(a);

    TEST_ASSERT_TRUE_MESSAGE(dest > 0.01f,
        "the destination went silent once the SOURCE bus grew an insert chain "
        "-- the dsp was spliced straight to the output and orphaned the "
        "splitter, while get_send kept reporting the send as configured");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_destination_is_silent_without_a_send);
    RUN_TEST(test_a_send_delivers_audio_to_the_destination_bus);
    RUN_TEST(test_the_send_amount_scales_the_delivered_level);
    RUN_TEST(test_a_self_send_is_refused);
    RUN_TEST(test_get_send_round_trips_and_is_destination_specific);
    RUN_TEST(test_a_later_insert_chain_does_not_detach_the_send);
    return UNITY_END();
}
