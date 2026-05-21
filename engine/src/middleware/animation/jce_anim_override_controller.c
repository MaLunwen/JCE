/*
 * jce_anim_override_controller.c  Stub animator override controller asset.
 */

#include <jce/middleware/animation/jce_anim_override_controller.h>
#include "os/core/jce_memory.h"
#include <stddef.h>

struct JceAnimOverrideController { int ref; };

JceAnimOverrideController *jce_anim_override_controller_load(const char *path)
{
    (void)path;
    JceAnimOverrideController *c = (JceAnimOverrideController *)JCE_CALLOC(1, sizeof(JceAnimOverrideController));
    if (c) c->ref = 1;
    return c;
}

void jce_anim_override_controller_unload(JceAnimOverrideController *c)
{
    if (!c) return;
    if (--c->ref <= 0) JCE_FREE(c);
}

uint32_t jce_anim_override_controller_pair_count(const JceAnimOverrideController *c)
{
    (void)c;
    return 0;
}

const char *jce_anim_override_controller_original(const JceAnimOverrideController *c, uint32_t i)
{
    (void)c;
    (void)i;
    return NULL;
}

const char *jce_anim_override_controller_override(const JceAnimOverrideController *c, uint32_t i)
{
    (void)c;
    (void)i;
    return NULL;
}
