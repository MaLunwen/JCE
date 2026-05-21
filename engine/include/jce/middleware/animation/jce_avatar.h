/*
 * jce_avatar.h  Skeleton + humanoid rig mapping asset (stub for P5).
 */

#ifndef JCE_AVATAR_H
#define JCE_AVATAR_H

#include <jce/os/core/jce_defs.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceAvatarAsset JceAvatarAsset;

JCE_API JceAvatarAsset *jce_avatar_load(const char *path);
JCE_API void            jce_avatar_unload(JceAvatarAsset *a);

JCE_API uint32_t        jce_avatar_bone_count(const JceAvatarAsset *a);
JCE_API const char     *jce_avatar_bone_name (const JceAvatarAsset *a, uint32_t i);

JCE_EXTERN_C_END

#endif /* JCE_AVATAR_H */
