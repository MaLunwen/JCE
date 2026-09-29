/*
 * test_jce_audio_send_cycle.c — an aux send may not close a ring.
 *
 * jce_audio_mixer_set_send guarded exactly one case, `src == dest`.  A->B
 * followed by B->A was accepted, and so was any longer ring.
 *
 * It has been harmless for one reason only: NOTHING ROUTES A SEND'S AUDIO.
 * jce_audio_mixer_resolve_send has no caller outside these tests, so the ring
 * is bookkeeping nobody walks.  The moment a device-side send exists it is a
 * ring in the miniaudio node graph, and pulling frames through it recurses
 * with no bottom -- not a wrong number, a hang.
 *
 * So the refusal lands BEFORE any routing, on its own, in the layer that
 * knows the graph.  These cases are what "may not close a ring" has to mean,
 * and they run with no device at all.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory.
 */
#include "unity.h"

#include <jce/middleware/audio/jce_audio_mixer.h>

void setUp(void) {}
void tearDown(void) {}

static JceAudioMixer *M;
static JceAudioBusId  A, B, C, D;

static void build(void)
{
    M = jce_audio_mixer_create();
    TEST_ASSERT_NOT_NULL(M);
    A = jce_audio_mixer_add_bus(M, JCE_AUDIO_BUS_MASTER, "A", 1.0f);
    B = jce_audio_mixer_add_bus(M, JCE_AUDIO_BUS_MASTER, "B", 1.0f);
    C = jce_audio_mixer_add_bus(M, JCE_AUDIO_BUS_MASTER, "C", 1.0f);
    D = jce_audio_mixer_add_bus(M, JCE_AUDIO_BUS_MASTER, "D", 1.0f);
}

static void teardown_mixer(void) { jce_audio_mixer_destroy(M); M = NULL; }

/* THE CASE THIS FILE EXISTS FOR.  Two hops, which `src == dest` never saw. */
static void test_a_two_hop_ring_is_refused(void)
{
    build();
    TEST_ASSERT_TRUE_MESSAGE(jce_audio_mixer_set_send(M, A, B, 0.5f),
        "positive control: a plain send must still be accepted");
    TEST_ASSERT_FALSE_MESSAGE(jce_audio_mixer_set_send(M, B, A, 0.5f),
        "B->A closes a ring with A->B and must be refused");

    /* Refused means NOT STORED.  A function that returns false and writes
     * anyway leaves the ring in the table for whoever walks it next. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_mixer_get_send(M, B, A));
    /* And the legitimate send it was tested against is untouched. */
    TEST_ASSERT_EQUAL_FLOAT(0.5f, jce_audio_mixer_get_send(M, A, B));
    teardown_mixer();
}

static void test_a_longer_ring_is_refused(void)
{
    build();
    TEST_ASSERT_TRUE(jce_audio_mixer_set_send(M, A, B, 0.5f));
    TEST_ASSERT_TRUE(jce_audio_mixer_set_send(M, B, C, 0.5f));
    TEST_ASSERT_FALSE_MESSAGE(jce_audio_mixer_set_send(M, C, A, 0.5f),
        "C->A closes the ring A->B->C and must be refused");
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_mixer_get_send(M, C, A));
    teardown_mixer();
}

/* THE NEGATIVE CONTROL, and it is the one that stops this guard being a
 * blanket refusal.  A diamond is not a ring: two buses may both send into a
 * third, and a chain may fan out, without anything ever returning to its
 * source.  A guard that rejected these would be indistinguishable from one
 * that rejected everything, and every case above would still pass. */
static void test_a_diamond_is_not_a_ring(void)
{
    build();
    TEST_ASSERT_TRUE(jce_audio_mixer_set_send(M, A, B, 0.5f));
    TEST_ASSERT_TRUE(jce_audio_mixer_set_send(M, A, C, 0.5f));
    TEST_ASSERT_TRUE_MESSAGE(jce_audio_mixer_set_send(M, B, D, 0.5f),
        "B->D does not return to A and must be accepted");
    TEST_ASSERT_TRUE_MESSAGE(jce_audio_mixer_set_send(M, C, D, 0.5f),
        "C->D joins the same destination from a second path -- a diamond, "
        "not a ring");
    TEST_ASSERT_EQUAL_FLOAT(0.5f, jce_audio_mixer_get_send(M, C, D));
    teardown_mixer();
}

/* The one-hop case the old guard did cover, kept so that fixing the two-hop
 * case cannot quietly drop it. */
static void test_self_send_is_still_refused(void)
{
    build();
    TEST_ASSERT_FALSE(jce_audio_mixer_set_send(M, A, A, 0.5f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_mixer_get_send(M, A, A));
    teardown_mixer();
}

/* Removing a send must re-open the path it was blocking: the refusal is a
 * statement about the CURRENT graph, not a permanent ban on a pair. */
static void test_removing_a_send_reopens_the_other_direction(void)
{
    build();
    TEST_ASSERT_TRUE(jce_audio_mixer_set_send(M, A, B, 0.5f));
    TEST_ASSERT_FALSE(jce_audio_mixer_set_send(M, B, A, 0.5f));

    TEST_ASSERT_TRUE(jce_audio_mixer_remove_send(M, A, B));
    TEST_ASSERT_TRUE_MESSAGE(jce_audio_mixer_set_send(M, B, A, 0.5f),
        "with A->B gone, B->A closes nothing and must be accepted");
    teardown_mixer();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_two_hop_ring_is_refused);
    RUN_TEST(test_a_longer_ring_is_refused);
    RUN_TEST(test_a_diamond_is_not_a_ring);
    RUN_TEST(test_self_send_is_still_refused);
    RUN_TEST(test_removing_a_send_reopens_the_other_direction);
    return UNITY_END();
}
