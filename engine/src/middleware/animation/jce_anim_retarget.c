/*
 * jce_anim_retarget.c  Skeleton animation retargeting implementation.
 *
 * See jce_anim_retarget.h for the contract and the bind-pose-relative rotation
 * transfer derivation.
 */

#include "middleware/animation/jce_anim_retarget.h"

#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "jce_anim_retarget"

/* Quaternion conjugate == inverse for a unit quaternion. Mirrors the helper in
 * jce_animation.c (kept local so this TU has no cross-file math dependency). */
static jce_quat retarget_quat_conjugate(jce_quat q)
{
    return jce_v4(-q.x, -q.y, -q.z, q.w);
}

/* ================================================================== */
/* Map construction                                                    */
/* ================================================================== */

JceAnimRetargetMap *jce_anim_retarget_map_create(const JceSkeleton *src,
                                                 const JceSkeleton *dst)
{
    if (!src || !dst) return NULL;

    uint32_t src_count = jce_skeleton_joint_count(src);
    uint32_t dst_count = jce_skeleton_joint_count(dst);
    if (src_count == 0 || dst_count == 0) return NULL;
    if (src_count > JCE_MAX_BONES || dst_count > JCE_MAX_BONES) {
        LOG_ERROR(LOG_TAG,
                  "skeleton joint count exceeds JCE_MAX_BONES (src=%u dst=%u cap=%d)",
                  src_count, dst_count, JCE_MAX_BONES);
        return NULL;
    }

    JceAnimRetargetMap *map = (JceAnimRetargetMap *)JCE_CALLOC(1, sizeof(*map));
    if (!map) return NULL;

    map->src       = src;
    map->dst       = dst;
    map->src_count = src_count;
    map->dst_count = dst_count;
    map->dst_root  = -1;
    map->src_root  = -1;
    map->root_height_ratio = 1.0f;

    /* Rest-pose rotation/translation arrays (interned in each skeleton). */
    const jce_vec3 *src_rt = NULL; const jce_quat *src_rr = NULL;
    const jce_vec3 *dst_rt = NULL; const jce_quat *dst_rr = NULL;
    jce_skeleton_rest_trs(src, &src_rt, &src_rr, NULL);
    jce_skeleton_rest_trs(dst, &dst_rt, &dst_rr, NULL);

    /* For each DST joint, match by name into SRC and cache bind rotations. */
    for (uint32_t i = 0; i < dst_count; ++i) {
        const char *dst_name = jce_skeleton_joint_name(dst, i);
        int s = dst_name ? jce_skeleton_find_joint(src, dst_name) : -1;

        map->src_of_dst[i]   = s;
        map->dst_bind_rot[i] = dst_rr ? dst_rr[i] : jce_q_identity();
        map->src_bind_rot[i] = (s >= 0 && src_rr) ? src_rr[(uint32_t)s]
                                                  : jce_q_identity();

        if (map->dst_root < 0 && jce_skeleton_joint_parent(dst, i) < 0) {
            map->dst_root = (int)i;
            map->src_root = s;
        }
    }

    /* Root height ratio: ratio of bind-pose root translation magnitudes so the
     * copied hips translation neither inflates nor shrinks the character. Falls
     * back to 1.0 when the source root height is degenerate (~0). */
    if (map->dst_root >= 0 && map->src_root >= 0 && src_rt && dst_rt) {
        float src_h = jce_v3_len(src_rt[(uint32_t)map->src_root]);
        float dst_h = jce_v3_len(dst_rt[(uint32_t)map->dst_root]);
        if (src_h > 1e-6f)
            map->root_height_ratio = dst_h / src_h;
    }

    return map;
}

void jce_anim_retarget_map_destroy(JceAnimRetargetMap *map)
{
    if (!map) return;
    JCE_FREE(map);
}

uint32_t jce_anim_retarget_mapped_count(const JceAnimRetargetMap *map)
{
    if (!map) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < map->dst_count; ++i)
        if (map->src_of_dst[i] >= 0) ++n;
    return n;
}

int jce_anim_retarget_source_of(const JceAnimRetargetMap *map, uint32_t dst_index)
{
    if (!map || dst_index >= map->dst_count) return -1;
    return map->src_of_dst[dst_index];
}

/* ================================================================== */
/* Pose transfer                                                       */
/* ================================================================== */

void jce_anim_retarget_pose(const JceAnimRetargetMap *map,
                            const jce_mat4 *src_locals,
                            jce_mat4 *out_dst_locals)
{
    if (!map || !src_locals || !out_dst_locals) return;

    const jce_mat4 *dst_rest = jce_skeleton_rest_pose(map->dst);

    /* DST rest TRS (bind translation/scale used for non-root rotation-only). */
    const jce_vec3 *dst_rt = NULL; const jce_vec3 *dst_rs = NULL;
    jce_skeleton_rest_trs(map->dst, &dst_rt, NULL, &dst_rs);

    for (uint32_t i = 0; i < map->dst_count; ++i) {
        int s = map->src_of_dst[i];

        /* Unmatched target joints inherit the dst rest pose. */
        if (s < 0) {
            out_dst_locals[i] = dst_rest ? dst_rest[i] : jce_m4_identity();
            continue;
        }

        const jce_mat4 *msrc = &src_locals[(uint32_t)s];

        /* Source animated local rotation (decomposed from the sampled local). */
        jce_quat r_src_anim = jce_m4_to_quat(msrc);

        /* Bind-pose-relative rotation transfer:
         *   R_dst_local = R_dst_bind * conj(R_src_bind) * R_src_anim
         * Identity-reduces to R_src_anim when both bind rotations are equal. */
        jce_quat dst_bind = map->dst_bind_rot[i];
        jce_quat src_bind = map->src_bind_rot[i];
        jce_quat rel = jce_q_multiply(retarget_quat_conjugate(src_bind),
                                      r_src_anim);
        jce_quat r_out = jce_q_normalize(jce_q_multiply(dst_bind, rel));

        /* Translation: copy + height-scale ONLY the root (hips); every other
         * joint keeps its dst bind translation (rotation-only retarget). */
        jce_vec3 t_out;
        if ((int)i == map->dst_root) {
            jce_vec3 t_src = jce_v3(msrc->raw[3][0], msrc->raw[3][1],
                                    msrc->raw[3][2]);
            t_out = jce_v3_scale(t_src, map->root_height_ratio);
        } else {
            t_out = dst_rt ? dst_rt[i]
                           : jce_v3(0.0f, 0.0f, 0.0f);
        }

        /* Scale: keep dst bind scale. */
        jce_vec3 s_out = dst_rs ? dst_rs[i] : jce_v3(1.0f, 1.0f, 1.0f);

        out_dst_locals[i] = jce_m4_from_trs(t_out, r_out, s_out);
    }
}
