/*
 * jce_ragdoll.c  Skeleton-driven ragdoll physics implementation.
 *
 * See jce_ragdoll.h for the contract and the body_to_bone derivation.
 */

#include "middleware/animation/jce_ragdoll.h"

#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "jce_ragdoll"

/* Minimum capsule length (metres) for leaf / zero-length bones so every joint
 * still gets a valid, non-degenerate body. */
#define JCE_RAGDOLL_MIN_BONE_LEN 0.05f

/* ================================================================== */
/* Bind-pose world transforms                                          */
/* ================================================================== */

/* Compute each joint's bind-pose LOCAL transform (from rest TRS) and then its
 * WORLD transform by walking the parent chain.  Joints are ordered so a parent
 * always precedes its children, so a single forward pass suffices.
 *
 * out_local[i] = TRS(rest_t[i], rest_r[i], rest_s[i])
 * out_world[i] = (parent < 0) ? out_local[i]
 *                             : out_world[parent] * out_local[i]
 */
static void ragdoll_bind_world(const JceSkeleton *skel,
                               uint32_t           count,
                               jce_mat4          *out_local,
                               jce_mat4          *out_world)
{
    const jce_vec3 *rt = NULL;
    const jce_quat *rr = NULL;
    const jce_vec3 *rs = NULL;
    jce_skeleton_rest_trs(skel, &rt, &rr, &rs);

    for (uint32_t i = 0; i < count; ++i) {
        jce_vec3 t = rt ? rt[i] : jce_v3(0.0f, 0.0f, 0.0f);
        jce_quat r = rr ? rr[i] : jce_q_identity();
        jce_vec3 s = rs ? jce_v3_safe_scale(rs[i]) : jce_v3(1.0f, 1.0f, 1.0f);

        out_local[i] = jce_m4_from_trs(t, r, s);

        int parent = jce_skeleton_joint_parent(skel, i);
        if (parent < 0) {
            out_world[i] = out_local[i];
        } else {
            out_world[i] = jce_m4_multiply(&out_world[(uint32_t)parent],
                                           &out_local[i]);
        }
    }
}

/* World-space origin (translation column) of a transform. */
static jce_vec3 ragdoll_origin(const jce_mat4 *m)
{
    return jce_v3(m->raw[3][0], m->raw[3][1], m->raw[3][2]);
}

/* ================================================================== */
/* Creation                                                            */
/* ================================================================== */

JceRagdoll *jce_ragdoll_create(const JceSkeleton *skel,
                               JcePhysicsWorld   *world,
                               float              radius,
                               float              height_scale)
{
    if (!skel || !world) return NULL;

    uint32_t count = jce_skeleton_joint_count(skel);
    if (count == 0) return NULL;
    if (count > JCE_MAX_BONES) {
        LOG_ERROR(LOG_TAG,
                  "skeleton joint count exceeds JCE_MAX_BONES (count=%u cap=%d)",
                  count, JCE_MAX_BONES);
        return NULL;
    }

    float cap_radius = (radius > 1e-4f) ? radius : 0.05f;
    float len_scale  = (height_scale > 1e-4f) ? height_scale : 1.0f;

    JceRagdoll *rd = (JceRagdoll *)JCE_CALLOC(1, sizeof(*rd));
    if (!rd) return NULL;

    rd->skel             = skel;
    rd->world            = world;
    rd->body_count       = 0;
    rd->constraint_count = 0;
    rd->blend_weight     = 1.0f;

    /* Bind-pose local + world transforms for every joint. */
    jce_mat4 *bind_local = (jce_mat4 *)JCE_CALLOC(count, sizeof(jce_mat4));
    jce_mat4 *bind_world = (jce_mat4 *)JCE_CALLOC(count, sizeof(jce_mat4));
    if (!bind_local || !bind_world) {
        if (bind_local) JCE_FREE(bind_local);
        if (bind_world) JCE_FREE(bind_world);
        JCE_FREE(rd);
        return NULL;
    }
    ragdoll_bind_world(skel, count, bind_local, bind_world);

    /* ---- One DYNAMIC capsule body per joint ---- */
    for (uint32_t i = 0; i < count; ++i) {
        jce_vec3 bone_pos = ragdoll_origin(&bind_world[i]);
        jce_quat bone_rot = jce_m4_to_quat(&bind_world[i]);

        /* Capsule length: distance from this joint to its first child (the bone
         * it "owns").  Leaf joints have no child → fall back to the distance
         * from the parent, then to the minimum. */
        float bone_len = 0.0f;
        for (uint32_t c = i + 1u; c < count; ++c) {
            if (jce_skeleton_joint_parent(skel, c) == (int)i) {
                jce_vec3 child_pos = ragdoll_origin(&bind_world[c]);
                bone_len = jce_v3_len(jce_v3_sub(child_pos, bone_pos));
                break;
            }
        }
        if (bone_len < JCE_RAGDOLL_MIN_BONE_LEN) {
            int parent = jce_skeleton_joint_parent(skel, i);
            if (parent >= 0) {
                jce_vec3 parent_pos = ragdoll_origin(&bind_world[(uint32_t)parent]);
                bone_len = jce_v3_len(jce_v3_sub(bone_pos, parent_pos));
            }
        }
        bone_len *= len_scale;
        if (bone_len < JCE_RAGDOLL_MIN_BONE_LEN)
            bone_len = JCE_RAGDOLL_MIN_BONE_LEN;

        /* Capsule half_height is the cylinder half-length (excluding caps), per
         * the physics desc convention: half_extents = (radius, half_height, 0). */
        float half_height = bone_len * 0.5f;

        /* Place the body at the bone's bind world transform.  body_to_bone then
         * captures the (here identity) offset so pose recovery is exact even if
         * a future placement centres the capsule on the bone midpoint. */
        JceBodyDesc bd;
        memset(&bd, 0, sizeof(bd));
        bd.type            = JCE_BODY_DYNAMIC;
        bd.shape           = JCE_SHAPE_CAPSULE;
        bd.position        = bone_pos;
        bd.rotation        = bone_rot;
        bd.half_extents    = jce_v3(cap_radius, half_height, 0.0f);
        bd.mass            = 1.0f;
        bd.friction        = 0.5f;
        bd.restitution     = 0.0f;
        bd.linear_damping  = 0.05f;
        bd.angular_damping = 0.05f;
        bd.collision_group = JCE_COLLISION_DEFAULT_GROUP;
        bd.collision_mask  = JCE_COLLISION_ALL_MASK;
        bd.is_trigger      = false;

        JceBodyHandle h = jce_physics_body_create(world, &bd);
        if (!jce_body_valid(h)) {
            LOG_ERROR(LOG_TAG, "failed to create ragdoll body for joint %u", i);
            /* Roll back what we created so far. */
            for (int b = rd->constraint_count - 1; b >= 0; --b)
                jce_physics_constraint_destroy(world, rd->constraints[b]);
            for (int b = rd->body_count - 1; b >= 0; --b)
                jce_physics_body_destroy(world, rd->bodies[b].body);
            JCE_FREE(bind_local);
            JCE_FREE(bind_world);
            JCE_FREE(rd);
            return NULL;
        }

        /* Recover transform: bone_world = body_world * body_to_bone, captured
         * from the bind pose so it is correct whatever the body placement. */
        jce_mat4 body_world = jce_m4_from_trs(bone_pos, bone_rot,
                                              jce_v3(1.0f, 1.0f, 1.0f));
        jce_mat4 inv_body   = jce_m4_inverse(&body_world);

        JceRagdollBody *rb = &rd->bodies[rd->body_count];
        rb->body         = h;
        rb->joint_index  = (int)i;
        rb->parent_joint = jce_skeleton_joint_parent(skel, i);
        rb->body_to_bone = jce_m4_multiply(&inv_body, &bind_world[i]);
        rd->body_count++;
    }

    /* ---- GENERIC6DOF constraint per non-root joint ---- */
    for (int i = 0; i < rd->body_count; ++i) {
        JceRagdollBody *rb = &rd->bodies[i];
        if (rb->parent_joint < 0) continue;

        /* Find the parent body's slot in our bodies[] (joints are ordered so
         * the parent precedes the child, so it is already created). */
        int parent_slot = -1;
        for (int p = 0; p < rd->body_count; ++p) {
            if (rd->bodies[p].joint_index == rb->parent_joint) {
                parent_slot = p;
                break;
            }
        }
        if (parent_slot < 0) continue;

        JceRagdollBody *parent_rb = &rd->bodies[parent_slot];

        /* Anchor the joint at this bone's bind origin, expressed LOCAL to each
         * body.  Because both bodies were placed with identity scale at their
         * bind world transforms, the local anchor is inverse(bodyN_world) *
         * child_bind_origin.  pivot is a point, so use the w=1 transform. */
        jce_vec3 anchor_world = ragdoll_origin(&bind_world[(uint32_t)rb->joint_index]);

        jce_mat4 child_bw = jce_m4_from_trs(
            ragdoll_origin(&bind_world[(uint32_t)rb->joint_index]),
            jce_m4_to_quat(&bind_world[(uint32_t)rb->joint_index]),
            jce_v3(1.0f, 1.0f, 1.0f));
        jce_mat4 parent_bw = jce_m4_from_trs(
            ragdoll_origin(&bind_world[(uint32_t)parent_rb->joint_index]),
            jce_m4_to_quat(&bind_world[(uint32_t)parent_rb->joint_index]),
            jce_v3(1.0f, 1.0f, 1.0f));

        jce_mat4 inv_child  = jce_m4_inverse(&child_bw);
        jce_mat4 inv_parent = jce_m4_inverse(&parent_bw);

        jce_vec4 aw       = jce_v4(anchor_world.x, anchor_world.y,
                                   anchor_world.z, 1.0f);
        jce_vec4 pivot_a4 = jce_m4_mul_v4(&inv_parent, aw);
        jce_vec4 pivot_b4 = jce_m4_mul_v4(&inv_child, aw);

        JceConstraintDesc cd;
        memset(&cd, 0, sizeof(cd));
        cd.type    = JCE_CONSTRAINT_GENERIC6DOF;
        cd.body_a  = parent_rb->body;       /* parent */
        cd.body_b  = rb->body;              /* this joint */
        cd.pivot_a = jce_v3(pivot_a4.x, pivot_a4.y, pivot_a4.z);
        cd.pivot_b = jce_v3(pivot_b4.x, pivot_b4.y, pivot_b4.z);
        cd.axis    = jce_v3(0.0f, 1.0f, 0.0f);
        /* lower == upper == 0 → the two anchor points are locked together on
         * all linear axes (a ball joint); angular axes are left free (Bullet's
         * default 6DOF angular limits), so the chain stays connected and bends
         * like a skeleton rather than separating. */
        cd.lower_limit       = 0.0f;
        cd.upper_limit       = 0.0f;
        cd.disable_collision = true;        /* parent/child capsules overlap */

        JceConstraintHandle ch = jce_physics_constraint_create(world, &cd);
        if (jce_constraint_valid(ch)) {
            rd->constraints[rd->constraint_count] = ch;
            rd->constraint_count++;
        } else {
            LOG_WARN(LOG_TAG, "failed to create constraint for joint %d",
                     rb->joint_index);
        }
    }

    JCE_FREE(bind_local);
    JCE_FREE(bind_world);

    return rd;
}

void jce_ragdoll_destroy(JceRagdoll *rd)
{
    if (!rd) return;

    /* Constraints first (reference the bodies), then the bodies. */
    for (int i = rd->constraint_count - 1; i >= 0; --i)
        jce_physics_constraint_destroy(rd->world, rd->constraints[i]);
    for (int i = rd->body_count - 1; i >= 0; --i)
        jce_physics_body_destroy(rd->world, rd->bodies[i].body);

    JCE_FREE(rd);
}

void jce_ragdoll_set_blend_weight(JceRagdoll *rd, float blend_weight)
{
    if (!rd) return;
    if (blend_weight < 0.0f) blend_weight = 0.0f;
    if (blend_weight > 1.0f) blend_weight = 1.0f;
    rd->blend_weight = blend_weight;
}

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

/* Read body `slot`'s live world transform as a matrix. */
static jce_mat4 ragdoll_body_world(const JceRagdoll *rd, int slot)
{
    jce_vec3 pos = jce_v3(0.0f, 0.0f, 0.0f);
    jce_quat rot = jce_q_identity();
    jce_physics_body_get_transform(rd->world, rd->bodies[slot].body,
                                   &pos, &rot);
    return jce_m4_from_trs(pos, rot, jce_v3(1.0f, 1.0f, 1.0f));
}

/* The bodies[] slot that represents joint `joint_index`, or -1. */
static int ragdoll_slot_of_joint(const JceRagdoll *rd, int joint_index)
{
    for (int i = 0; i < rd->body_count; ++i)
        if (rd->bodies[i].joint_index == joint_index)
            return i;
    return -1;
}

/* ================================================================== */
/* bodies -> bone-local pose                                           */
/* ================================================================== */

void jce_ragdoll_sync_to_pose(const JceRagdoll *rd, jce_mat4 *out_local_transforms)
{
    if (!rd || !out_local_transforms) return;

    uint32_t count = jce_skeleton_joint_count(rd->skel);

    /* Recover every joint's WORLD transform from its body first, indexed by
     * joint index (bodies[] is joint-ordered, so slot i == joint i here, but
     * resolve via the stored joint_index to stay robust). */
    jce_mat4 joint_world[JCE_MAX_BONES];
    bool     have_world[JCE_MAX_BONES];
    for (uint32_t i = 0; i < count; ++i) {
        joint_world[i] = jce_m4_identity();
        have_world[i]  = false;
    }

    for (int slot = 0; slot < rd->body_count; ++slot) {
        const JceRagdollBody *rb = &rd->bodies[slot];
        if (rb->joint_index < 0 || (uint32_t)rb->joint_index >= count) continue;
        jce_mat4 body_world = ragdoll_body_world(rd, slot);
        /* bone_world = body_world * body_to_bone */
        joint_world[(uint32_t)rb->joint_index] =
            jce_m4_multiply(&body_world, &rb->body_to_bone);
        have_world[(uint32_t)rb->joint_index] = true;
    }

    /* Convert each joint world transform to a LOCAL transform:
     *   local = inverse(parent_world) * world   (root: local = world). */
    for (uint32_t i = 0; i < count; ++i) {
        if (!have_world[i]) {
            out_local_transforms[i] = jce_m4_identity();
            continue;
        }
        int parent = jce_skeleton_joint_parent(rd->skel, i);
        if (parent < 0 || !have_world[(uint32_t)parent]) {
            out_local_transforms[i] = joint_world[i];
        } else {
            jce_mat4 inv_parent = jce_m4_inverse(&joint_world[(uint32_t)parent]);
            out_local_transforms[i] =
                jce_m4_multiply(&inv_parent, &joint_world[i]);
        }
    }
}

/* ================================================================== */
/* animated pose -> bodies                                             */
/* ================================================================== */

void jce_ragdoll_sync_from_pose(JceRagdoll      *rd,
                                const jce_mat4  *local_transforms,
                                float            blend_weight,
                                float            dt)
{
    if (!rd || !local_transforms) return;

    if (blend_weight < 0.0f) blend_weight = 0.0f;
    if (blend_weight > 1.0f) blend_weight = 1.0f;
    rd->blend_weight = blend_weight;

    uint32_t count = jce_skeleton_joint_count(rd->skel);

    /* Animated WORLD transforms via a parent walk (joints are ordered so a
     * parent precedes its children). */
    jce_mat4 anim_world[JCE_MAX_BONES];
    for (uint32_t i = 0; i < count; ++i) {
        int parent = jce_skeleton_joint_parent(rd->skel, i);
        if (parent < 0)
            anim_world[i] = local_transforms[i];
        else
            anim_world[i] = jce_m4_multiply(&anim_world[(uint32_t)parent],
                                            &local_transforms[i]);
    }

    /* "Snap" threshold: above it we teleport the body to the target and zero
     * its velocity (animation fully owns the pose); below it we steer the body
     * toward the target with a weighted velocity so physics keeps integrating. */
    const float snap_threshold = 0.999f;
    float       inv_dt         = (dt > 1e-6f) ? (1.0f / dt) : 0.0f;

    for (int slot = 0; slot < rd->body_count; ++slot) {
        JceRagdollBody *rb = &rd->bodies[slot];
        if (rb->joint_index < 0 || (uint32_t)rb->joint_index >= count) continue;

        /* Target body world = bone_world * inverse(body_to_bone). */
        jce_mat4 inv_b2b      = jce_m4_inverse(&rb->body_to_bone);
        jce_mat4 target_body  = jce_m4_multiply(&anim_world[(uint32_t)rb->joint_index],
                                                &inv_b2b);
        jce_vec3 target_pos   = ragdoll_origin(&target_body);
        jce_quat target_rot   = jce_m4_to_quat(&target_body);

        if (blend_weight >= snap_threshold) {
            jce_physics_body_set_transform(rd->world, rb->body,
                                           target_pos, target_rot);
            jce_physics_body_set_velocity(rd->world, rb->body,
                                          jce_v3(0.0f, 0.0f, 0.0f));
            jce_physics_body_set_angular_velocity(rd->world, rb->body,
                                                  jce_v3(0.0f, 0.0f, 0.0f));
        } else if (blend_weight > 0.0f) {
            /* Steer toward the target: velocity = w * (target - current)/dt. */
            jce_vec3 cur_pos = jce_v3(0.0f, 0.0f, 0.0f);
            jce_quat cur_rot = jce_q_identity();
            jce_physics_body_get_transform(rd->world, rb->body,
                                           &cur_pos, &cur_rot);
            jce_vec3 delta = jce_v3_sub(target_pos, cur_pos);
            jce_vec3 vel   = jce_v3_scale(delta, blend_weight * inv_dt);
            jce_physics_body_set_velocity(rd->world, rb->body, vel);
        }
        /* blend_weight == 0: leave the body entirely to physics. */
    }
}
