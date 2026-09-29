/*
 * test_jce_humanoid_retarget.c -- moving a pose from one rig onto another.
 *
 * Two skeletons that agree about NOTHING except being humanoid: different
 * joint names (Unreal's vs Blender's), different joint COUNT and order,
 * different rest poses, and different sizes.  The claim under test is that a
 * rotation applied to the source's arm comes out as the same rotation applied
 * to the target's arm -- relative to each rig's own rest pose, which is what
 * makes a retarget proportion-independent.
 */
/* The name-based retargeter this one is chosen OVER.  Internal src-side
 * header, reached the same way test_jce_anim_retarget.c reaches it. */
#include "middleware/animation/jce_anim_retarget.h"

#include "unity.h"

#include <jce/middleware/animation/jce_avatar.h>
#include <jce/middleware/animation/jce_humanoid.h>
#include <jce/middleware/animation/jce_skeleton.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void joint(JceJoint *j, const char *name, int parent,
                  jce_vec3 t, jce_quat r)
{
    memset(j, 0, sizeof(*j));
    strncpy(j->name, name, sizeof(j->name) - 1);
    j->parent = (int16_t)parent;
    j->rest_translation = t;
    j->rest_rotation = r;
    j->rest_scale = jce_v3(1.0f, 1.0f, 1.0f);
    j->inverse_bind_matrix = jce_m4_identity();
    j->local_transform = jce_m4_identity();
}

static jce_quat about_z(float deg)
{
    const float h = deg * 0.5f * 3.14159265358979f / 180.0f;
    jce_quat q; q.x = 0.0f; q.y = 0.0f; q.z = sinf(h); q.w = cosf(h);
    return q;
}

static jce_quat about_x(float deg)
{
    const float h = deg * 0.5f * 3.14159265358979f / 180.0f;
    jce_quat q; q.x = sinf(h); q.y = 0.0f; q.z = 0.0f; q.w = cosf(h);
    return q;
}

/* SOURCE: Unreal names, 5 joints, hips at y=1.0, arm rest rotated 20 deg. */
static JceSkeleton *make_src(void)
{
    JceJoint j[5];
    joint(&j[0], "pelvis",     -1, jce_v3(0, 1.0f, 0), jce_q_identity());
    joint(&j[1], "spine_01",    0, jce_v3(0, 0.4f, 0), jce_q_identity());
    joint(&j[2], "clavicle_l",  1, jce_v3(0.1f, 0.3f, 0), jce_q_identity());
    joint(&j[3], "upperarm_l",  2, jce_v3(0.2f, 0, 0), about_z(20.0f));
    joint(&j[4], "lowerarm_l",  3, jce_v3(0.3f, 0, 0), jce_q_identity());
    return jce_skeleton_create(j, 5);
}

/* TARGET: Blender names, 6 joints (one of them is an IK helper the map must
 * ignore), hips at y=0.5 -- HALF the source's height -- and a DIFFERENT arm
 * rest rotation, so a retarget that copied absolute orientations would be
 * visibly wrong here and one that transports deltas would not. */
static JceSkeleton *make_dst(void)
{
    JceJoint j[6];
    joint(&j[0], "HIP CONTROLLER", -1, jce_v3(0, 0.5f, 0), jce_q_identity());
    joint(&j[1], "SPINE",           0, jce_v3(0, 0.2f, 0), jce_q_identity());
    joint(&j[2], "SHOULDER.L",      1, jce_v3(0.05f, 0.15f, 0), jce_q_identity());
    joint(&j[3], "UPPER ARM.L",     2, jce_v3(0.1f, 0, 0), about_z(-35.0f));
    joint(&j[4], "FOREARM.L",       3, jce_v3(0.15f, 0, 0), jce_q_identity());
    joint(&j[5], "TARGET.L",        4, jce_v3(0.05f, 0, 0), jce_q_identity());
    return jce_skeleton_create(j, 6);
}

static float quat_angle_deg(jce_quat a, jce_quat b)
{
    /* The angle of a*inverse(b), which is what "how far apart are these two
     * rotations" means. */
    jce_quat bi; bi.x = -b.x; bi.y = -b.y; bi.z = -b.z; bi.w = b.w;
    const jce_quat d = jce_q_multiply(a, bi);
    float w = d.w; if (w < -1.0f) w = -1.0f; if (w > 1.0f) w = 1.0f;
    float ang = 2.0f * acosf(fabsf(w));
    return ang * 180.0f / 3.14159265358979f;
}

static void test_the_two_rigs_map_despite_sharing_no_names(void)
{
    JceSkeleton *s = make_src(), *d = make_dst();
    JceHumanoidMap ms, md;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(s, &ms));
    TEST_ASSERT_TRUE(jce_humanoid_map_build(d, &md));

    TEST_ASSERT_EQUAL_INT(0, ms.joint[JCE_HB_HIPS]);
    TEST_ASSERT_EQUAL_INT(3, ms.joint[JCE_HB_LEFT_UPPER_ARM]);
    TEST_ASSERT_EQUAL_INT(0, md.joint[JCE_HB_HIPS]);
    TEST_ASSERT_EQUAL_INT(3, md.joint[JCE_HB_LEFT_UPPER_ARM]);
    /* The IK helper is not a bone. */
    TEST_ASSERT_EQUAL_INT(-1, md.joint[JCE_HB_LEFT_HAND]);

    jce_skeleton_destroy(s);
    jce_skeleton_destroy(d);
}

static void test_a_rotation_transfers_as_the_rotation_it_added(void)
{
    JceSkeleton *s = make_src(), *d = make_dst();
    JceHumanoidMap ms, md;
    jce_humanoid_map_build(s, &ms);
    jce_humanoid_map_build(d, &md);

    /* The source pose = its rest, plus 50 degrees on the upper arm. */
    jce_quat sl[5], dl[6], rest_dl[6];
    jce_humanoid_rest_locals(s, sl);
    jce_humanoid_rest_locals(d, rest_dl);
    memcpy(dl, rest_dl, sizeof dl);
    sl[3] = jce_q_multiply(sl[3], about_x(50.0f));

    const uint32_t moved = jce_humanoid_retarget(&ms, s, sl, &md, d, dl, NULL);
    TEST_ASSERT_TRUE_MESSAGE(moved >= 4, "too few roles transported");

    /* THE CLAIM.  The target's upper arm now differs from its own rest pose by
     * the SAME 50 degrees -- not by the source's absolute orientation, which
     * would have dragged the source's 20-degree rest rotation across and
     * fought the target's own -35. */
    const float delta = quat_angle_deg(dl[3], rest_dl[3]);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, 50.0f, delta,
        "the transported rotation is not the rotation the animation added");

    /* And a joint no role names keeps its rest pose exactly. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, rest_dl[5].x, dl[5].x);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, rest_dl[5].w, dl[5].w);

    jce_skeleton_destroy(s);
    jce_skeleton_destroy(d);
}

static void test_the_rest_pose_transfers_as_no_change(void)
{
    /* THE NEGATIVE CONTROL.  Retargeting the source's REST pose must leave the
     * target in ITS rest pose.  Without this, a retarget that simply copied
     * rotations would still pass the test above by accident whenever the two
     * rigs' rest poses happened to be close. */
    JceSkeleton *s = make_src(), *d = make_dst();
    JceHumanoidMap ms, md;
    jce_humanoid_map_build(s, &ms);
    jce_humanoid_map_build(d, &md);

    jce_quat sl[5], dl[6], rest_dl[6];
    jce_humanoid_rest_locals(s, sl);          /* the rest pose, unmodified */
    jce_humanoid_rest_locals(d, rest_dl);
    memcpy(dl, rest_dl, sizeof dl);

    jce_humanoid_retarget(&ms, s, sl, &md, d, dl, NULL);

    for (int i = 0; i < 6; i++) {
        const float off = quat_angle_deg(dl[i], rest_dl[i]);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, 0.0f, off,
            "retargeting the rest pose moved a joint");
    }

    jce_skeleton_destroy(s);
    jce_skeleton_destroy(d);
}

static void test_root_translation_is_scaled_by_the_height_ratio(void)
{
    /* The target's hips sit at half the source's height, so a one-metre stride
     * must arrive as half a metre -- the same stride RELATIVE TO THE BODY. */
    JceSkeleton *s = make_src(), *d = make_dst();
    JceHumanoidMap ms, md;
    jce_humanoid_map_build(s, &ms);
    jce_humanoid_map_build(d, &md);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, ms.hips_height);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, md.hips_height);

    jce_quat sl[5], dl[6];
    jce_humanoid_rest_locals(s, sl);
    jce_humanoid_rest_locals(d, dl);
    jce_vec3 root = jce_v3(1.0f, 0.0f, 2.0f);
    jce_humanoid_retarget(&ms, s, sl, &md, d, dl, &root);

    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, root.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, root.z);

    jce_skeleton_destroy(s);
    jce_skeleton_destroy(d);
}

/* ── The avatar asset ──────────────────────────────────────────────── */

static void test_avatar_round_trips_by_joint_name(void)
{
    JceSkeleton *s = make_src();
    JceAvatarAsset *a = jce_avatar_build(s);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(jce_avatar_mapped_count(a) >= 4);
    TEST_ASSERT_EQUAL_STRING("upperarm_l",
        jce_avatar_joint_name(a, JCE_HB_LEFT_UPPER_ARM));

    const char *path = "test_avatar_roundtrip.avatar";
    TEST_ASSERT_TRUE(jce_avatar_save(a, path));

    JceAvatarAsset *b2 = jce_avatar_load(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(b2, "the avatar did not load back");
    /* A LOADED avatar holds NAMES and no indices until it is bound -- an index
     * means nothing outside the skeleton it was taken from. */
    TEST_ASSERT_EQUAL_INT(-1, jce_avatar_map(b2)->joint[JCE_HB_LEFT_UPPER_ARM]);
    TEST_ASSERT_EQUAL_STRING("upperarm_l",
        jce_avatar_joint_name(b2, JCE_HB_LEFT_UPPER_ARM));

    TEST_ASSERT_TRUE(jce_avatar_bind(b2, s) >= 4);
    TEST_ASSERT_EQUAL_INT(3, jce_avatar_map(b2)->joint[JCE_HB_LEFT_UPPER_ARM]);

    /* Bound to the OTHER rig, whose joints have none of those names, it
     * resolves nothing -- rather than pointing at whatever now sits at index
     * 3. */
    JceSkeleton *d = make_dst();
    TEST_ASSERT_EQUAL_UINT32(0, jce_avatar_bind(b2, d));
    TEST_ASSERT_EQUAL_INT(-1, jce_avatar_map(b2)->joint[JCE_HB_LEFT_UPPER_ARM]);

    jce_avatar_unload(a);
    jce_avatar_unload(b2);
    jce_skeleton_destroy(s);
    jce_skeleton_destroy(d);
    remove(path);
}

static void test_a_missing_avatar_file_is_null_not_empty(void)
{
    /* The old stub returned an allocated empty struct, so every caller's
     * `if (!asset)` passed and "there is no such file" was indistinguishable
     * from success. */
    TEST_ASSERT_NULL(jce_avatar_load("no_such_file_here.avatar"));
    TEST_ASSERT_NULL(jce_avatar_load(NULL));
    TEST_ASSERT_NULL(jce_avatar_build(NULL));
}

/* ── the SELECTION rule the runtime uses ──────────────────────────── */
static void test_the_role_map_beats_the_name_map_on_these_rigs(void)
{
    /* The runtime retargets by JOINT NAME, and that covers rigs sharing a
     * naming convention and NOTHING else: these two rigs share no bone names
     * at all, so a name map matches nothing, every joint falls back to the
     * destination rest pose, and the clip visibly does nothing.
     *
     * The scene renderer therefore builds BOTH maps and uses whichever covers
     * more joints.  That comparison is the decision, and this is where it is
     * decidable without a renderer: assert the two inputs to it.
     *
     * A TIE GOES TO THE NAME MAP in the renderer, so a rig pair that already
     * matched by name keeps exactly the path and the pose it had -- which is
     * why this asserts STRICTLY MORE and not merely "not fewer". */
    JceSkeleton *src = make_src(), *dst = make_dst();
    TEST_ASSERT_NOT_NULL(src); TEST_ASSERT_NOT_NULL(dst);

    JceAnimRetargetMap *nm = jce_anim_retarget_map_create(src, dst);
    const uint32_t by_name = nm ? jce_anim_retarget_mapped_count(nm) : 0u;

    JceHumanoidMap hs, hd;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(src, &hs));
    TEST_ASSERT_TRUE(jce_humanoid_map_build(dst, &hd));
    uint32_t common = 0;
    for (int b = 0; b < JCE_HB_COUNT; b++)
        if (hs.joint[b] >= 0 && hd.joint[b] >= 0) common++;

    TEST_ASSERT_TRUE_MESSAGE(common > by_name,
        "the role map does not cover more of these two rigs than the name map "
        "does -- the renderer's selection would keep using names, and a clip "
        "across differently-named rigs would still do nothing");
    TEST_ASSERT_TRUE_MESSAGE(common >= 3u,
        "the role map covers almost nothing either; the fixture no longer "
        "exercises what it was built to exercise");

    if (nm) jce_anim_retarget_map_destroy(nm);
    jce_skeleton_destroy(src);
    jce_skeleton_destroy(dst);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_two_rigs_map_despite_sharing_no_names);
    RUN_TEST(test_the_role_map_beats_the_name_map_on_these_rigs);
    RUN_TEST(test_a_rotation_transfers_as_the_rotation_it_added);
    RUN_TEST(test_the_rest_pose_transfers_as_no_change);
    RUN_TEST(test_root_translation_is_scaled_by_the_height_ratio);
    RUN_TEST(test_avatar_round_trips_by_joint_name);
    RUN_TEST(test_a_missing_avatar_file_is_null_not_empty);
    return UNITY_END();
}
