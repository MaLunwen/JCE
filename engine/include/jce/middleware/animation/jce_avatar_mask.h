/*
 * jce_avatar_mask.h  Per-bone weight mask for layered/additive blending.
 *
 * A mask is a dense per-bone float weight array sized to a skeleton.  Bones
 * that were never explicitly authored default to 1.0 (fully affected).  The
 * weight scales how much of an animation LAYER's delta/override a given bone
 * receives during composition (0 = bone keeps the underlying pose, 1 = bone
 * fully takes the layer).
 *
 * Asset format (.mask) — minimal JSON, documented here so authoring stays
 * dependency-free:
 *
 *   {
 *     "default": 1.0,            // optional; weight for any bone NOT listed
 *     "weights": [
 *       { "index": 3,  "weight": 0.5 },   // address a bone by skeleton index
 *       { "bone": "LeftArm", "weight": 0.0 }  // or by name (needs a skeleton)
 *     ]
 *   }
 *
 * jce_avatar_mask_load() parses index-addressed entries only (no skeleton, so
 * name entries are skipped).  jce_avatar_mask_load_for_skeleton() additionally
 * resolves "bone" name entries against the supplied skeleton.  Either way the
 * "default" key sets the fill value for unlisted bones.
 *
 * Layer: Animation (Layer 3).
 */

#ifndef JCE_AVATAR_MASK_H
#define JCE_AVATAR_MASK_H

#include <jce/os/core/jce_defs.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceAvatarMask JceAvatarMask;
typedef struct JceSkeleton   JceSkeleton;

/* Create an empty mask with `bone_count` bones, all weights = `default_weight`.
 * `bone_count` may be 0 (the mask grows on demand via set_weight). */
JCE_API JceAvatarMask *JCE_CALL jce_avatar_mask_create(uint32_t bone_count,
                                                       float    default_weight);

/* Load a .mask asset.  Index-addressed entries only (name entries skipped, as
 * there is no skeleton to resolve them).  Returns NULL on I/O / parse failure. */
JCE_API JceAvatarMask *jce_avatar_mask_load(const char *path);

/* Load a .mask asset, resolving name-addressed ("bone") entries against the
 * given skeleton (NULL skeleton behaves like jce_avatar_mask_load).  The mask
 * is sized to the skeleton's joint count when one is supplied. */
JCE_API JceAvatarMask *JCE_CALL jce_avatar_mask_load_for_skeleton(
    const char *path, const JceSkeleton *skel);

/* Like jce_avatar_mask_load_for_skeleton but parses an in-memory .mask JSON
 * buffer (single-exe: bytes decompressed from the embedded PAK).  `len` may be
 * 0 to strlen(text). */
JCE_API JceAvatarMask *JCE_CALL jce_avatar_mask_load_for_skeleton_mem(
    const char *text, size_t len, const JceSkeleton *skel);

JCE_API void JCE_CALL jce_avatar_mask_unload(JceAvatarMask *m);

/* Number of bones the mask currently stores explicit weights for. */
JCE_API uint32_t JCE_CALL jce_avatar_mask_count(const JceAvatarMask *m);

/* Per-bone weight.  Returns the mask's default weight for bones beyond the
 * stored range (unset bones), or 1.0 when `m` is NULL. */
JCE_API float JCE_CALL jce_avatar_mask_weight(const JceAvatarMask *m,
                                              uint32_t bone_index);

/* Set the weight of one bone (clamped to [0,1]).  Grows the storage as needed,
 * filling any new gap with the mask's default weight. */
JCE_API void JCE_CALL jce_avatar_mask_set_weight(JceAvatarMask *m,
                                                 uint32_t bone_index, float w);

JCE_EXTERN_C_END

#endif /* JCE_AVATAR_MASK_H */
