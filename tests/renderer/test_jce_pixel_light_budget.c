/*
 * test_jce_pixel_light_budget.c — Unity's QualitySettings.pixelLightCount.
 *
 * Project Settings > Quality has carried a per-level `pixel_light_count` for as
 * long as the tab has existed.  Nothing accepted it: no engine call took the
 * number and jce_editor_effective_render_settings never exported it, so a
 * designer capping lights for a low-end target changed a value in a file and
 * the renderer selected exactly as many lights as before.
 *
 * The compile-time ceilings (JCE_MAX_POINT_LIGHTS 16, JCE_MAX_SPOT_LIGHTS 4)
 * are what the shader's uniform arrays hold.  The budget is the RUNTIME cap
 * under them, spent across BOTH kinds highest-score-first -- which is the whole
 * point: a budget of 4 must keep the four lights that matter most to this
 * camera, not the first four the scene declares.
 *
 * THE LOAD-BEARING CASE IS THE DEFAULT.  budget 0 must reproduce the old
 * selection light-for-light, or this lands a silent visual change in every
 * project that never opened the Quality tab.  The argument for that is that
 * two independent top-N passes and one merged pass with an unreachable budget
 * select the same set -- and an argument is not a measurement, so it is
 * asserted here.
 *
 * sr_select_lights only reads and writes four fields of JceSceneRenderer, so a
 * zeroed one is enough; no renderer, no bgfx, no window.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "middleware/scene/jce_sr_internal.h"

#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_lighting_system.h>

#include <stdlib.h>
#include <string.h>

static JceScene         *s_scene;
static JceSceneRenderer *s_sr;
static JceCamera        *s_cam;
static EntityList        s_list;

void setUp(void)
{
    s_scene = jce_scene_create();
    /* Only frame_sel_* is touched; zeroed is a valid renderer for this call. */
    s_sr    = (JceSceneRenderer *)calloc(1, sizeof(JceSceneRenderer));
    JceCameraDesc cd;
    memset(&cd, 0, sizeof cd);
    s_cam   = jce_camera_create(&cd);
    memset(&s_list, 0, sizeof s_list);
    jce_lighting_set_pixel_light_count(0);   /* the default, every time */
}

void tearDown(void)
{
    jce_camera_destroy(s_cam);
    free(s_sr);
    jce_scene_destroy(s_scene);
    jce_lighting_set_pixel_light_count(0);
}

/* A light at distance `d` on +X.  score = intensity / d², so a larger d is a
 * lower score and the ordering is exactly the caller's to choose. */
static JceEntity add_point(float d)
{
    JceEntity e = jce_scene_create_entity(s_scene, "pt");
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.position = jce_v3(d, 0.0f, 0.0f);
    t.rotation = jce_q_identity();
    t.scale    = jce_v3(1.0f, 1.0f, 1.0f);
    jce_scene_set_transform(s_scene, e, &t);
    JcePointLight pl;
    memset(&pl, 0, sizeof pl);
    pl.intensity = 1.0f;
    pl.radius    = 100.0f;
    jce_scene_set_point_light(s_scene, e, &pl);
    jce_scene_set_component_enabled(s_scene, e, JCE_COMP_FLAG_POINT_LIGHT, true);
    return e;
}

static JceEntity add_spot(float d)
{
    JceEntity e = jce_scene_create_entity(s_scene, "sp");
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.position = jce_v3(d, 0.0f, 0.0f);
    t.rotation = jce_q_identity();
    t.scale    = jce_v3(1.0f, 1.0f, 1.0f);
    jce_scene_set_transform(s_scene, e, &t);
    JceSpotLight sl;
    memset(&sl, 0, sizeof sl);
    sl.intensity = 1.0f;
    sl.radius    = 100.0f;
    sl.inner_cone_cos = 0.9f;
    sl.outer_cone_cos = 0.8f;
    jce_scene_set_spot_light(s_scene, e, &sl);
    jce_scene_set_component_enabled(s_scene, e, JCE_COMP_FLAG_SPOT_LIGHT, true);
    return e;
}

static void select_now(void)
{
    sr_select_lights(s_sr, s_scene, &s_list, s_cam);
}

static void test_the_default_selects_exactly_what_it_always_did(void)
{
    /* THE CASE THIS CHANGE RESTS ON.  Twenty point lights and six spots, no
     * budget: the ceilings alone decide, and they must decide exactly as two
     * independent top-N passes did.  If this moves, every project that never
     * opened the Quality tab gets a different picture. */
    for (int i = 0; i < 20; ++i) add_point(1.0f + (float)i);
    for (int i = 0; i < 6;  ++i) add_spot(1.5f + (float)i);

    select_now();
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_MAX_POINT_LIGHTS, s_sr->frame_sel_point_n,
        "no budget means the compile-time point ceiling and nothing else");
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_MAX_SPOT_LIGHTS, s_sr->frame_sel_spot_n,
        "...and the spot ceiling, independently");

    /* THE COUNT IS NOT THE CLAIM.  A merge that changed WHICH sixteen points
     * were kept would still report sixteen, so assert the SET: the points were
     * added at distances 1..20 and score is 1/d², so the sixteen selected must
     * be exactly the sixteen nearest -- the first sixteen created. */
    for (int i = 0; i < JCE_MAX_POINT_LIGHTS; ++i) {
        bool found = false;
        for (int j = 0; j < s_sr->frame_sel_point_n && !found; ++j) {
            JceTransform *xf = jce_scene_get_transform(
                s_scene, s_sr->frame_sel_point[j]);
            if (xf && xf->position.x == (1.0f + (float)i)) found = true;
        }
        TEST_ASSERT_TRUE_MESSAGE(found,
            "every one of the sixteen NEAREST point lights must be selected -- "
            "the same set two independent top-N passes produced");
    }
}

static void test_a_budget_caps_the_combined_count(void)
{
    for (int i = 0; i < 20; ++i) add_point(1.0f + (float)i);
    for (int i = 0; i < 6;  ++i) add_spot(1.5f + (float)i);

    jce_lighting_set_pixel_light_count(5);
    select_now();
    TEST_ASSERT_EQUAL_INT_MESSAGE(5,
        s_sr->frame_sel_point_n + s_sr->frame_sel_spot_n,
        "the budget is COMBINED across point and spot, which is what Unity "
        "counts -- five per-kind would be ten lights");
}

static void test_the_budget_is_spent_by_score_not_by_kind(void)
{
    /* Put the spots NEARER than every point.  A per-kind split would still
     * hand most of the budget to points; spending by score must give it to the
     * spots, which are the lights actually lighting this camera's view. */
    for (int i = 0; i < 8; ++i) add_point(50.0f + (float)i);   /* far */
    for (int i = 0; i < 3; ++i) add_spot(1.0f + (float)i);     /* near */

    jce_lighting_set_pixel_light_count(3);
    select_now();
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, s_sr->frame_sel_spot_n,
        "the three nearest lights are the spots, so the whole budget is theirs");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_sr->frame_sel_point_n,
        "and no far point light displaces a near spot");
}

static void test_a_budget_of_one_keeps_the_nearest_light(void)
{
    const JceEntity near_pt = add_point(1.0f);
    add_point(10.0f);
    add_spot(20.0f);

    jce_lighting_set_pixel_light_count(1);
    select_now();
    TEST_ASSERT_EQUAL_INT(1, s_sr->frame_sel_point_n);
    TEST_ASSERT_EQUAL_INT(0, s_sr->frame_sel_spot_n);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)near_pt,
        (uint64_t)s_sr->frame_sel_point[0],
        "a budget of one must keep the single most important light, not the "
        "first one the scene happened to declare");
}

static void test_a_budget_above_the_ceilings_changes_nothing(void)
{
    for (int i = 0; i < 20; ++i) add_point(1.0f + (float)i);
    for (int i = 0; i < 6;  ++i) add_spot(1.5f + (float)i);

    jce_lighting_set_pixel_light_count(1000);
    select_now();
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_MAX_POINT_LIGHTS, s_sr->frame_sel_point_n,
        "a budget larger than the hardware ceiling cannot raise it");
    TEST_ASSERT_EQUAL_INT(JCE_MAX_SPOT_LIGHTS, s_sr->frame_sel_spot_n);
}

static void test_fewer_lights_than_the_budget_selects_all_of_them(void)
{
    add_point(1.0f);
    add_spot(2.0f);
    jce_lighting_set_pixel_light_count(8);
    select_now();
    TEST_ASSERT_EQUAL_INT(1, s_sr->frame_sel_point_n);
    TEST_ASSERT_EQUAL_INT(1, s_sr->frame_sel_spot_n);
}

static void test_the_knob_clamps_and_round_trips(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, jce_lighting_get_pixel_light_count(),
        "no budget is the default -- a cap nobody asked for would change every "
        "existing project's lighting");
    jce_lighting_set_pixel_light_count(6);
    TEST_ASSERT_EQUAL_INT(6, jce_lighting_get_pixel_light_count());
    jce_lighting_set_pixel_light_count(-3);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, jce_lighting_get_pixel_light_count(),
        "a negative budget is 'no budget', not a negative count");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_default_selects_exactly_what_it_always_did);
    RUN_TEST(test_a_budget_caps_the_combined_count);
    RUN_TEST(test_the_budget_is_spent_by_score_not_by_kind);
    RUN_TEST(test_a_budget_of_one_keeps_the_nearest_light);
    RUN_TEST(test_a_budget_above_the_ceilings_changes_nothing);
    RUN_TEST(test_fewer_lights_than_the_budget_selects_all_of_them);
    RUN_TEST(test_the_knob_clamps_and_round_trips);
    return UNITY_END();
}
