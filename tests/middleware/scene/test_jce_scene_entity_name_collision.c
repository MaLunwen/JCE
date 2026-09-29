/*
 * test_jce_scene_entity_name_collision.c
 *
 * Entity names must be what the caller asked for.
 *
 * JCE creates scene entities in the flecs ROOT scope, where flecs's own
 * built-in entities live ("Empty", "Target", "Prefab", "Disabled", "Name",
 * "Component", "World", "Module", "Observer", ...).  flecs ABORTS the process
 * on a duplicate name within a scope, so jce_scene_create_entity defensively
 * appends "_<entity_id>" when ecs_lookup finds the name taken.
 *
 * The consequence was silent and user-facing: asking for an entity named
 * "Target" produced one named "Target_470", so every later lookup by the
 * authored name failed and the authored name never reached the saved scene.
 * Four previously-unregistered authoring tests were failing for exactly this
 * reason (three used "Empty", one used "Target") and the failure looked like
 * a component-serialisation bug, which it was not.
 *
 * The contract these tests lock: the AUTHORED name is the entity's identity.
 * Uniquification may still happen inside flecs to preserve its index
 * invariant, but it must never be observable through the public API or the
 * serialised scene.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* Names that collide with flecs built-ins in a fresh world.  Measured, not
 * guessed -- see the probe results recorded with this fix. */
static const char *const k_colliding[] = {
    "Empty", "Target", "Prefab", "Disabled",
    "Name", "Component", "World", "Module", "Observer",
};
static const int k_colliding_count =
    (int)(sizeof k_colliding / sizeof k_colliding[0]);

typedef struct { const char *want; JceEntity found; int seen; } FindCtx;

static void find_cb(JceScene *s, JceEntity e, void *ud)
{
    FindCtx *ctx = (FindCtx *)ud;
    const char *nm = jce_scene_entity_registered_name(s, e);
    ctx->seen++;
    if (nm && strcmp(nm, ctx->want) == 0) ctx->found = e;
}

static JceEntity find_by_name(JceScene *s, const char *name)
{
    FindCtx ctx = { name, JCE_ENTITY_INVALID, 0 };
    jce_scene_each_entity(s, find_cb, &ctx);
    return ctx.found;
}

/* ── 1. create_entity returns the name that was asked for ──────────── */

static void test_created_name_is_the_authored_name(void)
{
    for (int i = 0; i < k_colliding_count; i++) {
        JceScene *s = jce_scene_create();
        TEST_ASSERT_NOT_NULL(s);

        JceEntity e = jce_scene_create_entity(s, k_colliding[i]);
        TEST_ASSERT_NOT_EQUAL_UINT64(JCE_ENTITY_INVALID, e);

        TEST_ASSERT_EQUAL_STRING(k_colliding[i],
                                 jce_scene_entity_registered_name(s, e));
        TEST_ASSERT_EQUAL_STRING(k_colliding[i],
                                 jce_scene_entity_name(s, e));

        jce_scene_destroy(s);
    }
}

/* ── 2. a non-colliding name is unaffected ─────────────────────────── */

static void test_ordinary_name_unchanged(void)
{
    JceScene *s = jce_scene_create();
    JceEntity e = jce_scene_create_entity(s, "Hero");
    TEST_ASSERT_EQUAL_STRING("Hero", jce_scene_entity_registered_name(s, e));
    TEST_ASSERT_EQUAL_STRING("Hero", jce_scene_entity_name(s, e));
    jce_scene_destroy(s);
}

/* ── 3. the authored name survives save -> load ────────────────────── */

static void test_colliding_name_round_trips(void)
{
    for (int i = 0; i < k_colliding_count; i++) {
        JceScene *src = jce_scene_create();
        JceScene *dst = jce_scene_create();

        JceEntity e = jce_scene_create_entity(src, k_colliding[i]);
        TEST_ASSERT_NOT_EQUAL_UINT64(JCE_ENTITY_INVALID, e);

        JceJson *root = jce_scene_save_json(src);
        TEST_ASSERT_NOT_NULL(root);
        int loaded = jce_scene_load_json(dst, root);
        TEST_ASSERT_EQUAL_INT(1, loaded);
        jce_json_free(root);

        JceEntity ne = find_by_name(dst, k_colliding[i]);
        TEST_ASSERT_NOT_EQUAL_UINT64(JCE_ENTITY_INVALID, ne);
        TEST_ASSERT_EQUAL_STRING(k_colliding[i],
                                 jce_scene_entity_registered_name(dst, ne));

        jce_scene_destroy(src);
        jce_scene_destroy(dst);
    }
}

/* ── 4. duplicate authored names both keep their authored name ─────── */

static void test_duplicate_authored_names_both_survive(void)
{
    JceScene *s = jce_scene_create();

    JceEntity a = jce_scene_create_entity(s, "Ped");
    JceEntity b = jce_scene_create_entity(s, "Ped");
    TEST_ASSERT_NOT_EQUAL_UINT64(JCE_ENTITY_INVALID, a);
    TEST_ASSERT_NOT_EQUAL_UINT64(JCE_ENTITY_INVALID, b);
    TEST_ASSERT_NOT_EQUAL_UINT64(a, b);

    /* Both entities were authored "Ped" and both must report "Ped".
     * Uniquification is an internal flecs concern, not an API-visible one. */
    TEST_ASSERT_EQUAL_STRING("Ped", jce_scene_entity_registered_name(s, a));
    TEST_ASSERT_EQUAL_STRING("Ped", jce_scene_entity_registered_name(s, b));

    jce_scene_destroy(s);
}

/* ── 5. set_entity_name with a colliding name must not abort ───────── */

static void test_set_entity_name_colliding_is_safe(void)
{
    JceScene *s = jce_scene_create();
    JceEntity e = jce_scene_create_entity(s, "Hero");

    /* Before this fix this called ecs_set_name directly with no
     * uniquification, which aborts the process on a taken name. */
    jce_scene_set_entity_name(s, e, "Target");
    TEST_ASSERT_EQUAL_STRING("Target", jce_scene_entity_registered_name(s, e));

    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_created_name_is_the_authored_name);
    RUN_TEST(test_ordinary_name_unchanged);
    RUN_TEST(test_colliding_name_round_trips);
    RUN_TEST(test_duplicate_authored_names_both_survive);
    RUN_TEST(test_set_entity_name_colliding_is_safe);
    return UNITY_END();
}
