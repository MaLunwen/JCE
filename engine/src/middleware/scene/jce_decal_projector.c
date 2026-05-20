/*
 * jce_decal_projector.c  Default-init helper.  Lifecycle / spawn /
 * despawn is driven by the scene system in a host build (consumes
 * jce_decals.c API).
 */

#include <jce/middleware/scene/jce_decal_projector.h>

#include <string.h>

void jce_decal_projector_init(JceDecalProjectorComponent *p)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->size[0]            = 1.0f;
    p->size[1]            = 1.0f;
    p->size[2]            = 1.0f;
    p->target_layer_mask  = 0xFFFFFFFFu;
    p->fade_dist_start    = 25.0f;
    p->fade_dist_end      = 50.0f;
    p->color[0] = p->color[1] = p->color[2] = p->color[3] = 1.0f;
    p->auto_orient        = true;
    p->runtime_handle     = 0;
}
