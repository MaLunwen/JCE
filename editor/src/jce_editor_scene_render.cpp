/*
 * jce_editor_scene_render.cpp  Editor 3D scene rendering (FBO pipeline).
 *
 * Renders sky gradient, grid, and entities to an off-screen framebuffer.
 * The resulting texture is displayed in the ImGui scene panel.
 *
 * Reference: SceneViewWindow.java, EditorRenderOrchestrator.java.
 */

#include "jce_editor_scene_render.h"
#include "jce_editor_state.h"
#include "jce_model_loader_assimp.h"

#include <bgfx/c99/bgfx.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <filesystem>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <limits>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

extern "C" {
#include <cjson/cJSON.h>
#include <jce/graphics/jce_editor_render_bridge.h>
#include <jce/graphics/jce_renderer.h>
#include <jce/graphics/jce_shaders.h>
#include <jce/graphics/jce_camera.h>
#include <jce/graphics/jce_views.h>
#include <jce/graphics/jce_mesh.h>
#include <jce/graphics/jce_material.h>
#include <jce/graphics/jce_texture.h>
#include <jce/graphics/jce_lighting.h>
#include <jce/graphics/jce_lighting_system.h>
#include <jce/graphics/jce_pbr_material.h>
#include <jce/resource/pak_loader.h>
#include <jce/core/jce_math.h>
#include <jce/core/jce_log.h>
}

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#define LOG_TAG "scene_render"

namespace fs = std::filesystem;

/* ── Background color: rgba(30, 30, 40, 255) ──────────────────────── */

#define BG_COLOR_RGBA  0x365FA0FF  /* matches Unity-like sky zenith */

/* ── Vertex type for transient buffers ─────────────────────────────── */

struct PosColorVertex {
    float    x, y, z;
    uint32_t abgr;
};

/* ── Internal state ────────────────────────────────────────────────── */

static struct {
    bool                    initialized;
    JceRenderer            *renderer;
    JceEditorRenderBridge  *bridge;
    JceCamera              *camera;
    bgfx_vertex_layout_t    layout;
    bgfx_program_handle_t   prog_color;
    bgfx_program_handle_t   prog_grid;

    /* Sky shader resources. */
    bgfx_program_handle_t   prog_sky;
    bgfx_vertex_layout_t    sky_layout;
    bgfx_uniform_handle_t   u_sky_colors;
    bgfx_uniform_handle_t   u_grid_camera;
    bgfx_uniform_handle_t   u_grid_fade;

    /* Procedural meshes for entity placeholders. */
    JceMesh                *cube_mesh;
    JceMesh                *plane_mesh;
    JceMesh                *sphere_mesh;
    JceMesh                *capsule_mesh;
    JceMesh                *cylinder_mesh;

    /* Orbit camera state (Maya-style). */
    jce_vec3                orbit_target;
    float                   orbit_distance;
    float                   orbit_yaw;    /* radians */
    float                   orbit_pitch;  /* radians */

    /* Mesh cache for loaded models (path → JceMesh*). */
    struct {
        char     path[128];
        JceMesh *mesh;
        bool     requested;
        bool     failed;
        uint64_t request_generation;
    } mesh_cache[512];
    int mesh_cache_count;

    /* 1×1 white fallback texture for SHADED mode and missing textures. */
    bgfx_texture_handle_t  white_tex;

    /* 8×8 magenta/black checkerboard for missing textures in TEXTURED mode. */
    bgfx_texture_handle_t  checker_tex;

    /* Texture cache for material diffuse textures (path → JceTexture). */
    struct {
        char       path[128];
        JceTexture tex;
        bool       tried;   /* true if load was attempted (even if it failed) */
        bool       requested;
        bool       failed;
        uint64_t   request_generation;
    } tex_cache[256];
    int tex_cache_count;

    /* Scene base directory for resolving mesh paths. */
    char scene_dir[512];

    /* Cached lighting uniform handles for flat-color selection outlines. */
    bgfx_uniform_handle_t  u_light_dir;
    bgfx_uniform_handle_t  u_light_color;

    /* ── Shadow mapping ─────────────────────────────────────────── */
    bgfx_texture_handle_t        shadow_tex;
    bgfx_frame_buffer_handle_t   shadow_fbo;
    bgfx_uniform_handle_t        u_shadowMap;
    bgfx_uniform_handle_t        u_shadowVP;
    bool                         shadow_valid;

    /* ── Multi-light environment ────────────────────────────────── */
    JceLightEnv                 *light_env;
} s_sr;

struct MeshLoadRequest {
    std::string mesh_path;
    std::string file_path;
    float       priority_dist2;
    uint32_t    order;
    uint64_t    generation;
};

struct MeshLoadResult {
    std::string        mesh_path;
    uint64_t           generation;
    bool               success;
    JceEditorCpuMeshData cpu;
};

struct MeshAsyncState {
    std::thread               worker;
    std::mutex                mutex;
    std::condition_variable   cv;
    std::vector<MeshLoadRequest> pending;
    std::vector<MeshLoadResult>  completed;
    uint32_t                  discovery;
    uint64_t                  generation;
    bool                      running;
    bool                      stop;
};

static MeshAsyncState s_mesh_async = {};

struct TextureLoadRequest {
    std::string key;
    std::string file_path;
    uint64_t    generation;
};

struct TextureLoadResult {
    std::string            key;
    uint64_t               generation;
    bool                   success;
    std::vector<uint8_t>   rgba;
    uint32_t               width;
    uint32_t               height;
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

/* ── Helpers ────────────────────────────────────────────────────────── */

static uint16_t scene_view_id(void)
{
    if (s_sr.bridge)
        return jce_editor_render_bridge_get_view_id(s_sr.bridge);
    return (uint16_t)JCE_VIEW_EDITOR_SCENE;
}

#define MESH_FINALIZE_BUDGET_PER_FRAME    2
#define TEX_FINALIZE_BUDGET_PER_FRAME     4

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

/* ── Sky gradient (smooth sky dome — no visible edges) ───────────────── */

static void draw_sky_gradient(void)
{
    /*
     * Fullscreen NDC quad sky — reference: EditorOverlayRenderer.java.
     *
     * A fullscreen quad is placed at depth 1.0 (far plane) in clip space.
     * The fragment shader reconstructs the world-space view ray direction
     * for each pixel using bgfx's built-in u_invViewProj, then blends
     * three colours (top / horizon / ground) based on direction.y.
     *
     * This avoids all geometry-in-world-space artefacts (tilted planes,
     * visible rim circles, corner artefacts) that plagued the dome/cone
     * approaches, and matches the Java reference sky exactly.
     *
     * Sky color palette (matching uSkyColorTop / uSkyColorHorizon /
     * uGroundColor from the Java reference):
     *   top     (0.40, 0.60, 0.90) — cornflower blue
     *   horizon (0.70, 0.80, 0.95) — pale sky
     *   ground  (0.25, 0.25, 0.30) — dark warm grey
     */
    if (!BGFX_HANDLE_IS_VALID(s_sr.prog_sky)) return;

    /* Fullscreen quad: 4 NDC corners, 2 triangles. */
    struct SkyVertex { float x, y, z; };

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;
    if (!bgfx_alloc_transient_buffers(&tvb, &s_sr.sky_layout, 4, &tib, 6, false))
        return;

    SkyVertex *v  = (SkyVertex *)tvb.data;
    uint16_t  *ix = (uint16_t  *)tib.data;

    v[0] = { -1.0f, -1.0f, 0.0f };
    v[1] = {  1.0f, -1.0f, 0.0f };
    v[2] = {  1.0f,  1.0f, 0.0f };
    v[3] = { -1.0f,  1.0f, 0.0f };

    ix[0] = 0; ix[1] = 1; ix[2] = 2;
    ix[3] = 0; ix[4] = 2; ix[5] = 3;

    /* Sky gradient colors: top / horizon / ground. */
    float sky_colors[12] = {
        0.25f, 0.45f, 0.80f, 1.0f,   /* [0] top     — deeper blue */
        0.65f, 0.78f, 0.92f, 1.0f,   /* [1] horizon — soft pale   */
        0.22f, 0.22f, 0.28f, 1.0f,   /* [2] ground  — dark grey   */
    };
    bgfx_set_uniform(s_sr.u_sky_colors, sky_colors, 3);

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, 4);
    bgfx_set_transient_index_buffer(&tib, 0, 6);

    /* No depth test / depth write: the sky always fills the background
     * and is overdrawn by grid and entities submitted afterwards. */
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA, 0);

    jce_mat4 identity = jce_m4_identity();
    bgfx_set_transform(identity.raw[0], 1);
    bgfx_submit(scene_view_id(), s_sr.prog_sky, 0, BGFX_DISCARD_ALL);
}

/* ── Infinite Grid Rendering (Blender-like fullscreen shader) ─────── */

static void draw_grid(void)
{
    if (!BGFX_HANDLE_IS_VALID(s_sr.prog_grid)) return;

    struct GridVertex { float x, y, z; };
    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t tib;
    if (!bgfx_alloc_transient_buffers(&tvb, &s_sr.sky_layout, 4, &tib, 6, false))
        return;

    GridVertex *v = (GridVertex *)tvb.data;
    uint16_t *ix = (uint16_t *)tib.data;
    v[0] = { -1.0f, -1.0f, 0.0f };
    v[1] = {  1.0f, -1.0f, 0.0f };
    v[2] = {  1.0f,  1.0f, 0.0f };
    v[3] = { -1.0f,  1.0f, 0.0f };
    ix[0] = 0; ix[1] = 1; ix[2] = 2;
    ix[3] = 0; ix[4] = 2; ix[5] = 3;

    jce_vec3 cam_pos = jce_camera_get_position(s_sr.camera);
    float fade_near = fmaxf(16.0f, s_sr.orbit_distance * 3.0f);
    float fade_far  = fmaxf(fade_near + 40.0f, s_sr.orbit_distance * 24.0f);
    float grid_camera[4] = { cam_pos.x, cam_pos.y, cam_pos.z, 0.0f };
    float grid_fade[4] = { fade_near, fade_far, 10.0f, 1.0f };

    bgfx_set_uniform(s_sr.u_grid_camera, grid_camera, 1);
    bgfx_set_uniform(s_sr.u_grid_fade, grid_fade, 1);
    bgfx_set_transient_vertex_buffer(0, &tvb, 0, 4);
    bgfx_set_transient_index_buffer(&tib, 0, 6);

    uint64_t state = BGFX_STATE_WRITE_RGB
                   | BGFX_STATE_WRITE_A
                   | BGFX_STATE_MSAA
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                            BGFX_STATE_BLEND_INV_SRC_ALPHA);
    bgfx_set_state(state, 0);

    jce_mat4 identity = jce_m4_identity();
    bgfx_set_transform(identity.raw[0], 1);
    bgfx_submit(scene_view_id(), s_sr.prog_grid, 0, BGFX_DISCARD_ALL);
}

/* ── Mesh cache + resolution ────────────────────────────────────────── */

/* Convert "SM_PascalCase_Name" or "SKM_PascalCase_Name" to "pascalcase-name". */
static void sm_to_kebab(const char *sm_name, char *out, int out_size)
{
    int o = 0;
    const char *src = sm_name;
    /* Skip common mesh prefixes: SM_, SKM_, SK_ */
    if (src[0]=='S' && src[1]=='K' && src[2]=='M' && src[3]=='_') src += 4;
    else if (src[0]=='S' && src[1]=='K' && src[2]=='_') src += 3;
    else if (src[0]=='S' && src[1]=='M' && src[2]=='_') src += 3;

    for (int i = 0; src[i] && o < out_size - 1; i++) {
        char c = src[i];
        if (c == '_') {
            out[o++] = '-';
        } else if (c >= 'A' && c <= 'Z') {
            out[o++] = (char)(c + 32); /* tolower */
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

/* Resolve a mesh path to a real file path on disk.
 * Returns true and writes out_path on success. */
static bool resolve_mesh_file_path(const char *mesh_path, char *out_path,
                                   size_t out_size)
{
    if (!mesh_path || mesh_path[0] == '\0' || !out_path || out_size == 0)
        return false;
    if (s_sr.scene_dir[0] == '\0')
        return false;

    /* Absolute path fast path. */
    {
        FILE *test_abs = fopen(mesh_path, "rb");
        if (test_abs) {
            fclose(test_abs);
            return copy_found_path(mesh_path, out_path, out_size);
        }
    }

    /* Extract the base name from the mesh path (e.g., "tree-scary-dead"). */
    const char *slash = strrchr(mesh_path, '/');
    if (!slash) slash = strrchr(mesh_path, '\\');
    const char *base = slash ? slash + 1 : mesh_path;

    /* Remove .obj extension. */
    char target_kebab[256];
    snprintf(target_kebab, sizeof(target_kebab), "%s", base);
    char *dot = strrchr(target_kebab, '.');
    if (dot) *dot = '\0';
    /* Lowercase the target for comparison. */
    for (char *p = target_kebab; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);

    /* Try direct path first (scene_dir + mesh_path). */
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s/%s", s_sr.scene_dir, mesh_path);
    for (char *p = full_path; *p; p++) if (*p == '/') *p = '\\';

    {
        FILE *test = fopen(full_path, "rb");
        if (test) {
            fclose(test);
            return copy_found_path(full_path, out_path, out_size);
        }
    }

    /* Dynamically scan ALL subdirectories under scene_dir/Meshes/.
     * Also try parent directory's Meshes/ if scene_dir is nested. */
    char meshes_dirs[4][512];
    int meshes_dir_count = 0;
    snprintf(meshes_dirs[meshes_dir_count++], 512, "%s\\Meshes", s_sr.scene_dir);
    /* Some projects place Meshes/ at the project root, one level up. */
    {
        char parent[512];
        snprintf(parent, sizeof(parent), "%s", s_sr.scene_dir);
        char *sep = strrchr(parent, '\\');
        if (!sep) sep = strrchr(parent, '/');
        if (sep) {
            *sep = '\0';
            snprintf(meshes_dirs[meshes_dir_count++], 512, "%s\\Meshes", parent);
        }
    }
    /* Also try scene_dir itself (flat layout: meshes directly in scene dir). */
    snprintf(meshes_dirs[meshes_dir_count++], 512, "%s", s_sr.scene_dir);

    /* Supported model file extensions. */
    static const char *mesh_exts[] = { "*.obj", "*.fbx", "*.gltf", "*.glb", NULL };

#ifdef _WIN32
    char subdir_paths[64][512];
    int subdir_count = 0;

    /* Recursively enumerate subdirectories up to 3 levels deep. */
    for (int md = 0; md < meshes_dir_count; md++) {
        WIN32_FIND_DATAA dir_fd;
        char dir_pattern[512];
        snprintf(dir_pattern, sizeof(dir_pattern), "%s\\*", meshes_dirs[md]);
        HANDLE hDir = FindFirstFileA(dir_pattern, &dir_fd);
        if (hDir == INVALID_HANDLE_VALUE) continue;

        /* Push this directory itself. */
        if (subdir_count < 63)
            snprintf(subdir_paths[subdir_count++], 512, "%s", meshes_dirs[md]);

        /* Level 1 subdirs */
        do {
            if (!(dir_fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (dir_fd.cFileName[0] == '.') continue;
            if (subdir_count >= 63) break;
            char l1[512];
            snprintf(l1, 512, "%s\\%s", meshes_dirs[md], dir_fd.cFileName);
            snprintf(subdir_paths[subdir_count++], 512, "%s", l1);

            /* Level 2 subdirs */
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

                    /* Level 3 subdirs */
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
        } while (FindNextFileA(hDir, &dir_fd));
        FindClose(hDir);
    }

    if (subdir_count == 0) {
        LOG_WARN(LOG_TAG, "mesh not found: %s (no Meshes dirs found under %s)",
                 mesh_path, s_sr.scene_dir);
        return NULL;
    }

    int target_len = (int)strlen(target_kebab);

    for (int d = 0; d < subdir_count; d++) {
        for (int ei = 0; mesh_exts[ei]; ei++) {
            WIN32_FIND_DATAA fd;
            char search_pattern[512];
            snprintf(search_pattern, sizeof(search_pattern), "%s\\%s",
                     subdir_paths[d], mesh_exts[ei]);
            HANDLE hFind = FindFirstFileA(search_pattern, &fd);
            if (hFind == INVALID_HANDLE_VALUE) continue;
            do {
                char base_name[256];
                snprintf(base_name, sizeof(base_name), "%s", fd.cFileName);
                char *ext = strrchr(base_name, '.');
                if (ext) *ext = '\0';

                /* Direct case-insensitive filename match (no kebab conversion). */
                {
                    char file_lower[256];
                    snprintf(file_lower, sizeof(file_lower), "%s", base_name);
                    for (char *p = file_lower; *p; p++)
                        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
                    if (strcmp(file_lower, target_kebab) == 0) {
                        char found_path[512];
                        snprintf(found_path, sizeof(found_path), "%s\\%s",
                                 subdir_paths[d], fd.cFileName);
                        FindClose(hFind);
                        return copy_found_path(found_path, out_path, out_size);
                    }
                }

                /* Kebab-converted match (handles SM_PascalCase -> kebab). */
                char file_kebab[256];
                sm_to_kebab(base_name, file_kebab, sizeof(file_kebab));

                if (strcmp(file_kebab, target_kebab) == 0) {
                    char found_path[512];
                    snprintf(found_path, sizeof(found_path), "%s\\%s",
                             subdir_paths[d], fd.cFileName);
                    FindClose(hFind);
                    return copy_found_path(found_path, out_path, out_size);
                }

                /* Suffix match: target "terrain" matches file "sm-terrain". */
                int fk_len = (int)strlen(file_kebab);
                if (fk_len > target_len && target_len > 0) {
                    const char *suffix = file_kebab + (fk_len - target_len);
                    if (strcmp(suffix, target_kebab) == 0
                        && suffix[-1] == '-') {
                        char found_path[512];
                        snprintf(found_path, sizeof(found_path), "%s\\%s",
                                 subdir_paths[d], fd.cFileName);
                        FindClose(hFind);
                        return copy_found_path(found_path, out_path, out_size);
                    }
                }

                /* Substring/contains match: target "stone-f" in "nature-stone-f". */
                if (target_len > 2 && fk_len > target_len) {
                    if (strstr(file_kebab, target_kebab) != NULL) {
                        char found_path[512];
                        snprintf(found_path, sizeof(found_path), "%s\\%s",
                                 subdir_paths[d], fd.cFileName);
                        FindClose(hFind);
                        return copy_found_path(found_path, out_path, out_size);
                    }
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    }

    /* Final fallback: search by exact filename recursively from scene roots. */
    {
        std::error_code ec;
        std::string target_name = base;
        std::transform(target_name.begin(), target_name.end(), target_name.begin(),
                       [](unsigned char c) { return (char)tolower(c); });

        std::vector<fs::path> roots;
        roots.push_back(fs::path(s_sr.scene_dir));
        fs::path parent = fs::path(s_sr.scene_dir).parent_path();
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

static int find_mesh_cache_entry(const char *mesh_path)
{
    if (!mesh_path || mesh_path[0] == '\0') return -1;

    for (int i = 0; i < s_sr.mesh_cache_count; i++) {
        if (strcmp(s_sr.mesh_cache[i].path, mesh_path) == 0)
            return i;
    }
    return -1;
}

static JceMesh *get_cached_mesh(const char *mesh_path, const float *world_pos)
{
    if (!mesh_path || mesh_path[0] == '\0')
        return NULL;

    int idx = find_mesh_cache_entry(mesh_path);
    if (idx < 0) {
        if (s_sr.mesh_cache_count >= 512)
            return NULL;

        idx = s_sr.mesh_cache_count++;
        snprintf(s_sr.mesh_cache[idx].path,
                 sizeof(s_sr.mesh_cache[idx].path), "%s", mesh_path);
        s_sr.mesh_cache[idx].mesh = NULL;
        s_sr.mesh_cache[idx].requested = false;
        s_sr.mesh_cache[idx].failed = false;
        s_sr.mesh_cache[idx].request_generation = 0;
    }

    if (s_sr.mesh_cache[idx].mesh)
        return s_sr.mesh_cache[idx].mesh;
    if (s_sr.mesh_cache[idx].failed)
        return NULL;

    uint64_t generation = mesh_async_current_generation();
    if (s_sr.mesh_cache[idx].requested
        && s_sr.mesh_cache[idx].request_generation == generation) {
        return NULL;
    }

    char found_path[512];
    if (!resolve_mesh_file_path(mesh_path, found_path, sizeof(found_path))) {
        s_sr.mesh_cache[idx].failed = true;
        LOG_WARN(LOG_TAG, "mesh not found: %s (under %s)",
                 mesh_path, s_sr.scene_dir);
        return NULL;
    }

    s_sr.mesh_cache[idx].requested = true;
    s_sr.mesh_cache[idx].request_generation = generation;
    mesh_async_queue_request(mesh_path, found_path,
                             mesh_request_priority(world_pos));
    return NULL;
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

        s_sr.mesh_cache[idx].requested = false;

        if (!res.success) {
            s_sr.mesh_cache[idx].failed = true;
            jce_editor_model_free_cpu_data(&res.cpu);
            continue;
        }

        JceMesh *mesh = jce_mesh_create(res.cpu.vertices,
                                        res.cpu.vertex_count,
                                        res.cpu.indices,
                                        res.cpu.index_count);
        jce_editor_model_free_cpu_data(&res.cpu);

        if (!mesh) {
            s_sr.mesh_cache[idx].failed = true;
            LOG_WARN(LOG_TAG, "mesh finalize failed: %s", res.mesh_path.c_str());
            continue;
        }

        s_sr.mesh_cache[idx].mesh = mesh;
        finalized++;
    }

    if (!deferred.empty())
        mesh_async_push_back_completed(&deferred);
}

/* ── Texture cache for material diffuse textures ────────────────────── */

/* C++-compatible invalid texture (MSVC doesn't support compound literals in C++). */
static inline JceTexture tex_invalid(void) { JceTexture t; t.idx = UINT16_MAX; return t; }

static std::string lower_copy(const std::string &s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return out;
}

static std::string trim_copy(const std::string &s)
{
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
    return s.substr(b, e - b);
}

static bool path_is_file(const fs::path &p)
{
    std::error_code ec;
    return fs::exists(p, ec) && fs::is_regular_file(p, ec);
}

static std::vector<fs::path> collect_scene_roots(void)
{
    std::vector<fs::path> roots;
    if (s_sr.scene_dir[0] == '\0') return roots;

    fs::path scene(s_sr.scene_dir);
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
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(v) || !v->valuestring || v->valuestring[0] == '\0')
        return std::string();
    return std::string(v->valuestring);
}

static bool resolve_material_file_path(const char *material_path, fs::path *out_mat)
{
    if (!material_path || material_path[0] == '\0' || !out_mat) return false;

    std::vector<std::string> candidates;
    const std::string raw = material_path;
    candidates.push_back(raw);

    std::string lower = lower_copy(raw);
    if (lower.size() >= 9 && lower.substr(lower.size() - 9) == ".mat.json") {
        candidates.push_back(raw.substr(0, raw.size() - 5)); /* -> .mat */
    } else if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".mat") {
        candidates.push_back(raw + ".json");                /* -> .mat.json */
    } else if (lower.size() >= 9 && lower.substr(lower.size() - 9) == ".material") {
        candidates.push_back(raw.substr(0, raw.size() - 9) + ".mat.json");
    } else if (fs::path(raw).extension().empty()) {
        candidates.push_back(raw + ".mat");
        candidates.push_back(raw + ".mat.json");
    }

    std::vector<fs::path> roots = collect_scene_roots();
    for (const std::string &cand : candidates) {
        fs::path p(cand);
        if (p.is_absolute() && path_is_file(p)) {
            *out_mat = p;
            return true;
        }
        if (path_is_file(p)) {
            *out_mat = p;
            return true;
        }
        for (const fs::path &root : roots) {
            fs::path rp = root / p;
            if (path_is_file(rp)) {
                *out_mat = rp;
                return true;
            }
        }

        fs::path by_name;
        if (find_file_by_name_recursive(roots, p.filename().string(), 8, &by_name)) {
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
            /* Priority: material dir -> parent of material dir -> scene roots -> filename search. */
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
        std::string t = trim_copy(line);
        if (t.size() > 7 && lower_copy(t.substr(0, 7)) == "mtllib ") {
            std::string ref = trim_copy(t.substr(7));
            if (!ref.empty()) mtl_refs.push_back(ref);
        }
    }
    if (mtl_refs.empty()) return false;

    for (const std::string &mtl_ref : mtl_refs) {
        fs::path mtl_path = mesh_abs.parent_path() / fs::path(mtl_ref);
        if (!path_is_file(mtl_path)) {
            fs::path by_name;
            std::vector<fs::path> roots = collect_scene_roots();
            if (!find_file_by_name_recursive(roots, fs::path(mtl_ref).filename().string(), 8, &by_name))
                continue;
            mtl_path = by_name;
        }

        std::ifstream mtl(mtl_path);
        if (!mtl.good()) continue;

        std::string mline;
        while (std::getline(mtl, mline)) {
            std::string t = trim_copy(mline);
            std::string tl = lower_copy(t);
            if (!(tl.rfind("map_kd ", 0) == 0 || tl.rfind("map_ka ", 0) == 0))
                continue;

            std::string rhs = trim_copy(t.substr(7));
            if (rhs.empty()) continue;

            /* map_Kd may contain options before filename; last token is usually file path. */
            std::string tex_ref = rhs;
            size_t sp = rhs.find_last_of(" \t");
            if (sp != std::string::npos) tex_ref = trim_copy(rhs.substr(sp + 1));
            if (tex_ref.empty()) continue;

            fs::path tex_path = mtl_path.parent_path() / fs::path(tex_ref);
            if (try_resolve_texture_path(tex_path, out_path))
                return true;

            fs::path by_name;
            std::vector<fs::path> roots = collect_scene_roots();
            if (find_file_by_name_recursive(roots, fs::path(tex_ref).filename().string(), 8, &by_name)) {
                if (try_resolve_texture_path(by_name, out_path))
                    return true;
            }
        }
    }

    return false;
}

/* Resolve a texture file path from material/mesh references.
 * Resolution chain (matching Java reference intent):
 *  1) material_path -> .mat/.mat.json -> properties.albedoMap
 *  2) mesh_path OBJ -> mtllib -> map_Kd
 *  3) scan Materials/ dir for .mat.json files with albedoMap
 *  4) recursive filename fallback by mesh/material basename */
static bool resolve_texture_path_for_material(const char *material_path,
                                              const char *mesh_path,
                                              fs::path *out_path)
{
    if (!out_path) return false;
    if (s_sr.scene_dir[0] == '\0') return false;

    LOG_INFO(LOG_TAG, "resolve_texture: mat='%s' mesh='%s' scene_dir='%s'",
             material_path ? material_path : "<null>",
             mesh_path ? mesh_path : "<null>",
             s_sr.scene_dir);

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

    /* Stage 3: scan Materials/ for .mat.json files with texture references.
     * Handles scenes where entities reference 'default.mat' (or similar
     * generic names) but actual textures are defined in separate .mat.json
     * files in the scene's Materials/ directory. */
    {
        const char *mat_subdirs[] = { "Materials", "materials", nullptr };
        std::error_code ec;
        fs::path scene_root(s_sr.scene_dir);

        std::vector<std::string> mat_candidates;

        for (int di = 0; mat_subdirs[di]; di++) {
            fs::path mdir = scene_root / mat_subdirs[di];
            if (!fs::is_directory(mdir, ec)) continue;

            fs::recursive_directory_iterator it(mdir,
                fs::directory_options::skip_permission_denied, ec);
            fs::recursive_directory_iterator end_it;

            for (; it != end_it; it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
                if (!it->is_regular_file(ec)) continue;
                std::string fname_lower = lower_copy(it->path().filename().string());
                if (fname_lower.size() < 9 ||
                    fname_lower.substr(fname_lower.size() - 9) != ".mat.json")
                    continue;

                fs::path rel = fs::relative(it->path(), scene_root, ec);
                if (ec) { ec.clear(); continue; }
                mat_candidates.push_back(rel.generic_string());
            }
        }

        /* Prefer "universal" materials (common atlas pattern). */
        std::sort(mat_candidates.begin(), mat_candidates.end(),
            [](const std::string &a, const std::string &b) {
                bool a_uni = lower_copy(a).find("universal") != std::string::npos;
                bool b_uni = lower_copy(b).find("universal") != std::string::npos;
                if (a_uni != b_uni) return a_uni;
                return a < b;
            });

        for (const std::string &mc : mat_candidates) {
            if (try_resolve_texture_from_material_json(mc.c_str(), out_path)) {
                LOG_INFO(LOG_TAG, "texture resolved via Materials/ scan: %s",
                         mc.c_str());
                return true;
            }
        }
    }

    /* Recursive basename fallback (for loose assets without material metadata). */
    std::vector<std::string> base_names;
    auto push_base = [&base_names](const char *src) {
        if (!src || src[0] == '\0') return;
        std::string b = fs::path(src).stem().string();
        if (b.empty()) return;
        base_names.push_back(lower_copy(b));
    };
    push_base(material_path);
    push_base(mesh_path);

    if (!base_names.empty()) {
        static const char *img_exts[] = { ".png", ".jpg", ".jpeg", ".tga", ".bmp", nullptr };
        std::vector<fs::path> roots = collect_scene_roots();
        std::error_code ec;

        LOG_DEBUG(LOG_TAG, "  basename fallback: searching %d roots for %d base names",
                 (int)roots.size(), (int)base_names.size());
        for (const auto &bn : base_names)
            LOG_DEBUG(LOG_TAG, "    base_name: '%s'", bn.c_str());

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
                    if (stem == base || stem == base + "_diffuse" || stem.find(base) != std::string::npos) {
                        if (try_resolve_texture_path(it->path(), out_path))
                            return true;
                    }
                }
            }
        }
    }

    return false;
}

static int find_texture_cache_entry(const char *key)
{
    if (!key || key[0] == '\0') return -1;

    for (int i = 0; i < s_sr.tex_cache_count; i++) {
        if (strcmp(s_sr.tex_cache[i].path, key) == 0)
            return i;
    }
    return -1;
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

        s_sr.tex_cache[idx].requested = false;

        if (!res.success || res.rgba.empty() || res.width == 0 || res.height == 0) {
            s_sr.tex_cache[idx].failed = true;
            continue;
        }

        SDL_Surface *surf = SDL_CreateSurface((int)res.width,
                                              (int)res.height,
                                              SDL_PIXELFORMAT_RGBA32);
        if (!surf || !surf->pixels) {
            if (surf) SDL_DestroySurface(surf);
            s_sr.tex_cache[idx].failed = true;
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
            s_sr.tex_cache[idx].failed = true;
            LOG_WARN(LOG_TAG, "texture finalize failed (upload): %s",
                     res.key.c_str());
            continue;
        }

        if (jce_texture_valid(s_sr.tex_cache[idx].tex))
            jce_texture_destroy(s_sr.tex_cache[idx].tex);

        s_sr.tex_cache[idx].tex = tex;
        s_sr.tex_cache[idx].failed = false;
        finalized++;
    }

    if (!deferred.empty())
        texture_async_push_back_completed(&deferred);
}

/* Get or queue a cached texture for a material/mesh pair. */
static JceTexture get_cached_texture(const char *material_path,
                                     const char *mesh_path)
{
    /* Build a cache key from material_path (or mesh_path if no material). */
    const char *key = (material_path && material_path[0] != '\0')
                    ? material_path : mesh_path;
    if (!key || key[0] == '\0') return tex_invalid();

    int idx = find_texture_cache_entry(key);
    if (idx < 0) {
        if (s_sr.tex_cache_count >= 256)
            return tex_invalid();

        idx = s_sr.tex_cache_count++;
        snprintf(s_sr.tex_cache[idx].path,
                 sizeof(s_sr.tex_cache[0].path), "%s", key);
        s_sr.tex_cache[idx].tex = tex_invalid();
        s_sr.tex_cache[idx].tried = false;
        s_sr.tex_cache[idx].requested = false;
        s_sr.tex_cache[idx].failed = false;
        s_sr.tex_cache[idx].request_generation = 0;
    }

    if (jce_texture_valid(s_sr.tex_cache[idx].tex))
        return s_sr.tex_cache[idx].tex;
    if (s_sr.tex_cache[idx].failed)
        return tex_invalid();

    const uint64_t generation = texture_async_current_generation();
    if (s_sr.tex_cache[idx].requested
        && s_sr.tex_cache[idx].request_generation == generation) {
        return tex_invalid();
    }

    fs::path resolved_path;
    if (!resolve_texture_path_for_material(material_path, mesh_path, &resolved_path)) {
        s_sr.tex_cache[idx].failed = true;
        s_sr.tex_cache[idx].tried = true;
        LOG_WARN(LOG_TAG, "tex cache: MISS (no texture found) key='%s' mat='%s' mesh='%s'",
                 key, material_path ? material_path : "<null>",
                 mesh_path ? mesh_path : "<null>");
        return tex_invalid();
    }

    s_sr.tex_cache[idx].requested = true;
    s_sr.tex_cache[idx].request_generation = generation;
    s_sr.tex_cache[idx].tried = true;

    texture_async_queue_request(key, resolved_path);
    return tex_invalid();
}

/* ── Entity Rendering (auto-detect components) ─────────────────────── */

/* Build a model matrix for entity i from its transform components.
   Returns false if the entity has no transform. */
static bool build_entity_model(JceEntityInfo *ent, jce_mat4 *out_model,
                                JceMesh **out_mesh,
                                const char **out_material_path)
{
    int comp_count = 0;
    JceComponentInfo *comps = jce_state_get_entity_components(ent->id, &comp_count);
    if (!comps) return false;

    const float *pos   = NULL;
    const float *rot   = NULL;
    const float *scale = NULL;
    bool has_mesh  = false;
    const char *mesh_path = NULL;
    const char *mat_path  = NULL;
    int  mesh_shape = 0; /* JCE_MESH_SHAPE_CUBE */

    for (int c = 0; c < comp_count; c++) {
        switch (comps[c].type) {
        case JCE_COMP_TRANSFORM:
            pos   = comps[c].data.transform.pos;
            rot   = comps[c].data.transform.rot;
            scale = comps[c].data.transform.scale;
            break;
        case JCE_COMP_MESH_RENDERER:
            has_mesh  = true;
            mesh_path = comps[c].data.mesh_renderer.mesh_path;
            mat_path  = comps[c].data.mesh_renderer.material_path;
            mesh_shape = comps[c].data.mesh_renderer.mesh_shape;
            break;
        default: break;
        }
    }

    if (!pos) return false;

    float sx = (scale && scale[0] != 0.0f) ? scale[0] : 1.0f;
    float sy = (scale && scale[1] != 0.0f) ? scale[1] : 1.0f;
    float sz = (scale && scale[2] != 0.0f) ? scale[2] : 1.0f;

    if (rot && (rot[0] != 0.0f || rot[1] != 0.0f || rot[2] != 0.0f)) {
        *out_model = jce_m4_from_trs(
            jce_v3(pos[0], pos[1], pos[2]),
            jce_q_from_euler(rot[0] * JCE_DEG2RAD, rot[1] * JCE_DEG2RAD, rot[2] * JCE_DEG2RAD),
            jce_v3(sx, sy, sz));
    } else {
        *out_model = jce_m4_identity();
        out_model->raw[0][0] = sx;
        out_model->raw[1][1] = sy;
        out_model->raw[2][2] = sz;
        out_model->raw[3][0] = pos[0];
        out_model->raw[3][1] = pos[1];
        out_model->raw[3][2] = pos[2];
    }

    if (out_mesh) {
        *out_mesh = NULL;
        if (has_mesh) {
            if (mesh_path && mesh_path[0] != '\0') {
                /* Queue background decode and return placeholder until ready. */
                *out_mesh = get_cached_mesh(mesh_path, pos);
            }
            /* If no file mesh loaded, fall back to procedural shape. */
            if (!*out_mesh) {
                switch (mesh_shape) {
                default: /* fall through */
                case JCE_MESH_SHAPE_CUBE:     *out_mesh = s_sr.cube_mesh;     break;
                case JCE_MESH_SHAPE_SPHERE:   *out_mesh = s_sr.sphere_mesh;   break;
                case JCE_MESH_SHAPE_PLANE:    *out_mesh = s_sr.plane_mesh;    break;
                case JCE_MESH_SHAPE_CAPSULE:  *out_mesh = s_sr.capsule_mesh;  break;
                case JCE_MESH_SHAPE_CYLINDER: *out_mesh = s_sr.cylinder_mesh; break;
                }
            }
        }
        /* has_mesh == false: entity has no MeshRenderer — no visual geometry. */
    }

    if (out_material_path)
        *out_material_path = mat_path;

    return true;
}

/* Orange wireframe overlay for selected entities (flat color, no lighting). */
static void draw_selection_outlines(void)
{
    int sel_count = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_count);
    if (sel_count == 0) return;

    /*
     * Flat-color mode: set u_lightDir.w = -1.0 to tell fs_mesh to output
     * u_lightColor.xyz directly, bypassing diffuse lighting.
     * This produces a consistent orange regardless of face normal.
     * Reference: EditorOverlayRenderer.java — SELECTION_COLOR (1.0, 0.75, 0.0).
     */
    float flat_dir[4]    = { 0.0f, -1.0f, 0.0f, -1.0f };  /* w<0 = flat mode */
    float flat_color[4]  = { 1.0f, 0.75f, 0.0f, 1.0f };    /* pure orange */

    /* Bind white texture so texel sampling is neutral. */
    JceUniformHandle uh = jce_renderer_get_tex_uniform(s_sr.renderer);
    bgfx_uniform_handle_t su = { uh.idx };

    for (int i = 0; i < sel_count; i++) {
        JceEntityInfo *ent = jce_state_get_entity(sel[i]);
        if (!ent || !ent->enabled) continue;

        jce_mat4 model;
        JceMesh *mesh = NULL;
        const char *mat_path = NULL;
        if (!build_entity_model(ent, &model, &mesh, &mat_path)) continue;
        if (!mesh) continue;

        bgfx_set_transform(model.raw[0], 1);

        /* Set flat-color uniforms + texture per draw call (bgfx consumes per submit). */
        bgfx_set_uniform(s_sr.u_light_dir,   flat_dir,   1);
        bgfx_set_uniform(s_sr.u_light_color, flat_color,  1);
        bgfx_set_texture(0, su, s_sr.white_tex, UINT32_MAX);

        jce_mesh_submit_wireframe_overlay(mesh, s_sr.renderer, scene_view_id());
    }

    /* Restore normal lighting. */
    JceDirLight sun = jce_dir_light_default();
    jce_lighting_apply(s_sr.renderer, &sun);
}

/* ── Shadow map pass ────────────────────────────────────────────────── */

#define SHADOW_MAP_SIZE  2048
#define SHADOW_ORTHO_SIZE 20.0f

static void compute_shadow_vp(const jce_vec3 *light_dir, float shadow_vp[16])
{
    /* Build an orthographic "camera" looking along the light direction. */
    jce_vec3 center = jce_v3(0.0f, 0.0f, 0.0f);
    jce_vec3 ld = jce_v3_normalize(*light_dir);
    jce_vec3 light_pos = jce_v3_scale(ld, 30.0f); /* push back from origin */

    jce_vec3 up = (fabsf(ld.y) > 0.99f) ? jce_v3(0,0,1) : jce_v3(0,1,0);

    /* Look-at view matrix: light position → center. */
    jce_mat4 view = jce_m4_look_at(light_pos, center, up);

    /* Orthographic projection enclosing the scene. */
    const bgfx_caps_t *caps = bgfx_get_caps();
    float S = SHADOW_ORTHO_SIZE;
    jce_mat4 proj = jce_m4_ortho(-S, S, -S, S, 0.1f, 80.0f,
                                  caps->homogeneousDepth);

    jce_mat4 vp = jce_m4_multiply(&proj, &view);
    memcpy(shadow_vp, vp.raw, 16 * sizeof(float));
}

static void draw_shadow_pass(void)
{
    if (!s_sr.shadow_valid) return;

    JceShaderHandle shadow_sh = jce_renderer_get_program_shadow(s_sr.renderer);
    if (shadow_sh.idx == UINT16_MAX) return;

    JceDirLight sun = jce_dir_light_default();

    /* Compute light-space view-projection. */
    float shadow_vp[16];
    compute_shadow_vp(&sun.direction, shadow_vp);

    /* Configure shadow view. */
    const uint16_t shadow_view = (uint16_t)JCE_VIEW_SHADOW_0;
    bgfx_set_view_rect(shadow_view, 0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
    bgfx_set_view_frame_buffer(shadow_view, s_sr.shadow_fbo);
    bgfx_set_view_clear(shadow_view,
                        BGFX_CLEAR_DEPTH, 0, 1.0f, 0);

    /* Set the VP matrix for the shadow view. */
    float identity[16];
    memset(identity, 0, sizeof(identity));
    identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
    bgfx_set_view_transform(shadow_view, identity, shadow_vp);

    /* Store shadow VP for the PBR shader. */
    bgfx_set_uniform(s_sr.u_shadowVP, shadow_vp, 1);

    /* Submit all mesh entities to the shadow depth view. */
    int count = jce_state_get_entity_count();
    for (int i = 0; i < count; i++) {
        JceEntityInfo *ent = jce_state_get_entity_by_index(i);
        if (!ent || !ent->enabled) continue;

        jce_mat4 model;
        JceMesh *mesh = NULL;
        const char *mat_path = NULL;
        if (!build_entity_model(ent, &model, &mesh, &mat_path)) continue;
        if (!mesh) continue;

        bgfx_set_transform(model.raw[0], 1);
        jce_mesh_submit_shadow(mesh, s_sr.renderer, shadow_view);
    }
}

static void draw_entities(void)
{
    int count = jce_state_get_entity_count();
    if (count == 0) return;

    /* Render shadow depth pass before main scene. */
    draw_shadow_pass();

    /* ── Gather lights from entity components ─────────────────── */
    if (s_sr.light_env) {
        jce_light_env_clear(s_sr.light_env);
        jce_light_env_set_ambient(s_sr.light_env,
                                  jce_v3(1.0f, 1.0f, 1.0f), 0.15f);

        bool has_any_light = false;
        for (int i = 0; i < count; i++) {
            JceEntityInfo *ent = jce_state_get_entity_by_index(i);
            if (!ent || !ent->enabled) continue;

            int comp_count = 0;
            JceComponentInfo *comps = jce_state_get_entity_components(ent->id,
                                                                      &comp_count);
            for (int c = 0; c < comp_count; c++) {
                if (comps[c].type != JCE_COMP_LIGHT) continue;

                const auto &ld = comps[c].data.light;
                jce_vec3 color = jce_v3(ld.color[0], ld.color[1], ld.color[2]);
                float intensity = ld.intensity;

                /* Get transform for light position/direction. */
                JceComponentInfo *xf = NULL;
                for (int t = 0; t < comp_count; t++) {
                    if (comps[t].type == JCE_COMP_TRANSFORM) { xf = &comps[t]; break; }
                }

                if (ld.type == 0) {
                    /* Directional light. */
                    JceDirLightDesc dl;
                    memset(&dl, 0, sizeof(dl));
                    dl.color = color;
                    dl.intensity = intensity > 0.0f ? intensity : 1.0f;
                    if (xf) {
                        /* Use negative Z as direction (forward). */
                        float yaw_rad = xf->data.transform.rot[1] * JCE_DEG2RAD;
                        float pitch_rad = xf->data.transform.rot[0] * JCE_DEG2RAD;
                        dl.direction = jce_v3(
                            -sinf(yaw_rad),
                             sinf(pitch_rad),
                            -cosf(yaw_rad));
                    } else {
                        dl.direction = jce_v3(0.5f, 1.0f, 0.3f);
                    }
                    jce_light_env_add_dir_light(s_sr.light_env, &dl);
                    has_any_light = true;
                } else if (ld.type == 1) {
                    /* Point light. */
                    JcePointLightDesc pl;
                    memset(&pl, 0, sizeof(pl));
                    pl.color = color;
                    pl.intensity = intensity > 0.0f ? intensity : 1.0f;
                    pl.radius = 10.0f;
                    if (xf) {
                        pl.position = jce_v3(xf->data.transform.pos[0],
                                             xf->data.transform.pos[1],
                                             xf->data.transform.pos[2]);
                    }
                    jce_light_env_add_point_light(s_sr.light_env, &pl);
                    has_any_light = true;
                }
            }
        }

        /* Fallback: always ensure at least one directional light. */
        if (!has_any_light) {
            JceDirLightDesc dl;
            memset(&dl, 0, sizeof(dl));
            dl.direction = jce_v3(0.5f, 1.0f, 0.3f);
            dl.color = jce_v3(1.0f, 1.0f, 1.0f);
            dl.intensity = 1.0f;
            jce_light_env_add_dir_light(s_sr.light_env, &dl);
        }

        /* Set camera position for PBR specular. */
        jce_vec3 cam_pos = jce_camera_get_position(s_sr.camera);
        jce_light_env_set_camera_pos(s_sr.light_env, cam_pos);

        /* Upload all light uniforms. */
        jce_light_env_apply(s_sr.light_env, s_sr.renderer);

        /* Also set legacy u_lightDir / u_lightColor for the basic mesh shader. */
        JceDirLight sun = jce_dir_light_default();
        jce_lighting_apply(s_sr.renderer, &sun);
    } else {
        /* Fallback to legacy single-light. */
        JceDirLight sun = jce_dir_light_default();
        jce_lighting_apply(s_sr.renderer, &sun);
    }

    /* Apply render mode. */
    JceSceneViewMode view_mode = jce_state_get_view_mode();
    if (view_mode == JCE_VIEW_WIREFRAME)
        jce_renderer_set_wireframe(s_sr.renderer, true);

    for (int i = 0; i < count; i++) {
        JceEntityInfo *ent = jce_state_get_entity_by_index(i);
        if (!ent || !ent->enabled) continue;

        jce_mat4 model;
        JceMesh *mesh = NULL;
        const char *mat_path = NULL;
        if (!build_entity_model(ent, &model, &mesh, &mat_path)) continue;
        if (!mesh) continue;

        bgfx_set_transform(model.raw[0], 1);

        if (view_mode != JCE_VIEW_WIREFRAME) {
            /* Find the mesh renderer component for PBR data. */
            int cc = 0;
            JceComponentInfo *cs = jce_state_get_entity_components(ent->id, &cc);
            JceComponentInfo *mr_comp = NULL;
            for (int c = 0; c < cc; c++) {
                if (cs[c].type == JCE_COMP_MESH_RENDERER) {
                    mr_comp = &cs[c];
                    break;
                }
            }

            /* Check if entity has explicit PBR texture data configured. */
            bool has_pbr_textures = mr_comp
                && (mr_comp->data.mesh_renderer.albedo_tex[0]
                    || mr_comp->data.mesh_renderer.mr_tex[0]
                    || mr_comp->data.mesh_renderer.normal_tex[0]
                    || mr_comp->data.mesh_renderer.ao_tex[0]
                    || mr_comp->data.mesh_renderer.emissive_tex[0]);

            if (view_mode == JCE_VIEW_TEXTURED && has_pbr_textures) {
                /* Build a JcePbrMaterial from the component's inline PBR data. */
                JcePbrMaterial pbr = jce_pbr_material_default();
                /* Only override base_color if it looks explicitly set (alpha > 0). */
                if (mr_comp->data.mesh_renderer.base_color[3] > 0.0f) {
                    pbr.base_color_factor[0] = mr_comp->data.mesh_renderer.base_color[0];
                    pbr.base_color_factor[1] = mr_comp->data.mesh_renderer.base_color[1];
                    pbr.base_color_factor[2] = mr_comp->data.mesh_renderer.base_color[2];
                    pbr.base_color_factor[3] = mr_comp->data.mesh_renderer.base_color[3];
                }
                pbr.metallic_factor      = mr_comp->data.mesh_renderer.metallic;
                pbr.roughness_factor     = mr_comp->data.mesh_renderer.roughness;
                pbr.emissive_factor[0]   = mr_comp->data.mesh_renderer.emissive[0];
                pbr.emissive_factor[1]   = mr_comp->data.mesh_renderer.emissive[1];
                pbr.emissive_factor[2]   = mr_comp->data.mesh_renderer.emissive[2];
                pbr.normal_scale         = mr_comp->data.mesh_renderer.normal_scale;
                pbr.ao_strength          = mr_comp->data.mesh_renderer.ao_strength;
                pbr.alpha_mode           = (JceAlphaMode)mr_comp->data.mesh_renderer.alpha_mode;
                pbr.alpha_cutoff         = mr_comp->data.mesh_renderer.alpha_cutoff;
                pbr.double_sided         = mr_comp->data.mesh_renderer.double_sided;

                /* Try to load cached textures for each PBR slot. */
                if (mr_comp->data.mesh_renderer.albedo_tex[0]) {
                    JceTexture t = get_cached_texture(mr_comp->data.mesh_renderer.albedo_tex, NULL);
                    if (jce_texture_valid(t)) pbr.albedo_map = t;
                }
                /* Fallback: use legacy mat_path/mesh_path texture search for albedo. */
                if (!jce_texture_valid(pbr.albedo_map)) {
                    const char *mp = mr_comp->data.mesh_renderer.mesh_path;
                    JceTexture t = get_cached_texture(mat_path, mp);
                    if (jce_texture_valid(t)) pbr.albedo_map = t;
                }
                if (mr_comp->data.mesh_renderer.mr_tex[0]) {
                    JceTexture t = get_cached_texture(mr_comp->data.mesh_renderer.mr_tex, NULL);
                    if (jce_texture_valid(t)) pbr.metallic_roughness_map = t;
                }
                if (mr_comp->data.mesh_renderer.normal_tex[0]) {
                    JceTexture t = get_cached_texture(mr_comp->data.mesh_renderer.normal_tex, NULL);
                    if (jce_texture_valid(t)) pbr.normal_map = t;
                }
                if (mr_comp->data.mesh_renderer.ao_tex[0]) {
                    JceTexture t = get_cached_texture(mr_comp->data.mesh_renderer.ao_tex, NULL);
                    if (jce_texture_valid(t)) pbr.ao_map = t;
                }
                if (mr_comp->data.mesh_renderer.emissive_tex[0]) {
                    JceTexture t = get_cached_texture(mr_comp->data.mesh_renderer.emissive_tex, NULL);
                    if (jce_texture_valid(t)) pbr.emissive_map = t;
                }

                jce_pbr_material_bind(&pbr, s_sr.renderer, scene_view_id());

                /* Bind shadow map to texture stage 5 for PBR shader. */
                if (s_sr.shadow_valid) {
                    bgfx_set_texture(5, s_sr.u_shadowMap, s_sr.shadow_tex, UINT32_MAX);
                    float shadow_vp[16];
                    jce_vec3 shadow_dir = jce_v3(0.5f, 1.0f, 0.3f); /* default */
                    compute_shadow_vp(&shadow_dir, shadow_vp);
                    bgfx_set_uniform(s_sr.u_shadowVP, shadow_vp, 1);
                }

                jce_mesh_submit_pbr(mesh, s_sr.renderer, scene_view_id());
                continue; /* skip the default submit below */
            }

            /* SHADED or TEXTURED fallback: use basic mesh program with single texture. */
            bgfx_texture_handle_t bind_tex = s_sr.white_tex;

            if (view_mode == JCE_VIEW_TEXTURED) {
                const char *mp = mr_comp ? mr_comp->data.mesh_renderer.mesh_path : NULL;

                JceTexture tex = get_cached_texture(mat_path, mp);
                if (tex.idx != UINT16_MAX) {
                    bind_tex.idx = tex.idx;
                } else {
                    /* Graceful degradation: use magenta/black checkerboard
                     * so missing textures are visually obvious. */
                    bind_tex = s_sr.checker_tex;
                    /* Only warn for entities that have a non-empty mesh renderer
                     * path; container/group nodes with empty mat+mesh are silent. */
                    bool has_mat  = mat_path && mat_path[0] != '\0';
                    bool has_mesh = mp       && mp[0]       != '\0';
                    if (has_mat || has_mesh) {
                        /* Log once: suppress repeats via the tex cache miss entry. */
                        const char *key = has_mat ? mat_path : mp;
                        bool already = false;
                        for (int ci = 0; ci < s_sr.tex_cache_count; ci++) {
                            if (s_sr.tex_cache[ci].tried &&
                                strcmp(s_sr.tex_cache[ci].path, key) == 0) {
                                already = true; break;
                            }
                        }
                        if (!already)
                            LOG_WARN(LOG_TAG,
                                     "TEXTURED entity '%s': NO texture (mat='%s' mesh='%s')",
                                     ent->name,
                                     has_mat  ? mat_path : "",
                                     has_mesh ? mp       : "");
                    }
                }
            }

            JceUniformHandle uh = jce_renderer_get_tex_uniform(s_sr.renderer);
            bgfx_uniform_handle_t su = { uh.idx };
            bgfx_set_texture(0, su, bind_tex, UINT32_MAX);
        }

        jce_mesh_submit(mesh, s_sr.renderer, scene_view_id());
    }

    if (view_mode == JCE_VIEW_WIREFRAME)
        jce_renderer_set_wireframe(s_sr.renderer, false);

    /* Draw orange wireframe outlines for selected entities. */
    draw_selection_outlines();
}

/* ── Public API ────────────────────────────────────────────────────── */

bool jce_editor_scene_render_init(JceRenderer *renderer, const PakArchive *pak)
{
    if (s_sr.initialized) return true;

    memset(&s_sr, 0, sizeof(s_sr));
    mesh_async_start();
    texture_async_start();
    s_sr.white_tex.idx = UINT16_MAX;
    s_sr.checker_tex.idx = UINT16_MAX;
    s_sr.renderer = renderer;
    s_sr.bridge = jce_editor_render_bridge_create(renderer,
                                                  (uint16_t)JCE_VIEW_EDITOR_SCENE);
    if (!s_sr.bridge) {
        LOG_WARN(LOG_TAG, "failed to create editor render bridge");
        mesh_async_stop();
        texture_async_stop();
        return false;
    }

    /* Create the editor orbit camera. */
    JceCameraDesc cam_desc;
    memset(&cam_desc, 0, sizeof(cam_desc));
    cam_desc.mode       = JCE_CAMERA_PERSPECTIVE;
    cam_desc.position   = jce_v3(8.0f, 6.0f, 8.0f);
    cam_desc.target     = jce_v3(0.0f, 0.0f, 0.0f);
    cam_desc.up         = jce_v3(0.0f, 1.0f, 0.0f);
    cam_desc.fov_deg    = 45.0f;
    cam_desc.near_plane = 0.1f;
    cam_desc.far_plane  = 500.0f;

    s_sr.camera = jce_camera_create(&cam_desc);
    if (!s_sr.camera) {
        LOG_WARN(LOG_TAG, "failed to create editor camera");
        if (s_sr.bridge) {
            jce_editor_render_bridge_destroy(s_sr.bridge);
            s_sr.bridge = NULL;
        }
        mesh_async_stop();
        texture_async_stop();
        return false;
    }

    /* Initialize orbit state from the camera's initial position/target. */
    s_sr.orbit_target = jce_v3(0.0f, 0.0f, 0.0f);
    jce_vec3 cam_pos = jce_camera_get_position(s_sr.camera);
    jce_vec3 diff = jce_v3_sub(cam_pos, s_sr.orbit_target);
    s_sr.orbit_distance = jce_v3_len(diff);
    s_sr.orbit_yaw   = atan2f(diff.x, -diff.z);
    s_sr.orbit_pitch = asinf(diff.y / s_sr.orbit_distance);

    /* Pos + color vertex layout for transient buffers (grid, sky). */
    bgfx_vertex_layout_begin(&s_sr.layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&s_sr.layout, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&s_sr.layout, BGFX_ATTRIB_COLOR0, 4,
                           BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_end(&s_sr.layout);

    /* Cache the color shader program handle. */
    JceShaderHandle sh = jce_renderer_get_program_color(renderer);
    s_sr.prog_color.idx = sh.idx;

    /* Load sky/grid shader programs from the PAK archive. */
    JceShaderHandle sky_sh = shader_load_program(pak, "sky");
    s_sr.prog_sky.idx = sky_sh.idx;
    if (sky_sh.idx == UINT16_MAX)
        LOG_WARN(LOG_TAG, "sky shader not found in PAK — sky will be skipped");
    JceShaderHandle grid_sh = shader_load_program(pak, "grid");
    s_sr.prog_grid.idx = grid_sh.idx;
    if (grid_sh.idx == UINT16_MAX)
        LOG_WARN(LOG_TAG, "grid shader not found in PAK — grid will be skipped");

    /* Position-only vertex layout for the fullscreen sky quad. */
    bgfx_vertex_layout_begin(&s_sr.sky_layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&s_sr.sky_layout, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&s_sr.sky_layout);

    /* Uniforms for sky gradient and fullscreen grid. */
    s_sr.u_sky_colors = bgfx_create_uniform("u_sky_colors",
                                             BGFX_UNIFORM_TYPE_VEC4, 3);
    s_sr.u_grid_camera = bgfx_create_uniform("u_grid_camera",
                                             BGFX_UNIFORM_TYPE_VEC4, 1);
    s_sr.u_grid_fade = bgfx_create_uniform("u_grid_fade",
                                           BGFX_UNIFORM_TYPE_VEC4, 1);

    /* Cache lighting uniform handles for flat-color selection outlines.
     * bgfx_create_uniform with the same name returns a reference to the
     * same uniform, so these share handles with the renderer's copies. */
    s_sr.u_light_dir   = bgfx_create_uniform("u_lightDir",
                                              BGFX_UNIFORM_TYPE_VEC4, 1);
    s_sr.u_light_color = bgfx_create_uniform("u_lightColor",
                                              BGFX_UNIFORM_TYPE_VEC4, 1);

    /* Procedural meshes. */
    s_sr.cube_mesh     = jce_mesh_create_cube(1.0f);
    s_sr.plane_mesh    = jce_mesh_create_plane(1.0f, 1.0f, 0);
    s_sr.sphere_mesh   = jce_mesh_create_sphere(0.5f);
    s_sr.capsule_mesh  = jce_mesh_create_capsule(0.25f, 1.0f);
    s_sr.cylinder_mesh = jce_mesh_create_cylinder(0.5f, 1.0f);

    /* 1×1 white fallback texture for SHADED mode. */
    {
        uint32_t white = 0xFFFFFFFF;
        const bgfx_memory_t *mem = bgfx_copy(&white, 4);
        s_sr.white_tex = bgfx_create_texture_2d(1, 1, false, 1,
                                                  BGFX_TEXTURE_FORMAT_RGBA8, 0, mem);
    }

    /* 8×8 magenta/black checkerboard for missing textures (TEXTURED mode). */
    {
        const uint32_t M = 0xFFFF00FF; /* magenta (ABGR) */
        const uint32_t K = 0xFF000000; /* black   (ABGR) */
        uint32_t checker[8 * 8];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                checker[y * 8 + x] = ((x ^ y) & 1) ? K : M;
        const bgfx_memory_t *cmem = bgfx_copy(checker, sizeof(checker));
        s_sr.checker_tex = bgfx_create_texture_2d(8, 8, false, 1,
                                                    BGFX_TEXTURE_FORMAT_RGBA8,
                                                    BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
                                                    cmem);
    }

    /* ── Shadow map resources ──────────────────────────────────────── */
    {
        const uint16_t SHADOW_SIZE = 2048;
        s_sr.shadow_tex = bgfx_create_texture_2d(
            SHADOW_SIZE, SHADOW_SIZE, false, 1,
            BGFX_TEXTURE_FORMAT_D16,
            BGFX_TEXTURE_RT | BGFX_SAMPLER_COMPARE_LEQUAL
            | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
            NULL);
        bgfx_attachment_t at;
        memset(&at, 0, sizeof(at));
        bgfx_attachment_init(&at, s_sr.shadow_tex, BGFX_ACCESS_WRITE,
                             0, 1, 0, BGFX_RESOLVE_AUTO_GEN_MIPS);
        s_sr.shadow_fbo = bgfx_create_frame_buffer_from_attachment(1, &at, false);
        s_sr.u_shadowMap = bgfx_create_uniform("s_shadowMap",
                                                BGFX_UNIFORM_TYPE_SAMPLER, 1);
        s_sr.u_shadowVP  = bgfx_create_uniform("u_shadowVP",
                                                BGFX_UNIFORM_TYPE_MAT4, 1);
        s_sr.shadow_valid = BGFX_HANDLE_IS_VALID(s_sr.shadow_fbo);
        if (s_sr.shadow_valid)
            LOG_INFO(LOG_TAG, "shadow map created (%dx%d)", SHADOW_SIZE, SHADOW_SIZE);
    }

    /* ── Multi-light environment ──────────────────────────────────── */
    s_sr.light_env = jce_light_env_create();

    s_sr.initialized = true;
    LOG_INFO(LOG_TAG, "editor scene renderer initialized (FBO pipeline)");
    return true;
}

void jce_editor_scene_render_shutdown(void)
{
    if (!s_sr.initialized) return;

    mesh_async_stop();
    texture_async_stop();

    if (s_sr.bridge) {
        jce_editor_render_bridge_destroy(s_sr.bridge);
        s_sr.bridge = NULL;
    }

    /* Free cached meshes. */
    for (int i = 0; i < s_sr.mesh_cache_count; i++) {
        if (s_sr.mesh_cache[i].mesh)
            jce_mesh_destroy(s_sr.mesh_cache[i].mesh);
    }
    s_sr.mesh_cache_count = 0;

    /* Free cached textures. */
    for (int i = 0; i < s_sr.tex_cache_count; i++) {
        if (s_sr.tex_cache[i].tex.idx != UINT16_MAX)
            jce_texture_destroy(s_sr.tex_cache[i].tex);
    }
    s_sr.tex_cache_count = 0;

    if (s_sr.camera)     { jce_camera_destroy(s_sr.camera);   s_sr.camera = NULL; }
    if (s_sr.cube_mesh)     { jce_mesh_destroy(s_sr.cube_mesh);     s_sr.cube_mesh = NULL; }
    if (s_sr.plane_mesh)    { jce_mesh_destroy(s_sr.plane_mesh);    s_sr.plane_mesh = NULL; }
    if (s_sr.sphere_mesh)   { jce_mesh_destroy(s_sr.sphere_mesh);   s_sr.sphere_mesh = NULL; }
    if (s_sr.capsule_mesh)  { jce_mesh_destroy(s_sr.capsule_mesh);  s_sr.capsule_mesh = NULL; }
    if (s_sr.cylinder_mesh) { jce_mesh_destroy(s_sr.cylinder_mesh); s_sr.cylinder_mesh = NULL; }

    /* Destroy white fallback texture. */
    if (BGFX_HANDLE_IS_VALID(s_sr.white_tex))
        bgfx_destroy_texture(s_sr.white_tex);
    if (BGFX_HANDLE_IS_VALID(s_sr.checker_tex))
        bgfx_destroy_texture(s_sr.checker_tex);

    /* Destroy sky/grid shader resources owned by the scene renderer. */
    if (BGFX_HANDLE_IS_VALID(s_sr.prog_sky))
        bgfx_destroy_program(s_sr.prog_sky);
    if (BGFX_HANDLE_IS_VALID(s_sr.prog_grid))
        bgfx_destroy_program(s_sr.prog_grid);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_sky_colors))
        bgfx_destroy_uniform(s_sr.u_sky_colors);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_grid_camera))
        bgfx_destroy_uniform(s_sr.u_grid_camera);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_grid_fade))
        bgfx_destroy_uniform(s_sr.u_grid_fade);

    /* Destroy shadow map resources. */
    if (BGFX_HANDLE_IS_VALID(s_sr.shadow_fbo))
        bgfx_destroy_frame_buffer(s_sr.shadow_fbo);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_shadowMap))
        bgfx_destroy_uniform(s_sr.u_shadowMap);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_shadowVP))
        bgfx_destroy_uniform(s_sr.u_shadowVP);

    /* Destroy multi-light environment. */
    if (s_sr.light_env) {
        jce_light_env_destroy(s_sr.light_env);
        s_sr.light_env = NULL;
    }

    s_sr.initialized = false;
    LOG_INFO(LOG_TAG, "editor scene renderer shutdown");
}

void jce_editor_scene_render_frame(uint32_t width, uint32_t height)
{
    if (!s_sr.initialized || !s_sr.renderer) return;
    if (width == 0 || height == 0) return;

    /* Configure the scene view to render into the FBO. */
    const bgfx_caps_t *caps = bgfx_get_caps();
    float aspect = (float)width / (float)height;

    jce_mat4 view = jce_camera_view(s_sr.camera);
    jce_mat4 proj = jce_camera_proj(s_sr.camera, aspect, caps->homogeneousDepth);

    JceSceneViewMode view_mode = jce_state_get_view_mode();

    uint32_t clear_color = (view_mode == JCE_VIEW_WIREFRAME)
        ? 0x373737FF
        : BG_COLOR_RGBA;

    if (!jce_editor_render_bridge_prepare(
            s_sr.bridge,
            width,
            height,
            view.raw[0],
            proj.raw[0],
            clear_color,
            "EditorScene")) {
        return;
    }

    /* Main-thread GPU finalize for background-decoded meshes. */
    mesh_finalize_completed_loads();
    texture_finalize_completed_loads();

    /* Draw sky gradient (behind everything) — skip in wireframe mode. */
    if (view_mode != JCE_VIEW_WIREFRAME)
        draw_sky_gradient();

    /* Draw grid. */
    if (jce_state_get_show_grid()) {
        draw_grid();
    }

    /* Draw entities. */
    draw_entities();
}

uint16_t jce_editor_scene_render_get_texture(void)
{
    if (!s_sr.initialized || !s_sr.bridge)
        return UINT16_MAX;
    return jce_editor_render_bridge_get_color_texture(s_sr.bridge);
}

JceCamera *jce_editor_scene_get_camera(void)
{
    return s_sr.camera;
}

bool jce_editor_scene_get_camera_matrices(float *out_view16,
                                           float *out_proj16,
                                           float *out_eye3,
                                           float viewport_w,
                                           float viewport_h)
{
    if (!s_sr.initialized || !s_sr.camera) return false;

    float aspect = (viewport_h > 0.0f) ? viewport_w / viewport_h : 1.0f;
    jce_mat4 view = jce_camera_view(s_sr.camera);
    jce_mat4 proj = jce_camera_proj(s_sr.camera, aspect, false);
    memcpy(out_view16, JCE_M4_PTR(view), 16 * sizeof(float));
    memcpy(out_proj16, JCE_M4_PTR(proj), 16 * sizeof(float));

    jce_vec3 pos = jce_camera_get_position(s_sr.camera);
    out_eye3[0] = pos.x;
    out_eye3[1] = pos.y;
    out_eye3[2] = pos.z;
    return true;
}

/* ── Orbit camera helpers ──────────────────────────────────────────── */

#define ORBIT_PITCH_MAX  (89.0f * JCE_DEG2RAD)
#define ORBIT_DIST_MIN   0.5f
#define ORBIT_DIST_MAX   500.0f

static void orbit_apply(void)
{
    if (!s_sr.camera) return;

    /* Compute camera position on sphere around orbit_target. */
    float y = s_sr.orbit_pitch;
    float x = s_sr.orbit_yaw;
    float d = s_sr.orbit_distance;

    jce_vec3 pos;
    pos.x = s_sr.orbit_target.x + d * sinf(x) * cosf(y);
    pos.y = s_sr.orbit_target.y + d * sinf(y);
    pos.z = s_sr.orbit_target.z - d * cosf(x) * cosf(y);

    jce_camera_set_position(s_sr.camera, pos);
    jce_camera_look_at(s_sr.camera, s_sr.orbit_target);
}

void jce_editor_scene_camera_orbit(float dyaw, float dpitch)
{
    if (!s_sr.initialized) return;
    s_sr.orbit_yaw   += dyaw;
    s_sr.orbit_pitch += dpitch;
    if (s_sr.orbit_pitch >  ORBIT_PITCH_MAX) s_sr.orbit_pitch =  ORBIT_PITCH_MAX;
    if (s_sr.orbit_pitch < -ORBIT_PITCH_MAX) s_sr.orbit_pitch = -ORBIT_PITCH_MAX;
    orbit_apply();
}

void jce_editor_scene_camera_pan(float dx, float dy)
{
    if (!s_sr.initialized || !s_sr.camera) return;
    jce_vec3 right = jce_camera_get_right(s_sr.camera);
    jce_vec3 up    = jce_camera_get_up(s_sr.camera);

    /* Scale by distance so panning feels natural at all zoom levels. */
    float scale = s_sr.orbit_distance * 0.002f;
    jce_vec3 offset = jce_v3_add(
        jce_v3_scale(right, -dx * scale),
        jce_v3_scale(up,     dy * scale));

    s_sr.orbit_target = jce_v3_add(s_sr.orbit_target, offset);
    orbit_apply();
}

void jce_editor_scene_camera_zoom(float delta)
{
    if (!s_sr.initialized) return;
    s_sr.orbit_distance -= delta * s_sr.orbit_distance * 0.1f;
    if (s_sr.orbit_distance < ORBIT_DIST_MIN) s_sr.orbit_distance = ORBIT_DIST_MIN;
    if (s_sr.orbit_distance > ORBIT_DIST_MAX) s_sr.orbit_distance = ORBIT_DIST_MAX;
    orbit_apply();
}

void jce_editor_scene_camera_get_target(float *out3)
{
    if (out3) {
        out3[0] = s_sr.orbit_target.x;
        out3[1] = s_sr.orbit_target.y;
        out3[2] = s_sr.orbit_target.z;
    }
}

void jce_editor_scene_camera_set_target(float x, float y, float z)
{
    s_sr.orbit_target = jce_v3(x, y, z);
    if (s_sr.initialized) orbit_apply();
}

void jce_editor_scene_camera_snap_view(JceCamPresetView preset)
{
    if (!s_sr.initialized) return;

    switch (preset) {
    case JCE_CAM_VIEW_FRONT:   s_sr.orbit_yaw = 0;              s_sr.orbit_pitch = 0;   break;
    case JCE_CAM_VIEW_BACK:    s_sr.orbit_yaw = JCE_PI;         s_sr.orbit_pitch = 0;   break;
    case JCE_CAM_VIEW_LEFT:    s_sr.orbit_yaw = -JCE_PI * 0.5f; s_sr.orbit_pitch = 0;   break;
    case JCE_CAM_VIEW_RIGHT:   s_sr.orbit_yaw =  JCE_PI * 0.5f; s_sr.orbit_pitch = 0;   break;
    case JCE_CAM_VIEW_TOP:     s_sr.orbit_yaw = 0;              s_sr.orbit_pitch =  ORBIT_PITCH_MAX; break;
    case JCE_CAM_VIEW_BOTTOM:  s_sr.orbit_yaw = 0;              s_sr.orbit_pitch = -ORBIT_PITCH_MAX; break;
    }
    orbit_apply();
}

void jce_editor_scene_camera_reset(void)
{
    if (!s_sr.initialized) return;
    /* Restore default orbit: position (8,6,8) looking at (0,0,0). */
    s_sr.orbit_target   = jce_v3(0.0f, 0.0f, 0.0f);
    s_sr.orbit_distance = sqrtf(8.0f*8.0f + 6.0f*6.0f + 8.0f*8.0f); /* ~12.2 */
    s_sr.orbit_yaw      = atan2f(8.0f, -8.0f); /* 135° → F/T/R quadrant (+X,+Y,+Z) */
    s_sr.orbit_pitch    = asinf(6.0f / s_sr.orbit_distance);
    orbit_apply();
}

void jce_editor_scene_set_scene_dir(const char *dir)
{
    mesh_async_begin_new_generation();
    texture_async_begin_new_generation();

    if (dir)
        snprintf(s_sr.scene_dir, sizeof(s_sr.scene_dir), "%s", dir);
    else
        s_sr.scene_dir[0] = '\0';

    /* Clear mesh and texture caches when scene directory changes. */
    for (int i = 0; i < s_sr.mesh_cache_count; i++) {
        if (s_sr.mesh_cache[i].mesh)
            jce_mesh_destroy(s_sr.mesh_cache[i].mesh);
    }
    s_sr.mesh_cache_count = 0;

    for (int i = 0; i < s_sr.tex_cache_count; i++) {
        if (s_sr.tex_cache[i].tex.idx != UINT16_MAX)
            jce_texture_destroy(s_sr.tex_cache[i].tex);
    }
    s_sr.tex_cache_count = 0;
}
