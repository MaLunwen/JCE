/*
 * jce_asset_cache_internal.h  Shared state for scene asset cache files.
 */

#ifndef JCE_ASSET_CACHE_INTERNAL_H
#define JCE_ASSET_CACHE_INTERNAL_H

#include "io/jce_editor_file_util.h"
#include "jce_editor_scene_asset_cache.h"
#include "jce_model_loader_assimp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_thread.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_texture.h>
#include <jce/resource/jce_asset.h>
#include <jce/resource/jce_asset_format.h>
#include <jce/resource/jce_image_decode.h>
}

#define LOG_TAG "scene_asset_cache"

/* ── Cache entry types ──────────────────────────────────────────── */

struct MeshCacheEntry {
    /* Path buffer must accommodate the longest absolute mesh path the
     * editor will ever cache. KayKit/Kenney asset packs nest content
     * deeply (e.g. .../KayKit_Medieval_Hexagon_Pack_1.0_FREE/Assets/
     * fbx/buildings/blue/building_archeryrange_blue.fbx), easily
     * exceeding 128 bytes. A truncated key silently de-syncs from the
     * full path used at lookup time, causing async load completions to
     * orphan their cache entry and the entity to fall back to the cube
     * shape with no error logged. */
    char     path[512];
    JceMesh *mesh;
    bool     requested;
    bool     failed;
    uint64_t request_generation;
};

struct TextureCacheEntry {
    /* See MeshCacheEntry::path comment — same truncation issue applies
     * to texture cache keys (resolved absolute paths can be long). */
    char           path[512];
    /* FNV-1a of path, compared before strcmp: keys are resolved absolute
     * paths sharing long directory prefixes, and the linear scan runs
     * hundreds of times per frame from sr_resolve_texture. */
    uint32_t       path_hash;
    JceTexture     tex;
    JceAssetHandle asset_handle;
    bool           tex_from_asset_manager;
    bool           warned_missing;
    bool           requested;
    bool           failed;
    uint64_t       request_generation;
};

/* ── Shared cache state ─────────────────────────────────────────── */

struct AssetCacheState {
    bool              initialized;
    JceAssetManager  *assets;
    MeshCacheEntry    mesh_cache[512];
    int               mesh_cache_count;
    TextureCacheEntry tex_cache[256];
    int               tex_cache_count;
    char              scene_dir[512];
};

extern AssetCacheState s_cache;

/* ── Locking helper ────────────────────────────────────────────── */

/* RAII guard around JceMutex — replaces std::lock_guard usage. */
struct JceMutexGuard {
    JceMutex *m;
    explicit JceMutexGuard(JceMutex *mtx) : m(mtx) { jce_mutex_lock(m); }
    ~JceMutexGuard() { jce_mutex_unlock(m); }
    JceMutexGuard(const JceMutexGuard &) = delete;
    JceMutexGuard &operator=(const JceMutexGuard &) = delete;
};

/* ── Shared async-loader scaffold ──────────────────────────────── */

/*
 * The mesh and texture caches each own one persistent worker thread that drains a
 * `pending` request queue and publishes results the main thread finalizes
 * under a per-frame budget.  Only the decode step and the finalize step
 * differ, so the worker / mutex / condvar / queue / generation plumbing
 * lives here once, parameterised on the request and result types.
 *
 * Threading contract (identical to the hand-rolled copies this replaces):
 *   - `pending`, `completed`, `generation`, `next_order` and `stop` are
 *     only ever touched with `mutex` held.
 *   - The worker decodes; it never touches a GPU resource.  Every
 *     jce_mesh_create / jce_texture_from_rgba call happens on the main
 *     thread inside async_loader_drain_completed().
 *   - `generation` invalidates in-flight work: the drain drops any result
 *     whose generation no longer matches instead of applying it.
 *   - async_loader_stop() joins the worker *before* the queues are
 *     cleared, so a worker can never outlive the state it writes into.
 *
 * The material cache deliberately does NOT use this scaffold — see the
 * note above MaterialAsyncState.
 */

template <typename Request, typename Result>
struct AsyncLoaderState {
    /* Named so the main-thread drain can hold a batch of results. */
    typedef Result ResultType;

    JceThread            *worker;
    JceMutex             *mutex;
    JceCondVar           *cv;
    std::vector<Request>  pending;
    std::vector<Result>   completed;
    /* Monotonic arrival counter stamped onto each queued request; queues
     * that pop by priority use it as the FIFO tie-break.  Reset with the
     * generation. */
    uint32_t              next_order;
    uint64_t              generation;
    bool                  running;
    bool                  stop;
};

/* What the main-thread finalize step decided about one result. */
enum AsyncFinalizeAction {
    ASYNC_FINALIZE_DROPPED,  /* stale or unusable; released, no budget spent */
    ASYNC_FINALIZE_DEFERRED, /* usable but out of budget; retry next frame   */
    ASYNC_FINALIZE_APPLIED   /* uploaded on the main thread; spends one slot */
};

/* Creates the sync primitives, resets the queues and spawns the worker.
 * Callers guard on `running` themselves so a second start cannot reset
 * state a live worker is using. */
template <typename State>
bool async_loader_start(State &st, JceThreadFn worker_main,
                        const char *thread_name)
{
    if (!st.mutex) st.mutex = jce_mutex_create();
    if (!st.cv)    st.cv    = jce_cond_create();
    if (!st.mutex || !st.cv) {
        if (st.cv) {
            jce_cond_destroy(st.cv);
            st.cv = NULL;
        }
        if (st.mutex) {
            jce_mutex_destroy(st.mutex);
            st.mutex = NULL;
        }
        st.running = false;
        return false;
    }

    st.next_order = 0;
    st.generation = 1;
    st.stop = false;
    st.pending.clear();
    st.completed.clear();

    st.worker = jce_thread_create(worker_main, NULL, thread_name);
    if (!st.worker) {
        jce_cond_destroy(st.cv);
        jce_mutex_destroy(st.mutex);
        st.cv = NULL;
        st.mutex = NULL;
        st.running = false;
        return false;
    }
    st.running = true;
    return true;
}

/* Stops accepting queued work, drops requests that have not started, and
 * joins the current decode before clearing completed results. This bounds
 * project-switch and editor-shutdown latency without abandoning a worker
 * that can still publish into the state. `on_discard` releases payloads
 * that nobody finalized. */
template <typename State, typename DiscardFn>
void async_loader_stop(State &st, DiscardFn on_discard)
{
    {
        JceMutexGuard lock(st.mutex);
        st.stop = true;
        st.pending.clear();
    }
    jce_cond_broadcast(st.cv);

    if (st.worker) {
        jce_thread_join(st.worker);
        st.worker = NULL;
    }

    for (auto &res : st.completed)
        on_discard(res);

    st.completed.clear();
    st.running = false;

    if (st.cv)    { jce_cond_destroy(st.cv);    st.cv = NULL; }
    if (st.mutex) { jce_mutex_destroy(st.mutex); st.mutex = NULL; }
}

template <typename State>
void async_loader_stop(State &st)
{
    async_loader_stop(st, [](auto &) {});
}

/* Invalidates every queued request.  `drop_completed` also throws away
 * results the worker already published — only correct when the result
 * type owns its payload; a result holding raw CPU data must instead reach
 * the drain, which releases it on the generation check. */
template <typename State>
void async_loader_begin_new_generation(State &st, bool drop_completed)
{
    JceMutexGuard lock(st.mutex);
    st.generation++;
    st.next_order = 0;
    st.pending.clear();
    if (drop_completed)
        st.completed.clear();
}

template <typename State>
uint64_t async_loader_generation(State &st)
{
    JceMutexGuard lock(st.mutex);
    return st.generation;
}

/* Queues a request unless an equal one is already pending for the current
 * generation, then wakes the worker.  `same(existing)` runs with the mutex
 * held and may fold the new request into the existing one (the mesh queue
 * tightens its distance priority that way); `make(generation, order)`
 * builds the request when nothing matched.  Both paths signal, exactly as
 * the hand-rolled queues did. */
template <typename State, typename SameFn, typename MakeFn>
void async_loader_queue_request(State &st, SameFn same, MakeFn make)
{
    {
        JceMutexGuard lock(st.mutex);

        bool merged = false;
        for (auto &req : st.pending) {
            if (req.generation == st.generation && same(req)) {
                merged = true;
                break;
            }
        }

        if (!merged)
            st.pending.push_back(make(st.generation, st.next_order++));
    }

    jce_cond_signal(st.cv);
}

/* Worker side: blocks until a request can be taken or the loader is
 * stopping.  `pop` runs with the mutex held and returns false when it
 * declined to take anything.  Returns false only once the loader is
 * stopping and the queue has drained — i.e. the worker should exit. */
template <typename State, typename PopFn>
bool async_loader_worker_take(State &st, PopFn pop)
{
    for (;;) {
        jce_mutex_lock(st.mutex);
        while (!(st.stop || !st.pending.empty()))
            jce_cond_wait(st.cv, st.mutex);

        if (st.stop && st.pending.empty()) {
            jce_mutex_unlock(st.mutex);
            return false;
        }

        const bool got = pop();
        jce_mutex_unlock(st.mutex);
        if (got)
            return true;
    }
}

/* Worker side: publish one finished result (moved from `result`). */
template <typename State, typename Result>
void async_loader_publish(State &st, Result &result)
{
    JceMutexGuard lock(st.mutex);
    st.completed.push_back(std::move(result));
}

/* Main thread: steal the whole completed list in one lock. */
template <typename State, typename Result>
void async_loader_take_completed(State &st, std::vector<Result> *out)
{
    if (!out) return;

    JceMutexGuard lock(st.mutex);
    out->swap(st.completed);
}

/* Main thread: hand back the results the per-frame budget deferred. */
template <typename State, typename Result>
void async_loader_push_back_completed(State &st, std::vector<Result> *results)
{
    if (!results || results->empty()) return;

    JceMutexGuard lock(st.mutex);
    for (Result &res : *results)
        st.completed.push_back(std::move(res));
    results->clear();
}

/* Main thread: take everything the worker published and finalize at most
 * `budget` of it, pushing the rest back for the next frame.
 *
 * `finalize(result, generation, budget_left)` runs once per result in
 * arrival order and reports what it did.  The generation is read once,
 * after the batch is taken, so an idle frame still costs a single lock. */
template <typename State, typename FinalizeFn>
void async_loader_drain_completed(State &st, uint32_t budget,
                                  FinalizeFn finalize)
{
    std::vector<typename State::ResultType> completed;
    async_loader_take_completed(st, &completed);
    if (completed.empty())
        return;

    const uint64_t generation = async_loader_generation(st);
    uint32_t finalized = 0;
    std::vector<typename State::ResultType> deferred;
    deferred.reserve(completed.size());

    for (typename State::ResultType &res : completed) {
        const AsyncFinalizeAction action =
            finalize(res, generation, finalized < budget);
        if (action == ASYNC_FINALIZE_DEFERRED)
            deferred.push_back(std::move(res));
        else if (action == ASYNC_FINALIZE_APPLIED)
            finalized++;
    }

    async_loader_push_back_completed(st, &deferred);
}

/* ── Mesh async types + state ───────────────────────────────────── */

struct MeshLoadRequest {
    std::string mesh_path;
    std::string file_path;
    float       priority_dist2;
    uint32_t    order;
    uint64_t    generation;
};

struct MeshLoadResult {
    std::string         mesh_path;
    uint64_t            generation;
    bool                success;
    JceEditorCpuMeshData cpu;
};

typedef AsyncLoaderState<MeshLoadRequest, MeshLoadResult> MeshAsyncState;

extern MeshAsyncState s_mesh_async;

/* ── Texture async types + state ────────────────────────────────── */

struct TextureLoadRequest {
    std::string key;
    std::string file_path;
    std::string material_path;
    std::string mesh_path;
    bool        resolve_path;
    uint64_t    generation;
};

struct TextureLoadResult {
    std::string          key;
    uint64_t             generation;
    bool                 success;
    std::vector<uint8_t> rgba;
    uint32_t             width;
    uint32_t             height;
};

typedef AsyncLoaderState<TextureLoadRequest, TextureLoadResult> TextureAsyncState;

extern TextureAsyncState s_tex_async;

/* ── Material async types + state (structured executor) ─────────── */

/* Not an AsyncLoaderState: material extraction owns no thread and no
 * condvar, submits independent jobs to a bounded JceAsyncExecutor instead
 * of a pending queue, has no per-frame finalize budget, and its results are
 * pulled by the caller rather than applied into a cache. */

struct MaterialAsyncContext {
    uint32_t              entity_id;
    char                  mesh_path[256];
    char                  file_path[512];
    uint64_t              generation;
    bool                  success;
    JceEditorMaterialInfo material;
};

struct MaterialInFlightTask {
    JceAsyncTask         *task;
    MaterialAsyncContext *context;
};

struct MaterialAsyncState {
    JceAsyncExecutor                           *executor;
    JceMutex                                   *mutex;
    std::vector<MaterialInFlightTask>           inflight;
    std::vector<JceEditorMaterialExtractResult> completed;
    uint64_t                                    generation;
    bool                                        running;
};

extern MaterialAsyncState s_mat_async;

/* ── Budget constants ───────────────────────────────────────────── */

#define MESH_FINALIZE_BUDGET_PER_FRAME 2
#define TEX_FINALIZE_BUDGET_PER_FRAME  4

/* ── Inline helpers ─────────────────────────────────────────────── */

static inline JceTexture tex_invalid(void)
{
    JceTexture tex;
    tex.idx = UINT16_MAX;
    return tex;
}

static inline JceAssetHandle asset_handle_invalid(void)
{
    JceAssetHandle handle;
    handle.index = 0xFFFF;
    handle.generation = 0;
    return handle;
}

static inline bool asset_handle_valid(JceAssetHandle handle)
{
    return handle.index != 0xFFFF;
}

static inline JceAssetLoadParams asset_load_params_default(void)
{
    JceAssetLoadParams params = {};
    return params;
}

/* ── Functions from jce_asset_cache_mesh.cpp ─────────────────────── */

void mesh_async_start(void);
void mesh_async_stop(void);
void mesh_async_begin_new_generation(void);
uint64_t mesh_async_current_generation(void);
void mesh_async_queue_request(const char *mesh_path,
                              const char *file_path,
                              float priority_dist2);

int  find_mesh_cache_entry(const char *mesh_path);
void reset_mesh_cache_entry(MeshCacheEntry *entry);
void clear_mesh_cache(void);

bool resolve_mesh_file_path(const char *mesh_path, char *out_path,
                            size_t out_size);

void     mesh_finalize_completed_loads(void);
JceMesh *asset_cache_get_mesh(const char *mesh_path, const float *world_pos);

/* ── Functions from jce_asset_cache_texture.cpp ──────────────────── */

bool looks_like_texture_asset_path(const char *path);
int  find_texture_cache_entry(const char *key);
void reset_texture_cache_entry(TextureCacheEntry *entry);
void clear_texture_cache(void);

void texture_async_start(void);
void texture_async_stop(void);
void texture_async_begin_new_generation(void);
uint64_t texture_async_current_generation(void);
void texture_async_queue_request(const char *key, const char *file_path);
void texture_async_queue_resolve_request(const char *key,
                                         const char *material_path,
                                         const char *mesh_path);

bool decode_texture_rgba_path(const char *path,
                              std::vector<uint8_t> *out_rgba,
                              uint32_t *out_w,
                              uint32_t *out_h);

void       texture_finalize_completed_loads(void);
JceTexture asset_cache_get_texture(const char *material_path,
                                   const char *mesh_path);

/* ── Functions from jce_asset_cache_material.cpp ─────────────────── */

void     material_async_start(void);
void     material_async_stop(void);
void     material_async_begin_new_generation(void);
uint64_t material_async_current_generation(void);
void     material_async_queue_request(uint32_t entity_id,
                                      const char *mesh_path,
                                      const char *file_path);

void material_finalize_completed_loads(void);
bool material_take_completed_result(JceEditorMaterialExtractResult *out_result);

/* ── Functions from jce_asset_cache_resolve.cpp ──────────────────── */

std::string lower_copy(const std::string &s);
std::string trim_copy(const std::string &s);
bool        path_is_file(const char *path);
/* Returns the cached scene-root search list (rebuilt only when the scene
 * dir / assetdb root / project root change) — read-only; do not retain
 * the reference across a scene switch. */
const std::vector<std::string> &collect_scene_roots(void);
bool        find_file_by_name_recursive(const std::vector<std::string> &roots,
                                        const std::string &file_name,
                                        int max_depth,
                                        char *out, size_t out_size);

bool resolve_texture_path_for_material(const char *material_path,
                                       const char *mesh_path,
                                       char *out_path, size_t out_size);

bool resolve_material_file_path(const char *material_path,
                                char *out_mat, size_t out_size);

#endif /* JCE_ASSET_CACHE_INTERNAL_H */
