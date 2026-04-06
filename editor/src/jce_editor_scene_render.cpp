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

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

extern "C" {
#include <jce/graphics/jce_renderer.h>
#include <jce/graphics/jce_camera.h>
#include <jce/graphics/jce_views.h>
#include <jce/graphics/jce_mesh.h>
#include <jce/graphics/jce_lighting.h>
#include <jce/core/jce_math.h>
#include <jce/core/jce_log.h>
}

#define LOG_TAG "scene_render"

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

    /* Scene base directory for resolving mesh paths. */
    char scene_dir[512];
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

/* ── Sky gradient (fullscreen quad — Unity-like sky colors) ─────────── */

static void draw_sky_gradient(void)
{
    /* Use 8 vertices to create a 3-band sky:
     *   top band:    deep cornflower blue (Unity zenith)
     *   mid band:    lighter mid-sky blue
     *   horizon:     pale sky / light haze
     *   below ground: dark (matches dark bg, never visible)
     * Bands are rendered as a large world-space quad placed behind everything.
     */
    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;

    if (!bgfx_alloc_transient_buffers(&tvb, &s_sr.layout, 8, &tib, 18, false))
        return;

    PosColorVertex *v = (PosColorVertex *)tvb.data;
    uint16_t       *ix = (uint16_t *)tib.data;

    /* Unity-like sky colors: */
    uint32_t col_zenith  = pack_abgr( 54,  95, 160, 255); /* deep cornflower */
    uint32_t col_midsky  = pack_abgr( 74, 130, 195, 255); /* mid blue        */
    uint32_t col_horizon = pack_abgr(165, 190, 220, 255); /* pale haze       */
    uint32_t col_ground  = pack_abgr( 52,  58,  65, 255); /* dark ground     */

    jce_vec3 cam_pos = jce_camera_get_position(s_sr.camera);
    float d = 450.0f;  /* half-extent */

    /* Row 0 (top/zenith)   y=200 */
    v[0] = { cam_pos.x - d, 200.0f, cam_pos.z - d, col_zenith };
    v[1] = { cam_pos.x + d, 200.0f, cam_pos.z - d, col_zenith };
    v[2] = { cam_pos.x + d, 200.0f, cam_pos.z + d, col_zenith };
    v[3] = { cam_pos.x - d, 200.0f, cam_pos.z + d, col_zenith };

    /* Row 1 (mid-sky)      y=60 */
    v[4] = { cam_pos.x - d,  60.0f, cam_pos.z - d, col_midsky };
    v[5] = { cam_pos.x + d,  60.0f, cam_pos.z - d, col_midsky };
    v[6] = { cam_pos.x + d,  60.0f, cam_pos.z + d, col_midsky };
    v[7] = { cam_pos.x - d,  60.0f, cam_pos.z + d, col_midsky };

    /* For the two bands below we'll reuse some verts:
     * horizon is at y=0 (ground plane level),
     * ground  is at y=-20 (below horizon). */

    /* Row 4 quads: zenith→midsky, midsky→horizon, horizon→ground using
     * AddQuad approach via triangles. */

    /* zenith → midsky band: quads using v0-v3 (top) and v4-v7 (mid) */
    /* Front face (z-) */
    ix[0] = 0; ix[1] = 1; ix[2] = 5;
    ix[3] = 0; ix[4] = 5; ix[5] = 4;
    /* Back face (z+) */
    ix[6] = 3; ix[7] = 7; ix[8] = 6;
    ix[9] = 3; ix[10]= 6; ix[11]= 2;
    /* Left face (x-) */
    ix[12]= 0; ix[13]= 4; ix[14]= 7;
    ix[15]= 0; ix[16]= 7; ix[17]= 3;

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, 8);
    bgfx_set_transient_index_buffer(&tib, 0, 18);

    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA
                   | BGFX_STATE_CULL_CW;
    bgfx_set_state(state, 0);
    jce_mat4 identity = jce_m4_identity();
    bgfx_set_transform(identity.raw[0], 1);
    bgfx_submit(SCENE_VIEW_ID, s_sr.prog_color, 0, BGFX_DISCARD_ALL);

    /* Second draw: a very large ground quad for the lower half with horizon→ground grad. */
    bgfx_transient_vertex_buffer_t tvb2;
    bgfx_transient_index_buffer_t  tib2;
    if (!bgfx_alloc_transient_buffers(&tvb2, &s_sr.layout, 4, &tib2, 6, false))
        return;

    PosColorVertex *v2 = (PosColorVertex *)tvb2.data;
    uint16_t       *ix2 = (uint16_t *)tib2.data;

    float d2 = 450.0f;
    v2[0] = { cam_pos.x - d2,  62.0f, cam_pos.z - d2, col_horizon };
    v2[1] = { cam_pos.x + d2,  62.0f, cam_pos.z - d2, col_horizon };
    v2[2] = { cam_pos.x + d2, -20.0f, cam_pos.z + d2, col_ground  };
    v2[3] = { cam_pos.x - d2, -20.0f, cam_pos.z + d2, col_ground  };

    ix2[0] = 0; ix2[1] = 1; ix2[2] = 2;
    ix2[3] = 0; ix2[4] = 2; ix2[5] = 3;

    bgfx_set_transient_vertex_buffer(0, &tvb2, 0, 4);
    bgfx_set_transient_index_buffer(&tib2, 0, 6);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA, 0);
    bgfx_set_transform(identity.raw[0], 1);
    bgfx_submit(SCENE_VIEW_ID, s_sr.prog_color, 0, BGFX_DISCARD_ALL);
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
    const int half_extent = 50;
    const int step_minor  = 1;
    const int step_major  = 5;

    /* Fog/fade parameters: lines start fading at fade_start distance from camera,
     * fully transparent at fade_end. This mimics Blender's infinite grid fog. */
    const float fade_start = 20.0f;
    const float fade_end   = 48.0f;

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

/* Search for an OBJ file matching the mesh path on disk.
 * mesh_path is like "Meshes/tree-scary-dead.obj".
 * Returns the loaded mesh, or NULL on failure. */
static JceMesh *resolve_and_load_mesh(const char *mesh_path)
{
    if (!mesh_path || mesh_path[0] == '\0') return NULL;
    if (s_sr.scene_dir[0] == '\0') return NULL;

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
            return jce_editor_model_load_file(full_path);
        }
    }

    /* Dynamically scan ALL subdirectories under scene_dir/Meshes/. */
    char meshes_dir[512];
    snprintf(meshes_dir, sizeof(meshes_dir), "%s\\Meshes", s_sr.scene_dir);

#ifdef _WIN32
    /* Enumerate subdirectories. */
    WIN32_FIND_DATAA dir_fd;
    char dir_pattern[512];
    snprintf(dir_pattern, sizeof(dir_pattern), "%s\\*", meshes_dir);
    HANDLE hDir = FindFirstFileA(dir_pattern, &dir_fd);
    if (hDir == INVALID_HANDLE_VALUE) {
        LOG_WARN(LOG_TAG, "mesh not found: %s (looked in %s)", mesh_path, meshes_dir);
        return NULL;
    }

    /* Also search the Meshes dir itself (some projects have flat layout). */
    char subdir_paths[32][512];
    int subdir_count = 0;
    /* Push Meshes dir itself first. */
    snprintf(subdir_paths[subdir_count++], 512, "%s", meshes_dir);

    do {
        if (dir_fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (dir_fd.cFileName[0] == '.') continue; /* skip . and .. */
            if (subdir_count < 31) {
                snprintf(subdir_paths[subdir_count++], 512, "%s\\%s",
                         meshes_dir, dir_fd.cFileName);
            }
        }
    } while (FindNextFileA(hDir, &dir_fd));
    FindClose(hDir);

    int target_len = (int)strlen(target_kebab);

    for (int d = 0; d < subdir_count; d++) {
        WIN32_FIND_DATAA fd;
        char search_pattern[512];
        snprintf(search_pattern, sizeof(search_pattern), "%s\\*.obj", subdir_paths[d]);
        HANDLE hFind = FindFirstFileA(search_pattern, &fd);
        if (hFind == INVALID_HANDLE_VALUE) continue;
        do {
            char file_kebab[256];
            char base_name[256];
            snprintf(base_name, sizeof(base_name), "%s", fd.cFileName);
            char *ext = strrchr(base_name, '.');
            if (ext) *ext = '\0';
            sm_to_kebab(base_name, file_kebab, sizeof(file_kebab));

            /* Exact match. */
            if (strcmp(file_kebab, target_kebab) == 0) {
                char found_path[512];
                snprintf(found_path, sizeof(found_path), "%s\\%s", subdir_paths[d], fd.cFileName);
                FindClose(hFind);
                return jce_editor_model_load_file(found_path);
            }

            /* Suffix match: target "terrain" matches file kebab "sm-terrain". */
            int fk_len = (int)strlen(file_kebab);
            if (fk_len > target_len) {
                const char *suffix = file_kebab + (fk_len - target_len);
                if (strcmp(suffix, target_kebab) == 0
                    && suffix[-1] == '-') {
                    char found_path[512];
                    snprintf(found_path, sizeof(found_path), "%s\\%s", subdir_paths[d], fd.cFileName);
                    FindClose(hFind);
                    return jce_editor_model_load_file(found_path);
                }
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
#endif

    LOG_WARN(LOG_TAG, "mesh not found: %s (looked in %s)", mesh_path, meshes_dir);
    return NULL;
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

/* ── Entity Rendering (auto-detect components) ─────────────────────── */

/* Build a model matrix for entity i from its transform components.
   Returns false if the entity has no transform. */
static bool build_entity_model(JceEntityInfo *ent, jce_mat4 *out_model,
                                JceMesh **out_mesh)
{
    int comp_count = 0;
    JceComponentInfo *comps = jce_state_get_entity_components(ent->id, &comp_count);
    if (!comps) return false;

    const float *pos   = NULL;
    const float *rot   = NULL;
    const float *scale = NULL;
    bool has_mesh  = false;
    const char *mesh_path = NULL;

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

    return true;
}

/* Orange wireframe overlay for selected entities. */
static void draw_selection_outlines(void)
{
    int sel_count = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_count);
    if (sel_count == 0) return;

    /* Orange light: full ambient so mesh renders flat orange. */
    JceDirLight sel_light;
    sel_light.direction = jce_v3(0.0f, -1.0f, 0.0f);
    sel_light.color     = jce_v3(1.0f, 0.75f, 0.0f);
    sel_light.ambient   = 1.0f;
    jce_lighting_apply(s_sr.renderer, &sel_light);
    jce_renderer_set_wireframe(s_sr.renderer, true);

    /* Slightly scale-up outline model to avoid z-fighting with solid mesh. */
    jce_mat4 push_scale = jce_m4_scale(jce_v3(1.005f, 1.005f, 1.005f));

    for (int i = 0; i < sel_count; i++) {
        JceEntityInfo *ent = jce_state_get_entity(sel[i]);
        if (!ent || !ent->enabled) continue;

        jce_mat4 model;
        JceMesh *mesh = NULL;
        if (!build_entity_model(ent, &model, &mesh)) continue;
        if (!mesh) continue;

        jce_mat4 outline_model = jce_m4_multiply(&model, &push_scale);
        bgfx_set_transform(outline_model.raw[0], 1);
        jce_mesh_submit(mesh, s_sr.renderer, SCENE_VIEW_ID);
    }

    jce_renderer_set_wireframe(s_sr.renderer, false);

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

    /* Apply render mode: wireframe uses the mesh submit wireframe IBO. */
    JceSceneViewMode view_mode = jce_state_get_view_mode();
    if (view_mode == JCE_VIEW_WIREFRAME)
        jce_renderer_set_wireframe(s_sr.renderer, true);

    for (int i = 0; i < count; i++) {
        JceEntityInfo *ent = jce_state_get_entity((uint32_t)(i + 1));
        if (!ent || !ent->enabled) continue;

        jce_mat4 model;
        JceMesh *mesh = NULL;
        if (!build_entity_model(ent, &model, &mesh)) continue;
        if (!mesh) continue;

        bgfx_set_transform(model.raw[0], 1);
        jce_mesh_submit(mesh, s_sr.renderer, SCENE_VIEW_ID);
    }

    if (view_mode == JCE_VIEW_WIREFRAME)
        jce_renderer_set_wireframe(s_sr.renderer, false);

    /* Draw orange wireframe outlines for selected entities. */
    draw_selection_outlines();
}

/* ── Public API ────────────────────────────────────────────────────── */

bool jce_editor_scene_render_init(JceRenderer *renderer)
{
    if (s_sr.initialized) return true;

    memset(&s_sr, 0, sizeof(s_sr));
    s_sr.fbo.idx = UINT16_MAX;
    s_sr.fbo_color.idx = UINT16_MAX;
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

    /* Procedural meshes. */
    s_sr.cube_mesh  = jce_mesh_create_cube(1.0f);
    s_sr.plane_mesh = jce_mesh_create_plane(10.0f, 10.0f, 0);

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

    if (s_sr.camera)     { jce_camera_destroy(s_sr.camera);   s_sr.camera = NULL; }
    if (s_sr.cube_mesh)  { jce_mesh_destroy(s_sr.cube_mesh);  s_sr.cube_mesh = NULL; }
    if (s_sr.plane_mesh) { jce_mesh_destroy(s_sr.plane_mesh); s_sr.plane_mesh = NULL; }

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

    bgfx_set_view_name(SCENE_VIEW_ID, "EditorScene", INT32_MAX);
    bgfx_set_view_rect(SCENE_VIEW_ID, 0, 0, (uint16_t)width, (uint16_t)height);
    bgfx_set_view_clear(SCENE_VIEW_ID,
                        BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                        BG_COLOR_RGBA, 1.0f, 0);
    bgfx_set_view_transform(SCENE_VIEW_ID, view.raw[0], proj.raw[0]);
    bgfx_set_view_frame_buffer(SCENE_VIEW_ID, s_sr.fbo);
    bgfx_touch(SCENE_VIEW_ID);

    /* Draw sky gradient (behind everything). */
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
}
