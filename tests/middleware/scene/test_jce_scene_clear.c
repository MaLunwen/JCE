/* test_jce_scene_clear.c
 *
 * Unit tests for jce_scene_clear(): the engine API that removes every
 * user entity from a scene without tearing down the scene object itself.
 *
 * Covered behaviour:
 *   - NULL guard returns -1
 *   - empty scene returns 0
 *   - mixed entity set with hierarchy is fully drained
 *   - scene object stays valid after clear (new entities can be created)
 *   - repeated clear is idempotent
 */

#include <jce/middleware/scene/jce_scene.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static int counter_cb(JceScene *s, JceEntity e, void *user)
{
    (void)s; (void)e;
    *(int *)user += 1;
    return 0;
}

static int count_entities(JceScene *s)
{
    int n = 0;
    jce_scene_each_entity(s, counter_cb, &n);
    return n;
}

static void test_clear_null(void)
{
    TEST_ASSERT_EQUAL_INT(-1, jce_scene_clear(NULL));
}

static void test_clear_empty(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    TEST_ASSERT_EQUAL_INT(0, jce_scene_clear(s));
    TEST_ASSERT_EQUAL_INT(0, count_entities(s));

    jce_scene_destroy(s);
}

static void test_clear_with_hierarchy(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity root = jce_scene_create_entity(s, "root");
    JceEntity a    = jce_scene_create_entity(s, "a");
    JceEntity b    = jce_scene_create_entity(s, "b");
    JceEntity c    = jce_scene_create_entity(s, "c");
    jce_scene_set_parent(s, a, root);
    jce_scene_set_parent(s, b, root);
    jce_scene_set_parent(s, c, a);

    TEST_ASSERT_EQUAL_INT(4, count_entities(s));

    int cleared = jce_scene_clear(s);
    TEST_ASSERT_EQUAL_INT(4, cleared);
    TEST_ASSERT_EQUAL_INT(0, count_entities(s));

    jce_scene_destroy(s);
}

static void test_scene_reusable_after_clear(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    jce_scene_create_entity(s, "old1");
    jce_scene_create_entity(s, "old2");
    TEST_ASSERT_EQUAL_INT(2, jce_scene_clear(s));

    JceEntity fresh = jce_scene_create_entity(s, "fresh");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, fresh);
    TEST_ASSERT_EQUAL_INT(1, count_entities(s));

    jce_scene_destroy(s);
}

static void test_clear_idempotent(void)
{
    JceScene *s = jce_scene_create();
    jce_scene_create_entity(s, "e1");
    TEST_ASSERT_EQUAL_INT(1, jce_scene_clear(s));
    TEST_ASSERT_EQUAL_INT(0, jce_scene_clear(s));
    TEST_ASSERT_EQUAL_INT(0, jce_scene_clear(s));
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_clear_null);
    RUN_TEST(test_clear_empty);
    RUN_TEST(test_clear_with_hierarchy);
    RUN_TEST(test_scene_reusable_after_clear);
    RUN_TEST(test_clear_idempotent);
    return UNITY_END();
}
