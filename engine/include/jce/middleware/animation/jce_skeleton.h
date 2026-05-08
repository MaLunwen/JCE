/*
 * jce_skeleton.h  Bone/joint hierarchy for skeletal animation.
 *
 * Stores the bind-pose skeleton: joint names, parent indices,
 * inverse bind matrices, and rest-pose local transforms.
 * Evaluates a set of local transforms into skinning matrices.
 *
 * Layer: Renderer (Layer 1).
 */

#ifndef JCE_SKELETON_H
#define JCE_SKELETON_H


#include <jce/renderer/jce_skinned_mesh.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceSkeleton JceSkeleton;

/* ================================================================== */
/* Joint descriptor                                                    */
/* ================================================================== */

typedef struct {
    char     name[64];
    int16_t  parent;              /* -1 = root joint */
    jce_mat4 inverse_bind_matrix; /* transforms from mesh space to bone-local space */
    jce_mat4 local_transform;     /* default rest pose (T * R * S) */
    jce_vec3 rest_translation;    /* rest-pose T from glTF node */
    jce_quat rest_rotation;       /* rest-pose R from glTF node */
    jce_vec3 rest_scale;          /* rest-pose S from glTF node */
} JceJoint;

/* ================================================================== */
/* Skeleton API                                                        */
/* ================================================================== */

/* Create a skeleton from an array of joint descriptors.
 * Joints must be ordered so that a parent always precedes its children.
 * Copies the data; caller retains ownership. */
JCE_API JceSkeleton *jce_skeleton_create(const JceJoint *joints, uint32_t num_joints);

/* Destroy a skeleton. */
JCE_API void jce_skeleton_destroy(JceSkeleton *skel);

/* Get the number of joints. */
JCE_API uint32_t jce_skeleton_joint_count(const JceSkeleton *skel);

/* Find a joint index by name. Returns -1 if not found. */
JCE_API int jce_skeleton_find_joint(const JceSkeleton *skel, const char *name);

/* Get the rest-pose local transforms (array of [joint_count] mat4). */
JCE_API const jce_mat4 *jce_skeleton_rest_pose(const JceSkeleton *skel);

/* Get rest-pose TRS arrays (each has [joint_count] elements).
 * These are the original glTF node TRS values, avoiding decomposition. */
JCE_API void JCE_CALL jce_skeleton_rest_trs(const JceSkeleton *skel,
                                            const jce_vec3 **out_translations,
                                            const jce_quat **out_rotations,
                                            const jce_vec3 **out_scales);

/* Evaluate skinning matrices from local transforms.
 *
 * local_transforms: per-joint local transforms (e.g. from animation sampling).
 *                   If NULL, uses the skeleton's rest pose.
 * out_matrices:     output array of [max_joints] mat4.
 *                   Each = globalTransform[i] * inverseBindMatrix[i].
 *                   Ready for upload to GPU via jce_skinned_mesh_set_bones().
 * max_joints:       capacity of out_matrices (clamped to skeleton joint count). */
JCE_API void JCE_CALL jce_skeleton_evaluate(const JceSkeleton *skel,
                                            const jce_mat4 *local_transforms,
                                            jce_mat4 *out_matrices,
                                            uint32_t max_joints);

/* Return the inverse bind matrix for joint joint_idx.
 * Returns identity if index is out of range.
 * Useful for recovering the animated joint world transform from a skin matrix:
 *   joint_global = skin_matrix[i] * inverse(jce_skeleton_get_inverse_bind(skel, i)) */
JCE_API jce_mat4 jce_skeleton_get_inverse_bind(const JceSkeleton *skel, uint32_t joint_idx);

JCE_EXTERN_C_END

#endif /* JCE_SKELETON_H */
