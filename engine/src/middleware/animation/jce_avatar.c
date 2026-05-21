/*
 * jce_avatar.c  Stub humanoid rig asset.
 *
 * Real skeleton mapping (ozz-animation backed) lands in P5. The stub lets
 * the ECS component compile + serialize today without dragging in any new
 * third-party code or hot paths.
 */

#include <jce/middleware/animation/jce_avatar.h>
#include "os/core/jce_memory.h"
#include <stddef.h>

struct JceAvatarAsset { int ref; };

JceAvatarAsset *jce_avatar_load(const char *path)
{
    (void)path;
    JceAvatarAsset *a = (JceAvatarAsset *)JCE_CALLOC(1, sizeof(JceAvatarAsset));
    if (a) a->ref = 1;
    return a;
}

void jce_avatar_unload(JceAvatarAsset *a)
{
    if (!a) return;
    if (--a->ref <= 0) JCE_FREE(a);
}

uint32_t jce_avatar_bone_count(const JceAvatarAsset *a)
{
    (void)a;
    return 0;
}

const char *jce_avatar_bone_name(const JceAvatarAsset *a, uint32_t i)
{
    (void)a;
    (void)i;
    return NULL;
}
