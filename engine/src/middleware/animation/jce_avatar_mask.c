/*
 * jce_avatar_mask.c  Stub per-bone blend weight mask.
 */

#include <jce/middleware/animation/jce_avatar_mask.h>
#include "os/core/jce_memory.h"
#include <stddef.h>

struct JceAvatarMask { int ref; };

JceAvatarMask *jce_avatar_mask_load(const char *path)
{
    (void)path;
    JceAvatarMask *m = (JceAvatarMask *)JCE_CALLOC(1, sizeof(JceAvatarMask));
    if (m) m->ref = 1;
    return m;
}

void jce_avatar_mask_unload(JceAvatarMask *m)
{
    if (!m) return;
    if (--m->ref <= 0) JCE_FREE(m);
}

float jce_avatar_mask_weight(const JceAvatarMask *m, uint32_t bone_index)
{
    (void)m;
    (void)bone_index;
    return 1.0f;
}

void jce_avatar_mask_set_weight(JceAvatarMask *m, uint32_t bone_index, float w)
{
    (void)m;
    (void)bone_index;
    (void)w;
}
