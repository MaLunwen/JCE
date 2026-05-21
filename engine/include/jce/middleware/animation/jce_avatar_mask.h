/*
 * jce_avatar_mask.h  Per-bone weight mask for blending (stub for P5).
 */

#ifndef JCE_AVATAR_MASK_H
#define JCE_AVATAR_MASK_H

#include <jce/os/core/jce_defs.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceAvatarMask JceAvatarMask;

JCE_API JceAvatarMask *jce_avatar_mask_load(const char *path);
JCE_API void           jce_avatar_mask_unload(JceAvatarMask *m);

JCE_API float          jce_avatar_mask_weight(const JceAvatarMask *m, uint32_t bone_index);
JCE_API void           jce_avatar_mask_set_weight(JceAvatarMask *m, uint32_t bone_index, float w);

JCE_EXTERN_C_END

#endif /* JCE_AVATAR_MASK_H */
