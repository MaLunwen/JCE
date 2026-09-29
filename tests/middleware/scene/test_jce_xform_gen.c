/*
 * test_jce_xform_gen.c
 *
 * Per-entity transform generation (dynamic-scene world-cache opt).
 *
 * Invariant under test: jce_scene_set_transform must NOT bump the global
 * structural epoch anymore — instead it bumps ONLY the moved entity's
 * per-entity gen plus every descendant's (a parent move changes its children's
 * world), leaving siblings untouched.  This is what lets the renderer keep a
 * moving camera / script-animated entity from invalidating the whole scene's
 * static world cache (measured ~+27% cpu/frame at 10k entities when it did).
 */

#include <jce/middleware/scene/jce_scene.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void set_x(JceScene *s, JceEntity e, float x)
{
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.position.x = x;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    t.rotation.w = 1.0f;            /* identity quat */
    jce_scene_set_transform(s, e, &t);
}

/* set_transform is per-entity: moving A bumps A + its descendant C, never the
 * sibling B, and never the GLOBAL structural epoch. */
void test_set_transform_is_per_entity(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity A = jce_scene_create_entity(s, "A");
    JceEntity B = jce_scene_create_entity(s, "B");
    JceEntity C = jce_scene_create_entity(s, "C");
    jce_scene_set_parent(s, C, A);     /* C is a child of A */

    set_x(s, A, 1.0f);
    set_x(s, B, 2.0f);
    set_x(s, C, 3.0f);

    /* Baseline AFTER all initial edits + the reparent. */
    const uint64_t epoch0 = jce_scene_get_structural_epoch(s);
    const uint64_t gA0 = jce_scene_entity_xform_gen(s, A);
    const uint64_t gB0 = jce_scene_entity_xform_gen(s, B);
    const uint64_t gC0 = jce_scene_entity_xform_gen(s, C);

    /* Move ONLY A. */
    set_x(s, A, 10.0f);

    /* THE FIX: a transform edit no longer bumps the global structural epoch
     * (so the renderer's static cache for every OTHER entity stays valid). */
    TEST_ASSERT_EQUAL_UINT64(epoch0, jce_scene_get_structural_epoch(s));

    /* A's own gen advanced. */
    TEST_ASSERT_TRUE(jce_scene_entity_xform_gen(s, A) > gA0);
    /* C is A's child → its world changed → gen propagated. */
    TEST_ASSERT_TRUE(jce_scene_entity_xform_gen(s, C) > gC0);
    /* B is a sibling → untouched. */
    TEST_ASSERT_EQUAL_UINT64(gB0, jce_scene_entity_xform_gen(s, B));

    jce_scene_destroy(s);
}

/* A scene that never set_transforms after creation keeps xgen inactive only
 * until the first edit; once any entity moves, the renderer's per-entity check
 * engages.  (Sanity: the accessor is 0 for an untracked entity.) */
void test_untracked_entity_gen_is_zero(void)
{
    JceScene *s = jce_scene_create();
    JceEntity e = jce_scene_create_entity(s, "lonely");
    /* No set_transform yet → gen is 0 (epoch-only fast path). */
    TEST_ASSERT_EQUAL_UINT64(0u, jce_scene_entity_xform_gen(s, e));
    set_x(s, e, 5.0f);
    TEST_ASSERT_TRUE(jce_scene_xgen_active(s));
    TEST_ASSERT_TRUE(jce_scene_entity_xform_gen(s, e) > 0u);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_set_transform_is_per_entity);
    RUN_TEST(test_untracked_entity_gen_is_zero);
    return UNITY_END();
}
