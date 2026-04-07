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

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

extern "C" {
#include <cjson/cJSON.h>
#include <jce/graphics/jce_renderer.h>
#include <jce/graphics/jce_shaders.h>
#include <jce/graphics/jce_camera.h>
#include <jce/graphics/jce_views.h>
#include <jce/graphics/jce_mesh.h>
#include <jce/graphics/jce_material.h>
#include <jce/graphics/jce_texture.h>
#include <jce/graphics/jce_lighting.h>
#include <jce/resource/pak_loader.h>
#include <jce/core/jce_math.h>
#include <jce/core/jce_log.h>
}

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#define LOG_TAG "scene_render"

namespace fs = std::filesystem;

/* ── Scene view ID (dedicated editor view, after engine views) ─────── */

#define SCENE_VIEW_ID  3

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
    JceCamera              *camera;
    bgfx_vertex_layout_t    layout;
    bgfx_program_handle_t   prog_color;

    /* Sky shader resources. */
    bgfx_program_handle_t   prog_sky;
    bgfx_vertex_layout_t    sky_layout;
    bgfx_uniform_handle_t   u_sky_colors;

    /* Framebuffer (render-to-texture). */
    bgfx_frame_buffer_handle_t fbo;
    bgfx_texture_handle_t      fbo_color;
    uint32_t                   fbo_w, fbo_h;

    /* Procedural meshes for entity placeholders. */
    JceMesh                *cube_mesh;
    JceMesh                *plane_mesh;

    /* Orbit camera state (Maya-style). */
    jce_vec3                orbit_target;
    float                   orbit_distance;
    float                   orbit_yaw;    /* radians */
    float                   orbit_pitch;  /* radians */

    /* Mesh cache for loaded models (path → JceMesh*). */
    struct {
        char     path[128];
        JceMesh *mesh;
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
    } tex_cache[256];
    int tex_cache_count;

    /* Scene base directory for resolving mesh paths. */
    char scene_dir[512];

    /* Cached lighting uniform handles for flat-color selection outlines. */
    bgfx_uniform_handle_t  u_light_dir;
    bgfx_uniform_handle_t  u_light_color;
} s_sr;

/* ── Helpers ────────────────────────────────────────────────────────── */

static uint32_t pack_abgr(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)a << 24) | ((uint32_t)b << 16)
         | ((uint32_t)g << 8)  |  (uint32_t)r;
}

/* ── FBO management ────────────────────────────────────────────────── */

static void destroy_fbo(void)
{
    if (s_sr.fbo_w == 0 && s_sr.fbo_h == 0) return;
    if (BGFX_HANDLE_IS_VALID(s_sr.fbo))
        bgfx_destroy_frame_buffer(s_sr.fbo);
    s_sr.fbo.idx = UINT16_MAX;
    s_sr.fbo_color.idx = UINT16_MAX;
    s_sr.fbo_w = s_sr.fbo_h = 0;
}

static bool ensure_fbo(uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0) return false;
    if (s_sr.fbo_w == w && s_sr.fbo_h == h && BGFX_HANDLE_IS_VALID(s_sr.fbo))
        return true;

    destroy_fbo();

    /* Create two textures: color (RGBA8) + depth (D24S8). */
    bgfx_texture_handle_t textures[2];
    textures[0] = bgfx_create_texture_2d(
        (uint16_t)w, (uint16_t)h, false, 1,
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
            | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
        NULL);

    textures[1] = bgfx_create_texture_2d(
        (uint16_t)w, (uint16_t)h, false, 1,
        BGFX_TEXTURE_FORMAT_D24S8,
        BGFX_TEXTURE_RT,
        NULL);

    bgfx_attachment_t attachments[2];
    memset(attachments, 0, sizeof(attachments));
    bgfx_attachment_init(&attachments[0], textures[0], BGFX_ACCESS_WRITE, 0, 1, 0, BGFX_RESOLVE_AUTO_GEN_MIPS);
    bgfx_attachment_init(&attachments[1], textures[1], BGFX_ACCESS_WRITE, 0, 1, 0, BGFX_RESOLVE_AUTO_GEN_MIPS);

    s_sr.fbo = bgfx_create_frame_buffer_from_attachment(2, attachments, true);
    if (!BGFX_HANDLE_IS_VALID(s_sr.fbo)) {
        LOG_WARN(LOG_TAG, "failed to create FBO %ux%u", w, h);
        return false;
    }

    /* Get the color texture handle from the FBO (attachment 0). */
    s_sr.fbo_color = bgfx_get_texture(s_sr.fbo, 0);
    s_sr.fbo_w = w;
    s_sr.fbo_h = h;
    LOG_INFO(LOG_TAG, "created FBO %ux%u", w, h);
    return true;
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
    bgfx_submit(SCENE_VIEW_ID, s_sr.prog_sky, 0, BGFX_DISCARD_ALL);
}

/* ── Grid Rendering (with distance fog fade like Blender) ──────────── */

/* Fade grid line alpha based on distance from camera (XZ plane).
 * Lines close to the camera are fully opaque; distant lines fade out. */
static uint32_t fade_grid_color(uint8_t r, uint8_t g, uint8_t b, uint8_t base_a,
                                 float line_coord, float cam_xz, float fade_start,
                                 float fade_end)
{
    float dist = fabsf(line_coord - cam_xz);
    float t = 1.0f;
    if (dist > fade_start) {
        t = 1.0f - (dist - fade_start) / (fade_end - fade_start);
        if (t < 0.0f) t = 0.0f;
    }
    uint8_t a = (uint8_t)(base_a * t);
    return pack_abgr(r, g, b, a);
}

static void draw_grid(void)
{
    const int half_extent = 100;
    const int step_minor  = 1;
    const int step_major  = 5;

    /* Fog/fade parameters: lines start fading at fade_start distance from camera,
     * fully transparent at fade_end. Wider range prevents grid from disappearing
     * at oblique camera angles. */
    const float fade_start = 35.0f;
    const float fade_end   = 95.0f;

    jce_vec3 cam_pos = jce_camera_get_position(s_sr.camera);

    /* +2 for the center axis lines (x=0 and z=0), guaranteed even if step
     * doesn't land exactly on 0.  (With step_minor=1 they already do, but
     * this makes the logic explicit.) */
    int lines_per_axis = (2 * half_extent / step_minor) + 1;
    int total_lines    = lines_per_axis * 2;
    int total_verts    = total_lines * 2;
    int total_indices  = total_lines * 2;

    if (total_verts > 65535) return;

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;

    if (!bgfx_alloc_transient_buffers(&tvb, &s_sr.layout, (uint32_t)total_verts,
                                      &tib, (uint32_t)total_indices, false))
        return;

    PosColorVertex *verts = (PosColorVertex *)tvb.data;
    uint16_t       *idx   = (uint16_t *)tib.data;
    int vi = 0, ii = 0;

    float extent = (float)half_extent;

    /* X-parallel lines (varying xi, line runs along Z). */
    for (int xi = -half_extent; xi <= half_extent; xi += step_minor) {
        uint8_t r, g, b, base_a;
        if (xi == 0) {
            /* Center X axis — red, higher alpha. */
            r = 200; g = 60; b = 60; base_a = 255;
        } else if (xi % step_major == 0) {
            r = 120; g = 120; b = 120; base_a = 255;
        } else {
            r = 80; g = 80; b = 80; base_a = 200;
        }

        /* Fade both endpoints by their Z-distance from camera. */
        uint32_t c0 = fade_grid_color(r, g, b, base_a, -extent, cam_pos.z,
                                      fade_start, fade_end);
        uint32_t c1 = fade_grid_color(r, g, b, base_a,  extent, cam_pos.z,
                                      fade_start, fade_end);

        /* Also fade by X-distance from camera (line's lateral distance). */
        float x_dist = fabsf((float)xi - cam_pos.x);
        float x_fade = 1.0f;
        if (x_dist > fade_start) {
            x_fade = 1.0f - (x_dist - fade_start) / (fade_end - fade_start);
            if (x_fade < 0.0f) x_fade = 0.0f;
        }

        /* Apply lateral fade to both endpoints. */
        {
            uint8_t a0 = (uint8_t)((c0 >> 24) * x_fade);
            c0 = (c0 & 0x00FFFFFF) | ((uint32_t)a0 << 24);
            uint8_t a1 = (uint8_t)((c1 >> 24) * x_fade);
            c1 = (c1 & 0x00FFFFFF) | ((uint32_t)a1 << 24);
        }

        verts[vi] = { (float)xi, 0.0f, -extent, c0 };
        idx[ii++] = (uint16_t)vi++;
        verts[vi] = { (float)xi, 0.0f,  extent, c1 };
        idx[ii++] = (uint16_t)vi++;
    }

    /* Z-parallel lines (varying zi, line runs along X). */
    for (int zi = -half_extent; zi <= half_extent; zi += step_minor) {
        uint8_t r, g, b, base_a;
        if (zi == 0) {
            /* Center Z axis — blue, higher alpha. */
            r = 60; g = 60; b = 200; base_a = 255;
        } else if (zi % step_major == 0) {
            r = 120; g = 120; b = 120; base_a = 255;
        } else {
            r = 80; g = 80; b = 80; base_a = 200;
        }

        /* Fade both endpoints by their X-distance from camera. */
        uint32_t c0 = fade_grid_color(r, g, b, base_a, -extent, cam_pos.x,
                                      fade_start, fade_end);
        uint32_t c1 = fade_grid_color(r, g, b, base_a,  extent, cam_pos.x,
                                      fade_start, fade_end);

        /* Also fade by Z-distance from camera (line's lateral distance). */
        float z_dist = fabsf((float)zi - cam_pos.z);
        float z_fade = 1.0f;
        if (z_dist > fade_start) {
            z_fade = 1.0f - (z_dist - fade_start) / (fade_end - fade_start);
            if (z_fade < 0.0f) z_fade = 0.0f;
        }

        {
            uint8_t a0 = (uint8_t)((c0 >> 24) * z_fade);
            c0 = (c0 & 0x00FFFFFF) | ((uint32_t)a0 << 24);
            uint8_t a1 = (uint8_t)((c1 >> 24) * z_fade);
            c1 = (c1 & 0x00FFFFFF) | ((uint32_t)a1 << 24);
        }

        verts[vi] = { -extent, 0.0f, (float)zi, c0 };
        idx[ii++] = (uint16_t)vi++;
        verts[vi] = {  extent, 0.0f, (float)zi, c1 };
        idx[ii++] = (uint16_t)vi++;
    }

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, (uint32_t)vi);
    bgfx_set_transient_index_buffer(&tib, 0, (uint32_t)ii);

    uint64_t state = BGFX_STATE_WRITE_RGB
                   | BGFX_STATE_WRITE_A
                   | BGFX_STATE_WRITE_Z
                   | BGFX_STATE_DEPTH_TEST_LESS
                   | BGFX_STATE_MSAA
                   | BGFX_STATE_PT_LINES
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                            BGFX_STATE_BLEND_INV_SRC_ALPHA);
    bgfx_set_state(state, 0);

    jce_mat4 identity = jce_m4_identity();
    bgfx_set_transform(identity.raw[0], 1);
    bgfx_submit(SCENE_VIEW_ID, s_sr.prog_color, 0, BGFX_DISCARD_ALL);
}

/* ── Mesh cache + resolution ────────────────────────────────────────── */

extern "C" JceMesh *jce_editor_model_load_file(const char *file_path);

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

/* Search for a mesh file matching mesh_path and load it.
 * mesh_path is like "Meshes/tree-scary-dead.obj".
 * Returns the loaded mesh, or NULL on failure. */
static JceMesh *resolve_and_load_mesh(const char *mesh_path)
{
    char found_path[512];
    if (!resolve_mesh_file_path(mesh_path, found_path, sizeof(found_path))) {
        LOG_WARN(LOG_TAG, "mesh not found: %s (under %s)",
                 mesh_path ? mesh_path : "<null>", s_sr.scene_dir);
        return NULL;
    }
    return jce_editor_model_load_file(found_path);
}

/* Find or load a mesh by its scene-relative path. */
static JceMesh *get_cached_mesh(const char *mesh_path)
{
    if (!mesh_path || mesh_path[0] == '\0') return NULL;

    /* Check cache. */
    for (int i = 0; i < s_sr.mesh_cache_count; i++) {
        if (strcmp(s_sr.mesh_cache[i].path, mesh_path) == 0)
            return s_sr.mesh_cache[i].mesh; /* may be NULL if load failed */
    }

    /* Not in cache — try to load. */
    JceMesh *mesh = resolve_and_load_mesh(mesh_path);

    /* Store in cache (even NULL to avoid re-trying). */
    if (s_sr.mesh_cache_count < 512) {
        snprintf(s_sr.mesh_cache[s_sr.mesh_cache_count].path,
                 sizeof(s_sr.mesh_cache[0].path), "%s", mesh_path);
        s_sr.mesh_cache[s_sr.mesh_cache_count].mesh = mesh;
        s_sr.mesh_cache_count++;
    }

    return mesh;
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

static bool try_load_texture_path(const fs::path &path, JceTexture *out_tex)
{
    if (!out_tex || !path_is_file(path)) return false;

    SDL_IOStream *io = SDL_IOFromFile(path.string().c_str(), "rb");
    if (!io) return false;

    SDL_Surface *surf = IMG_Load_IO(io, true);
    if (!surf) return false;

    if (surf->format != SDL_PIXELFORMAT_RGBA32) {
        SDL_Surface *conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(surf);
        surf = conv;
    }
    if (!surf) return false;

    JceTexture tex = jce_texture_load_from_surface(surf, JCE_TEX_WRAP);
    SDL_DestroySurface(surf);
    if (tex.idx == UINT16_MAX) return false;

    *out_tex = tex;
    LOG_DEBUG(LOG_TAG, "loaded texture: %s", path.string().c_str());
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

static bool try_load_texture_from_material_json(const char *material_path,
                                                JceTexture *out_tex)
{
    if (!out_tex) return false;

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
            loaded = try_load_texture_path(tex_path, out_tex);
        } else {
            /* Priority: material dir -> parent of material dir -> scene roots -> filename search. */
            loaded = try_load_texture_path(mat_file.parent_path() / tex_path, out_tex);
            if (!loaded) {
                fs::path mat_parent = mat_file.parent_path().parent_path();
                if (!mat_parent.empty())
                    loaded = try_load_texture_path(mat_parent / tex_path, out_tex);
            }
            if (!loaded) {
                std::vector<fs::path> roots = collect_scene_roots();
                for (const fs::path &root_dir : roots) {
                    if (try_load_texture_path(root_dir / tex_path, out_tex)) {
                        loaded = true;
                        break;
                    }
                }
            }
            if (!loaded) {
                fs::path by_name;
                std::vector<fs::path> roots = collect_scene_roots();
                if (find_file_by_name_recursive(roots, tex_path.filename().string(), 8, &by_name))
                    loaded = try_load_texture_path(by_name, out_tex);
            }
        }
    }

    cJSON_Delete(root);
    return loaded;
}

static bool try_load_texture_from_obj_mtl(const char *mesh_path, JceTexture *out_tex)
{
    if (!mesh_path || !out_tex) return false;

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
            if (try_load_texture_path(tex_path, out_tex))
                return true;

            fs::path by_name;
            std::vector<fs::path> roots = collect_scene_roots();
            if (find_file_by_name_recursive(roots, fs::path(tex_ref).filename().string(), 8, &by_name)) {
                if (try_load_texture_path(by_name, out_tex))
                    return true;
            }
        }
    }

    return false;
}

/* Try to load a texture from disk (not from PAK).
 * Resolution chain (matching Java reference intent):
 *  1) material_path -> .mat/.mat.json -> properties.albedoMap
 *  2) mesh_path OBJ -> mtllib -> map_Kd
 *  3) scan Materials/ dir for .mat.json files with albedoMap
 *  4) recursive filename fallback by mesh/material basename */
static JceTexture resolve_texture_for_material(const char *material_path,
                                                const char *mesh_path)
{
    if (s_sr.scene_dir[0] == '\0') return tex_invalid();

    LOG_INFO(LOG_TAG, "resolve_texture: mat='%s' mesh='%s' scene_dir='%s'",
             material_path ? material_path : "<null>",
             mesh_path ? mesh_path : "<null>",
             s_sr.scene_dir);

    JceTexture tex = tex_invalid();

    if (material_path && material_path[0] != '\0') {
        if (try_load_texture_from_material_json(material_path, &tex)) {
            LOG_INFO(LOG_TAG, "texture loaded via material JSON: %s", material_path);
            return tex;
        }
        LOG_DEBUG(LOG_TAG, "  material JSON path failed for '%s'", material_path);
    }

    if (mesh_path && mesh_path[0] != '\0') {
        if (try_load_texture_from_obj_mtl(mesh_path, &tex)) {
            LOG_INFO(LOG_TAG, "texture loaded via OBJ/MTL: %s", mesh_path);
            return tex;
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
            if (try_load_texture_from_material_json(mc.c_str(), &tex)) {
                LOG_INFO(LOG_TAG, "texture loaded via Materials/ scan: %s",
                         mc.c_str());
                return tex;
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
                        if (try_load_texture_path(it->path(), &tex))
                            return tex;
                    }
                }
            }
        }
    }

    return tex_invalid();
}

/* Get or load a cached texture for a material/mesh pair. */
static JceTexture get_cached_texture(const char *material_path,
                                      const char *mesh_path)
{
    /* Build a cache key from material_path (or mesh_path if no material). */
    const char *key = (material_path && material_path[0] != '\0')
                    ? material_path : mesh_path;
    if (!key || key[0] == '\0') return tex_invalid();

    for (int i = 0; i < s_sr.tex_cache_count; i++) {
        if (strcmp(s_sr.tex_cache[i].path, key) == 0)
            return s_sr.tex_cache[i].tex;
    }

    JceTexture tex = resolve_texture_for_material(material_path, mesh_path);

    if (tex.idx == UINT16_MAX) {
        LOG_WARN(LOG_TAG, "tex cache: MISS (no texture found) key='%s' mat='%s' mesh='%s'",
                 key, material_path ? material_path : "<null>",
                 mesh_path ? mesh_path : "<null>");
    } else {
        LOG_INFO(LOG_TAG, "tex cache: loaded texture key='%s' idx=%u",
                key, (unsigned)tex.idx);
    }

    if (s_sr.tex_cache_count < 256) {
        snprintf(s_sr.tex_cache[s_sr.tex_cache_count].path,
                 sizeof(s_sr.tex_cache[0].path), "%s", key);
        s_sr.tex_cache[s_sr.tex_cache_count].tex = tex;
        s_sr.tex_cache[s_sr.tex_cache_count].tried = true;
        s_sr.tex_cache_count++;
    }

    return tex;
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
        if (has_mesh && mesh_path && mesh_path[0] != '\0') {
            *out_mesh = get_cached_mesh(mesh_path);
            if (!*out_mesh) *out_mesh = s_sr.cube_mesh;
        } else {
            *out_mesh = s_sr.cube_mesh;
        }
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

        jce_mesh_submit_wireframe_overlay(mesh, s_sr.renderer, SCENE_VIEW_ID);
    }

    /* Restore normal lighting. */
    JceDirLight sun = jce_dir_light_default();
    jce_lighting_apply(s_sr.renderer, &sun);
}

static void draw_entities(void)
{
    int count = jce_state_get_entity_count();
    if (count == 0) return;

    JceDirLight sun = jce_dir_light_default();
    jce_lighting_apply(s_sr.renderer, &sun);

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
            /* Both SHADED and TEXTURED use program_mesh which samples s_texColor.
             * SHADED → always bind 1×1 white fallback (lit shading only).
             * TEXTURED → bind actual texture if found, white fallback otherwise. */
            bgfx_texture_handle_t bind_tex = s_sr.white_tex;

            if (view_mode == JCE_VIEW_TEXTURED) {
                /* Try to find an actual diffuse texture for this entity. */
                const char *mp = NULL;
                int cc = 0;
                JceComponentInfo *cs = jce_state_get_entity_components(ent->id, &cc);
                for (int c = 0; c < cc; c++)
                    if (cs[c].type == JCE_COMP_MESH_RENDERER)
                        { mp = cs[c].data.mesh_renderer.mesh_path; break; }

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

        jce_mesh_submit(mesh, s_sr.renderer, SCENE_VIEW_ID);
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
    s_sr.fbo.idx = UINT16_MAX;
    s_sr.fbo_color.idx = UINT16_MAX;
    s_sr.white_tex.idx = UINT16_MAX;
    s_sr.checker_tex.idx = UINT16_MAX;
    s_sr.renderer = renderer;

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

    /* Load sky shader program from the PAK archive. */
    JceShaderHandle sky_sh = shader_load_program(pak, "sky");
    s_sr.prog_sky.idx = sky_sh.idx;
    if (sky_sh.idx == UINT16_MAX)
        LOG_WARN(LOG_TAG, "sky shader not found in PAK — sky will be skipped");

    /* Position-only vertex layout for the fullscreen sky quad. */
    bgfx_vertex_layout_begin(&s_sr.sky_layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&s_sr.sky_layout, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&s_sr.sky_layout);

    /* Uniform for sky gradient colors (top / horizon / ground). */
    s_sr.u_sky_colors = bgfx_create_uniform("u_sky_colors",
                                             BGFX_UNIFORM_TYPE_VEC4, 3);

    /* Cache lighting uniform handles for flat-color selection outlines.
     * bgfx_create_uniform with the same name returns a reference to the
     * same uniform, so these share handles with the renderer's copies. */
    s_sr.u_light_dir   = bgfx_create_uniform("u_lightDir",
                                              BGFX_UNIFORM_TYPE_VEC4, 1);
    s_sr.u_light_color = bgfx_create_uniform("u_lightColor",
                                              BGFX_UNIFORM_TYPE_VEC4, 1);

    /* Procedural meshes. */
    s_sr.cube_mesh  = jce_mesh_create_cube(1.0f);
    s_sr.plane_mesh = jce_mesh_create_plane(10.0f, 10.0f, 0);

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

    s_sr.initialized = true;
    LOG_INFO(LOG_TAG, "editor scene renderer initialized (FBO pipeline)");
    return true;
}

void jce_editor_scene_render_shutdown(void)
{
    if (!s_sr.initialized) return;

    destroy_fbo();

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
    if (s_sr.cube_mesh)  { jce_mesh_destroy(s_sr.cube_mesh);  s_sr.cube_mesh = NULL; }
    if (s_sr.plane_mesh) { jce_mesh_destroy(s_sr.plane_mesh); s_sr.plane_mesh = NULL; }

    /* Destroy white fallback texture. */
    if (BGFX_HANDLE_IS_VALID(s_sr.white_tex))
        bgfx_destroy_texture(s_sr.white_tex);
    if (BGFX_HANDLE_IS_VALID(s_sr.checker_tex))
        bgfx_destroy_texture(s_sr.checker_tex);

    /* Destroy sky shader resources owned by the scene renderer. */
    if (BGFX_HANDLE_IS_VALID(s_sr.prog_sky))
        bgfx_destroy_program(s_sr.prog_sky);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_sky_colors))
        bgfx_destroy_uniform(s_sr.u_sky_colors);

    s_sr.initialized = false;
    LOG_INFO(LOG_TAG, "editor scene renderer shutdown");
}

void jce_editor_scene_render_frame(uint32_t width, uint32_t height)
{
    if (!s_sr.initialized || !s_sr.renderer) return;
    if (width == 0 || height == 0) return;

    /* Ensure FBO matches the requested size. */
    if (!ensure_fbo(width, height)) return;

    /* Configure the scene view to render into the FBO. */
    const bgfx_caps_t *caps = bgfx_get_caps();
    float aspect = (float)width / (float)height;

    jce_mat4 view = jce_camera_view(s_sr.camera);
    jce_mat4 proj = jce_camera_proj(s_sr.camera, aspect, caps->homogeneousDepth);

    JceSceneViewMode view_mode = jce_state_get_view_mode();

    bgfx_set_view_name(SCENE_VIEW_ID, "EditorScene", INT32_MAX);
    bgfx_set_view_rect(SCENE_VIEW_ID, 0, 0, (uint16_t)width, (uint16_t)height);

    /* In wireframe mode, use a gray clear color (matching Java reference:
     * groundColor rgb(55, 55, 55) ≈ #373737) and skip the sky gradient.
     * In shaded/textured modes, use the sky-blue clear color + gradient. */
    if (view_mode == JCE_VIEW_WIREFRAME) {
        bgfx_set_view_clear(SCENE_VIEW_ID,
                            BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                            0x373737FF, 1.0f, 0);
    } else {
        bgfx_set_view_clear(SCENE_VIEW_ID,
                            BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                            BG_COLOR_RGBA, 1.0f, 0);
    }

    bgfx_set_view_transform(SCENE_VIEW_ID, view.raw[0], proj.raw[0]);
    bgfx_set_view_frame_buffer(SCENE_VIEW_ID, s_sr.fbo);
    bgfx_set_view_mode(SCENE_VIEW_ID, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_touch(SCENE_VIEW_ID);

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
    if (!s_sr.initialized || !BGFX_HANDLE_IS_VALID(s_sr.fbo))
        return UINT16_MAX;
    return s_sr.fbo_color.idx;
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
