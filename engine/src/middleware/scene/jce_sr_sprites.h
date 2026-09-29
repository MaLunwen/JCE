/*
 * jce_sr_sprites.h  The 2D pass entry point.
 *
 * Deliberately NOT in jce_sr_internal.h: this has exactly one caller
 * (jce_sr_draw.c) and is not part of the renderer-wide internal surface.
 * jce_sr_internal.h is also one of the size-frozen files, and a shared
 * header is the wrong place to spend that budget on a single-caller
 * prototype.
 */
#ifndef JCE_SR_SPRITES_H
#define JCE_SR_SPRITES_H

#include "jce_sr_internal.h"

/* Draw entity `e` if it is a 2D drawable (sprite animator / sprite /
 * billboard).  Returns true when it handled the entity, i.e. the caller
 * should skip the rest of the type-dispatch ladder. */
bool sr_draw_2d_entity(JceSceneRenderer *sr, JceScene *scene, JceEntity e,
                       const JceSceneRenderConfig *cfg, bool kc_fast,
                       jce_mat4 model, const JceCamera *camera,
                       uint16_t view_id);

#endif /* JCE_SR_SPRITES_H */
