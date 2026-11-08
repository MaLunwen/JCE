/* jce_sr_model_table.h  Stable-address, grow-on-demand model cache table.
 * Private C99 helper included AFTER jce_sr_internal.h in jce_scene_renderer.c.
 * Unit tests compile this SAME header against minimal fake renderer/entry structs.
 * All operations are on the single render owner thread; never call concurrently.
 */
#ifndef JCE_SR_MODEL_TABLE_H
#define JCE_SR_MODEL_TABLE_H
#include <stdint.h>
#include <stdbool.h>

/* Try to keep load factor below 75%, but preserve existing free slots on OOM.
 * A table that cannot grow admits until actually full, then refuses and emits
 * a diagnostic through its caller.  Rehashing moves only POINTERS to entries;
 * an SrModelCache * returned to draw code stays valid until explicit eviction
 * or cache invalidation.  Both happen at known frame boundaries. */
static bool sr_model_table_prepare_new_key(JceSceneRenderer *sr)
{
    if (!sr) return false;
    const uint32_t old_cap = sr->model_cache_capacity;
    if (old_cap && (sr->model_cache_used + 1u) * 4u <= old_cap * 3u)
        return true;
    if (old_cap >= SR_MODEL_CACHE_MAX)
        return sr->model_cache_used < old_cap;

    uint32_t new_cap = old_cap ? old_cap * 2u : SR_MODEL_CACHE_INITIAL;
    if (new_cap > SR_MODEL_CACHE_MAX) new_cap = SR_MODEL_CACHE_MAX;
    SrModelCache **tab = (SrModelCache **)JCE_CALLOC(new_cap, sizeof(*tab));
    if (!tab) return old_cap && sr->model_cache_used < old_cap;
    const uint32_t mask = new_cap - 1u;
    for (uint32_t i = 0; i < old_cap; ++i) {
        SrModelCache *entry = sr->model_cache[i];
        if (!entry) continue;
        uint32_t slot = entry->path_hash & mask;
        while (tab[slot]) slot = (slot + 1u) & mask;
        tab[slot] = entry;
    }
    JCE_FREE(sr->model_cache);
    sr->model_cache = tab;
    sr->model_cache_capacity = new_cap;
    return true;
}

/* Unlink a slot using Knuth backward shift. Returns the SAME entry pointer to
 * the caller, which owns its model/task/batch resource teardown and JCE_FREE.
 * Table rehash and deletion never move an entry's allocated bytes. */
static SrModelCache *sr_model_table_unlink(JceSceneRenderer *sr, uint32_t slot)
{
    if (!sr || !sr->model_cache || slot >= sr->model_cache_capacity)
        return NULL;
    SrModelCache *removed = sr->model_cache[slot];
    if (!removed) return NULL;
    if (sr->model_lookup_hint == removed) sr->model_lookup_hint = NULL;
    const uint32_t cap = sr->model_cache_capacity;
    const uint32_t mask = cap - 1u;
    uint32_t hole = slot;
    uint32_t scan = (hole + 1u) & mask;
    sr->model_cache[hole] = NULL;
    for (uint32_t steps = 1u; steps < cap; ++steps) {
        SrModelCache *next = sr->model_cache[scan];
        if (!next) break;
        uint32_t home = next->path_hash & mask;
        const bool home_is_between = hole <= scan
            ? (home > hole && home <= scan)
            : (home > hole || home <= scan);
        if (!home_is_between) {
            sr->model_cache[hole] = next;
            sr->model_cache[scan] = NULL;
            hole = scan;
        }
        scan = (scan + 1u) & mask;
    }
    if (sr->model_cache_used) --sr->model_cache_used;
    return removed;
}

#endif /* JCE_SR_MODEL_TABLE_H */
