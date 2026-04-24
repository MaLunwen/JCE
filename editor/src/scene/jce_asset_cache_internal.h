/*
 * jce_asset_cache_internal.h  Shared state for scene asset cache files.
 */

#ifndef JCE_ASSET_CACHE_INTERNAL_H
#define JCE_ASSET_CACHE_INTERNAL_H

#include "jce_editor_scene_asset_cache.h"
#include "jce_model_loader_assimp.h"

#include <string.h>
#include <math.h>
#include <stdio.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
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
#include <jce/core/jce_thread.h>
#include <jce/graphics/jce_mesh.h>
#include <jce/graphics/jce_texture.h>
#include <jce/resource/jce_asset.h>
}

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#define LOG_TAG "scene_asset_cache"

namespace fs = std::filesystem;

/* ── Cache entry types ──────────────────────────────────────────── */

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

struct MeshAsyncState {
    JceThread                   *worker;
    JceMutex                    *mutex;
    JceCondVar                  *cv;
    std::vector<MeshLoadRequest> pending;
    std::vector<MeshLoadResult>  completed;
    uint32_t                    discovery;
    uint64_t                    generation;
    bool                        running;
    bool                        stop;
};

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

struct TextureAsyncState {
    JceThread                      *worker;
    JceMutex                       *mutex;
    JceCondVar                     *cv;
    std::vector<TextureLoadRequest> pending;
    std::vector<TextureLoadResult>  completed;
    uint64_t                       generation;
    bool                           running;
    bool                           stop;
};

extern TextureAsyncState s_tex_async;

/* ── Material async types + state (thread-pool based) ───────────── */

struct MaterialAsyncContext {
    uint32_t              entity_id;
    char                  mesh_path[128];
    char                  file_path[512];
    uint64_t              generation;
    bool                  success;
    JceEditorMaterialInfo material;
};

struct MaterialInFlightTask {
    JceTask              *task;
    MaterialAsyncContext *context;
};

struct MaterialAsyncState {
    JceThreadPool                              *pool;
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

/* RAII guard around JceMutex — replaces std::lock_guard usage. */
struct JceMutexGuard {
    JceMutex *m;
    explicit JceMutexGuard(JceMutex *mtx) : m(mtx) { jce_mutex_lock(m); }
    ~JceMutexGuard() { jce_mutex_unlock(m); }
    JceMutexGuard(const JceMutexGuard &) = delete;
    JceMutexGuard &operator=(const JceMutexGuard &) = delete;
};

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
void texture_async_queue_request(const char *key, const fs::path &file_path);
void texture_async_queue_resolve_request(const char *key,
                                         const char *material_path,
                                         const char *mesh_path);

bool decode_texture_rgba_path(const fs::path &path,
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
bool        path_is_file(const fs::path &path);
std::vector<fs::path> collect_scene_roots(void);
bool find_file_by_name_recursive(const std::vector<fs::path> &roots,
                                 const std::string &file_name,
                                 int max_depth,
                                 fs::path *out);

bool resolve_texture_path_for_material(const char *material_path,
                                       const char *mesh_path,
                                       fs::path *out_path);

#endif /* JCE_ASSET_CACHE_INTERNAL_H */
