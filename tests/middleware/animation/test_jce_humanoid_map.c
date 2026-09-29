/*
 * test_jce_humanoid_map.c -- the humanoid name matcher, against the joint
 * names of the FOUR rigged models actually in this tree.
 *
 * The names below are not invented.  They were read out of the skins of
 * CesiumMan.glb, RiggedFigure.glb, PSX_BagMan.glb and UAL1_Standard.glb, which
 * between them use the Khronos sample convention (`torso_joint_2`,
 * `arm_joint_L_3`), a Blender one (`UPPER ARM.L`, `HIP CONTROLLER`) and
 * Unreal's (`spine_02`, `upperarm_l`, `calf_r`).  A matcher tested against
 * names someone made up for the test would pass and then meet a real rig.
 */
#include "unity.h"

#include <jce/middleware/animation/jce_humanoid.h>

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define MAPS(name, bone) \
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)(bone), \
        (int)jce_humanoid_bone_from_joint_name(name), \
        "'" name "' did not map to " #bone)

/* ── Unreal Engine naming (UAL1_Standard.glb, 65 joints, 45 clips) ── */
static void test_unreal_naming(void)
{
    MAPS("pelvis",     JCE_HB_HIPS);
    MAPS("spine_01",   JCE_HB_SPINE);
    MAPS("spine_02",   JCE_HB_CHEST);
    MAPS("spine_03",   JCE_HB_UPPER_CHEST);
    MAPS("neck_01",    JCE_HB_NECK);
    MAPS("Head",       JCE_HB_HEAD);
    MAPS("clavicle_l", JCE_HB_LEFT_SHOULDER);
    MAPS("clavicle_r", JCE_HB_RIGHT_SHOULDER);
    MAPS("upperarm_l", JCE_HB_LEFT_UPPER_ARM);
    MAPS("lowerarm_r", JCE_HB_RIGHT_LOWER_ARM);
    MAPS("hand_l",     JCE_HB_LEFT_HAND);
    MAPS("thigh_r",    JCE_HB_RIGHT_UPPER_LEG);
    MAPS("calf_l",     JCE_HB_LEFT_LOWER_LEG);
    MAPS("foot_r",     JCE_HB_RIGHT_FOOT);
    MAPS("ball_l",     JCE_HB_LEFT_TOES);

    /* And the sixty joints that are NOT humanoid roles stay unmapped.  This is
     * the half that matters: a matcher generous enough to catch `hand_l` and
     * also catch `index_01_l` would drive the hand with a finger. */
    MAPS("root",             JCE_HB_COUNT);
    MAPS("index_01_l",       JCE_HB_COUNT);
    MAPS("thumb_03_r",       JCE_HB_COUNT);
    MAPS("ball_leaf_l",      JCE_HB_LEFT_TOES);   /* a leaf DOES look like one;
                                                   * first-wins in the map
                                                   * build keeps `ball_l` */
    MAPS("middle_04_leaf_r", JCE_HB_COUNT);
}

/* ── Blender-ish naming (PSX_BagMan.glb) ───────────────────────────── */
static void test_blender_naming(void)
{
    MAPS("HIP CONTROLLER", JCE_HB_HIPS);
    MAPS("SPINE",          JCE_HB_SPINE);
    MAPS("TORSO",          JCE_HB_CHEST);
    MAPS("HEAD",           JCE_HB_HEAD);
    MAPS("SHOULDER.L",     JCE_HB_LEFT_SHOULDER);
    MAPS("UPPER ARM.L",    JCE_HB_LEFT_UPPER_ARM);
    MAPS("FOREARM.R",      JCE_HB_RIGHT_LOWER_ARM);
    MAPS("HAND.R",         JCE_HB_RIGHT_HAND);
    MAPS("UPPER LEG.L",    JCE_HB_LEFT_UPPER_LEG);
    MAPS("SHIN.R",         JCE_HB_RIGHT_LOWER_LEG);
    MAPS("FOOT.L",         JCE_HB_LEFT_FOOT);

    /* THE ONE THAT CAUGHT A DEFECT.  This rig has `HIP CONTROLLER` for the
     * hips AND `HIP.L` / `HIP.R` for the leg roots.  `hip` is an unsided rule,
     * so without rejecting sided names the left hip claimed JCE_HB_HIPS and
     * which one won came down to which joint the file listed first. */
    MAPS("HIP.L", JCE_HB_COUNT);
    MAPS("HIP.R", JCE_HB_COUNT);

    /* IK helpers are not bones. */
    MAPS("TARGET.L", JCE_HB_COUNT);
    MAPS("POLE.R",   JCE_HB_COUNT);
    MAPS("ROOT",     JCE_HB_COUNT);
}

/* ── Khronos sample naming (RiggedFigure.glb, CesiumMan.glb) ───────── */
static void test_khronos_sample_naming(void)
{
    MAPS("torso_joint_1", JCE_HB_HIPS);
    MAPS("torso_joint_2", JCE_HB_SPINE);
    MAPS("torso_joint_3", JCE_HB_CHEST);
    MAPS("neck_joint_1",  JCE_HB_NECK);
    MAPS("neck_joint_2",  JCE_HB_HEAD);
    MAPS("arm_joint_L_1", JCE_HB_LEFT_UPPER_ARM);
    MAPS("arm_joint_R_2", JCE_HB_RIGHT_LOWER_ARM);
    MAPS("arm_joint_L_3", JCE_HB_LEFT_HAND);
    MAPS("leg_joint_R_1", JCE_HB_RIGHT_UPPER_LEG);
    MAPS("leg_joint_L_2", JCE_HB_LEFT_LOWER_LEG);
    MAPS("leg_joint_L_3", JCE_HB_LEFT_FOOT);

    /* CesiumMan prefixes half its joints and numbers its arms from 2.  The
     * prefix is handled; the numbering is NOT, and that is recorded here
     * rather than papered over -- `arm_joint_L__4_` is that rig's hand and
     * this matcher does not know it. */
    MAPS("Skeleton_torso_joint_1",   JCE_HB_HIPS);
    MAPS("Skeleton_neck_joint_2",    JCE_HB_HEAD);
    MAPS("Skeleton_arm_joint_L__4_", JCE_HB_COUNT);
    MAPS("Skeleton_arm_joint_R",     JCE_HB_COUNT);
}

/* ── The side rule ─────────────────────────────────────────────────── */
static void test_a_paired_bone_without_a_side_is_not_guessed(void)
{
    /* An arm with no side could be either, and driving a left arm with a right
     * arm's motion is worse than an arm that does not move. */
    MAPS("upperarm", JCE_HB_COUNT);
    MAPS("hand",     JCE_HB_COUNT);
    MAPS("foot",     JCE_HB_COUNT);

    /* `clavicle` ends in an l, and it is not a left anything. */
    MAPS("clavicle", JCE_HB_COUNT);

    /* The long spellings work too. */
    MAPS("Left_Hand",  JCE_HB_LEFT_HAND);
    MAPS("right_foot", JCE_HB_RIGHT_FOOT);
}

static void test_bone_names_are_all_present(void)
{
    for (int b = 0; b < JCE_HB_COUNT; b++) {
        const char *n = jce_humanoid_bone_name((JceHumanoidBone)b);
        TEST_ASSERT_NOT_NULL(n);
        TEST_ASSERT_TRUE_MESSAGE(n[0] != '\0' && strcmp(n, "?") != 0,
                                 "a humanoid bone has no name");
    }
    TEST_ASSERT_EQUAL_STRING("?", jce_humanoid_bone_name((JceHumanoidBone)999));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_unreal_naming);
    RUN_TEST(test_blender_naming);
    RUN_TEST(test_khronos_sample_naming);
    RUN_TEST(test_a_paired_bone_without_a_side_is_not_guessed);
    RUN_TEST(test_bone_names_are_all_present);
    return UNITY_END();
}
