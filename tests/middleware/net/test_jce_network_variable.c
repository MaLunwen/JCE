/*
 * test_jce_network_variable.c — FEATURE 7.2 typed property replication.
 *
 * Exercises the REAL replication encode -> decode delta path (no mocks):
 *
 *   1. The server registers the built-in NetworkVariable components, spawns
 *      a NetworkObject, and mutates a typed value on the authority.
 *   2. The REAL snapshot encoder (jce_net_replication_encode_snapshot —
 *      the same code the broadcast path runs) produces wire bytes.
 *   3. A FRESH client replication state + its own flecs world (via JceScene)
 *      decodes those bytes through jce_net_replication_handle_packet and the
 *      remote copy ends up with the new value.
 *   4. The OnValueChanged hook fires exactly once on an actual change and
 *      NOT when an identical snapshot re-arrives.
 *   5. A non-authority write is rejected.
 *   6. A late-joiner FULL snapshot carries the CURRENT value.
 *
 * Server and client cannot share the singleton replication state, so the
 * test captures the server's encoded snapshots first, tears the server
 * down, then replays the captured bytes against a fresh client state +
 * world.  Both the encode and the decode are the production code paths.
 */

#include "unity.h"

#include <jce/api_net.h>
#include <jce/middleware/net/jce_network_variable.h>
#include <jce/middleware/net/jce_replication.h>
#include <jce/middleware/scene/jce_scene.h>

#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* OnValueChanged capture                                              */
/* ------------------------------------------------------------------ */

static int      g_changed_count;
static uint64_t g_changed_entity;
static float    g_changed_old;
static float    g_changed_new;

static void on_f32_changed(uint64_t entity, float old_value, float new_value,
                           void *user)
{
    (void)user;
    ++g_changed_count;
    g_changed_entity = entity;
    g_changed_old    = old_value;
    g_changed_new    = new_value;
}

/* Captured server snapshots, replayed against the client. */
typedef struct CapturedSnap {
    void    *buf;
    uint32_t size;
} CapturedSnap;

/* ================================================================== */
/* The single end-to-end scenario                                      */
/* ================================================================== */

static void test_network_variable_end_to_end(void)
{
    JceScene      *server_scene;
    JceNetObjectId net_id;
    uint64_t       server_entity;

    CapturedSnap full_initial = { NULL, 0 };  /* late-joiner burst, value=10 */
    CapturedSnap delta_change = { NULL, 0 };  /* delta 10 -> 20             */
    CapturedSnap delta_nop    = { NULL, 0 };  /* no change -> empty of comp */
    CapturedSnap full_current = { NULL, 0 };  /* burst with CURRENT value   */

    /* ============================================================== *
     * SERVER PHASE — register, spawn, mutate, capture real encodes.  *
     * ============================================================== */
    jce_net_replication_init();
    jce_net_replication_set_role(JCE_NET_ROLE_SERVER);
    jce_net_replication_set_local_client_id(JCE_CLIENT_SERVER);

    server_scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(server_scene);
    jce_net_replication_set_world(jce_scene_get_world(server_scene));
    jce_net_var_reset_hooks();

    /* PRODUCTION registration path: real components on the bound world. */
    jce_net_var_register_all();
    TEST_ASSERT_TRUE(jce_net_replication_component_count() >= 1u);

    /* Spawn a NetworkObject owned by client #1 (not the server seat). */
    {
        JceNetObjectDesc d;
        memset(&d, 0, sizeof(d));
        d.owner = (JceClientId)1;
        net_id  = jce_net_object_spawn(&d);
    }
    TEST_ASSERT_NOT_EQUAL(JCE_NET_OBJECT_INVALID, net_id);
    server_entity = jce_net_object_to_entity(net_id);
    TEST_ASSERT_NOT_EQUAL(0u, server_entity);

    /* Authority: the server has authority over every object, so the write
     * is accepted and stored. */
    TEST_ASSERT_TRUE(jce_net_var_f32_set(server_entity, 10.0f));
    TEST_ASSERT_EQUAL_FLOAT(10.0f, jce_net_var_f32_get(server_entity, -1.0f));

    /* (1) Late-joiner FULL burst — must carry the CURRENT value (10). */
    full_initial.size =
        jce_net_replication_encode_snapshot(1u, /*full=*/true, &full_initial.buf);
    TEST_ASSERT_TRUE(full_initial.size > 0u);
    TEST_ASSERT_NOT_NULL(full_initial.buf);

    /* Prime the delta baseline with the value the burst already carried so
     * the first delta reflects only the NEXT change (mirrors a real server:
     * the burst and the steady delta share one authoritative baseline). */
    {
        void *prime = NULL;
        uint32_t n = jce_net_replication_encode_snapshot(2u, /*full=*/false,
                                                         &prime);
        jce_net_replication_free_buffer(prime);
        (void)n;
    }

    /* Mutate on the authority: 10 -> 20. */
    TEST_ASSERT_TRUE(jce_net_var_f32_set(server_entity, 20.0f));
    TEST_ASSERT_EQUAL_FLOAT(20.0f, jce_net_var_f32_get(server_entity, -1.0f));

    /* (2) Delta snapshot carrying the change. */
    delta_change.size =
        jce_net_replication_encode_snapshot(3u, /*full=*/false, &delta_change.buf);
    TEST_ASSERT_TRUE(delta_change.size > 0u);

    /* Identical re-write: accepted (has authority) but value unchanged. */
    TEST_ASSERT_TRUE(jce_net_var_f32_set(server_entity, 20.0f));

    /* (3) Delta with NO change — the substrate skips the unchanged comp,
     * so this delta carries zero JceNetVarF32 entries. */
    delta_nop.size =
        jce_net_replication_encode_snapshot(4u, /*full=*/false, &delta_nop.buf);
    /* (may still be a valid header-only packet; size > 0 is fine) */

    /* (6) Another FULL burst now reflects the CURRENT value (20). */
    full_current.size =
        jce_net_replication_encode_snapshot(5u, /*full=*/true, &full_current.buf);
    TEST_ASSERT_TRUE(full_current.size > 0u);

    /* Tear the server down (frees its registry + object table). */
    jce_net_replication_shutdown();
    jce_scene_destroy(server_scene);

    /* ============================================================== *
     * CLIENT PHASE — fresh state + world, decode the captured bytes. *
     * ============================================================== */
    {
        JceScene      *client_scene;
        uint64_t       client_entity;
        JceNetObjectId client_net_id;

        jce_net_replication_init();
        jce_net_replication_set_role(JCE_NET_ROLE_CLIENT);
        /* Local seat is client #2 — does NOT own net obj (owner=1). */
        jce_net_replication_set_local_client_id((JceClientId)2);

        client_scene = jce_scene_create();
        TEST_ASSERT_NOT_NULL(client_scene);
        jce_net_replication_set_world(jce_scene_get_world(client_scene));
        jce_net_var_reset_hooks();

        /* Register in the SAME order so the u16 component interning matches. */
        jce_net_var_register_all();

        g_changed_count = 0;

        /* --- (6) Late-joiner FULL burst carries the current value. The
         *     burst we apply first is full_initial (value 10): it must
         *     spawn the object and seed value 10 on the client. --- */
        jce_net_replication_handle_packet(full_initial.buf, full_initial.size);

        client_net_id = net_id;  /* server-authoritative id is preserved */
        client_entity = jce_net_object_to_entity(client_net_id);
        TEST_ASSERT_NOT_EQUAL(0u, client_entity);
        TEST_ASSERT_EQUAL_FLOAT(10.0f,
                                jce_net_var_f32_get(client_entity, -1.0f));

        /* Register the OnValueChanged hook now that the entity exists. */
        jce_net_var_f32_on_changed(client_entity, on_f32_changed, NULL);

        /* --- (3) Apply the delta carrying 10 -> 20: remote receives the
         *     new value and the hook fires EXACTLY once. --- */
        jce_net_replication_handle_packet(delta_change.buf, delta_change.size);
        TEST_ASSERT_EQUAL_FLOAT(20.0f,
                                jce_net_var_f32_get(client_entity, -1.0f));
        TEST_ASSERT_EQUAL_INT(1, g_changed_count);
        TEST_ASSERT_EQUAL_UINT64(client_entity, g_changed_entity);
        TEST_ASSERT_EQUAL_FLOAT(10.0f, g_changed_old);
        TEST_ASSERT_EQUAL_FLOAT(20.0f, g_changed_new);

        /* --- (4) Re-apply the SAME delta: value is identical, so the hook
         *     must NOT fire again. --- */
        jce_net_replication_handle_packet(delta_change.buf, delta_change.size);
        TEST_ASSERT_EQUAL_FLOAT(20.0f,
                                jce_net_var_f32_get(client_entity, -1.0f));
        TEST_ASSERT_EQUAL_INT(1, g_changed_count);  /* still exactly once */

        /* --- The no-change delta must carry no JceNetVarF32 entry, so
         *     applying it leaves the value + hook count untouched. --- */
        if (delta_nop.size > 0u)
            jce_net_replication_handle_packet(delta_nop.buf, delta_nop.size);
        TEST_ASSERT_EQUAL_FLOAT(20.0f,
                                jce_net_var_f32_get(client_entity, -1.0f));
        TEST_ASSERT_EQUAL_INT(1, g_changed_count);

        /* --- (5) Non-authority write is rejected on the client (it does
         *     not own the object; owner=1, local seat=2). Value unchanged. */
        TEST_ASSERT_FALSE(jce_net_object_has_authority(client_net_id));
        TEST_ASSERT_FALSE(jce_net_var_f32_set(client_entity, 999.0f));
        TEST_ASSERT_EQUAL_FLOAT(20.0f,
                                jce_net_var_f32_get(client_entity, -1.0f));

        jce_net_replication_shutdown();
        jce_scene_destroy(client_scene);
    }

    /* ============================================================== *
     * Bonus: a FULL burst captured AFTER the change carries value 20 *
     * to a brand-new late joiner.                                     *
     * ============================================================== */
    {
        JceScene *joiner_scene;
        uint64_t  joiner_entity;

        jce_net_replication_init();
        jce_net_replication_set_role(JCE_NET_ROLE_CLIENT);
        jce_net_replication_set_local_client_id((JceClientId)3);

        joiner_scene = jce_scene_create();
        TEST_ASSERT_NOT_NULL(joiner_scene);
        jce_net_replication_set_world(jce_scene_get_world(joiner_scene));
        jce_net_var_reset_hooks();
        jce_net_var_register_all();

        jce_net_replication_handle_packet(full_current.buf, full_current.size);
        joiner_entity = jce_net_object_to_entity(net_id);
        TEST_ASSERT_NOT_EQUAL(0u, joiner_entity);
        /* The late-joiner FULL snapshot carried the CURRENT value (20). */
        TEST_ASSERT_EQUAL_FLOAT(20.0f,
                                jce_net_var_f32_get(joiner_entity, -1.0f));

        jce_net_replication_shutdown();
        jce_scene_destroy(joiner_scene);
    }

    jce_net_replication_free_buffer(full_initial.buf);
    jce_net_replication_free_buffer(delta_change.buf);
    jce_net_replication_free_buffer(delta_nop.buf);
    jce_net_replication_free_buffer(full_current.buf);
}

/* ================================================================== */
/* int32 variant — value travels + authority gating                    */
/* ================================================================== */

static void test_network_variable_i32_round_trip(void)
{
    JceScene      *server_scene;
    JceNetObjectId net_id;
    uint64_t       server_entity;
    CapturedSnap   burst = { NULL, 0 };

    jce_net_replication_init();
    jce_net_replication_set_role(JCE_NET_ROLE_SERVER);
    jce_net_replication_set_local_client_id(JCE_CLIENT_SERVER);

    server_scene = jce_scene_create();
    jce_net_replication_set_world(jce_scene_get_world(server_scene));
    jce_net_var_reset_hooks();
    jce_net_var_register_all();

    {
        JceNetObjectDesc d;
        memset(&d, 0, sizeof(d));
        d.owner = JCE_CLIENT_SERVER;
        net_id  = jce_net_object_spawn(&d);
    }
    server_entity = jce_net_object_to_entity(net_id);

    TEST_ASSERT_TRUE(jce_net_var_i32_set(server_entity, -1234));
    TEST_ASSERT_EQUAL_INT32(-1234, jce_net_var_i32_get(server_entity, 0));

    burst.size = jce_net_replication_encode_snapshot(1u, true, &burst.buf);
    TEST_ASSERT_TRUE(burst.size > 0u);

    jce_net_replication_shutdown();
    jce_scene_destroy(server_scene);

    {
        JceScene *client_scene;
        uint64_t  client_entity;

        jce_net_replication_init();
        jce_net_replication_set_role(JCE_NET_ROLE_CLIENT);
        jce_net_replication_set_local_client_id((JceClientId)7);
        client_scene = jce_scene_create();
        jce_net_replication_set_world(jce_scene_get_world(client_scene));
        jce_net_var_reset_hooks();
        jce_net_var_register_all();

        jce_net_replication_handle_packet(burst.buf, burst.size);
        client_entity = jce_net_object_to_entity(net_id);
        TEST_ASSERT_NOT_EQUAL(0u, client_entity);
        TEST_ASSERT_EQUAL_INT32(-1234,
                                jce_net_var_i32_get(client_entity, 0));

        jce_net_replication_shutdown();
        jce_scene_destroy(client_scene);
    }

    jce_net_replication_free_buffer(burst.buf);
}

/* ================================================================== */
/* runner                                                             */
/* ================================================================== */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_network_variable_end_to_end);
    RUN_TEST(test_network_variable_i32_round_trip);
    return UNITY_END();
}
