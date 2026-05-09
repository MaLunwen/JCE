/*
 * jce_anim_layer_apply.h  Bind ECS layer-state component → layer stack.
 *
 * The editor-authored JceAnimationLayerStateComponent stores per-layer
 * config (clip path / weight / mode) on an entity.  The runtime side
 * needs to:
 *   1. Resolve clip_path strings to JceAnimClip* via the asset system
 *      (caller-supplied resolver — keeps this module decoupled from
 *      whichever asset registry is in use).
 *   2. Push those clips into a JceAnimLayerStack with current weights.
 *   3. Advance per-layer time by dt × speed.
 *
 * This is a thin helper rather than an automated ECS system because
 * different host shells (game runtime vs editor preview) plug clip
 * resolution differently.  Callers loop over animated entities and
 * invoke jce_anim_layer_apply() each frame.
 *
 * Layer: middleware / animation (Layer 3) — public.
 */

#ifndef JCE_ANIM_LAYER_APPLY_H
#define JCE_ANIM_LAYER_APPLY_H

#include <jce/middleware/animation/jce_anim_layers.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

/* Clip resolver callback.  Caller hooks this to whatever asset cache
 * holds JceAnimClips.  Return NULL for missing clips — that layer
 * will be skipped during evaluate. */
typedef const JceAnimClip *(*JceAnimClipResolveFn)(const char *clip_path,
                                                    void *user_data);

/* Per-entity tick: read JceAnimationLayerStateComponent, resolve clip
 * paths via the callback, configure the supplied layer stack, and
 * advance per-layer time by dt × speed.  No-op if either argument is
 * NULL or the entity has no layer-state component.
 *
 * The layer stack must already match the entity's skeleton joint
 * count (created via jce_anim_layer_stack_create earlier and stored
 * by the caller). */
JCE_API void jce_anim_layer_apply(JceScene             *scene,
                                  JceEntity             entity,
                                  JceAnimLayerStack    *stack,
                                  float                 dt,
                                  JceAnimClipResolveFn  resolve,
                                  void                 *resolve_ud);

JCE_EXTERN_C_END

#endif /* JCE_ANIM_LAYER_APPLY_H */
