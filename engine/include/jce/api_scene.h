/*
 * api_scene.h  Layer 5 — Scene management.
 *
 * ECS-based scene graph (flecs) and spatial partitioning
 * for frustum culling, ray queries, and proximity searches.
 */

#ifndef JCE_API_SCENE_H
#define JCE_API_SCENE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_camera.h>
#include <jce/middleware/scene/jce_script_exports.h>
#include <jce/middleware/scene/jce_scene_reflection_probe.h>
#include <jce/middleware/scene/jce_scene_fullscreen_effect.h>
#include <jce/middleware/scene/jce_scene_async.h>
#include <jce/middleware/scene/jce_scene_compiler.h>
#include <jce/middleware/scene/jce_scene_recipe.h>
#include <jce/middleware/scene/jce_scene_systems.h>
#include <jce/middleware/scene/jce_scene_transaction.h>
#include <jce/middleware/scene/jce_foliage.h>
#include <jce/middleware/scene/jce_space_partition.h>
#include <jce/middleware/scene/jce_terrain.h>
#include <jce/middleware/scene/jce_vcam_system.h>
#include <jce/middleware/scene/jce_water.h>
#include <jce/middleware/scene/jce_water_fft.h>
#include <jce/middleware/scene/jce_water_field.h>

/* Completed 2026-08-31.  These headers export JCE_API symbols and were
 * reachable from NO umbrella, so §4's promise -- `#include <jce/api.h>`
 * gives you the whole engine -- did not hold for them.  Several are named
 * in §4's own table by capability.
 */
#include <jce/middleware/scene/jce_component_registry.h>
#include <jce/middleware/scene/jce_lod.h>
#include <jce/middleware/scene/jce_prefab.h>
#include <jce/middleware/scene/jce_material_override.h>
#include <jce/middleware/scene/jce_scene_probe_capture.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/middleware/scene/jce_scene_sequencer.h>
#include <jce/middleware/scene/jce_sequencer.h>
#include <jce/middleware/scene/jce_str_intern.h>
#include <jce/middleware/scene/jce_tilemap.h>
#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/middleware/scene/jce_water_ripple.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_SCENE_H */
