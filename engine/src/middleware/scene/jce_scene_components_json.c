/*
 * jce_scene_serial.c  Engine scene JSON serializer/parser.
 *
 * Flat-entity-array format used by both editor and runtime. The editor
 * supplements with backwards-compat parsing on top of these primitives.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_str.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/resource/jce_scene_contract.h>

#include "os/core/jce_memory.h"

#include <cjson/cJSON.h>
#include <math.h>
#include <SDL3/SDL_filesystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "scene_serial"

/* Base directory of the currently-loading scene file; used to resolve
 * sibling .mat.json material references (e.g. "Materials/foo.mat.json").
 * Set by jce_scene_serial_set_base_dir() before parse, cleared after. */
static char s_scene_base_dir[1024] = { 0 };

void jce_scene_serial_set_base_dir(const char *dir)
{
    if (!dir || !*dir) {
        s_scene_base_dir[0] = '\0';
        return;
    }
    size_t L = strlen(dir);
    if (L >= sizeof(s_scene_base_dir)) L = sizeof(s_scene_base_dir) - 1;
    memcpy(s_scene_base_dir, dir, L);
    s_scene_base_dir[L] = '\0';
    /* Strip trailing slash for consistent join with snprintf("%s/%s"). */
    while (L > 0 && (s_scene_base_dir[L-1] == '/' || s_scene_base_dir[L-1] == '\\')) {
        s_scene_base_dir[--L] = '\0';
    }
}

static bool sse_path_is_absolute(const char *p)
{
    if (!p || !*p) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    if (p[1] == ':' && (p[2] == '/' || p[2] == '\\')) return true; /* C:\ */
    return false;
}

static bool sse_file_exists(const char *p)
{
    if (!p || !*p) return false;
    SDL_PathInfo info;
    return SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_FILE;
}

static bool sse_dir_exists(const char *p)
{
    if (!p || !*p) return false;
    SDL_PathInfo info;
    return SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

/* Strip the last path segment from `path` in place, returning true if a
 * parent exists (i.e. there was at least one separator). */
static bool sse_path_parent_inplace(char *path)
{
    if (!path || !*path) return false;
    size_t L = strlen(path);
    while (L > 0 && (path[L-1] == '/' || path[L-1] == '\\')) path[--L] = '\0';
    while (L > 0 && path[L-1] != '/' && path[L-1] != '\\') L--;
    while (L > 0 && (path[L-1] == '/' || path[L-1] == '\\')) L--;
    if (L == 0) return false;
    path[L] = '\0';
    return true;
}

/* Score a .mat.json basename for use as a fallback material when a
 * scene's referenced material file is missing.  Higher score wins. */
static int fallback_material_score(const char *basename)
{
    if (!basename) return 0;
    const char *lc = basename;
    /* Hand-rolled tolower compare (basename buffer is small). */
    char low[128];
    size_t i = 0;
    for (; lc[i] && i + 1 < sizeof(low); i++) {
        char ch = lc[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        low[i] = ch;
    }
    low[i] = '\0';
    if (strstr(low, "_default.mat.json")) return 100;
    if (strstr(low, "default"))           return 90;
    if (strstr(low, "universal_a"))       return 85;
    if (strstr(low, "universal"))         return 80;
    if (strstr(low, "main"))              return 60;
    if (strstr(low, "base"))              return 55;
    return 1;
}

/* Returns true when `needle` (case-insensitive) appears as a path
 * segment inside `haystack`, e.g. "Nature" inside "Meshes/Nature/Grass". */
static bool path_contains_segment_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle) return false;
    size_t nL = strlen(needle);
    const char *p = haystack;
    while (*p) {
        const char *seg_start = p;
        while (*p && *p != '/' && *p != '\\') p++;
        size_t segL = (size_t)(p - seg_start);
        if (segL == nL) {
            bool eq = true;
            for (size_t k = 0; k < segL; k++) {
                char a = seg_start[k]; if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                char b = needle[k];    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { eq = false; break; }
            }
            if (eq) return true;
        }
        if (*p) p++;
    }
    return false;
}

/* Extract the leaf directory name of `dirpath` (no trailing slash)
 * into `out`. */
static void path_leaf_into(const char *dirpath, char *out, size_t out_size)
{
    if (out_size == 0) return;
    out[0] = '\0';
    if (!dirpath) return;
    size_t L = strlen(dirpath);
    while (L > 0 && (dirpath[L-1] == '/' || dirpath[L-1] == '\\')) L--;
    size_t end = L;
    while (L > 0 && dirpath[L-1] != '/' && dirpath[L-1] != '\\') L--;
    size_t leafL = end - L;
    if (leafL >= out_size) leafL = out_size - 1;
    memcpy(out, dirpath + L, leafL);
    out[leafL] = '\0';
}

struct FallbackMatScan {
    char        best[1280];
    int         best_score;
    int         depth;       /* current recursion depth, capped to limit fanout */
    /* Mesh path hint (lowercased preferred-form, e.g. "Meshes/Nature/Grass/SM_Foo.obj");
     * empty when no hint available. Used to give domain-matched materials
     * a large bonus so e.g. a foliage mesh maps to Materials/Nature/* and
     * not the universal metallic material. */
    const char *mesh_hint;
};

static bool fallback_mat_walker(void *user, const char *dirpath, const char *name)
{
    struct FallbackMatScan *st = (struct FallbackMatScan *)user;
    if (!name) return true;
    size_t L = strlen(name);
    if (L < 9 || strcmp(name + L - 9, ".mat.json") != 0) return true;
    int score = fallback_material_score(name);

    /* Domain-match bonus: if the material lives inside a folder whose
     * leaf name (e.g. "Nature", "VFX", "Colors") also appears as a
     * segment in the mesh path, prefer it strongly over generic
     * "universal" materials.  This is what fixes Unity-imported scenes
     * where every MeshRenderer points at "default.mat" — without the
     * bonus the scanner picks one universal material for ALL meshes,
     * so e.g. grass renders with the metallic universal albedo. */
    if (st->mesh_hint && st->mesh_hint[0] && dirpath && *dirpath) {
        char leaf[64];
        path_leaf_into(dirpath, leaf, sizeof(leaf));
        if (leaf[0]
            && jce_strcasecmp(leaf, "Materials") != 0
            && jce_strcasecmp(leaf, "materials") != 0
            && path_contains_segment_ci(st->mesh_hint, leaf)) {
            score += 500;
        }
    }

    if (score > st->best_score) {
        st->best_score = score;
        snprintf(st->best, sizeof(st->best), "%s/%s", dirpath, name);
    }
    return true;
}

/* SDL_EnumerateDirectory adapter — recurses into subdirectories so that
 * nested Materials/Nature, Materials/VFX, Materials/Colors, ... are all
 * considered (Unity asset packs typically organise materials this way). */
static SDL_EnumerationResult SDLCALL sse_sdl_enum_cb(void *userdata, const char *dirpath, const char *fname);

static void scan_dir_recursive(const char *dir, struct FallbackMatScan *st);

static SDL_EnumerationResult SDLCALL
sse_sdl_enum_cb(void *userdata, const char *dirpath, const char *fname)
{
    struct FallbackMatScan *st = (struct FallbackMatScan *)userdata;
    if (!fname) return SDL_ENUM_CONTINUE;

    char full[1280];
    snprintf(full, sizeof(full), "%s/%s", dirpath, fname);

    SDL_PathInfo info;
    if (SDL_GetPathInfo(full, &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
        if (st->depth < 4) {
            st->depth++;
            scan_dir_recursive(full, st);
            st->depth--;
        }
        return SDL_ENUM_CONTINUE;
    }
    fallback_mat_walker(userdata, dirpath, fname);
    return SDL_ENUM_CONTINUE;
}

static void scan_dir_recursive(const char *dir, struct FallbackMatScan *st)
{
    if (!dir || !*dir || !sse_dir_exists(dir)) return;
    SDL_EnumerateDirectory(dir, sse_sdl_enum_cb, st);
}

/* Walk `dir` recursively for .mat.json files; updates `st` with the
 * highest-scoring candidate. */
static void scan_dir_for_fallback_material(const char *dir,
                                           struct FallbackMatScan *st)
{
    st->depth = 0;
    scan_dir_recursive(dir, st);
}

/* Locate a sensible fallback .mat.json by scanning Materials/ folders
 * near the scene file.  Returns true and fills out_path if one was
 * found. Used when the scene's stated materialPath does not resolve
 * (e.g. Unity-exported scenes that reference a non-existent
 * "default.mat" for every MeshRenderer).
 *
 * `mesh_hint` (optional) gives the scoring routine a chance to prefer
 * a material whose folder leaf matches a segment of the mesh path
 * (e.g. mesh "Meshes/Nature/Grass/..." prefers "Materials/Nature/..."). */
static bool find_fallback_material(const char *mesh_hint,
                                   char *out_path, size_t out_size)
{
    if (!out_path || out_size == 0) return false;
    if (s_scene_base_dir[0] == '\0') return false;

    struct FallbackMatScan st;
    st.best[0] = '\0';
    st.best_score = 0;
    st.depth = 0;
    st.mesh_hint = (mesh_hint && *mesh_hint) ? mesh_hint : NULL;

    char base[1024];
    snprintf(base, sizeof(base), "%s", s_scene_base_dir);

    /* Try scene_dir, then walk up to 4 ancestors, scanning Materials/
     * and materials/ at each level. */
    for (int up = 0; up <= 4; up++) {
        char sub[1280];
        snprintf(sub, sizeof(sub), "%s/Materials", base);
        scan_dir_for_fallback_material(sub, &st);
        snprintf(sub, sizeof(sub), "%s/materials", base);
        scan_dir_for_fallback_material(sub, &st);
        if (!sse_path_parent_inplace(base)) break;
    }

    if (st.best_score > 0) {
        snprintf(out_path, out_size, "%s", st.best);
        return true;
    }
    return false;
}

/* Resolve a (possibly relative) texture path stored inside a .mat.json
 * to an absolute path on disk.  `mat_path` is the absolute path of the
 * material file.  On success, copies the resolved path into `tex_path`
 * (overwriting it).  On failure, leaves `tex_path` unchanged. */
static void resolve_tex_relative_to_material(const char *mat_path,
                                              char *tex_path,
                                              size_t tex_path_sz)
{
    if (!mat_path || !*mat_path || !tex_path || !*tex_path) return;
    if (sse_path_is_absolute(tex_path) && sse_file_exists(tex_path)) return;

    char mat_dir[1024];
    snprintf(mat_dir, sizeof(mat_dir), "%s", mat_path);
    if (!sse_path_parent_inplace(mat_dir)) return;

    char candidate[1280];
    /* (1) sibling of the .mat.json. */
    snprintf(candidate, sizeof(candidate), "%s/%s", mat_dir, tex_path);
    if (sse_file_exists(candidate)) {
        snprintf(tex_path, tex_path_sz, "%s", candidate);
        return;
    }
    /* (2) walk parents up to 4 levels (Unity layout: Materials/ and
     *     Textures/ are siblings under the project root). */
    char base[1024];
    snprintf(base, sizeof(base), "%s", mat_dir);
    for (int up = 0; up < 4; up++) {
        if (!sse_path_parent_inplace(base)) break;
        snprintf(candidate, sizeof(candidate), "%s/%s", base, tex_path);
        if (sse_file_exists(candidate)) {
            snprintf(tex_path, tex_path_sz, "%s", candidate);
            return;
        }
    }
}

/* ── Local Euler ↔ Quaternion helpers (degrees) ───────────────────── */

static void q_to_euler_deg(jce_quat q, float out[3])
{
    jce_vec3 e = jce_q_to_euler(q);
    out[0] = e.x * JCE_RAD2DEG;
    out[1] = e.y * JCE_RAD2DEG;
    out[2] = e.z * JCE_RAD2DEG;
}

static jce_quat q_from_euler_deg(float x_deg, float y_deg, float z_deg)
{
    return jce_q_from_euler(x_deg * JCE_DEG2RAD,
                            y_deg * JCE_DEG2RAD,
                            z_deg * JCE_DEG2RAD);
}

/* ── JSON helpers ─────────────────────────────────────────────────── */

static double j_num(const cJSON *o, const char *k, double def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(it)) return it->valuedouble;
    return def;
}

static double j_num2(const cJSON *o, const char *k1, const char *k2, double def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k1);
    if (cJSON_IsNumber(it)) return it->valuedouble;
    it = cJSON_GetObjectItemCaseSensitive(o, k2);
    if (cJSON_IsNumber(it)) return it->valuedouble;
    return def;
}

static bool j_bool(const cJSON *o, const char *k, bool def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(it)) return cJSON_IsTrue(it);
    if (cJSON_IsNumber(it)) return it->valuedouble != 0.0;
    return def;
}

static const char *j_str(const cJSON *o, const char *k, const char *def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(it) && it->valuestring) return it->valuestring;
    return def;
}

static const char *j_str_any(const cJSON *o, const char *const *keys, int n)
{
    for (int i = 0; i < n; i++) {
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, keys[i]);
        if (cJSON_IsString(it) && it->valuestring) return it->valuestring;
    }
    return NULL;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int streq_ci(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ── Component parsing ────────────────────────────────────────────── */

static void parse_transform(JceScene *s, JceEntity e, const cJSON *c)
{
    JceTransform t;
    t.position = jce_v3((float)j_num2(c, "posX", "pos_x", 0.0),
                        (float)j_num2(c, "posY", "pos_y", 0.0),
                        (float)j_num2(c, "posZ", "pos_z", 0.0));
    float rx = (float)j_num2(c, "rotX", "rot_x", 0.0);
    float ry = (float)j_num2(c, "rotY", "rot_y", 0.0);
    float rz = (float)j_num2(c, "rotZ", "rot_z", 0.0);

    /* Accept quaternion (rotW present) or euler degrees. */
    const cJSON *rw = cJSON_GetObjectItemCaseSensitive(c, "rotW");
    if (!rw) rw = cJSON_GetObjectItemCaseSensitive(c, "rot_w");
    if (cJSON_IsNumber(rw)) {
        jce_quat q = jce_v4(rx, ry, rz, (float)rw->valuedouble);
        t.rotation = jce_q_normalize(q);
    } else {
        t.rotation = q_from_euler_deg(rx, ry, rz);
    }

    t.scale = jce_v3((float)j_num2(c, "scaleX", "scale_x", 1.0),
                     (float)j_num2(c, "scaleY", "scale_y", 1.0),
                     (float)j_num2(c, "scaleZ", "scale_z", 1.0));
    jce_scene_set_transform(s, e, &t);
}

static void parse_mesh_renderer(JceScene *s, JceEntity e, const cJSON *c)
{
    JceMeshRenderer mr;
    memset(&mr, 0, sizeof(mr));
    mr.visible = true;
    static const char *const mk[] = { "meshPath", "mesh_path", "mesh" };
    static const char *const matk[] = { "materialPath", "material_path", "material" };
    const char *mp = j_str_any(c, mk, 3);
    const char *mt = j_str_any(c, matk, 3);
    if (mp) copy_str(mr.mesh_path, sizeof(mr.mesh_path), mp);
    if (mt) copy_str(mr.material_path, sizeof(mr.material_path), mt);
    mr.mesh_shape       = (int)j_num(c, "meshShape", 0);
    mr.base_color[0]    = (float)j_num(c, "baseColorR", 1.0);
    mr.base_color[1]    = (float)j_num(c, "baseColorG", 1.0);
    mr.base_color[2]    = (float)j_num(c, "baseColorB", 1.0);
    mr.base_color[3]    = (float)j_num(c, "baseColorA", 1.0);
    mr.metallic         = (float)j_num(c, "metallic",   0.0);
    mr.roughness        = (float)j_num(c, "roughness",  1.0);
    mr.emissive[0]      = (float)j_num(c, "emissiveR",  0.0);
    mr.emissive[1]      = (float)j_num(c, "emissiveG",  0.0);
    mr.emissive[2]      = (float)j_num(c, "emissiveB",  0.0);
    mr.normal_scale     = (float)j_num(c, "normalScale", 1.0);
    mr.ao_strength      = (float)j_num(c, "aoStrength",  1.0);
    mr.alpha_mode       = (int)j_num(c, "alphaMode",   0);
    mr.alpha_cutoff     = (float)j_num(c, "alphaCutoff", 0.5);
    mr.double_sided     = j_bool(c, "doubleSided", false);
    copy_str(mr.albedo_tex,   sizeof(mr.albedo_tex),   j_str(c, "albedoTex",   ""));
    copy_str(mr.mr_tex,       sizeof(mr.mr_tex),       j_str(c, "mrTex",       ""));
    copy_str(mr.normal_tex,   sizeof(mr.normal_tex),   j_str(c, "normalTex",   ""));
    copy_str(mr.ao_tex,       sizeof(mr.ao_tex),       j_str(c, "aoTex",       ""));
    copy_str(mr.emissive_tex, sizeof(mr.emissive_tex), j_str(c, "emissiveTex", ""));

    /* If a .mat.json material is referenced and no per-entity texture
     * overrides exist, backfill texture paths from the material file.
     * This is required for Unity-exported scenes where MeshRenderer
     * carries only materialPath and the textures live inside the .mat. */
    if (mr.material_path[0] != '\0' && mr.albedo_tex[0] == '\0') {
        char mat_full[1280] = { 0 };
        const char *try_paths[3] = { NULL, NULL, NULL };
        int n_try = 0;

        if (sse_path_is_absolute(mr.material_path)) {
            try_paths[n_try++] = mr.material_path;
        } else {
            if (s_scene_base_dir[0]) {
                snprintf(mat_full, sizeof(mat_full), "%s/%s",
                         s_scene_base_dir, mr.material_path);
                try_paths[n_try++] = mat_full;
            }
            try_paths[n_try++] = mr.material_path;
        }

        const char *resolved = NULL;
        for (int i = 0; i < n_try; i++) {
            if (sse_file_exists(try_paths[i])) { resolved = try_paths[i]; break; }
        }
        /* Walk parent directories of the scene file to find the asset
         * root (Unity layout: scenes/ and Materials/ are siblings). */
        char parent_try[1280] = { 0 };
        if (!resolved && s_scene_base_dir[0] && !sse_path_is_absolute(mr.material_path)) {
            char base[1024];
            snprintf(base, sizeof(base), "%s", s_scene_base_dir);
            for (int up = 0; up < 4 && !resolved; up++) {
                /* trim last segment */
                long L = (long)strlen(base);
                while (L > 0 && base[L-1] != '/' && base[L-1] != '\\') L--;
                while (L > 0 && (base[L-1] == '/' || base[L-1] == '\\')) L--;
                if (L <= 0) break;
                base[L] = '\0';
                snprintf(parent_try, sizeof(parent_try), "%s/%s", base, mr.material_path);
                if (sse_file_exists(parent_try)) { resolved = parent_try; break; }
            }
        }
        /* Also accept ".mat" → ".mat.json" alias. */
        char alt[1300] = { 0 };
        if (!resolved) {
            for (int i = 0; i < n_try; i++) {
                size_t L = strlen(try_paths[i]);
                if (L >= 4 && strcmp(try_paths[i] + L - 4, ".mat") == 0) {
                    snprintf(alt, sizeof(alt), "%s.json", try_paths[i]);
                    if (sse_file_exists(alt)) { resolved = alt; break; }
                }
            }
        }

        /* Final fallback (Unity-imported scenes): scan Materials/ folders
         * near the scene file for a sensible substitute material when the
         * stated material doesn't exist (e.g. "default.mat" placeholders).
         * Without this, every mesh in a Unity-converted scene renders
         * untextured because every MeshRenderer points at a missing file. */
        char fallback_mat[1280] = { 0 };
        if (!resolved && find_fallback_material(mr.mesh_path, fallback_mat, sizeof(fallback_mat))) {
            resolved = fallback_mat;
            LOG_INFO(LOG_TAG,
                "material '%s' not found for mesh '%s'; fallback => '%s'",
                mr.material_path, mr.mesh_path, fallback_mat);
        }

        if (resolved) {
            JcePbrMaterial pbr;
            char tex_paths[5][256];
            if (jce_pbr_material_load_json(resolved, &pbr, tex_paths)) {
                /* Resolve each texture relative to the material file
                 * (which may be in Materials/ while textures are in
                 * Textures/ at the project root) so the renderer can
                 * load them regardless of the runtime cwd. */
                for (int ti = 0; ti < 5; ti++) {
                    if (tex_paths[ti][0]) {
                        resolve_tex_relative_to_material(resolved,
                                                         tex_paths[ti],
                                                         sizeof(tex_paths[ti]));
                    }
                }
                if (tex_paths[0][0]) copy_str(mr.albedo_tex,   sizeof(mr.albedo_tex),   tex_paths[0]);
                if (tex_paths[1][0]) copy_str(mr.mr_tex,       sizeof(mr.mr_tex),       tex_paths[1]);
                if (tex_paths[2][0]) copy_str(mr.normal_tex,   sizeof(mr.normal_tex),   tex_paths[2]);
                if (tex_paths[3][0]) copy_str(mr.ao_tex,       sizeof(mr.ao_tex),       tex_paths[3]);
                if (tex_paths[4][0]) copy_str(mr.emissive_tex, sizeof(mr.emissive_tex), tex_paths[4]);
                /* Forward PBR factors so backfilled materials shade correctly. */
                mr.base_color[0] = pbr.base_color_factor[0];
                mr.base_color[1] = pbr.base_color_factor[1];
                mr.base_color[2] = pbr.base_color_factor[2];
                mr.base_color[3] = pbr.base_color_factor[3];
                mr.metallic     = pbr.metallic_factor;
                mr.roughness    = pbr.roughness_factor;
                mr.emissive[0]  = pbr.emissive_factor[0];
                mr.emissive[1]  = pbr.emissive_factor[1];
                mr.emissive[2]  = pbr.emissive_factor[2];
                mr.normal_scale = pbr.normal_scale;
                mr.ao_strength  = pbr.ao_strength;
            }
        }
    }

    jce_scene_set_mesh_renderer(s, e, &mr);
}

static void parse_camera(JceScene *s, JceEntity e, const cJSON *c)
{
    JceCameraComponent cc;
    memset(&cc, 0, sizeof(cc));
    cc.fov_deg    = (float)j_num(c, "fov", 60.0);
    cc.near_plane = (float)j_num2(c, "nearClip", "near_clip", 0.1);
    cc.far_plane  = (float)j_num2(c, "farClip", "far_clip", 1000.0);
    cc.is_primary = j_bool(c, "primary", false);
    cc.ortho      = j_bool(c, "orthographic", false);
    if (!cc.ortho) cc.ortho = j_bool(c, "ortho", false);
    if (!cc.ortho) cc.ortho = j_bool(c, "isOrtho", false);
    jce_scene_set_camera(s, e, &cc);
}

static void parse_dir_light(JceScene *s, JceEntity e, const cJSON *c)
{
    JceDirectionalLight dl;
    memset(&dl, 0, sizeof(dl));
    dl.direction.x = (float)j_num(c, "dirX", 0.0);
    dl.direction.y = (float)j_num(c, "dirY", -1.0);
    dl.direction.z = (float)j_num(c, "dirZ", 0.0);
    dl.color.x = (float)j_num(c, "colorR", 1.0);
    dl.color.y = (float)j_num(c, "colorG", 1.0);
    dl.color.z = (float)j_num(c, "colorB", 1.0);
    dl.intensity = (float)j_num(c, "intensity", 1.0);
    dl.casts_shadow = j_bool(c, "castsShadow", false);
    jce_scene_set_dir_light(s, e, &dl);
}

static void parse_point_light(JceScene *s, JceEntity e, const cJSON *c)
{
    JcePointLight pl;
    memset(&pl, 0, sizeof(pl));
    pl.position.x = (float)j_num(c, "posX", 0.0);
    pl.position.y = (float)j_num(c, "posY", 0.0);
    pl.position.z = (float)j_num(c, "posZ", 0.0);
    pl.color.x = (float)j_num(c, "colorR", 1.0);
    pl.color.y = (float)j_num(c, "colorG", 1.0);
    pl.color.z = (float)j_num(c, "colorB", 1.0);
    pl.intensity = (float)j_num(c, "intensity", 1.0);
    pl.radius    = (float)j_num(c, "radius", 10.0);
    jce_scene_set_point_light(s, e, &pl);
}

static void parse_spot_light(JceScene *s, JceEntity e, const cJSON *c)
{
    JceSpotLight sl;
    memset(&sl, 0, sizeof(sl));
    sl.position.x = (float)j_num(c, "posX", 0.0);
    sl.position.y = (float)j_num(c, "posY", 0.0);
    sl.position.z = (float)j_num(c, "posZ", 0.0);
    sl.direction.x = (float)j_num(c, "dirX", 0.0);
    sl.direction.y = (float)j_num(c, "dirY", -1.0);
    sl.direction.z = (float)j_num(c, "dirZ", 0.0);
    sl.color.x = (float)j_num(c, "colorR", 1.0);
    sl.color.y = (float)j_num(c, "colorG", 1.0);
    sl.color.z = (float)j_num(c, "colorB", 1.0);
    sl.intensity = (float)j_num(c, "intensity", 1.0);
    sl.radius    = (float)j_num(c, "radius", 10.0);
    float inner_deg = (float)j_num(c, "innerConeDeg", 30.0);
    float outer_deg = (float)j_num(c, "outerConeDeg", 45.0);
    sl.inner_cone_cos = cosf(inner_deg * JCE_DEG2RAD);
    sl.outer_cone_cos = cosf(outer_deg * JCE_DEG2RAD);
    jce_scene_set_spot_light(s, e, &sl);
}

static void parse_skybox(JceScene *s, JceEntity e, const cJSON *c)
{
    JceSkyboxComponent sky;
    memset(&sky, 0, sizeof(sky));
    copy_str(sky.hdr_path, sizeof(sky.hdr_path), j_str(c, "hdrPath", ""));
    sky.rotation   = (float)j_num(c, "rotation", 0.0);
    sky.exposure   = (float)j_num(c, "exposure", 1.0);
    sky.use_as_ibl = j_bool(c, "useAsIbl", true);
    jce_scene_set_skybox(s, e, &sky);
}

static void parse_sprite_renderer(JceScene *s, JceEntity e, const cJSON *c)
{
    JceSpriteRendererComponent sp;
    memset(&sp, 0, sizeof(sp));
    copy_str(sp.sprite_path, sizeof(sp.sprite_path), j_str(c, "spritePath", ""));
    sp.color[0] = (float)j_num(c, "colorR", 1.0);
    sp.color[1] = (float)j_num(c, "colorG", 1.0);
    sp.color[2] = (float)j_num(c, "colorB", 1.0);
    sp.color[3] = (float)j_num(c, "colorA", 1.0);
    sp.flip_x   = j_bool(c, "flipX", false);
    sp.flip_y   = j_bool(c, "flipY", false);
    sp.sorting_order = (int)j_num(c, "sortingOrder", 0);
    jce_scene_set_sprite_renderer(s, e, &sp);
}

static void parse_sprite_animator(JceScene *s, JceEntity e, const cJSON *c)
{
    JceSpriteAnimatorComponent sa;
    memset(&sa, 0, sizeof(sa));
    copy_str(sa.sheet_path,    sizeof(sa.sheet_path),    j_str(c, "sheetPath", ""));
    copy_str(sa.atlas_path,    sizeof(sa.atlas_path),    j_str(c, "atlasPath", ""));
    sa.frame_width  = (int)j_num(c, "frameWidth",  0);
    sa.frame_height = (int)j_num(c, "frameHeight", 0);
    copy_str(sa.current_anim, sizeof(sa.current_anim), j_str(c, "currentAnim", ""));
    sa.speed   = (float)j_num(c, "speed", 1.0);
    sa.loop    = j_bool(c, "loop", true);
    sa.playing = j_bool(c, "playing", false);
    jce_scene_set_sprite_animator(s, e, &sa);
}

static void parse_skeletal_animator(JceScene *s, JceEntity e, const cJSON *c)
{
    JceSkeletalAnimatorComponent sk;
    memset(&sk, 0, sizeof(sk));
    copy_str(sk.skeleton_path, sizeof(sk.skeleton_path), j_str(c, "skeletonPath", ""));
    sk.speed       = (float)j_num(c, "speed", 1.0);
    sk.loop        = j_bool(c, "loop", true);
    sk.playing     = j_bool(c, "playing", false);
    sk.active_clip = (int)j_num(c, "activeClip", -1);

    const cJSON *clips = cJSON_GetObjectItemCaseSensitive(c, "clipNames");
    if (cJSON_IsArray(clips)) {
        int n = cJSON_GetArraySize(clips);
        if (n > 8) n = 8;
        sk.clip_count = n;
        for (int i = 0; i < n; i++) {
            const cJSON *it = cJSON_GetArrayItem(clips, i);
            if (cJSON_IsString(it))
                copy_str(sk.clip_names[i], sizeof(sk.clip_names[i]), it->valuestring);
        }
    }
    jce_scene_set_skeletal_animator(s, e, &sk);
}

static void parse_editor_meta(JceScene *s, JceEntity e, const cJSON *c)
{
    JceEditorMeta m;
    memset(&m, 0, sizeof(m));
    copy_str(m.name, sizeof(m.name), j_str(c, "name", ""));
    copy_str(m.tag,  sizeof(m.tag),  j_str(c, "tag",  ""));
    m.tag_color       = (uint8_t)j_num(c, "tagColor", 0);
    m.enabled         = j_bool(c, "enabled", true);
    m.prefab_instance = j_bool(c, "prefabInstance", false);
    copy_str(m.prefab_path, sizeof(m.prefab_path), j_str(c, "prefabPath", ""));
    jce_scene_set_editor_meta(s, e, &m);
}

/* Unified "Light" component: dispatches to dir/point/spot based on lightType. */
static void parse_unified_light(JceScene *s, JceEntity e, const cJSON *c)
{
    float colorR = (float)j_num2(c, "colorR", "color_r", 1.0);
    float colorG = (float)j_num2(c, "colorG", "color_g", 1.0);
    float colorB = (float)j_num2(c, "colorB", "color_b", 1.0);
    float intensity = (float)j_num(c, "intensity", 1.0);
    bool  casts_shadow = j_bool(c, "castsShadow", false);

    int ltype = 0;
    const cJSON *lt = cJSON_GetObjectItemCaseSensitive(c, "lightType");
    if (!lt) lt = cJSON_GetObjectItemCaseSensitive(c, "type");
    if (cJSON_IsNumber(lt)) ltype = lt->valueint;
    else if (cJSON_IsString(lt) && lt->valuestring) {
        if (streq_ci(lt->valuestring, "point")) ltype = 1;
        else if (streq_ci(lt->valuestring, "spot")) ltype = 2;
    }

    if (ltype == 1) {
        JcePointLight pl;
        memset(&pl, 0, sizeof(pl));
        pl.color.x = colorR; pl.color.y = colorG; pl.color.z = colorB;
        pl.intensity = intensity;
        pl.radius    = (float)j_num(c, "radius", 10.0);
        jce_scene_set_point_light(s, e, &pl);
    } else if (ltype == 2) {
        JceSpotLight sl;
        memset(&sl, 0, sizeof(sl));
        sl.color.x = colorR; sl.color.y = colorG; sl.color.z = colorB;
        sl.intensity = intensity;
        sl.radius    = (float)j_num(c, "radius", 10.0);
        float inner_deg = (float)j_num2(c, "innerConeDeg", "inner_cone_deg", 25.0);
        float outer_deg = (float)j_num2(c, "outerConeDeg", "outer_cone_deg", 35.0);
        sl.inner_cone_cos = cosf(inner_deg * JCE_DEG2RAD);
        sl.outer_cone_cos = cosf(outer_deg * JCE_DEG2RAD);
        jce_scene_set_spot_light(s, e, &sl);
    } else {
        JceDirectionalLight dl;
        memset(&dl, 0, sizeof(dl));
        dl.color.x = colorR; dl.color.y = colorG; dl.color.z = colorB;
        dl.intensity    = intensity;
        dl.casts_shadow = casts_shadow;
        jce_scene_set_dir_light(s, e, &dl);
    }
}

static void parse_animator(JceScene *s, JceEntity e, const cJSON *c)
{
    JceAnimatorComponent a;
    memset(&a, 0, sizeof(a));
    copy_str(a.clip_name, sizeof(a.clip_name), j_str(c, "clipName", ""));
    a.speed   = (float)j_num(c, "speed", 1.0);
    a.loop    = j_bool(c, "loop", false);
    a.playing = j_bool(c, "playing", false);
    jce_scene_set_animator(s, e, &a);
}

static void parse_rigidbody(JceScene *s, JceEntity e, const cJSON *c)
{
    JceRigidBodyComponent rb;
    memset(&rb, 0, sizeof(rb));
    rb.mass         = (float)j_num(c, "mass", 1.0);
    rb.drag         = (float)j_num(c, "drag", 0.0);
    rb.angular_drag = (float)j_num(c, "angularDrag", 0.05);
    rb.use_gravity  = j_bool(c, "useGravity", true);
    rb.is_kinematic = j_bool(c, "isKinematic", false);
    jce_scene_set_rigidbody(s, e, &rb);
}

static void parse_box_collider(JceScene *s, JceEntity e, const cJSON *c)
{
    JceBoxColliderComponent bc;
    memset(&bc, 0, sizeof(bc));
    bc.center[0] = (float)j_num(c, "centerX", 0.0);
    bc.center[1] = (float)j_num(c, "centerY", 0.0);
    bc.center[2] = (float)j_num(c, "centerZ", 0.0);
    bc.size[0]   = (float)j_num(c, "sizeX", 1.0);
    bc.size[1]   = (float)j_num(c, "sizeY", 1.0);
    bc.size[2]   = (float)j_num(c, "sizeZ", 1.0);
    bc.is_trigger = j_bool(c, "isTrigger", false);
    jce_scene_set_box_collider(s, e, &bc);
}

static void parse_sphere_collider(JceScene *s, JceEntity e, const cJSON *c)
{
    JceSphereColliderComponent sc2;
    memset(&sc2, 0, sizeof(sc2));
    sc2.center[0] = (float)j_num(c, "centerX", 0.0);
    sc2.center[1] = (float)j_num(c, "centerY", 0.0);
    sc2.center[2] = (float)j_num(c, "centerZ", 0.0);
    sc2.radius     = (float)j_num(c, "radius", 0.5);
    sc2.is_trigger = j_bool(c, "isTrigger", false);
    jce_scene_set_sphere_collider(s, e, &sc2);
}

static void parse_character_controller(JceScene *s, JceEntity e, const cJSON *c)
{
    JceCharacterControllerComponent cc;
    memset(&cc, 0, sizeof(cc));
    cc.height      = (float)j_num(c, "height", 2.0);
    cc.radius      = (float)j_num(c, "radius", 0.5);
    cc.step_offset = (float)j_num(c, "stepOffset", 0.3);
    cc.slope_limit = (float)j_num(c, "slopeLimit", 45.0);
    jce_scene_set_character_controller(s, e, &cc);
}

static void parse_audio_source(JceScene *s, JceEntity e, const cJSON *c)
{
    JceAudioSourceComponent as;
    memset(&as, 0, sizeof(as));
    copy_str(as.clip_path, sizeof(as.clip_path), j_str(c, "clipPath", ""));
    as.volume        = (float)j_num(c, "volume", 1.0);
    as.pitch         = (float)j_num(c, "pitch", 1.0);
    as.spatial_blend  = (float)j_num(c, "spatialBlend", 0.0);
    as.loop          = j_bool(c, "loop", false);
    as.play_on_awake = j_bool(c, "playOnAwake", true);
    jce_scene_set_audio_source(s, e, &as);
}

static void parse_script(JceScene *s, JceEntity e, const cJSON *c)
{
    JceScriptComponent sc2;
    memset(&sc2, 0, sizeof(sc2));
    copy_str(sc2.script_path, sizeof(sc2.script_path), j_str(c, "scriptPath", ""));
    jce_scene_set_script(s, e, &sc2);
}

static void parse_terrain(JceScene *s, JceEntity e, const cJSON *c)
{
    JceTerrainComponent tc;
    memset(&tc, 0, sizeof(tc));
    copy_str(tc.terrain_path, sizeof(tc.terrain_path),
             j_str(c, "terrainPath", ""));
    const char *keys[4] = { "layerAlbedoPath0", "layerAlbedoPath1",
                            "layerAlbedoPath2", "layerAlbedoPath3" };
    for (int i = 0; i < 4; ++i)
        copy_str(tc.layer_albedo_path[i], sizeof(tc.layer_albedo_path[i]),
                 j_str(c, keys[i], ""));
    tc.tile_scale     = (float)j_num(c, "tileScale", 10.0);
    tc.tint[0]        = (float)j_num(c, "tintR", 1.0);
    tc.tint[1]        = (float)j_num(c, "tintG", 1.0);
    tc.tint[2]        = (float)j_num(c, "tintB", 1.0);
    tc.visible        = j_bool(c, "visible", true);
    tc.splat_enabled  = j_bool(c, "splatEnabled", true);
    jce_scene_set_terrain(s, e, &tc);
}

static void parse_constraint(JceScene *s, JceEntity e, const cJSON *c)
{
    JceConstraintComponent cn;
    memset(&cn, 0, sizeof(cn));
    cn.constraint_type  = (int)j_num(c, "constraintType", 0);
    cn.target_entity    = (uint32_t)j_num(c, "targetEntity", 0);
    cn.pivot_a[0] = (float)j_num(c, "pivotAx", 0.0);
    cn.pivot_a[1] = (float)j_num(c, "pivotAy", 0.0);
    cn.pivot_a[2] = (float)j_num(c, "pivotAz", 0.0);
    cn.pivot_b[0] = (float)j_num(c, "pivotBx", 0.0);
    cn.pivot_b[1] = (float)j_num(c, "pivotBy", 0.0);
    cn.pivot_b[2] = (float)j_num(c, "pivotBz", 0.0);
    cn.axis[0]    = (float)j_num(c, "axisX", 0.0);
    cn.axis[1]    = (float)j_num(c, "axisY", 1.0);
    cn.axis[2]    = (float)j_num(c, "axisZ", 0.0);
    cn.lower_limit = (float)j_num(c, "lowerLimit", 0.0);
    cn.upper_limit = (float)j_num(c, "upperLimit", 0.0);
    cn.disable_collision = j_bool(c, "disableCollision", false);
    jce_scene_set_constraint(s, e, &cn);
}

static void parse_one_component(JceScene *s, JceEntity e, const cJSON *comp)
{
    /* Accept "type", "componentType", or "class" for the type field. */
    const char *type = j_str(comp, "type", NULL);
    if (!type) type = j_str(comp, "componentType", NULL);
    if (!type) type = j_str(comp, "class", NULL);
    if (!type) return;

    /* Resolve optional "properties" sub-object (some formats nest data). */
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(comp, "properties");
    if (!cJSON_IsObject(props)) props = comp;

    /* Transform. */
    if (strcmp(type, "Transform") == 0 || strcmp(type, "transform") == 0) {
        parse_transform(s, e, props); return;
    }
    /* Mesh renderer. */
    if (strcmp(type, "MeshRenderer") == 0 || strcmp(type, "meshRenderer") == 0
        || strcmp(type, "Mesh Renderer") == 0 || strcmp(type, "mesh_renderer") == 0) {
        parse_mesh_renderer(s, e, props); return;
    }
    /* Camera. */
    if (strcmp(type, "Camera") == 0 || strcmp(type, "camera") == 0) {
        parse_camera(s, e, props); return;
    }
    /* Separate light types. */
    if (strcmp(type, "DirectionalLight") == 0) { parse_dir_light(s, e, props); return; }
    if (strcmp(type, "PointLight") == 0)       { parse_point_light(s, e, props); return; }
    if (strcmp(type, "SpotLight") == 0)        { parse_spot_light(s, e, props); return; }
    /* Unified Light. */
    if (strcmp(type, "Light") == 0 || strcmp(type, "light") == 0) {
        parse_unified_light(s, e, props); return;
    }
    /* Skybox. */
    if (strcmp(type, "Skybox") == 0 || strcmp(type, "skybox") == 0) {
        parse_skybox(s, e, props); return;
    }
    /* Sprite renderer. */
    if (strcmp(type, "SpriteRenderer") == 0 || strcmp(type, "spriteRenderer") == 0
        || strcmp(type, "Sprite Renderer") == 0 || strcmp(type, "sprite_renderer") == 0) {
        parse_sprite_renderer(s, e, props); return;
    }
    /* Sprite animator. */
    if (strcmp(type, "SpriteAnimator") == 0 || strcmp(type, "spriteAnimator") == 0
        || strcmp(type, "Sprite Animator") == 0) {
        parse_sprite_animator(s, e, props); return;
    }
    /* Animator (non-skeletal). */
    if (strcmp(type, "Animator") == 0 || strcmp(type, "animator") == 0) {
        parse_animator(s, e, props); return;
    }
    /* Skeletal animator. */
    if (strcmp(type, "SkeletalAnimator") == 0 || strcmp(type, "Skeletal Animator") == 0) {
        parse_skeletal_animator(s, e, props); return;
    }
    /* Editor meta. */
    if (strcmp(type, "EditorMeta") == 0) {
        parse_editor_meta(s, e, props); return;
    }
    /* Rigidbody. */
    if (strcmp(type, "Rigidbody") == 0 || strcmp(type, "rigidbody") == 0) {
        parse_rigidbody(s, e, props); return;
    }
    /* Box collider. */
    if (strcmp(type, "BoxCollider") == 0 || strcmp(type, "Box Collider") == 0) {
        parse_box_collider(s, e, props); return;
    }
    /* Sphere collider. */
    if (strcmp(type, "SphereCollider") == 0 || strcmp(type, "Sphere Collider") == 0) {
        parse_sphere_collider(s, e, props); return;
    }
    /* Character controller. */
    if (strcmp(type, "CharacterController") == 0 || strcmp(type, "Character Controller") == 0) {
        parse_character_controller(s, e, props); return;
    }
    /* Audio source. */
    if (strcmp(type, "AudioSource") == 0 || strcmp(type, "Audio Source") == 0) {
        parse_audio_source(s, e, props); return;
    }
    /* Script. */
    if (strcmp(type, "Script") == 0 || strcmp(type, "script") == 0) {
        parse_script(s, e, props); return;
    }
    /* Constraint. */
    if (strcmp(type, "Constraint") == 0 || strcmp(type, "constraint") == 0) {
        parse_constraint(s, e, props); return;
    }
    /* Terrain. */
    if (strcmp(type, "Terrain") == 0 || strcmp(type, "terrain") == 0) {
        parse_terrain(s, e, props); return;
    }
}

/* ── Component serialization ─────────────────────────────────────── */

static void ser_transform(const JceTransform *t, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Transform");
    cJSON_AddNumberToObject(o, "posX", t->position.x);
    cJSON_AddNumberToObject(o, "posY", t->position.y);
    cJSON_AddNumberToObject(o, "posZ", t->position.z);
    float eu[3];
    q_to_euler_deg(t->rotation, eu);
    cJSON_AddNumberToObject(o, "rotX", eu[0]);
    cJSON_AddNumberToObject(o, "rotY", eu[1]);
    cJSON_AddNumberToObject(o, "rotZ", eu[2]);
    cJSON_AddNumberToObject(o, "scaleX", t->scale.x);
    cJSON_AddNumberToObject(o, "scaleY", t->scale.y);
    cJSON_AddNumberToObject(o, "scaleZ", t->scale.z);
    cJSON_AddItemToArray(arr, o);
}

static void ser_mesh_renderer(const JceMeshRenderer *mr, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "MeshRenderer");
    cJSON_AddStringToObject(o, "meshPath", mr->mesh_path);
    cJSON_AddStringToObject(o, "materialPath", mr->material_path);
    cJSON_AddNumberToObject(o, "meshShape", mr->mesh_shape);
    cJSON_AddNumberToObject(o, "baseColorR", mr->base_color[0]);
    cJSON_AddNumberToObject(o, "baseColorG", mr->base_color[1]);
    cJSON_AddNumberToObject(o, "baseColorB", mr->base_color[2]);
    cJSON_AddNumberToObject(o, "baseColorA", mr->base_color[3]);
    cJSON_AddNumberToObject(o, "metallic",  mr->metallic);
    cJSON_AddNumberToObject(o, "roughness", mr->roughness);
    cJSON_AddNumberToObject(o, "emissiveR", mr->emissive[0]);
    cJSON_AddNumberToObject(o, "emissiveG", mr->emissive[1]);
    cJSON_AddNumberToObject(o, "emissiveB", mr->emissive[2]);
    cJSON_AddNumberToObject(o, "normalScale", mr->normal_scale);
    cJSON_AddNumberToObject(o, "aoStrength",  mr->ao_strength);
    cJSON_AddNumberToObject(o, "alphaMode",   mr->alpha_mode);
    cJSON_AddNumberToObject(o, "alphaCutoff", mr->alpha_cutoff);
    cJSON_AddBoolToObject(o, "doubleSided", mr->double_sided);
    if (mr->albedo_tex[0])   cJSON_AddStringToObject(o, "albedoTex",   mr->albedo_tex);
    if (mr->mr_tex[0])       cJSON_AddStringToObject(o, "mrTex",       mr->mr_tex);
    if (mr->normal_tex[0])   cJSON_AddStringToObject(o, "normalTex",   mr->normal_tex);
    if (mr->ao_tex[0])       cJSON_AddStringToObject(o, "aoTex",       mr->ao_tex);
    if (mr->emissive_tex[0]) cJSON_AddStringToObject(o, "emissiveTex", mr->emissive_tex);
    cJSON_AddItemToArray(arr, o);
}

static void ser_camera(const JceCameraComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Camera");
    cJSON_AddNumberToObject(o, "fov", c->fov_deg);
    cJSON_AddNumberToObject(o, "nearClip", c->near_plane);
    cJSON_AddNumberToObject(o, "farClip",  c->far_plane);
    cJSON_AddBoolToObject(o, "primary", c->is_primary);
    cJSON_AddBoolToObject(o, "orthographic", c->ortho);
    cJSON_AddItemToArray(arr, o);
}

/* Unified light serialization: always emits "Light" with lightType sub-field. */
static void ser_light_unified(JceScene *s, JceEntity e, cJSON *arr)
{
    JceDirectionalLight *dl = jce_scene_get_dir_light(s, e);
    if (dl) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "Light");
        cJSON_AddNumberToObject(o, "colorR", dl->color.x);
        cJSON_AddNumberToObject(o, "colorG", dl->color.y);
        cJSON_AddNumberToObject(o, "colorB", dl->color.z);
        cJSON_AddNumberToObject(o, "intensity", dl->intensity);
        cJSON_AddNumberToObject(o, "lightType", 0);
        cJSON_AddBoolToObject(o, "castsShadow", dl->casts_shadow);
        cJSON_AddItemToArray(arr, o);
        return;
    }
    JcePointLight *pl = jce_scene_get_point_light(s, e);
    if (pl) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "Light");
        cJSON_AddNumberToObject(o, "colorR", pl->color.x);
        cJSON_AddNumberToObject(o, "colorG", pl->color.y);
        cJSON_AddNumberToObject(o, "colorB", pl->color.z);
        cJSON_AddNumberToObject(o, "intensity", pl->intensity);
        cJSON_AddNumberToObject(o, "lightType", 1);
        cJSON_AddNumberToObject(o, "radius", pl->radius);
        cJSON_AddBoolToObject(o, "castsShadow", false);
        cJSON_AddItemToArray(arr, o);
        return;
    }
    JceSpotLight *sl = jce_scene_get_spot_light(s, e);
    if (sl) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "Light");
        cJSON_AddNumberToObject(o, "colorR", sl->color.x);
        cJSON_AddNumberToObject(o, "colorG", sl->color.y);
        cJSON_AddNumberToObject(o, "colorB", sl->color.z);
        cJSON_AddNumberToObject(o, "intensity", sl->intensity);
        cJSON_AddNumberToObject(o, "lightType", 2);
        cJSON_AddNumberToObject(o, "radius", sl->radius);
        float inner_deg = acosf(sl->inner_cone_cos) * JCE_RAD2DEG;
        float outer_deg = acosf(sl->outer_cone_cos) * JCE_RAD2DEG;
        cJSON_AddNumberToObject(o, "innerConeDeg", inner_deg);
        cJSON_AddNumberToObject(o, "outerConeDeg", outer_deg);
        cJSON_AddBoolToObject(o, "castsShadow", false);
        cJSON_AddItemToArray(arr, o);
    }
}

static void ser_animator(const JceAnimatorComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Animator");
    cJSON_AddStringToObject(o, "clipName", c->clip_name);
    cJSON_AddNumberToObject(o, "speed", c->speed);
    cJSON_AddBoolToObject(o, "loop", c->loop);
    cJSON_AddItemToArray(arr, o);
}

static void ser_rigidbody(const JceRigidBodyComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Rigidbody");
    cJSON_AddNumberToObject(o, "mass", c->mass);
    cJSON_AddNumberToObject(o, "drag", c->drag);
    cJSON_AddNumberToObject(o, "angularDrag", c->angular_drag);
    cJSON_AddBoolToObject(o, "useGravity", c->use_gravity);
    cJSON_AddBoolToObject(o, "isKinematic", c->is_kinematic);
    cJSON_AddItemToArray(arr, o);
}

static void ser_box_collider(const JceBoxColliderComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "BoxCollider");
    cJSON_AddNumberToObject(o, "centerX", c->center[0]);
    cJSON_AddNumberToObject(o, "centerY", c->center[1]);
    cJSON_AddNumberToObject(o, "centerZ", c->center[2]);
    cJSON_AddNumberToObject(o, "sizeX", c->size[0]);
    cJSON_AddNumberToObject(o, "sizeY", c->size[1]);
    cJSON_AddNumberToObject(o, "sizeZ", c->size[2]);
    cJSON_AddBoolToObject(o, "isTrigger", c->is_trigger);
    cJSON_AddItemToArray(arr, o);
}

static void ser_sphere_collider(const JceSphereColliderComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "SphereCollider");
    cJSON_AddNumberToObject(o, "centerX", c->center[0]);
    cJSON_AddNumberToObject(o, "centerY", c->center[1]);
    cJSON_AddNumberToObject(o, "centerZ", c->center[2]);
    cJSON_AddNumberToObject(o, "radius", c->radius);
    cJSON_AddBoolToObject(o, "isTrigger", c->is_trigger);
    cJSON_AddItemToArray(arr, o);
}

static void ser_character_controller(const JceCharacterControllerComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "CharacterController");
    cJSON_AddNumberToObject(o, "height", c->height);
    cJSON_AddNumberToObject(o, "radius", c->radius);
    cJSON_AddNumberToObject(o, "stepOffset", c->step_offset);
    cJSON_AddNumberToObject(o, "slopeLimit", c->slope_limit);
    cJSON_AddItemToArray(arr, o);
}

static void ser_audio_source(const JceAudioSourceComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "AudioSource");
    cJSON_AddStringToObject(o, "clipPath", c->clip_path);
    cJSON_AddNumberToObject(o, "volume", c->volume);
    cJSON_AddNumberToObject(o, "pitch", c->pitch);
    cJSON_AddNumberToObject(o, "spatialBlend", c->spatial_blend);
    cJSON_AddBoolToObject(o, "loop", c->loop);
    cJSON_AddBoolToObject(o, "playOnAwake", c->play_on_awake);
    cJSON_AddItemToArray(arr, o);
}

static void ser_script(const JceScriptComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Script");
    cJSON_AddStringToObject(o, "scriptPath", c->script_path);
    cJSON_AddItemToArray(arr, o);
}

static void ser_constraint(const JceConstraintComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Constraint");
    cJSON_AddNumberToObject(o, "constraintType", c->constraint_type);
    cJSON_AddNumberToObject(o, "targetEntity", (double)c->target_entity);
    cJSON_AddNumberToObject(o, "pivotAx", c->pivot_a[0]);
    cJSON_AddNumberToObject(o, "pivotAy", c->pivot_a[1]);
    cJSON_AddNumberToObject(o, "pivotAz", c->pivot_a[2]);
    cJSON_AddNumberToObject(o, "pivotBx", c->pivot_b[0]);
    cJSON_AddNumberToObject(o, "pivotBy", c->pivot_b[1]);
    cJSON_AddNumberToObject(o, "pivotBz", c->pivot_b[2]);
    cJSON_AddNumberToObject(o, "axisX", c->axis[0]);
    cJSON_AddNumberToObject(o, "axisY", c->axis[1]);
    cJSON_AddNumberToObject(o, "axisZ", c->axis[2]);
    cJSON_AddNumberToObject(o, "lowerLimit", c->lower_limit);
    cJSON_AddNumberToObject(o, "upperLimit", c->upper_limit);
    cJSON_AddBoolToObject(o, "disableCollision", c->disable_collision);
    cJSON_AddItemToArray(arr, o);
}

static void ser_terrain(const JceTerrainComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Terrain");
    cJSON_AddStringToObject(o, "terrainPath", c->terrain_path);
    cJSON_AddStringToObject(o, "layerAlbedoPath0", c->layer_albedo_path[0]);
    cJSON_AddStringToObject(o, "layerAlbedoPath1", c->layer_albedo_path[1]);
    cJSON_AddStringToObject(o, "layerAlbedoPath2", c->layer_albedo_path[2]);
    cJSON_AddStringToObject(o, "layerAlbedoPath3", c->layer_albedo_path[3]);
    cJSON_AddNumberToObject(o, "tileScale", c->tile_scale);
    cJSON_AddNumberToObject(o, "tintR", c->tint[0]);
    cJSON_AddNumberToObject(o, "tintG", c->tint[1]);
    cJSON_AddNumberToObject(o, "tintB", c->tint[2]);
    cJSON_AddBoolToObject(o, "visible", c->visible);
    cJSON_AddBoolToObject(o, "splatEnabled", c->splat_enabled);
    cJSON_AddItemToArray(arr, o);
}

static void ser_skybox(const JceSkyboxComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Skybox");
    cJSON_AddStringToObject(o, "hdrPath", c->hdr_path);
    cJSON_AddNumberToObject(o, "rotation", c->rotation);
    cJSON_AddNumberToObject(o, "exposure", c->exposure);
    cJSON_AddBoolToObject(o, "useAsIbl", c->use_as_ibl);
    cJSON_AddItemToArray(arr, o);
}

static void ser_sprite_renderer(const JceSpriteRendererComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "SpriteRenderer");
    cJSON_AddStringToObject(o, "spritePath", c->sprite_path);
    cJSON_AddNumberToObject(o, "colorR", c->color[0]);
    cJSON_AddNumberToObject(o, "colorG", c->color[1]);
    cJSON_AddNumberToObject(o, "colorB", c->color[2]);
    cJSON_AddNumberToObject(o, "colorA", c->color[3]);
    cJSON_AddBoolToObject(o, "flipX", c->flip_x);
    cJSON_AddBoolToObject(o, "flipY", c->flip_y);
    cJSON_AddNumberToObject(o, "sortingOrder", c->sorting_order);
    cJSON_AddItemToArray(arr, o);
}

static void ser_sprite_animator(const JceSpriteAnimatorComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "SpriteAnimator");
    cJSON_AddStringToObject(o, "sheetPath", c->sheet_path);
    cJSON_AddStringToObject(o, "atlasPath", c->atlas_path);
    cJSON_AddNumberToObject(o, "frameWidth", c->frame_width);
    cJSON_AddNumberToObject(o, "frameHeight", c->frame_height);
    cJSON_AddStringToObject(o, "currentAnim", c->current_anim);
    cJSON_AddNumberToObject(o, "speed", c->speed);
    cJSON_AddBoolToObject(o, "loop", c->loop);
    cJSON_AddBoolToObject(o, "playing", c->playing);
    cJSON_AddItemToArray(arr, o);
}

static void ser_skeletal_animator(const JceSkeletalAnimatorComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "SkeletalAnimator");
    cJSON_AddStringToObject(o, "skeletonPath", c->skeleton_path);
    cJSON_AddNumberToObject(o, "speed", c->speed);
    cJSON_AddBoolToObject(o, "loop", c->loop);
    cJSON_AddBoolToObject(o, "playing", c->playing);
    cJSON_AddNumberToObject(o, "activeClip", c->active_clip);
    /* Defensive clamp: clip_count must be in [0, ARRAY_LEN]. Garbage
     * here would walk into adjacent memory and crash cJSON_strdup. */
    int cc = c->clip_count;
    if (cc < 0) cc = 0;
    if (cc > (int)(sizeof(c->clip_names) / sizeof(c->clip_names[0])))
        cc = (int)(sizeof(c->clip_names) / sizeof(c->clip_names[0]));
    if (cc > 0) {
        cJSON *clips = cJSON_CreateArray();
        for (int i = 0; i < cc; i++)
            cJSON_AddItemToArray(clips, cJSON_CreateString(c->clip_names[i]));
        cJSON_AddItemToObject(o, "clipNames", clips);
    }
    cJSON_AddItemToArray(arr, o);
}

static void ser_editor_meta(const JceEditorMeta *m, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "EditorMeta");
    cJSON_AddStringToObject(o, "name", m->name);
    /* Only emit non-default fields — keeps runtime scene files small.
     * Loader treats absent fields as defaults: enabled=true, tag="",
     * tagColor=0, prefabInstance=false. */
    if (m->tag[0] != '\0')
        cJSON_AddStringToObject(o, "tag", m->tag);
    if (m->tag_color != 0)
        cJSON_AddNumberToObject(o, "tagColor", m->tag_color);
    if (!m->enabled)
        cJSON_AddBoolToObject(o, "enabled", false);
    if (m->prefab_instance) {
        cJSON_AddBoolToObject(o, "prefabInstance", true);
        if (m->prefab_path[0] != '\0')
            cJSON_AddStringToObject(o, "prefabPath", m->prefab_path);
    }
    cJSON_AddItemToArray(arr, o);
}

/* ── Entity-level (de)serialization ───────────────────────────────── */

typedef struct {
    JceEntity src_id;     /* id from JSON */
    JceEntity new_id;     /* actual created entity */
    JceEntity parent_src; /* parent id from JSON (0 = none) */
} EntityRemap;

typedef struct {
    cJSON       *entities;
    JceScene    *scene;
} SerCtx;

static void ser_entity_cb(JceScene *s, JceEntity e, void *ud)
{
    SerCtx *ctx = (SerCtx *)ud;
    cJSON *eobj = cJSON_CreateObject();
    if (!eobj) return;

    const char *name = jce_scene_entity_registered_name(s, e);
    cJSON_AddNumberToObject(eobj, "id", (double)e);
    cJSON_AddStringToObject(eobj, "name", name ? name : "");
    cJSON_AddNumberToObject(eobj, "parentId", (double)jce_scene_get_parent(s, e));

    cJSON *comps = cJSON_CreateArray();
    if (!comps) { cJSON_Delete(eobj); return; }

    uint32_t f = jce_scene_get_component_flags(s, e);

    if (f & JCE_COMP_FLAG_TRANSFORM) {
        JceTransform *t = jce_scene_get_transform(s, e);
        if (t) ser_transform(t, comps);
    }
    if (f & JCE_COMP_FLAG_MESH_RENDERER) {
        JceMeshRenderer *mr = jce_scene_get_mesh_renderer(s, e);
        if (mr) ser_mesh_renderer(mr, comps);
    }
    if (f & JCE_COMP_FLAG_CAMERA) {
        JceCameraComponent *c = jce_scene_get_camera(s, e);
        if (c) ser_camera(c, comps);
    }
    /* Use unified Light format for all light types. */
    if (f & (JCE_COMP_FLAG_DIR_LIGHT | JCE_COMP_FLAG_POINT_LIGHT |
             JCE_COMP_FLAG_SPOT_LIGHT)) {
        ser_light_unified(s, e, comps);
    }
    if (f & JCE_COMP_FLAG_SKYBOX) {
        JceSkyboxComponent *c = jce_scene_get_skybox(s, e);
        if (c) ser_skybox(c, comps);
    }
    if (f & JCE_COMP_FLAG_SPRITE_RENDERER) {
        JceSpriteRendererComponent *c = jce_scene_get_sprite_renderer(s, e);
        if (c) ser_sprite_renderer(c, comps);
    }
    if (f & JCE_COMP_FLAG_SPRITE_ANIMATOR) {
        JceSpriteAnimatorComponent *c = jce_scene_get_sprite_animator(s, e);
        if (c) ser_sprite_animator(c, comps);
    }
    if (f & JCE_COMP_FLAG_ANIMATOR) {
        JceAnimatorComponent *c = jce_scene_get_animator(s, e);
        if (c) ser_animator(c, comps);
    }
    if (f & JCE_COMP_FLAG_SKELETAL_ANIMATOR) {
        JceSkeletalAnimatorComponent *c = jce_scene_get_skeletal_animator(s, e);
        if (c) ser_skeletal_animator(c, comps);
    }
    if (f & JCE_COMP_FLAG_RIGIDBODY) {
        JceRigidBodyComponent *c = jce_scene_get_rigidbody(s, e);
        if (c) ser_rigidbody(c, comps);
    }
    if (f & JCE_COMP_FLAG_BOX_COLLIDER) {
        JceBoxColliderComponent *c = jce_scene_get_box_collider(s, e);
        if (c) ser_box_collider(c, comps);
    }
    if (f & JCE_COMP_FLAG_SPHERE_COLLIDER) {
        JceSphereColliderComponent *c = jce_scene_get_sphere_collider(s, e);
        if (c) ser_sphere_collider(c, comps);
    }
    if (f & JCE_COMP_FLAG_CHARACTER_CONTROLLER) {
        JceCharacterControllerComponent *c = jce_scene_get_character_controller(s, e);
        if (c) ser_character_controller(c, comps);
    }
    if (f & JCE_COMP_FLAG_AUDIO_SOURCE) {
        JceAudioSourceComponent *c = jce_scene_get_audio_source(s, e);
        if (c) ser_audio_source(c, comps);
    }
    if (f & JCE_COMP_FLAG_SCRIPT) {
        JceScriptComponent *c = jce_scene_get_script(s, e);
        if (c) ser_script(c, comps);
    }
    if (f & JCE_COMP_FLAG_CONSTRAINT) {
        JceConstraintComponent *c = jce_scene_get_constraint(s, e);
        if (c) ser_constraint(c, comps);
    }
    if (f & JCE_COMP_FLAG_TERRAIN) {
        JceTerrainComponent *c = jce_scene_get_terrain(s, e);
        if (c) ser_terrain(c, comps);
    }
    if (f & JCE_COMP_FLAG_EDITOR_META) {
        JceEditorMeta *m = jce_scene_get_editor_meta(s, e);
        if (m) ser_editor_meta(m, comps);
    }

    cJSON_AddItemToObject(eobj, "components", comps);
    cJSON_AddItemToArray(ctx->entities, eobj);
}

/* ── Public API ───────────────────────────────────────────────────── */

cJSON *jce_scene_save_json(const JceScene *scene)
{
    /* Emit the contract envelope format that the editor uses. */
    cJSON *root     = cJSON_CreateObject();
    cJSON *contract = cJSON_CreateObject();
    cJSON *sobj     = cJSON_CreateObject();
    cJSON *entities = cJSON_CreateArray();
    if (!root || !contract || !sobj || !entities) {
        cJSON_Delete(root); cJSON_Delete(contract);
        cJSON_Delete(sobj); cJSON_Delete(entities);
        return NULL;
    }

    cJSON_AddItemToObject(root, "contract", contract);
    cJSON_AddStringToObject(contract, "name", "jce.scene");
    cJSON_AddNumberToObject(contract, "major", 1);
    cJSON_AddNumberToObject(contract, "minor", 0);

    cJSON_AddItemToObject(root, "scene", sobj);
    cJSON_AddNumberToObject(sobj, "version", 1);
    cJSON_AddItemToObject(sobj, "entities", entities);

    if (scene) {
        SerCtx ctx;
        ctx.entities = entities;
        ctx.scene = (JceScene *)scene;
        jce_scene_each_entity((JceScene *)scene, ser_entity_cb, &ctx);
    }

    return root;
}

/* Subtree variant — same envelope, but the entities array contains only
 * `root` and its transitive children. Used by the prefab system. */
static void collect_subtree(const JceScene *scene, JceEntity e, SerCtx *ctx)
{
    ser_entity_cb((JceScene *)scene, e, ctx);

    int n = jce_scene_get_child_count(scene, e);
    if (n <= 0) return;
    /* Use a small stack + heap fallback for very wide hierarchies. */
    JceEntity stack_buf[16];
    JceEntity *kids = stack_buf;
    if ((size_t)n > sizeof(stack_buf) / sizeof(stack_buf[0]))
        kids = (JceEntity *)JCE_MALLOC((size_t)n * sizeof(JceEntity));
    if (!kids) return;

    int got = jce_scene_get_children(scene, e, kids, n);
    for (int i = 0; i < got; i++)
        collect_subtree(scene, kids[i], ctx);

    if (kids != stack_buf) JCE_FREE(kids);
}

cJSON *jce_scene_save_subtree_json(const JceScene *scene, JceEntity root)
{
    cJSON *r        = cJSON_CreateObject();
    cJSON *contract = cJSON_CreateObject();
    cJSON *sobj     = cJSON_CreateObject();
    cJSON *entities = cJSON_CreateArray();
    if (!r || !contract || !sobj || !entities) {
        cJSON_Delete(r); cJSON_Delete(contract);
        cJSON_Delete(sobj); cJSON_Delete(entities);
        return NULL;
    }
    cJSON_AddItemToObject(r, "contract", contract);
    cJSON_AddStringToObject(contract, "name", "jce.scene");
    cJSON_AddNumberToObject(contract, "major", 1);
    cJSON_AddNumberToObject(contract, "minor", 0);
    cJSON_AddItemToObject(r, "scene", sobj);
    cJSON_AddNumberToObject(sobj, "version", 1);
    cJSON_AddItemToObject(sobj, "entities", entities);

    if (scene && root) {
        SerCtx ctx;
        ctx.entities = entities;
        ctx.scene    = (JceScene *)scene;
        collect_subtree(scene, root, &ctx);
    }
    return r;
}
static const cJSON *resolve_entities(const cJSON *root)
{
    if (!root || !cJSON_IsObject(root)) return NULL;

    /* Contract envelope: root.scene.entities */
    const cJSON *scene_obj = cJSON_GetObjectItemCaseSensitive(root, "scene");
    if (cJSON_IsObject(scene_obj)) {
        const cJSON *ents = cJSON_GetObjectItemCaseSensitive(scene_obj, "entities");
        if (cJSON_IsArray(ents)) return ents;
    }

    /* Flat root: root.entities */
    const cJSON *ents = cJSON_GetObjectItemCaseSensitive(root, "entities");
    if (cJSON_IsArray(ents)) return ents;

    return NULL;
}

static bool is_legacy_unnamed_entity_name(const char *name)
{
    return !name || name[0] == '\0' || strcmp(name, "(unnamed)") == 0;
}

int jce_scene_load_json(JceScene *scene, const cJSON *root)
{
    if (!scene || !root) return -1;

    const cJSON *entities = resolve_entities(root);
    if (!entities) {
        LOG_WARN(LOG_TAG, "scene JSON has no 'entities' array");
        return -1;
    }

    int total = cJSON_GetArraySize(entities);
    if (total <= 0) return 0;

    /* Pre-allocate remap table to resolve parent_id references after creation. */
    EntityRemap *map = (EntityRemap *)calloc((size_t)total, sizeof(EntityRemap));
    if (!map) return -1;

    int loaded = 0;

    /* First pass: create entities and parse components. */
    for (int i = 0; i < total; i++) {
        const cJSON *eobj = cJSON_GetArrayItem(entities, i);
        if (!cJSON_IsObject(eobj)) continue;

        const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(eobj, "name");
        const char *name = (cJSON_IsString(name_item) && name_item->valuestring)
                         ? name_item->valuestring
                         : "";
        JceEntity new_e = jce_scene_create_entity(
            scene,
            is_legacy_unnamed_entity_name(name) ? NULL : name);
        if (new_e == JCE_ENTITY_INVALID) continue;

        map[loaded].src_id = (JceEntity)j_num(eobj, "id", 0.0);
        map[loaded].new_id = new_e;
        /* Accept both "parent_id" and "parentId". */
        map[loaded].parent_src = (JceEntity)j_num2(eobj, "parent_id", "parentId", 0.0);
        loaded++;

        /* Apply entity-level EditorMeta fields (enabled, tag, tagColor, etc.).
         * Always create EditorMeta with the entity name so the editor can see
         * hand-authored scenes that omit these optional fields. */
        {
            bool has_enabled = cJSON_GetObjectItemCaseSensitive(eobj, "enabled") != NULL;
            bool has_tag     = cJSON_GetObjectItemCaseSensitive(eobj, "tag") != NULL;
            bool has_tc      = cJSON_GetObjectItemCaseSensitive(eobj, "tagColor") != NULL;
            bool has_pi      = cJSON_GetObjectItemCaseSensitive(eobj, "prefabInstance") != NULL;
            bool has_pp      = cJSON_GetObjectItemCaseSensitive(eobj, "prefabPath") != NULL;
            JceEditorMeta *m = jce_scene_get_editor_meta(scene, new_e);
            if (!m) {
                JceEditorMeta fresh;
                memset(&fresh, 0, sizeof(fresh));
                copy_str(fresh.name, sizeof(fresh.name),
                         is_legacy_unnamed_entity_name(name) ? "Entity" : name);
                fresh.enabled = true;
                jce_scene_set_editor_meta(scene, new_e, &fresh);
                m = jce_scene_get_editor_meta(scene, new_e);
            }
            if (m) {
                if (has_enabled) m->enabled = j_bool(eobj, "enabled", true);
                if (has_tag) copy_str(m->tag, sizeof(m->tag),
                                      j_str(eobj, "tag", ""));
                if (has_tc) m->tag_color = (uint8_t)j_num(eobj, "tagColor", 0);
                if (has_pi) m->prefab_instance = j_bool(eobj, "prefabInstance", false);
                if (has_pp) copy_str(m->prefab_path, sizeof(m->prefab_path),
                                     j_str(eobj, "prefabPath", ""));
            }
        }

        const cJSON *comps = cJSON_GetObjectItemCaseSensitive(eobj, "components");
        if (!comps) comps = cJSON_GetObjectItemCaseSensitive(eobj, "component");
        if (cJSON_IsArray(comps)) {
            int cn = cJSON_GetArraySize(comps);
            for (int k = 0; k < cn; k++) {
                const cJSON *c = cJSON_GetArrayItem(comps, k);
                if (cJSON_IsObject(c))
                    parse_one_component(scene, new_e, c);
            }
        }
    }

    /* Second pass: fix up parent references. */
    for (int i = 0; i < loaded; i++) {
        if (map[i].parent_src == 0) continue;
        for (int j = 0; j < loaded; j++) {
            if (map[j].src_id == map[i].parent_src) {
                jce_scene_set_parent(scene, map[i].new_id, map[j].new_id);
                break;
            }
        }
    }

    free(map);
    return loaded;
}

void jce_scene_parse_entity_json(JceScene *scene, JceEntity e,
                                 const cJSON *entity_obj)
{
    if (!scene || e == JCE_ENTITY_INVALID || !cJSON_IsObject(entity_obj)) return;

    /* Apply entity-level EditorMeta fields if present. */
    {
        bool has_enabled = cJSON_GetObjectItemCaseSensitive(entity_obj, "enabled") != NULL;
        bool has_tag     = cJSON_GetObjectItemCaseSensitive(entity_obj, "tag") != NULL;
        bool has_tc      = cJSON_GetObjectItemCaseSensitive(entity_obj, "tagColor") != NULL;
        bool has_pi      = cJSON_GetObjectItemCaseSensitive(entity_obj, "prefabInstance") != NULL;
        bool has_pp      = cJSON_GetObjectItemCaseSensitive(entity_obj, "prefabPath") != NULL;
        if (has_enabled || has_tag || has_tc || has_pi || has_pp) {
            JceEditorMeta *m = jce_scene_get_editor_meta(scene, e);
            if (!m) {
                JceEditorMeta fresh;
                memset(&fresh, 0, sizeof(fresh));
                const char *n = j_str(entity_obj, "name", "");
                copy_str(fresh.name, sizeof(fresh.name),
                         is_legacy_unnamed_entity_name(n) ? "Entity" : n);
                fresh.enabled = true;
                jce_scene_set_editor_meta(scene, e, &fresh);
                m = jce_scene_get_editor_meta(scene, e);
            }
            if (m) {
                if (has_enabled) m->enabled = j_bool(entity_obj, "enabled", true);
                if (has_tag) copy_str(m->tag, sizeof(m->tag),
                                      j_str(entity_obj, "tag", ""));
                if (has_tc) m->tag_color = (uint8_t)j_num(entity_obj, "tagColor", 0);
                if (has_pi) m->prefab_instance = j_bool(entity_obj, "prefabInstance", false);
                if (has_pp) copy_str(m->prefab_path, sizeof(m->prefab_path),
                                     j_str(entity_obj, "prefabPath", ""));
            }
        }
    }

    /* Parse components array. */
    const cJSON *comps = cJSON_GetObjectItemCaseSensitive(entity_obj, "components");
    if (!comps)
        comps = cJSON_GetObjectItemCaseSensitive(entity_obj, "component");
    if (cJSON_IsArray(comps)) {
        int cn = cJSON_GetArraySize(comps);
        for (int k = 0; k < cn; k++) {
            const cJSON *c = cJSON_GetArrayItem(comps, k);
            if (cJSON_IsObject(c))
                parse_one_component(scene, e, c);
        }
    }
}

cJSON *jce_scene_serialize_entity_components(JceScene *scene, JceEntity e)
{
    if (!scene || e == JCE_ENTITY_INVALID) return NULL;

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    uint32_t f = jce_scene_get_component_flags(scene, e);

    if (f & JCE_COMP_FLAG_TRANSFORM) {
        JceTransform *t = jce_scene_get_transform(scene, e);
        if (t) ser_transform(t, arr);
    }
    if (f & JCE_COMP_FLAG_MESH_RENDERER) {
        JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
        if (mr) ser_mesh_renderer(mr, arr);
    }
    if (f & JCE_COMP_FLAG_CAMERA) {
        JceCameraComponent *c = jce_scene_get_camera(scene, e);
        if (c) ser_camera(c, arr);
    }
    if (f & (JCE_COMP_FLAG_DIR_LIGHT | JCE_COMP_FLAG_POINT_LIGHT |
             JCE_COMP_FLAG_SPOT_LIGHT)) {
        ser_light_unified(scene, e, arr);
    }
    if (f & JCE_COMP_FLAG_SKYBOX) {
        JceSkyboxComponent *c = jce_scene_get_skybox(scene, e);
        if (c) ser_skybox(c, arr);
    }
    if (f & JCE_COMP_FLAG_SPRITE_RENDERER) {
        JceSpriteRendererComponent *c = jce_scene_get_sprite_renderer(scene, e);
        if (c) ser_sprite_renderer(c, arr);
    }
    if (f & JCE_COMP_FLAG_SPRITE_ANIMATOR) {
        JceSpriteAnimatorComponent *c = jce_scene_get_sprite_animator(scene, e);
        if (c) ser_sprite_animator(c, arr);
    }
    if (f & JCE_COMP_FLAG_ANIMATOR) {
        JceAnimatorComponent *c = jce_scene_get_animator(scene, e);
        if (c) ser_animator(c, arr);
    }
    if (f & JCE_COMP_FLAG_SKELETAL_ANIMATOR) {
        JceSkeletalAnimatorComponent *c = jce_scene_get_skeletal_animator(scene, e);
        if (c) ser_skeletal_animator(c, arr);
    }
    if (f & JCE_COMP_FLAG_RIGIDBODY) {
        JceRigidBodyComponent *c = jce_scene_get_rigidbody(scene, e);
        if (c) ser_rigidbody(c, arr);
    }
    if (f & JCE_COMP_FLAG_BOX_COLLIDER) {
        JceBoxColliderComponent *c = jce_scene_get_box_collider(scene, e);
        if (c) ser_box_collider(c, arr);
    }
    if (f & JCE_COMP_FLAG_SPHERE_COLLIDER) {
        JceSphereColliderComponent *c = jce_scene_get_sphere_collider(scene, e);
        if (c) ser_sphere_collider(c, arr);
    }
    if (f & JCE_COMP_FLAG_CHARACTER_CONTROLLER) {
        JceCharacterControllerComponent *c = jce_scene_get_character_controller(scene, e);
        if (c) ser_character_controller(c, arr);
    }
    if (f & JCE_COMP_FLAG_AUDIO_SOURCE) {
        JceAudioSourceComponent *c = jce_scene_get_audio_source(scene, e);
        if (c) ser_audio_source(c, arr);
    }
    if (f & JCE_COMP_FLAG_SCRIPT) {
        JceScriptComponent *c = jce_scene_get_script(scene, e);
        if (c) ser_script(c, arr);
    }
    if (f & JCE_COMP_FLAG_CONSTRAINT) {
        JceConstraintComponent *c = jce_scene_get_constraint(scene, e);
        if (c) ser_constraint(c, arr);
    }
    if (f & JCE_COMP_FLAG_TERRAIN) {
        JceTerrainComponent *c = jce_scene_get_terrain(scene, e);
        if (c) ser_terrain(c, arr);
    }
    if (f & JCE_COMP_FLAG_EDITOR_META) {
        JceEditorMeta *m = jce_scene_get_editor_meta(scene, e);
        if (m) ser_editor_meta(m, arr);
    }

    return arr;
}

