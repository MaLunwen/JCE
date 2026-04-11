/*
 * jce_editor_scene_asset_cache.cpp  Scene asset loading/cache helpers.
 */

#include "jce_editor_scene_asset_cache.h"

#include "jce_model_loader_assimp.h"

#include <string.h>
#include <math.h>
#include <stdio.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

extern "C" {
#include <cjson/cJSON.h>
#include <jce/core/jce_log.h>
#include <jce/graphics/jce_mesh.h>
#include <jce/graphics/jce_texture.h>
#include <jce/resource/jce_asset.h>
}

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#define LOG_TAG "scene_asset_cache"

namespace fs = std::filesystem;

struct MeshCacheEntry {
    char     path[128];
    JceMesh *mesh;
    bool     requested;
    bool     failed;
    uint64_t request_generation;
};

struct TextureCacheEntry {
    char           path[128];
    JceTexture     tex;
    JceAssetHandle asset_handle;
    bool           tex_from_asset_manager;
    bool           warned_missing;
    bool           requested;
    bool           failed;
    uint64_t       request_generation;
};

static struct {
    bool              initialized;
    JceAssetManager  *assets;
    MeshCacheEntry    mesh_cache[512];
    int               mesh_cache_count;
    TextureCacheEntry tex_cache[256];
    int               tex_cache_count;
    char              scene_dir[512];
} s_cache;

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

struct MeshAsyncState {
    std::thread                 worker;
    std::mutex                  mutex;
    std::condition_variable     cv;
    std::vector<MeshLoadRequest> pending;
    std::vector<MeshLoadResult>  completed;
    uint32_t                    discovery;
    uint64_t                    generation;
    bool                        running;
    bool                        stop;
};

static MeshAsyncState s_mesh_async = {};

struct TextureLoadRequest {
    std::string key;
    std::string file_path;
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

struct TextureAsyncState {
    std::thread                    worker;
    std::mutex                     mutex;
    std::condition_variable        cv;
    std::vector<TextureLoadRequest> pending;
    std::vector<TextureLoadResult>  completed;
    uint64_t                       generation;
    bool                           running;
    bool                           stop;
};

static TextureAsyncState s_tex_async = {};

#define MESH_FINALIZE_BUDGET_PER_FRAME 2
#define TEX_FINALIZE_BUDGET_PER_FRAME  4

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

static void mesh_async_start(void)
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

static void mesh_async_stop(void)
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

static void mesh_async_begin_new_generation(void)
{
    std::lock_guard<std::mutex> lock(s_mesh_async.mutex);
    s_mesh_async.generation++;
    s_mesh_async.discovery = 0;
    s_mesh_async.pending.clear();
}

static uint64_t mesh_async_current_generation(void)
{
    std::lock_guard<std::mutex> lock(s_mesh_async.mutex);
    return s_mesh_async.generation;
}

static void mesh_async_queue_request(const char *mesh_path,
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

static int find_mesh_cache_entry(const char *mesh_path)
{
    if (!mesh_path || mesh_path[0] == '\0') return -1;

    for (int i = 0; i < s_cache.mesh_cache_count; i++) {
        if (strcmp(s_cache.mesh_cache[i].path, mesh_path) == 0)
            return i;
    }
    return -1;
}

static void reset_mesh_cache_entry(MeshCacheEntry *entry)
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

static void clear_mesh_cache(void)
{
    for (int i = 0; i < s_cache.mesh_cache_count; i++)
        reset_mesh_cache_entry(&s_cache.mesh_cache[i]);
    s_cache.mesh_cache_count = 0;
}

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

static bool resolve_mesh_file_path(const char *mesh_path, char *out_path,
                                   size_t out_size)
{
    if (!mesh_path || mesh_path[0] == '\0' || !out_path || out_size == 0)
        return false;
    if (s_cache.scene_dir[0] == '\0')
        return false;

    {
        FILE *test_abs = fopen(mesh_path, "rb");
        if (test_abs) {
            fclose(test_abs);
            return copy_found_path(mesh_path, out_path, out_size);
        }
    }

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

    {
        FILE *test = fopen(full_path, "rb");
        if (test) {
            fclose(test);
            return copy_found_path(full_path, out_path, out_size);
        }
    }

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

static void mesh_finalize_completed_loads(void)
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

static JceMesh *get_cached_mesh(const char *mesh_path, const float *world_pos)
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

static std::string lower_copy(const std::string &s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return out;
}

static std::string trim_copy(const std::string &s)
{
    size_t begin = 0;
    while (begin < s.size()
           && (s[begin] == ' ' || s[begin] == '\t'
               || s[begin] == '\r' || s[begin] == '\n')) {
        begin++;
    }

    size_t end = s.size();
    while (end > begin
           && (s[end - 1] == ' ' || s[end - 1] == '\t'
               || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        end--;
    }

    return s.substr(begin, end - begin);
}

static bool path_is_file(const fs::path &path)
{
    std::error_code ec;
    return fs::exists(path, ec) && fs::is_regular_file(path, ec);
}

static std::vector<fs::path> collect_scene_roots(void)
{
    std::vector<fs::path> roots;
    if (s_cache.scene_dir[0] == '\0') return roots;

    fs::path scene(s_cache.scene_dir);
    roots.push_back(scene);

    fs::path parent = scene.parent_path();
    if (!parent.empty()) roots.push_back(parent);

    roots.push_back(scene / "Meshes");
    roots.push_back(scene / "Materials");
    roots.push_back(scene / "Textures");
    roots.push_back(scene / "materials");
    roots.push_back(scene / "textures");

    if (!parent.empty()) {
        roots.push_back(parent / "Meshes");
        roots.push_back(parent / "Materials");
        roots.push_back(parent / "Textures");
        roots.push_back(parent / "materials");
        roots.push_back(parent / "textures");
    }

    return roots;
}

static bool find_file_by_name_recursive(const std::vector<fs::path> &roots,
                                        const std::string &file_name,
                                        int max_depth,
                                        fs::path *out)
{
    if (!out || file_name.empty()) return false;

    const std::string target = lower_copy(file_name);
    std::error_code ec;
    for (const fs::path &root : roots) {
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;

        fs::recursive_directory_iterator it(root,
            fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;

        for (; it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (it.depth() > max_depth) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            if (lower_copy(it->path().filename().string()) == target) {
                *out = it->path();
                return true;
            }
        }
    }

    return false;
}

static bool try_resolve_texture_path(const fs::path &path, fs::path *out_path)
{
    if (!out_path || !path_is_file(path)) return false;

    *out_path = path;
    return true;
}

static bool decode_texture_rgba_path(const fs::path &path,
                                     std::vector<uint8_t> *out_rgba,
                                     uint32_t *out_w,
                                     uint32_t *out_h)
{
    if (!out_rgba || !out_w || !out_h || !path_is_file(path))
        return false;

    out_rgba->clear();
    *out_w = 0;
    *out_h = 0;

    SDL_IOStream *io = SDL_IOFromFile(path.string().c_str(), "rb");
    if (!io) return false;

    SDL_Surface *surf = IMG_Load_IO(io, true);
    if (!surf) return false;

    if (surf->format != SDL_PIXELFORMAT_RGBA32) {
        SDL_Surface *conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(surf);
        surf = conv;
    }
    if (!surf || surf->w <= 0 || surf->h <= 0 || !surf->pixels) {
        if (surf) SDL_DestroySurface(surf);
        return false;
    }

    *out_w = (uint32_t)surf->w;
    *out_h = (uint32_t)surf->h;

    const size_t row_bytes = (size_t)(*out_w) * 4;
    const size_t total_bytes = row_bytes * (size_t)(*out_h);
    out_rgba->resize(total_bytes);

    const uint8_t *src = (const uint8_t *)surf->pixels;
    uint8_t *dst = out_rgba->data();
    for (uint32_t y = 0; y < *out_h; y++) {
        memcpy(dst + (size_t)y * row_bytes,
               src + (size_t)y * (size_t)surf->pitch,
               row_bytes);
    }

    SDL_DestroySurface(surf);
    LOG_DEBUG(LOG_TAG, "decoded texture: %s (%ux%u)",
              path.string().c_str(), *out_w, *out_h);
    return true;
}

static std::string cjson_string(cJSON *obj, const char *key)
{
    if (!obj || !key) return std::string();
    cJSON *value = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(value) || !value->valuestring || value->valuestring[0] == '\0')
        return std::string();
    return std::string(value->valuestring);
}

static bool resolve_material_file_path(const char *material_path, fs::path *out_mat)
{
    if (!material_path || material_path[0] == '\0' || !out_mat) return false;

    std::vector<std::string> candidates;
    const std::string raw = material_path;
    candidates.push_back(raw);

    std::string lower = lower_copy(raw);
    if (lower.size() >= 9 && lower.substr(lower.size() - 9) == ".mat.json") {
        candidates.push_back(raw.substr(0, raw.size() - 5));
    } else if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".mat") {
        candidates.push_back(raw + ".json");
    } else if (lower.size() >= 9 && lower.substr(lower.size() - 9) == ".material") {
        candidates.push_back(raw.substr(0, raw.size() - 9) + ".mat.json");
    } else if (fs::path(raw).extension().empty()) {
        candidates.push_back(raw + ".mat");
        candidates.push_back(raw + ".mat.json");
    }

    std::vector<fs::path> roots = collect_scene_roots();
    for (const std::string &candidate : candidates) {
        fs::path path(candidate);
        if (path.is_absolute() && path_is_file(path)) {
            *out_mat = path;
            return true;
        }
        if (path_is_file(path)) {
            *out_mat = path;
            return true;
        }
        for (const fs::path &root : roots) {
            fs::path resolved = root / path;
            if (path_is_file(resolved)) {
                *out_mat = resolved;
                return true;
            }
        }

        fs::path by_name;
        if (find_file_by_name_recursive(roots, path.filename().string(), 8, &by_name)) {
            *out_mat = by_name;
            return true;
        }
    }

    return false;
}

static bool try_resolve_texture_from_material_json(const char *material_path,
                                                   fs::path *out_path)
{
    if (!out_path) return false;

    fs::path mat_file;
    if (!resolve_material_file_path(material_path, &mat_file))
        return false;

    std::ifstream in(mat_file, std::ios::binary);
    if (!in.good()) return false;
    std::string json((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (json.empty()) return false;

    cJSON *root = cJSON_Parse(json.c_str());
    if (!root) return false;

    cJSON *props = cJSON_GetObjectItemCaseSensitive(root, "properties");
    if (!cJSON_IsObject(props)) props = root;

    std::string tex_ref = cjson_string(props, "albedoMap");
    if (tex_ref.empty()) tex_ref = cjson_string(props, "baseColorMap");
    if (tex_ref.empty()) tex_ref = cjson_string(props, "diffuseMap");
    if (tex_ref.empty()) tex_ref = cjson_string(props, "mainTexture");

    bool loaded = false;
    if (!tex_ref.empty()) {
        fs::path tex_path(tex_ref);
        if (tex_path.is_absolute()) {
            loaded = try_resolve_texture_path(tex_path, out_path);
        } else {
            loaded = try_resolve_texture_path(mat_file.parent_path() / tex_path, out_path);
            if (!loaded) {
                fs::path mat_parent = mat_file.parent_path().parent_path();
                if (!mat_parent.empty())
                    loaded = try_resolve_texture_path(mat_parent / tex_path, out_path);
            }
            if (!loaded) {
                std::vector<fs::path> roots = collect_scene_roots();
                for (const fs::path &root_dir : roots) {
                    if (try_resolve_texture_path(root_dir / tex_path, out_path)) {
                        loaded = true;
                        break;
                    }
                }
            }
            if (!loaded) {
                fs::path by_name;
                std::vector<fs::path> roots = collect_scene_roots();
                if (find_file_by_name_recursive(roots, tex_path.filename().string(), 8, &by_name))
                    loaded = try_resolve_texture_path(by_name, out_path);
            }
        }
    }

    cJSON_Delete(root);
    return loaded;
}

static bool try_resolve_texture_from_obj_mtl(const char *mesh_path, fs::path *out_path)
{
    if (!mesh_path || !out_path) return false;

    char mesh_file[512];
    if (!resolve_mesh_file_path(mesh_path, mesh_file, sizeof(mesh_file)))
        return false;

    fs::path mesh_abs(mesh_file);
    if (lower_copy(mesh_abs.extension().string()) != ".obj")
        return false;

    std::ifstream obj(mesh_abs);
    if (!obj.good()) return false;

    std::vector<std::string> mtl_refs;
    std::string line;
    while (std::getline(obj, line)) {
        std::string trimmed = trim_copy(line);
        if (trimmed.size() > 7 && lower_copy(trimmed.substr(0, 7)) == "mtllib ") {
            std::string ref = trim_copy(trimmed.substr(7));
            if (!ref.empty()) mtl_refs.push_back(ref);
        }
    }
    if (mtl_refs.empty()) return false;

    for (const std::string &mtl_ref : mtl_refs) {
        fs::path mtl_path = mesh_abs.parent_path() / fs::path(mtl_ref);
        if (!path_is_file(mtl_path)) {
            fs::path by_name;
            std::vector<fs::path> roots = collect_scene_roots();
            if (!find_file_by_name_recursive(roots, fs::path(mtl_ref).filename().string(),
                                             8, &by_name)) {
                continue;
            }
            mtl_path = by_name;
        }

        std::ifstream mtl(mtl_path);
        if (!mtl.good()) continue;

        std::string mline;
        while (std::getline(mtl, mline)) {
            std::string trimmed = trim_copy(mline);
            std::string lower = lower_copy(trimmed);
            if (!(lower.rfind("map_kd ", 0) == 0 || lower.rfind("map_ka ", 0) == 0))
                continue;

            std::string rhs = trim_copy(trimmed.substr(7));
            if (rhs.empty()) continue;

            std::string tex_ref = rhs;
            size_t sp = rhs.find_last_of(" \t");
            if (sp != std::string::npos) tex_ref = trim_copy(rhs.substr(sp + 1));
            if (tex_ref.empty()) continue;

            fs::path tex_path = mtl_path.parent_path() / fs::path(tex_ref);
            if (try_resolve_texture_path(tex_path, out_path))
                return true;

            fs::path by_name;
            std::vector<fs::path> roots = collect_scene_roots();
            if (find_file_by_name_recursive(roots, fs::path(tex_ref).filename().string(),
                                             8, &by_name)) {
                if (try_resolve_texture_path(by_name, out_path))
                    return true;
            }
        }
    }

    return false;
}

static bool resolve_texture_path_for_material(const char *material_path,
                                              const char *mesh_path,
                                              fs::path *out_path)
{
    if (!out_path) return false;
    if (s_cache.scene_dir[0] == '\0') return false;

    LOG_INFO(LOG_TAG, "resolve_texture: mat='%s' mesh='%s' scene_dir='%s'",
             material_path ? material_path : "<null>",
             mesh_path ? mesh_path : "<null>",
             s_cache.scene_dir);

    if (material_path && material_path[0] != '\0') {
        if (try_resolve_texture_from_material_json(material_path, out_path)) {
            LOG_INFO(LOG_TAG, "texture resolved via material JSON: %s", material_path);
            return true;
        }
        LOG_DEBUG(LOG_TAG, "  material JSON path failed for '%s'", material_path);
    }

    if (mesh_path && mesh_path[0] != '\0') {
        if (try_resolve_texture_from_obj_mtl(mesh_path, out_path)) {
            LOG_INFO(LOG_TAG, "texture resolved via OBJ/MTL: %s", mesh_path);
            return true;
        }
        LOG_DEBUG(LOG_TAG, "  OBJ/MTL path failed for '%s'", mesh_path);
    }

    {
        const char *mat_subdirs[] = { "Materials", "materials", nullptr };
        std::error_code ec;
        fs::path scene_root(s_cache.scene_dir);

        std::vector<std::string> mat_candidates;

        for (int di = 0; mat_subdirs[di]; di++) {
            fs::path material_dir = scene_root / mat_subdirs[di];
            if (!fs::is_directory(material_dir, ec)) continue;

            fs::recursive_directory_iterator it(material_dir,
                fs::directory_options::skip_permission_denied, ec);
            fs::recursive_directory_iterator end_it;

            for (; it != end_it; it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
                if (!it->is_regular_file(ec)) continue;
                std::string fname_lower = lower_copy(it->path().filename().string());
                if (fname_lower.size() < 9
                    || fname_lower.substr(fname_lower.size() - 9) != ".mat.json") {
                    continue;
                }

                fs::path rel = fs::relative(it->path(), scene_root, ec);
                if (ec) { ec.clear(); continue; }
                mat_candidates.push_back(rel.generic_string());
            }
        }

        std::sort(mat_candidates.begin(), mat_candidates.end(),
            [](const std::string &a, const std::string &b) {
                bool a_uni = lower_copy(a).find("universal") != std::string::npos;
                bool b_uni = lower_copy(b).find("universal") != std::string::npos;
                if (a_uni != b_uni) return a_uni;
                return a < b;
            });

        for (const std::string &candidate : mat_candidates) {
            if (try_resolve_texture_from_material_json(candidate.c_str(), out_path)) {
                LOG_INFO(LOG_TAG, "texture resolved via Materials/ scan: %s",
                         candidate.c_str());
                return true;
            }
        }
    }

    std::vector<std::string> base_names;
    auto push_base = [&base_names](const char *src) {
        if (!src || src[0] == '\0') return;
        std::string base = fs::path(src).stem().string();
        if (base.empty()) return;
        base_names.push_back(lower_copy(base));
    };
    push_base(material_path);
    push_base(mesh_path);

    if (!base_names.empty()) {
        static const char *img_exts[] = {
            ".png", ".jpg", ".jpeg", ".tga", ".bmp", nullptr
        };
        std::vector<fs::path> roots = collect_scene_roots();
        std::error_code ec;

        LOG_DEBUG(LOG_TAG, "  basename fallback: searching %d roots for %d base names",
                 (int)roots.size(), (int)base_names.size());
        for (const auto &base : base_names)
            LOG_DEBUG(LOG_TAG, "    base_name: '%s'", base.c_str());

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

                std::string ext = lower_copy(it->path().extension().string());
                bool is_img = false;
                for (int ei = 0; img_exts[ei]; ei++) {
                    if (ext == img_exts[ei]) { is_img = true; break; }
                }
                if (!is_img) continue;

                std::string stem = lower_copy(it->path().stem().string());
                for (const std::string &base : base_names) {
                    if (stem == base
                        || stem == base + "_diffuse"
                        || stem.find(base) != std::string::npos) {
                        if (try_resolve_texture_path(it->path(), out_path))
                            return true;
                    }
                }
            }
        }
    }

    return false;
}

static bool looks_like_texture_asset_path(const char *path)
{
    if (!path || path[0] == '\0')
        return false;

    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg"
        || ext == ".bmp" || ext == ".tga" || ext == ".dds"
        || ext == ".ktx" || ext == ".ktx2";
}

static int find_texture_cache_entry(const char *key)
{
    if (!key || key[0] == '\0') return -1;

    for (int i = 0; i < s_cache.tex_cache_count; i++) {
        if (strcmp(s_cache.tex_cache[i].path, key) == 0)
            return i;
    }
    return -1;
}

static void reset_texture_cache_entry(TextureCacheEntry *entry)
{
    if (!entry)
        return;

    if (entry->path[0] != '\0') {
        if (s_cache.assets && asset_handle_valid(entry->asset_handle)) {
            jce_asset_release(s_cache.assets, entry->asset_handle);
            entry->asset_handle = asset_handle_invalid();
        }

        if (jce_texture_valid(entry->tex) && !entry->tex_from_asset_manager)
            jce_texture_destroy(entry->tex);
    }

    entry->path[0] = '\0';
    entry->tex = tex_invalid();
    entry->asset_handle = asset_handle_invalid();
    entry->tex_from_asset_manager = false;
    entry->warned_missing = false;
    entry->requested = false;
    entry->failed = false;
    entry->request_generation = 0;
}

static void clear_texture_cache(void)
{
    for (int i = 0; i < s_cache.tex_cache_count; i++)
        reset_texture_cache_entry(&s_cache.tex_cache[i]);
    s_cache.tex_cache_count = 0;
}

static void texture_async_worker_main(void)
{
    for (;;) {
        TextureLoadRequest req;
        {
            std::unique_lock<std::mutex> lock(s_tex_async.mutex);
            s_tex_async.cv.wait(lock, [] {
                return s_tex_async.stop || !s_tex_async.pending.empty();
            });

            if (s_tex_async.stop && s_tex_async.pending.empty())
                break;

            if (s_tex_async.pending.empty())
                continue;

            req = std::move(s_tex_async.pending.back());
            s_tex_async.pending.pop_back();
        }

        TextureLoadResult result = {};
        result.key = req.key;
        result.generation = req.generation;
        result.success = decode_texture_rgba_path(req.file_path,
                                                  &result.rgba,
                                                  &result.width,
                                                  &result.height);

        std::lock_guard<std::mutex> lock(s_tex_async.mutex);
        s_tex_async.completed.push_back(std::move(result));
    }
}

static void texture_async_start(void)
{
    if (s_tex_async.running)
        return;

    s_tex_async.generation = 1;
    s_tex_async.stop = false;
    s_tex_async.pending.clear();
    s_tex_async.completed.clear();

    s_tex_async.worker = std::thread(texture_async_worker_main);
    s_tex_async.running = true;
}

static void texture_async_stop(void)
{
    if (!s_tex_async.running)
        return;

    {
        std::lock_guard<std::mutex> lock(s_tex_async.mutex);
        s_tex_async.stop = true;
    }
    s_tex_async.cv.notify_all();

    if (s_tex_async.worker.joinable())
        s_tex_async.worker.join();

    s_tex_async.pending.clear();
    s_tex_async.completed.clear();
    s_tex_async.running = false;
}

static void texture_async_begin_new_generation(void)
{
    std::lock_guard<std::mutex> lock(s_tex_async.mutex);
    s_tex_async.generation++;
    s_tex_async.pending.clear();
    s_tex_async.completed.clear();
}

static uint64_t texture_async_current_generation(void)
{
    std::lock_guard<std::mutex> lock(s_tex_async.mutex);
    return s_tex_async.generation;
}

static void texture_async_queue_request(const char *key, const fs::path &file_path)
{
    if (!s_tex_async.running || !key || key[0] == '\0' || file_path.empty())
        return;

    bool inserted = false;
    {
        std::lock_guard<std::mutex> lock(s_tex_async.mutex);
        for (TextureLoadRequest &req : s_tex_async.pending) {
            if (req.generation == s_tex_async.generation && req.key == key) {
                inserted = true;
                break;
            }
        }

        if (!inserted) {
            TextureLoadRequest req;
            req.key = key;
            req.file_path = file_path.string();
            req.generation = s_tex_async.generation;
            s_tex_async.pending.push_back(std::move(req));
            inserted = true;
        }
    }

    if (inserted)
        s_tex_async.cv.notify_one();
}

static void texture_async_take_completed(std::vector<TextureLoadResult> *out)
{
    if (!out) return;

    std::lock_guard<std::mutex> lock(s_tex_async.mutex);
    out->swap(s_tex_async.completed);
}

static void texture_async_push_back_completed(std::vector<TextureLoadResult> *results)
{
    if (!results || results->empty()) return;

    std::lock_guard<std::mutex> lock(s_tex_async.mutex);
    for (TextureLoadResult &res : *results)
        s_tex_async.completed.push_back(std::move(res));
    results->clear();
}

static void texture_finalize_completed_loads(void)
{
    std::vector<TextureLoadResult> completed;
    texture_async_take_completed(&completed);
    if (completed.empty())
        return;

    const uint64_t generation = texture_async_current_generation();
    uint32_t finalized = 0;
    std::vector<TextureLoadResult> deferred;
    deferred.reserve(completed.size());

    for (TextureLoadResult &res : completed) {
        if (res.generation != generation)
            continue;

        const int idx = find_texture_cache_entry(res.key.c_str());
        if (idx < 0)
            continue;

        if (finalized >= TEX_FINALIZE_BUDGET_PER_FRAME) {
            deferred.push_back(std::move(res));
            continue;
        }

        s_cache.tex_cache[idx].requested = false;

        if (!res.success || res.rgba.empty() || res.width == 0 || res.height == 0) {
            s_cache.tex_cache[idx].failed = true;
            continue;
        }

        SDL_Surface *surf = SDL_CreateSurface((int)res.width,
                                              (int)res.height,
                                              SDL_PIXELFORMAT_RGBA32);
        if (!surf || !surf->pixels) {
            if (surf) SDL_DestroySurface(surf);
            s_cache.tex_cache[idx].failed = true;
            LOG_WARN(LOG_TAG, "texture finalize failed (surface): %s",
                     res.key.c_str());
            continue;
        }

        const size_t row_bytes = (size_t)res.width * 4;
        const uint8_t *src = res.rgba.data();
        uint8_t *dst = (uint8_t *)surf->pixels;
        for (uint32_t y = 0; y < res.height; y++) {
            memcpy(dst + (size_t)y * (size_t)surf->pitch,
                   src + (size_t)y * row_bytes,
                   row_bytes);
        }

        JceTexture tex = jce_texture_load_from_surface(surf, JCE_TEX_WRAP);
        SDL_DestroySurface(surf);

        if (!jce_texture_valid(tex)) {
            s_cache.tex_cache[idx].failed = true;
            LOG_WARN(LOG_TAG, "texture finalize failed (upload): %s",
                     res.key.c_str());
            continue;
        }

        if (jce_texture_valid(s_cache.tex_cache[idx].tex)
            && !s_cache.tex_cache[idx].tex_from_asset_manager) {
            jce_texture_destroy(s_cache.tex_cache[idx].tex);
        }

        if (s_cache.assets && asset_handle_valid(s_cache.tex_cache[idx].asset_handle)) {
            jce_asset_release(s_cache.assets, s_cache.tex_cache[idx].asset_handle);
            s_cache.tex_cache[idx].asset_handle = asset_handle_invalid();
        }

        s_cache.tex_cache[idx].tex = tex;
        s_cache.tex_cache[idx].tex_from_asset_manager = false;
        s_cache.tex_cache[idx].warned_missing = false;
        s_cache.tex_cache[idx].failed = false;
        finalized++;
    }

    if (!deferred.empty())
        texture_async_push_back_completed(&deferred);
}

static JceTexture get_cached_texture(const char *material_path,
                                     const char *mesh_path)
{
    const char *key = (material_path && material_path[0] != '\0')
                    ? material_path : mesh_path;
    if (!key || key[0] == '\0') return tex_invalid();

    int idx = find_texture_cache_entry(key);
    if (idx < 0) {
        if (s_cache.tex_cache_count >= 256)
            return tex_invalid();

        idx = s_cache.tex_cache_count++;
        reset_texture_cache_entry(&s_cache.tex_cache[idx]);
        snprintf(s_cache.tex_cache[idx].path,
                 sizeof(s_cache.tex_cache[0].path), "%s", key);
    }

    TextureCacheEntry *entry = &s_cache.tex_cache[idx];

    if (entry->tex_from_asset_manager
        && s_cache.assets
        && asset_handle_valid(entry->asset_handle)) {
        JceAssetState state = jce_asset_state(s_cache.assets, entry->asset_handle);
        if (state == JCE_ASSET_STATE_READY) {
            JceTexture tex = jce_asset_get_texture(s_cache.assets,
                                                   entry->asset_handle);
            if (jce_texture_valid(tex)) {
                entry->tex = tex;
                entry->warned_missing = false;
                entry->failed = false;
                return tex;
            }

            jce_asset_release(s_cache.assets, entry->asset_handle);
            entry->asset_handle = asset_handle_invalid();
            entry->tex = tex_invalid();
            entry->tex_from_asset_manager = false;
        } else if (state == JCE_ASSET_STATE_FAILED
                || state == JCE_ASSET_STATE_UNLOADED) {
            jce_asset_release(s_cache.assets, entry->asset_handle);
            entry->asset_handle = asset_handle_invalid();
            entry->tex = tex_invalid();
            entry->tex_from_asset_manager = false;
        } else {
            entry->tex = tex_invalid();
            return tex_invalid();
        }
    }

    if (jce_texture_valid(entry->tex)) {
        if (s_cache.assets
            && !entry->tex_from_asset_manager
            && asset_handle_valid(entry->asset_handle)) {
            jce_asset_release(s_cache.assets, entry->asset_handle);
            entry->asset_handle = asset_handle_invalid();
        }
        return entry->tex;
    }
    if (entry->failed)
        return tex_invalid();

    if (s_cache.assets && material_path && looks_like_texture_asset_path(material_path)) {
        if (!asset_handle_valid(entry->asset_handle)) {
            JceAssetLoadParams params = asset_load_params_default();
            params.texture_sampler_mode = JCE_TEX_WRAP;
            params.sync = true;
            entry->asset_handle =
                jce_asset_acquire(s_cache.assets,
                                  material_path,
                                  JCE_ASSET_TEXTURE,
                                  &params);
        }

        if (asset_handle_valid(entry->asset_handle)) {
            JceAssetState state = jce_asset_state(s_cache.assets,
                                                  entry->asset_handle);
            if (state == JCE_ASSET_STATE_READY) {
                JceTexture tex = jce_asset_get_texture(s_cache.assets,
                                                       entry->asset_handle);
                if (jce_texture_valid(tex)) {
                    entry->tex = tex;
                    entry->tex_from_asset_manager = true;
                    entry->warned_missing = false;
                    entry->failed = false;
                    return tex;
                }
            } else if (state == JCE_ASSET_STATE_FAILED
                    || state == JCE_ASSET_STATE_UNLOADED) {
                jce_asset_release(s_cache.assets, entry->asset_handle);
                entry->asset_handle = asset_handle_invalid();
            } else {
                return tex_invalid();
            }
        }
    }

    const uint64_t generation = texture_async_current_generation();
    if (entry->requested
        && entry->request_generation == generation) {
        return tex_invalid();
    }

    fs::path resolved_path;
    if (!resolve_texture_path_for_material(material_path, mesh_path, &resolved_path)) {
        entry->failed = true;
        LOG_WARN(LOG_TAG,
                 "tex cache: MISS (no texture found) key='%s' mat='%s' mesh='%s'",
                 key, material_path ? material_path : "<null>",
                 mesh_path ? mesh_path : "<null>");
        return tex_invalid();
    }

    entry->requested = true;
    entry->request_generation = generation;

    texture_async_queue_request(key, resolved_path);
    return tex_invalid();
}

void jce_editor_scene_asset_cache_init(JceAssetManager *assets)
{
    if (s_cache.initialized) {
        s_cache.assets = assets;
        return;
    }

    memset(&s_cache, 0, sizeof(s_cache));
    s_cache.assets = assets;
    mesh_async_start();
    texture_async_start();
    s_cache.initialized = true;
}

void jce_editor_scene_asset_cache_shutdown(void)
{
    mesh_async_stop();
    texture_async_stop();
    clear_mesh_cache();
    clear_texture_cache();
    memset(&s_cache, 0, sizeof(s_cache));
}

void jce_editor_scene_asset_cache_finalize(void)
{
    if (!s_cache.initialized)
        return;

    mesh_finalize_completed_loads();
    texture_finalize_completed_loads();
}

void jce_editor_scene_asset_cache_set_scene_dir(const char *dir)
{
    if (!s_cache.initialized)
        return;

    mesh_async_begin_new_generation();
    texture_async_begin_new_generation();

    if (dir) {
        snprintf(s_cache.scene_dir, sizeof(s_cache.scene_dir), "%s", dir);
    } else {
        s_cache.scene_dir[0] = '\0';
    }

    clear_mesh_cache();
    clear_texture_cache();
}

JceMesh *jce_editor_scene_asset_cache_get_mesh(const char *mesh_path,
                                               const float *world_pos)
{
    if (!s_cache.initialized)
        return NULL;
    return get_cached_mesh(mesh_path, world_pos);
}

JceTexture jce_editor_scene_asset_cache_get_texture(const char *material_path,
                                                    const char *mesh_path)
{
    if (!s_cache.initialized)
        return tex_invalid();
    return get_cached_texture(material_path, mesh_path);
}

bool jce_editor_scene_asset_cache_take_texture_warning(const char *material_path,
                                                       const char *mesh_path)
{
    const char *key = (material_path && material_path[0] != '\0')
                    ? material_path : mesh_path;
    if (!key || key[0] == '\0')
        return false;

    int idx = find_texture_cache_entry(key);
    if (idx < 0)
        return false;

    TextureCacheEntry &entry = s_cache.tex_cache[idx];
    if (!entry.failed || entry.warned_missing)
        return false;

    entry.warned_missing = true;
    return true;
}