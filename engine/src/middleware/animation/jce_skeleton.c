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
};

/* ================================================================== */
/* Create / Destroy                                                    */
/* ================================================================== */

JceSkeleton *jce_skeleton_create(const JceJoint *joints, uint32_t num_joints)
{
    if (!joints || num_joints == 0) return NULL;

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
        return;
    }

    /* Fallback: software path (no ozz). */
    jce_mat4 *globals = (jce_mat4 *)JCE_MALLOC(count * sizeof(jce_mat4));
    if (!globals) {
        LOG_ERROR(LOG_TAG, "failed to allocate globals buffer for %u joints", count);
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
}

jce_mat4 jce_skeleton_get_inverse_bind(const JceSkeleton *skel, uint32_t joint_idx)
{
    if (!skel || joint_idx >= skel->num_joints)
        return jce_m4_identity();
    return skel->joints[joint_idx].inverse_bind_matrix;
}
