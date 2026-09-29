/* test_jce_scene_world_transform.c
 *
 * Unit tests for jce_scene_get_world_matrix(): parent-chain transform
 * composition (world = parent_world * local). Guards the fix for the
 * "children don't inherit parent transforms" gap.
 *
 * Covered behaviour:
 *   - NULL / invalid guard returns identity
 *   - a root entity's world == its local (flat scenes unchanged)
 *   - a parented child composes translation up the chain
 *   - parent SCALE applies to the child's local offset (proves the
 *     parent*child multiply ORDER, not child*parent)
 *   - a 3-level chain accumulates correctly
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_math.h>

#include "unity.h"
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static JceTransform mk_trs(jce_vec3 pos, jce_vec3 scale)
{
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.position = pos;
    t.rotation = (jce_quat){ 0.0f, 0.0f, 0.0f, 1.0f }; /* identity */
    t.scale    = scale;
    return t;
}

static void assert_matrix_close(const jce_mat4 *expected,
                                const jce_mat4 *actual)
{
    int column;
    int row;

    for (column = 0; column < 4; ++column) {
        for (row = 0; row < 4; ++row) {
            TEST_ASSERT_FLOAT_WITHIN(
                0.0001f,
                expected->raw[column][row],
                actual->raw[column][row]);
        }
    }
}

static void test_world_null_is_identity(void)
{
    jce_mat4 m = jce_scene_get_world_matrix(NULL, 0);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, m.raw[0][0]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, m.raw[1][1]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, m.col[3].x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, m.col[3].y);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, m.col[3].z);
}

static void test_world_root_equals_local(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity e = jce_scene_create_entity(s, "root");
    JceTransform t = mk_trs(jce_v3(5.0f, 6.0f, 7.0f), jce_v3(1.0f, 1.0f, 1.0f));
    jce_scene_set_transform(s, e, &t);

    jce_mat4 w = jce_scene_get_world_matrix(s, e);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, w.col[3].x);
    TEST_ASSERT_EQUAL_FLOAT(6.0f, w.col[3].y);
    TEST_ASSERT_EQUAL_FLOAT(7.0f, w.col[3].z);

    jce_scene_destroy(s);
}

static void test_world_parent_translate_composes(void)
{
    JceScene *s = jce_scene_create();
    JceEntity p = jce_scene_create_entity(s, "p");
    JceEntity c = jce_scene_create_entity(s, "c");

    JceTransform tp = mk_trs(jce_v3(10.0f, 0.0f, 0.0f), jce_v3(1.0f, 1.0f, 1.0f));
    JceTransform tc = mk_trs(jce_v3(0.0f, 0.0f, 5.0f),  jce_v3(1.0f, 1.0f, 1.0f));
    jce_scene_set_transform(s, p, &tp);
    jce_scene_set_transform(s, c, &tc);
    jce_scene_set_parent(s, c, p);

    jce_mat4 w = jce_scene_get_world_matrix(s, c);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, w.col[3].x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f,  w.col[3].y);
    TEST_ASSERT_EQUAL_FLOAT(5.0f,  w.col[3].z);

    jce_scene_destroy(s);
}

/* Parent scale must apply to the child's local offset. world = parent*child
 * → offset (1,0,0) scaled by 2 → (2,0,0). If the multiply order were reversed
 * (child*parent) the result would be (1,0,0); this asserts the correct order. */
static void test_world_parent_scale_order(void)
{
    JceScene *s = jce_scene_create();
    JceEntity p = jce_scene_create_entity(s, "p");
    JceEntity c = jce_scene_create_entity(s, "c");

    JceTransform tp = mk_trs(jce_v3(0.0f, 0.0f, 0.0f), jce_v3(2.0f, 2.0f, 2.0f));
    JceTransform tc = mk_trs(jce_v3(1.0f, 0.0f, 0.0f), jce_v3(1.0f, 1.0f, 1.0f));
    jce_scene_set_transform(s, p, &tp);
    jce_scene_set_transform(s, c, &tc);
    jce_scene_set_parent(s, c, p);

    jce_mat4 w = jce_scene_get_world_matrix(s, c);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, w.col[3].x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, w.col[3].y);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, w.col[3].z);

    jce_scene_destroy(s);
}

static void test_world_three_level_chain(void)
{
    JceScene *s = jce_scene_create();
    JceEntity a = jce_scene_create_entity(s, "a");
    JceEntity b = jce_scene_create_entity(s, "b");
    JceEntity c = jce_scene_create_entity(s, "c");

    JceTransform ta = mk_trs(jce_v3(1.0f, 0.0f, 0.0f), jce_v3(1.0f, 1.0f, 1.0f));
    JceTransform tb = mk_trs(jce_v3(0.0f, 2.0f, 0.0f), jce_v3(1.0f, 1.0f, 1.0f));
    JceTransform tc = mk_trs(jce_v3(0.0f, 0.0f, 3.0f), jce_v3(1.0f, 1.0f, 1.0f));
    jce_scene_set_transform(s, a, &ta);
    jce_scene_set_transform(s, b, &tb);
    jce_scene_set_transform(s, c, &tc);
    jce_scene_set_parent(s, b, a);
    jce_scene_set_parent(s, c, b);

    jce_mat4 w = jce_scene_get_world_matrix(s, c);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, w.col[3].x);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, w.col[3].y);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, w.col[3].z);

    jce_scene_destroy(s);
}

static void test_world_pivot_offsets_model_without_moving_transform_origin(void)
{
    JceScene *s = jce_scene_create();
    JceEntity e = jce_scene_create_entity(s, "pivoted");

    JceTransform t = mk_trs(jce_v3(10.0f, 0.0f, 0.0f),
                            jce_v3(2.0f, 2.0f, 2.0f));
    JcePivotComponent p;
    memset(&p, 0, sizeof p);
    p.local_position = jce_v3(1.0f, 0.0f, 0.0f);
    jce_scene_set_transform(s, e, &t);
    jce_scene_set_pivot(s, e, &p);

    jce_mat4 w = jce_scene_get_world_matrix(s, e);

    TEST_ASSERT_EQUAL_FLOAT(8.0f, w.col[3].x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, w.col[3].y);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, w.col[3].z);
    TEST_ASSERT_TRUE(jce_scene_has_pivot(s, e));

    jce_scene_destroy(s);
}

static void test_pivot_component_serializes_round_trip(void)
{
    JceScene *s = jce_scene_create();
    JceEntity e = jce_scene_create_entity(s, "pivot_save");
    JcePivotComponent p;
    memset(&p, 0, sizeof p);
    p.local_position = jce_v3(1.5f, -2.0f, 3.25f);
    p.local_rotation = jce_q_identity();
    jce_scene_set_pivot(s, e, &p);

    JceJson *root = jce_scene_save_json(s);
    TEST_ASSERT_NOT_NULL(root);

    JceScene *loaded = jce_scene_create();
    TEST_ASSERT_TRUE(jce_scene_load_json(loaded, root));

    JcePivotComponent *loaded_p = jce_scene_get_pivot(loaded, e);
    TEST_ASSERT_NOT_NULL(loaded_p);
    TEST_ASSERT_EQUAL_FLOAT(1.5f, loaded_p->local_position.x);
    TEST_ASSERT_EQUAL_FLOAT(-2.0f, loaded_p->local_position.y);
    TEST_ASSERT_EQUAL_FLOAT(3.25f, loaded_p->local_position.z);

    jce_json_free(root);
    jce_scene_destroy(loaded);
    jce_scene_destroy(s);
}

static void test_set_pivot_world_position_preserves_model_matrix(void)
{
    JceScene *s = jce_scene_create();
    JceEntity e = jce_scene_create_entity(s, "pivot_preserve");

    JceTransform t = mk_trs(jce_v3(10.0f, 0.0f, 0.0f),
                            jce_v3(2.0f, 2.0f, 2.0f));
    jce_scene_set_transform(s, e, &t);

    jce_mat4 before = jce_scene_get_world_matrix(s, e);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, before.col[3].x);

    jce_scene_set_pivot_world_position_preserve_model(
        s, e, jce_v3(12.0f, 0.0f, 0.0f));

    JceTransform *after_t = jce_scene_get_transform(s, e);
    JcePivotComponent *after_p = jce_scene_get_pivot(s, e);
    jce_mat4 after = jce_scene_get_world_matrix(s, e);

    TEST_ASSERT_NOT_NULL(after_t);
    TEST_ASSERT_NOT_NULL(after_p);
    TEST_ASSERT_EQUAL_FLOAT(12.0f, after_t->position.x);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, after_p->local_position.x);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, after.col[3].x);
    TEST_ASSERT_EQUAL_FLOAT(before.col[3].y, after.col[3].y);
    TEST_ASSERT_EQUAL_FLOAT(before.col[3].z, after.col[3].z);

    jce_scene_destroy(s);
}

static void test_reparent_preserves_world_matrix(void)
{
    JceScene *s = jce_scene_create();
    JceEntity parent = jce_scene_create_entity(s, "parent");
    JceEntity child = jce_scene_create_entity(s, "child");
    JceTransform parent_t = mk_trs(jce_v3(10.0f, 2.0f, -3.0f),
                                   jce_v3(2.0f, 2.0f, 2.0f));
    JceTransform child_t = mk_trs(jce_v3(3.0f, 4.0f, 5.0f),
                                  jce_v3(0.5f, 0.5f, 0.5f));
    jce_mat4 before;
    jce_mat4 after;

    jce_scene_set_transform(s, parent, &parent_t);
    jce_scene_set_transform(s, child, &child_t);
    before = jce_scene_get_world_matrix(s, child);

    TEST_ASSERT_TRUE(jce_scene_reparent(s, child, parent, true));
    TEST_ASSERT_EQUAL_UINT64(parent, jce_scene_get_parent(s, child));
    after = jce_scene_get_world_matrix(s, child);
    assert_matrix_close(&before, &after);

    jce_scene_destroy(s);
}

static void test_reparent_preserves_transform_across_table_move(void)
{
    JceScene *s = jce_scene_create();
    JceEntity parent = jce_scene_create_entity(s, "parent");
    JceEntity child = jce_scene_create_entity(s, "child");
    JceEntity sentinel = jce_scene_create_entity(s, "sentinel");
    JceTransform parent_t = mk_trs(jce_v3(9.0f, -2.0f, 4.0f),
                                   jce_v3(1.5f, 1.5f, 1.5f));
    JceTransform child_t = mk_trs(jce_v3(2.0f, 3.0f, -5.0f),
                                  jce_v3(0.75f, 0.75f, 0.75f));
    JceTransform sentinel_t = mk_trs(jce_v3(700.0f, 800.0f, 900.0f),
                                     jce_v3(7.0f, 8.0f, 9.0f));
    jce_mat4 before;
    jce_mat4 after;

    jce_scene_set_transform(s, parent, &parent_t);
    jce_scene_set_transform(s, child, &child_t);
    jce_scene_set_transform(s, sentinel, &sentinel_t);
    before = jce_scene_get_world_matrix(s, child);

    /* child is deliberately not the last entity in its Flecs table. Moving it
     * to a parented table swap-removes sentinel into child's former slot. */
    TEST_ASSERT_TRUE(jce_scene_reparent(s, child, parent, true));
    after = jce_scene_get_world_matrix(s, child);
    assert_matrix_close(&before, &after);

    jce_scene_destroy(s);
}

static void test_reparent_preserves_pivoted_model_matrix(void)
{
    JceScene *s = jce_scene_create();
    JceEntity parent = jce_scene_create_entity(s, "parent");
    JceEntity child = jce_scene_create_entity(s, "pivoted_child");
    JceTransform parent_t = mk_trs(jce_v3(-7.0f, 3.0f, 2.0f),
                                   jce_v3(1.5f, 1.5f, 1.5f));
    JceTransform child_t = mk_trs(jce_v3(8.0f, 4.0f, -2.0f),
                                  jce_v3(2.0f, 2.0f, 2.0f));
    JcePivotComponent pivot;
    jce_mat4 before;
    jce_mat4 after;

    memset(&pivot, 0, sizeof pivot);
    pivot.local_position = jce_v3(1.0f, -0.5f, 0.25f);
    jce_scene_set_transform(s, parent, &parent_t);
    jce_scene_set_transform(s, child, &child_t);
    jce_scene_set_pivot(s, child, &pivot);
    before = jce_scene_get_world_matrix(s, child);

    TEST_ASSERT_TRUE(jce_scene_reparent(s, child, parent, true));
    after = jce_scene_get_world_matrix(s, child);
    assert_matrix_close(&before, &after);

    jce_scene_destroy(s);
}

static void test_unparent_preserves_world_matrix(void)
{
    JceScene *s = jce_scene_create();
    JceEntity parent = jce_scene_create_entity(s, "parent");
    JceEntity child = jce_scene_create_entity(s, "child");
    JceTransform parent_t = mk_trs(jce_v3(10.0f, 0.0f, 0.0f),
                                   jce_v3(1.0f, 1.0f, 1.0f));
    JceTransform child_t = mk_trs(jce_v3(1.0f, 2.0f, 3.0f),
                                  jce_v3(1.0f, 1.0f, 1.0f));
    jce_mat4 before;
    jce_mat4 after;

    jce_scene_set_transform(s, parent, &parent_t);
    jce_scene_set_transform(s, child, &child_t);
    jce_scene_set_parent(s, child, parent);
    before = jce_scene_get_world_matrix(s, child);

    TEST_ASSERT_TRUE(jce_scene_reparent(
        s, child, JCE_ENTITY_INVALID, true));
    TEST_ASSERT_EQUAL_UINT64(JCE_ENTITY_INVALID,
                             jce_scene_get_parent(s, child));
    after = jce_scene_get_world_matrix(s, child);
    assert_matrix_close(&before, &after);

    jce_scene_destroy(s);
}

static void test_reparent_can_keep_local_transform(void)
{
    JceScene *s = jce_scene_create();
    JceEntity parent = jce_scene_create_entity(s, "parent");
    JceEntity child = jce_scene_create_entity(s, "child");
    JceTransform parent_t = mk_trs(jce_v3(10.0f, 0.0f, 0.0f),
                                   jce_v3(1.0f, 1.0f, 1.0f));
    JceTransform child_t = mk_trs(jce_v3(1.0f, 0.0f, 0.0f),
                                  jce_v3(1.0f, 1.0f, 1.0f));
    JceTransform *local;
    jce_mat4 world;

    jce_scene_set_transform(s, parent, &parent_t);
    jce_scene_set_transform(s, child, &child_t);

    TEST_ASSERT_TRUE(jce_scene_reparent(s, child, parent, false));
    local = jce_scene_get_transform(s, child);
    TEST_ASSERT_NOT_NULL(local);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, local->position.x);
    world = jce_scene_get_world_matrix(s, child);
    TEST_ASSERT_EQUAL_FLOAT(11.0f, world.col[3].x);

    jce_scene_destroy(s);
}

static void test_reparent_rejects_self_cycles_and_dead_entities(void)
{
    JceScene *s = jce_scene_create();
    JceEntity root = jce_scene_create_entity(s, "root");
    JceEntity child = jce_scene_create_entity(s, "child");
    JceEntity grandchild = jce_scene_create_entity(s, "grandchild");
    JceEntity dead = jce_scene_create_entity(s, "dead");

    jce_scene_set_parent(s, child, root);
    jce_scene_set_parent(s, grandchild, child);
    jce_scene_destroy_entity(s, dead);

    TEST_ASSERT_FALSE(jce_scene_reparent(s, root, root, true));
    TEST_ASSERT_FALSE(jce_scene_reparent(s, root, grandchild, true));
    TEST_ASSERT_FALSE(jce_scene_reparent(s, child, dead, true));
    TEST_ASSERT_FALSE(jce_scene_reparent(s, dead, root, true));
    TEST_ASSERT_EQUAL_UINT64(JCE_ENTITY_INVALID,
                             jce_scene_get_parent(s, root));
    TEST_ASSERT_EQUAL_UINT64(root, jce_scene_get_parent(s, child));
    TEST_ASSERT_EQUAL_UINT64(child, jce_scene_get_parent(s, grandchild));

    jce_scene_destroy(s);
}

static void test_set_parent_rejects_hierarchy_cycle(void)
{
    JceScene *s = jce_scene_create();
    JceEntity child = jce_scene_create_entity(s, "child");
    JceEntity cycle_a = jce_scene_create_entity(s, "cycle_a");
    JceEntity cycle_b = jce_scene_create_entity(s, "cycle_b");

    jce_scene_set_parent(s, cycle_a, cycle_b);
    jce_scene_set_parent(s, cycle_b, cycle_a);

    TEST_ASSERT_EQUAL_UINT64(cycle_b,
                             jce_scene_get_parent(s, cycle_a));
    TEST_ASSERT_EQUAL_UINT64(JCE_ENTITY_INVALID,
                             jce_scene_get_parent(s, cycle_b));
    TEST_ASSERT_TRUE(jce_scene_reparent(s, child, cycle_a, true));
    TEST_ASSERT_EQUAL_UINT64(cycle_a, jce_scene_get_parent(s, child));

    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_world_null_is_identity);
    RUN_TEST(test_world_root_equals_local);
    RUN_TEST(test_world_parent_translate_composes);
    RUN_TEST(test_world_parent_scale_order);
    RUN_TEST(test_world_three_level_chain);
    RUN_TEST(test_world_pivot_offsets_model_without_moving_transform_origin);
    RUN_TEST(test_pivot_component_serializes_round_trip);
    RUN_TEST(test_set_pivot_world_position_preserves_model_matrix);
    RUN_TEST(test_reparent_preserves_world_matrix);
    RUN_TEST(test_reparent_preserves_transform_across_table_move);
    RUN_TEST(test_reparent_preserves_pivoted_model_matrix);
    RUN_TEST(test_unparent_preserves_world_matrix);
    RUN_TEST(test_reparent_can_keep_local_transform);
    RUN_TEST(test_reparent_rejects_self_cycles_and_dead_entities);
    RUN_TEST(test_set_parent_rejects_hierarchy_cycle);
    return UNITY_END();
}
