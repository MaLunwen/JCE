/*
 * jce_world_partition.c  Engine-side world partitioning (Direction A1).
 *
 * See jce_world_partition.h.  Pure transform: serialise the scene once, spatial
 * -hash streamable entities into grid cells, MOVE their JSON nodes into per-cell
 * fragment trees, leave residents in the master, and inject the chunk roster.
 *
 * Engine-internal TU: may use cJSON directly (only DetachItemViaPointer, which
 * jce_json.h does not expose) — same allowance the scene (de)serialiser uses.
 */

#include <jce/resource/jce_world_partition.h>

#include <jce/middleware/scene/jce_scene.h>                  /* JCE_SCENE_MAX_STREAM_CHUNKS */
#include <jce/middleware/scene/jce_scene_components_json.h>  /* jce_scene_save_json          */
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <cjson/cJSON.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "world_partition"

/* ── id -> entity-index hash (open addressing, linear probe) ──────────── */

typedef struct { uint64_t id; uint32_t idx; bool used; } EntSlot;

static uint32_t mix64(uint64_t h)
{
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (uint32_t)h;
}

/* ── Per-cell accumulator ────────────────────────────────────────────── */

typedef struct {
    int      gx, gz;
    double   sum[3];     /* centroid accumulator                            */
    uint32_t count;
    float    radius;
    JceJson *fents;      /* the fragment's "entities" array (push target)   */
} Cell;

void jce_world_partition_free(JcePartitionResult *res)
{
    if (!res) return;
    if (res->master_json) jce_json_free(res->master_json);
    if (res->chunks) {
        for (uint32_t i = 0; i < res->chunk_count; ++i)
            if (res->chunks[i].json) jce_json_free(res->chunks[i].json);
        JCE_FREE(res->chunks);
    }
    memset(res, 0, sizeof(*res));
}

bool jce_world_partition_build(const struct JceScene    *scene,
                               const JcePartitionEntity *ents,
                               uint32_t                  ent_count,
                               const JcePartitionConfig *cfg,
                               JcePartitionResult       *out)
{
    if (!scene || !cfg || !out) return false;
    if (!ents && ent_count) return false;
    if (!(cfg->cell_size > 0.0f) || !cfg->fragment_path_fmt) {
        LOG_ERROR(LOG_TAG, "invalid config (cell_size>0 + fragment_path_fmt required)");
        return false;
    }
    memset(out, 0, sizeof(*out));

    /* 1. Serialise the whole scene once. */
    JceJson *full = jce_scene_save_json(scene);
    if (!full) { LOG_ERROR(LOG_TAG, "scene serialise failed"); return false; }
    JceJson *scene_obj = jce_json_get(full, "scene");
    JceJson *entities  = scene_obj ? jce_json_get(scene_obj, "entities") : NULL;
    if (!jce_json_is_object(scene_obj) || !jce_json_is_array(entities)) {
        LOG_ERROR(LOG_TAG, "serialised scene missing scene.entities");
        jce_json_free(full);
        return false;
    }

    bool      ok          = false;
    EntSlot  *hash        = NULL;
    int      *cell_of_ent = NULL;
    Cell     *cells       = NULL;
    JceJson **node_ptrs   = NULL;

    /* 2. id -> ent index hash. */
    uint32_t hcap = 16;
    while (hcap < ent_count * 2u) hcap <<= 1;
    hash = (EntSlot *)JCE_CALLOC(hcap, sizeof(EntSlot));
    cell_of_ent = (int *)JCE_MALLOC((size_t)(ent_count ? ent_count : 1) * sizeof(int));
    cells = (Cell *)JCE_CALLOC(JCE_SCENE_MAX_STREAM_CHUNKS, sizeof(Cell));
    if (!hash || !cell_of_ent || !cells) goto done;

    for (uint32_t i = 0; i < ent_count; ++i) {
        uint32_t h = mix64(ents[i].entity_id) & (hcap - 1);
        while (hash[h].used) h = (h + 1) & (hcap - 1);
        hash[h].used = true;
        hash[h].id   = ents[i].entity_id;
        hash[h].idx  = i;
    }

    /* 3. Pass 1 — assign streamable entities to cells; accumulate centroids. */
    uint32_t cell_count = 0;
    for (uint32_t i = 0; i < ent_count; ++i) {
        cell_of_ent[i] = -1;
        if (!ents[i].streamable) continue;
        int gx = (int)floorf(ents[i].center[0] / cfg->cell_size);
        int gz = (int)floorf(ents[i].center[2] / cfg->cell_size);
        int ci = -1;
        for (uint32_t c = 0; c < cell_count; ++c)
            if (cells[c].gx == gx && cells[c].gz == gz) { ci = (int)c; break; }
        if (ci < 0) {
            if (cell_count >= (uint32_t)JCE_SCENE_MAX_STREAM_CHUNKS) {
                LOG_ERROR(LOG_TAG,
                    "partition would create >%d chunks — increase cell_size",
                    JCE_SCENE_MAX_STREAM_CHUNKS);
                goto done;
            }
            ci = (int)cell_count++;
            cells[ci].gx = gx; cells[ci].gz = gz;
            cells[ci].sum[0] = cells[ci].sum[1] = cells[ci].sum[2] = 0.0;
            cells[ci].count = 0; cells[ci].radius = 0.0f;
        }
        cell_of_ent[i] = ci;
        cells[ci].sum[0] += ents[i].center[0];
        cells[ci].sum[1] += ents[i].center[1];
        cells[ci].sum[2] += ents[i].center[2];
        cells[ci].count++;
    }

    /* 4. Centroids, then a second pass for the bounding-sphere radius. */
    for (uint32_t c = 0; c < cell_count; ++c) {
        double inv = cells[c].count ? 1.0 / (double)cells[c].count : 0.0;
        cells[c].sum[0] *= inv; cells[c].sum[1] *= inv; cells[c].sum[2] *= inv;
    }
    for (uint32_t i = 0; i < ent_count; ++i) {
        int ci = cell_of_ent[i];
        if (ci < 0) continue;
        double dx = ents[i].center[0] - cells[ci].sum[0];
        double dy = ents[i].center[1] - cells[ci].sum[1];
        double dz = ents[i].center[2] - cells[ci].sum[2];
        float r = (float)sqrt(dx*dx + dy*dy + dz*dz) + ents[i].radius;
        if (r > cells[ci].radius) cells[ci].radius = r;
    }

    /* 5. Allocate result chunks + build empty fragment trees. */
    out->chunks = (JcePartitionChunk *)JCE_CALLOC(cell_count ? cell_count : 1,
                                                  sizeof(JcePartitionChunk));
    if (!out->chunks) goto done;
    for (uint32_t c = 0; c < cell_count; ++c) {
        JcePartitionChunk *ch = &out->chunks[c];
        ch->gx = cells[c].gx; ch->gz = cells[c].gz;
        ch->id = cfg->id_base + c;
        ch->center[0] = (float)cells[c].sum[0];
        ch->center[1] = (float)cells[c].sum[1];
        ch->center[2] = (float)cells[c].sum[2];
        ch->radius = cells[c].radius > 0.0f ? cells[c].radius : 1.0f;
        snprintf(ch->path, sizeof ch->path, cfg->fragment_path_fmt,
                 cells[c].gx, cells[c].gz);

        JceJson *frag   = jce_json_object();
        JceJson *fscene = jce_json_object();
        JceJson *fents  = jce_json_array();
        if (!frag || !fscene || !fents) {
            if (frag) jce_json_free(frag);
            if (fscene && fscene != frag) jce_json_free(fscene);
            if (fents) jce_json_free(fents);
            goto done;
        }
        jce_json_set_int(fscene, "version", 1);
        jce_json_set_child(fscene, "entities", fents);
        jce_json_set_child(frag, "scene", fscene);
        ch->json     = frag;
        cells[c].fents = fents;   /* push target during the node pass        */
    }
    out->chunk_count = cell_count;

    /* 6. Node pass — MOVE streamable entity nodes into their cell fragments,
     *    leave residents in the master.  Collect node pointers first so the
     *    detaches don't disturb the array we're iterating. */
    int node_n = jce_json_array_size(entities);
    node_ptrs = (JceJson **)JCE_MALLOC((size_t)(node_n > 0 ? node_n : 1) * sizeof(JceJson *));
    if (!node_ptrs) goto done;
    for (int i = 0; i < node_n; ++i) node_ptrs[i] = jce_json_array_at(entities, i);

    for (int i = 0; i < node_n; ++i) {
        JceJson *node = node_ptrs[i];
        if (!node) continue;
        uint64_t id = (uint64_t)jce_json_get_number(node, "id", -1.0);
        /* hash lookup */
        int ci = -1;
        if (ent_count) {
            uint32_t h = mix64(id) & (hcap - 1);
            while (hash[h].used) {
                if (hash[h].id == id) { ci = cell_of_ent[hash[h].idx]; break; }
                h = (h + 1) & (hcap - 1);
            }
        }
        if (ci >= 0) {
            cJSON_DetachItemViaPointer((cJSON *)entities, (cJSON *)node);
            jce_json_array_push(cells[ci].fents, node);
            out->streamed_count++;
        } else {
            out->resident_count++;
        }
    }

    /* 7. Inject the streaming roster into the master scene. */
    jce_json_remove(scene_obj, "streaming");
    {
        JceJson *st = jce_json_object();
        if (!st) goto done;
        jce_json_set_int(st, "version", 1);
        jce_json_set_bool(st, "enabled", true);
        jce_json_set_int(st, "mode", 0);
        jce_json_set_number(st, "loadRadius",  cfg->load_radius);
        jce_json_set_number(st, "unloadRadius", cfg->unload_radius);
        jce_json_set_int(st, "maxPending", 16);
        jce_json_set_int(st, "budgetMb", 512);
        jce_json_set_number(st, "frameBudgetMs", 4.0);
        JceJson *chunks = jce_json_array();
        if (!chunks) { jce_json_free(st); goto done; }
        for (uint32_t c = 0; c < cell_count; ++c) {
            JceJson *co = jce_json_object();
            if (!co) continue;
            jce_json_set_int(co, "id", (int)out->chunks[c].id);
            jce_json_set_float_array(co, "center", out->chunks[c].center, 3);
            jce_json_set_number(co, "radius", out->chunks[c].radius);
            jce_json_set_string(co, "path", out->chunks[c].path);
            jce_json_array_push(chunks, co);
        }
        jce_json_set_child(st, "chunks", chunks);
        jce_json_set_child(scene_obj, "streaming", st);
    }

    out->master_json = full;
    full = NULL;   /* ownership transferred to the result */
    ok = true;

    LOG_INFO(LOG_TAG,
        "partitioned: %u streamed into %u chunks, %u resident (cell_size=%.1f)",
        out->streamed_count, cell_count, out->resident_count,
        (double)cfg->cell_size);

done:
    JCE_FREE(node_ptrs);
    JCE_FREE(cells);
    JCE_FREE(cell_of_ent);
    JCE_FREE(hash);
    if (!ok) {
        jce_world_partition_free(out);   /* frees any partial chunks/json     */
        if (full) jce_json_free(full);
    }
    return ok;
}
