/* Conservative presence admission; lighting/particles retain the global list. */
#ifndef JCE_SR_NODRAW_H
#define JCE_SR_NODRAW_H
#include <jce/middleware/scene/jce_component_registry.h>

static inline bool sr_nodraw_cache_current(int snapshot, int current)
{
    return snapshot >= 0 && snapshot == current;
}

/* A callback can append registry rows during dispatch. Never re-admit this frame. */
static inline bool sr_nodraw_dispatch_current(bool valid, int snapshot, int current)
{
    return valid && sr_nodraw_cache_current(snapshot, current);
}

/* Invalidate all existing structural-keyed metadata before admitting new kinds. */
static inline bool sr_nodraw_refresh_registry(JceScene *scene, int *snapshot, int current)
{
    if (!scene || !snapshot || current < 0 || *snapshot == current) return false;
    jce_scene_invalidate_world_cache(scene);
    *snapshot = current;
    return true;
}

static inline bool sr_nodraw_flags(uint64_t flags, bool unknown_present)
{
    const uint64_t inert = JCE_COMP_FLAG_TRANSFORM | JCE_COMP_FLAG_EDITOR_META
        | JCE_COMP_FLAG_TAG | JCE_COMP_FLAG_LAYER;
    return !unknown_present && (flags & ~inert) == 0;
}

static inline bool sr_entity_has_no_draw(JceScene *scene, JceEntity entity)
{
    const uint64_t flags = jce_scene_get_component_flags(scene, entity);
    if (!sr_nodraw_flags(flags, false)) return false;
    /* Presence cannot be certified by a legacy mask for post-64/custom rows.
     * Fail open for every such component rather than guess its semantics. */
    for (int id = 0; id < jce_component_count(); ++id)
        if (!jce_component_legacy_flag(id) && jce_scene_has_comp(scene, entity, id))
            return false;
    return true;
}
#endif
