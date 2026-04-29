/*
 * jce_world_streamer.c  High-level open-world scene streaming.
 *
 * Binds JceStreamingSystem (I/O layer) to scene entity instantiation
 * so that chunk files are automatically parsed and entities are
 * spawned / destroyed as the camera moves through the world.
 *
 * Design notes
 * ─────────────
 * • Entity ownership is tracked per-chunk in a fixed-size pool
 *   (MAX_WORLD_CHUNKS chunks × MAX_ENTITIES_PER_CHUNK slots).
 * • The on_loaded callback is invoked from a worker thread; raw bytes
 *   are stored on the chunk record and the actual scene-serial work
 *   is deferred to jce_world_streamer_update() (main thread).
 * • This ensures all bgfx GPU handle creation happens on the render
 *   thread, not in a background worker.
 */

#include <jce/resource/jce_scene_serial.h>
#include <jce/resource/jce_world_streamer.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/middleware/scene/jce_scene.h>

#include "os/core/jce_memory.h"

#include <SDL3/SDL_atomic.h>
#include <string.h>
#include <stdlib.h>

#define LOG_TAG             "world_streamer"
#define MAX_WORLD_CHUNKS    256
#define MAX_ENTITY_SLOTS    2048   /* per-chunk entity roster capacity */

/* ── Per-chunk entity roster ─────────────────────────────────────── */

typedef struct {
    uint32_t   chunk_id;
    bool       active;       /* true = registered */

    /* Pending deferred load (set from worker thread, consumed on main). */
    SDL_AtomicInt pending_apply; /* 1 = data ready to apply, 0 = idle   */
    void         *pending_data;
    size_t        pending_size;
    bool          pending_ok;

    /* Entities spawned by this chunk (dynamic, owned by us). */
    JceEntity *entities;
    uint32_t   entity_count;
    uint32_t   entity_cap;
} ChunkRoster;

/* ── World streamer struct ───────────────────────────────────────── */

struct JceWorldStreamer {
    JceStreamingSystem *ss;
    JceScene           *scene;

    ChunkRoster         rosters[MAX_WORLD_CHUNKS];
    uint32_t            roster_count;

    /* Aggregate stats (updated each apply). */
    uint32_t            total_entities;
};

/* ── Roster helpers ──────────────────────────────────────────────── */

static ChunkRoster *find_roster(JceWorldStreamer *ws, uint32_t id)
{
    for (uint32_t i = 0; i < ws->roster_count; ++i)
        if (ws->rosters[i].active && ws->rosters[i].chunk_id == id)
            return &ws->rosters[i];
    return NULL;
}

static ChunkRoster *alloc_roster(JceWorldStreamer *ws, uint32_t id)
{
    /* Recycle inactive slot first. */
    for (uint32_t i = 0; i < ws->roster_count; ++i) {
        if (!ws->rosters[i].active) {
            memset(&ws->rosters[i], 0, sizeof(ChunkRoster));
            ws->rosters[i].chunk_id = id;
            ws->rosters[i].active   = true;
            return &ws->rosters[i];
        }
    }
    if (ws->roster_count >= MAX_WORLD_CHUNKS) {
        LOG_ERROR(LOG_TAG, "max chunk roster limit (%d) reached", MAX_WORLD_CHUNKS);
        return NULL;
    }
    ChunkRoster *r = &ws->rosters[ws->roster_count++];
    memset(r, 0, sizeof(*r));
    r->chunk_id = id;
    r->active   = true;
    return r;
}

static void roster_free_entities(JceWorldStreamer *ws, ChunkRoster *r)
{
    for (uint32_t i = 0; i < r->entity_count; ++i)
        jce_scene_destroy_entity(ws->scene, r->entities[i]);
    ws->total_entities -= r->entity_count;
    JCE_FREE(r->entities);
    r->entities     = NULL;
    r->entity_count = 0;
    r->entity_cap   = 0;
}

/* ── Streaming callbacks (may be called from worker thread) ─────── */

static void on_chunk_loaded(uint32_t chunk_id, void *data, size_t size,
                             void *user_data)
{
    JceWorldStreamer *ws = (JceWorldStreamer *)user_data;
    ChunkRoster      *r  = find_roster(ws, chunk_id);
    if (!r) return;

    if (!data || size == 0) {
        LOG_ERROR(LOG_TAG, "chunk %u load failed (no data)", chunk_id);
        return;
    }

    /* Copy data to a heap buffer owned by the roster, then signal main thread. */
    void *buf = JCE_MALLOC(size + 1);
    if (!buf) {
        LOG_ERROR(LOG_TAG, "chunk %u: OOM staging buffer", chunk_id);
        return;
    }
    memcpy(buf, data, size);
    ((char *)buf)[size] = '\0';

    r->pending_data = buf;
    r->pending_size = size;
    r->pending_ok   = true;
    SDL_SetAtomicInt(&r->pending_apply, 1);
}

static void on_chunk_unloaded(uint32_t chunk_id, void *user_data)
{
    JceWorldStreamer *ws = (JceWorldStreamer *)user_data;
    ChunkRoster      *r  = find_roster(ws, chunk_id);
    if (!r) return;

    roster_free_entities(ws, r);
    LOG_INFO(LOG_TAG, "chunk %u unloaded (entities removed)", chunk_id);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

JceWorldStreamer *jce_world_streamer_create(
    const JceWorldStreamConfig *config,
    JceScene                   *scene,
    JceFileSystem              *fs,
    JceThreadPool              *thread_pool)
{
    if (!config || !scene || !fs) return NULL;

    JceWorldStreamer *ws = (JceWorldStreamer *)JCE_CALLOC(1, sizeof(*ws));
    if (!ws) return NULL;

    ws->scene = scene;

    JceStreamingConfig ssc;
    memset(&ssc, 0, sizeof(ssc));
    ssc.mode            = config->mode;
    ssc.load_radius     = config->load_radius;
    ssc.unload_radius   = config->unload_radius;
    ssc.max_pending     = config->max_pending  ? config->max_pending  : 4;
    ssc.budget_mb       = config->budget_mb;
    ssc.single_thread   = config->single_thread;
    ssc.frame_budget_ms = config->frame_budget_ms > 0.f
                              ? config->frame_budget_ms
                              : 2.0f;

    ws->ss = jce_streaming_create(&ssc);
    if (!ws->ss) {
        JCE_FREE(ws);
        return NULL;
    }

    jce_streaming_set_filesystem(ws->ss, fs);
    if (thread_pool)
        jce_streaming_set_thread_pool(ws->ss, thread_pool);

    jce_streaming_set_callbacks(ws->ss, on_chunk_loaded, on_chunk_unloaded, ws);

    LOG_INFO(LOG_TAG, "world streamer created (r=%.0f/%.0f, budget=%u MiB)",
             config->load_radius, config->unload_radius, config->budget_mb);
    return ws;
}

void jce_world_streamer_destroy(JceWorldStreamer *ws)
{
    if (!ws) return;

    /* Destroy all streamed entities. */
    for (uint32_t i = 0; i < ws->roster_count; ++i) {
        ChunkRoster *r = &ws->rosters[i];
        if (!r->active) continue;
        roster_free_entities(ws, r);

        /* Free any pending staging buffer. */
        if (SDL_GetAtomicInt(&r->pending_apply)) {
            JCE_FREE(r->pending_data);
            r->pending_data = NULL;
        }
    }

    jce_streaming_destroy(ws->ss);
    JCE_FREE(ws);
}

/* ── Chunk registration ──────────────────────────────────────────── */

void jce_world_streamer_register_chunk(JceWorldStreamer *ws,
                                        uint32_t         chunk_id,
                                        jce_vec3         center,
                                        float            radius,
                                        const char      *asset_path)
{
    if (!ws || !asset_path) return;

    ChunkRoster *r = alloc_roster(ws, chunk_id);
    if (!r) return;

    JceStreamChunk sc;
    sc.chunk_id   = chunk_id;
    sc.center     = center;
    sc.radius     = radius;
    sc.asset_path = asset_path;
    jce_streaming_register_chunk(ws->ss, &sc);
}

void jce_world_streamer_unregister_chunk(JceWorldStreamer *ws, uint32_t chunk_id)
{
    if (!ws) return;

    ChunkRoster *r = find_roster(ws, chunk_id);
    if (r) {
        roster_free_entities(ws, r);

        if (SDL_GetAtomicInt(&r->pending_apply)) {
            JCE_FREE(r->pending_data);
            r->pending_data = NULL;
        }
        r->active = false;
    }

    jce_streaming_unregister_chunk(ws->ss, chunk_id);
}

/* ── Per-frame update ────────────────────────────────────────────── */

void jce_world_streamer_update(JceWorldStreamer *ws, jce_vec3 camera_pos)
{
    if (!ws) return;
    JCE_PROFILE_ZONE_N("WorldStreamer::Update");

    /* Drive I/O loads/unloads. */
    jce_streaming_update(ws->ss, camera_pos);

    /* Apply any chunks whose background load completed. */
    for (uint32_t i = 0; i < ws->roster_count; ++i) {
        ChunkRoster *r = &ws->rosters[i];
        if (!r->active) continue;
        if (!SDL_CompareAndSwapAtomicInt(&r->pending_apply, 1, 0)) continue;

        if (!r->pending_ok || !r->pending_data) {
            JCE_FREE(r->pending_data);
            r->pending_data = NULL;
            continue;
        }

        /* Remove stale entities from a previous load (hot-reload). */
        roster_free_entities(ws, r);

        /* Spawn new entities from the chunk JSON. */
        JceEntity *new_ents  = NULL;
        uint32_t   new_count = 0;
        bool ok = jce_scene_serial_load_additive(ws->scene,
                                                  (const char *)r->pending_data,
                                                  r->pending_size,
                                                  &new_ents, &new_count);

        JCE_FREE(r->pending_data);
        r->pending_data = NULL;

        if (!ok) {
            LOG_ERROR(LOG_TAG, "chunk %u: scene deserialise failed", r->chunk_id);
            continue;
        }

        r->entities     = new_ents;
        r->entity_count = new_count;
        r->entity_cap   = new_count;
        ws->total_entities += new_count;

        LOG_INFO(LOG_TAG, "chunk %u applied: %u entities spawned",
                 r->chunk_id, new_count);
    }
    JCE_PROFILE_ZONE_END;
}

/* ── Stats ───────────────────────────────────────────────────────── */

uint32_t jce_world_streamer_loaded_count(const JceWorldStreamer *ws)
{
    return ws ? jce_streaming_loaded_count(ws->ss) : 0;
}

uint32_t jce_world_streamer_pending_count(const JceWorldStreamer *ws)
{
    return ws ? jce_streaming_pending_count(ws->ss) : 0;
}

uint64_t jce_world_streamer_memory_used(const JceWorldStreamer *ws)
{
    return ws ? jce_streaming_memory_used(ws->ss) : 0;
}

uint32_t jce_world_streamer_evicted_count(const JceWorldStreamer *ws)
{
    return ws ? jce_streaming_evicted_count(ws->ss) : 0;
}

uint32_t jce_world_streamer_entity_count(const JceWorldStreamer *ws)
{
    return ws ? ws->total_entities : 0;
}

uint32_t jce_world_streamer_chunk_count(const JceWorldStreamer *ws)
{
    if (!ws) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < ws->roster_count; ++i)
        if (ws->rosters[i].active) ++n;
    return n;
}
