/*
 * test_jce_input_command.c — CLIENT->SERVER input command channel (F12 slice).
 *
 * Proves the PURE, transport-free core of the upstream input channel:
 *
 *   1. CODEC round-trip: fixed-layout encode -> decode reproduces every field
 *      byte-exactly; decode of a too-small buffer fails (returns 0); encode
 *      into a too-small buffer fails (returns 0).
 *   2. STORE latest-wins: highest-tick-per-client wins, older ticks are
 *      ignored, an unknown client returns false, reset clears everything, and
 *      multiple clients are independent.  Bounded capacity is respected.
 *   3. HANDLER path (no live socket): the SAME decode+store seam the server
 *      RPC handler forwards to (jce_input_command_server_receive) round-trips
 *      an encoded command into the store under a fake sender id; bad bytes are
 *      rejected.
 *
 * Pure module -> links jce_core + jce_net only (no ECS / scene / physics /
 * runtime / transport).  Live over-the-wire client->server RPC is F12; this is
 * the deterministic proof of the codec + store + receive-handler logic.
 */

#include "unity.h"

#include <jce/middleware/net/jce_net_input_command.h>

#include <stdint.h>
#include <string.h>

void setUp(void)    { jce_input_command_store_reset(); }
void tearDown(void) { jce_input_command_store_reset(); }

/* ================================================================== */
/* 1. Codec round-trip                                                 */
/* ================================================================== */

static JceInputCommand make_cmd(uint32_t tick, float wx, float wz, float sm,
                                uint8_t jump, uint8_t sprint)
{
    JceInputCommand c;
    memset(&c, 0, sizeof c);
    c.tick       = tick;
    c.walk_x     = wx;
    c.walk_z     = wz;
    c.speed_mult = sm;
    c.jump       = jump;
    c.sprint     = sprint;
    return c;
}

static void test_codec_round_trip(void)
{
    JceInputCommand in = make_cmd(42u, 0.5f, -0.25f, 1.0f, /*jump=*/1u,
                                  /*sprint=*/0u);

    uint8_t  buf[JCE_INPUT_COMMAND_WIRE_SIZE];
    uint32_t n = jce_input_command_encode(&in, buf, sizeof buf);
    TEST_ASSERT_EQUAL_UINT32(JCE_INPUT_COMMAND_WIRE_SIZE, n);

    JceInputCommand out;
    uint32_t rd = jce_input_command_decode(buf, n, &out);
    TEST_ASSERT_EQUAL_UINT32(JCE_INPUT_COMMAND_WIRE_SIZE, rd);

    /* Every field byte-equal across the round-trip. */
    TEST_ASSERT_EQUAL_UINT32(in.tick,   out.tick);
    TEST_ASSERT_EQUAL_FLOAT (in.walk_x, out.walk_x);
    TEST_ASSERT_EQUAL_FLOAT (in.walk_z, out.walk_z);
    TEST_ASSERT_EQUAL_FLOAT (in.speed_mult, out.speed_mult);
    TEST_ASSERT_EQUAL_UINT8 (in.jump,   out.jump);
    TEST_ASSERT_EQUAL_UINT8 (in.sprint, out.sprint);

    /* Floats are bit-exact (IEEE-754 bit copy), so memcmp of the encoded
     * fields matches a re-encode of the decoded struct. */
    uint8_t buf2[JCE_INPUT_COMMAND_WIRE_SIZE];
    TEST_ASSERT_EQUAL_UINT32(JCE_INPUT_COMMAND_WIRE_SIZE,
                             jce_input_command_encode(&out, buf2, sizeof buf2));
    TEST_ASSERT_EQUAL_MEMORY(buf, buf2, JCE_INPUT_COMMAND_WIRE_SIZE);
}

static void test_codec_rejects_bad_size(void)
{
    JceInputCommand in = make_cmd(7u, 1.0f, 0.0f, 1.0f, 0u, 1u);
    uint8_t buf[JCE_INPUT_COMMAND_WIRE_SIZE];

    /* Encode into a too-small destination -> 0. */
    TEST_ASSERT_EQUAL_UINT32(
        0u, jce_input_command_encode(&in, buf,
                                     JCE_INPUT_COMMAND_WIRE_SIZE - 1u));

    /* A valid encode, then decode with a truncated size -> 0. */
    TEST_ASSERT_EQUAL_UINT32(JCE_INPUT_COMMAND_WIRE_SIZE,
                             jce_input_command_encode(&in, buf, sizeof buf));
    JceInputCommand out;
    TEST_ASSERT_EQUAL_UINT32(
        0u, jce_input_command_decode(buf, JCE_INPUT_COMMAND_WIRE_SIZE - 1u,
                                     &out));

    /* NULL args are rejected. */
    TEST_ASSERT_EQUAL_UINT32(0u, jce_input_command_encode(NULL, buf, sizeof buf));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_input_command_encode(&in, NULL, sizeof buf));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_input_command_decode(NULL, sizeof buf, &out));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_input_command_decode(buf, sizeof buf, NULL));
}

/* ================================================================== */
/* 2. Store latest-wins + bounded + per-client independent             */
/* ================================================================== */

static void test_store_latest_wins(void)
{
    JceInputCommand got;

    /* Unknown client -> false. */
    TEST_ASSERT_FALSE(jce_input_command_get_latest(1u, &got));

    /* store(client=1, tick=10) then store(client=1, tick=12) -> 12 wins. */
    JceInputCommand c10 = make_cmd(10u, 0.1f, 0.0f, 1.0f, 0u, 0u);
    JceInputCommand c12 = make_cmd(12u, 0.2f, 0.0f, 1.0f, 1u, 0u);
    jce_input_command_store(1u, &c10);
    jce_input_command_store(1u, &c12);
    TEST_ASSERT_TRUE(jce_input_command_get_latest(1u, &got));
    TEST_ASSERT_EQUAL_UINT32(12u, got.tick);
    TEST_ASSERT_EQUAL_FLOAT(0.2f, got.walk_x);
    TEST_ASSERT_EQUAL_UINT8(1u, got.jump);

    /* An OLDER tick=11 must be ignored (latest-wins by tick). */
    JceInputCommand c11 = make_cmd(11u, 9.9f, 0.0f, 1.0f, 0u, 1u);
    jce_input_command_store(1u, &c11);
    TEST_ASSERT_TRUE(jce_input_command_get_latest(1u, &got));
    TEST_ASSERT_EQUAL_UINT32(12u, got.tick);   /* still 12 */
    TEST_ASSERT_EQUAL_FLOAT(0.2f, got.walk_x); /* not clobbered by c11 */

    /* Equal tick is also ignored (<= rule). */
    JceInputCommand c12b = make_cmd(12u, 7.7f, 0.0f, 1.0f, 0u, 0u);
    jce_input_command_store(1u, &c12b);
    TEST_ASSERT_TRUE(jce_input_command_get_latest(1u, &got));
    TEST_ASSERT_EQUAL_FLOAT(0.2f, got.walk_x); /* unchanged */

    /* reset -> nothing stored. */
    jce_input_command_store_reset();
    TEST_ASSERT_FALSE(jce_input_command_get_latest(1u, &got));
}

static void test_store_clients_independent(void)
{
    JceInputCommand got;

    JceInputCommand a = make_cmd(5u, 1.0f, 0.0f, 1.0f, 0u, 0u);
    JceInputCommand b = make_cmd(8u, 0.0f, 1.0f, 1.0f, 1u, 1u);
    jce_input_command_store(1u, &a);
    jce_input_command_store(2u, &b);

    TEST_ASSERT_TRUE(jce_input_command_get_latest(1u, &got));
    TEST_ASSERT_EQUAL_UINT32(5u, got.tick);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, got.walk_x);

    TEST_ASSERT_TRUE(jce_input_command_get_latest(2u, &got));
    TEST_ASSERT_EQUAL_UINT32(8u, got.tick);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, got.walk_z);
    TEST_ASSERT_EQUAL_UINT8(1u, got.sprint);

    /* Updating client 1 does not touch client 2. */
    JceInputCommand a2 = make_cmd(6u, 2.0f, 0.0f, 1.0f, 0u, 0u);
    jce_input_command_store(1u, &a2);
    TEST_ASSERT_TRUE(jce_input_command_get_latest(2u, &got));
    TEST_ASSERT_EQUAL_UINT32(8u, got.tick);   /* client 2 unchanged */
}

static void test_store_bounded(void)
{
    JceInputCommand got;

    /* Fill every slot with a distinct client id. */
    for (uint32_t i = 0; i < JCE_INPUT_COMMAND_MAX_CLIENTS; ++i) {
        JceInputCommand c = make_cmd(100u + i, (float)i, 0.0f, 1.0f, 0u, 0u);
        jce_input_command_store(i + 1u, &c);   /* ids 1..MAX */
    }
    /* All present. */
    for (uint32_t i = 0; i < JCE_INPUT_COMMAND_MAX_CLIENTS; ++i)
        TEST_ASSERT_TRUE(jce_input_command_get_latest(i + 1u, &got));

    /* A brand-new client id when full is dropped (existing clients keep
     * updating, but the new id is not tracked). */
    JceInputCommand overflow = make_cmd(999u, 1.0f, 1.0f, 1.0f, 1u, 1u);
    jce_input_command_store(9999u, &overflow);
    TEST_ASSERT_FALSE(jce_input_command_get_latest(9999u, &got));

    /* An existing client still updates while full. */
    JceInputCommand upd = make_cmd(500u, 3.0f, 0.0f, 1.0f, 0u, 0u);
    jce_input_command_store(1u, &upd);
    TEST_ASSERT_TRUE(jce_input_command_get_latest(1u, &got));
    TEST_ASSERT_EQUAL_UINT32(500u, got.tick);
}

/* ================================================================== */
/* 3. Server-receive handler seam (decode + store), no live socket     */
/* ================================================================== */

static void test_server_receive_handler(void)
{
    JceInputCommand orig = make_cmd(77u, -0.75f, 0.5f, 0.5f, /*jump=*/1u,
                                    /*sprint=*/1u);

    /* Encode the command, then feed the exact bytes + a fake sender id to the
     * SAME seam the live RPC handler forwards to. */
    uint8_t  buf[JCE_INPUT_COMMAND_WIRE_SIZE];
    uint32_t n = jce_input_command_encode(&orig, buf, sizeof buf);
    TEST_ASSERT_EQUAL_UINT32(JCE_INPUT_COMMAND_WIRE_SIZE, n);

    const uint32_t fake_sender = 3u;
    TEST_ASSERT_TRUE(jce_input_command_server_receive(fake_sender, buf, n));

    JceInputCommand got;
    TEST_ASSERT_TRUE(jce_input_command_get_latest(fake_sender, &got));
    TEST_ASSERT_EQUAL_UINT32(orig.tick,   got.tick);
    TEST_ASSERT_EQUAL_FLOAT (orig.walk_x, got.walk_x);
    TEST_ASSERT_EQUAL_FLOAT (orig.walk_z, got.walk_z);
    TEST_ASSERT_EQUAL_FLOAT (orig.speed_mult, got.speed_mult);
    TEST_ASSERT_EQUAL_UINT8 (orig.jump,   got.jump);
    TEST_ASSERT_EQUAL_UINT8 (orig.sprint, got.sprint);

    /* Wrong-size bytes are rejected by the receive seam (no store). */
    TEST_ASSERT_FALSE(
        jce_input_command_server_receive(4u, buf,
                                         JCE_INPUT_COMMAND_WIRE_SIZE - 1u));
    TEST_ASSERT_FALSE(jce_input_command_get_latest(4u, &got));

    /* The receive seam respects latest-wins through the store: an older tick
     * from the same sender does not overwrite. */
    JceInputCommand older = make_cmd(50u, 9.0f, 9.0f, 1.0f, 0u, 0u);
    TEST_ASSERT_EQUAL_UINT32(JCE_INPUT_COMMAND_WIRE_SIZE,
                             jce_input_command_encode(&older, buf, sizeof buf));
    TEST_ASSERT_TRUE(jce_input_command_server_receive(fake_sender, buf,
                                                      sizeof buf));
    TEST_ASSERT_TRUE(jce_input_command_get_latest(fake_sender, &got));
    TEST_ASSERT_EQUAL_UINT32(77u, got.tick);   /* still the newer one */
}

/* ================================================================== */
/* runner                                                             */
/* ================================================================== */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_codec_round_trip);
    RUN_TEST(test_codec_rejects_bad_size);
    RUN_TEST(test_store_latest_wins);
    RUN_TEST(test_store_clients_independent);
    RUN_TEST(test_store_bounded);
    RUN_TEST(test_server_receive_handler);
    return UNITY_END();
}
