/* test_jce_net_enet_lifecycle.c
 *
 * Characterization tests for REF-005 (dedup audit): ENet subsystem ownership.
 *
 * Before REF-005 two modules initialised ENet independently and shutdown was
 * asymmetric:
 *   jce_net.c          g_enet_initialised (one-shot) + atexit(enet_shutdown)
 *   jce_lan_discovery  g_lan_enet_init    (one-shot) + NO shutdown at all
 * Either module could bring ENet up; only one could ever tear it down, and it
 * did so process-globally regardless of the other's state.
 *
 * REF-005 replaced both with a single refcount owned by jce_net.c
 * (jce__net_enet_acquire / jce__net_enet_release): N acquires require N
 * releases and only the last release calls enet_deinitialize().
 *
 * The failure mode a refcount introduces is UNDER-counting — deinitialising
 * while another user is still live, or failing to re-initialise after the
 * count legitimately reaches zero.  These tests exercise exactly that:
 * interleaved lifetimes across BOTH modules, and reuse after a full teardown.
 *
 * Everything here goes through the public API only; no test hooks.
 */

#include <jce/middleware/net/jce_lan_discovery.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/middleware/net/jce_net.h>

#include "unity.h"

#include <string.h>

/* A discovery port unlikely to collide with a real service on a dev box. */
#define TEST_DISCOVERY_PORT 45999
#define TEST_GAME_PORT      45998

void setUp(void)    {}
void tearDown(void)
{
    /* Never leave the subsystem running if an assertion aborted a test. */
    if (jce_lan_discovery_server_is_running())
        jce_lan_discovery_server_stop();
    if (jce_lan_discovery_client_is_scanning())
        jce_lan_discovery_client_stop_scan();
}

static JceNetHost *make_client_host(void)
{
    JceNetHostDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.port          = 0;   /* client / ephemeral */
    desc.max_peers     = 4;
    desc.channel_count = 2;
    return jce_net_host_create(&desc, jce_allocator_default());
}

/* A single balanced create/destroy cycle must work, repeatedly.  With the old
 * one-shot flag the first cycle worked by accident; a refcount that
 * over-releases would break the SECOND cycle. */
static void test_repeated_host_cycles(void)
{
    for (int i = 0; i < 5; ++i) {
        JceNetHost *h = make_client_host();
        TEST_ASSERT_NOT_NULL_MESSAGE(h, "host create failed on a later cycle "
                                        "(ENet refcount likely over-released)");
        jce_net_host_destroy(h);
    }
}

/* THE regression this file exists for: two independent modules hold ENet at
 * the same time, and one of them goes away first.  The survivor must keep
 * working — the last release, not the first, may deinitialize. */
static void test_interleaved_host_and_lan_discovery(void)
{
    JceNetHost *h = make_client_host();
    TEST_ASSERT_NOT_NULL(h);

    const bool started = jce_lan_discovery_server_start(
        "jce-test", TEST_GAME_PORT, 0, 8, TEST_DISCOVERY_PORT);
    if (!started) {
        /* Port busy / sandboxed network: nothing to assert about interleaving,
         * but the host must still tear down cleanly. */
        jce_net_host_destroy(h);
        TEST_IGNORE_MESSAGE("LAN discovery could not bind; skipping interleave");
        return;
    }
    TEST_ASSERT_TRUE(jce_lan_discovery_server_is_running());

    /* Drop the NET host first.  ENet must stay up for the LAN server. */
    jce_net_host_destroy(h);
    TEST_ASSERT_TRUE_MESSAGE(jce_lan_discovery_server_is_running(),
        "destroying the net host tore down ENet under the LAN server");

    /* The survivor must still function, not just report running. */
    jce_lan_discovery_server_tick();
    jce_lan_discovery_server_set_player_count(3);
    jce_lan_discovery_server_tick();

    /* A brand-new host must be creatable while the LAN server still holds a
     * reference (acquire on a non-zero count must not re-init incorrectly). */
    JceNetHost *h2 = make_client_host();
    TEST_ASSERT_NOT_NULL_MESSAGE(h2, "second host create failed while LAN "
                                     "discovery still held ENet");
    jce_net_host_destroy(h2);

    jce_lan_discovery_server_stop();
    TEST_ASSERT_FALSE(jce_lan_discovery_server_is_running());
}

/* After the refcount legitimately reaches zero and ENet is deinitialised, a
 * later acquire must bring it back up.  A one-shot "already initialised" flag
 * fails exactly here. */
static void test_reuse_after_full_teardown(void)
{
    JceNetHost *h = make_client_host();
    TEST_ASSERT_NOT_NULL(h);
    jce_net_host_destroy(h);          /* refcount -> 0, ENet deinitialised */

    JceNetHost *again = make_client_host();
    TEST_ASSERT_NOT_NULL_MESSAGE(again,
        "ENet did not come back up after the refcount reached zero");
    jce_net_host_destroy(again);
}

/* The LAN client scanner is the third acquire site; it must balance too. */
static void test_lan_client_scan_balances(void)
{
    for (int i = 0; i < 3; ++i) {
        if (!jce_lan_discovery_client_start_scan(TEST_DISCOVERY_PORT, 0)) {
            TEST_IGNORE_MESSAGE("LAN client scan could not start; skipping");
            return;
        }
        TEST_ASSERT_TRUE(jce_lan_discovery_client_is_scanning());
        jce_lan_discovery_client_tick();
        jce_lan_discovery_client_stop_scan();
        TEST_ASSERT_FALSE(jce_lan_discovery_client_is_scanning());
    }

    /* And ENet must still be usable afterwards. */
    JceNetHost *h = make_client_host();
    TEST_ASSERT_NOT_NULL_MESSAGE(h, "ENet unusable after LAN scan cycles");
    jce_net_host_destroy(h);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_repeated_host_cycles);
    RUN_TEST(test_interleaved_host_and_lan_discovery);
    RUN_TEST(test_reuse_after_full_teardown);
    RUN_TEST(test_lan_client_scan_balances);
    return UNITY_END();
}
