/*
 * test_jce_cloth.c — Self-test for P3-C.4 cloth/soft-body.
 *
 * Enables sim, creates a 4×4 patch pinned at top corners, steps ~30
 * frames, verifies bottom nodes have dropped under gravity, destroys
 * and verifies active_count == 0.
 */

#include "unity.h"

#include <jce/middleware/physics/jce_cloth.h>

#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static void test_cloth_patch_drops(void)
{
    jce_cloth_set_simulation_enabled(true);
    TEST_ASSERT_TRUE(jce_cloth_is_simulation_enabled());

    JceClothDesc d;
    memset(&d, 0, sizeof(d));
    d.corner_00 = jce_v3(-1.0f, 2.0f, -1.0f);
    d.corner_10 = jce_v3( 1.0f, 2.0f, -1.0f);
    d.corner_01 = jce_v3(-1.0f, 2.0f,  1.0f);
    d.corner_11 = jce_v3( 1.0f, 2.0f,  1.0f);
    d.res_u = 4;
    d.res_v = 4;
    d.mass_total       = 1.0f;
    d.stiffness_linear = 0.5f;
    d.stiffness_angular= 0.5f;
    d.damping          = 0.05f;
    d.iterations       = 4;

    /* Pin top row (indices 0..3 — row v=0). */
    uint32_t pins[4] = { 0, 1, 2, 3 };
    d.pinned_indices = pins;
    d.pinned_count   = 4;

    JceClothHandle h = jce_cloth_create(&d);
    TEST_ASSERT_NOT_EQUAL(JCE_CLOTH_INVALID, h);
    TEST_ASSERT_EQUAL_UINT32(1u, jce_cloth_active_count());
    TEST_ASSERT_EQUAL_UINT32(16u, jce_cloth_node_count(h));

    float pos0[16 * 3];
    TEST_ASSERT_TRUE(jce_cloth_get_positions(h, pos0, 16 * 3));

    /* Bottom-row index in row-major (v=res_v-1, u=res_u-1) = 15. */
    float initial_y = pos0[15 * 3 + 1];

    for (int i = 0; i < 30; ++i) {
        jce_cloth_step_(1.0f / 60.0f);
    }

    float pos1[16 * 3];
    TEST_ASSERT_TRUE(jce_cloth_get_positions(h, pos1, 16 * 3));
    float final_y = pos1[15 * 3 + 1];

    /* Bottom corner should have moved downward measurably. */
    TEST_ASSERT_TRUE(final_y < initial_y - 0.01f);

    /* Pinned nodes (top row) should NOT have moved noticeably. */
    for (int k = 0; k < 4; ++k) {
        float dy = pos1[k * 3 + 1] - pos0[k * 3 + 1];
        TEST_ASSERT_TRUE(dy > -0.001f && dy < 0.001f);
    }

    jce_cloth_destroy(h);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_cloth_active_count());

    jce_cloth_shutdown_();
}

static void test_cloth_disabled_does_not_step(void)
{
    jce_cloth_set_simulation_enabled(false);

    JceClothDesc d;
    memset(&d, 0, sizeof(d));
    d.corner_00 = jce_v3(-1.0f, 2.0f, -1.0f);
    d.corner_10 = jce_v3( 1.0f, 2.0f, -1.0f);
    d.corner_01 = jce_v3(-1.0f, 2.0f,  1.0f);
    d.corner_11 = jce_v3( 1.0f, 2.0f,  1.0f);
    d.res_u = 2;
    d.res_v = 2;
    d.mass_total = 1.0f;
    d.stiffness_linear = 0.5f;
    d.iterations = 2;

    JceClothHandle h = jce_cloth_create(&d);
    TEST_ASSERT_NOT_EQUAL(JCE_CLOTH_INVALID, h);

    float pos0[4 * 3];
    jce_cloth_get_positions(h, pos0, 4 * 3);

    for (int i = 0; i < 30; ++i) {
        jce_cloth_step_(1.0f / 60.0f);
    }

    float pos1[4 * 3];
    jce_cloth_get_positions(h, pos1, 4 * 3);

    /* Disabled — no motion. */
    for (int i = 0; i < 12; ++i) {
        float d_ = pos1[i] - pos0[i];
        TEST_ASSERT_TRUE(d_ > -1e-5f && d_ < 1e-5f);
    }

    jce_cloth_destroy(h);
    jce_cloth_shutdown_();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cloth_patch_drops);
    RUN_TEST(test_cloth_disabled_does_not_step);
    return UNITY_END();
}
