/*
 * test_jce_net_animator.c -- the three things that make animator state
 * different from a transform, each of which is a bug when it is not handled.
 */
#include "unity.h"

#include <jce/middleware/net/jce_net_animator.h>

#include <string.h>

void setUp(void)    { jce_net_animator_shutdown(); }
void tearDown(void) { jce_net_animator_shutdown(); }

static JceNetAnimatorState st(uint32_t hash, float t, float speed)
{
    JceNetAnimatorState s;
    memset(&s, 0, sizeof s);
    s.state_hash = hash;
    s.normalized_time = t;
    s.speed = speed;
    return s;
}

/* 1. NORMALISED TIME WRAPS. */
static void test_normalized_time_goes_forward_through_the_loop(void)
{
    const JceNetObjectId id = 42;
    TEST_ASSERT_TRUE(jce_net_animator_register(id, NULL));

    const uint32_t walk = jce_net_animator_hash("Walk");
    /* 0.95 -> 0.05 is 0.10 FORWARD through the loop point.  A plain lerp
     * reads it as 0.90 backward and plays the clip in reverse, once per
     * loop, forever. */
    JceNetAnimatorState a = st(walk, 0.95f, 1.0f);
    JceNetAnimatorState b = st(walk, 0.05f, 1.0f);
    jce_net_animator_inject_snapshot(id, 100, &a);
    jce_net_animator_inject_snapshot(id, 110, &b);
    jce_net_animator_render_step(0.0);

    JceNetAnimatorState out;
    TEST_ASSERT_TRUE(jce_net_animator_get_state(id, &out));
    TEST_ASSERT_EQUAL_UINT32(walk, out.state_hash);
    /* Anywhere in [0.95, 1.0) or [0.0, 0.05] is forward; the failure mode is
     * a value in the MIDDLE of the clip, which is what a plain lerp gives. */
    const bool forward = (out.normalized_time >= 0.94f) ||
                         (out.normalized_time <= 0.06f);
    TEST_ASSERT_TRUE_MESSAGE(forward,
        "normalized_time interpolated BACKWARD through the loop point");
}

static void test_normalized_time_still_lerps_normally_away_from_the_wrap(void)
{
    /* The negative control for the case above: the wrap handling must not
     * change ordinary interpolation.  0.20 -> 0.60 is 0.40 forward and has no
     * shorter way round. */
    const JceNetObjectId id = 43;
    jce_net_animator_register(id, NULL);
    const uint32_t run = jce_net_animator_hash("Run");
    JceNetAnimatorState a = st(run, 0.20f, 1.0f);
    JceNetAnimatorState b = st(run, 0.60f, 1.0f);
    jce_net_animator_inject_snapshot(id, 100, &a);
    jce_net_animator_inject_snapshot(id, 110, &b);
    jce_net_animator_render_step(0.0);

    JceNetAnimatorState out;
    TEST_ASSERT_TRUE(jce_net_animator_get_state(id, &out));
    TEST_ASSERT_TRUE(out.normalized_time > 0.20f);
    TEST_ASSERT_TRUE(out.normalized_time < 0.60f);
}

/* 2. A STATE CHANGE IS NOT A BLEND. */
static void test_a_state_change_snaps_instead_of_blending(void)
{
    const JceNetObjectId id = 44;
    jce_net_animator_register(id, NULL);
    const uint32_t idle = jce_net_animator_hash("Idle");
    const uint32_t jump = jce_net_animator_hash("Jump");
    TEST_ASSERT_TRUE(idle != jump);

    JceNetAnimatorState a = st(idle, 0.10f, 1.0f);
    JceNetAnimatorState b = st(jump, 0.80f, 2.0f);
    jce_net_animator_inject_snapshot(id, 100, &a);
    jce_net_animator_inject_snapshot(id, 110, &b);
    jce_net_animator_render_step(0.0);

    JceNetAnimatorState out;
    TEST_ASSERT_TRUE(jce_net_animator_get_state(id, &out));
    /* Forty per cent of the way from Idle to Jump is not a pose. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(jump, out.state_hash,
        "a state change was blended instead of snapped");
    TEST_ASSERT_EQUAL_FLOAT(0.80f, out.normalized_time);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, out.speed);
}

/* 3. A TRIGGER IS AN EDGE. */
static void test_triggers_queue_in_order_and_drop_the_oldest_when_full(void)
{
    const JceNetObjectId id = 45;
    jce_net_animator_register(id, NULL);

    uint32_t got = 0;
    TEST_ASSERT_FALSE_MESSAGE(jce_net_animator_poll_trigger(id, &got),
        "an empty queue reported a trigger");

    /* The queue holds 8.  Pushing 10 must keep the LAST 8: a queue that
     * discarded NEW triggers when full stops responding exactly when the most
     * is happening. */
    for (uint32_t i = 1; i <= 10; i++)
        jce_net_animator_inject_trigger(id, i);

    uint32_t first = 0;
    TEST_ASSERT_TRUE(jce_net_animator_poll_trigger(id, &first));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, first,
        "the queue dropped new triggers instead of old ones");

    uint32_t v = 0, n = 1;
    while (jce_net_animator_poll_trigger(id, &v)) n++;
    TEST_ASSERT_EQUAL_UINT32(8u, n);
    TEST_ASSERT_EQUAL_UINT32(10u, v);   /* FIFO: the newest comes out last */
}

/* Nothing received is not the same as idle. */
static void test_never_received_is_false_not_a_zeroed_state(void)
{
    JceNetAnimatorState out;
    memset(&out, 0xAB, sizeof out);
    TEST_ASSERT_FALSE(jce_net_animator_get_state(999, &out));

    jce_net_animator_register(46, NULL);
    TEST_ASSERT_FALSE_MESSAGE(jce_net_animator_get_state(46, &out),
        "a registered object that has received nothing reported a state");
}

static void test_registration_is_idempotent_and_unregister_frees_the_slot(void)
{
    TEST_ASSERT_TRUE(jce_net_animator_register(1, NULL));
    TEST_ASSERT_TRUE(jce_net_animator_register(1, NULL));
    TEST_ASSERT_EQUAL_UINT32(1u, jce_net_animator_registered_count());
    jce_net_animator_register(2, NULL);
    TEST_ASSERT_EQUAL_UINT32(2u, jce_net_animator_registered_count());
    jce_net_animator_unregister(1);
    TEST_ASSERT_EQUAL_UINT32(1u, jce_net_animator_registered_count());
    jce_net_animator_unregister(1234);   /* unknown: silent */
    TEST_ASSERT_EQUAL_UINT32(1u, jce_net_animator_registered_count());
}

static void test_the_hash_is_stable_and_distinguishes(void)
{
    /* Both sides must agree, so this is public and must not drift. */
    TEST_ASSERT_EQUAL_UINT32(jce_net_animator_hash("Idle"),
                             jce_net_animator_hash("Idle"));
    TEST_ASSERT_TRUE(jce_net_animator_hash("Idle") !=
                     jce_net_animator_hash("idle"));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_net_animator_hash(NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_normalized_time_goes_forward_through_the_loop);
    RUN_TEST(test_normalized_time_still_lerps_normally_away_from_the_wrap);
    RUN_TEST(test_a_state_change_snaps_instead_of_blending);
    RUN_TEST(test_triggers_queue_in_order_and_drop_the_oldest_when_full);
    RUN_TEST(test_never_received_is_false_not_a_zeroed_state);
    RUN_TEST(test_registration_is_idempotent_and_unregister_frees_the_slot);
    RUN_TEST(test_the_hash_is_stable_and_distinguishes);
    return UNITY_END();
}
