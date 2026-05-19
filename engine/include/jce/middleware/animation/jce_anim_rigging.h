/*
 * jce_anim_rigging.h  Unity Animation Rigging constraint chain.
 *
 * Phase-2 IK on top of jce_anim_ik (Look-At + FABRIK).  Adds:
 *   - TwoBoneIK   : two-segment chain solver (shoulder→elbow→wrist)
 *                   with pole-vector hint + auto-twist
 *   - MultiAim    : aim a bone at a weighted blend of N targets
 *   - DampedTransform : critically-damped follow toward a target
 *   - OverrideTransform : blend bone's local pose toward a target
 *                          pose with per-channel weights
 *   - TwistChain  : distribute root→tip twist along intermediate
 *                   bones in fractional steps (forearm/upper-arm)
 *
 * All solvers operate on a caller-supplied joint pose buffer
 * (vec3 position + quat orientation per joint).  No ozz coupling.
 *
 * Layer: animation (Layer 4) — public.
 */

#ifndef JCE_ANIM_RIGGING_H
#define JCE_ANIM_RIGGING_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Joint pose — local-space relative to parent.  Caller resolves the
 * joint hierarchy and feeds the world-space pose to the solver. */
typedef struct {
    float position[3];
    float orientation[4]; /* xyzw quaternion */
} JceRigPose;

/* ── Two-bone IK ────────────────────────────────────────────── */

typedef struct {
    /* Joint indices into a caller-owned skeleton pose array. */
    int   root_joint;       /* shoulder */
    int   mid_joint;        /* elbow */
    int   tip_joint;        /* wrist */
    /* Optional pole-vector hint joint; -1 = no hint. */
    int   pole_joint;
    /* World-space target position the tip should reach. */
    float target_pos[3];
    /* World-space target rotation applied to tip after IK. */
    float target_rot[4];
    /* Blend weight 0..1 — interpolate between input and solved pose. */
    float weight;
    /* When true, the tip's rotation is forced to `target_rot`. */
    bool  match_target_rotation;
} JceRigTwoBoneIK;

/* Solve a TwoBone IK on `poses[]` (world-space, mutated in place). */
JCE_API void jce_rig_solve_two_bone_ik(JceRigPose *poses,
                                         uint32_t      pose_count,
                                         const JceRigTwoBoneIK *c);

/* ── Multi-aim ──────────────────────────────────────────────── */

#define JCE_RIG_MULTI_AIM_MAX_TARGETS 4

typedef struct {
    int   source_joint;
    float source_axis[3];   /* local axis to align with target */
    float target_pos[JCE_RIG_MULTI_AIM_MAX_TARGETS][3];
    float target_weight[JCE_RIG_MULTI_AIM_MAX_TARGETS];
    uint32_t target_count;
    float weight;            /* 0..1 overall */
} JceRigMultiAim;

JCE_API void jce_rig_solve_multi_aim(JceRigPose *poses, uint32_t pose_count,
                                       const JceRigMultiAim *c);

/* ── Damped transform ───────────────────────────────────────── */

typedef struct {
    int   joint;
    float target_pos[3];
    float target_rot[4];
    /* Critically-damped half-life in seconds — 0 = snap. */
    float damping_time;
    /* Per-axis position/rotation weights (0..1). */
    float pos_weight;
    float rot_weight;
} JceRigDampedTransform;

JCE_API void jce_rig_solve_damped_transform(JceRigPose *poses,
                                              uint32_t pose_count,
                                              const JceRigDampedTransform *c,
                                              float dt);

/* ── Override transform ─────────────────────────────────────── */

typedef struct {
    int   joint;
    float override_pos[3];
    float override_rot[4];
    float pos_weight;        /* per-axis would need vec3; scalar here */
    float rot_weight;
    bool  override_in_world_space;
} JceRigOverrideTransform;

JCE_API void jce_rig_solve_override_transform(JceRigPose *poses,
                                                uint32_t pose_count,
                                                const JceRigOverrideTransform *c);

/* ── Twist chain ────────────────────────────────────────────── */

#define JCE_RIG_TWIST_CHAIN_MAX 8

typedef struct {
    int   root_joint;
    int   tip_joint;
    /* Mid joints to distribute twist into.  Each has a 0..1
     * fraction interpolating root→tip twist. */
    int   mid_joints[JCE_RIG_TWIST_CHAIN_MAX];
    float mid_fractions[JCE_RIG_TWIST_CHAIN_MAX];
    uint32_t mid_count;
    /* Axis along which twist propagates (typically the bone's local
     * +X). */
    float twist_axis[3];
    float weight;
} JceRigTwistChain;

JCE_API void jce_rig_solve_twist_chain(JceRigPose *poses, uint32_t pose_count,
                                         const JceRigTwistChain *c);

JCE_EXTERN_C_END

#endif /* JCE_ANIM_RIGGING_H */
