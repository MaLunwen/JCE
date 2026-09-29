/*
 * test_jce_camera_clear_mode.c — JceCameraComponent.clear_mode decides two
 * separate things: whether the sky is drawn, and what the target KEEPS from
 * the previous frame.
 *
 * The field was authored, serialised and shown in the Inspector behind an
 * unwired badge, and read by nothing: every camera cleared the same way no
 * matter what the dropdown said.
 *
 * THE ASSERTION HAS TO CROSS THE SEAM.  A test that only checks the pose carry
 * and a lookup table stays green when the host line that applies the answer is
 * deleted -- which is precisely the built-but-unwired shape this whole campaign
 * exists to find, and how jce_scene_camera_apply_primary sat with zero callers
 * in the whole tree until 694b77d3.  So the cases below run
 * jce_scene_camera_resolve_primary on a REAL scene and drive the resolved
 * clear_mode through jce_scene_camera_clear_draws_skybox, which is the exact
 * expression both hosts assign to JceSceneRenderConfig.draw_skybox.  A gate
 * (check_camera_clear_single_source.py) holds the last two inches.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_camera.h>

#include <stdbool.h>
#include <string.h>

/* Build a scene whose single primary camera authors `mode`, resolve it, and
 * return the answer a host would assign to cfg.draw_skybox. */
static bool sky_for_mode(uint8_t mode, bool *out_resolved, uint8_t *out_carried)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity e = jce_scene_create_entity(s, "Cam");
    JceTransform tf;
    memset(&tf, 0, sizeof tf);
    tf.rotation.w = 1.0f;
    tf.scale = jce_v3(1.0f, 1.0f, 1.0f);
    tf.position = jce_v3(0.0f, 2.0f, 5.0f);
    jce_scene_set_transform(s, e, &tf);

    JceCameraComponent cam;
    memset(&cam, 0, sizeof cam);
    cam.is_primary  = true;
    cam.fov_deg     = 60.0f;
    cam.near_plane  = 0.1f;
    cam.far_plane   = 1000.0f;
    cam.clear_mode  = mode;
    jce_scene_set_camera(s, e, &cam);

    JceSceneCameraPose pose;
    memset(&pose, 0, sizeof pose);
    const bool ok = jce_scene_camera_resolve_primary(s, &pose) ==
                    JCE_SCENE_CAMERA_RESOLVE_OK;
    if (out_resolved) *out_resolved = ok;
    if (out_carried)  *out_carried  = pose.clear_mode;

    jce_scene_destroy(s);
    return jce_scene_camera_clear_draws_skybox(pose.clear_mode);
}

static void test_skybox_is_the_default_and_keeps_the_sky(void)
{
    /* 0 == JCE_CAMERA_CLEAR_SKYBOX, what every scene written before this
     * carries and what jce_scene_render_config_default() already does. */
    bool ok = false; uint8_t carried = 255;
    const bool sky = sky_for_mode((uint8_t)JCE_CAMERA_CLEAR_SKYBOX,
                                  &ok, &carried);
    TEST_ASSERT_TRUE_MESSAGE(ok, "one enabled primary camera must resolve");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, carried,
        "the pose must carry the authored mode out of the component");
    TEST_ASSERT_TRUE_MESSAGE(sky,
        "the default must keep today's behaviour exactly -- otherwise wiring "
        "this field changes every scene that never authored it");
}

static void test_solid_colour_turns_the_sky_off(void)
{
    bool ok = false; uint8_t carried = 255;
    const bool sky = sky_for_mode((uint8_t)JCE_CAMERA_CLEAR_COLOR,
                                  &ok, &carried);
    TEST_ASSERT_TRUE_MESSAGE(ok, "the camera must still resolve");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, carried, "the pose carries mode 1");
    TEST_ASSERT_FALSE_MESSAGE(sky,
        "this is the whole feature: an authored Solid Color must stop the sky "
        "pass, and before this the dropdown decided nothing");
}

static void test_depth_only_and_nothing_also_turn_the_sky_off(void)
{
    /* This case used to assert that "sky off" was ALL these two modes did,
     * and gave the reason: the engine could not safely keep a buffer, because
     * the offscreen target's attachments are created with `NULL, 0` and
     * keeping them would present uninitialised memory.  The renderer now
     * overrides the keep on the one frame where that is true, so the reason is
     * gone and the modes do both halves.  The sky half is unchanged and is
     * still asserted here -- a feature landing must not quietly move the part
     * that already worked. */
    bool ok = false;
    TEST_ASSERT_FALSE_MESSAGE(
        sky_for_mode((uint8_t)JCE_CAMERA_CLEAR_DEPTH_ONLY, &ok, NULL),
        "Depth Only must resolve to sky off");
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FALSE_MESSAGE(
        sky_for_mode((uint8_t)JCE_CAMERA_CLEAR_NOTHING, &ok, NULL),
        "Nothing must resolve to sky off");
    TEST_ASSERT_TRUE(ok);
}

/* ── the other half: what the target keeps ─────────────── */

/* Same seam-crossing shape as sky_for_mode: author the mode on a REAL camera,
 * resolve it, and drive the CARRIED value -- not the literal -- through the
 * mapping, so deleting the line that carries clear_mode onto the pose fails
 * here instead of staying green on a lookup table. */
static void keeps_for_mode(uint8_t mode, bool *out_color, bool *out_depth)
{
    bool resolved = false;
    uint8_t carried = 255;
    (void)sky_for_mode(mode, &resolved, &carried);
    TEST_ASSERT_TRUE_MESSAGE(resolved, "the camera must resolve");
    jce_scene_camera_clear_keeps(carried, out_color, out_depth);
}

static void test_the_two_clearing_modes_keep_nothing(void)
{
    /* SKYBOX and COLOR are the modes every existing scene carries.  If either
     * started keeping a buffer, every scene written before this changed
     * appearance -- which is the one outcome a parity feature must not have. */
    bool c = true, d = true;
    keeps_for_mode((uint8_t)JCE_CAMERA_CLEAR_SKYBOX, &c, &d);
    TEST_ASSERT_FALSE_MESSAGE(c, "Skybox must clear colour");
    TEST_ASSERT_FALSE_MESSAGE(d, "Skybox must clear depth");
    c = true; d = true;
    keeps_for_mode((uint8_t)JCE_CAMERA_CLEAR_COLOR, &c, &d);
    TEST_ASSERT_FALSE_MESSAGE(c, "Solid Color must clear colour");
    TEST_ASSERT_FALSE_MESSAGE(d, "Solid Color must clear depth");
}

static void test_depth_only_keeps_colour_and_nothing_else(void)
{
    /* The mode's whole content: this camera's geometry draws over last frame's
     * picture WITHOUT being occluded by last frame's depth.  Keeping depth too
     * would be the other mode, and the two must not collapse into one. */
    bool c = false, d = true;
    keeps_for_mode((uint8_t)JCE_CAMERA_CLEAR_DEPTH_ONLY, &c, &d);
    TEST_ASSERT_TRUE_MESSAGE(c, "Depth Only must KEEP colour");
    TEST_ASSERT_FALSE_MESSAGE(d,
        "Depth Only must CLEAR depth -- keeping it is the Nothing mode, and a "
        "collapse of the two is invisible in any picture where nothing moves");
}

static void test_nothing_keeps_both(void)
{
    bool c = false, d = false;
    keeps_for_mode((uint8_t)JCE_CAMERA_CLEAR_NOTHING, &c, &d);
    TEST_ASSERT_TRUE_MESSAGE(c, "Don't Clear must keep colour");
    TEST_ASSERT_TRUE_MESSAGE(d, "Don't Clear must keep depth");
}

static void test_an_out_of_range_mode_keeps_nothing(void)
{
    /* Symmetric with the sky half: a hand-edited clearMode: 9 must fall back
     * to what every scene already gets, not to the last case written. */
    bool c = true, d = true;
    jce_scene_camera_clear_keeps(9, &c, &d);
    TEST_ASSERT_FALSE_MESSAGE(c, "an unknown mode must clear colour");
    TEST_ASSERT_FALSE_MESSAGE(d, "an unknown mode must clear depth");
}

static void test_both_out_pointers_are_optional(void)
{
    /* A caller that wants one half must not have to invent storage for the
     * other, and must not crash for asking. */
    bool only = false;
    jce_scene_camera_clear_keeps((uint8_t)JCE_CAMERA_CLEAR_NOTHING, &only, NULL);
    TEST_ASSERT_TRUE(only);
    only = false;
    jce_scene_camera_clear_keeps((uint8_t)JCE_CAMERA_CLEAR_NOTHING, NULL, &only);
    TEST_ASSERT_TRUE(only);
    jce_scene_camera_clear_keeps((uint8_t)JCE_CAMERA_CLEAR_NOTHING, NULL, NULL);
}

static void test_an_out_of_range_mode_keeps_the_default(void)
{
    /* A hand-edited clearMode: 9 must not mean "no sky".  Out of range falls
     * back to the behaviour every scene already has, not to the newest arm. */
    TEST_ASSERT_TRUE_MESSAGE(sky_for_mode(9, NULL, NULL),
        "an unknown mode must keep the default, not the last case written");
}

static void test_a_scene_with_no_camera_is_untouched(void)
{
    JceScene *s = jce_scene_create();
    JceEntity e = jce_scene_create_entity(s, "NotACamera");
    JceTransform tf;
    memset(&tf, 0, sizeof tf);
    tf.rotation.w = 1.0f;
    jce_scene_set_transform(s, e, &tf);

    JceSceneCameraPose pose;
    memset(&pose, 0, sizeof pose);
    TEST_ASSERT_TRUE_MESSAGE(
        jce_scene_camera_resolve_primary(s, &pose) !=
        JCE_SCENE_CAMERA_RESOLVE_OK,
        "no primary camera must not resolve");
    jce_scene_destroy(s);
    /* Both hosts leave draw_skybox at the engine default on this branch, so
     * the overwhelming majority of scenes are byte-identical. */
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_skybox_is_the_default_and_keeps_the_sky);
    RUN_TEST(test_solid_colour_turns_the_sky_off);
    RUN_TEST(test_depth_only_and_nothing_also_turn_the_sky_off);
    RUN_TEST(test_an_out_of_range_mode_keeps_the_default);
    RUN_TEST(test_a_scene_with_no_camera_is_untouched);
    RUN_TEST(test_the_two_clearing_modes_keep_nothing);
    RUN_TEST(test_depth_only_keeps_colour_and_nothing_else);
    RUN_TEST(test_nothing_keeps_both);
    RUN_TEST(test_an_out_of_range_mode_keeps_nothing);
    RUN_TEST(test_both_out_pointers_are_optional);
    return UNITY_END();
}
