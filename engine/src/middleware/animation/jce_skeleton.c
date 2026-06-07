/*
 * jce_skeleton.c  Bone/joint hierarchy implementation.
 */

#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/os/core/jce_log.h>

#include "middleware/animation/jce_anim_ozz.h"
#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "jce_skeleton"

struct JceSkeleton {
    JceJoint *joints;
    jce_mat4 *rest_locals;   /* rest-pose local transforms */
    jce_vec3 *rest_t;        /* rest-pose translations */
    jce_quat *rest_r;        /* rest-pose rotations */
    jce_vec3 *rest_s;        /* rest-pose scales */
    uint32_t  num_joints;
    JceOzzSkeleton *ozz_skel; /* ozz-animation bridge handle */
    jce_mat4  root_transform;     /* armature world transform above root joints */
    int       has_root_transform; /* 0 = identity (skip), 1 = apply to roots */
};

/* ================================================================== */
/* Create / Destroy                                                    */
/* ================================================================== */

JceSkeleton *jce_skeleton_create(const JceJoint *joints, uint32_t num_joints)
{
    if (!joints || num_joints == 0) return NULL;

    /* The GPU skinning palette (u_model[] in vs_pbr_skinned.sc /
     * vs_shadow_skinned.sc) addresses at most JCE_MAX_BONES joints. Beyond that
     * the vertex shaders clamp the joint index to the last slot, so the surplus
     * joints silently collapse onto bone JCE_MAX_BONES-1 and the mesh deforms
     * wrong. Emit a clear load-time error so artists know to reduce/merge bones
     * or split the rig, instead of debugging mystery deformation. */
    if (num_joints > JCE_MAX_BONES) {
        LOG_ERROR(LOG_TAG,
                  "skeleton has %u joints but the GPU palette caps at %d "
                  "(JCE_MAX_BONES); joints %d..%u will collapse onto bone %d "
                  "and deform incorrectly — reduce the bone count or split the "
                  "rig",
                  num_joints, JCE_MAX_BONES, JCE_MAX_BONES, num_joints - 1,
                  JCE_MAX_BONES - 1);
    }

    JceSkeleton *skel = (JceSkeleton *)JCE_CALLOC(1, sizeof(*skel));
    if (!skel) return NULL;

    skel->num_joints = num_joints;

    skel->joints = (JceJoint *)JCE_MALLOC(num_joints * sizeof(JceJoint));
    skel->rest_locals = (jce_mat4 *)JCE_MALLOC(num_joints * sizeof(jce_mat4));
    skel->rest_t = (jce_vec3 *)JCE_MALLOC(num_joints * sizeof(jce_vec3));
    skel->rest_r = (jce_quat *)JCE_MALLOC(num_joints * sizeof(jce_quat));
    skel->rest_s = (jce_vec3 *)JCE_MALLOC(num_joints * sizeof(jce_vec3));
    if (!skel->joints || !skel->rest_locals || !skel->rest_t ||
        !skel->rest_r || !skel->rest_s) {
        JCE_FREE(skel->joints);
        JCE_FREE(skel->rest_locals);
        JCE_FREE(skel->rest_t);
        JCE_FREE(skel->rest_r);
        JCE_FREE(skel->rest_s);
        JCE_FREE(skel);
        return NULL;
    }

    memcpy(skel->joints, joints, num_joints * sizeof(JceJoint));

    for (uint32_t i = 0; i < num_joints; i++) {
        skel->rest_locals[i] = joints[i].local_transform;
        skel->rest_t[i] = joints[i].rest_translation;
        skel->rest_r[i] = joints[i].rest_rotation;
        skel->rest_s[i] = joints[i].rest_scale;
    }

    /* Build ozz-animation bridge skeleton for accelerated evaluation. */
    {
        int *parent_indices = (int *)JCE_MALLOC(num_joints * sizeof(int));
        if (parent_indices) {
            for (uint32_t i = 0; i < num_joints; i++)
                parent_indices[i] = (int)joints[i].parent;
            skel->ozz_skel = jce_ozz_skeleton_create(skel->rest_locals,
                                                      parent_indices,
                                                      num_joints);
            JCE_FREE(parent_indices);
        } else {
            skel->ozz_skel = NULL;
        }
    }

    LOG_DEBUG(LOG_TAG, "created skeleton with %u joints", num_joints);
    return skel;
}

void jce_skeleton_destroy(JceSkeleton *skel)
{
    if (!skel) return;
    jce_ozz_skeleton_destroy(skel->ozz_skel);
    JCE_FREE(skel->joints);
    JCE_FREE(skel->rest_locals);
    JCE_FREE(skel->rest_t);
    JCE_FREE(skel->rest_r);
    JCE_FREE(skel->rest_s);
    JCE_FREE(skel);
}

void jce_skeleton_set_root_transform(JceSkeleton *skel, const jce_mat4 *transform)
{
    if (!skel || !transform) return;
    skel->root_transform = *transform;
    skel->has_root_transform = 1;
}

/* ================================================================== */
/* Queries                                                             */
/* ================================================================== */

uint32_t jce_skeleton_joint_count(const JceSkeleton *skel)
{
    return skel ? skel->num_joints : 0;
}

int jce_skeleton_find_joint(const JceSkeleton *skel, const char *name)
{
    if (!skel || !name) return -1;
    for (uint32_t i = 0; i < skel->num_joints; i++) {
        if (strcmp(skel->joints[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

const jce_mat4 *jce_skeleton_rest_pose(const JceSkeleton *skel)
{
    return skel ? skel->rest_locals : NULL;
}

void jce_skeleton_rest_trs(const JceSkeleton *skel,
                            const jce_vec3 **out_translations,
                            const jce_quat **out_rotations,
                            const jce_vec3 **out_scales)
{
    if (!skel) {
        if (out_translations) *out_translations = NULL;
        if (out_rotations)    *out_rotations    = NULL;
        if (out_scales)       *out_scales       = NULL;
        return;
    }
    if (out_translations) *out_translations = skel->rest_t;
    if (out_rotations)    *out_rotations    = skel->rest_r;
    if (out_scales)       *out_scales       = skel->rest_s;
}

/* ================================================================== */
/* Evaluate skinning matrices                                          */
/* ================================================================== */

void jce_skeleton_evaluate(const JceSkeleton *skel,
                            const jce_mat4 *local_transforms,
                            jce_mat4 *out_matrices,
                            uint32_t max_joints)
{
    if (!skel || !out_matrices || max_joints == 0) return;

    uint32_t count = skel->num_joints < max_joints ? skel->num_joints : max_joints;
    const jce_mat4 *locals = local_transforms ? local_transforms : skel->rest_locals;

    /* Fold the armature world transform into root joints so the whole hierarchy
     * inherits it (e.g. CesiumMan's Z-up->Y-up rotation lives on the armature
     * node above the root joint). Both eval paths treat a root joint's local as
     * its global, so pre-multiplying root_transform here fixes both. Without it,
     * skin matrices collapse to root_transform^-1 at bind pose and the character
     * renders rotated onto its face. */
    jce_mat4 *adjusted = NULL;
    if (skel->has_root_transform) {
        adjusted = (jce_mat4 *)JCE_MALLOC(count * sizeof(jce_mat4));
        if (adjusted) {
            for (uint32_t i = 0; i < count; i++) {
                if (skel->joints[i].parent < 0)
                    adjusted[i] = jce_m4_multiply(&skel->root_transform, &locals[i]);
                else
                    adjusted[i] = locals[i];
            }
            locals = adjusted;
        }
    }

    /* Delegate hierarchy traversal to the ozz bridge when available.
     * jce_ozz_skeleton_evaluate computes global (model-space) transforms
     * using ozz SIMD math; we then apply the inverse-bind matrices here. */
    if (skel->ozz_skel) {
        /* Use out_matrices as temporary storage for the globals. */
        jce_ozz_skeleton_evaluate(skel->ozz_skel, locals, out_matrices, count);

        /* Multiply by inverse bind matrices to produce skinning matrices. */
        for (uint32_t i = 0; i < count; i++)
            out_matrices[i] = jce_m4_multiply(&out_matrices[i],
                                              &skel->joints[i].inverse_bind_matrix);
        if (adjusted) JCE_FREE(adjusted);
        return;
    }

    /* Fallback: software path (no ozz). */
    jce_mat4 *globals = (jce_mat4 *)JCE_MALLOC(count * sizeof(jce_mat4));
    if (!globals) {
        LOG_ERROR(LOG_TAG, "failed to allocate globals buffer for %u joints", count);
        if (adjusted) JCE_FREE(adjusted);
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (skel->joints[i].parent < 0)
            globals[i] = locals[i];
        else
            globals[i] = jce_m4_multiply(&globals[skel->joints[i].parent], &locals[i]);
    }

    /* Multiply by inverse bind matrices to produce skinning matrices. */
    for (uint32_t i = 0; i < count; i++)
        out_matrices[i] = jce_m4_multiply(&globals[i], &skel->joints[i].inverse_bind_matrix);

    JCE_FREE(globals);
    if (adjusted) JCE_FREE(adjusted);
}

jce_mat4 jce_skeleton_get_inverse_bind(const JceSkeleton *skel, uint32_t joint_idx)
{
    if (!skel || joint_idx >= skel->num_joints)
        return jce_m4_identity();
    return skel->joints[joint_idx].inverse_bind_matrix;
}
