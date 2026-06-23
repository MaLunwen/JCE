/*
 * jce_anim_fbbik.h — Full-Body IK solver core (FABRIK on a joint TREE).
 *
 * The generic, skeleton-agnostic heart of a full-body IK rig (UE5 Control Rig /
 * Unity Avatar IK / RootMotion FinalIK FBBIK class of feature): a body is a TREE
 * of position nodes joined by fixed-length bones; several EFFECTORS pull their
 * nodes toward world targets at once, and the solver finds node positions that
 * reach the targets while PRESERVING every bone length (rigidity) — so pulling a
 * hand propagates through the arm into the shared spine/pelvis and out to the
 * other limbs, exactly the coupled behaviour single-chain IK (two-bone / CCD /
 * single-chain FABRIK in jce_anim_ik.h) cannot express.
 *
 * Algorithm: FABRIK extended to a tree with sub-base averaging (Aristidou &
 * Lasenby).  Each iteration runs a backward pass (leaves->root, averaging the
 * pull of every child at shared nodes) then a forward pass (root re-anchored to
 * its original position -> leaves), iterating until the worst effector is within
 * tolerance or the iteration cap is hit.
 *
 * 100% PURE + headless + deterministic: positions in, positions out, no
 * skeleton / quaternion / ECS / GPU dependency.  The skeleton<->body binding
 * (extract node positions from a pose, write solved positions back as joint
 * rotations) is a SEPARATE consuming layer (documented multi-session followup).
 *
 * Layer: L4 (middleware/animation).  Consumed via <jce/api_animation.h>.
 */

#ifndef JCE_ANIM_FBBIK_H
#define JCE_ANIM_FBBIK_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_FBBIK_MAX_NODES     256   /* covers a full humanoid rig as one body */
#define JCE_FBBIK_MAX_EFFECTORS 8

/*
 * A full-body solve body.  Caller fills it; the solver mutates `positions` in
 * place.  CONTRACT: `parents[i] < i` for every i > 0 (parent-before-child, the
 * standard skeleton ordering), node 0 is the ROOT (parents[0] == -1, lengths[0]
 * ignored), and lengths[i] is the bone length from node i to parents[i] (it is
 * NOT recomputed — pass the rest-pose distances so rigidity is preserved exactly).
 */
typedef struct JceFbbikBody {
    jce_vec3 positions[JCE_FBBIK_MAX_NODES]; /* in: rest positions; out: solved */
    int      parents[JCE_FBBIK_MAX_NODES];   /* parent index, -1 for the root    */
    float    lengths[JCE_FBBIK_MAX_NODES];   /* bone length to parent (0 at root) */
    int      node_count;                     /* 1..JCE_FBBIK_MAX_NODES            */
} JceFbbikBody;

/* One effector: pull `node` toward `target` with `weight` (0 = ignored, 1 = full). */
typedef struct JceFbbikEffector {
    int      node;
    jce_vec3 target;
    float    weight;
} JceFbbikEffector;

/*
 * Solve the body in place.
 *
 *   body          : node tree (positions mutated to the solution)
 *   effectors     : effector_count pulls (node indices must be in range)
 *   max_iterations: hard cap on FABRIK iterations (e.g. 10)
 *   tolerance     : converged when every weighted effector is within this world
 *                   distance of its target (e.g. 1e-3)
 *
 * Returns the number of iterations actually run (>= 1 if it did any work, 0 on
 * invalid args).  The root node (0) is re-anchored to its INPUT position every
 * iteration, so the body never drifts off its base.  Bone lengths are preserved
 * to within floating-point error.  NULL-safe (returns 0).
 *
 * Helper: compute lengths[] from the current positions + parents (rest pose),
 * so the caller can fill positions + parents and let the solver derive the
 * rigid bone lengths.  No-op on NULL.
 */
JCE_API void JCE_CALL jce_anim_fbbik_compute_lengths(JceFbbikBody *body);

JCE_API int JCE_CALL
jce_anim_fbbik_solve(JceFbbikBody            *body,
                     const JceFbbikEffector  *effectors,
                     int                      effector_count,
                     int                      max_iterations,
                     float                    tolerance);

/* ================================================================== */
/* Skeleton binding (inc 2): pose -> FK -> solve -> solved world pos    */
/* ================================================================== */

typedef struct JceSkeleton JceSkeleton;   /* opaque (jce_skeleton.h) */

/* Effector keyed by SKELETON joint index (not body node) + a WORLD target. */
typedef struct JceFbbikSkelEffector {
    int      joint;
    jce_vec3 target;
    float    weight;
} JceFbbikSkelEffector;

/*
 * Solve full-body IK against a skeleton in its current LOCAL pose.
 *
 * Forward-kinematics the per-joint local transforms into world space (each
 * joint becomes one body node, parents + bone lengths taken from the skeleton),
 * runs the FBBIK solver toward the (joint, world-target) effectors, and writes
 * the solved WORLD POSITION of every joint into out_world_positions[joint_count]
 * — matching the engine's position-based IK convention (foot IK et al.): the
 * renderer/consumer turns these positions back into joint rotations, exactly as
 * it already does for the other IK kinds.  This keeps the binding pure +
 * headless-testable (re-running FK / measuring the effector reach needs no GPU).
 *
 *   skel             : topology (joint count, parents) + rest pose
 *   local_transforms : per-joint local pose mats (NULL -> skeleton rest pose)
 *   root_world       : armature world transform above the root joints (NULL ->
 *                      identity), pre-multiplied onto root joints like evaluate
 *   effectors        : effector_count (joint, world target, weight) pulls
 *   out_world_positions : [joint_count] solved world positions (required)
 *
 * Returns the FBBIK iterations run, or 0 on invalid args / a skeleton larger
 * than JCE_FBBIK_MAX_NODES.  NULL-safe.
 */
JCE_API int JCE_CALL
jce_anim_fbbik_solve_pose(const JceSkeleton           *skel,
                          const jce_mat4              *local_transforms,
                          const jce_mat4              *root_world,
                          const JceFbbikSkelEffector  *effectors,
                          int                          effector_count,
                          jce_vec3                    *out_world_positions,
                          int                          max_iterations,
                          float                        tolerance);

/*
 * Position -> rotation WRITE-BACK (the renderer's half of FBBIK, factored out
 * pure so it is headless-testable).  Given each joint's PRE-solve world global
 * (old_globals) and the solved world POSITIONS (solved_positions, from
 * solve_pose / solve), produce new world globals where every joint sits at its
 * solved position AND is rotated so its primary bone (joint -> its first child)
 * aligns from the old direction to the new one — the standard FABRIK->skeleton
 * orientation step.  Leaf joints keep their orientation (delta = identity).
 * Multi-child sub-bases orient toward their FIRST child (a documented v1
 * approximation; per-child best-fit is a followup).
 *
 * The consumer derives the skin palette as palette[j] = out_globals[j] *
 * inverse_bind[j], exactly like the existing IK-constraint pass.  Pure +
 * deterministic; out_globals may NOT alias old_globals.  NULL-safe (no-op).
 */
JCE_API void JCE_CALL
jce_anim_fbbik_write_back(const JceSkeleton *skel,
                          const jce_mat4   *old_globals,
                          const jce_vec3   *solved_positions,
                          jce_mat4         *out_globals);

JCE_EXTERN_C_END

#endif /* JCE_ANIM_FBBIK_H */
