/*
 * test_jce_scene_ref_remap.c  Cross-entity reference remap on scene load (L4).
 *
 * Regression guard for audit R2F17: jce_scene_load_json renumbers entity ids
 * (every entity is created fresh), and patches parent / IkConstraints /
 * SequencePlayer references through a src_id -> new_id map — but VirtualCamera
 * follow/look-at, Constraint.target_entity, ConfigurableJoint / Joint2D
 * connected_body, and NavAgent.target_entity were copied verbatim, so they
 * pointed at stale ids after every reload / prefab instantiation.
 */

#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

typedef struct { const char *want; JceEntity found; } FindCtx;

static void find_cb(JceScene *s, JceEntity e, void *u)
{
    FindCtx *c = (FindCtx *)u;
    const char *n = jce_scene_entity_name(s, e);
    if (n && strcmp(n, c->want) == 0) c->found = e;
}

static JceEntity find_named(JceScene *s, const char *name)
{
    FindCtx c = { name, 0 };
    jce_scene_each_entity(s, find_cb, &c);
    return c.found;
}

static void test_cross_entity_refs_remap_on_load(void)
{
    JceScene *s1 = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s1);

    /* Churn ids in the source scene so they differ from the fresh destination
     * scene — a coincidental id match would otherwise mask a missing remap. */
    for (int i = 0; i < 8; i++) {
        JceEntity tmp = jce_scene_create_entity(s1, "churn");
        jce_scene_destroy_entity(s1, tmp);
    }

    JceEntity a = jce_scene_create_entity(s1, "cam");
    JceEntity b = jce_scene_create_entity(s1, "target");
    TEST_ASSERT_NOT_EQUAL(0, a);
    TEST_ASSERT_NOT_EQUAL(0, b);

    JceVirtualCameraComponent vc;
    memset(&vc, 0, sizeof(vc));
    vc.follow_target  = (uint64_t)b;
    vc.look_at_target = (uint64_t)b;
    jce_scene_set_virtual_camera(s1, a, &vc);

    JceConstraintComponent cn;
    memset(&cn, 0, sizeof(cn));
    cn.target_entity = (uint32_t)b;
    jce_scene_set_constraint(s1, a, &cn);

    JceJson *root = jce_scene_save_json(s1);
    TEST_ASSERT_NOT_NULL(root);

    JceScene *s2 = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s2);
    TEST_ASSERT_TRUE(jce_scene_load_json(s2, root) > 0);
    jce_json_free(root);

    JceEntity a2 = find_named(s2, "cam");
    JceEntity b2 = find_named(s2, "target");
    TEST_ASSERT_NOT_EQUAL(0, a2);
    TEST_ASSERT_NOT_EQUAL(0, b2);
    /* The reloaded ids must actually differ from the source ids, or the test
     * could not distinguish a remap from a verbatim copy. */
    TEST_ASSERT_TRUE(b2 != b);

    JceVirtualCameraComponent *vc2 = jce_scene_get_virtual_camera(s2, a2);
    TEST_ASSERT_NOT_NULL(vc2);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)b2, vc2->follow_target);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)b2, vc2->look_at_target);

    JceConstraintComponent *cn2 = jce_scene_get_constraint(s2, a2);
    TEST_ASSERT_NOT_NULL(cn2);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)b2, cn2->target_entity);

    jce_scene_destroy(s1);
    jce_scene_destroy(s2);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cross_entity_refs_remap_on_load);
    return UNITY_END();
}
