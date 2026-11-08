/*
 * jce_scene_renderer_view_order.h  Pure bgfx view-order helper.
 */

#ifndef JCE_SCENE_RENDERER_VIEW_ORDER_H
#define JCE_SCENE_RENDERER_VIEW_ORDER_H

#include <jce/renderer/jce_views.h>

#include <stdbool.h>
#include <stdint.h>

/* 64 (was 32): omnidirectional point shadows (#7) add a sparse cube-tile view
 * band (base+100..) on top of the contiguous color/shadow/fog range. */
/* Big enough for a whole remap WINDOW, not just the ids the builder names:
 * the array must be a permutation of [first, highest], so the point-light cube
 * band (base+100..116) alone needs 117 entries. */
#define JCE_SCENE_RENDERER_VIEW_ORDER_MAX 128u
/* Native product bgfx configuration: valid view IDs are 0..255.  A remap
 * window must not exceed this domain even when its own array still fits. */
#define JCE_SCENE_RENDERER_VIEW_ID_LIMIT 256u

/* Dynamic CSM uses the public reservation: offset 52..56, with its depth
 * clear and four cascade tiles. Keep this alias single-sourced with the
 * actual shadow pass; view order cannot repair framebuffer-slot collisions.
 *
 * bgfx remaps the whole contiguous [first, first+count) window, including
 * unnamed filler views. The current Scene base 3 and Game base 120 are 117
 * slots apart. The sparse cube band at offsets 100..116 fits that gap; no
 * added band may extend one viewport's window into its neighbour's.
 * The builder closes each admitted window into a complete permutation. */
#define JCE_VIEW_DYN_CSM_OFFSET JCE_VIEW_SR_DYN_CSM_OFFSET

typedef struct {
    uint16_t first;
    uint16_t count;
    /* How many of `order` are views the scene renderer OWNS, including the
     * reserved current-camera depth slot. Empty reservations submit no work.
     * The rest are filler the builder appends to close the remap window into a
     * permutation (see the build function) -- ids that belong to postfx, the
     * editor overlays and whatever else lives in the span.  Sort position is
     * all we take from them; their view STATE belongs to their real owner.
     *
     * The distinction is not cosmetic: jce_view_bands_claim() reads this array
     * to declare ownership, and claiming the filler made the scene renderer
     * collide with postfx on every frame.  A guard that always fires cannot
     * report the collision it exists for. */
    uint16_t named_count;
    uint16_t order[JCE_SCENE_RENDERER_VIEW_ORDER_MAX];
} JceSceneRendererViewOrder;

/* Reserve base+1 before base+0 for any current-camera depth submission,
 * including pipeline-only depth_prepass with every optional band disabled.
 * This sets sort order only: it does not touch or enable an unused view. */
bool jce_scene_renderer_view_order_build(uint16_t view_id_base,
                                         bool include_shadow_views,
                                         uint8_t csm_cascade_count,
                                         bool include_fog_views,
                                         bool include_gpu_particle_view,
                                         bool include_gpu_cull_view,
                                         bool include_point_cube_views,
                                         bool include_dyn_csm_views,
                                         /* Underwater absorption at base+17.
                                          * Gated so a scene with no absorbing
                                          * water keeps a minimal window. */
                                         bool include_underwater_view,
                                         uint8_t camera_overlay_count,
                                         JceSceneRendererViewOrder *out);

/* Keep the pair ordered even in Game frames that submit neither dispatch.
 * A request for any other base is rejected. */
bool jce_scene_renderer_view_order_build_ex(uint16_t view_id_base,
                                            bool include_shadow_views,
                                            uint8_t csm_cascade_count,
                                            bool include_fog_views,
                                            bool include_gpu_particle_view,
                                            bool include_gpu_cull_view,
                                            bool include_point_cube_views,
                                            bool include_dyn_csm_views,
                                            bool include_underwater_view,
                                            uint8_t camera_overlay_count,
                                            JceSceneRendererViewOrder *out);

bool jce_scene_renderer_view_order_build_with_caustic(uint16_t view_id_base,
                                            bool include_shadow_views,
                                            uint8_t csm_cascade_count,
                                            bool include_fog_views,
                                            bool include_gpu_particle_view,
                                            bool include_gpu_cull_view,
                                            bool include_point_cube_views,
                                            bool include_dyn_csm_views,
                                            bool include_underwater_view,
                                            uint8_t camera_overlay_count,
                                            bool include_water_caustics,
                                            JceSceneRendererViewOrder *out);

/* Optics reads opaque color after base+0 and resumes transparency before
 * fog, underwater, overlay and downstream color readers. */
bool jce_scene_renderer_view_order_build_with_optics(uint16_t view_id_base,
                                            bool include_shadow_views,
                                            uint8_t csm_cascade_count,
                                            bool include_fog_views,
                                            bool include_gpu_particle_view,
                                            bool include_gpu_cull_view,
                                            bool include_point_cube_views,
                                            bool include_dyn_csm_views,
                                            bool include_underwater_view,
                                            uint8_t camera_overlay_count,
                                            bool include_water_caustics,
                                            bool include_water_optics,
                                            JceSceneRendererViewOrder *out);

bool jce_scene_renderer_view_order_build_with_shafts(uint16_t view_id_base,
                                            bool include_shadow_views,
                                            uint8_t csm_cascade_count,
                                            bool include_fog_views,
                                            bool include_gpu_particle_view,
                                            bool include_gpu_cull_view,
                                            bool include_point_cube_views,
                                            bool include_dyn_csm_views,
                                            bool include_underwater_view,
                                            uint8_t camera_overlay_count,
                                            bool include_water_caustics,
                                            bool include_water_optics,
                                            bool include_screen_shafts,
                                            JceSceneRendererViewOrder *out);

bool jce_scene_renderer_view_order_build_with_cloud_temporal(uint16_t view_id_base,
                                            bool include_shadow_views,
                                            uint8_t csm_cascade_count,
                                            bool include_fog_views,
                                            bool include_gpu_particle_view,
                                            bool include_gpu_cull_view,
                                            bool include_point_cube_views,
                                            bool include_dyn_csm_views,
                                            bool include_underwater_view,
                                            uint8_t camera_overlay_count,
                                            bool include_water_caustics,
                                            bool include_water_optics,
                                            bool include_screen_shafts,
                                            bool include_toon_hair,
                                            bool include_cloud_temporal,
                                            JceSceneRendererViewOrder *out);

bool jce_scene_renderer_view_order_build_with_toon_hair(uint16_t view_id_base,
                                            bool include_shadow_views,
                                            uint8_t csm_cascade_count,
                                            bool include_fog_views,
                                            bool include_gpu_particle_view,
                                            bool include_gpu_cull_view,
                                            bool include_point_cube_views,
                                            bool include_dyn_csm_views,
                                            bool include_underwater_view,
                                            uint8_t camera_overlay_count,
                                            bool include_water_caustics,
                                            bool include_water_optics,
                                            bool include_screen_shafts,
                                            bool include_toon_hair,
                                            JceSceneRendererViewOrder *out);

typedef struct {
    uint16_t first;
    uint16_t count;
} JceSceneRendererViewOrderTail;

/* bgfx retains the previous permutation. Restore just the retired suffix
 * before installing the new order. The caller owns previous_count per base. */
JceSceneRendererViewOrderTail jce_scene_renderer_view_order_tail_reset(
    const JceSceneRendererViewOrder *order, uint16_t previous_count);

#endif /* JCE_SCENE_RENDERER_VIEW_ORDER_H */
