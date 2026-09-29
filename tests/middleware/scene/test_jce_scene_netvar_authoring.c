/*
 * test_jce_scene_netvar_authoring.c
 *
 * FEATURE 7.2 last-mile — NETWORK VARIABLE authoring as a scene component.
 *
 * Typed NetworkVariables already exist as ENGINE flecs components
 * (JceNetVarF32/I32) replicated on the snapshot substrate (authority gate,
 * OnValueChanged, delta/baseline/late-joiner) — unit-tested end-to-end in
 * tests/middleware/net/test_jce_network_variable.c.  THIS test closes the
 * AUTHORING gap:
 *
 *   1. The new presence-gated JceNetworkVariable scene component round-trips
 *      field-for-field through the REAL component JSON serializer
 *      (jce_scene_save_json -> jce_scene_load_json): variable name, the type
 *      enum (F32/I32/Bool), the authority enum (Server/Client/Owner) and the
 *      initial value.
 *   2. An entity with NO component reloads with none (byte-identical default).
 *   3. The runtime registration path the bridge uses — bind the world,
 *      jce_net_var_register_all(), then seed the authored typed value via the
 *      PUBLIC typed-set API — actually attaches the matching replicated NetVar
 *      and the value reads back.  This is the exact public seam rt_spawn_net
 *      drives at Play; the renderer/editor sliders are F12-only.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/middleware/net/jce_network_variable.h>
#include <jce/middleware/net/jce_replication.h>
#include <jce/os/core/jce_json.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

typedef struct {
    const char *want;
    JceEntity   found;
} FindCtx;

static void find_cb(JceScene *s, JceEntity e, void *ud)
{
    FindCtx *ctx = (FindCtx *)ud;
    const char *nm = jce_scene_entity_registered_name(s, e);
    if (nm && strcmp(nm, ctx->want) == 0)
        ctx->found = e;
}

static JceEntity find_by_name(JceScene *s, const char *name)
{
    FindCtx ctx = { name, JCE_ENTITY_INVALID };
    jce_scene_each_entity(s, find_cb, &ctx);
    return ctx.found;
}

/* ── NetworkVariable component round-trip (F32 / Owner) ─────────────── */

static void test_netvar_round_trip_f32(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    JceEntity e = jce_scene_create_entity(src, "Health");

    JceNetworkVariableComponent nv;
    memset(&nv, 0, sizeof nv);
    snprintf(nv.var_name, sizeof nv.var_name, "%s", "health");
    nv.var_type      = JCE_NETVAR_AUTHOR_TYPE_F32;
    nv.authority     = JCE_NETVAR_AUTHOR_AUTH_OWNER;
    nv.initial_value = 87.5f;
    jce_scene_set_network_variable(src, e, &nv);

    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Health");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    TEST_ASSERT_TRUE(jce_scene_has_network_variable(dst, ne));
    JceNetworkVariableComponent *out = jce_scene_get_network_variable(dst, ne);
    TEST_ASSERT_NOT_NULL(out);

    TEST_ASSERT_EQUAL_STRING("health", out->var_name);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_NETVAR_AUTHOR_TYPE_F32, out->var_type);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_NETVAR_AUTHOR_AUTH_OWNER, out->authority);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 87.5f, out->initial_value);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* ── NetworkVariable component round-trip (I32 / Client) ────────────── */

static void test_netvar_round_trip_i32(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();

    JceEntity e = jce_scene_create_entity(src, "Ammo");

    JceNetworkVariableComponent nv;
    memset(&nv, 0, sizeof nv);
    snprintf(nv.var_name, sizeof nv.var_name, "%s", "ammo");
    nv.var_type      = JCE_NETVAR_AUTHOR_TYPE_I32;
    nv.authority     = JCE_NETVAR_AUTHOR_AUTH_CLIENT;
    nv.initial_value = 30.0f;   /* int authored through the float field */
    jce_scene_set_network_variable(src, e, &nv);

    JceJson *root = jce_scene_save_json(src);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Ammo");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    JceNetworkVariableComponent *out = jce_scene_get_network_variable(dst, ne);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("ammo", out->var_name);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_NETVAR_AUTHOR_TYPE_I32, out->var_type);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_NETVAR_AUTHOR_AUTH_CLIENT, out->authority);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 30.0f, out->initial_value);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* ── NetworkVariable component round-trip (Bool / Server) ───────────── */

static void test_netvar_round_trip_bool(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();

    JceEntity e = jce_scene_create_entity(src, "Alive");

    JceNetworkVariableComponent nv;
    memset(&nv, 0, sizeof nv);
    snprintf(nv.var_name, sizeof nv.var_name, "%s", "isAlive");
    nv.var_type      = JCE_NETVAR_AUTHOR_TYPE_BOOL;
    nv.authority     = JCE_NETVAR_AUTHOR_AUTH_SERVER;
    nv.initial_value = 1.0f;   /* true */
    jce_scene_set_network_variable(src, e, &nv);

    JceJson *root = jce_scene_save_json(src);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Alive");
    JceNetworkVariableComponent *out = jce_scene_get_network_variable(dst, ne);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("isAlive", out->var_name);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_NETVAR_AUTHOR_TYPE_BOOL, out->var_type);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_NETVAR_AUTHOR_AUTH_SERVER, out->authority);
    TEST_ASSERT_TRUE(out->initial_value != 0.0f);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* An entity with NO NetworkVariable component must reload with none — the
 * runtime net path stays byte-identical to legacy scenes. */
static void test_no_netvar_default(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();

    JceEntity e = jce_scene_create_entity(src, "Plain");
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(src, e, &t);

    JceJson *root = jce_scene_save_json(src);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Plain");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    TEST_ASSERT_FALSE(jce_scene_has_network_variable(dst, ne));

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* An empty (default-added) NetworkVariable component round-trips and stays
 * present: empty name, type F32, authority Server, initial 0. */
static void test_netvar_empty_round_trip(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();

    JceEntity e = jce_scene_create_entity(src, "Empty");
    JceNetworkVariableComponent nv;
    memset(&nv, 0, sizeof nv);
    jce_scene_set_network_variable(src, e, &nv);

    JceJson *root = jce_scene_save_json(src);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Empty");
    TEST_ASSERT_TRUE(jce_scene_has_network_variable(dst, ne));
    JceNetworkVariableComponent *out = jce_scene_get_network_variable(dst, ne);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("", out->var_name);
    TEST_ASSERT_EQUAL_UINT8(0u, out->var_type);
    TEST_ASSERT_EQUAL_UINT8(0u, out->authority);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, out->initial_value);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* ── Headless runtime registration (the rt_spawn_net public seam) ────────
 *
 * The bridge binds the world, calls jce_net_var_register_all(), then seeds the
 * authored initial value via the PUBLIC typed-set API.  That set ATTACHES the
 * matching JceNetVarF32/I32 backing component, registering the variable for
 * replication.  We assert the substrate carries the components and the value
 * is readable — the same path rt_spawn_gameplay's net walk drives at Play.
 * The entity here carries no NetworkObject (net_id == INVALID), so the typed
 * setter's authority gate is bypassed (local-only writable), letting the
 * single-process headless test exercise the attach/read without standing up a
 * full server session. */
static void test_netvar_runtime_registration(void)
{
    JceScene *scene;
    JceEntity e;
    uint64_t  ent;

    jce_net_replication_init();
    scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    jce_net_replication_set_world(jce_scene_get_world(scene));
    jce_net_var_reset_hooks();

    /* The production registration the net bridge runs (after set_world). */
    jce_net_var_register_all();
    TEST_ASSERT_TRUE(jce_net_replication_component_count() >= 1u);

    e   = jce_scene_create_entity(scene, "Authored");
    ent = (uint64_t)e;

    /* F32 author: seed initial via the public typed-set (attaches NetVarF32). */
    TEST_ASSERT_TRUE(jce_net_var_f32_set(ent, 42.0f));
    TEST_ASSERT_EQUAL_FLOAT(42.0f, jce_net_var_f32_get(ent, -1.0f));

    /* I32 author on a second entity (attaches NetVarI32). */
    {
        JceEntity e2  = jce_scene_create_entity(scene, "Authored2");
        uint64_t  en2 = (uint64_t)e2;
        TEST_ASSERT_TRUE(jce_net_var_i32_set(en2, 7));
        TEST_ASSERT_EQUAL_INT32(7, jce_net_var_i32_get(en2, -1));
    }

    /* Detach the world before destroying the scene so the substrate does not
     * retain a dangling world pointer between cases. */
    jce_net_replication_set_world(NULL);
    jce_net_var_reset_hooks();
    jce_scene_destroy(scene);
    jce_net_replication_shutdown();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_netvar_round_trip_f32);
    RUN_TEST(test_netvar_round_trip_i32);
    RUN_TEST(test_netvar_round_trip_bool);
    RUN_TEST(test_no_netvar_default);
    RUN_TEST(test_netvar_empty_round_trip);
    RUN_TEST(test_netvar_runtime_registration);
    return UNITY_END();
}
