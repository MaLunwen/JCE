/* test_jce_scene_each_canvas.c
 *
 * jce_scene_each_canvas exists because the UI overlay used to find its canvases
 * by walking EVERY entity in the world and probing has_canvas on each - and the
 * "no canvases, nothing to do" early-out ran only AFTER that walk, so a scene
 * with no UI at all still paid for it: 4.14 ms per frame at 200k entities, with
 * the overlay at its shipped default of ON.
 *
 * Swapping a world walk for a component query is only correct if the query
 * visits exactly the same set, so that is what these lock: every canvas, only
 * canvases, each once - including the case the optimisation was written for,
 * a large scene with no canvas at all (which must visit nothing).
 */
#include <jce/middleware/scene/jce_scene.h>

#include "unity.h"
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

typedef struct {
    int      count;
    JceEntity seen[64];
} Collected;

static void collect_cb(JceScene *s, JceEntity e, void *user)
{
    (void)s;
    Collected *c = (Collected *)user;
    if (c->count < 64) c->seen[c->count] = e;
    c->count++;
}

static bool saw(const Collected *c, JceEntity e)
{
    for (int i = 0; i < c->count && i < 64; i++)
        if (c->seen[i] == e) return true;
    return false;
}

static void test_visits_every_canvas_and_only_canvases(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceCanvasComponent cv;
    memset(&cv, 0, sizeof cv);

    JceEntity with[3], without[5];
    for (int i = 0; i < 3; i++) {
        with[i] = jce_scene_create_entity(s, "canvas");
        jce_scene_set_canvas(s, with[i], &cv);
    }
    for (int i = 0; i < 5; i++)
        without[i] = jce_scene_create_entity(s, "plain");

    Collected c;
    memset(&c, 0, sizeof c);
    jce_scene_each_canvas(s, collect_cb, &c);

    TEST_ASSERT_EQUAL_INT(3, c.count);              /* each once, no duplicates */
    for (int i = 0; i < 3; i++)
        TEST_ASSERT_TRUE(saw(&c, with[i]));         /* every canvas */
    for (int i = 0; i < 5; i++)
        TEST_ASSERT_FALSE(saw(&c, without[i]));     /* only canvases */

    jce_scene_destroy(s);
}

static void test_scene_with_no_canvas_visits_nothing(void)
{
    /* The case the change exists for: plenty of entities, zero canvases.  The
     * old world walk visited all of them to conclude there was nothing to do. */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    for (int i = 0; i < 200; i++) jce_scene_create_entity(s, "plain");

    Collected c;
    memset(&c, 0, sizeof c);
    jce_scene_each_canvas(s, collect_cb, &c);
    TEST_ASSERT_EQUAL_INT(0, c.count);

    jce_scene_destroy(s);
}

static void test_removing_a_canvas_drops_it_from_the_iteration(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceCanvasComponent cv;
    memset(&cv, 0, sizeof cv);

    JceEntity a = jce_scene_create_entity(s, "a");
    JceEntity b = jce_scene_create_entity(s, "b");
    jce_scene_set_canvas(s, a, &cv);
    jce_scene_set_canvas(s, b, &cv);

    Collected c;
    memset(&c, 0, sizeof c);
    jce_scene_each_canvas(s, collect_cb, &c);
    TEST_ASSERT_EQUAL_INT(2, c.count);

    jce_scene_remove_canvas(s, a);
    memset(&c, 0, sizeof c);
    jce_scene_each_canvas(s, collect_cb, &c);
    TEST_ASSERT_EQUAL_INT(1, c.count);
    TEST_ASSERT_TRUE(saw(&c, b));
    TEST_ASSERT_FALSE(saw(&c, a));

    /* Destroying the last one empties it. */
    jce_scene_destroy_entity(s, b);
    memset(&c, 0, sizeof c);
    jce_scene_each_canvas(s, collect_cb, &c);
    TEST_ASSERT_EQUAL_INT(0, c.count);

    jce_scene_destroy(s);
}

static void test_null_arguments_are_no_ops(void)
{
    Collected c;
    memset(&c, 0, sizeof c);
    jce_scene_each_canvas(NULL, collect_cb, &c);
    TEST_ASSERT_EQUAL_INT(0, c.count);

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    jce_scene_each_canvas(s, NULL, &c);   /* must not crash */
    TEST_ASSERT_EQUAL_INT(0, c.count);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_visits_every_canvas_and_only_canvases);
    RUN_TEST(test_scene_with_no_canvas_visits_nothing);
    RUN_TEST(test_removing_a_canvas_drops_it_from_the_iteration);
    RUN_TEST(test_null_arguments_are_no_ops);
    return UNITY_END();
}
