/*
 * jce_skeleton.c  Bone/joint hierarchy implementation.
 */

#include "jce_skeleton.h"
#include <jce/core/jce_log.h>

#include <SDL3/SDL.h>
#include <string.h>

#define LOG_TAG "jce_skeleton"

struct JceSkeleton {
    JceJoint *joints;
    jce_mat4 *rest_locals;   /* rest-pose local transforms */
    uint32_t  num_joints;
};

/* ================================================================== */
/* Create / Destroy                                                    */
/* ================================================================== */

JceSkeleton *jce_skeleton_create(const JceJoint *joints, uint32_t num_joints)
{
    if (!joints || num_joints == 0) return NULL;

    JceSkeleton *skel = (JceSkeleton *)SDL_calloc(1, sizeof(*skel));
    if (!skel) return NULL;

    skel->num_joints = num_joints;

    skel->joints = (JceJoint *)SDL_malloc(num_joints * sizeof(JceJoint));
    skel->rest_locals = (jce_mat4 *)SDL_malloc(num_joints * sizeof(jce_mat4));
    if (!skel->joints || !skel->rest_locals) {
        SDL_free(skel->joints);
        SDL_free(skel->rest_locals);
        SDL_free(skel);
        return NULL;
    }

    memcpy(skel->joints, joints, num_joints * sizeof(JceJoint));

    for (uint32_t i = 0; i < num_joints; i++)
        skel->rest_locals[i] = joints[i].local_transform;

    LOG_DEBUG(LOG_TAG, "created skeleton with %u joints", num_joints);
    return skel;
}

void jce_skeleton_destroy(JceSkeleton *skel)
{
    if (!skel) return;
    SDL_free(skel->joints);
    SDL_free(skel->rest_locals);
    SDL_free(skel);
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

    /* Compute global transforms. */
    jce_mat4 *globals = (jce_mat4 *)SDL_malloc(count * sizeof(jce_mat4));
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

    SDL_free(globals);
}
