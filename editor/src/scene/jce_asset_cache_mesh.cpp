/*
 * jce_asset_cache_mesh.cpp  Mesh cache + async mesh loading.
 */

#include "jce_asset_cache_internal.h"
#include "ui/jce_editor_panels.h"

/* ── Mesh request priority ──────────────────────────────────────── */

static float mesh_request_priority(const float *world_pos)
{
    if (!world_pos)
        return std::numeric_limits<float>::max();
    return world_pos[0] * world_pos[0]
         + world_pos[1] * world_pos[1]
         + world_pos[2] * world_pos[2];
}

static bool mesh_request_is_higher_priority(const MeshLoadRequest &a,
                                            const MeshLoadRequest &b)
{
    if (a.priority_dist2 < b.priority_dist2) return true;
    if (a.priority_dist2 > b.priority_dist2) return false;
    return a.order < b.order;
}

static bool mesh_pop_best_request_locked(MeshLoadRequest *out)
{
    if (!out || s_mesh_async.pending.empty())
        return false;

    size_t best = 0;
    for (size_t i = 1; i < s_mesh_async.pending.size(); i++) {
        if (mesh_request_is_higher_priority(s_mesh_async.pending[i],
                                            s_mesh_async.pending[best])) {
            best = i;
        }
    }

    *out = std::move(s_mesh_async.pending[best]);
    s_mesh_async.pending.erase(
        s_mesh_async.pending.begin()
        + static_cast<std::vector<MeshLoadRequest>::difference_type>(best));
    return true;
}

/* ── Mesh async worker ──────────────────────────────────────────── */

static void mesh_async_worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        MeshLoadRequest req;
        if (!async_loader_worker_take(
                s_mesh_async,
                [&req] { return mesh_pop_best_request_locked(&req); })) {
            break;
        }

        MeshLoadResult result = {};
        result.mesh_path = req.mesh_path;
        result.generation = req.generation;

        if (req.file_path.empty()) {
            char found_path[512];
            if (!resolve_mesh_file_path(req.mesh_path.c_str(),
                                        found_path,
                                        sizeof(found_path))) {
                result.success = false;
                async_loader_publish(s_mesh_async, result);
                continue;
            }
            req.file_path = found_path;
        }

        result.success = jce_editor_model_load_cpu_file(req.file_path.c_str(),
                                                        &result.cpu);
        async_loader_publish(s_mesh_async, result);
    }
}

/* ── Mesh async lifecycle ───────────────────────────────────────── */

void mesh_async_start(void)
{
    if (s_mesh_async.running)
        return;

    if (!async_loader_start(s_mesh_async, mesh_async_worker_main,
                            "scene_mesh_async"))
        LOG_ERROR(LOG_TAG, "failed to start mesh decode service");
}

void mesh_async_stop(void)
{
    if (!s_mesh_async.running)
        return;

    async_loader_stop(s_mesh_async, [](MeshLoadResult &res) {
        jce_editor_model_free_cpu_data(&res.cpu);
    });
}

void mesh_async_begin_new_generation(void)
{
    /* Results the worker already published stay queued: they hold raw CPU
     * mesh data that only the drain releases, and it drops them anyway on
     * the generation check. */
    async_loader_begin_new_generation(s_mesh_async, false);
}

uint64_t mesh_async_current_generation(void)
{
    return async_loader_generation(s_mesh_async);
}

void mesh_async_queue_request(const char *mesh_path,
                              const char *file_path,
                              float priority_dist2)
{
    if (!s_mesh_async.running || !mesh_path)
        return;

    async_loader_queue_request(
        s_mesh_async,
        /* A pending request for the same mesh absorbs this one, keeping the
         * nearest distance so the worker picks it up sooner. */
        [&](MeshLoadRequest &req) {
            if (req.mesh_path != mesh_path)
                return false;
            if (priority_dist2 < req.priority_dist2)
                req.priority_dist2 = priority_dist2;
            return true;
        },
        [&](uint64_t generation, uint32_t order) {
            MeshLoadRequest req;
            req.mesh_path = mesh_path;
            req.file_path = file_path ? file_path : "";
            req.priority_dist2 = priority_dist2;
            req.order = order;
            req.generation = generation;
            return req;
        });
}

/* ── Mesh cache entry management ────────────────────────────────── */

int find_mesh_cache_entry(const char *mesh_path)
{
    if (!mesh_path || mesh_path[0] == '\0') return -1;

    for (int i = 0; i < s_cache.mesh_cache_count; i++) {
        if (strcmp(s_cache.mesh_cache[i].path, mesh_path) == 0)
            return i;
    }
    return -1;
}

void reset_mesh_cache_entry(MeshCacheEntry *entry)
{
    if (!entry)
        return;

    if (entry->mesh)
        jce_mesh_destroy(entry->mesh);

    entry->path[0] = '\0';
    entry->mesh = NULL;
    entry->requested = false;
    entry->failed = false;
    entry->request_generation = 0;
}

void clear_mesh_cache(void)
{
    for (int i = 0; i < s_cache.mesh_cache_count; i++)
        reset_mesh_cache_entry(&s_cache.mesh_cache[i]);
    s_cache.mesh_cache_count = 0;
}

/* ── Mesh file path resolution ──────────────────────────────────── */

static bool copy_found_path(const char *src, char *out_path, size_t out_size)
{
    if (!src || !out_path || out_size == 0) return false;
    snprintf(out_path, out_size, "%s", src);
    return true;
}

bool resolve_mesh_file_path(const char *mesh_path, char *out_path,
                            size_t out_size)
{
    if (!mesh_path || mesh_path[0] == '\0' || !out_path || out_size == 0)
        return false;

    /* VFS-first: if an active bundle/PAK is mounted (editor scene preview
     * from a .jbundle) and contains this exact virtual path, return it
     * unchanged.  The downstream Assimp importer is bundle-aware and will
     * route the actual read through jce_fs_host_read_all → active VFS. */
    {
        JceFileSystem *afs = jce_fs_get_active();
        if (afs) {
            if (jce_fs_exists(afs, mesh_path))
                return copy_found_path(mesh_path, out_path, out_size);
            if (jce_fs_get_active_policy() == JCE_FS_ACTIVE_ISOLATED)
                return false;
        }
    }

    /* If the path already points to an existing file (e.g. absolute), use it. */
    if (jce_fs_host_exists_file(mesh_path))
        return copy_found_path(mesh_path, out_path, out_size);

    char basename[256];
    if (!jce_path_basename(basename, sizeof(basename), mesh_path)) {
        snprintf(basename, sizeof(basename), "%s", mesh_path);
    }

    /* Try joining with scene_dir if known. */
    if (s_cache.scene_dir[0] != '\0') {
        char full_path[512];
        if (jce_path_join(full_path, sizeof(full_path), s_cache.scene_dir, mesh_path)) {
            if (jce_fs_host_exists_file(full_path))
                return copy_found_path(full_path, out_path, out_size);
        }
    }

    /* Recursive search across all scene + project roots so meshes resolve
     * Unity-style by basename anywhere under the project. */
    {
        const std::vector<std::string> &roots = collect_scene_roots();
        if (roots.empty())
            return false;

        char found[512];
        if (find_file_by_name_recursive(roots, std::string(basename), 8, found, sizeof(found))) {
            return copy_found_path(found, out_path, out_size);
        }
    }

    return false;
}

/* ── Mesh finalization + cache lookup ───────────────────────────── */

static AsyncFinalizeAction mesh_finalize_result(MeshLoadResult &res,
                                                uint64_t generation,
                                                bool budget_left)
{
    if (res.generation != generation) {
        jce_editor_model_free_cpu_data(&res.cpu);
        return ASYNC_FINALIZE_DROPPED;
    }

    int idx = find_mesh_cache_entry(res.mesh_path.c_str());
    if (idx < 0) {
        /* The cache key was lost (e.g. path buffer was too small at
         * queue time, or the cache was cleared mid-flight). Log so a
         * future regression surfaces immediately rather than as a
         * silent fallback to the procedural cube shape. */
        LOG_WARN(LOG_TAG,
            "mesh finalize: no cache entry for completed load: %s",
            res.mesh_path.c_str());
        jce_editor_console_log_level(JCE_CONSOLE_WARNING,
            "Mesh load completed but cache entry was lost: %s",
            res.mesh_path.c_str());
        jce_editor_model_free_cpu_data(&res.cpu);
        return ASYNC_FINALIZE_DROPPED;
    }

    if (!budget_left)
        return ASYNC_FINALIZE_DEFERRED;

    s_cache.mesh_cache[idx].requested = false;

    if (!res.success) {
        s_cache.mesh_cache[idx].failed = true;
        LOG_WARN(LOG_TAG, "mesh async load failed: %s",
                 res.mesh_path.c_str());
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Mesh import failed: %s (Assimp could not parse the file or path resolution failed)",
            res.mesh_path.c_str());
        jce_editor_model_free_cpu_data(&res.cpu);
        return ASYNC_FINALIZE_DROPPED;
    }

    JceMesh *mesh = jce_mesh_create(res.cpu.vertices,
                                    res.cpu.vertex_count,
                                    res.cpu.indices,
                                    res.cpu.index_count);
    jce_editor_model_free_cpu_data(&res.cpu);

    if (!mesh) {
        s_cache.mesh_cache[idx].failed = true;
        LOG_WARN(LOG_TAG, "mesh finalize failed: %s", res.mesh_path.c_str());
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Mesh GPU upload failed: %s (vertex/index buffer creation rejected)",
            res.mesh_path.c_str());
        return ASYNC_FINALIZE_DROPPED;
    }

    s_cache.mesh_cache[idx].mesh = mesh;
    return ASYNC_FINALIZE_APPLIED;
}

void mesh_finalize_completed_loads(void)
{
    async_loader_drain_completed(s_mesh_async, MESH_FINALIZE_BUDGET_PER_FRAME,
                                 mesh_finalize_result);
}

JceMesh *asset_cache_get_mesh(const char *mesh_path, const float *world_pos)
{
    if (!mesh_path || mesh_path[0] == '\0')
        return NULL;

    int idx = find_mesh_cache_entry(mesh_path);
    if (idx < 0) {
        if (s_cache.mesh_cache_count >= 512)
            return NULL;

        idx = s_cache.mesh_cache_count++;
        reset_mesh_cache_entry(&s_cache.mesh_cache[idx]);
        snprintf(s_cache.mesh_cache[idx].path,
                 sizeof(s_cache.mesh_cache[idx].path), "%s", mesh_path);
    }

    if (s_cache.mesh_cache[idx].mesh)
        return s_cache.mesh_cache[idx].mesh;
    if (s_cache.mesh_cache[idx].failed)
        return NULL;

    uint64_t generation = mesh_async_current_generation();
    if (s_cache.mesh_cache[idx].requested
        && s_cache.mesh_cache[idx].request_generation == generation) {
        return NULL;
    }

    s_cache.mesh_cache[idx].requested = true;
    s_cache.mesh_cache[idx].request_generation = generation;
    mesh_async_queue_request(mesh_path, NULL,
                             mesh_request_priority(world_pos));
    return NULL;
}
