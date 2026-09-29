/*
 * test_jce_net_despawn_reap.c
 *
 * Regression guard for the despawn leak fixed 2026-08-31.
 *
 * jce_net_object_despawn() does the local teardown immediately and keeps the
 * table entry so the next outbound packet can name the id; the entry is meant
 * to be reaped by the following jce_net_replication_tick().  That sweep lived
 * inside `if (role == SERVER && g_repl.host)`, so with no host -- single
 * player, or any time before listen() -- it never ran.  Every despawn then
 * consumed an object-table slot permanently and jce_net_object_count() never
 * came back down.
 *
 * jce_net_replication_self_test() asserts exactly this and had never been
 * called by anything.  It also lives behind `#ifndef NDEBUG` and reports
 * through assert(), so it cannot guard the Release build where the leak
 * actually bites.  This test uses only the public header and runs in every
 * configuration.
 */

#include <jce/middleware/net/jce_replication.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_despawn_is_reaped_with_no_host(void)
{
    jce_net_replication_init();
    jce_net_replication_set_role(JCE_NET_ROLE_SERVER);   /* no host started */

    TEST_ASSERT_EQUAL_UINT32(0u, jce_net_object_count());

    JceNetObjectDesc d;
    memset(&d, 0, sizeof(d));
    d.prefab_path = "test/despawn_reap";
    d.owner       = JCE_CLIENT_SERVER;

    const JceNetObjectId id = jce_net_object_spawn(&d);
    TEST_ASSERT_EQUAL_UINT32(1u, jce_net_object_count());

    jce_net_object_despawn(id);
    jce_net_replication_tick(0u);

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, jce_net_object_count(),
        "a despawned object was not reaped: with no host the sweep never ran, "
        "so the table slot is held for the life of the process");

    /* Repeat: a leak shows up as monotonic growth, and one round trip alone
     * would not distinguish "reaped" from "never counted". */
    for (int i = 0; i < 8; ++i) {
        const JceNetObjectId n = jce_net_object_spawn(&d);
        jce_net_object_despawn(n);
        jce_net_replication_tick((uint32_t)(i + 1));
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, jce_net_object_count(),
        "object count grew across eight spawn/despawn cycles");

    jce_net_replication_shutdown();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_despawn_is_reaped_with_no_host);
    return UNITY_END();
}
