/*
 * test_jce_comp_set_partial.c  comp_set is a PARTIAL update.
 *
 * jce_scene_component_apply_json is what the scripting API's comp_set() lands
 * on.  It used to hand the caller's JSON fragment straight to the component's
 * parse function, and every parse_* opens with `memset(&x, 0, sizeof x)` and
 * then fills each field from j_num/j_bool with a LITERAL default.  So a script
 * naming one key silently reverted every other field of that component to its
 * parse-time default:
 *
 *     comp_set(e, "CanvasGroup", '{"alpha": 0.5}')
 *
 * -- the obvious way to fade a panel -- also rewrote interactable,
 * blocks_raycasts and ignore_parent_groups, discarding what the scene authored.
 * Not a UI defect: EVERY component behaved this way.
 *
 * The sibling entry point jce_scene_rendering_apply_json had always merged, so
 * the two halves of the same scripting API disagreed about what "set" means.
 *
 * These pin the merge on three shapes: a bool that must survive, a float that
 * must survive, and a string that must survive -- strings are the case a
 * numeric-only spot check would miss.
 */

#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>

#include <string.h>

/* Engine-internal (jce_scene_internal.h); declared here so the test does not
 * need the private include path. */
bool  jce_scene_component_apply_json(JceScene *s, JceEntity e,
                                     const char *type, const char *json);
char *jce_scene_component_to_json(JceScene *s, JceEntity e, const char *type);

void setUp(void)    {}
void tearDown(void) {}

/* A bool and a bool: alpha is patched, the other three must not move. */
static void test_a_one_key_patch_leaves_the_rest_of_the_component_alone(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "Panel");

    JceCanvasGroupComponent cg;
    memset(&cg, 0, sizeof cg);
    cg.alpha                = 1.0f;
    cg.interactable         = false;   /* NOT the parse default (true)  */
    cg.blocks_raycasts      = false;   /* NOT the parse default (true)  */
    cg.ignore_parent_groups = true;    /* NOT the parse default (false) */
    jce_scene_set_canvas_group(s, e, &cg);

    TEST_ASSERT_TRUE(jce_scene_component_apply_json(
        s, e, "CanvasGroup", "{\"alpha\":0.5}"));

    JceCanvasGroupComponent *got = jce_scene_get_canvas_group(s, e);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.5f, got->alpha);   /* patched      */
    TEST_ASSERT_FALSE(got->interactable);                  /* survived     */
    TEST_ASSERT_FALSE(got->blocks_raycasts);               /* survived     */
    TEST_ASSERT_TRUE(got->ignore_parent_groups);           /* survived     */

    jce_scene_destroy(s);
}

/* A string field: the shape a numeric spot check would miss. */
static void test_a_patch_does_not_blank_an_unmentioned_string(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "Btn");

    JceUIButtonComponent bt;
    memset(&bt, 0, sizeof bt);
    bt.interactable  = true;
    bt.fade_duration = 0.25f;
    snprintf(bt.on_click_handler, sizeof bt.on_click_handler, "%s", "on_play");
    jce_scene_set_ui_button(s, e, &bt);

    TEST_ASSERT_TRUE(jce_scene_component_apply_json(
        s, e, "UIButton", "{\"fadeDuration\":0.75}"));

    JceUIButtonComponent *got = jce_scene_get_ui_button(s, e);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.75f, got->fade_duration);
    TEST_ASSERT_EQUAL_STRING("on_play", got->on_click_handler);

    jce_scene_destroy(s);
}

/* An ABSENT component still takes the patch whole -- that is how a script adds
 * one, and it must not regress into "merge onto nothing produces nothing". */
static void test_comp_set_on_an_absent_component_still_adds_it(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "New");

    TEST_ASSERT_FALSE(jce_scene_has_canvas_group(s, e));
    TEST_ASSERT_TRUE(jce_scene_component_apply_json(
        s, e, "CanvasGroup", "{\"alpha\":0.25}"));

    JceCanvasGroupComponent *got = jce_scene_get_canvas_group(s, e);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.25f, got->alpha);
    /* and the unmentioned fields take their documented parse defaults */
    TEST_ASSERT_TRUE(got->interactable);
    TEST_ASSERT_TRUE(got->blocks_raycasts);

    jce_scene_destroy(s);
}

/* UIProgressBar has no dedicated script accessor -- the ui_get/ui_set family
 * covers slider, toggle and text only.  Its `value` is documented "read-only;
 * gameplay sets", so the question that decides whether that is a capability
 * gap or an ergonomics gap is whether the GENERIC comp_set can drive it.
 *
 * It can, but only now: before the merge landed, a script setting `value` also
 * reset min_value, max_value, direction, both colours and fill_sprite to their
 * parse defaults, which is not a usable way to drive a progress bar every
 * frame.  This pins the working path so the dedicated accessor is a
 * convenience rather than the only route. */
static void test_a_progress_bar_can_be_driven_through_the_generic_setter(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "Bar");

    JceUIProgressBarComponent pb;
    memset(&pb, 0, sizeof pb);
    pb.value     = 0.0f;
    pb.min_value = 0.0f;
    pb.max_value = 200.0f;      /* NOT the parse default (1.0) */
    pb.direction = 3;           /* NOT the parse default (0)   */
    jce_scene_set_ui_progress_bar(s, e, &pb);

    TEST_ASSERT_TRUE(jce_scene_component_apply_json(
        s, e, "UIProgressBar", "{\"value\":120.0}"));

    JceUIProgressBarComponent *got = jce_scene_get_ui_progress_bar(s, e);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 120.0f, got->value);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 200.0f, got->max_value);  /* survived */
    TEST_ASSERT_EQUAL_INT(3, got->direction);                 /* survived */

    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_one_key_patch_leaves_the_rest_of_the_component_alone);
    RUN_TEST(test_a_patch_does_not_blank_an_unmentioned_string);
    RUN_TEST(test_comp_set_on_an_absent_component_still_adds_it);
    RUN_TEST(test_a_progress_bar_can_be_driven_through_the_generic_setter);
    return UNITY_END();
}
