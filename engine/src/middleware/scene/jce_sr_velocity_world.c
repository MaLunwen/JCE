/* Private world-transform history for velocity consumers. No device ownership. */
#include "jce_sr_internal.h"
#include <string.h>

static uint32_t sr_prev_xform_hash(JceEntity entity)
{
    return (uint32_t)(entity ^ (entity >> 32)) * 2654435761u;
}

void sr_prev_xform_reset(JceSceneRenderer *sr)
{
    if (sr->prev_xform)
        memset(sr->prev_xform, 0, (size_t)sr->prev_xform_cap * sizeof *sr->prev_xform);
    sr->prev_xform_count = 0;
    sr->prev_xform_scene = NULL;
    sr->prev_xform_gen_valid = false;
}

/* Grow the open-addressing table to hold >=need live entries at <0.7 load,
 * rehashing existing entries.  Keeps per-entity find/store O(1) amortized: the
 * old FIXED 512-slot table degraded to O(N*512) once the scene exceeded 512
 * entities (every probe scanned all 512 slots, never finding an open one) —
 * measured ~7ms/frame of the velocity pre-pass at 10k entities. */
static void sr_prev_xform_grow(JceSceneRenderer *sr, uint32_t need)
{
    uint32_t newcap = sr->prev_xform_cap ? sr->prev_xform_cap : SR_PREV_XFORM_MAX;
    while ((uint64_t)need * 10u >= (uint64_t)newcap * 7u) newcap *= 2u;
    if (sr->prev_xform && newcap <= sr->prev_xform_cap) return;
    SrPrevXform *nw = (SrPrevXform *)JCE_CALLOC(newcap, sizeof(SrPrevXform));
    if (!nw) return;  /* OOM: keep the smaller table — slower but correct */
    SrPrevXform *old = sr->prev_xform;
    uint32_t oldcap = sr->prev_xform_cap;
    for (uint32_t i = 0; i < oldcap; i++) {
        if (!old[i].used) continue;
        uint32_t h = sr_prev_xform_hash(old[i].entity) % newcap;
        for (uint32_t j = 0; j < newcap; j++) {
            uint32_t s = (h + j) % newcap;
            if (!nw[s].used) { nw[s] = old[i]; break; }
        }
    }
    if (old) JCE_FREE(old);
    sr->prev_xform = nw;
    sr->prev_xform_cap = newcap;
}

jce_mat4 *sr_prev_xform_find(JceSceneRenderer *sr, JceEntity entity)
{
    if (!sr->prev_xform) return NULL;
    uint32_t cap = sr->prev_xform_cap;
    uint32_t h = sr_prev_xform_hash(entity) % cap;
    for (uint32_t i = 0; i < cap; i++) {
        uint32_t s = (h + i) % cap;
        SrPrevXform *e = &sr->prev_xform[s];
        if (e->used && e->entity == entity) {
            if (e->frame_gen != sr->vel_frame_gen) return &e->current_world;
            return e->prev_valid ? &e->prev_world : NULL;
        }
        if (!e->used) return NULL; /* open slot ends the probe chain */
    }
    return NULL;
}

/* Latch previous only once in this host generation, then retain the latest
 * current value for the following generation. A new entity remains untracked
 * for every viewport in its first round, rather than adopting viewport 1. */
void sr_prev_xform_store(JceSceneRenderer *sr, JceEntity entity,
                                const jce_mat4 *world)
{
    if (!sr->prev_xform ||
        (uint64_t)(sr->prev_xform_count + 1u) * 10u >= (uint64_t)sr->prev_xform_cap * 7u)
        sr_prev_xform_grow(sr, sr->prev_xform_count + 1u);
    if (!sr->prev_xform) return;  /* OOM */
    uint32_t cap = sr->prev_xform_cap;
    uint32_t h = sr_prev_xform_hash(entity) % cap;
    for (uint32_t i = 0; i < cap; i++) {
        uint32_t s = (h + i) % cap;
        SrPrevXform *e = &sr->prev_xform[s];
        if (e->used && e->entity == entity) {
            if (e->frame_gen != sr->vel_frame_gen) {
                e->prev_world = e->current_world;
                e->prev_valid = true;
                e->frame_gen = sr->vel_frame_gen;
            }
            e->current_world = *world;
            e->touched = true;
            return;
        }
        if (!e->used) {
            e->used = true; e->entity = entity; e->current_world = *world;
            e->frame_gen = sr->vel_frame_gen;
            e->prev_valid = false;
            e->touched = true;
            sr->prev_xform_count++;
            return;
        }
    }
    /* A failed grow can leave a full table. This untracked entity uses current
     * as previous at the caller; never replace another entity's history. */
}

/* Backshift deletion preserves probe chains without tombstones or another
 * allocation. Merely clearing used would hide collided surviving entities. */
static void sr_prev_xform_remove(JceSceneRenderer *sr, uint32_t hole)
{
    const uint32_t cap = sr->prev_xform_cap;
    uint32_t next = (hole + 1u) % cap;
    memset(&sr->prev_xform[hole], 0, sizeof sr->prev_xform[hole]);
    while (sr->prev_xform[next].used) {
        const uint32_t home = sr_prev_xform_hash(sr->prev_xform[next].entity) % cap;
        if ((hole + cap - home) % cap < (next + cap - home) % cap) {
            sr->prev_xform[hole] = sr->prev_xform[next];
            memset(&sr->prev_xform[next], 0, sizeof sr->prev_xform[next]);
            hole = next;
        }
        next = (next + 1u) % cap;
    }
    memset(&sr->prev_xform[hole], 0, sizeof sr->prev_xform[hole]);
    --sr->prev_xform_count;
}

/* Prune the preceding round's union before any viewport starts the next one.
 * Do not reset touched at the end of each viewport. */
static void sr_prev_xform_prune(JceSceneRenderer *sr)
{
    if (!sr->prev_xform) return;
    uint32_t i = 0;
    while (i < sr->prev_xform_cap) {
        if (sr->prev_xform[i].used && !sr->prev_xform[i].touched)
            sr_prev_xform_remove(sr, i); /* A shifted entry needs this check too. */
        else
            ++i;
    }
    for (i = 0; i < sr->prev_xform_cap; ++i)
        sr->prev_xform[i].touched = false;
}

void sr_prev_xform_begin(JceSceneRenderer *sr, const JceScene *scene)
{
    /* Single-view callers need no new clock/API. Their actual velocity pass
     * advances the same existing generation; driven hosts own that increment. */
    if (!sr->velocity_frame_driven) ++sr->vel_frame_gen;
    if (sr->prev_xform_scene != scene) sr_prev_xform_reset(sr);
    if (sr->prev_xform_gen_valid && sr->prev_xform_gen == sr->vel_frame_gen) return;
    sr_prev_xform_prune(sr);
    sr->prev_xform_scene = scene;
    sr->prev_xform_gen = sr->vel_frame_gen;
    sr->prev_xform_gen_valid = true;
}
