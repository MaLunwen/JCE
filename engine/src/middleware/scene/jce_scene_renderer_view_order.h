/*
 * jce_scene_renderer_view_order.h  Pure bgfx view-order helper.
 */

#ifndef JCE_SCENE_RENDERER_VIEW_ORDER_H
#define JCE_SCENE_RENDERER_VIEW_ORDER_H

#include <stdbool.h>
#include <stdint.h>

/* 64 (was 32): omnidirectional point shadows (#7) add a sparse cube-tile view
 * band (base+100..) on top of the contiguous color/shadow/fog range. */
#define JCE_SCENE_RENDERER_VIEW_ORDER_MAX 64u

typedef struct {
    uint16_t first;
    uint16_t count;
    uint16_t order[JCE_SCENE_RENDERER_VIEW_ORDER_MAX];
} JceSceneRendererViewOrder;

bool jce_scene_renderer_view_order_build(uint16_t view_id_base,
                                         bool include_shadow_views,
                                         uint8_t csm_cascade_count,
                                         bool include_fog_views,
                                         bool include_gpu_particle_view,
                                         bool include_gpu_cull_view,
                                         bool include_point_cube_views,
                                         bool include_dyn_csm_views,
                                         JceSceneRendererViewOrder *out);

#endif /* JCE_SCENE_RENDERER_VIEW_ORDER_H */
