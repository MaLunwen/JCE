/*
 * jce_ragdoll.h  Skeleton-driven ragdoll physics (internal).
 *
 * Builds a per-bone DYNAMIC rigid-body chain from a skeleton's bind pose,
 * simulates it in a JcePhysicsWorld, and converts between physics-body world
 * transforms and skeleton bone LOCAL transforms with an animation<->physics
 * blend weight.
 *
 *   blend_weight = 1  : full animation drive (bodies tracked to the animated
 *                       pose every frame; physics is a passenger).
 *   blend_weight = 0  : full physics (bodies fall / collide freely; the bone
 *                       pose is read back from them).
 *   in between        : the bodies are nudged toward the animated pose by a
 *                       weighted velocity, so authoring can cross-fade in/out
 *                       of a ragdoll (hit reactions, death blends).
 *
 * One body per joint, positioned at the joint's bind-pose WORLD transform.
 * Each non-root joint gets a GENERIC6DOF constraint linking its body to the
 * parent's body, anchored at the child joint's bind origin, so the chain
 * stays connected like a skeleton (linear-locked ball joint — the public
 * constraint API exposes only linear limits, which is exactly the "stay
 * connected" condition a ragdoll needs).
 *
 * `body_to_bone` recovers the bone world transform from the body world
 * transform:  bone_world = body_world * body_to_bone.  It is captured at
 * create time as inverse(body_world_bind) * bone_world_bind, so it is correct
 * regardless of how the capsule body is centred/oriented along the bone.
 *
 * This header is an INTERNAL src-side header (like jce_anim_retarget.h /
 * jce_anim_ozz.h): it is included by jce_ragdoll.c and by the unit test.  It
 * is NOT part of the public <jce/api_animation.h> surface — only the public
 * skeleton + physics + math APIs cross the boundary.  Scene-renderer
 * integration (the physics-step-ordering wiring) is a documented followup; this
 * is the headless-testable core only.
 *
 * Layer: Animation (Layer 4) -- internal.
 */

#ifndef JCE_RAGDOLL_H
#define JCE_RAGDOLL_H

#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/middleware/physics/jce_physics.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_skinned_mesh.h>   /* JCE_MAX_BONES */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Per-bone physics body                                               */
/* ================================================================== */

typedef struct {
    JceBodyHandle body;          /* DYNAMIC capsule for this joint           */
    int           joint_index;   /* joint this body represents               */
    int           parent_joint;  /* parent joint index, -1 for the root      */
    jce_mat4      body_to_bone;  /* bone_world = body_world * body_to_bone    */
} JceRagdollBody;

/* ================================================================== */
/* Ragdoll                                                             */
/* ================================================================== */

/* The struct carries a TAG (`struct JceRagdoll`) so the PUBLIC runtime header
 * (jce/application/jce_runtime.h) can forward-declare it as an opaque type
 * (typedef struct JceRagdoll JceRagdoll;) for jce_runtime_entity_ragdoll
 * WITHOUT exposing this internal definition.  The tag is otherwise inert. */
typedef struct JceRagdoll {
    const JceSkeleton *skel;     /* borrowed; caller retains ownership        */
    JcePhysicsWorld   *world;    /* borrowed; caller retains ownership        */

    JceRagdollBody       bodies[JCE_MAX_BONES];
    int                  body_count;

    JceConstraintHandle  constraints[JCE_MAX_BONES];
    int                  constraint_count;

    float blend_weight;          /* 1 = full animation, 0 = full physics      */
} JceRagdoll;

/* ================================================================== */
/* Lifecycle                                                          */
/* ================================================================== */

/* Build a ragdoll from a skeleton's bind pose.
 *
 *   radius        : capsule radius for every bone body (metres).
 *   height_scale  : multiplies the bone->child bind distance to size each
 *                   capsule's length (1.0 = exact bind segment length).  A
 *                   minimum length is enforced so leaf / zero-length bones
 *                   still get a valid body.
 *
 * Creates one DYNAMIC CAPSULE body per joint at its bind-pose world transform
 * and, for every joint with a parent, a GENERIC6DOF constraint anchoring this
 * joint's bind origin in both the parent body and this body (keeps the chain
 * connected).  The borrowed world + skeleton must outlive the ragdoll.
 *
 * Returns NULL on NULL inputs, an empty skeleton, a skeleton exceeding
 * JCE_MAX_BONES, or allocation / body-creation failure.  blend_weight starts
 * at 1.0 (animation-driven). */
JceRagdoll *jce_ragdoll_create(const JceSkeleton *skel,
                               JcePhysicsWorld   *world,
                               float              radius,
                               float              height_scale);

/* Destroy every constraint then every body, then the ragdoll.  Leaves the
 * borrowed world + skeleton untouched.  NULL-safe. */
void jce_ragdoll_destroy(JceRagdoll *rd);

/* Set the animation<->physics blend weight (clamped to [0,1]). */
void jce_ragdoll_set_blend_weight(JceRagdoll *rd, float blend_weight);

/* ================================================================== */
/* Pose <-> bodies sync                                                */
/* ================================================================== */

/* Read each body's world transform, recover the bone WORLD transform via
 * body_to_bone, convert to LOCAL (multiply by the inverse of the parent
 * joint's recovered world transform; root = world directly) and write
 * out_local_transforms[joint] = TRS.  The result is a pose ready to feed
 * jce_skeleton_evaluate(skel, out_local_transforms, ...).
 *
 * out_local_transforms must hold at least joint_count entries.  No allocation. */
void jce_ragdoll_sync_to_pose(const JceRagdoll *rd, jce_mat4 *out_local_transforms);

/* Drive the bodies toward an animated pose.
 *
 *   local_transforms : [joint_count] per-joint LOCAL transforms (e.g. from
 *                      animation sampling / jce_skeleton evaluation input).
 *   blend_weight     : 1 = snap bodies to the animated pose (kinematic-like
 *                      tracking); 0 = leave the bodies to physics; in between
 *                      = nudge via a weighted velocity toward the target.
 *   dt               : timestep used to derive the tracking velocity.
 *
 * Computes each joint's animated WORLD transform from local_transforms (parent
 * walk), then the matching body world transform via the inverse of
 * body_to_bone (body_world = bone_world * inverse(body_to_bone)).  When
 * blend_weight is high the body is teleported to that transform and its
 * velocities zeroed; otherwise a velocity proportional to (target - current)
 * scaled by blend_weight is applied so physics keeps integrating.
 *
 * Also stores blend_weight on the ragdoll.  No allocation. */
void jce_ragdoll_sync_from_pose(JceRagdoll      *rd,
                                const jce_mat4  *local_transforms,
                                float            blend_weight,
                                float            dt);

#ifdef __cplusplus
}
#endif

#endif /* JCE_RAGDOLL_H */
