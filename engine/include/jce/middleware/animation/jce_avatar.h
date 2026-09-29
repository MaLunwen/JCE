/*
 * jce_avatar.h  A skeleton's humanoid rig mapping, as an asset.
 *
 * An avatar answers one question about one rig: which joint plays each
 * humanoid role (jce_humanoid.h).  It is what JceAvatarComponent.avatar_path
 * names, and what makes a clip authored on one skeleton playable on another.
 *
 * BY NAME, NOT BY INDEX.  The file stores joint NAMES, because an index is
 * meaningless outside the exact skeleton it was taken from -- re-export the
 * model with one more bone and every index shifts.  Binding a saved avatar to
 * a skeleton is therefore a lookup, and a bone whose joint is gone comes back
 * unmapped rather than pointing at whatever now occupies that slot.
 */

#ifndef JCE_AVATAR_H
#define JCE_AVATAR_H

#include <jce/middleware/animation/jce_humanoid.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceAvatarAsset JceAvatarAsset;
typedef struct JceSkeleton    JceSkeleton;

/* Build an avatar by auto-mapping `skel`'s joint names.  Returns NULL on a
 * NULL skeleton.  A skeleton nothing matches still produces an avatar -- with
 * zero mapped bones, which jce_avatar_mapped_count reports and which is the
 * honest answer for a rig that is not humanoid. */
JCE_API JceAvatarAsset *jce_avatar_build(const JceSkeleton *skel);

/* Load a .avatar (JSON: {"bones":{"<Role>":"<joint name>", ...}}).
 *
 * Returns NULL when the file is missing or unparseable -- NOT an empty asset.
 * This function used to return an allocated empty struct, which made every
 * caller's `if (!asset)` pass and turned "there is no such file" into
 * something indistinguishable from success. */
JCE_API JceAvatarAsset *jce_avatar_load(const char *path);

/* Write the avatar as a .avatar file.  Returns false on a write failure. */
JCE_API bool jce_avatar_save(const JceAvatarAsset *a, const char *path);

JCE_API void jce_avatar_unload(JceAvatarAsset *a);

/* Bind a loaded avatar to a skeleton: resolve its stored joint NAMES to that
 * skeleton's indices and read back the rest pose it needs to retarget.  The
 * map is owned by the avatar and valid until it is unloaded or re-bound.
 *
 * Returns the number of roles resolved.  An avatar built by jce_avatar_build
 * is already bound to the skeleton it was built from. */
JCE_API uint32_t jce_avatar_bind(JceAvatarAsset *a, const JceSkeleton *skel);

/* The mapping, or NULL.  Bound to a skeleton by build() or bind(); before
 * either, the joint indices are all -1. */
JCE_API const JceHumanoidMap *jce_avatar_map(const JceAvatarAsset *a);

/* How many humanoid roles this avatar maps (0..JCE_HB_COUNT). */
JCE_API uint32_t jce_avatar_mapped_count(const JceAvatarAsset *a);

/* The joint NAME this avatar assigns to `bone`, or NULL when unmapped. */
JCE_API const char *jce_avatar_joint_name(const JceAvatarAsset *a,
                                          JceHumanoidBone bone);

/* ── Legacy shape, kept because callers exist ─────────────────────────
 * bone_count/bone_name walked "the avatar's bones" back when the stub had
 * none.  They now walk the MAPPED humanoid roles, which is the only bone list
 * an avatar has ever had a right to claim. */
JCE_API uint32_t    jce_avatar_bone_count(const JceAvatarAsset *a);
JCE_API const char *jce_avatar_bone_name (const JceAvatarAsset *a, uint32_t i);

JCE_EXTERN_C_END

#endif /* JCE_AVATAR_H */
