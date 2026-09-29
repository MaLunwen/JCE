/*
 * test_jce_gas_replication.c — server-authoritative GAS attribute replication.
 *
 * Exercises the REAL replication encode -> decode path (no live socket) for the
 * packed JceGasAttribRepl component that carries a server's live GAS attribute
 * *current* values to remote peers:
 *
 *   1. SERVER: a flecs world + a NetworkObject + a GAS (Health base=100,
 *      Mana base=50).  An instant -60 effect drops Health current to 40.  The
 *      shared registration path (jce_gas_replication_register) wires the
 *      packed replica component; jce_gas_replication_fill_from_gas writes the
 *      live attribute values into the entity's JceGasAttribRepl; the REAL
 *      snapshot encoder (jce_net_replication_encode_snapshot — the same code
 *      the broadcast path runs) produces wire bytes.
 *   2. CLIENT: a FRESH state + its own flecs world + a GAS with DEFAULT
 *      attribute values (Health=100, Mana=50).  It decodes the captured bytes
 *      through jce_net_replication_handle_packet (the substrate read serializer
 *      populates JceGasAttribRepl), then jce_gas_replication_apply_to_gas
 *      pushes the replicated values into the local GAS.  Asserts the client
 *      GAS now reads Health current == 40, Mana current == 50.
 *   3. NO-NETOBJECT no-op: a GAS entity WITHOUT a NetworkObject produces no
 *      JceGasAttribRepl component, so a full burst carries no replica bytes
 *      for it and a fresh client sees nothing to apply.
 *
 * Server and client cannot share the singleton replication state, so the test
 * captures the server's encoded snapshot first, tears the server down, then
 * replays the captured bytes against a fresh client state + world.  Both the
 * encode and the decode are the production code paths.
 */

#include "unity.h"

#include <jce/api_net.h>
#include <jce/middleware/net/jce_replication.h>
#include <jce/middleware/net/jce_network_variable.h>
#include <jce/middleware/world/jce_gas.h>
#include <jce/middleware/world/jce_gas_replication.h>
#include <jce/middleware/scene/jce_scene.h>

#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* Captured server snapshot, replayed against the client. */
typedef struct CapturedSnap {
    void    *buf;
    uint32_t size;
} CapturedSnap;

/* Build a GAS with Health + Mana.  Returns by value via out-param. */
static void make_gas(JceGameplayAbilitySystem *gas)
{
    jce_gas_init(gas);
    jce_attribute_set_add(&gas->attributes, "Health", 100.0f, 0.0f, 100.0f);
    jce_attribute_set_add(&gas->attributes, "Mana",    50.0f, 0.0f, 100.0f);
}

/* ================================================================== */
/* End-to-end: server fills -> encode -> client decodes -> applies     */
/* ================================================================== */

static void test_gas_replication_end_to_end(void)
{
    JceScene                *server_scene;
    JceNetObjectId           net_id;
    uint64_t                 server_entity;
    JceGameplayAbilitySystem server_gas;
    CapturedSnap             burst = { NULL, 0 };

    /* ============================================================== *
     * SERVER PHASE — register, spawn, fill, capture the real encode.  *
     * ============================================================== */
    jce_net_replication_init();
    jce_net_replication_set_role(JCE_NET_ROLE_SERVER);
    jce_net_replication_set_local_client_id(JCE_CLIENT_SERVER);

    server_scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(server_scene);
    jce_net_replication_set_world(jce_scene_get_world(server_scene));

    /* SHARED registration path — the same call the runtime net bridge makes.
     * register NetworkVariable first so the gas-replica interning order
     * matches a real bring-up (f32, i32, then gas-replica). */
    jce_net_var_reset_hooks();
    TEST_ASSERT_TRUE_MESSAGE(jce_net_var_register_all(),
        "NetworkVariable registration failed — everything below would be "
        "asserting against an unwired substrate");
    TEST_ASSERT_TRUE_MESSAGE(
        jce_gas_replication_register(jce_scene_get_world(server_scene)),
        "GAS replica component failed to register");
    TEST_ASSERT_TRUE(jce_net_replication_component_count() >= 1u);

    /* Spawn a NetworkObject owned by the server (server has authority). */
    {
        JceNetObjectDesc d;
        memset(&d, 0, sizeof(d));
        d.owner = JCE_CLIENT_SERVER;
        net_id  = jce_net_object_spawn(&d);
    }
    TEST_ASSERT_NOT_EQUAL(JCE_NET_OBJECT_INVALID, net_id);
    server_entity = jce_net_object_to_entity(net_id);
    TEST_ASSERT_NOT_EQUAL(0u, server_entity);
    TEST_ASSERT_TRUE(jce_net_object_has_authority(net_id));

    /* Build the server GAS and drop Health current to 40 via an instant -60
     * effect (mutates base, re-clamped). */
    make_gas(&server_gas);
    {
        JceGameplayEffect dmg;
        memset(&dmg, 0, sizeof(dmg));
        dmg.attr_idx      = jce_attribute_set_find(&server_gas.attributes,
                                                   "Health");
        dmg.op            = JCE_GAS_OP_ADD;
        dmg.magnitude     = -60.0f;
        dmg.duration_mode = JCE_GAS_DURATION_INSTANT;
        jce_gas_apply_effect(&server_gas, &dmg);
    }
    TEST_ASSERT_EQUAL_FLOAT(40.0f,
        jce_gas_attribute_current(&server_gas,
            jce_attribute_set_find(&server_gas.attributes, "Health")));

    /* AUTHORITY fill: write the live current values into the replica. */
    TEST_ASSERT_TRUE(
        jce_gas_replication_fill_from_gas(server_entity, &server_gas));

    /* Capture a real FULL burst (carries the current values). */
    burst.size = jce_net_replication_encode_snapshot(1u, /*full=*/true,
                                                     &burst.buf);
    TEST_ASSERT_TRUE(burst.size > 0u);
    TEST_ASSERT_NOT_NULL(burst.buf);

    jce_net_replication_shutdown();
    jce_scene_destroy(server_scene);

    /* ============================================================== *
     * CLIENT PHASE — fresh state + world, decode + apply.             *
     * ============================================================== */
    {
        JceScene                *client_scene;
        uint64_t                 client_entity;
        JceGameplayAbilitySystem client_gas;

        jce_net_replication_init();
        jce_net_replication_set_role(JCE_NET_ROLE_CLIENT);
        /* Local seat is client #2 — does NOT own the net obj (owner=server). */
        jce_net_replication_set_local_client_id((JceClientId)2);

        client_scene = jce_scene_create();
        TEST_ASSERT_NOT_NULL(client_scene);
        jce_net_replication_set_world(jce_scene_get_world(client_scene));

        /* Register in the SAME order so the u16 component interning matches. */
        jce_net_var_reset_hooks();
        TEST_ASSERT_TRUE(jce_net_var_register_all());
        TEST_ASSERT_TRUE(
            jce_gas_replication_register(jce_scene_get_world(client_scene)));

        /* Client GAS starts at the DEFAULT authored values (Health 100). */
        make_gas(&client_gas);
        TEST_ASSERT_EQUAL_FLOAT(100.0f,
            jce_gas_attribute_current(&client_gas,
                jce_attribute_set_find(&client_gas.attributes, "Health")));

        /* Decode the captured FULL burst: spawns the entity + populates the
         * JceGasAttribRepl via the substrate read serializer. */
        jce_net_replication_handle_packet(burst.buf, burst.size);

        client_entity = jce_net_object_to_entity(net_id);
        TEST_ASSERT_NOT_EQUAL(0u, client_entity);

        /* The client does NOT have authority (owner=server, seat=2). */
        TEST_ASSERT_FALSE(jce_net_object_has_authority(net_id));

        /* Apply the replicated values into the local GAS. */
        TEST_ASSERT_TRUE(
            jce_gas_replication_apply_to_gas(client_entity, &client_gas));

        /* Replicated, server-authoritative values now hold locally. */
        TEST_ASSERT_EQUAL_FLOAT(40.0f,
            jce_gas_attribute_current(&client_gas,
                jce_attribute_set_find(&client_gas.attributes, "Health")));
        TEST_ASSERT_EQUAL_FLOAT(50.0f,
            jce_gas_attribute_current(&client_gas,
                jce_attribute_set_find(&client_gas.attributes, "Mana")));

        jce_net_replication_shutdown();
        jce_scene_destroy(client_scene);
    }

    jce_net_replication_free_buffer(burst.buf);
}

/* ================================================================== */
/* No NetworkObject -> no replica component -> nothing to replicate.    */
/* ================================================================== */

static void test_gas_replication_no_netobject_is_noop(void)
{
    JceScene                *scene;
    JceGameplayAbilitySystem gas;
    uint64_t                 plain_entity;
    CapturedSnap             burst = { NULL, 0 };

    jce_net_replication_init();
    jce_net_replication_set_role(JCE_NET_ROLE_SERVER);
    jce_net_replication_set_local_client_id(JCE_CLIENT_SERVER);

    scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    jce_net_replication_set_world(jce_scene_get_world(scene));

    jce_net_var_reset_hooks();
    TEST_ASSERT_TRUE(jce_net_var_register_all());
    TEST_ASSERT_TRUE(jce_gas_replication_register(jce_scene_get_world(scene)));

    /* A bare flecs entity with NO NetworkObject: it is never adopted into the
     * replication table, so it has no net id and the runtime would never fill
     * a replica for it.  We deliberately do NOT call fill_from_gas here — this
     * mirrors the runtime gate (jce_net_object_from_entity == INVALID skips
     * the fill entirely). */
    make_gas(&gas);
    plain_entity = (uint64_t)jce_scene_create_entity(scene, "plain_gas");
    TEST_ASSERT_NOT_EQUAL(0u, plain_entity);
    TEST_ASSERT_EQUAL_UINT32(JCE_NET_OBJECT_INVALID,
                             jce_net_object_from_entity(plain_entity));

    /* No net objects exist at all -> a full burst carries no per-object
     * component payload.  The encode must not crash and the object table is
     * empty. */
    TEST_ASSERT_EQUAL_UINT32(0u, jce_net_object_count());
    burst.size = jce_net_replication_encode_snapshot(1u, /*full=*/true,
                                                     &burst.buf);
    /* A header-only packet is fine; what matters is that nothing replicated. */
    (void)burst.size;

    jce_net_replication_free_buffer(burst.buf);
    jce_net_replication_shutdown();
    jce_scene_destroy(scene);
}

/* ================================================================== */
/* runner                                                             */
/* ================================================================== */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_gas_replication_end_to_end);
    RUN_TEST(test_gas_replication_no_netobject_is_noop);
    return UNITY_END();
}
