/*
 * jce_renderer_ecs.h — flecs ECS adapter for renderer post-effects.
 *
 * Bridges screen-space-reflections and volumetric fog into a flecs
 * world. Both effects are screen-wide (singleton-style), but exposing
 * them as components lets gameplay/levels enable/tweak them per camera
 * entity without bespoke API plumbing.
 *
 * Components (typically attached to a single "active camera" entity):
 *   JceSsrComponent              — params + last-set-revision tracking
 *   JceVolumetricFogComponent    — params + last-set-revision tracking
 *
 * Per-frame ticks:
 *   jce_renderer_ecs_tick_ssr(scene_textures, view, proj, first_view_id)
 *   jce_renderer_ecs_tick_fog(depth_tex,      view, proj, first_view_id)
 *
 * Each tick walks the corresponding component (expecting one entity),
 * pushes any changed params, and renders the effect. With no matching
 * entity present, the tick is a no-op (effect disabled this frame).
 *
 * Thread-safety: single-threaded; call from the bgfx-owning thread.
 *
 * Example:
 *   JceRendererEcs *re = jce_renderer_ecs_create(world, ssr, fog);
 *   ecs_entity_t cam = ecs_new(world);
 *   ecs_set_ptr(world, cam, JceSsrComponent, &(JceSsrComponent){
 *       .params = jce_ssr_default_params(), .params_dirty = true,
 *   });
 *   ecs_set_ptr(world, cam, JceVolumetricFogComponent,
 *               &(JceVolumetricFogComponent){
 *                   .params = jce_volumetric_fog_default_params(),
 *                   .params_dirty = true });
 *
 *   for (;;) {
 *       jce_renderer_ecs_tick_ssr(re, color, depth, normal,
 *                                 &view, &proj, view_id);
 *       jce_renderer_ecs_tick_fog(re, depth, &view, &proj, view_id+1);
 *   }
 *
 * Layer: renderer.
 */
#ifndef JCE_RENDERER_ECS_H
#define JCE_RENDERER_ECS_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_ssr.h>
#include <jce/renderer/jce_volumetric_fog.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ECS world is passed as an opaque void* (from jce_scene_get_world());
 * the concrete flecs type is private and must not appear in the public ABI. */
typedef struct JceRendererEcs JceRendererEcs;

typedef struct {
    JceSsrParams params;
    bool         params_dirty;
    bool         enabled;       /* if false, tick skips the effect */
} JceSsrComponent;

typedef struct {
    JceVolumetricFogParams params;
    bool                   params_dirty;
    bool                   enabled;
} JceVolumetricFogComponent;

JCE_API JceRendererEcs *jce_renderer_ecs_create(void *w,
                                                JceSsr *ssr,
                                                JceVolumetricFog *fog);
JCE_API void            jce_renderer_ecs_destroy(JceRendererEcs *re);

JCE_API void jce_renderer_ecs_tick_ssr(JceRendererEcs *re,
                                       uint16_t color_tex_handle,
                                       uint16_t depth_tex_handle,
                                       uint16_t normal_tex_handle,
                                       const jce_mat4 *view,
                                       const jce_mat4 *proj,
                                       uint16_t first_view_id);

JCE_API void jce_renderer_ecs_tick_fog(JceRendererEcs *re,
                                       uint16_t depth_tex_handle,
                                       const jce_mat4 *view,
                                       const jce_mat4 *proj,
                                       uint16_t first_view_id);

#ifdef __cplusplus
}
#endif

#endif /* JCE_RENDERER_ECS_H */
