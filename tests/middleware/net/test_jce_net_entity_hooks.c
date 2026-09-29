/*
 * test_jce_net_entity_hooks.c — replicated entities must go through the
 * scene layer (audit C5-03).
 *
 * The replication substrate used to call ecs_new()/ecs_delete() straight on
 * the bound flecs world.  That is not merely "a different way to make an
 * entity" — it skips everything jce_scene_create_entity does around one:
 *
 *   - the scene's roster epoch, so every cache keyed on it (editor
 *     hierarchy, streaming, renderer rosters) keeps a stale entity list
 *     across a network spawn or despawn;
 *   - the default JceTransform, so a replicated entity has no transform
 *     until a snapshot happens to carry one;
 *   - JceTagActive, so systems that filter on "active" skip the entity
 *     entirely — present in the world, but not simulated.  That third
 *     one is NOT asserted below: the tag has no public predicate, so a
 *     test cannot observe it without reaching into flecs.  It is fixed
 *     by the same call the other two verify (jce_scene_create_entity
 *     adds all three together), but the coverage here is indirect and
 *     saying so is better than implying it was checked.
 *
 * jce_net is L4 and links only jce_core, so it cannot call jce_scene_*.  The
 * owner installs hooks instead.  These tests pin that the hooks are actually
 * taken, and that the un-hooked fallback still works for standalone
 * embeddings rather than being broken by the fix.
 */

#include "unity.h"

#include <jce/middleware/net/jce_replication.h>
#include <jce/middleware/scene/jce_scene.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static JceScene *g_scene;
static int       g_created;
static int       g_destroyed;
static uint64_t  g_last_created;

void setUp(void)
{
    g_scene = jce_scene_create();
    g_created = g_destroyed = 0;
    g_last_created = 0;
    jce_net_replication_init();
    jce_net_replication_set_world(jce_scene_get_world(g_scene));
    /* spawn() is server-authoritative; without this every case below
       would fail at the spawn rather than at what it means to test. */
    jce_net_replication_set_role(JCE_NET_ROLE_SERVER);
}

void tearDown(void)
{
    jce_net_replication_set_entity_hooks(NULL, NULL, NULL);
    jce_net_replication_shutdown();
    if (g_scene) { jce_scene_destroy(g_scene); g_scene = NULL; }
}

static uint64_t hook_create(void *user)
{
    JceScene *s = (JceScene *)user;
    ++g_created;
    g_last_created = (uint64_t)jce_scene_create_entity(s, "NetObject");
    return g_last_created;
}

static void hook_destroy(void *user, uint64_t entity)
{
    JceScene *s = (JceScene *)user;
    ++g_destroyed;
    jce_scene_destroy_entity(s, (JceEntity)entity);
}

static JceNetObjectId spawn_one(void)
{
    JceNetObjectDesc d;
    memset(&d, 0, sizeof d);
    d.owner = JCE_CLIENT_SERVER;
    return jce_net_object_spawn(&d);
}

/* The hook must actually be taken — not merely installed. */
static void test_spawn_routes_through_the_hook(void)
{
    jce_net_replication_set_entity_hooks(hook_create, hook_destroy, g_scene);

    const JceNetObjectId id = spawn_one();
    TEST_ASSERT_NOT_EQUAL_UINT32(JCE_NET_OBJECT_INVALID, id);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_created,
        "replication created its entity without going through the hook");

    const uint64_t e = jce_net_object_to_entity(id);
    TEST_ASSERT_EQUAL_UINT64(g_last_created, e);
    TEST_ASSERT_TRUE(jce_scene_entity_alive(g_scene, (JceEntity)e));
}

/* The three things the raw path skipped, asserted on the entity itself. */
static void test_hooked_entity_gets_scene_bookkeeping(void)
{
    const uint64_t epoch_before = jce_scene_get_roster_epoch(g_scene);

    jce_net_replication_set_entity_hooks(hook_create, hook_destroy, g_scene);
    const JceNetObjectId id = spawn_one();
    TEST_ASSERT_NOT_EQUAL_UINT32(JCE_NET_OBJECT_INVALID, id);

    const JceEntity e = (JceEntity)jce_net_object_to_entity(id);
    TEST_ASSERT_NOT_EQUAL_UINT64(0u, e);

    /* (1) a transform exists without waiting for a snapshot */
    TEST_ASSERT_TRUE_MESSAGE(jce_scene_has_transform(g_scene, e),
        "replicated entity has no default transform");

    /* (2) roster caches are invalidated */
    TEST_ASSERT_TRUE_MESSAGE(
        jce_scene_get_roster_epoch(g_scene) != epoch_before,
        "roster epoch did not move — caches keep a stale entity list across "
        "a network spawn");
}

static void test_despawn_routes_through_the_hook(void)
{
    jce_net_replication_set_entity_hooks(hook_create, hook_destroy, g_scene);
    const JceNetObjectId id = spawn_one();
    const JceEntity e = (JceEntity)jce_net_object_to_entity(id);
    TEST_ASSERT_TRUE(jce_scene_entity_alive(g_scene, e));

    jce_net_object_despawn(id);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_destroyed,
        "replication deleted its entity without going through the hook");
    TEST_ASSERT_FALSE_MESSAGE(jce_scene_entity_alive(g_scene, e),
        "entity survived a network despawn");
}

/* Without hooks the substrate must still function — a standalone or test
   embedding that never installs them is degraded, not broken. */
static void test_unhooked_fallback_still_creates_an_entity(void)
{
    jce_net_replication_set_entity_hooks(NULL, NULL, NULL);

    const JceNetObjectId id = spawn_one();
    TEST_ASSERT_NOT_EQUAL_UINT32(JCE_NET_OBJECT_INVALID, id);
    TEST_ASSERT_NOT_EQUAL_UINT64_MESSAGE(
        0u, jce_net_object_to_entity(id),
        "the un-hooked fallback stopped producing an entity at all");
    TEST_ASSERT_EQUAL_INT(0, g_created);   /* hook genuinely not used */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_spawn_routes_through_the_hook);
    RUN_TEST(test_hooked_entity_gets_scene_bookkeeping);
    RUN_TEST(test_despawn_routes_through_the_hook);
    RUN_TEST(test_unhooked_fallback_still_creates_an_entity);
    return UNITY_END();
}
