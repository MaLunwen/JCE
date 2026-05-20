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

#include <jce/middleware/scene/jce_camera_helpers.h>
#include <jce/middleware/scene/jce_decal_projector.h>
#include <jce/middleware/scene/jce_particle_force_fields.h>
#include <jce/middleware/scene/jce_particle_modules.h>
#include <jce/middleware/scene/jce_postfx_stack.h>
#include <jce/middleware/scene/jce_prefab.h>
#include <jce/middleware/scene/jce_prefab_nested.h>
#include <jce/middleware/scene/jce_prefab_overrides.h>
#include <jce/middleware/scene/jce_quality_apply.h>
#include <jce/middleware/scene/jce_quality_settings.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_async.h>
#include <jce/middleware/scene/jce_space_partition.h>
#include <jce/middleware/scene/jce_sprite_animator.h>
#include <jce/middleware/scene/jce_terrain_foliage.h>
#include <jce/middleware/scene/jce_vcam_blend.h>
#include <jce/middleware/scene/jce_vcam_confiner.h>
#include <jce/middleware/scene/jce_vcam_dolly_group.h>
#include <jce/middleware/scene/jce_vcam_freelook_apply.h>
#include <jce/middleware/scene/jce_vcam_impulse.h>
#include <jce/middleware/scene/jce_volume_stack_manager.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_SCENE_H */
