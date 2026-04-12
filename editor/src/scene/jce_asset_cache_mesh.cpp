/*
 * jce_asset_cache_mesh.cpp  Mesh cache + async mesh loading.
 */

#include "jce_asset_cache_internal.h"

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

static void mesh_async_worker_main(void)
{
    for (;;) {
        MeshLoadRequest req;
        {
            std::unique_lock<std::mutex> lock(s_mesh_async.mutex);
            s_mesh_async.cv.wait(lock, [] {
                return s_mesh_async.stop || !s_mesh_async.pending.empty();
            });

            if (s_mesh_async.stop && s_mesh_async.pending.empty())
                break;

            if (!mesh_pop_best_request_locked(&req))
                continue;
        }

        MeshLoadResult result = {};
        result.mesh_path = req.mesh_path;
        result.generation = req.generation;
        result.success = jce_editor_model_load_cpu_file(req.file_path.c_str(),
                                                        &result.cpu);

        std::lock_guard<std::mutex> lock(s_mesh_async.mutex);
        s_mesh_async.completed.push_back(std::move(result));
    }
}

/* ── Mesh async lifecycle ───────────────────────────────────────── */

void mesh_async_start(void)
{
    if (s_mesh_async.running)
        return;

    s_mesh_async.discovery = 0;
    s_mesh_async.generation = 1;
    s_mesh_async.stop = false;
    s_mesh_async.pending.clear();
    s_mesh_async.completed.clear();

    s_mesh_async.worker = std::thread(mesh_async_worker_main);
    s_mesh_async.running = true;
}

void mesh_async_stop(void)
{
    if (!s_mesh_async.running)
        return;

    {
        std::lock_guard<std::mutex> lock(s_mesh_async.mutex);
        s_mesh_async.stop = true;
    }
    s_mesh_async.cv.notify_all();

    if (s_mesh_async.worker.joinable())
        s_mesh_async.worker.join();

    for (auto &res : s_mesh_async.completed)
        jce_editor_model_free_cpu_data(&res.cpu);

    s_mesh_async.pending.clear();
    s_mesh_async.completed.clear();
    s_mesh_async.running = false;
}

void mesh_async_begin_new_generation(void)
{
    std::lock_guard<std::mutex> lock(s_mesh_async.mutex);
    s_mesh_async.generation++;
    s_mesh_async.discovery = 0;
    s_mesh_async.pending.clear();
}

uint64_t mesh_async_current_generation(void)
{
    std::lock_guard<std::mutex> lock(s_mesh_async.mutex);
    return s_mesh_async.generation;
}

void mesh_async_queue_request(const char *mesh_path,
                              const char *file_path,
                              float priority_dist2)
{
    if (!s_mesh_async.running || !mesh_path || !file_path)
        return;

    bool inserted = false;
    {
        std::lock_guard<std::mutex> lock(s_mesh_async.mutex);
        for (MeshLoadRequest &req : s_mesh_async.pending) {
            if (req.generation == s_mesh_async.generation
                && req.mesh_path == mesh_path) {
                if (priority_dist2 < req.priority_dist2)
                    req.priority_dist2 = priority_dist2;
                inserted = true;
                break;
            }
        }

        if (!inserted) {
            MeshLoadRequest req;
            req.mesh_path = mesh_path;
            req.file_path = file_path;
            req.priority_dist2 = priority_dist2;
            req.order = s_mesh_async.discovery++;
            req.generation = s_mesh_async.generation;
            s_mesh_async.pending.push_back(std::move(req));
            inserted = true;
        }
    }

    if (inserted)
        s_mesh_async.cv.notify_one();
}

static void mesh_async_take_completed(std::vector<MeshLoadResult> *out)
{
    if (!out) return;

    std::lock_guard<std::mutex> lock(s_mesh_async.mutex);
    out->swap(s_mesh_async.completed);
}

static void mesh_async_push_back_completed(std::vector<MeshLoadResult> *results)
{
    if (!results || results->empty()) return;

    std::lock_guard<std::mutex> lock(s_mesh_async.mutex);
    for (MeshLoadResult &res : *results)
        s_mesh_async.completed.push_back(std::move(res));
    results->clear();
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

/* Convert "SM_PascalCase_Name" or "SKM_PascalCase_Name" to "pascalcase-name". */
static void sm_to_kebab(const char *sm_name, char *out, int out_size)
{
    int o = 0;
    const char *src = sm_name;
    if (src[0] == 'S' && src[1] == 'K' && src[2] == 'M' && src[3] == '_') src += 4;
    else if (src[0] == 'S' && src[1] == 'K' && src[2] == '_') src += 3;
    else if (src[0] == 'S' && src[1] == 'M' && src[2] == '_') src += 3;

    for (int i = 0; src[i] && o < out_size - 1; i++) {
        char c = src[i];
        if (c == '_') {
            out[o++] = '-';
        } else if (c >= 'A' && c <= 'Z') {
            out[o++] = (char)(c + 32);
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

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
    if (s_cache.scene_dir[0] == '\0')
        return false;

    if (fs::exists(mesh_path))
        return copy_found_path(mesh_path, out_path, out_size);

    const char *slash = strrchr(mesh_path, '/');
    if (!slash) slash = strrchr(mesh_path, '\\');
    const char *base = slash ? slash + 1 : mesh_path;

    char target_kebab[256];
    snprintf(target_kebab, sizeof(target_kebab), "%s", base);
    char *dot = strrchr(target_kebab, '.');
    if (dot) *dot = '\0';
    for (char *p = target_kebab; *p; p++) {
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p + 32);
    }

    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s/%s", s_cache.scene_dir, mesh_path);
    for (char *p = full_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }

    if (fs::exists(full_path))
        return copy_found_path(full_path, out_path, out_size);

    char meshes_dirs[4][512];
    int meshes_dir_count = 0;
    snprintf(meshes_dirs[meshes_dir_count++], 512, "%s\\Meshes", s_cache.scene_dir);
    {
        char parent[512];
        snprintf(parent, sizeof(parent), "%s", s_cache.scene_dir);
        char *sep = strrchr(parent, '\\');
        if (!sep) sep = strrchr(parent, '/');
        if (sep) {
            *sep = '\0';
            snprintf(meshes_dirs[meshes_dir_count++], 512, "%s\\Meshes", parent);
        }
    }
    snprintf(meshes_dirs[meshes_dir_count++], 512, "%s", s_cache.scene_dir);

    static const char *mesh_exts[] = { "*.obj", "*.fbx", "*.gltf", "*.glb", NULL };

#ifdef _WIN32
    char subdir_paths[64][512];
    int subdir_count = 0;

    for (int md = 0; md < meshes_dir_count; md++) {
        WIN32_FIND_DATAA dir_fd;
        char dir_pattern[512];
        snprintf(dir_pattern, sizeof(dir_pattern), "%s\\*", meshes_dirs[md]);
        HANDLE h_dir = FindFirstFileA(dir_pattern, &dir_fd);
        if (h_dir == INVALID_HANDLE_VALUE) continue;

        if (subdir_count < 63)
            snprintf(subdir_paths[subdir_count++], 512, "%s", meshes_dirs[md]);

        do {
            if (!(dir_fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (dir_fd.cFileName[0] == '.') continue;
            if (subdir_count >= 63) break;
            char l1[512];
            snprintf(l1, 512, "%s\\%s", meshes_dirs[md], dir_fd.cFileName);
            snprintf(subdir_paths[subdir_count++], 512, "%s", l1);

            WIN32_FIND_DATAA l2_fd;
            char l2_pat[512];
            snprintf(l2_pat, sizeof(l2_pat), "%s\\*", l1);
            HANDLE h2 = FindFirstFileA(l2_pat, &l2_fd);
            if (h2 != INVALID_HANDLE_VALUE) {
                do {
                    if (!(l2_fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                    if (l2_fd.cFileName[0] == '.') continue;
                    if (subdir_count >= 63) break;
                    char l2[512];
                    snprintf(l2, 512, "%s\\%s", l1, l2_fd.cFileName);
                    snprintf(subdir_paths[subdir_count++], 512, "%s", l2);

                    WIN32_FIND_DATAA l3_fd;
                    char l3_pat[512];
                    snprintf(l3_pat, sizeof(l3_pat), "%s\\*", l2);
                    HANDLE h3 = FindFirstFileA(l3_pat, &l3_fd);
                    if (h3 != INVALID_HANDLE_VALUE) {
                        do {
                            if (!(l3_fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                            if (l3_fd.cFileName[0] == '.') continue;
                            if (subdir_count >= 63) break;
                            snprintf(subdir_paths[subdir_count++], 512,
                                     "%s\\%s", l2, l3_fd.cFileName);
                        } while (FindNextFileA(h3, &l3_fd));
                        FindClose(h3);
                    }
                } while (FindNextFileA(h2, &l2_fd));
                FindClose(h2);
            }
        } while (FindNextFileA(h_dir, &dir_fd));
        FindClose(h_dir);
    }

    if (subdir_count == 0) {
        LOG_WARN(LOG_TAG, "mesh not found: %s (no Meshes dirs found under %s)",
                 mesh_path, s_cache.scene_dir);
        return false;
    }

    int target_len = (int)strlen(target_kebab);

    for (int d = 0; d < subdir_count; d++) {
        for (int ei = 0; mesh_exts[ei]; ei++) {
            WIN32_FIND_DATAA fd;
            char search_pattern[512];
            snprintf(search_pattern, sizeof(search_pattern), "%s\\%s",
                     subdir_paths[d], mesh_exts[ei]);
            HANDLE h_find = FindFirstFileA(search_pattern, &fd);
            if (h_find == INVALID_HANDLE_VALUE) continue;
            do {
                char base_name[256];
                snprintf(base_name, sizeof(base_name), "%s", fd.cFileName);
                char *ext = strrchr(base_name, '.');
                if (ext) *ext = '\0';

                {
                    char file_lower[256];
                    snprintf(file_lower, sizeof(file_lower), "%s", base_name);
                    for (char *p = file_lower; *p; p++) {
                        if (*p >= 'A' && *p <= 'Z')
                            *p = (char)(*p + 32);
                    }
                    if (strcmp(file_lower, target_kebab) == 0) {
                        char found_path[512];
                        snprintf(found_path, sizeof(found_path), "%s\\%s",
                                 subdir_paths[d], fd.cFileName);
                        FindClose(h_find);
                        return copy_found_path(found_path, out_path, out_size);
                    }
                }

                char file_kebab[256];
                sm_to_kebab(base_name, file_kebab, sizeof(file_kebab));

                if (strcmp(file_kebab, target_kebab) == 0) {
                    char found_path[512];
                    snprintf(found_path, sizeof(found_path), "%s\\%s",
                             subdir_paths[d], fd.cFileName);
                    FindClose(h_find);
                    return copy_found_path(found_path, out_path, out_size);
                }

                int fk_len = (int)strlen(file_kebab);
                if (fk_len > target_len && target_len > 0) {
                    const char *suffix = file_kebab + (fk_len - target_len);
                    if (strcmp(suffix, target_kebab) == 0 && suffix[-1] == '-') {
                        char found_path[512];
                        snprintf(found_path, sizeof(found_path), "%s\\%s",
                                 subdir_paths[d], fd.cFileName);
                        FindClose(h_find);
                        return copy_found_path(found_path, out_path, out_size);
                    }
                }

                if (target_len > 2 && fk_len > target_len) {
                    if (strstr(file_kebab, target_kebab) != NULL) {
                        char found_path[512];
                        snprintf(found_path, sizeof(found_path), "%s\\%s",
                                 subdir_paths[d], fd.cFileName);
                        FindClose(h_find);
                        return copy_found_path(found_path, out_path, out_size);
                    }
                }
            } while (FindNextFileA(h_find, &fd));
            FindClose(h_find);
        }
    }

    {
        std::error_code ec;
        std::string target_name = base;
        std::transform(target_name.begin(), target_name.end(), target_name.begin(),
                       [](unsigned char c) { return (char)tolower(c); });

        std::vector<fs::path> roots;
        roots.push_back(fs::path(s_cache.scene_dir));
        fs::path parent = fs::path(s_cache.scene_dir).parent_path();
        if (!parent.empty()) roots.push_back(parent);

        for (const fs::path &root : roots) {
            if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;
            fs::recursive_directory_iterator it(root,
                fs::directory_options::skip_permission_denied, ec);
            fs::recursive_directory_iterator end;
            for (; it != end; it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                if (it.depth() > 8) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(ec)) continue;
                std::string fn = it->path().filename().string();
                std::transform(fn.begin(), fn.end(), fn.begin(),
                               [](unsigned char c) { return (char)tolower(c); });
                if (fn == target_name) {
                    return copy_found_path(it->path().string().c_str(),
                                           out_path, out_size);
                }
            }
        }
    }
#endif

    return false;
}

/* ── Mesh finalization + cache lookup ───────────────────────────── */

void mesh_finalize_completed_loads(void)
{
    std::vector<MeshLoadResult> completed;
    mesh_async_take_completed(&completed);
    if (completed.empty())
        return;

    const uint64_t generation = mesh_async_current_generation();
    uint32_t finalized = 0;
    std::vector<MeshLoadResult> deferred;
    deferred.reserve(completed.size());

    for (MeshLoadResult &res : completed) {
        if (res.generation != generation) {
            jce_editor_model_free_cpu_data(&res.cpu);
            continue;
        }

        int idx = find_mesh_cache_entry(res.mesh_path.c_str());
        if (idx < 0) {
            jce_editor_model_free_cpu_data(&res.cpu);
            continue;
        }

        if (finalized >= MESH_FINALIZE_BUDGET_PER_FRAME) {
            deferred.push_back(std::move(res));
            continue;
        }

        s_cache.mesh_cache[idx].requested = false;

        if (!res.success) {
            s_cache.mesh_cache[idx].failed = true;
            jce_editor_model_free_cpu_data(&res.cpu);
            continue;
        }

        JceMesh *mesh = jce_mesh_create(res.cpu.vertices,
                                        res.cpu.vertex_count,
                                        res.cpu.indices,
                                        res.cpu.index_count);
        jce_editor_model_free_cpu_data(&res.cpu);

        if (!mesh) {
            s_cache.mesh_cache[idx].failed = true;
            LOG_WARN(LOG_TAG, "mesh finalize failed: %s", res.mesh_path.c_str());
            continue;
        }

        s_cache.mesh_cache[idx].mesh = mesh;
        finalized++;
    }

    if (!deferred.empty())
        mesh_async_push_back_completed(&deferred);
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

    char found_path[512];
    if (!resolve_mesh_file_path(mesh_path, found_path, sizeof(found_path))) {
        s_cache.mesh_cache[idx].failed = true;
        LOG_WARN(LOG_TAG, "mesh not found: %s (under %s)",
                 mesh_path, s_cache.scene_dir);
        return NULL;
    }

    s_cache.mesh_cache[idx].requested = true;
    s_cache.mesh_cache[idx].request_generation = generation;
    mesh_async_queue_request(mesh_path, found_path,
                             mesh_request_priority(world_pos));
    return NULL;
}
