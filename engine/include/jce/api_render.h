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

#include <jce/renderer/jce_compute.h>
#include <jce/renderer/jce_frame_debugger.h>
#include <jce/renderer/jce_gi_probes.h>
#include <jce/renderer/jce_light_cookie.h>
#include <jce/renderer/jce_light_probe_eval.h>
#include <jce/renderer/jce_lightmap_uv.h>
#include <jce/renderer/jce_lightmapper_bake.h>
#include <jce/renderer/jce_lightmapper_embree.h>
#include <jce/renderer/jce_material_property_block.h>
#include <jce/renderer/jce_morph_target.h>
#include <jce/renderer/jce_proc_mesh.h>
#include <jce/renderer/jce_reflection_probe.h>
#include <jce/renderer/jce_reflection_probe_bake.h>
#include <jce/renderer/jce_render_graph.h>
#include <jce/renderer/jce_sdf_font.h>
#include <jce/renderer/jce_shader_graph.h>
#include <jce/renderer/jce_shader_graph_nodes.h>
#include <jce/renderer/jce_skinned_morph_pack.h>
#include <jce/renderer/jce_sprite_atlas.h>
#include <jce/renderer/jce_taa_jitter.h>
#include <jce/renderer/jce_tilemap.h>
#include <jce/renderer/jce_render_queue.h>
#include <jce/renderer/jce_scene_renderer.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_RENDER_H */
