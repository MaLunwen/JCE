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
#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_json.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/renderer/jce_texture.h>

#include "os/core/jce_memory.h"

#include <SDL3/SDL_atomic.h>
#include <stdio.h>          /* sscanf / snprintf — HLOD proxy name parse */
#include <string.h>
#include <stdlib.h>

#define LOG_TAG             "world_streamer"
/* MUST stay equal to JCE_SCENE_MAX_STREAM_CHUNKS in jce_scene.h — the
 * scene's authored chunk table and this roster pool are sized as one. */
#define MAX_WORLD_CHUNKS    256
#define MAX_ENTITY_SLOTS    2048   /* per-chunk entity roster capacity */

/* HLOD cross-fade hand-off (Direction B): how long (ms) a freshly-loaded
 * chunk's resident HLOD proxy STAYS visible after the chunk loads, so the
 * detail entities dither in (screen-door) OVER the proxy before it disappears
 * — eliminating the see-through gap / pop.  Matches the renderer's per-entity
 * fade duration (JCE_SR_FADE_DURATION = 0.4s) so the proxy is hidden exactly
 * as the detail finishes fading in.  On UNLOAD the proxy is shown immediately
 * (no delay), so the skyline is never missing. */
#define JCE_WS_PROXY_HIDE_DELAY_MS  400.0

/* Conservative per-spawned-entity residency estimate (ECS components +
 * transform + a share of per-entity asset/runtime overhead).  Used to report
 * a chunk's true memory footprint to the streaming budget — the raw chunk
 * JSON is tiny vs. what it spawns, so a JSON-byte budget never triggers
 * LRU/pressure (audit F3).  A heuristic, not exact; the point is that the
 * accounted size scales with residency rather than source bytes. */
#define JCE_WS_BYTES_PER_ENTITY  8192u

/* Per-frame chunk-APPLY budget (entity spawning happens on the main/render
 * thread, so it cannot be offloaded — only spread).  We spawn entities a
 * slice at a time and stop once we have either spawned this many entities or
 * spent the configured wall-clock budget on apply work this frame, whichever
 * comes first.  Reading the chunk bytes is already bounded separately by the
 * streaming I/O layer's frame_budget_ms (jce_streaming_update). */
#define JCE_WS_APPLY_ENTITIES_PER_SLICE  64

/* VRAM ceiling: re-query a freshly-applied chunk's real residency for this many
 * updates after finalize so async model/texture decodes that land late are
 * reflected in the budget.  Bounded so it never costs more than a brief window
 * of O(resident-entities) queries per chunk. */
#define JCE_WS_RESIDENCY_CONVERGE_FRAMES  90

static double ws_now_ms(void)
{
    uint64_t freq = jce_time_perf_freq();
    if (!freq) return 0.0;
    return (double)jce_time_perf_counter() / (double)freq * 1000.0;
}

/* P3-A.2 — streaming pressure → texture mip-bias bridge.
 *
 * Translates the three-level streaming pressure signal into a global
 * mip bias that the renderer/texture system honors when scheduling
 * mip residency.  Registered as an internal chained hook so it never
 * stomps on the user-facing pressure callback slot.
 *
 *   OK   -> bias 0  (full quality)
 *   SOFT -> bias 1  (drop top mip)
 *   HARD -> bias 2  (drop two top mips)
 */
static void mip_streaming_pressure_hook(JceStreamingPressure prev,
                                         JceStreamingPressure curr,
                                         uint64_t              memory_used,
                                         uint64_t              budget_bytes,
                                         void                 *user_data)
{
    (void)prev; (void)memory_used; (void)budget_bytes; (void)user_data;
    int8_t bias;
    switch (curr) {
    case JCE_STREAM_PRESSURE_SOFT: bias = 1; break;
    case JCE_STREAM_PRESSURE_HARD: bias = 2; break;
    case JCE_STREAM_PRESSURE_OK:   /* fallthrough */
    default:                       bias = 0; break;
    }
    jce_texture_set_global_mip_bias(bias);
    LOG_DEBUG(LOG_TAG, "streaming pressure %s -> global texture mip bias %d",
              jce_streaming_pressure_name(curr), (int)bias);
}

/* ── Per-chunk entity roster ─────────────────────────────────────── */

typedef struct {
    uint32_t   chunk_id;
    bool       active;       /* true = registered */

    /* Pending deferred load (set from worker thread, consumed on main). */
    SDL_AtomicInt pending_apply; /* 1 = data ready to apply, 0 = idle   */
    void         *pending_data;
    size_t        pending_size;
    bool          pending_ok;

    /* In-flight time-sliced apply (main thread only).  When a chunk's bytes
     * arrive we parse the JSON once (apply_root) and open a streaming load
     * (apply_stream); jce_world_streamer_update() then spawns a few entities
     * per frame under a wall-clock budget and finalizes when drained.  This
     * spreads the per-chunk spawn cost across frames instead of stalling the
     * frame that the chunk happened to finish loading on. */
    JceJson            *apply_root;   /* owned parsed JSON, freed at finalize */
    JceSceneLoadStream *apply_stream; /* borrows apply_root + the scene        */
    size_t              apply_size;   /* source byte count for residency calc  */

    /* Entities spawned by this chunk (dynamic, owned by us). */
    JceEntity *entities;
    uint32_t   entity_count;
    uint32_t   entity_cap;

    /* VRAM ceiling: last residency value reported to the streaming budget for
     * this chunk + a small convergence window.  A chunk's async model decodes
     * may land several frames AFTER its entities spawn, so the real GPU bytes
     * grow over the next few frames; we re-query for a bounded window and
     * re-report only when the value changes (so the budget tracks real VRAM
     * without per-frame churn).  converge_left counts the remaining re-queries. */
    uint64_t   reported_bytes;
    int        converge_left;
} ChunkRoster;

/* ── World streamer struct ───────────────────────────────────────── */

struct JceWorldStreamer {
    JceStreamingSystem *ss;
    JceScene           *scene;

    /* Copy of the creation config (the config has no setters — callers
     * recreate the streamer to change it; this is what get_config returns). */
    JceWorldStreamConfig config;

    ChunkRoster         rosters[MAX_WORLD_CHUNKS];
    uint32_t            roster_count;

    /* Aggregate stats (updated each apply). */
    uint32_t            total_entities;

    /* Editor hierarchy-integration callbacks (optional; NULL in the runtime). */
    JceWorldStreamerEntityCb on_spawn;
    JceWorldStreamerEntityCb on_despawn;
    void                    *entity_cb_user;

    /* Per-chunk load/unload callback (optional; NULL in the runtime).  Carries
     * the chunk id so a caller can toggle a per-chunk resource such as an HLOD
     * proxy.  Separate user pointer from the entity callbacks above. */
    JceWorldStreamerChunkCb  on_chunk_state;
    void                    *chunk_cb_user;

    /* Real-residency query (VRAM ceiling): maps a chunk's entities to their real
     * GPU footprint so the streaming budget accounts for actual VRAM, not a flat
     * per-entity estimate.  NULL => estimate-only (legacy behaviour). */
    JceWorldStreamerResidencyFn residency_query;
    void                       *residency_user;

    /* ── HLOD far-skyline proxy coordination (jce_world_streamer_attach_hlod) ─
     * Chunk-id -> always-resident proxy entity ("HLOD_<gx>_<gz>"), resolved
     * once at attach.  When attached, on_chunk_state above points at the
     * internal hlod_chunk_state_cb, which hides/shows the proxy then chains to
     * hlod_extra_cb/hlod_extra_ud below (e.g. the editor's hierarchy grouping).
     * MAX_WORLD_CHUNKS-sized parallel arrays keep this allocation-free and in
     * lockstep with the roster pool. */
    bool      hlod_attached;
    uint32_t  hlod_count;
    uint32_t  hlod_chunk_ids[MAX_WORLD_CHUNKS];
    JceEntity hlod_proxies  [MAX_WORLD_CHUNKS];
    /* Cross-fade hand-off (Direction B): per-proxy wall-clock deadline (ms, from
     * ws_now_ms()) at which a freshly-loaded chunk's still-visible proxy is to be
     * hidden — i.e. once the detail entities have dithered in over it.  0 = no
     * pending hide (steady state / proxy already hidden or shown).  Advanced +
     * fired in jce_world_streamer_update.  In lockstep with hlod_proxies[]. */
    double    hlod_hide_at_ms[MAX_WORLD_CHUNKS];
    JceScene *hlod_scene;                 /* scene the proxies live in */
    JceWorldStreamerChunkCb hlod_extra_cb;
    void                   *hlod_extra_ud;
};

/* ── Roster helpers ──────────────────────────────────────────────── */

static void roster_abort_apply(JceWorldStreamer *ws, ChunkRoster *r);

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
    /* Notify the editor BEFORE destroying, while the ids are still valid, so it
     * can pull these entities out of its hierarchy/selection mirror. */
    if (ws->on_despawn && r->entity_count)
        ws->on_despawn(r->entities, r->entity_count, ws->entity_cb_user);

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

    /* Drop any earlier staged bytes not yet consumed (defensive: avoids a
     * leak if a second load completes before update() promoted the first). */
    JCE_FREE(r->pending_data);
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

    /* Tell the caller this chunk is going away (e.g. show its HLOD proxy) BEFORE
     * the detailed entities are destroyed, so there is never a visible gap. */
    if (ws->on_chunk_state)
        ws->on_chunk_state(chunk_id, false, ws->chunk_cb_user);

    /* If this chunk was still being applied across frames, abort that first
     * (destroys the partial entities) so it cannot finalize after unload. */
    roster_abort_apply(ws, r);
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

    ws->scene  = scene;
    ws->config = *config;

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
    (void)thread_pool;

    jce_streaming_set_callbacks(ws->ss, on_chunk_loaded, on_chunk_unloaded, ws);

    /* P3-A.2: install runtime mip-streaming pressure hook so the
     * texture system drops/restores top mips automatically as the
     * streaming budget tightens.  Chained so the user-facing
     * pressure callback slot stays free. */
    jce_streaming_add_pressure_hook(ws->ss, mip_streaming_pressure_hook, NULL);

    LOG_INFO(LOG_TAG, "world streamer created (r=%.0f/%.0f, budget=%u MiB)",
             config->load_radius, config->unload_radius, config->budget_mb);
    return ws;
}

void jce_world_streamer_set_entity_callbacks(
    JceWorldStreamer         *ws,
    JceWorldStreamerEntityCb  on_spawn,
    JceWorldStreamerEntityCb  on_despawn,
    void                     *user)
{
    if (!ws) return;
    ws->on_spawn       = on_spawn;
    ws->on_despawn     = on_despawn;
    ws->entity_cb_user = user;
}

void jce_world_streamer_set_chunk_callback(
    JceWorldStreamer        *ws,
    JceWorldStreamerChunkCb  on_chunk_state,
    void                    *user)
{
    if (!ws) return;
    ws->on_chunk_state = on_chunk_state;
    ws->chunk_cb_user  = user;
}

void jce_world_streamer_set_residency_query(
    JceWorldStreamer            *ws,
    JceWorldStreamerResidencyFn  query,
    void                        *user)
{
    if (!ws) return;
    ws->residency_query = query;
    ws->residency_user  = user;
}

/* Compute a roster's residency bytes to report to the streaming budget: the real
 * GPU footprint via the residency_query when available + non-zero, else the
 * per-entity estimate (source JSON bytes + 8 KiB/entity).  The estimate is the
 * floor so a chunk is never accounted as zero while its async model decodes are
 * still in flight (which would let the budget over-admit). */
static uint64_t roster_residency_bytes(JceWorldStreamer *ws, ChunkRoster *r)
{
    uint64_t estimate = (uint64_t)r->apply_size +
                        (uint64_t)r->entity_count * JCE_WS_BYTES_PER_ENTITY;
    if (!ws->residency_query || r->entity_count == 0)
        return estimate;
    uint64_t real = ws->residency_query(r->entities, r->entity_count,
                                        ws->residency_user);
    /* Real bytes once models have uploaded; keep the lightweight estimate as a
     * floor for ECS/runtime overhead the GPU figure doesn't capture. */
    return real > estimate ? real : estimate;
}

/* ── HLOD far-skyline proxy coordination ─────────────────────────────
 * Moved here from the editor so BOTH the editor (scene-view preview + Play)
 * and the standalone runtime (default_main) hide a chunk's cheap far-proxy
 * once its detailed geometry streams in — the shipped exe no longer
 * double-draws / z-fights the proxy box over the real buildings.  The proxies
 * are always-resident entities named "HLOD_<gx>_<gz>" baked into the master
 * scene (tools/worldgen/gen_hlod.py); we map chunk id -> proxy entity once at
 * attach from the scene's authored streaming table.  See the header for the
 * rationale behind matching by the proxy's EditorMeta name (survives cook in both
 * builds — verified against the cooked street_demo master scene). */

/* Parse "<dir>/cell_<gx>_<gz>.scene.json" -> the proxy name "HLOD_<gx>_<gz>".
 * Returns false when the path is not a chunk fragment of that form. */
static bool hlod_proxy_name_from_chunk_path(const char *path,
                                            char *out, size_t out_sz)
{
    if (!path || !out || out_sz == 0) return false;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strncmp(base, "cell_", 5) != 0) return false;
    const char *coords = base + 5;
    /* Strip a trailing ".scene.json" (or any extension) so only "<gx>_<gz>"
     * remains; the two integers can each be negative. */
    char buf[64];
    size_t n = 0;
    for (const char *p = coords; *p && *p != '.' && n + 1 < sizeof(buf); ++p)
        buf[n++] = *p;
    buf[n] = '\0';
    int gx = 0, gz = 0;
    if (sscanf(buf, "%d_%d", &gx, &gz) != 2) return false;
    snprintf(out, out_sz, "HLOD_%d_%d", gx, gz);
    return true;
}

/* jce_scene_each_entity callback context: find the entity whose EditorMeta name
 * matches `want`.  The shared scene loader stamps the authored "name" onto
 * EditorMeta in both the editor and the cooked runtime scene, so this resolves
 * the proxy engine-side without any editor-only state. */
typedef struct {
    const char *want;
    JceEntity   found;
} HlodNameFind;

static void hlod_name_find_cb(JceScene *s, JceEntity e, void *ud)
{
    HlodNameFind *f = (HlodNameFind *)ud;
    if (f->found != JCE_ENTITY_INVALID) return;   /* first match wins */
    JceEditorMeta *m = jce_scene_get_editor_meta(s, e);
    if (m && strcmp(m->name, f->want) == 0) f->found = e;
}

/* Single-name lookup — the chunk load/unload event path (one name per
 * transition; a full-scene walk there is an acceptable, event-rate cost). */
static JceEntity hlod_find_proxy(JceScene *scene, const char *name)
{
    HlodNameFind f;
    f.want  = name;
    f.found = JCE_ENTITY_INVALID;
    jce_scene_each_entity(scene, hlod_name_find_cb, &f);
    return f.found;
}

/* ── Batch proxy resolution (attach) ─────────────────────────────────
 * Resolving every chunk's proxy with hlod_find_proxy was O(chunks x scene):
 * street_demo = 196 chunks x 53k entities ~= 10M strcmp+meta lookups on every
 * attach (scene load / Play start).  Invert the loop: hash all wanted names
 * once (chunks <= 1024, open addressing, 2x capacity), then ONE scene pass
 * matches each entity's name against the table — O(scene + chunks). */
typedef struct {
    const char (*names)[64];     /* wanted proxy names, parallel to slots */
    JceEntity   *out;            /* resolved entity per wanted name       */
    uint16_t    *idx;            /* name-hash -> want+1 (0 = empty)       */
    uint32_t     cap;            /* index capacity (power of two)         */
    uint32_t     want_count;
} HlodBatchFind;

static void hlod_batch_find_cb(JceScene *s, JceEntity e, void *ud)
{
    HlodBatchFind *b = (HlodBatchFind *)ud;
    JceEditorMeta *m = jce_scene_get_editor_meta(s, e);
    if (!m || !m->name[0]) return;
    uint32_t mask = b->cap - 1u;
    uint32_t h = jce_fnv1a32_str(m->name) & mask;
    while (b->idx[h]) {
        uint32_t w = (uint32_t)b->idx[h] - 1u;
        if (strcmp(b->names[w], m->name) == 0) {
            if (b->out[w] == JCE_ENTITY_INVALID)  /* first match wins */
                b->out[w] = e;
            return;
        }
        h = (h + 1u) & mask;
    }
}

static void hlod_set_proxy_visible(JceScene *scene, JceEntity proxy, bool visible)
{
    if (!scene || proxy == JCE_ENTITY_INVALID) return;
    if (!jce_scene_has_editor_meta(scene, proxy)) return;   /* gone */
    /* Toggle the MeshRenderer: the renderer skips a mesh whose MeshRenderer is
     * disabled (scene-view + game-view + runtime share that path), and the
     * data persists either way. */
    jce_scene_set_component_enabled(scene, proxy,
                                    JCE_COMP_FLAG_MESH_RENDERER, visible);
}

/* Reset every mapped proxy to VISIBLE — the baseline before any chunk is
 * resident (resident chunks hide theirs on load). */
static void hlod_show_all(JceWorldStreamer *ws)
{
    for (uint32_t i = 0; i < ws->hlod_count; ++i)
        hlod_set_proxy_visible(ws->hlod_scene, ws->hlod_proxies[i], true);
}


/* Streamer chunk-state hook installed by attach (Direction B cross-fade):
 *  • chunk RESIDENT (loaded): KEEP the proxy visible for now and schedule it to
 *    be hidden JCE_WS_PROXY_HIDE_DELAY_MS later (jce_world_streamer_update fires
 *    it).  The detail entities dither in (renderer screen-door) over the still-
 *    visible proxy during that window, so the cell never pops / shows through.
 *  • chunk GONE (unloaded): SHOW the proxy immediately and cancel any pending
 *    hide, so the far skyline is never missing while the detail is torn down.
 * Then chain to any extra caller callback (the editor's hierarchy chunk-
 * grouping).  Fired per chunk load/unload on the main thread (never per frame). */
static void hlod_chunk_state_cb(uint32_t chunk_id, bool loaded, void *user)
{
    JceWorldStreamer *ws = (JceWorldStreamer *)user;
    for (uint32_t i = 0; i < ws->hlod_count; ++i) {
        if (ws->hlod_chunk_ids[i] != chunk_id) continue;
        JceEntity proxy = ws->hlod_proxies[i];
        if (loaded) {
            /* Detail just became resident: hold the proxy and arm the delayed
             * hide so the detail can fade in underneath it first. */
            hlod_set_proxy_visible(ws->hlod_scene, proxy, /*visible=*/true);
            ws->hlod_hide_at_ms[i] = ws_now_ms() + JCE_WS_PROXY_HIDE_DELAY_MS;
        } else {
            /* Detail gone: bring the proxy back at once, drop any pending hide. */
            ws->hlod_hide_at_ms[i] = 0.0;
            hlod_set_proxy_visible(ws->hlod_scene, proxy, /*visible=*/true);
        }
        break;
    }
    if (ws->hlod_extra_cb)
        ws->hlod_extra_cb(chunk_id, loaded, ws->hlod_extra_ud);
}

void jce_world_streamer_attach_hlod(JceWorldStreamer        *ws,
                                    JceScene                *scene,
                                    JceWorldStreamerChunkCb  extra_cb,
                                    void                    *extra_ud)
{
    if (!ws) return;
    if (!scene) scene = ws->scene;

    ws->hlod_count    = 0;
    ws->hlod_scene    = scene;
    ws->hlod_extra_cb = extra_cb;
    ws->hlod_extra_ud = extra_ud;
    ws->hlod_attached = true;

    const JceSceneStreamingSettings *st =
        scene ? jce_scene_get_streaming_settings(scene) : NULL;
    if (st && st->chunk_count > 0) {
        /* Pass 1: derive every chunk's wanted proxy name + hash it (O(chunks)).
         * Pass 2: ONE scene walk resolves all of them (O(scene)).  This
         * replaced a per-chunk full-scene walk — O(chunks x scene), ~10M
         * strcmp on a 196-chunk / 53k-entity streamed city — per attach. */
        enum { HLOD_IDX_CAP = MAX_WORLD_CHUNKS * 2 };  /* pow2, 2x load */
        /* static: ~70KB of attach-time scratch off the stack; attach runs on
         * the main thread only (scene load / Play start), never reentrant. */
        static char      s_names[MAX_WORLD_CHUNKS][64];
        static JceEntity s_found[MAX_WORLD_CHUNKS];
        static uint32_t  s_want_chunk[MAX_WORLD_CHUNKS];
        static uint16_t  s_idx[HLOD_IDX_CAP];
        memset(s_idx, 0, sizeof(s_idx));
        uint32_t want = 0;
        for (uint32_t i = 0;
             i < st->chunk_count && want < MAX_WORLD_CHUNKS; ++i) {
            const JceSceneStreamChunk *c = &st->chunks[i];
            if (c->path[0] == '\0') continue;
            if (!hlod_proxy_name_from_chunk_path(c->path, s_names[want],
                                                 sizeof s_names[want]))
                continue;
            s_found[want]      = JCE_ENTITY_INVALID;
            s_want_chunk[want] = c->id;
            uint32_t h = jce_fnv1a32_str(s_names[want]) & (HLOD_IDX_CAP - 1u);
            while (s_idx[h] &&
                   strcmp(s_names[s_idx[h] - 1u], s_names[want]) != 0)
                h = (h + 1u) & (HLOD_IDX_CAP - 1u);
            if (!s_idx[h]) s_idx[h] = (uint16_t)(want + 1u);
            /* duplicate name: first want wins the index; the duplicate simply
             * resolves to the same entity below via its own linear check. */
            want++;
        }
        if (want > 0) {
            HlodBatchFind b;
            b.names      = (const char (*)[64])s_names;
            b.out        = s_found;
            b.idx        = s_idx;
            b.cap        = HLOD_IDX_CAP;
            b.want_count = want;
            jce_scene_each_entity(scene, hlod_batch_find_cb, &b);
            for (uint32_t w = 0;
                 w < want && ws->hlod_count < MAX_WORLD_CHUNKS; ++w) {
                JceEntity proxy = s_found[w];
                /* Duplicate names share the index cell; resolve them off the
                 * winning cell's result. */
                if (proxy == JCE_ENTITY_INVALID) {
                    uint32_t h = jce_fnv1a32_str(s_names[w]) & (HLOD_IDX_CAP - 1u);
                    while (s_idx[h]) {
                        uint32_t o = (uint32_t)s_idx[h] - 1u;
                        if (strcmp(s_names[o], s_names[w]) == 0) {
                            proxy = s_found[o];
                            break;
                        }
                        h = (h + 1u) & (HLOD_IDX_CAP - 1u);
                    }
                }
                if (proxy != JCE_ENTITY_INVALID) {
                    ws->hlod_chunk_ids [ws->hlod_count] = s_want_chunk[w];
                    ws->hlod_proxies   [ws->hlod_count] = proxy;
                    ws->hlod_hide_at_ms[ws->hlod_count] = 0.0;
                    ws->hlod_count++;
                }
            }
        }
    }

    /* Baseline: all proxies visible (already-resident chunks hide theirs as the
     * streamer fires load events).  Then route chunk state through the internal
     * HLOD callback (which chains to extra_cb). */
    hlod_show_all(ws);
    jce_world_streamer_set_chunk_callback(ws, hlod_chunk_state_cb, ws);

    LOG_INFO(LOG_TAG, "HLOD proxy coordination attached (%u proxy/chunk)",
             ws->hlod_count);
}

void jce_world_streamer_destroy(JceWorldStreamer *ws)
{
    if (!ws) return;

    /* Re-show every HLOD proxy so the always-resident master skyline is whole
     * again once this streamer is gone (no chunk is resident to hide them).
     * Matters for the editor, where the master scene outlives the streamer
     * (Play/preview end); harmless in the runtime, where the scene is torn down
     * with the app. */
    if (ws->hlod_attached) hlod_show_all(ws);

    /* Destroy all streamed entities. */
    for (uint32_t i = 0; i < ws->roster_count; ++i) {
        ChunkRoster *r = &ws->rosters[i];
        if (!r->active) continue;
        roster_abort_apply(ws, r);
        roster_free_entities(ws, r);

        /* Free any pending staging buffer. */
        JCE_FREE(r->pending_data);
        r->pending_data = NULL;
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

void jce_world_streamer_register_from_scene_settings(
    JceWorldStreamer                *ws,
    const JceSceneStreamingSettings *settings)
{
    if (!ws || !settings) return;

    uint32_t n = settings->chunk_count;
    if (n > MAX_WORLD_CHUNKS) n = MAX_WORLD_CHUNKS;

    uint32_t registered = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const JceSceneStreamChunk *c = &settings->chunks[i];
        if (c->path[0] == '\0') continue;     /* placeholder row — skip */
        jce_vec3 center = jce_v3(c->center[0], c->center[1], c->center[2]);
        jce_world_streamer_register_chunk(ws, c->id, center, c->radius,
                                          c->path);
        registered++;
    }
    if (registered)
        LOG_INFO(LOG_TAG, "registered %u chunk(s) from scene settings",
                 registered);
}

void jce_world_streamer_unregister_chunk(JceWorldStreamer *ws, uint32_t chunk_id)
{
    if (!ws) return;

    ChunkRoster *r = find_roster(ws, chunk_id);
    if (r) {
        roster_abort_apply(ws, r);
        roster_free_entities(ws, r);

        JCE_FREE(r->pending_data);
        r->pending_data = NULL;
        r->active = false;
    }

    jce_streaming_unregister_chunk(ws->ss, chunk_id);
}

/* ── Per-frame update ────────────────────────────────────────────── */

/* Abandon an in-flight apply stream (chunk unregistered/unloaded mid-apply).
 * finalize() is always safe to call and runs ref-fixups over whatever was
 * created so far; we then destroy those partial entities since the chunk is
 * going away, so nothing leaks into the live scene. */
static void roster_abort_apply(JceWorldStreamer *ws, ChunkRoster *r)
{
    if (r->apply_stream) {
        /* Capture the partial roster (O(new)) before finalize frees the map. */
        uint32_t   n  = jce_scene_load_stream_new_entities(r->apply_stream, NULL, 0);
        JceEntity *ids = NULL;
        if (n) {
            ids = (JceEntity *)JCE_MALLOC((size_t)n * sizeof(JceEntity));
            if (ids)
                n = jce_scene_load_stream_new_entities(r->apply_stream, ids, n);
            else
                n = 0;
        }
        (void)jce_scene_load_stream_finalize(r->apply_stream);
        r->apply_stream = NULL;
        for (uint32_t i = 0; i < n; ++i)
            jce_scene_destroy_entity(ws->scene, ids[i]);
        JCE_FREE(ids);
    }
    if (r->apply_root) { jce_json_free(r->apply_root); r->apply_root = NULL; }
    r->apply_size = 0;
}

/* Finish an in-flight apply: run ref fixups, capture the per-chunk entity
 * roster in O(new) from the stream's remap table, fire callbacks, account
 * residency.  Returns true if a chunk was finalized. */
static void roster_finalize_apply(JceWorldStreamer *ws, ChunkRoster *r)
{
    JceSceneLoadStream *st = r->apply_stream;

    /* Pull the created-entity roster straight from the stream (O(new)),
     * before finalize() frees the remap table. */
    uint32_t new_count = jce_scene_load_stream_new_entities(st, NULL, 0);
    JceEntity *new_ents = NULL;
    if (new_count) {
        new_ents = (JceEntity *)JCE_MALLOC((size_t)new_count * sizeof(JceEntity));
        if (new_ents)
            new_count = jce_scene_load_stream_new_entities(st, new_ents, new_count);
        else
            new_count = 0;
    }

    (void)jce_scene_load_stream_finalize(st);   /* runs parent/ref fixups */
    r->apply_stream = NULL;
    if (r->apply_root) { jce_json_free(r->apply_root); r->apply_root = NULL; }

    r->entities     = new_ents;
    r->entity_count = new_count;
    r->entity_cap   = new_count;
    ws->total_entities += new_count;

    if (ws->on_spawn && new_count)
        ws->on_spawn(new_ents, new_count, ws->entity_cb_user);

    if (ws->on_chunk_state)
        ws->on_chunk_state(r->chunk_id, true, ws->chunk_cb_user);

    /* Report REAL VRAM residency (via the residency query) when available so the
     * streaming budget + LRU evict against actual GPU bytes, not a flat estimate
     * (audit F3 / VRAM ceiling).  At this instant a chunk's async model decodes
     * may still be in flight, so the query can read low; the per-update
     * convergence pass below re-reports until the real bytes settle. */
    r->reported_bytes = roster_residency_bytes(ws, r);
    r->converge_left  = ws->residency_query ? JCE_WS_RESIDENCY_CONVERGE_FRAMES : 0;
    jce_streaming_set_chunk_residency(ws->ss, r->chunk_id, r->reported_bytes);

    LOG_INFO(LOG_TAG, "chunk %u applied: %u entities spawned",
             r->chunk_id, new_count);
    /* Keep apply_size for the convergence re-report (cleared when the chunk
     * unloads / its roster is recycled). */
}

void jce_world_streamer_update(JceWorldStreamer *ws, jce_vec3 camera_pos)
{
    if (!ws) return;
    JCE_PROFILE_ZONE_N("WorldStreamer::Update");

    /* Drive I/O loads/unloads. */
    jce_streaming_update(ws->ss, camera_pos);

    /* HLOD cross-fade hand-off (Direction B): hide each freshly-loaded chunk's
     * proxy once its delay has elapsed — by now the detail entities have
     * dithered in over it (renderer screen-door), so the swap is seamless.
     * 0 = no pending hide.  Only when HLOD is attached; otherwise inert. */
    if (ws->hlod_attached) {
        const double now_ms = ws_now_ms();
        for (uint32_t i = 0; i < ws->hlod_count; ++i) {
            if (ws->hlod_hide_at_ms[i] != 0.0 &&
                now_ms >= ws->hlod_hide_at_ms[i]) {
                hlod_set_proxy_visible(ws->hlod_scene, ws->hlod_proxies[i],
                                       /*visible=*/false);
                ws->hlod_hide_at_ms[i] = 0.0;   /* fired */
            }
        }
    }

    /* Chunk APPLY (JSON parse + entity spawn) runs on the main/render thread
     * and cannot be offloaded, only spread.  Spawn at most a few entities per
     * frame and stop once a wall-clock budget is spent, so a chunk that just
     * finished loading no longer stalls the frame it landed on.  Reuse the
     * I/O frame_budget_ms; fall back to a small default if unset. */
    const double budget_ms = (ws->config.frame_budget_ms > 0.0f)
                                 ? (double)ws->config.frame_budget_ms
                                 : 2.0;
    const double start_ms  = ws_now_ms();
    bool finalized_one     = false;   /* keep GPU-resource bursts to ≤1/frame */

    for (uint32_t i = 0; i < ws->roster_count; ++i) {
        ChunkRoster *r = &ws->rosters[i];
        if (!r->active) continue;

        /* 1. Promote a freshly-loaded chunk's bytes into an in-flight apply
         *    stream: parse the JSON once, open the time-sliced loader.  Only
         *    when this roster isn't already mid-apply. */
        if (!r->apply_stream &&
            SDL_CompareAndSwapAtomicInt(&r->pending_apply, 1, 0)) {

            void  *data = r->pending_data;
            size_t size = r->pending_size;
            r->pending_data = NULL;

            if (!r->pending_ok || !data) {
                JCE_FREE(data);
            } else {
                /* Remove stale entities from a previous load (hot-reload). */
                roster_free_entities(ws, r);

                JceJson *root = jce_json_parse((const char *)data, size);
                JCE_FREE(data);
                if (!root) {
                    LOG_ERROR(LOG_TAG, "chunk %u: JSON parse failed", r->chunk_id);
                } else {
                    JceSceneLoadStream *st =
                        jce_scene_load_stream_begin(ws->scene, root, NULL);
                    if (!st) {
                        jce_json_free(root);
                        LOG_ERROR(LOG_TAG,
                                  "chunk %u: scene stream begin failed",
                                  r->chunk_id);
                    } else {
                        r->apply_root   = root;
                        r->apply_stream = st;
                        r->apply_size   = size;
                    }
                }
            }
        }

        /* 2. Drive an in-flight apply a slice at a time under the budget.
         *    Finalize at most one chunk per frame to cap the GPU-resource
         *    burst that the spawn + ref-fixup pass triggers. */
        if (r->apply_stream) {
            while (!jce_scene_load_stream_done(r->apply_stream)) {
                jce_scene_load_stream_step(r->apply_stream,
                                           JCE_WS_APPLY_ENTITIES_PER_SLICE);
                if (ws_now_ms() - start_ms >= budget_ms)
                    break;   /* resume this chunk next frame */
            }

            if (jce_scene_load_stream_done(r->apply_stream)) {
                if (finalized_one) {
                    /* Hold the completed first pass; finalize next frame so we
                     * never run two finalize fixup+spawn bursts in one frame. */
                } else {
                    roster_finalize_apply(ws, r);
                    finalized_one = true;
                }
            }
        }

        /* Out of budget — leave remaining chunks for the next frame. */
        if (ws_now_ms() - start_ms >= budget_ms)
            break;
    }

    /* ── VRAM ceiling: residency convergence ────────────────────────────
     * A chunk's async model/texture decodes can land several frames after its
     * entities spawn, so the real GPU bytes grow over the next few frames.  For
     * a bounded window after finalize, re-query each applied chunk's real
     * residency and re-report it to the streaming budget only when it changed
     * (so the budget tracks actual VRAM without per-frame churn).  Skipped
     * entirely when no residency query is installed (estimate-only legacy). */
    if (ws->residency_query) {
        for (uint32_t i = 0; i < ws->roster_count; ++i) {
            ChunkRoster *r = &ws->rosters[i];
            if (!r->active || r->converge_left <= 0) continue;
            if (r->apply_stream) continue;   /* still applying — not finalized */
            r->converge_left--;
            uint64_t now = roster_residency_bytes(ws, r);
            if (now != r->reported_bytes) {
                r->reported_bytes = now;
                jce_streaming_set_chunk_residency(ws->ss, r->chunk_id, now);
            }
        }
    }
    JCE_PROFILE_ZONE_END;
}

/* ── Preview / authoring load-override ────────────────────────────── */

void jce_world_streamer_set_preview_load(JceWorldStreamer    *ws,
                                         JceStreamPreviewMode mode,
                                         const uint32_t      *filter_ids,
                                         uint32_t             count)
{
    if (!ws) return;
    jce_streaming_set_preview(ws->ss, mode, filter_ids, count);
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

JceWorldStreamConfig jce_world_streamer_get_config(const JceWorldStreamer *ws)
{
    return ws ? ws->config : jce_world_stream_config_default();
}

JceStreamingPressure jce_world_streamer_pressure(const JceWorldStreamer *ws)
{
    return ws ? jce_streaming_get_pressure(ws->ss) : JCE_STREAM_PRESSURE_OK;
}

JceStreamingPressure jce_world_streamer_pressure_high_water(
    const JceWorldStreamer *ws)
{
    return ws ? jce_streaming_pressure_high_water(ws->ss)
              : JCE_STREAM_PRESSURE_OK;
}

uint32_t jce_world_streamer_refused_loads(const JceWorldStreamer *ws)
{
    return ws ? jce_streaming_refused_loads(ws->ss) : 0;
}

JceChunkState jce_world_streamer_chunk_state(const JceWorldStreamer *ws,
                                             uint32_t chunk_id)
{
    return ws ? jce_streaming_chunk_state(ws->ss, chunk_id)
              : JCE_CHUNK_UNLOADED;
}
