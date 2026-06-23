/*
 * jce_anim_retarget.h  Skeleton animation retargeting (internal).
 *
 * Plays a clip authored for a SOURCE skeleton S on a DIFFERENT TARGET
 * skeleton T by transferring a sampled pose bone-by-bone using a name-based
 * joint map and a BIND-POSE-RELATIVE rotation transfer.  This lets a single
 * humanoid run/idle/walk cycle drive many character variants whose rest
 * orientations and proportions differ.
 *
 * The rotation transfer expresses the source bone's bind-relative local
 * rotation in the TARGET bone's bind frame, so it is correct even when the
 * two rigs were authored with different rest orientations:
 *
 *     R_dst_local = R_dst_bind * conj(R_src_bind) * R_src_anim
 *
 * When the two skeletons share the same bind orientation this reduces to a
 * straight copy (R_dst_local == R_src_anim).  Translation is rotation-only by
 * default: every non-root bone keeps its DST bind translation (limb lengths
 * come from the target rig), while the ROOT (hips) translation is copied and
 * scaled by the bind-pose height ratio so the character neither shrinks nor
 * drifts.  Scale always keeps the DST bind scale.  DST bones with no matching
 * source name inherit the DST rest pose.
 *
 * This header is an INTERNAL src-side header (like jce_anim_ozz.h): it is
 * included by jce_anim_retarget.c and by the unit test.  It is NOT part of the
 * public <jce/api_animation.h> surface, mirroring how the ozz bridge stays
 * internal — only the data the retargeter needs (the public JceSkeleton API)
 * crosses the boundary.
 *
 * Layer: Animation (Layer 4) -- internal.
 */

#ifndef JCE_ANIM_RETARGET_H
#define JCE_ANIM_RETARGET_H

#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_skinned_mesh.h>   /* JCE_MAX_BONES */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Retarget map                                                        */
/* ================================================================== */

/* Precomputed, name-based bone correspondence + cached bind rotations
 * between a source skeleton and a target skeleton.  Fixed-size (no per-call
 * allocation in the hot path); created once when a clip is bound to a variant.
 *
 * Indexed by TARGET joint index in [0, dst_count):
 *   src_of_dst[i]  = source joint index feeding target joint i, or -1 if the
 *                    target bone has no same-named source bone.
 *   src_bind_rot[i]/dst_bind_rot[i] = cached rest-pose rotations used by the
 *                    bind-pose-relative transfer (identity for unmatched bones).
 */
typedef struct {
    const JceSkeleton *src;        /* borrowed; caller retains ownership */
    const JceSkeleton *dst;        /* borrowed; caller retains ownership */
    uint32_t           src_count;
    uint32_t           dst_count;

    int      src_of_dst[JCE_MAX_BONES];   /* per dst joint: matched src idx, -1 */
    jce_quat src_bind_rot[JCE_MAX_BONES]; /* per dst joint: src bind rotation   */
    jce_quat dst_bind_rot[JCE_MAX_BONES]; /* per dst joint: dst bind rotation   */

    int      dst_root;             /* dst root joint index (parent < 0), or -1 */
    int      src_root;             /* the src joint feeding dst_root, or -1    */
    float    root_height_ratio;    /* dst_root_height / src_root_height (or 1) */
} JceAnimRetargetMap;

/* Build a retarget map: for every DST joint, look up its name in SRC.
 * Caches the source/target bind rotations and the root height ratio.
 * `src` and `dst` are borrowed (not copied, not freed by destroy); they must
 * outlive the map.  Returns NULL on allocation failure or NULL inputs, or if
 * either skeleton exceeds JCE_MAX_BONES joints. */
JceAnimRetargetMap *jce_anim_retarget_map_create(const JceSkeleton *src,
                                                 const JceSkeleton *dst);

/* Free a retarget map (does not touch the borrowed skeletons). NULL-safe. */
void jce_anim_retarget_map_destroy(JceAnimRetargetMap *map);

/* Number of TARGET joints actually mapped to a SOURCE joint (diagnostic). */
uint32_t jce_anim_retarget_mapped_count(const JceAnimRetargetMap *map);

/* The source joint feeding target joint `dst_index`, or -1 if unmatched / OOB. */
int jce_anim_retarget_source_of(const JceAnimRetargetMap *map,
                                uint32_t dst_index);

/* ================================================================== */
/* Pose transfer                                                       */
/* ================================================================== */

/* Retarget one sampled pose.
 *
 * src_locals : [src_count] per-joint LOCAL transforms for the source skeleton,
 *              e.g. produced by jce_anim_clip_sample(clip, t, ...) against the
 *              source skeleton's rest TRS.
 * out_dst_locals : [dst_count] per-joint LOCAL transforms for the target
 *              skeleton, ready to feed jce_skeleton_evaluate(dst, ...).
 *
 * Per target joint i with src = src_of_dst[i] >= 0:
 *   ROTATION    : R_dst_bind * conj(R_src_bind) * R_src_anim  (bind-relative)
 *   TRANSLATION : root  -> src local translation * root_height_ratio
 *                 other -> dst bind translation (rotation-only retarget)
 *   SCALE       : dst bind scale
 * Unmatched target joints get the dst rest-pose local transform.
 *
 * No allocation; out_dst_locals must hold at least dst_count entries. */
void jce_anim_retarget_pose(const JceAnimRetargetMap *map,
                            const jce_mat4 *src_locals,
                            jce_mat4 *out_dst_locals);

#ifdef __cplusplus
}
#endif

#endif /* JCE_ANIM_RETARGET_H */
