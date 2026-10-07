/* Shared identity of retained camera depth; independent of TAA velocity. */
#include "jce_sr_internal.h"

SrOcclusionHistoryKey sr_depth_history_key(JceSceneRenderer *sr,
    JceScene *scene, const void *owner, uint16_t view_id, uint16_t target,
    uint32_t width, uint32_t height, const jce_mat4 *view, const jce_mat4 *proj)
{
    SrOcclusionHistoryKey key = {0};
    key.metadata.scene = scene;
    key.metadata.list_generation = jce_scene_get_roster_epoch(scene);
    key.metadata.structural_generation = jce_scene_get_structural_epoch(scene);
    key.metadata.enable_generation = jce_scene_get_enable_gen(scene);
    key.metadata.component_generation = jce_scene_get_cull_data_gen(scene);
    key.culler = owner;
    key.transform_generation = jce_scene_get_xform_counter(scene);
    key.content_generation = sr->dc_content_gen;
    key.width = width;
    key.height = height;
    key.camera_mask = sr->frame_culling_mask;
    key.frame_buffer = target;
    key.view_id = view_id;
    memcpy(key.view, JCE_M4_PTR(*view), sizeof(key.view));
    memcpy(key.projection, JCE_M4_PTR(*proj), sizeof(key.projection));
    return key;
}

void sr_depth_history_prepare(JceSceneRenderer *sr, JceScene *scene,
    uint16_t view_id, const jce_mat4 *view, const jce_mat4 *proj)
{
    SrOcclusionHistoryKey key = sr_depth_history_key(sr, scene, sr, view_id,
        sr->ssao_depth_tex.idx, sr->ssao_w, sr->ssao_h, view, proj);
    /* Prior depth is unsafe on camera/occluder/content changes. Animated or
     * morphed palettes can change without a scene transform notification;
     * keep that scene visible until a deformation history is available. */
    sr->depth_history_reused = sr_depth_history_reusable(&sr->depth_history,
        &key, sr->anim_storage.count != 0);
    sr->hiz_depth_vp = jce_m4_multiply(proj, view);
}
