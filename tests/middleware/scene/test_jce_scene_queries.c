/* test_jce_scene_queries.c
 *
 * Unit tests for the scene-graph query API (parity roadmap #8 — Unity
 * Find/FindWithTag/OverlapSphere, Godot groups):
 *   - jce_scene_query_by_tag   : entities carrying a given tag
 *   - jce_scene_query_by_name  : entities with a given name
 *   - jce_scene_query_sphere   : entities whose transform origin is within r
 *   - guards (NULL args) + max-count cap
 *
 * Exercises the real jce_scene API; no editor / GPU.
 */

#include <jce/middleware/scene/jce_scene.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void set_pos(JceScene *s, JceEntity e, float x, float y, float z)
{
    JceTransform *t = jce_scene_get_transform(s, e);
    if (t) t->position = jce_v3(x, y, z);
}

static void test_query_by_tag(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    jce_scene_set_entity_tag_name(s, jce_scene_create_entity(s, "a"), "Enemy");
    jce_scene_set_entity_tag_name(s, jce_scene_create_entity(s, "b"), "Enemy");
    jce_scene_set_entity_tag_name(s, jce_scene_create_entity(s, "c"), "Pickup");
    jce_scene_create_entity(s, "d");   /* untagged */

    JceEntity out[16];
    TEST_ASSERT_EQUAL_INT(2, jce_scene_query_by_tag(s, "Enemy",  out, 16));
    TEST_ASSERT_EQUAL_INT(1, jce_scene_query_by_tag(s, "Pickup", out, 16));
    TEST_ASSERT_EQUAL_INT(0, jce_scene_query_by_tag(s, "Nope",   out, 16));
    jce_scene_destroy(s);
}

static void test_query_by_name(void)
{
    /* jce_scene_create_entity uniquifies duplicate names, so each name is a
     * single match (Unity Find-by-name semantics). */
    JceScene *s = jce_scene_create();
    jce_scene_create_entity(s, "Boss");
    jce_scene_create_entity(s, "Minion");
    jce_scene_create_entity(s, "Healer");

    JceEntity out[16];
    TEST_ASSERT_EQUAL_INT(1, jce_scene_query_by_name(s, "Boss",   out, 16));
    TEST_ASSERT_EQUAL_INT(1, jce_scene_query_by_name(s, "Minion", out, 16));
    TEST_ASSERT_EQUAL_INT(1, jce_scene_query_by_name(s, "Healer", out, 16));
    TEST_ASSERT_EQUAL_INT(0, jce_scene_query_by_name(s, "Ghost",  out, 16));
    jce_scene_destroy(s);
}

static void test_query_sphere(void)
{
    JceScene *s = jce_scene_create();
    JceEntity a = jce_scene_create_entity(s, "a"); set_pos(s, a,   0, 0, 0);
    JceEntity b = jce_scene_create_entity(s, "b"); set_pos(s, b,   3, 0, 0);
    JceEntity c = jce_scene_create_entity(s, "c"); set_pos(s, c, 100, 0, 0);
    (void)a; (void)b; (void)c;

    JceEntity out[16];
    /* radius 5 at origin: a(0) + b(3) inside, c(100) outside -> 2 */
    TEST_ASSERT_EQUAL_INT(2, jce_scene_query_sphere(s, jce_v3(0,0,0), 5.0f, out, 16));
    /* radius 1: only a -> 1 */
    TEST_ASSERT_EQUAL_INT(1, jce_scene_query_sphere(s, jce_v3(0,0,0), 1.0f, out, 16));
    /* radius 200: all three */
    TEST_ASSERT_EQUAL_INT(3, jce_scene_query_sphere(s, jce_v3(0,0,0), 200.0f, out, 16));
    jce_scene_destroy(s);
}

static void test_query_guards_and_cap(void)
{
    JceScene *s = jce_scene_create();
    JceEntity out[4];
    TEST_ASSERT_EQUAL_INT(0, jce_scene_query_by_tag(NULL, "x", out, 4));
    TEST_ASSERT_EQUAL_INT(0, jce_scene_query_by_tag(s, NULL, out, 4));
    TEST_ASSERT_EQUAL_INT(0, jce_scene_query_by_name(s, "x", NULL, 4));
    TEST_ASSERT_EQUAL_INT(0, jce_scene_query_sphere(s, jce_v3(0,0,0), 1.0f, out, 0));

    /* max cap: 3 tagged, ask for max 2 -> 2 */
    jce_scene_set_entity_tag_name(s, jce_scene_create_entity(s, "t1"), "T");
    jce_scene_set_entity_tag_name(s, jce_scene_create_entity(s, "t2"), "T");
    jce_scene_set_entity_tag_name(s, jce_scene_create_entity(s, "t3"), "T");
    TEST_ASSERT_EQUAL_INT(2, jce_scene_query_by_tag(s, "T", out, 2));
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_query_by_tag);
    RUN_TEST(test_query_by_name);
    RUN_TEST(test_query_sphere);
    RUN_TEST(test_query_guards_and_cap);
    return UNITY_END();
}
