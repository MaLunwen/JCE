/* Renderer-owned world and focus bounds tables. */
#include "jce_sr_internal.h"
#include <jce/os/core/jce_hashmap.h>

/* ── Cross-frame persistent static world cache (large-world-opt M1 #2) ──────
 *
 * The ecull build loop composes each entity's world matrix + world AABB every
 * frame.  In a streaming city the overwhelming majority of entities are STATIC
 * (buildings, roads, trees, lamps) and never move, yet the per-frame
 * jce_scene_begin_render_world_cache drop forced a full recompose every frame.
 * This table caches the composed (world, wmin, wmax, has_aabb, casts_shadow)
 * across frames, keyed by entity id, and reuses it while the scene's structural
 * epoch is unchanged AND the entity is statically determinable as non-moving.
 *
 * Conservative by construction: an entity is cached ONLY when neither it nor any
 * ancestor carries a component the runtime mutates in place (rigidbody, 2D body,
 * character controller, skeletal animator, vehicle, wheel collider, soft body) —
 * those write the scene Transform directly in rt_sync_transforms WITHOUT bumping
 * the structural epoch, so they must always recompute.  Script-driven motion
 * goes through jce_scene_set_transform, which DOES bump the epoch, so it is
 * caught.  When in any doubt (ancestor walk hits the depth guard) the entity is
 * treated as dynamic and recomputed. */

/* Look up a live, epoch-current cache entry for `entity`.  Returns NULL on miss
   or stale.  Never grows. */
/* Non-static: lever ③'s draw-cmd cache (jce_sr_draw.c) re-finds the wcache entry
 * for an eligible entity to attach/read its cached command. */
SrWorldCacheEntry *sr_wcache_find(JceSceneRenderer *sr, JceEntity entity,
                                  uint64_t epoch, uint64_t xform_gen)
{
    if (!sr->wcache || sr->wcache_cap == 0 || entity == 0) return NULL;
    uint32_t mask = sr->wcache_cap - 1;
    uint32_t h = sr_bounds_cache_hash(entity) & mask;
    for (uint32_t i = 0; i < sr->wcache_cap; i++) {
        uint32_t s = (h + i) & mask;
        SrWorldCacheEntry *en = &sr->wcache[s];
        if (!en->used) return NULL;                 /* open slot ends the probe */
        if (en->entity == entity)
            return (en->structural_epoch == epoch && en->xform_gen == xform_gen)
                       ? en : NULL;
    }
    return NULL;
}

/* Grow + rehash the table to `new_cap` (power of two).  Drops untouched-style
   bookkeeping; pure capacity grow.  No-op on OOM (caller then skips caching). */
static bool sr_wcache_grow(JceSceneRenderer *sr, uint32_t need)
{
    uint32_t new_cap = sr->wcache_cap ? sr->wcache_cap : 1024;
    while (new_cap < need * 2u) new_cap *= 2u;   /* keep load factor < 0.5 */
    SrWorldCacheEntry *ns =
        (SrWorldCacheEntry *)JCE_CALLOC(new_cap, sizeof(SrWorldCacheEntry));
    if (!ns) return false;
    if (sr->wcache) {
        uint32_t nmask = new_cap - 1;
        for (uint32_t i = 0; i < sr->wcache_cap; i++) {
            SrWorldCacheEntry *o = &sr->wcache[i];
            if (!o->used) continue;
            uint32_t h = sr_bounds_cache_hash(o->entity) & nmask;
            while (ns[h].used) h = (h + 1) & nmask;
            ns[h] = *o;
        }
        JCE_FREE(sr->wcache);
    }
    sr->wcache = ns;
    sr->wcache_cap = new_cap;
    return true;
}

/* Insert/overwrite the cached value for `entity`. */
void sr_wcache_store(JceSceneRenderer *sr, JceEntity entity,
                            const SrEntityCull *src,
                            const jce_mat4 *src_world, uint64_t epoch,
                            uint64_t xform_gen)
{
    if (entity == 0) return;
    if (sr->wcache_cap == 0 || (sr->wcache_count + 1) * 2u >= sr->wcache_cap) {
        if (!sr_wcache_grow(sr, sr->wcache_count + 1)) return;
    }
    uint32_t mask = sr->wcache_cap - 1;
    uint32_t h = sr_bounds_cache_hash(entity) & mask;
    while (sr->wcache[h].used && sr->wcache[h].entity != entity)
        h = (h + 1) & mask;
    SrWorldCacheEntry *en = &sr->wcache[h];
    if (!en->used) {
        en->used = true; en->entity = entity; sr->wcache_count++;
        /* Fresh/reused slot: drop any lever-③ draw-cmd cached by a PRIOR entity
         * in this slot. Without this, a pruned-then-reused slot could serve the
         * old entity's cmd if both happen to share material_gen (e.g. both 0). */
        en->dc_valid = false;
    }
    en->structural_epoch = epoch;
    en->xform_gen    = xform_gen;
    en->world        = *src_world;
    en->world_valid  = src->world_valid;
    en->wmin         = src->wmin;
    en->wmax         = src->wmax;
    en->has_aabb     = src->has_aabb;
    en->casts_shadow = src->casts_shadow;
    en->render_kind  = src->render_kind;   /* Fix#1: cache the classification too */
    en->kc_enabled   = src->kc_enabled;
    en->kc_has_light = src->kc_has_light;
    en->parallel_eligible = src->parallel_eligible;
    en->touched      = true;
}

/* Drop stale entries so the table tracks the active streamed set and can't grow
   without bound.  Called from the build loop; AMORTIZED — runs only when the
   structural epoch changed (a real edit invalidated everything) OR the table is
   getting full (>=75% of capacity).  Two reasons it is NOT an every-render
   sweep: (1) sr_wcache_find is epoch-gated, so a stale entry already returns a
   safe miss without a sweep; (2) the editor renders >1 viewport per bgfx frame
   on a SHARED renderer with DIFFERENT focus-bounded entity subsets, so a
   per-render "evict untouched" sweep would ping-pong entries that one viewport
   sees and the other doesn't.  Amortized pruning keeps both viewports' static
   entries resident.  Drops: epoch-stale entries always; touched==false (streamed
   out / not seen this prune cycle) only on a full-table sweep. */
void sr_wcache_maybe_prune(JceSceneRenderer *sr, uint64_t epoch, bool full_sweep)
{
    if (!sr->wcache || sr->wcache_cap == 0) return;
    const uint32_t cap = sr->wcache_cap;
    const uint32_t mask = cap - 1;
    uint32_t empty = 0;
    while (empty < cap && sr->wcache[empty].used) ++empty;
    if (empty == cap) return;

    /* A scan from an empty slot cannot shift an already visited survivor. */
    for (uint32_t i = (empty + 1) & mask; i != empty;) {
        SrWorldCacheEntry *entry = &sr->wcache[i];
        if (entry->used && (entry->structural_epoch != epoch || (full_sweep && !entry->touched))) {
            uint32_t hole = i;
            for (uint32_t probe = (i + 1) & mask; sr->wcache[probe].used;
                 probe = (probe + 1) & mask) {
                const uint32_t home = sr_bounds_cache_hash(sr->wcache[probe].entity) & mask;
                if (jce_hashmap_shift_back(home, hole, probe, cap)) {
                    sr->wcache[hole] = sr->wcache[probe];
                    hole = probe;
                }
            }
            sr->wcache[hole] = (SrWorldCacheEntry){0};
            --sr->wcache_count;
            continue;
        }
        if (full_sweep) entry->touched = false;
        i = (i + 1) & mask;
    }
}

/* ── Persistent focus-cull world-AABB cache ───────────────────────────────
 * Open-addressed table (mirrors wcache) keyed by entity id, holding each
 * collected entity's world AABB so the NEXT frame's focus-bounded COLLECT can
 * keep an entity whose AABB OVERLAPS the focus disc (vs the legacy transform-
 * origin distance test that dropped a world-spanning terrain authored at the
 * origin once the focus moved off-centre).  Unlike wcache this is NOT gated on
 * the static predicate — it must cover a rigidbody-bearing static terrain too. */
struct SrFocusAabbEntry *sr_focus_aabb_find(JceSceneRenderer *sr,
                                                   JceEntity entity)
{
    if (!sr->focus_aabb || sr->focus_aabb_cap == 0 || entity == 0) return NULL;
    uint32_t mask = sr->focus_aabb_cap - 1;
    uint32_t h = sr_bounds_cache_hash(entity) & mask;
    for (uint32_t i = 0; i < sr->focus_aabb_cap; i++) {
        uint32_t s = (h + i) & mask;
        struct SrFocusAabbEntry *en = &sr->focus_aabb[s];
        if (!en->used) return NULL;                 /* open slot ends the probe */
        if (en->entity == entity) return en;
    }
    return NULL;
}

static bool sr_focus_aabb_grow(JceSceneRenderer *sr, uint32_t need)
{
    uint32_t new_cap = sr->focus_aabb_cap ? sr->focus_aabb_cap : 1024;
    while (new_cap < need * 2u) new_cap *= 2u;   /* load factor < 0.5 */
    struct SrFocusAabbEntry *ns = (struct SrFocusAabbEntry *)
        JCE_CALLOC(new_cap, sizeof(struct SrFocusAabbEntry));
    if (!ns) return false;
    if (sr->focus_aabb) {
        uint32_t nmask = new_cap - 1;
        for (uint32_t i = 0; i < sr->focus_aabb_cap; i++) {
            struct SrFocusAabbEntry *o = &sr->focus_aabb[i];
            if (!o->used) continue;
            uint32_t h = sr_bounds_cache_hash(o->entity) & nmask;
            while (ns[h].used) h = (h + 1) & nmask;
            ns[h] = *o;
        }
        JCE_FREE(sr->focus_aabb);
    }
    sr->focus_aabb = ns;
    sr->focus_aabb_cap = new_cap;
    return true;
}

/* Store/refresh an entity's world AABB for next frame's collect. */
void sr_focus_aabb_store(JceSceneRenderer *sr, JceEntity entity,
                               jce_vec3 wmin, jce_vec3 wmax)
{
    if (entity == 0) return;
    if (sr->focus_aabb_cap == 0 ||
        (sr->focus_aabb_count + 1) * 2u >= sr->focus_aabb_cap) {
        if (!sr_focus_aabb_grow(sr, sr->focus_aabb_count + 1)) return;
    }
    uint32_t mask = sr->focus_aabb_cap - 1;
    uint32_t h = sr_bounds_cache_hash(entity) & mask;
    while (sr->focus_aabb[h].used && sr->focus_aabb[h].entity != entity)
        h = (h + 1) & mask;
    struct SrFocusAabbEntry *en = &sr->focus_aabb[h];
    if (!en->used) { en->used = true; en->entity = entity; sr->focus_aabb_count++; }
    en->wmin = wmin; en->wmax = wmax;
    en->touched = true;
}

/* Overwrite-only variant safe for the parallel ecull hit path: never inserts
 * or grows (each entity hits its own slot => disjoint writes; the probe chain
 * is frozen while no insert runs).  Returns false when the entity has no
 * entry — possible for a wcache-HIT entity whose focus entry was pruned while
 * it streamed out of the collect (its wcache entry survives the independent,
 * epoch-gated wcache prune) — so the caller can queue a serial insert. */
bool sr_focus_aabb_touch_update(JceSceneRenderer *sr, JceEntity entity,
                                       jce_vec3 wmin, jce_vec3 wmax)
{
    if (entity == 0) return true;   /* nothing to store — don't queue */
    if (!sr->focus_aabb || sr->focus_aabb_cap == 0) return false;
    uint32_t mask = sr->focus_aabb_cap - 1;
    uint32_t h = sr_bounds_cache_hash(entity) & mask;
    while (sr->focus_aabb[h].used) {
        if (sr->focus_aabb[h].entity == entity) {
            struct SrFocusAabbEntry *en = &sr->focus_aabb[h];
            en->wmin = wmin; en->wmax = wmax;
            en->touched = true;
            return true;
        }
        h = (h + 1) & mask;
    }
    return false;
}

static void sr_focus_aabb_erase(JceSceneRenderer *sr, uint32_t hole)
{
    const uint32_t cap = sr->focus_aabb_cap;
    const uint32_t mask = cap - 1;
    for (uint32_t probe = (hole + 1) & mask; sr->focus_aabb[probe].used;
         probe = (probe + 1) & mask) {
        const uint32_t home = sr_bounds_cache_hash(sr->focus_aabb[probe].entity) & mask;
        if (jce_hashmap_shift_back(home, hole, probe, cap)) {
            sr->focus_aabb[hole] = sr->focus_aabb[probe];
            hole = probe;
        }
    }
    sr->focus_aabb[hole] = (struct SrFocusAabbEntry){0};
    --sr->focus_aabb_count;
}

bool sr_focus_aabb_remove(JceSceneRenderer *sr, JceEntity entity)
{
    struct SrFocusAabbEntry *entry = sr_focus_aabb_find(sr, entity);
    if (!entry) return false;
    sr_focus_aabb_erase(sr, (uint32_t)(entry - sr->focus_aabb));
    return true;
}

/* Preserve the existing amortized focus sweep and its touched window. */
void sr_focus_aabb_maybe_prune(JceSceneRenderer *sr)
{
    if (!sr->focus_aabb || sr->focus_aabb_cap == 0) return;
    if (sr->focus_aabb_count * 4u < sr->focus_aabb_cap * 3u) {
        for (uint32_t i = 0; i < sr->focus_aabb_cap; ++i)
            sr->focus_aabb[i].touched = false;
        return;
    }
    const uint32_t cap = sr->focus_aabb_cap;
    const uint32_t mask = cap - 1;
    uint32_t empty = 0;
    while (empty < cap && sr->focus_aabb[empty].used) ++empty;
    if (empty == cap) return;

    /* A scan from an empty slot cannot shift an already visited survivor. */
    for (uint32_t i = (empty + 1) & mask; i != empty;) {
        struct SrFocusAabbEntry *entry = &sr->focus_aabb[i];
        if (entry->used && (!entry->touched)) {
            sr_focus_aabb_erase(sr, i);
            continue;
        }
        entry->touched = false;
        i = (i + 1) & mask;
    }
}

void sr_bounds_cache_reset(JceSceneRenderer *sr)
{
    if (sr->wcache)
        memset(sr->wcache, 0, (size_t)sr->wcache_cap * sizeof sr->wcache[0]);
    if (sr->focus_aabb)
        memset(sr->focus_aabb, 0, (size_t)sr->focus_aabb_cap * sizeof sr->focus_aabb[0]);
    sr->wcache_count = 0;
    sr->focus_aabb_count = 0;
}
