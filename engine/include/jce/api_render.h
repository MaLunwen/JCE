/*
 * api_render.h  Layer 4 — Render abstraction.
 *
 * Declarative frame graph and sorted draw-call queue.
 * Wraps bgfx views into a higher-level render pipeline.
 *
 * STATUS: Implemented — declarative frame graph and sorted draw-call queue.
 */

#ifndef JCE_API_RENDER_H
#define JCE_API_RENDER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/renderer/jce_ies_profile.h>
#include <jce/renderer/jce_fullscreen_effect.h>
#include <jce/renderer/jce_render_readback.h>
#include <jce/renderer/jce_material_registry.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_quality_preset.h>
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_render_queue.h>
#include <jce/renderer/jce_reflection_probe_bake.h>
#include <jce/renderer/jce_scene_pick.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/renderer/jce_volume_profile.h>

/* Completed 2026-08-31.  These headers export JCE_API symbols and were
 * reachable from NO umbrella, so §4's promise -- `#include <jce/api.h>`
 * gives you the whole engine -- did not hold for them.  Several are named
 * in §4's own table by capability.
 */
#include <jce/renderer/jce_debug_draw.h>
#include <jce/renderer/jce_forwardplus.h>
#include <jce/renderer/jce_gi_probes.h>
#include <jce/renderer/jce_gpu_capture.h>
#include <jce/renderer/jce_gpu_particles.h>
#include <jce/renderer/jce_gpu_scene.h>
#include <jce/renderer/jce_ibl.h>
#include <jce/renderer/jce_image.h>
#include <jce/renderer/jce_impostor.h>
#include <jce/renderer/jce_light_cluster.h>
#include <jce/renderer/jce_lightmapper.h>
#include <jce/renderer/jce_local_shadow.h>
#include <jce/renderer/jce_render_preview.h>
#include <jce/renderer/jce_render_settings.h>
#include <jce/renderer/jce_renderer_ecs.h>
#include <jce/renderer/jce_skin_palette.h>
#include <jce/renderer/jce_skybox.h>
#include <jce/renderer/jce_sprite.h>
#include <jce/renderer/jce_sprite_batch.h>
#include <jce/renderer/jce_ssao.h>
#include <jce/renderer/jce_planar_reflection.h>
#include <jce/renderer/jce_ssgi.h>
#include <jce/renderer/jce_ssr.h>
#include <jce/renderer/jce_taa.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_RENDER_H */
