/*
 * jce_avatar_humanoid.h  Humanoid avatar enum + retarget mapping.
 *
 * Unity HumanBodyBones equivalent.  Each humanoid skeleton has a
 * canonical set of named bones (Hips / Chest / LeftUpperArm / …);
 * a JceAvatarMap binds a source skeleton's joint indices to these
 * canonical names so an animation authored on one rig can play
 * back on another.
 *
 * Plus an AvatarMask: per-bone weight + per-body-region toggle that
 * gates which bones an animation layer affects.
 *
 * Layer: animation (Layer 4) — public.
 */

#ifndef JCE_AVATAR_HUMANOID_H
#define JCE_AVATAR_HUMANOID_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Canonical humanoid bone enum — matches Unity HumanBodyBones. */
typedef enum {
    JCE_HBB_HIPS              = 0,
    JCE_HBB_LEFT_UPPER_LEG    = 1,
    JCE_HBB_RIGHT_UPPER_LEG   = 2,
    JCE_HBB_LEFT_LOWER_LEG    = 3,
    JCE_HBB_RIGHT_LOWER_LEG   = 4,
    JCE_HBB_LEFT_FOOT         = 5,
    JCE_HBB_RIGHT_FOOT        = 6,
    JCE_HBB_SPINE             = 7,
    JCE_HBB_CHEST             = 8,
    JCE_HBB_UPPER_CHEST       = 9,
    JCE_HBB_NECK              = 10,
    JCE_HBB_HEAD              = 11,
    JCE_HBB_LEFT_SHOULDER     = 12,
    JCE_HBB_RIGHT_SHOULDER    = 13,
    JCE_HBB_LEFT_UPPER_ARM    = 14,
    JCE_HBB_RIGHT_UPPER_ARM   = 15,
    JCE_HBB_LEFT_LOWER_ARM    = 16,
    JCE_HBB_RIGHT_LOWER_ARM   = 17,
    JCE_HBB_LEFT_HAND         = 18,
    JCE_HBB_RIGHT_HAND        = 19,
    JCE_HBB_LEFT_TOES         = 20,
    JCE_HBB_RIGHT_TOES        = 21,
    JCE_HBB_LEFT_EYE          = 22,
    JCE_HBB_RIGHT_EYE         = 23,
    JCE_HBB_JAW               = 24,
    /* Fingers — left then right, 5 fingers × 3 joints. */
    JCE_HBB_LEFT_THUMB_PROXIMAL    = 25,
    JCE_HBB_LEFT_THUMB_INTERMEDIATE= 26,
    JCE_HBB_LEFT_THUMB_DISTAL      = 27,
    JCE_HBB_LEFT_INDEX_PROXIMAL    = 28,
    JCE_HBB_LEFT_INDEX_INTERMEDIATE= 29,
    JCE_HBB_LEFT_INDEX_DISTAL      = 30,
    JCE_HBB_LEFT_MIDDLE_PROXIMAL   = 31,
    JCE_HBB_LEFT_MIDDLE_INTERMEDIATE = 32,
    JCE_HBB_LEFT_MIDDLE_DISTAL     = 33,
    JCE_HBB_LEFT_RING_PROXIMAL     = 34,
    JCE_HBB_LEFT_RING_INTERMEDIATE = 35,
    JCE_HBB_LEFT_RING_DISTAL       = 36,
    JCE_HBB_LEFT_LITTLE_PROXIMAL   = 37,
    JCE_HBB_LEFT_LITTLE_INTERMEDIATE= 38,
    JCE_HBB_LEFT_LITTLE_DISTAL     = 39,
    JCE_HBB_RIGHT_THUMB_PROXIMAL   = 40,
    JCE_HBB_RIGHT_THUMB_INTERMEDIATE= 41,
    JCE_HBB_RIGHT_THUMB_DISTAL     = 42,
    JCE_HBB_RIGHT_INDEX_PROXIMAL   = 43,
    JCE_HBB_RIGHT_INDEX_INTERMEDIATE= 44,
    JCE_HBB_RIGHT_INDEX_DISTAL     = 45,
    JCE_HBB_RIGHT_MIDDLE_PROXIMAL  = 46,
    JCE_HBB_RIGHT_MIDDLE_INTERMEDIATE= 47,
    JCE_HBB_RIGHT_MIDDLE_DISTAL    = 48,
    JCE_HBB_RIGHT_RING_PROXIMAL    = 49,
    JCE_HBB_RIGHT_RING_INTERMEDIATE= 50,
    JCE_HBB_RIGHT_RING_DISTAL      = 51,
    JCE_HBB_RIGHT_LITTLE_PROXIMAL  = 52,
    JCE_HBB_RIGHT_LITTLE_INTERMEDIATE= 53,
    JCE_HBB_RIGHT_LITTLE_DISTAL    = 54,
    JCE_HBB__COUNT
} JceHumanBodyBone;

JCE_API const char *jce_hbb_name(JceHumanBodyBone b);
JCE_API JceHumanBodyBone jce_hbb_from_name(const char *name);

/* ── Avatar map: humanoid bone → skeleton joint index ───────── */

typedef struct {
    /* Joint index for each humanoid bone slot.  -1 = unmapped. */
    int32_t joint_index[JCE_HBB__COUNT];
} JceAvatarMap;

JCE_API void jce_avatar_map_init(JceAvatarMap *m);
JCE_API void jce_avatar_map_set (JceAvatarMap *m, JceHumanBodyBone b, int32_t joint);
JCE_API int32_t jce_avatar_map_get(const JceAvatarMap *m, JceHumanBodyBone b);

/* Retarget pose data from `src` rig to `dst` rig using their avatar
 * maps + a shared humanoid-pose buffer.  Caller supplies world-space
 * pose arrays sized to each rig's joint count.  Bones not present
 * in both maps are left unchanged on dst. */
typedef struct {
    float position[3];
    float orientation[4];
} JceAvatarPose;

JCE_API void jce_avatar_retarget(const JceAvatarMap *src_map,
                                   const JceAvatarPose *src_pose,
                                   uint32_t              src_count,
                                   const JceAvatarMap *dst_map,
                                   JceAvatarPose       *dst_pose,
                                   uint32_t              dst_count);

/* ── Avatar mask ─────────────────────────────────────────────── */

typedef enum {
    JCE_AVATAR_REGION_BODY      = 0,
    JCE_AVATAR_REGION_HEAD      = 1,
    JCE_AVATAR_REGION_LEFT_ARM  = 2,
    JCE_AVATAR_REGION_RIGHT_ARM = 3,
    JCE_AVATAR_REGION_LEFT_LEG  = 4,
    JCE_AVATAR_REGION_RIGHT_LEG = 5,
    JCE_AVATAR_REGION_ROOT      = 6,
    JCE_AVATAR_REGION__COUNT
} JceAvatarRegion;

typedef struct {
    /* Per-region enable flag — fastest gate. */
    bool  region_enabled[JCE_AVATAR_REGION__COUNT];
    /* Optional per-bone weight 0..1 — applied on top of region gate. */
    float bone_weight[JCE_HBB__COUNT];
} JceAvatarMask;

JCE_API void jce_avatar_mask_init     (JceAvatarMask *m);
JCE_API float jce_avatar_mask_weight  (const JceAvatarMask *m,
                                         JceHumanBodyBone bone);

/* Region containing a bone — used internally + exposed for tools. */
JCE_API JceAvatarRegion jce_avatar_bone_region(JceHumanBodyBone bone);

JCE_EXTERN_C_END

#endif /* JCE_AVATAR_HUMANOID_H */
