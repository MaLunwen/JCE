/* test_jce_scene_cloth.c
 *
 * Cloth runtime bridge: the scene's cloth-reconciliation pass (jce_scene_update)
 * creates a live solver handle from an authored JceClothComponent AND enables
 * the soft-body simulation (the fix — authoring cloth is an explicit opt-in that
 * must override the render-tier HW gate, which defaults sim OFF on the low/mid
 * baseline; without it an authored cloth sits frozen).
 *
 * Headless: jce_cloth_create lazily stands up its own Bullet soft world, so no
 * render pipeline / GPU is needed.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/physics/jce_cloth.h>
#include <jce/os/core/jce_math.h>

#include "unity.h"
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static void test_authored_cloth_creates_handle_and_enables_sim(void)
{
    /* Baseline: sim OFF (the render-tier default the HW gate sets). */
    jce_cloth_set_simulation_enabled(false);
    TEST_ASSERT_FALSE(jce_cloth_is_simulation_enabled());

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "Cloth");

    JceClothComponent c;
    memset(&c, 0, sizeof c);
    c.corner_00 = jce_v3(0, 0, 0); c.corner_10 = jce_v3(1, 0, 0);
    c.corner_01 = jce_v3(0, 0, 1); c.corner_11 = jce_v3(1, 0, 1);
    c.res_u = 4; c.res_v = 4;
    c.mass_total = 1.0f; c.stiffness_linear = 0.5f; c.damping = 0.1f;
    c.dirty = true; c.handle = 0;
    jce_scene_set_cloth(s, e, &c);

    /* Reconciliation runs inside jce_scene_update. */
    jce_scene_update(s, 1.0f / 60.0f);

    JceClothComponent *got = jce_scene_get_cloth(s, e);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_NOT_EQUAL(0, got->handle);                  /* handle created */
    TEST_ASSERT_EQUAL_UINT32(16u,
        jce_cloth_node_count((JceClothHandle)got->handle)); /* 4x4 grid */
    TEST_ASSERT_TRUE(jce_cloth_is_simulation_enabled());    /* THE FIX */

    /* A second update with no edit keeps the handle (no rebuild churn). */
    uint32_t h0 = got->handle;
    jce_scene_update(s, 1.0f / 60.0f);
    got = jce_scene_get_cloth(s, e);
    TEST_ASSERT_EQUAL_UINT32(h0, got->handle);

    jce_scene_destroy(s);
    jce_cloth_shutdown_();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_authored_cloth_creates_handle_and_enables_sim);
    return UNITY_END();
}
