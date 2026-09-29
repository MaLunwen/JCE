/*
 * test_jce_vcam_by_name.c — "cut to the camera called BossIntro".
 *
 * JceVirtualCameraComponent.vcam_name was authored, serialised and shown in
 * the editor's VCam Manager, and NOTHING under engine/src ever looked at it:
 * jce_vcam_system_evaluate picks the highest-priority active camera, so the
 * name was a label in a panel.  The one thing a cutscene, a trigger or a scene
 * authored through the SDK actually wants to say could not be expressed.
 *
 * WHAT THESE CASES PIN, in the order that matters:
 *
 *   1. The override BEATS priority.  Two cameras, the named one deliberately
 *      the LOWER priority, so "it won" cannot be satisfied by the old rule.
 *   2. It does not resurrect.  Naming an inactive camera must not activate it
 *      -- an author who unticked Active said something -- so selection falls
 *      back to priority rather than to nothing.
 *   3. Clearing hands the decision back, and the picture returns to exactly
 *      what it was in case 1's control.
 *   4. reset() drops it.  The override may name a camera that has not
 *      streamed in yet, which is precisely why it must not survive a scene
 *      change: a cut requested in the last level would sit waiting to hijack
 *      the first same-named camera in the next one.
 *
 * NOT ASSERTED: that the authored components are unchanged is checked as a
 * value comparison, not inferred from "we did not write any code that would".
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_vcam_system.h>

#include "unity.h"

#include <string.h>

void setUp(void)    { jce_vcam_system_reset(); }
void tearDown(void) { jce_vcam_system_reset(); }

static JceEntity make_vcam(JceScene *s, const char *name, int32_t prio,
                           bool active, float x)
{
    JceEntity                 e = jce_scene_create_entity(s, name);
    JceTransform              t;
    JceVirtualCameraComponent vc;

    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);

    memset(&vc, 0, sizeof vc);
    snprintf(vc.vcam_name, sizeof vc.vcam_name, "%s", name);
    vc.priority = prio;
    vc.active   = active;
    vc.fov_deg  = 60.0f;
    vc.damping  = 0.0f;           /* snap: one evaluate is the answer */
    /* The x position is the tell: which camera won is read off the pose. */
    vc.position[0] = x;
    jce_scene_set_virtual_camera(s, e, &vc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_VIRTUAL_CAMERA, true);
    return e;
}

/* One evaluate with damping 0 puts the winner's position straight into `out`. */
static float live_x(JceScene *s, bool *out_has_active)
{
    JceVcamOutput out;
    memset(&out, 0, sizeof out);
    jce_vcam_system_evaluate(s, 1.0f / 60.0f, &out, out_has_active);
    return out.position[0];
}

static void test_the_name_beats_priority(void)
{
    JceScene *s = jce_scene_create();
    bool      has = false;
    TEST_ASSERT_NOT_NULL(s);

    make_vcam(s, "wide",      10, true, 100.0f);   /* higher priority */
    make_vcam(s, "BossIntro",  1, true,   7.0f);   /* the one we will name */

    /* THE CONTROL.  Priority alone must pick "wide", or case 1 below would be
     * satisfied by the named camera simply being the only candidate. */
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(100.0f, live_x(s, &has),
        "priority alone did not pick the higher-priority camera");
    TEST_ASSERT_TRUE(has);

    TEST_ASSERT_TRUE_MESSAGE(jce_vcam_system_set_active_by_name(s, "BossIntro"),
        "the named camera exists and is active, so the cut must be taken");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(7.0f, live_x(s, &has),
        "the named camera did not win: vcam_name still decides nothing");

    /* The authored data is untouched -- compared, not assumed. */
    {
        JceEntity e = jce_vcam_find_by_name(s, "BossIntro");
        JceVirtualCameraComponent *vc = jce_scene_get_virtual_camera(s, e);
        TEST_ASSERT_NOT_NULL(vc);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(1, vc->priority,
            "the override raised the authored priority; Ctrl+S would then bake "
            "a cutscene's camera choice into the level");
    }

    jce_scene_destroy(s);
}

static void test_naming_an_inactive_camera_does_not_resurrect_it(void)
{
    JceScene *s = jce_scene_create();
    bool      has = false;
    TEST_ASSERT_NOT_NULL(s);

    make_vcam(s, "wide",   10, true,  100.0f);
    make_vcam(s, "unused",  1, false,   7.0f);   /* Active unticked */

    TEST_ASSERT_FALSE_MESSAGE(jce_vcam_system_set_active_by_name(s, "unused"),
        "an inactive camera must report the cut as NOT taken");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(100.0f, live_x(s, &has),
        "naming an inactive camera activated it -- 'cut to it' cannot also "
        "mean 'and turn it on'; an author who unticked Active said something");
    TEST_ASSERT_TRUE_MESSAGE(has,
        "selection must fall back to priority, not to no camera at all");

    jce_scene_destroy(s);
}

static void test_clearing_hands_the_decision_back(void)
{
    JceScene *s = jce_scene_create();
    bool      has = false;
    TEST_ASSERT_NOT_NULL(s);

    make_vcam(s, "wide",      10, true, 100.0f);
    make_vcam(s, "BossIntro",  1, true,   7.0f);

    TEST_ASSERT_TRUE(jce_vcam_system_set_active_by_name(s, "BossIntro"));
    TEST_ASSERT_EQUAL_FLOAT(7.0f, live_x(s, &has));

    jce_vcam_system_set_active_by_name(s, NULL);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", jce_vcam_system_get_active_name(),
        "NULL must clear the override, not store a literal");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(100.0f, live_x(s, &has),
        "clearing did not return the decision to priority");

    jce_scene_destroy(s);
}

static void test_reset_drops_the_override(void)
{
    JceScene *s = jce_scene_create();
    bool      has = false;
    TEST_ASSERT_NOT_NULL(s);

    make_vcam(s, "wide",      10, true, 100.0f);
    make_vcam(s, "BossIntro",  1, true,   7.0f);

    TEST_ASSERT_TRUE(jce_vcam_system_set_active_by_name(s, "BossIntro"));
    TEST_ASSERT_EQUAL_FLOAT(7.0f, live_x(s, &has));

    /* A scene change.  The override is allowed to name a camera that is not
     * loaded yet, so surviving this would let the last level's cut hijack the
     * next level's same-named camera. */
    jce_vcam_system_reset();
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", jce_vcam_system_get_active_name(),
        "reset() left the named-camera override standing");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(100.0f, live_x(s, &has),
        "the override survived a reset");

    jce_scene_destroy(s);
}

static void test_find_by_name_is_exact_and_says_no(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity boss = make_vcam(s, "BossIntro", 1, true, 7.0f);

    TEST_ASSERT_EQUAL_UINT64(boss, jce_vcam_find_by_name(s, "BossIntro"));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u, jce_vcam_find_by_name(s, "bossintro"),
        "matching is exact and case-sensitive, like every other name lookup "
        "in this scene API -- a near miss must read as absent, not as a hit");
    TEST_ASSERT_EQUAL_UINT64(0u, jce_vcam_find_by_name(s, "nope"));
    TEST_ASSERT_EQUAL_UINT64(0u, jce_vcam_find_by_name(s, ""));
    TEST_ASSERT_EQUAL_UINT64(0u, jce_vcam_find_by_name(NULL, "BossIntro"));

    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_name_beats_priority);
    RUN_TEST(test_naming_an_inactive_camera_does_not_resurrect_it);
    RUN_TEST(test_clearing_hands_the_decision_back);
    RUN_TEST(test_reset_drops_the_override);
    RUN_TEST(test_find_by_name_is_exact_and_says_no);
    return UNITY_END();
}
