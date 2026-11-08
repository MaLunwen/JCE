/* Render-thread model geometry publication and retained CPU state. */
#include "jce_sr_internal.h"
#include "jce_scene_internal.h"

static bool model_subscriber_alive(void *user, uint64_t entity)
{
    return jce_scene_entity_alive(user, entity);
}

void sr_model_bounds_retire_geometry(JceSceneRenderer *sr)
{
    sr->model_bounds_geometry_dirty = false;
    sr->cull_frame_valid = false;
    sr->shadow_space_valid = false;
    sr->shadow_cache_cascades = 0;
    for (int i = 0; i < JCE_CSM_MAX_CASCADES; ++i)
        sr->shadow_cache_valid[i] = false;
    for (size_t i = 0; i < sizeof sr->pre_cache / sizeof sr->pre_cache[0]; ++i)
        sr->pre_cache[i].valid = false;
    sr->depth_history.valid = false;
    sr->occlusion_history.valid = false;
    sr->depth_history_reused = false;
}

void sr_model_bounds_drop_tables(JceSceneRenderer *sr)
{
    sr_bounds_cache_reset(sr);
    sr->model_bounds_focus_collect_valid = false;
    ++sr->model_bounds_focus_revision;
    sr->ecull_frz_valid = false;
    sr->ecull_metadata_valid = false;
    sr->eidx_list_gen = 0;
}

void sr_model_bounds_reset(JceSceneRenderer *sr)
{
    if (!sr->model_bounds_enabled) return;
    SrModelDeps *deps = &sr->model_bounds_deps;
    if (deps->epoch == UINT64_MAX)
        deps->fallback = true;
    else
        sr_model_deps_reset(deps, deps->epoch + 1);
    sr->model_bounds_roster = 0;
    sr_model_bounds_drop_tables(sr);
    sr_model_bounds_retire_geometry(sr);
}

void sr_model_bounds_enter(JceSceneRenderer *sr, JceScene *scene)
{
    if (!sr->model_bounds_enabled) return;
    const uint64_t instance = jce_scene_internal_instance_id(scene);
    if (sr->model_bounds_scene != instance) {
        sr_model_bounds_reset(sr);
        sr->model_bounds_scene = instance;
        sr->resource_prepared_valid = false;
    }
    const uint64_t roster = jce_scene_get_roster_epoch(scene);
    if (sr->model_bounds_roster != roster) {
        SrModelDeps *deps = &sr->model_bounds_deps;
        sr_model_deps_prune(deps, deps->epoch, model_subscriber_alive, scene);
        sr->model_bounds_roster = roster;
    }
}

void sr_model_bounds_note(JceSceneRenderer *sr, const char *path, JceModel *ready)
{
    if (!sr->model_bounds_enabled) return;
    sr->model_bounds_geometry_dirty = true;
    SrModelDeps *deps = &sr->model_bounds_deps;
    if (sr->model_bounds_revision == UINT64_MAX) {
        deps->fallback = true;
        return;
    }
    SrModelDepEvent event = {0};
    event.revision = ++sr->model_bounds_revision;
    event.ready = ready != NULL;
    event.has_aabb = ready && jce_model_get_aabb(ready, event.min, event.max);
    sr_model_deps_publish(deps, deps->epoch, path, &event);
}

void sr_model_bounds_track(JceSceneRenderer *sr, JceEntity entity, const char *path)
{
    if (!sr->model_bounds_enabled) return;
    SrModelDeps *deps = &sr->model_bounds_deps;
    if (path)
        sr_model_deps_bind(deps, deps->epoch, entity, path);
    else if (deps->row_count)
        sr_model_deps_remove(deps, deps->epoch, entity);
}
