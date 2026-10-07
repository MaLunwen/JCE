/* One owner for complete per-instance retirement and scene membership. */
#include "jce_sr_internal.h"

static void release_instance(void *value)
{
    SrAnimInstance *a = value;
    if (a->player) jce_anim_player_destroy(a->player);
    if (a->sm_binding) jce_anim_sm_binding_destroy(a->sm_binding);
    if (a->blend_tree) jce_anim_blend_tree_destroy(a->blend_tree);
    JCE_FREE(a->ev_pool);
    if (a->avatar_mask) jce_avatar_mask_unload(a->avatar_mask);
    for (int i = 0; i < SR_AVATAR_MAX_LAYERS; ++i)
        if (a->layer_mask[i]) jce_avatar_mask_unload(a->layer_mask[i]);
    if (a->retarget_map) jce_anim_retarget_map_destroy(a->retarget_map);
    if (a->aoc) jce_anim_override_controller_unload(a->aoc);
    sr_release_file_clips(a);
    /* Only initialized records exist in this store. Their invalid morph
     * handles were installed before any draw could observe the record. */
    if (a->used) sr_free_morph_vbs(a);
}

void sr_anim_instances_destroy(JceSceneRenderer *sr)
{
    sr_anim_storage_destroy(&sr->anim_storage, release_instance);
    JCE_FREE(sr->anim_requests);
    sr->anim_requests = NULL;
    sr->anim_request_capacity = 0;
    sr->anim_storage_scene = NULL;
}

void sr_anim_instances_prune(JceSceneRenderer *sr, JceScene *scene)
{
    /* Entity IDs are scene-local. A renderer reused for a different scene
     * must not carry the old scene's playback or borrowed skeleton state. */
    if (sr->anim_storage_scene != scene) {
        sr_anim_storage_destroy(&sr->anim_storage, release_instance);
        sr->anim_storage_scene = scene;
    }
    /* O(live animators), never O(all scene entities). Disabled/off-screen
     * actors retain their clock; removed components and destroyed actors do not. */
    for (uint32_t i = 0; i < sr->anim_storage.count;) {
        JceEntity entity = sr->anim_storage.records[i].entity;
        const JceSkeletalAnimatorComponent *sa = jce_scene_entity_alive(scene, entity)
            ? jce_scene_get_skeletal_animator(scene, entity) : NULL;
        if (!sa || !sa->skeleton_path[0])
            sr_anim_storage_remove(&sr->anim_storage, entity, release_instance);
        else
            ++i;
    }
}
