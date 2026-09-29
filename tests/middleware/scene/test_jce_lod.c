/* test_jce_lod.c
 *
 * Pure-math unit tests for the distance-based LOD selector.  The picker
 * is a hysteresis-driven state machine, so we exercise every transition:
 *   - nominal selection (no prior level)
 *   - step-down when distance crosses thr*(1+h)
 *   - step-up when distance crosses thr_prev*(1-h)
 *   - dead-zone preservation
 *   - cull beyond the last level (only when explicit, not FLT_MAX)
 *   - hysteresis clamping (sane default when caller passes garbage)
 *
 * No real JceMesh is allocated; the picker never dereferences the mesh
 * pointer, so we hand it small sentinel structures.
 */

#include <jce/middleware/scene/jce_lod.h>

#include <float.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

struct JceMesh { int id; };
static struct JceMesh high_mesh = { 1 };
static struct JceMesh mid_mesh  = { 2 };
static struct JceMesh low_mesh  = { 3 };

static void test_init_resets_and_defaults(void)
{
    JceLodGroup g;
    memset(&g, 0xAA, sizeof(g));
    jce_lod_init(&g);
    TEST_ASSERT_EQUAL_INT(0, g.count);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.05f, g.hysteresis);

    jce_lod_init(NULL);
}

static void test_setup_packs_levels_and_caps_last(void)
{
    JceLodGroup g;
    jce_lod_setup(&g, &high_mesh, 10.0f, &mid_mesh, 30.0f, &low_mesh, 80.0f);
    TEST_ASSERT_EQUAL_INT(3, g.count);
    TEST_ASSERT_EQUAL_PTR(&high_mesh, g.levels[0].mesh);
    TEST_ASSERT_EQUAL_PTR(&mid_mesh,  g.levels[1].mesh);
    TEST_ASSERT_EQUAL_PTR(&low_mesh,  g.levels[2].mesh);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f, g.levels[0].distance);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 30.0f, g.levels[1].distance);
    TEST_ASSERT_TRUE(g.levels[2].distance >= FLT_MAX * 0.5f);
}

static void test_setup_skips_null_slots(void)
{
    JceLodGroup g;
    jce_lod_setup(&g, &high_mesh, 25.0f, NULL, 0.0f, NULL, 0.0f);
    TEST_ASSERT_EQUAL_INT(1, g.count);
    TEST_ASSERT_EQUAL_PTR(&high_mesh, g.levels[0].mesh);
    TEST_ASSERT_TRUE(g.levels[0].distance >= FLT_MAX * 0.5f);
}

static void test_pick_returns_minus_one_for_empty(void)
{
    JceLodGroup g;
    jce_lod_init(&g);
    TEST_ASSERT_EQUAL_INT(-1, jce_lod_pick(&g, 5.0f, -1));
    TEST_ASSERT_EQUAL_INT(-1, jce_lod_pick(NULL, 5.0f, -1));
}

static void test_pick_nominal_no_prev(void)
{
    JceLodGroup g;
    jce_lod_setup(&g, &high_mesh, 10.0f, &mid_mesh, 30.0f, &low_mesh, 80.0f);
    TEST_ASSERT_EQUAL_INT(0, jce_lod_pick(&g,  5.0f, -1));
    TEST_ASSERT_EQUAL_INT(1, jce_lod_pick(&g, 20.0f, -1));
    TEST_ASSERT_EQUAL_INT(2, jce_lod_pick(&g, 1.0e9f, -1));
}

static void test_pick_step_down_requires_hysteresis(void)
{
    JceLodGroup g;
    jce_lod_setup(&g, &high_mesh, 10.0f, &mid_mesh, 30.0f, &low_mesh, 80.0f);
    g.hysteresis = 0.10f;
    TEST_ASSERT_EQUAL_INT(0, jce_lod_pick(&g, 10.5f, 0));
    TEST_ASSERT_EQUAL_INT(1, jce_lod_pick(&g, 11.5f, 0));
    TEST_ASSERT_EQUAL_INT(1, jce_lod_pick(&g, 200.0f, 0));
}

static void test_pick_step_up_requires_hysteresis(void)
{
    JceLodGroup g;
    jce_lod_setup(&g, &high_mesh, 10.0f, &mid_mesh, 30.0f, &low_mesh, 80.0f);
    g.hysteresis = 0.10f;
    TEST_ASSERT_EQUAL_INT(1, jce_lod_pick(&g, 9.5f, 1));
    TEST_ASSERT_EQUAL_INT(0, jce_lod_pick(&g, 8.5f, 1));
}

static void test_pick_dead_zone_keeps_prev(void)
{
    JceLodGroup g;
    jce_lod_setup(&g, &high_mesh, 10.0f, &mid_mesh, 30.0f, &low_mesh, 80.0f);
    g.hysteresis = 0.10f;
    TEST_ASSERT_EQUAL_INT(1, jce_lod_pick(&g, 20.0f, 1));
    TEST_ASSERT_EQUAL_INT(1, jce_lod_pick(&g, 31.0f, 1));
}

static void test_pick_cull_only_when_last_threshold_finite(void)
{
    JceLodGroup g;
    jce_lod_init(&g);
    g.levels[0].mesh = &high_mesh;
    g.levels[0].distance = 10.0f;
    g.levels[1].mesh = &low_mesh;
    g.levels[1].distance = 50.0f;
    g.count = 2;
    g.hysteresis = 0.10f;
    TEST_ASSERT_EQUAL_INT(-1, jce_lod_pick(&g, 1000.0f, 1));
    g.levels[1].distance = FLT_MAX;
    TEST_ASSERT_EQUAL_INT(1, jce_lod_pick(&g, 1000.0f, 1));
}

static void test_pick_invalid_hysteresis_falls_back_to_default(void)
{
    JceLodGroup g;
    jce_lod_setup(&g, &high_mesh, 10.0f, &mid_mesh, 30.0f, NULL, 0.0f);
    g.hysteresis = -1.0f;
    TEST_ASSERT_EQUAL_INT(0, jce_lod_pick(&g, 10.4f, 0));
    TEST_ASSERT_EQUAL_INT(1, jce_lod_pick(&g, 10.6f, 0));
    g.hysteresis = 0.9f;
    TEST_ASSERT_EQUAL_INT(1, jce_lod_pick(&g, 10.6f, 0));
}

static void test_pick_invalid_prev_treated_as_first_call(void)
{
    JceLodGroup g;
    jce_lod_setup(&g, &high_mesh, 10.0f, &mid_mesh, 30.0f, NULL, 0.0f);
    TEST_ASSERT_EQUAL_INT(0, jce_lod_pick(&g, 5.0f, 99));
    TEST_ASSERT_EQUAL_INT(1, jce_lod_pick(&g, 25.0f, -42));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_resets_and_defaults);
    RUN_TEST(test_setup_packs_levels_and_caps_last);
    RUN_TEST(test_setup_skips_null_slots);
    RUN_TEST(test_pick_returns_minus_one_for_empty);
    RUN_TEST(test_pick_nominal_no_prev);
    RUN_TEST(test_pick_step_down_requires_hysteresis);
    RUN_TEST(test_pick_step_up_requires_hysteresis);
    RUN_TEST(test_pick_dead_zone_keeps_prev);
    RUN_TEST(test_pick_cull_only_when_last_threshold_finite);
    RUN_TEST(test_pick_invalid_hysteresis_falls_back_to_default);
    RUN_TEST(test_pick_invalid_prev_treated_as_first_call);
    return UNITY_END();
}
