/*
 * jce_avatar_humanoid.c  Humanoid bone enum + avatar map + mask.
 */

#include <jce/middleware/animation/jce_avatar_humanoid.h>

#include <string.h>

static const char *s_names[JCE_HBB__COUNT] = {
    "Hips","LeftUpperLeg","RightUpperLeg","LeftLowerLeg","RightLowerLeg",
    "LeftFoot","RightFoot","Spine","Chest","UpperChest","Neck","Head",
    "LeftShoulder","RightShoulder","LeftUpperArm","RightUpperArm",
    "LeftLowerArm","RightLowerArm","LeftHand","RightHand",
    "LeftToes","RightToes","LeftEye","RightEye","Jaw",
    "LeftThumbProximal","LeftThumbIntermediate","LeftThumbDistal",
    "LeftIndexProximal","LeftIndexIntermediate","LeftIndexDistal",
    "LeftMiddleProximal","LeftMiddleIntermediate","LeftMiddleDistal",
    "LeftRingProximal","LeftRingIntermediate","LeftRingDistal",
    "LeftLittleProximal","LeftLittleIntermediate","LeftLittleDistal",
    "RightThumbProximal","RightThumbIntermediate","RightThumbDistal",
    "RightIndexProximal","RightIndexIntermediate","RightIndexDistal",
    "RightMiddleProximal","RightMiddleIntermediate","RightMiddleDistal",
    "RightRingProximal","RightRingIntermediate","RightRingDistal",
    "RightLittleProximal","RightLittleIntermediate","RightLittleDistal",
};

const char *jce_hbb_name(JceHumanBodyBone b)
{
    if ((unsigned)b >= JCE_HBB__COUNT) return "(invalid)";
    return s_names[b];
}

JceHumanBodyBone jce_hbb_from_name(const char *name)
{
    if (!name) return JCE_HBB__COUNT;
    for (int i = 0; i < JCE_HBB__COUNT; ++i)
        if (strcmp(s_names[i], name) == 0) return (JceHumanBodyBone)i;
    return JCE_HBB__COUNT;
}

/* ── Avatar map ─────────────────────────────────────────────── */

void jce_avatar_map_init(JceAvatarMap *m)
{
    if (!m) return;
    for (int i = 0; i < JCE_HBB__COUNT; ++i) m->joint_index[i] = -1;
}

void jce_avatar_map_set(JceAvatarMap *m, JceHumanBodyBone b, int32_t joint)
{
    if (!m || (unsigned)b >= JCE_HBB__COUNT) return;
    m->joint_index[b] = joint;
}

int32_t jce_avatar_map_get(const JceAvatarMap *m, JceHumanBodyBone b)
{
    if (!m || (unsigned)b >= JCE_HBB__COUNT) return -1;
    return m->joint_index[b];
}

void jce_avatar_retarget(const JceAvatarMap *src_map,
                           const JceAvatarPose *src_pose, uint32_t src_n,
                           const JceAvatarMap *dst_map,
                           JceAvatarPose       *dst_pose, uint32_t dst_n)
{
    if (!src_map || !src_pose || !dst_map || !dst_pose) return;
    for (int i = 0; i < JCE_HBB__COUNT; ++i) {
        int32_t src_j = src_map->joint_index[i];
        int32_t dst_j = dst_map->joint_index[i];
        if (src_j < 0 || dst_j < 0) continue;
        if ((uint32_t)src_j >= src_n || (uint32_t)dst_j >= dst_n) continue;
        /* Copy orientation; position preserves dst (target rig scale). */
        memcpy(dst_pose[dst_j].orientation,
                src_pose[src_j].orientation, 16);
    }
}

/* ── Region table ───────────────────────────────────────────── */

static const JceAvatarRegion s_region[JCE_HBB__COUNT] = {
    JCE_AVATAR_REGION_ROOT,                 /* hips */
    JCE_AVATAR_REGION_LEFT_LEG,             /* l upper leg */
    JCE_AVATAR_REGION_RIGHT_LEG,            /* r upper leg */
    JCE_AVATAR_REGION_LEFT_LEG,             /* l lower leg */
    JCE_AVATAR_REGION_RIGHT_LEG,            /* r lower leg */
    JCE_AVATAR_REGION_LEFT_LEG,             /* l foot */
    JCE_AVATAR_REGION_RIGHT_LEG,            /* r foot */
    JCE_AVATAR_REGION_BODY,                 /* spine */
    JCE_AVATAR_REGION_BODY,                 /* chest */
    JCE_AVATAR_REGION_BODY,                 /* upper chest */
    JCE_AVATAR_REGION_HEAD,                 /* neck */
    JCE_AVATAR_REGION_HEAD,                 /* head */
    JCE_AVATAR_REGION_LEFT_ARM,             /* l shoulder */
    JCE_AVATAR_REGION_RIGHT_ARM,            /* r shoulder */
    JCE_AVATAR_REGION_LEFT_ARM,             /* l upper arm */
    JCE_AVATAR_REGION_RIGHT_ARM,            /* r upper arm */
    JCE_AVATAR_REGION_LEFT_ARM,             /* l lower arm */
    JCE_AVATAR_REGION_RIGHT_ARM,            /* r lower arm */
    JCE_AVATAR_REGION_LEFT_ARM,             /* l hand */
    JCE_AVATAR_REGION_RIGHT_ARM,            /* r hand */
    JCE_AVATAR_REGION_LEFT_LEG,             /* l toes */
    JCE_AVATAR_REGION_RIGHT_LEG,            /* r toes */
    JCE_AVATAR_REGION_HEAD,                 /* l eye */
    JCE_AVATAR_REGION_HEAD,                 /* r eye */
    JCE_AVATAR_REGION_HEAD,                 /* jaw */
    /* Left fingers: arm. */
    JCE_AVATAR_REGION_LEFT_ARM,JCE_AVATAR_REGION_LEFT_ARM,JCE_AVATAR_REGION_LEFT_ARM,
    JCE_AVATAR_REGION_LEFT_ARM,JCE_AVATAR_REGION_LEFT_ARM,JCE_AVATAR_REGION_LEFT_ARM,
    JCE_AVATAR_REGION_LEFT_ARM,JCE_AVATAR_REGION_LEFT_ARM,JCE_AVATAR_REGION_LEFT_ARM,
    JCE_AVATAR_REGION_LEFT_ARM,JCE_AVATAR_REGION_LEFT_ARM,JCE_AVATAR_REGION_LEFT_ARM,
    JCE_AVATAR_REGION_LEFT_ARM,JCE_AVATAR_REGION_LEFT_ARM,JCE_AVATAR_REGION_LEFT_ARM,
    /* Right fingers: arm. */
    JCE_AVATAR_REGION_RIGHT_ARM,JCE_AVATAR_REGION_RIGHT_ARM,JCE_AVATAR_REGION_RIGHT_ARM,
    JCE_AVATAR_REGION_RIGHT_ARM,JCE_AVATAR_REGION_RIGHT_ARM,JCE_AVATAR_REGION_RIGHT_ARM,
    JCE_AVATAR_REGION_RIGHT_ARM,JCE_AVATAR_REGION_RIGHT_ARM,JCE_AVATAR_REGION_RIGHT_ARM,
    JCE_AVATAR_REGION_RIGHT_ARM,JCE_AVATAR_REGION_RIGHT_ARM,JCE_AVATAR_REGION_RIGHT_ARM,
    JCE_AVATAR_REGION_RIGHT_ARM,JCE_AVATAR_REGION_RIGHT_ARM,JCE_AVATAR_REGION_RIGHT_ARM,
};

JceAvatarRegion jce_avatar_bone_region(JceHumanBodyBone b)
{
    if ((unsigned)b >= JCE_HBB__COUNT) return JCE_AVATAR_REGION_BODY;
    return s_region[b];
}

void jce_avatar_mask_init(JceAvatarMask *m)
{
    if (!m) return;
    for (int i = 0; i < JCE_AVATAR_REGION__COUNT; ++i)
        m->region_enabled[i] = true;
    for (int i = 0; i < JCE_HBB__COUNT; ++i)
        m->bone_weight[i] = 1.0f;
}

float jce_avatar_mask_weight(const JceAvatarMask *m, JceHumanBodyBone b)
{
    if (!m || (unsigned)b >= JCE_HBB__COUNT) return 0.0f;
    JceAvatarRegion r = jce_avatar_bone_region(b);
    if (!m->region_enabled[r]) return 0.0f;
    return m->bone_weight[b];
}
