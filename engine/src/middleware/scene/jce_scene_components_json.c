/*
 * jce_scene_serial.c  Engine scene JSON serializer/parser.
 *
 * Flat-entity-array format used by both editor and runtime. The editor
 * supplements with backwards-compat parsing on top of these primitives.
 */

#include "jce_scene_components_internal.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_thread.h>

#define LOG_TAG "scene_serial"

/* Relative asset resolution is scoped to the parsing thread: async scene
 * loading must never make another scene resolve materials against its path. */
typedef struct SceneSerialContext {
    char                 base_dir[1024];
    const JceFileSystem *asset_fs;
} SceneSerialContext;

static JceTLS *s_scene_context_tls = NULL;
static SceneSerialContext s_scene_context_fallback;

static void sse_context_destroy(void *value)
{
    JCE_FREE(value);
}

static SceneSerialContext *sse_context_get(void)
{
    if (!s_scene_context_tls)
        s_scene_context_tls = jce_tls_create(sse_context_destroy);
    if (!s_scene_context_tls)
        return &s_scene_context_fallback;

    SceneSerialContext *context =
        (SceneSerialContext *)jce_tls_get(s_scene_context_tls);
    if (!context) {
        context = (SceneSerialContext *)JCE_CALLOC(1, sizeof(*context));
        if (!context)
            return &s_scene_context_fallback;
        jce_tls_set(s_scene_context_tls, context);
    }
    return context;
}

#define SSE_SCENE_BASE_DIR (sse_context_get()->base_dir)

void jce_scene_serial_set_base_dir(const char *dir)
{
    char *base_dir = SSE_SCENE_BASE_DIR;
    if (!dir || !*dir) {
        base_dir[0] = '\0';
        return;
    }
    size_t L = strlen(dir);
    if (L >= sizeof(SSE_SCENE_BASE_DIR)) L = sizeof(SSE_SCENE_BASE_DIR) - 1;
    memcpy(base_dir, dir, L);
    base_dir[L] = '\0';
    /* Strip trailing slash for consistent join with snprintf("%s/%s"). */
    while (L > 0 && (base_dir[L-1] == '/' || base_dir[L-1] == '\\')) {
        base_dir[--L] = '\0';
    }
}

void jce_scene_serial_set_asset_vfs(const JceFileSystem *fs)
{
    sse_context_get()->asset_fs = fs;
}

/* Absolute-path test: use the public jce_path_is_absolute() (jce_path.h is
 * already included) — the previous local sse_ copy duplicated it. */

static bool sse_file_exists(const char *p)
{
    if (!p || !*p) return false;
    const JceFileSystem *fs = sse_context_get()->asset_fs;
    if (fs) return jce_fs_exists(fs, p);
    SDL_PathInfo info;
    return SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_FILE;
}

static bool sse_dir_exists(const char *p)
{
    if (!p || !*p) return false;
    if (sse_context_get()->asset_fs) return false;
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
        jce_path_join(st->best, sizeof(st->best), dirpath, name);
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
    jce_path_join(full, sizeof(full), dirpath, fname);

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
    if (SSE_SCENE_BASE_DIR[0] == '\0') return false;

    struct FallbackMatScan st;
    st.best[0] = '\0';
    st.best_score = 0;
    st.depth = 0;
    st.mesh_hint = (mesh_hint && *mesh_hint) ? mesh_hint : NULL;

    char base[1024];
    snprintf(base, sizeof(base), "%s", SSE_SCENE_BASE_DIR);

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
    if (jce_path_is_absolute(tex_path) && sse_file_exists(tex_path)) return;

    char mat_dir[1024];
    snprintf(mat_dir, sizeof(mat_dir), "%s", mat_path);
    if (!sse_path_parent_inplace(mat_dir)) return;

    char candidate[1280];
    /* (1) sibling of the .mat.json. */
    jce_path_join(candidate, sizeof(candidate), mat_dir, tex_path);
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
        jce_path_join(candidate, sizeof(candidate), base, tex_path);
        if (sse_file_exists(candidate)) {
            snprintf(tex_path, tex_path_sz, "%s", candidate);
            return;
        }
    }
}


static cJSON *ser_scene_rendering_settings(
    const JceSceneRenderingSettings *r)
{
    if (!r) return NULL;

    cJSON *root = cJSON_CreateObject();
    cJSON *ambient = cJSON_CreateObject();
    cJSON *fog = cJSON_CreateObject();
    cJSON *shadows = cJSON_CreateObject();
    cJSON *postfx = cJSON_CreateObject();
    if (!root || !ambient || !fog || !shadows || !postfx) {
        cJSON_Delete(root);
        cJSON_Delete(ambient);
        cJSON_Delete(fog);
        cJSON_Delete(shadows);
        cJSON_Delete(postfx);
        return NULL;
    }

    cJSON_AddNumberToObject(root, "version", (double)r->version);

    cJSON_AddItemToObject(ambient, "color", json_float3(r->ambient_color));
    cJSON_AddNumberToObject(ambient, "intensity", r->ambient_intensity);
    cJSON_AddItemToObject(root, "ambient", ambient);

    cJSON_AddBoolToObject(fog, "enabled", r->fog_enabled);
    cJSON_AddNumberToObject(fog, "mode", r->fog_mode);
    cJSON_AddItemToObject(fog, "color", json_float3(r->fog_color));
    cJSON_AddNumberToObject(fog, "density", r->fog_density);
    cJSON_AddNumberToObject(fog, "start", r->fog_start);
    cJSON_AddNumberToObject(fog, "end", r->fog_end);
    cJSON_AddNumberToObject(fog, "heightFalloff", r->fog_height_falloff);
    cJSON_AddNumberToObject(fog, "heightOrigin", r->fog_height_origin);
    cJSON_AddItemToObject(root, "fog", fog);
    cJSON_AddBoolToObject(root, "iblEnabled", r->ibl_enabled);
    cJSON_AddNumberToObject(root, "dynamicGI", r->gi_dynamic);

    cJSON_AddNumberToObject(shadows, "distance", r->shadow_distance);
    cJSON_AddNumberToObject(shadows, "cascades", r->cascade_count);
    cJSON_AddNumberToObject(shadows, "splitLambda", r->split_lambda);
    cJSON_AddNumberToObject(shadows, "resolution", r->shadow_resolution);
    cJSON_AddNumberToObject(shadows, "soft", r->soft_shadow_mode);
    cJSON_AddItemToObject(root, "shadows", shadows);

    cJSON_AddBoolToObject(postfx, "tonemap", r->postfx_enabled[0]);
    cJSON_AddBoolToObject(postfx, "bloom", r->postfx_enabled[1]);
    cJSON_AddBoolToObject(postfx, "fxaa", r->postfx_enabled[2]);
    cJSON_AddBoolToObject(postfx, "vignette", r->postfx_enabled[3]);
    cJSON_AddBoolToObject(postfx, "chromatic", r->postfx_enabled[4]);
    cJSON_AddBoolToObject(postfx, "grayscale", r->postfx_enabled[5]);
    cJSON_AddNumberToObject(postfx, "exposure", r->exposure);
    cJSON_AddNumberToObject(postfx, "gamma", r->gamma);
    cJSON_AddNumberToObject(postfx, "bloomThreshold", r->bloom_threshold);
    cJSON_AddNumberToObject(postfx, "bloomIntensity", r->bloom_intensity);
    cJSON_AddNumberToObject(postfx, "fxaaSpanMax", r->fxaa_span_max);
    cJSON_AddNumberToObject(postfx, "vignetteIntensity", r->vignette_intensity);
    cJSON_AddNumberToObject(postfx, "vignetteSmoothness", r->vignette_smoothness);
    cJSON_AddNumberToObject(postfx, "chromaticStrength", r->chromatic_strength);

    /* Generic custom post pass (engine style-agnostic). */
    cJSON_AddBoolToObject(postfx, "custom", r->postfx_enabled[6]);
    if (r->custom_post_shader[0]) {
        cJSON_AddStringToObject(postfx, "customPostShader", r->custom_post_shader);
        cJSON_AddBoolToObject(postfx, "customPostDepth", r->custom_post_needs_depth);
        cJSON *cparr = cJSON_CreateArray();
        int cn = r->custom_post_param_count;
        if (cn > 8) cn = 8;
        for (int ci = 0; ci < cn * 4; ci++)
            cJSON_AddItemToArray(cparr, cJSON_CreateNumber(r->custom_post_params[ci]));
        cJSON_AddItemToObject(postfx, "customPostParams", cparr);
    }

    /* Screen-space AO (off by default). */
    cJSON_AddBoolToObject(postfx, "ssao", r->ssao_enabled);
    cJSON_AddNumberToObject(postfx, "ssaoIntensity", r->ssao_intensity);
    cJSON_AddNumberToObject(postfx, "ssaoRadius", r->ssao_radius);
    cJSON_AddBoolToObject(postfx, "ssr", r->ssr_enabled);
    cJSON_AddNumberToObject(postfx, "ssrIntensity", r->ssr_intensity);
    cJSON_AddNumberToObject(postfx, "ssrMaxDistance", r->ssr_max_distance);
    cJSON_AddNumberToObject(postfx, "taaFeedback",    r->taa_feedback);
    cJSON_AddNumberToObject(postfx, "taaLumaClamp",   r->taa_luma_clamp);
    cJSON_AddNumberToObject(postfx, "taaMotionClamp", r->taa_motion_clamp);

    cJSON_AddItemToObject(root, "postfx", postfx);

    /* ── Environment: time-of-day + weather (P2-weather-decals-tod) ──── */
    cJSON *env = cJSON_CreateObject();
    if (env) {
        cJSON *tod = cJSON_CreateObject();
        if (tod) {
            cJSON_AddBoolToObject(tod, "enabled", r->tod_enabled);
            cJSON_AddNumberToObject(tod, "hour", r->tod_hour);
            cJSON_AddNumberToObject(tod, "speed", r->tod_speed);
            cJSON_AddNumberToObject(tod, "latitude", r->tod_latitude);
            cJSON_AddNumberToObject(tod, "dawnHour", r->tod_dawn_hour);
            cJSON_AddNumberToObject(tod, "duskHour", r->tod_dusk_hour);
            cJSON_AddItemToObject(env, "timeOfDay", tod);
        }
        cJSON *weather = cJSON_CreateObject();
        if (weather) {
            cJSON_AddNumberToObject(weather, "type", r->weather_type);
            cJSON_AddNumberToObject(weather, "intensity", r->weather_intensity);
            cJSON_AddItemToObject(env, "weather", weather);
        }
        if (r->temperature_c != 15.0f)
            cJSON_AddNumberToObject(env, "temperatureC", r->temperature_c);
        if (r->wind_direction_x != 0.0f || r->wind_direction_z != 0.0f) {
            cJSON *wind = cJSON_CreateObject();
            if (wind) {
                cJSON_AddNumberToObject(wind, "directionX", r->wind_direction_x);
                cJSON_AddNumberToObject(wind, "directionZ", r->wind_direction_z);
                cJSON_AddItemToObject(env, "wind", wind);
            }
        }
        cJSON *sky = cJSON_CreateObject();
        if (sky) {
            cJSON_AddNumberToObject(sky, "mode", r->sky_mode);
            cJSON_AddNumberToObject(sky, "turbidity", r->sky_turbidity);
            cJSON_AddNumberToObject(sky, "cloudCoverage", r->cloud_coverage);
            cJSON_AddNumberToObject(sky, "cloudDensity",  r->cloud_density);
            cJSON_AddNumberToObject(sky, "cloudBottomKm", r->cloud_bottom_km);
            cJSON_AddNumberToObject(sky, "cloudTopKm",    r->cloud_top_km);
            cJSON_AddNumberToObject(sky, "styliseBands",      r->sky_stylise_bands);
            cJSON_AddNumberToObject(sky, "styliseRim",        r->sky_stylise_rim);
            cJSON_AddNumberToObject(sky, "styliseSaturation", r->sky_stylise_saturation);
            cJSON_AddItemToObject(sky, "tintShadow", json_float3(r->sky_tint_shadow));
            cJSON_AddItemToObject(sky, "tintMid",    json_float3(r->sky_tint_mid));
            cJSON_AddItemToObject(sky, "tintHigh",   json_float3(r->sky_tint_high));
            /* Stylized dome (absent in old scenes → parse fills defaults). */
            cJSON *dome = cJSON_CreateObject();
            if (dome) {
                cJSON_AddItemToObject(dome, "zenith",  json_float3(r->sky_dome_zenith));
                cJSON_AddItemToObject(dome, "mid",     json_float3(r->sky_dome_mid));
                cJSON_AddNumberToObject(dome, "midPos", r->sky_dome_mid_pos);
                cJSON_AddItemToObject(dome, "horizon", json_float3(r->sky_dome_horizon));
                cJSON_AddItemToObject(dome, "ground",  json_float3(r->sky_dome_ground));
                cJSON_AddItemToObject(dome, "glow",    json_float3(r->sky_dome_glow));
                cJSON_AddNumberToObject(dome, "glowFalloff",  r->sky_dome_glow_falloff);
                cJSON_AddItemToObject(dome, "sunColor", json_float3(r->sky_dome_sun_color));
                cJSON_AddNumberToObject(dome, "sunSize",      r->sky_dome_sun_size);
                cJSON_AddNumberToObject(dome, "sunSoftness",  r->sky_dome_sun_softness);
                cJSON_AddNumberToObject(dome, "haloPower",    r->sky_dome_halo_power);
                cJSON_AddNumberToObject(dome, "haloStrength", r->sky_dome_halo_strength);
                cJSON_AddNumberToObject(dome, "anchorRadius",    r->sky_dome_anchor_radius);
                cJSON_AddNumberToObject(dome, "sunRayCount",     r->sky_dome_ray_count);
                cJSON_AddNumberToObject(dome, "sunRayLength",    r->sky_dome_ray_length);
                cJSON_AddNumberToObject(dome, "sunRaySharpness", r->sky_dome_ray_sharpness);
                cJSON_AddNumberToObject(dome, "sunRayStrength",  r->sky_dome_ray_strength);
                cJSON_AddItemToObject(dome, "sunDir", json_float3(r->sky_dome_sun_dir));
                cJSON_AddItemToObject(sky, "dome", dome);
            }
            cJSON_AddItemToObject(env, "sky", sky);
        }
        /* Floating origin (opt-in; absent → disabled, old scenes byte-id). */
        cJSON *fo = cJSON_CreateObject();
        if (fo) {
            cJSON_AddBoolToObject(fo, "enabled", r->floating_origin_enabled);
            cJSON_AddNumberToObject(fo, "threshold",
                                    r->floating_origin_threshold);
            cJSON_AddItemToObject(env, "floatingOrigin", fo);
        }
        cJSON_AddItemToObject(root, "environment", env);
    }

    /* ── Look Profile (stylized slice plan 02) ─────────────────────────
     * Nested object; absent in old files → parse keeps neutral defaults. */
    cJSON *look = cJSON_CreateObject();
    if (look) {
        cJSON_AddNumberToObject(look, "wrap", r->wrap_factor);
        cJSON_AddBoolToObject(look, "hemisphere", r->ambient_hemisphere);
        cJSON_AddItemToObject(look, "groundColor",
                              json_float3(r->ambient_ground_color));
        cJSON_AddItemToObject(look, "rimColor", json_float3(r->rim_color));
        cJSON_AddNumberToObject(look, "rimPower", r->rim_power);
        cJSON_AddNumberToObject(look, "rimIntensity", r->rim_intensity);
        cJSON_AddNumberToObject(look, "tonemapOp", r->tonemap_op);
        if (r->lut_path[0])
            cJSON_AddStringToObject(look, "lutPath", r->lut_path);
        cJSON_AddNumberToObject(look, "lutStrength", r->lut_strength);
        cJSON_AddBoolToObject(look, "toonCharacter", r->toon_character);
        cJSON_AddNumberToObject(look, "bloomKnee", r->bloom_knee);
        cJSON_AddItemToObject(root, "look", look);
    }

    return root;
}

/* ── Scene-level world-streaming settings ("streaming" block) ──────── */

static cJSON *ser_scene_streaming_settings(
    const JceSceneStreamingSettings *st)
{
    if (!st) return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "version", (double)st->version);
    cJSON_AddBoolToObject(root, "enabled", st->enabled);
    cJSON_AddNumberToObject(root, "mode", st->mode);
    cJSON_AddNumberToObject(root, "loadRadius", st->load_radius);
    cJSON_AddNumberToObject(root, "unloadRadius", st->unload_radius);
    cJSON_AddNumberToObject(root, "maxPending", (double)st->max_pending);
    cJSON_AddNumberToObject(root, "budgetMb", (double)st->budget_mb);
    cJSON_AddNumberToObject(root, "frameBudgetMs", st->frame_budget_ms);

    cJSON *chunks = cJSON_CreateArray();
    if (chunks) {
        uint32_t n = st->chunk_count;
        if (n > JCE_SCENE_MAX_STREAM_CHUNKS)
            n = JCE_SCENE_MAX_STREAM_CHUNKS;
        for (uint32_t i = 0; i < n; i++) {
            const JceSceneStreamChunk *c = &st->chunks[i];
            cJSON *co = cJSON_CreateObject();
            if (!co) continue;
            cJSON_AddNumberToObject(co, "id", (double)c->id);
            cJSON_AddItemToObject(co, "center", json_float3(c->center));
            cJSON_AddNumberToObject(co, "radius", c->radius);
            cJSON_AddStringToObject(co, "path", c->path);
            cJSON_AddItemToArray(chunks, co);
        }
        cJSON_AddItemToObject(root, "chunks", chunks);
    }
    return root;
}

static const cJSON *scene_root_object(const cJSON *root)
{
    if (!root || !cJSON_IsObject(root)) return NULL;
    const cJSON *scene_obj = cJSON_GetObjectItemCaseSensitive(root, "scene");
    return cJSON_IsObject(scene_obj) ? scene_obj : root;
}

static bool parse_scene_streaming_settings(JceScene *scene,
                                           const cJSON *root)
{
    const cJSON *scene_obj = scene_root_object(root);
    if (!scene_obj) return false;

    const cJSON *src = cJSON_GetObjectItemCaseSensitive(scene_obj, "streaming");
    if (!cJSON_IsObject(src))
        return false;

    /* ~70 KB with the full chunk table — keep it off the stack. */
    JceSceneStreamingSettings *st = (JceSceneStreamingSettings *)
        JCE_MALLOC(sizeof(*st));
    if (!st) return false;
    *st = jce_scene_streaming_settings_default();

    st->version         = (uint32_t)j_num(src, "version", 1.0);
    st->enabled         = j_bool(src, "enabled", st->enabled);
    st->mode            = (int)j_num(src, "mode", st->mode);
    st->load_radius     = (float)j_num(src, "loadRadius", st->load_radius);
    st->unload_radius   = (float)j_num(src, "unloadRadius", st->unload_radius);
    st->max_pending     = (uint32_t)j_num(src, "maxPending", st->max_pending);
    st->budget_mb       = (uint32_t)j_num(src, "budgetMb", st->budget_mb);
    st->frame_budget_ms =
        (float)j_num(src, "frameBudgetMs", st->frame_budget_ms);

    const cJSON *chunks = cJSON_GetObjectItemCaseSensitive(src, "chunks");
    if (cJSON_IsArray(chunks)) {
        int total = cJSON_GetArraySize(chunks);
        if (total > JCE_SCENE_MAX_STREAM_CHUNKS) {
            LOG_WARN(LOG_TAG,
                     "scene streaming: %d chunks exceed the %d-chunk limit — "
                     "extra entries dropped",
                     total, JCE_SCENE_MAX_STREAM_CHUNKS);
            total = JCE_SCENE_MAX_STREAM_CHUNKS;
        }
        uint32_t n = 0;
        for (int i = 0; i < total; i++) {
            const cJSON *co = cJSON_GetArrayItem(chunks, i);
            if (!cJSON_IsObject(co)) continue;
            JceSceneStreamChunk *c = &st->chunks[n];
            memset(c, 0, sizeof(*c));
            c->id     = (uint32_t)j_num(co, "id", 0.0);
            j_float3(co, "center", c->center, c->center);
            c->radius = (float)j_num(co, "radius", 0.0);
            copy_str(c->path, sizeof(c->path), j_str(co, "path", ""));
            n++;
        }
        st->chunk_count = n;
    }

    jce_scene_set_streaming_settings(scene, st);   /* copies + sanitizes */
    JCE_FREE(st);
    return true;
}

/* Parse all sky-dome sub-keys from a "dome" cJSON object into `r`.
 * Called from both the "environment.sky.dome" and top-level "sky.dome" paths
 * so that adding a new dome field only requires one edit site. */
static void parse_dome_into(const cJSON *dome, JceSceneRenderingSettings *r)
{
    j_float3(dome, "zenith",  r->sky_dome_zenith,  r->sky_dome_zenith);
    j_float3(dome, "mid",     r->sky_dome_mid,     r->sky_dome_mid);
    r->sky_dome_mid_pos      = (float)j_num(dome, "midPos",      r->sky_dome_mid_pos);
    j_float3(dome, "horizon", r->sky_dome_horizon, r->sky_dome_horizon);
    j_float3(dome, "ground",  r->sky_dome_ground,  r->sky_dome_ground);
    j_float3(dome, "glow",    r->sky_dome_glow,    r->sky_dome_glow);
    r->sky_dome_glow_falloff = (float)j_num(dome, "glowFalloff", r->sky_dome_glow_falloff);
    j_float3(dome, "sunColor", r->sky_dome_sun_color, r->sky_dome_sun_color);
    r->sky_dome_sun_size     = (float)j_num(dome, "sunSize",     r->sky_dome_sun_size);
    r->sky_dome_sun_softness = (float)j_num(dome, "sunSoftness", r->sky_dome_sun_softness);
    r->sky_dome_halo_power   = (float)j_num(dome, "haloPower",   r->sky_dome_halo_power);
    r->sky_dome_halo_strength= (float)j_num(dome, "haloStrength",r->sky_dome_halo_strength);
    /* Stylized sun rays (absent = defaults: count 0 = feature off). */
    r->sky_dome_anchor_radius= (float)j_num(dome, "anchorRadius",   r->sky_dome_anchor_radius);
    r->sky_dome_ray_count    = (float)j_num(dome, "sunRayCount",    r->sky_dome_ray_count);
    r->sky_dome_ray_length   = (float)j_num(dome, "sunRayLength",   r->sky_dome_ray_length);
    r->sky_dome_ray_sharpness= (float)j_num(dome, "sunRaySharpness",r->sky_dome_ray_sharpness);
    r->sky_dome_ray_strength = (float)j_num(dome, "sunRayStrength", r->sky_dome_ray_strength);
    /* Optional authored disk direction (sun/moon). Absent = zero = legacy. */
    j_float3(dome, "sunDir",  r->sky_dome_sun_dir, r->sky_dome_sun_dir);
}

/* Extract all rendering settings from a "rendering" (or "lighting") cJSON
 * object.  Seeds from defaults so absent keys keep the golden-hour/neutral
 * values.  Shared by the full scene parse and the public wrapper below. */
static JceSceneRenderingSettings extract_rendering_settings_from_obj(
    const cJSON *src)
{
    JceSceneRenderingSettings r = jce_scene_rendering_settings_default();
    r.version = (uint32_t)j_num(src, "version", 1.0);

    const cJSON *ambient = cJSON_GetObjectItemCaseSensitive(src, "ambient");
    if (cJSON_IsObject(ambient)) {
        j_float3(ambient, "color", r.ambient_color, r.ambient_color);
        r.ambient_intensity =
            (float)j_num(ambient, "intensity", r.ambient_intensity);
    }

    r.ibl_enabled = j_bool(src, "iblEnabled", r.ibl_enabled);
    r.gi_dynamic  = (float)j_num(src, "dynamicGI", r.gi_dynamic);

    const cJSON *fog = cJSON_GetObjectItemCaseSensitive(src, "fog");
    if (cJSON_IsObject(fog)) {
        r.fog_enabled = j_bool(fog, "enabled", r.fog_enabled);
        r.fog_mode = (int)j_num(fog, "mode", r.fog_mode);
        j_float3(fog, "color", r.fog_color, r.fog_color);
        r.fog_density =
            (float)j_num(fog, "density", r.fog_density);
        r.fog_start =
            (float)j_num(fog, "start", r.fog_start);
        r.fog_end =
            (float)j_num(fog, "end", r.fog_end);
        r.fog_height_falloff =
            (float)j_num(fog, "heightFalloff", r.fog_height_falloff);
        r.fog_height_origin =
            (float)j_num(fog, "heightOrigin", r.fog_height_origin);
    }

    const cJSON *shadows = cJSON_GetObjectItemCaseSensitive(src, "shadows");
    if (cJSON_IsObject(shadows)) {
        r.shadow_distance =
            (float)j_num(shadows, "distance", r.shadow_distance);
        r.cascade_count =
            (int)j_num(shadows, "cascades", r.cascade_count);
        r.split_lambda =
            (float)j_num(shadows, "splitLambda", r.split_lambda);
        r.shadow_resolution =
            (int)j_num(shadows, "resolution", r.shadow_resolution);
        r.soft_shadow_mode =
            (int)j_num(shadows, "soft", r.soft_shadow_mode);
    }

    const cJSON *postfx = cJSON_GetObjectItemCaseSensitive(src, "postfx");
    if (cJSON_IsObject(postfx)) {
        r.postfx_enabled[0] = j_bool(postfx, "tonemap", r.postfx_enabled[0]);
        r.postfx_enabled[1] = j_bool(postfx, "bloom", r.postfx_enabled[1]);
        r.postfx_enabled[2] = j_bool(postfx, "fxaa", r.postfx_enabled[2]);
        r.postfx_enabled[3] = j_bool(postfx, "vignette", r.postfx_enabled[3]);
        r.postfx_enabled[4] = j_bool(postfx, "chromatic", r.postfx_enabled[4]);
        r.postfx_enabled[5] = j_bool(postfx, "grayscale", r.postfx_enabled[5]);
        r.exposure =
            (float)j_num(postfx, "exposure", r.exposure);
        r.gamma =
            (float)j_num(postfx, "gamma", r.gamma);
        r.bloom_threshold =
            (float)j_num(postfx, "bloomThreshold", r.bloom_threshold);
        r.bloom_intensity =
            (float)j_num(postfx, "bloomIntensity", r.bloom_intensity);
        r.fxaa_span_max =
            (float)j_num(postfx, "fxaaSpanMax", r.fxaa_span_max);
        r.vignette_intensity =
            (float)j_num(postfx, "vignetteIntensity", r.vignette_intensity);
        r.vignette_smoothness =
            (float)j_num(postfx, "vignetteSmoothness", r.vignette_smoothness);
        r.chromatic_strength =
            (float)j_num(postfx, "chromaticStrength", r.chromatic_strength);

        /* Screen-space AO (off by default; missing keys keep defaults). */
        r.ssao_enabled   = j_bool(postfx, "ssao", r.ssao_enabled);
        r.ssao_intensity = (float)j_num(postfx, "ssaoIntensity", r.ssao_intensity);
        r.ssao_radius    = (float)j_num(postfx, "ssaoRadius", r.ssao_radius);
        r.ssr_enabled      = j_bool(postfx, "ssr", r.ssr_enabled);
        r.ssr_intensity    = (float)j_num(postfx, "ssrIntensity", r.ssr_intensity);
        r.ssr_max_distance = (float)j_num(postfx, "ssrMaxDistance", r.ssr_max_distance);
        r.taa_feedback     = (float)j_num(postfx, "taaFeedback",    r.taa_feedback);
        r.taa_luma_clamp   = (float)j_num(postfx, "taaLumaClamp",   r.taa_luma_clamp);
        r.taa_motion_clamp = (float)j_num(postfx, "taaMotionClamp", r.taa_motion_clamp);

        /* Generic custom post pass (engine style-agnostic). */
        r.postfx_enabled[6] = j_bool(postfx, "custom", r.postfx_enabled[6]);
        const cJSON *cshader =
            cJSON_GetObjectItemCaseSensitive(postfx, "customPostShader");
        if (cJSON_IsString(cshader) && cshader->valuestring) {
            size_t ci = 0;
            for (; cshader->valuestring[ci] &&
                   ci + 1 < sizeof(r.custom_post_shader); ci++)
                r.custom_post_shader[ci] = cshader->valuestring[ci];
            r.custom_post_shader[ci] = '\0';
        }
        r.custom_post_needs_depth =
            j_bool(postfx, "customPostDepth", r.custom_post_needs_depth);
        const cJSON *cparr =
            cJSON_GetObjectItemCaseSensitive(postfx, "customPostParams");
        if (cJSON_IsArray(cparr)) {
            int cn = cJSON_GetArraySize(cparr);
            if (cn > 32) cn = 32;
            for (int ci = 0; ci < cn; ci++) {
                const cJSON *e = cJSON_GetArrayItem(cparr, ci);
                if (cJSON_IsNumber(e))
                    r.custom_post_params[ci] = (float)e->valuedouble;
            }
            r.custom_post_param_count = cn / 4;   /* vec4 count */
        }
    }

    /* ── Environment: time-of-day + weather (P2-weather-decals-tod) ──── */
    const cJSON *env = cJSON_GetObjectItemCaseSensitive(src, "environment");
    if (cJSON_IsObject(env)) {
        const cJSON *tod = cJSON_GetObjectItemCaseSensitive(env, "timeOfDay");
        if (cJSON_IsObject(tod)) {
            r.tod_enabled   = j_bool(tod, "enabled", r.tod_enabled);
            r.tod_hour      = (float)j_num(tod, "hour", r.tod_hour);
            r.tod_speed     = (float)j_num(tod, "speed", r.tod_speed);
            r.tod_latitude  = (float)j_num(tod, "latitude", r.tod_latitude);
            r.tod_dawn_hour = (float)j_num(tod, "dawnHour", r.tod_dawn_hour);
            r.tod_dusk_hour = (float)j_num(tod, "duskHour", r.tod_dusk_hour);
        }
        const cJSON *weather = cJSON_GetObjectItemCaseSensitive(env, "weather");
        if (cJSON_IsObject(weather)) {
            r.weather_type      = (int)j_num(weather, "type", r.weather_type);
            r.weather_intensity =
                (float)j_num(weather, "intensity", r.weather_intensity);
        }
        /* Wind. Absent => (0,0) => the environment keeps its own default, so
         * every scene written before this key round-trips unchanged. */
        const cJSON *wind = cJSON_GetObjectItemCaseSensitive(env, "wind");
        if (cJSON_IsObject(wind)) {
            r.wind_direction_x = (float)j_num(wind, "directionX", 0.0);
            r.wind_direction_z = (float)j_num(wind, "directionZ", 0.0);
        }
        /* Absent => the default already in `r`, which is the environment's own
         * 15 C, so a scene written before this key round-trips unchanged. */
        r.temperature_c = (float)j_num(env, "temperatureC", (double)r.temperature_c);

        /* Sky (analytic Preetham; absent key → default GRADIENT, no change). */
        const cJSON *sky = cJSON_GetObjectItemCaseSensitive(env, "sky");
        if (cJSON_IsObject(sky)) {
            r.sky_mode      = (int)j_num(sky, "mode", r.sky_mode);
            r.sky_turbidity = (float)j_num(sky, "turbidity", r.sky_turbidity);
            /* Absent => 0 => clouds off, so pre-existing scenes round-trip
             * byte-identically. */
            r.cloud_coverage  = (float)j_num(sky, "cloudCoverage", 0.0);
            r.cloud_density   = (float)j_num(sky, "cloudDensity",  0.0);
            r.cloud_bottom_km = (float)j_num(sky, "cloudBottomKm", 0.0);
            r.cloud_top_km    = (float)j_num(sky, "cloudTopKm",    0.0);
            /* Stylisation defaults to IDENTITY, not to zero.  A scene written
             * before this layer existed has none of these keys, and a zeroed
             * tint is BLACK -- so the absent-key default has to be 1, or every
             * existing scene would load with a black sky. */
            r.sky_stylise_bands      = (int)j_num(sky, "styliseBands", 0.0);
            r.sky_stylise_rim        = (float)j_num(sky, "styliseRim", 0.0);
            r.sky_stylise_saturation = (float)j_num(sky, "styliseSaturation", 1.0);
            for (int ti = 0; ti < 3; ++ti) {
                r.sky_tint_shadow[ti] = 1.0f;
                r.sky_tint_mid[ti]    = 1.0f;
                r.sky_tint_high[ti]   = 1.0f;
            }
            j_float3(sky, "tintShadow", r.sky_tint_shadow, r.sky_tint_shadow);
            j_float3(sky, "tintMid",    r.sky_tint_mid,    r.sky_tint_mid);
            j_float3(sky, "tintHigh",   r.sky_tint_high,   r.sky_tint_high);
            const cJSON *dome = cJSON_GetObjectItemCaseSensitive(sky, "dome");
            if (cJSON_IsObject(dome)) parse_dome_into(dome, &r);
        }
        /* Floating origin (opt-in; absent key → disabled default, no change). */
        const cJSON *fo = cJSON_GetObjectItemCaseSensitive(env, "floatingOrigin");
        if (cJSON_IsObject(fo)) {
            r.floating_origin_enabled =
                j_bool(fo, "enabled", r.floating_origin_enabled);
            r.floating_origin_threshold =
                (float)j_num(fo, "threshold", r.floating_origin_threshold);
        }
    }

    /* Top-level "sky" shortcut: allows partial JSON like {"sky":{"mode":2}}
     * as input to the public _from_json wrapper without needing the full
     * "environment" wrapper.  The inner-env path above takes precedence when
     * the "environment" block is present. */
    {
        const cJSON *sky_top = cJSON_GetObjectItemCaseSensitive(src, "sky");
        if (cJSON_IsObject(sky_top) && !cJSON_GetObjectItemCaseSensitive(src, "environment")) {
            r.sky_mode      = (int)j_num(sky_top, "mode", r.sky_mode);
            r.sky_turbidity = (float)j_num(sky_top, "turbidity", r.sky_turbidity);
            r.cloud_coverage  = (float)j_num(sky_top, "cloudCoverage", r.cloud_coverage);
            r.cloud_density   = (float)j_num(sky_top, "cloudDensity",  r.cloud_density);
            r.cloud_bottom_km = (float)j_num(sky_top, "cloudBottomKm", r.cloud_bottom_km);
            r.cloud_top_km    = (float)j_num(sky_top, "cloudTopKm",    r.cloud_top_km);
            const cJSON *dome = cJSON_GetObjectItemCaseSensitive(sky_top, "dome");
            if (cJSON_IsObject(dome)) parse_dome_into(dome, &r);
        }
    }

    /* ── Look Profile (absent key → r already holds neutral defaults) ─── */
    const cJSON *look = cJSON_GetObjectItemCaseSensitive(src, "look");
    if (cJSON_IsObject(look)) {
        r.wrap_factor        = (float)j_num(look, "wrap", r.wrap_factor);
        r.ambient_hemisphere = j_bool(look, "hemisphere", r.ambient_hemisphere);
        j_float3(look, "groundColor", r.ambient_ground_color,
                 r.ambient_ground_color);
        j_float3(look, "rimColor", r.rim_color, r.rim_color);
        r.rim_power     = (float)j_num(look, "rimPower", r.rim_power);
        r.rim_intensity = (float)j_num(look, "rimIntensity", r.rim_intensity);
        r.tonemap_op    = (int)j_num(look, "tonemapOp", r.tonemap_op);
        const cJSON *lp = cJSON_GetObjectItemCaseSensitive(look, "lutPath");
        if (cJSON_IsString(lp) && lp->valuestring) {
            size_t i = 0;
            for (; lp->valuestring[i] && i + 1 < sizeof(r.lut_path); i++)
                r.lut_path[i] = lp->valuestring[i];
            r.lut_path[i] = '\0';
        }
        r.lut_strength   = (float)j_num(look, "lutStrength", r.lut_strength);
        r.toon_character = j_bool(look, "toonCharacter", r.toon_character);
        r.bloom_knee     = (float)j_num(look, "bloomKnee", r.bloom_knee);
    }

    return r;
}

static bool parse_scene_rendering_settings(JceScene *scene,
                                           const cJSON *root)
{
    const cJSON *scene_obj = scene_root_object(root);
    if (!scene_obj) return false;

    const cJSON *src = cJSON_GetObjectItemCaseSensitive(scene_obj, "rendering");
    if (!cJSON_IsObject(src))
        src = cJSON_GetObjectItemCaseSensitive(scene_obj, "lighting");
    if (!cJSON_IsObject(src))
        return false;

    JceSceneRenderingSettings r = extract_rendering_settings_from_obj(src);
    jce_scene_set_rendering_settings(scene, &r);
    return true;
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

static void parse_pivot(JceScene *s, JceEntity e, const cJSON *c)
{
    JcePivotComponent p;
    memset(&p, 0, sizeof(p));
    p.local_position =
        jce_v3((float)j_num2(c, "localX", "local_x", 0.0),
               (float)j_num2(c, "localY", "local_y", 0.0),
               (float)j_num2(c, "localZ", "local_z", 0.0));

    float rx = (float)j_num2(c, "rotX", "rot_x", 0.0);
    float ry = (float)j_num2(c, "rotY", "rot_y", 0.0);
    float rz = (float)j_num2(c, "rotZ", "rot_z", 0.0);
    const cJSON *rw = cJSON_GetObjectItemCaseSensitive(c, "rotW");
    if (!rw) rw = cJSON_GetObjectItemCaseSensitive(c, "rot_w");
    if (cJSON_IsNumber(rw)) {
        p.local_rotation =
            jce_q_normalize(jce_v4(rx, ry, rz, (float)rw->valuedouble));
    } else {
        p.local_rotation = q_from_euler_deg(rx, ry, rz);
    }
    jce_scene_set_pivot(s, e, &p);
}

/* Material-backfill cache: active only during a full jce_scene_load_json so a
   .mat.json shared by many MeshRenderers (common in Unity-imported scenes,
   e.g. 84 meshes pointing at one material) is read + parsed once, not once per
   entity. Inactive for single-entity/prefab parses, so no cross-load staleness. */
typedef struct {
    char           path[1280];
    JcePbrMaterial pbr;
    char           tex[5][256];
} MatCacheEntry;
#define MAT_CACHE_MAX 64
static MatCacheEntry s_matcache[MAT_CACHE_MAX];
static int           s_matcache_count  = 0;
static bool          s_matcache_active = false;

static void matcache_begin(void) { s_matcache_count = 0; s_matcache_active = true;  }
static void matcache_end(void)   { s_matcache_count = 0; s_matcache_active = false; }

static int matcache_find(const char *path)
{
    if (!s_matcache_active) return -1;
    for (int i = 0; i < s_matcache_count; i++)
        if (strcmp(s_matcache[i].path, path) == 0) return i;
    return -1;
}
static void matcache_insert(const char *path, const JcePbrMaterial *pbr,
                            char tex[5][256])
{
    if (!s_matcache_active || s_matcache_count >= MAT_CACHE_MAX) return;
    MatCacheEntry *e = &s_matcache[s_matcache_count++];
    copy_str(e->path, sizeof(e->path), path);
    e->pbr = *pbr;
    memcpy(e->tex, tex, sizeof(e->tex));
}

static void parse_mesh_renderer(JceScene *s, JceEntity e, const cJSON *c)
{
    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    /* READ IT.  This line used to be `mr.visible = true;` — a hardcode, not a
     * default — and ser_mesh_renderer emitted no "visible" key to go with it,
     * so MeshRenderer was the ONE renderer component whose visibility could
     * not survive JSON in either direction.  GrassField, FoliageCluster,
     * Water, TerrainChunk, VegetationScatter and BillboardRenderer all
     * round-trip theirs (j_bool + cJSON_AddBoolToObject); this one silently
     * discarded the caller's intent and answered "visible".
     *
     * Two consumers were losing, both measured:
     *   * THE EDITOR CANNOT SAVE A HIDDEN MESH.  Hide a MeshRenderer in the
     *     Inspector, save the scene, reload: it is back.  Nothing warned.
     *   * ANY SCRIPT'S comp_get -> modify -> comp_set TURNS THE MESH ON, and
     *     a script that never mentions `visible` does it too, because the key
     *     is absent from what comp_get returns.  elemental_serenity's Lua
     *     director sets `mr.visible` per season on 102 flower cards and has
     *     never hidden one: 1445 magenta flower pixels in shot_spring_day.png
     *     (authored visibility 1.00) versus 1410 in shot_winter_day.png
     *     (0.20) — 97.6% of them, where 20% was asked for.
     *
     * `true` stays the DEFAULT, so every scene, prefab and cooked asset in
     * the tree loads exactly as before: no tracked .scene.json contains a
     * "visible" key on a MeshRenderer at all (git grep, 0 hits). */
    mr.visible = j_bool(c, "visible", true);
    static const char *const mk[] = { "meshPath", "mesh_path", "mesh" };
    static const char *const matk[] = { "materialPath", "material_path", "material" };
    const char *mp = j_str_any(c, mk, 3);
    const char *mt = j_str_any(c, matk, 3);
    if (mp) mr.mesh_path = jce_scene_intern(s, mp);
    /* Normalize absolute mesh paths saved by the editor
     * (e.g. "D:/.../resources/assets\models\...") to the PAK-relative
     * forward-slash key so jce_pak_find can look them up at runtime. */
    if (jce_path_is_absolute(mr.mesh_path)) {
        char norm[512];
        const char *rel = jce_path_asset_key(mr.mesh_path, norm, sizeof(norm));
        if (rel)
            mr.mesh_path = jce_scene_intern(s, rel);
    }
    if (mt) mr.material_path = jce_scene_intern(s, mt);
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
    /* Unity-style per-renderer shadow flags; default ON (legacy scenes lack
       the keys). Stored inverted in the component. */
    mr.shadow_cast_off    = !j_bool(c, "castsShadow",    true);
    mr.shadow_receive_off = !j_bool(c, "receivesShadow", true);
    mr.albedo_tex = jce_scene_intern(s, j_str(c, "albedoTex",   ""));
    mr.mr_tex = jce_scene_intern(s, j_str(c, "mrTex",       ""));
    mr.normal_tex = jce_scene_intern(s, j_str(c, "normalTex",   ""));
    mr.ao_tex = jce_scene_intern(s, j_str(c, "aoTex",       ""));
    mr.emissive_tex = jce_scene_intern(s, j_str(c, "emissiveTex", ""));
    mr.toon           = j_bool(c, "toon", false);
    mr.toon_bands     = (int)j_num(c, "toonBands", 0);
    mr.rim_power      = (float)j_num(c, "toonRimPower", 0.0);
    mr.rim_intensity  = (float)j_num(c, "toonRimIntensity", 0.0);
    mr.rim_color[0]   = (float)j_num(c, "toonRimColorR", 0.0);
    mr.rim_color[1]   = (float)j_num(c, "toonRimColorG", 0.0);
    mr.rim_color[2]   = (float)j_num(c, "toonRimColorB", 0.0);
    mr.outline_width  = (float)j_num(c, "toonOutlineWidth", 0.0);
    mr.outline_color[0] = (float)j_num(c, "toonOutlineColorR", 0.0);
    mr.outline_color[1] = (float)j_num(c, "toonOutlineColorG", 0.0);
    mr.outline_color[2] = (float)j_num(c, "toonOutlineColorB", 0.0);

    /* If a .mat.json material is referenced and no per-entity texture
     * overrides exist, backfill texture paths from the material file.
     * This is required for Unity-exported scenes where MeshRenderer
     * carries only materialPath and the textures live inside the .mat. */
    if (mr.material_path[0] != '\0' && mr.albedo_tex[0] == '\0') {
        char mat_full[1280] = { 0 };
        const char *try_paths[3] = { NULL, NULL, NULL };
        int n_try = 0;

        if (jce_path_is_absolute(mr.material_path)) {
            try_paths[n_try++] = mr.material_path;
        } else {
            if (SSE_SCENE_BASE_DIR[0]) {
                jce_path_join(mat_full, sizeof(mat_full),
                         SSE_SCENE_BASE_DIR, mr.material_path);
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
        if (!resolved && SSE_SCENE_BASE_DIR[0] && !jce_path_is_absolute(mr.material_path)) {
            char base[1024];
            snprintf(base, sizeof(base), "%s", SSE_SCENE_BASE_DIR);
            for (int up = 0; up < 4 && !resolved; up++) {
                /* trim last segment */
                long L = (long)strlen(base);
                while (L > 0 && base[L-1] != '/' && base[L-1] != '\\') L--;
                while (L > 0 && (base[L-1] == '/' || base[L-1] == '\\')) L--;
                if (L <= 0) break;
                base[L] = '\0';
                jce_path_join(parent_try, sizeof(parent_try), base, mr.material_path);
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
            bool have_mat = false;
            int  ci = matcache_find(resolved);
            if (ci >= 0) {
                pbr = s_matcache[ci].pbr;          /* cache hit: skip read+parse */
                memcpy(tex_paths, s_matcache[ci].tex, sizeof(tex_paths));
                have_mat = true;
            } else if ((sse_context_get()->asset_fs
                            ? jce_pbr_material_load_json_vfs(
                                  sse_context_get()->asset_fs, resolved,
                                  &pbr, tex_paths)
                            : jce_pbr_material_load_json(
                                  resolved, &pbr, tex_paths))) {
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
                matcache_insert(resolved, &pbr, tex_paths);
                have_mat = true;
            }
            if (have_mat) {
                if (tex_paths[0][0]) mr.albedo_tex = jce_scene_intern(s, tex_paths[0]);
                if (tex_paths[1][0]) mr.mr_tex = jce_scene_intern(s, tex_paths[1]);
                if (tex_paths[2][0]) mr.normal_tex = jce_scene_intern(s, tex_paths[2]);
                if (tex_paths[3][0]) mr.ao_tex = jce_scene_intern(s, tex_paths[3]);
                if (tex_paths[4][0]) mr.emissive_tex = jce_scene_intern(s, tex_paths[4]);
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
                if (pbr.custom_program != UINT16_MAX) {
                    mr.has_custom_program = true;
                    mr.custom_program_idx = pbr.custom_program;
                }
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
    cc.stack_index = (uint8_t)(int)j_num(c, "stackIndex", 0);
    cc.clear_mode  = (uint8_t)(int)j_num(c, "clearMode",  0);
    jce_scene_set_camera(s, e, &cc);
}








static void parse_editor_meta(JceScene *s, JceEntity e, const cJSON *c)
{
    JceEditorMeta m;
    jce_editor_meta_init(&m);
    copy_str(m.name, sizeof(m.name), j_str(c, "name", ""));
    copy_str(m.tag,  sizeof(m.tag),  j_str(c, "tag",  ""));
    m.tag_color       = (uint8_t)j_num(c, "tagColor", 0);
    m.enabled         = j_bool(c, "enabled", true);
    m.prefab_instance = j_bool(c, "prefabInstance", false);
    m.prefab_path = jce_scene_intern(s, j_str(c, "prefabPath", ""));
    m.layer = (int)j_num(c, "layer", 0);
    if (m.layer < 0 || m.layer > 31) m.layer = 0;
    jce_scene_set_editor_meta(s, e, &m);

    /* P4-A.4 — mirror the EditorMeta tag into the engine ECS tag component
     * so jce_scene_find_with_tag() can see it.  An explicit entity-level
     * "tag" key was already mirrored before the components parse — keep it
     * (entity-level wins over the EditorMeta block). */
    if (m.tag[0] != '\0') {
        const char *cur = jce_scene_get_entity_tag_name(s, e);
        if (!cur || !*cur || strcmp(cur, "Untagged") == 0)
            jce_scene_set_entity_tag_name(s, e, m.tag);
    }
}





static void parse_particle_emitter(JceScene *s, JceEntity e, const cJSON *c)
{
    JceParticleEmitterComponent pe;
    memset(&pe, 0, sizeof(pe));
    /* Authored `*.particles.json` asset (empty = use the legacy quick-tune
     * fields below).  Accept both keys for forward/backward compatibility. */
    {
        static const char *const pkeys[] = { "assetPath", "particlePath" };
        copy_str(pe.asset_path, sizeof(pe.asset_path),
                 j_str_any(c, pkeys, 2));
    }
    pe.emit_rate     = (float)j_num(c, "emitRate", 10.0);
    pe.lifetime_min  = (float)j_num(c, "lifetimeMin", 1.0);
    pe.lifetime_max  = (float)j_num(c, "lifetimeMax", 2.0);
    pe.gpu           = j_bool(c, "gpu", false);
    /* Runtime fields are NOT serialized; memset above already zeroed them.
     * emitter_handle_idx must read as "none" so the first tick rebuilds. */
    pe.emitter_handle_idx = UINT32_MAX;
    pe.loaded             = false;
    pe.asset_epoch        = 0;
    jce_scene_set_particle_emitter(s, e, &pe);
}






static void parse_video_player(JceScene *s, JceEntity e, const cJSON *c)
{
    JceVideoPlayerComponent vp;
    memset(&vp, 0, sizeof(vp));
    copy_str(vp.clip_path, sizeof(vp.clip_path), j_str(c, "clipPath", ""));
    vp.loop     = j_bool(c, "loop", false);
    vp.autoplay = j_bool(c, "autoplay", true);
    /* Runtime fields stay zeroed: the video handle / texture are opened on
     * demand by jce_scene_video_update().  Keep output_tex invalid. */
    vp.output_tex = JCE_TEXTURE_INVALID;
    jce_scene_set_video_player(s, e, &vp);
}
























/* ── P2 add-on parsers (Wheel/ConstantForce/Joints/Billboard/UI) ── */


































/* ── Inline parse branches factored for registry dispatch ─────────── */





/* One-way legacy migration (consolidation v0.9.9). The orphaned VFX
 * Graph runtime was removed — its `*.vfx.json` key set always parsed
 * identically to `*.particles.json`, so the authored graphPath maps
 * straight onto a ParticleEmitterComponent asset path and the entity
 * joins the standard particle pipeline. Legacy-only knobs
 * (playOnAwake / loop / rateMultiplier / intensity) have no
 * counterpart and are dropped. An explicitly authored ParticleEmitter
 * on the same entity always wins; the component is never re-saved as
 * VfxGraph (its registry row has serialize == NULL). */
static void parse_vfx_graph_migrate(JceScene *s, JceEntity e, const cJSON *props)
{
    const char *gp = j_str(props, "graphPath", "");
    if (gp[0] && !jce_scene_has_particle_emitter(s, e)) {
        JceParticleEmitterComponent pe;
        memset(&pe, 0, sizeof pe);
        copy_str(pe.asset_path, sizeof pe.asset_path, gp);
        pe.emit_rate          = 10.0f;   /* legacy quick-tune fallbacks */
        pe.lifetime_min       = 1.0f;
        pe.lifetime_max       = 2.0f;
        pe.emitter_handle_idx = UINT32_MAX;
        jce_scene_set_particle_emitter(s, e, &pe);
    }
}






/* Per-component enable toggles persisted at entity level.  Current
 * write format is a JSON array of canonical component names; legacy
 * scenes stored a 64-bit JCE_COMP_FLAG_* DISABLED mask as a number —
 * accept both. */
static void parse_disabled_components(JceScene *s, JceEntity e,
                                      const cJSON *entity_obj)
{
    const cJSON *it =
        cJSON_GetObjectItemCaseSensitive(entity_obj, "disabledComponents");
    if (!it) return;

    if (cJSON_IsNumber(it)) {
        /* Legacy numeric mask: one bit per pre-registry component. */
        uint64_t mask = (uint64_t)it->valuedouble;
        for (int b = 0; b < 64 && mask != 0; b++) {
            uint64_t bit = UINT64_C(1) << b;
            if (!(mask & bit)) continue;
            mask &= ~bit;
            int id = jce_component_from_legacy_flag(bit);
            if (id != JCE_COMP_ID_INVALID)
                jce_scene_set_comp_enabled(s, e, id, false);
        }
        return;
    }
    if (cJSON_IsArray(it)) {
        const cJSON *n = NULL;
        cJSON_ArrayForEach(n, it) {
            if (!cJSON_IsString(n) || !n->valuestring) continue;
            int id = jce_component_find(n->valuestring);
            if (id != JCE_COMP_ID_INVALID)
                jce_scene_set_comp_enabled(s, e, id, false);
        }
    }
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

    /* Registry dispatch — canonical names + alias spellings live in the
     * component registry rows (see jce_scene_components_register_all).
     * Unknown types are ignored, exactly like the old strcmp chain. */
    int id = jce_component_find(type);
    if (id >= 0) {
        const JceComponentDesc *d = jce_component_desc(id);
        if (d->parse) d->parse(s, e, props);
        return;
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

static void ser_pivot(const JcePivotComponent *p, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Pivot");
    cJSON_AddNumberToObject(o, "localX", p->local_position.x);
    cJSON_AddNumberToObject(o, "localY", p->local_position.y);
    cJSON_AddNumberToObject(o, "localZ", p->local_position.z);
    cJSON_AddNumberToObject(o, "rotX", p->local_rotation.x);
    cJSON_AddNumberToObject(o, "rotY", p->local_rotation.y);
    cJSON_AddNumberToObject(o, "rotZ", p->local_rotation.z);
    cJSON_AddNumberToObject(o, "rotW", p->local_rotation.w);
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
    if (c->stack_index != 0)
        cJSON_AddNumberToObject(o, "stackIndex", c->stack_index);
    if (c->clear_mode != 0)
        cJSON_AddNumberToObject(o, "clearMode",  c->clear_mode);
    cJSON_AddItemToArray(arr, o);
}







static void ser_particle_emitter(const JceParticleEmitterComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "ParticleEmitter");
    if (c->asset_path[0])
        cJSON_AddStringToObject(o, "assetPath", c->asset_path);
    cJSON_AddNumberToObject(o, "emitRate", c->emit_rate);
    cJSON_AddNumberToObject(o, "lifetimeMin", c->lifetime_min);
    cJSON_AddNumberToObject(o, "lifetimeMax", c->lifetime_max);
    cJSON_AddBoolToObject(o, "gpu", c->gpu);
    cJSON_AddItemToArray(arr, o);
}






static void ser_video_player(const JceVideoPlayerComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "VideoPlayer");
    cJSON_AddStringToObject(o, "clipPath", c->clip_path);
    cJSON_AddBoolToObject(o, "loop", c->loop);
    cJSON_AddBoolToObject(o, "autoplay", c->autoplay);
    cJSON_AddItemToArray(arr, o);
}

























/* ── P2 add-on serializers ──────────────────────────────────────── */



































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
    if (m->layer != 0)
        cJSON_AddNumberToObject(o, "layer", m->layer);
    cJSON_AddItemToArray(arr, o);
}

/* ── Shared entity-envelope writers ───────────────────────────────── */

void jce_scene_write_disabled_components(JceJson *obj, const JceScene *s,
                                         JceEntity e)
{
    if (!obj || !s) return;
    JceJson *dis = NULL;
    const int n = jce_component_count();
    for (int id = 0; id < n; id++) {
        if (jce_scene_comp_enabled(s, e, id)) continue;
        if (!dis) {
            dis = jce_json_array();
            if (!dis) return;
        }
        jce_json_array_push_string(dis, jce_component_name(id));
    }
    /* Nothing disabled -> emit nothing, so a default entity keeps
     * serialising byte-for-byte as it did before this was extracted. */
    if (dis) jce_json_set_child(obj, "disabledComponents", dis);
}

void jce_scene_write_entity_layer(JceJson *obj, const JceScene *s, JceEntity e)
{
    if (!obj || !s) return;
    if (!(jce_scene_get_component_flags(s, e) & JCE_COMP_FLAG_LAYER)) return;
    const uint8_t layer = jce_scene_get_entity_layer((JceScene *)s, e);
    if (layer != 0)
        jce_json_set_number(obj, "layer", (double)layer);
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

/* ── Uniform serialize wrappers (component-registry rows) ──────────
 *
 * One wrapper per serializing registry row, all sharing the uniform
 * JceCompSerFn signature: fetch + null-check + call the existing
 * ser_* with its original arguments.  Bodies are the old hard-coded
 * chain entries, moved verbatim. */

static void serw_transform(JceScene *s, JceEntity e, cJSON *arr)
{
    JceTransform *t = jce_scene_get_transform(s, e);
    if (t) ser_transform(t, arr);
}

static void serw_pivot(JceScene *s, JceEntity e, cJSON *arr)
{
    JcePivotComponent *p = jce_scene_get_pivot(s, e);
    if (p) ser_pivot(p, arr);
}


static void serw_camera(JceScene *s, JceEntity e, cJSON *arr)
{
    JceCameraComponent *c = jce_scene_get_camera(s, e);
    if (c) ser_camera(c, arr);
}









static void serw_particle_emitter(JceScene *s, JceEntity e, cJSON *arr)
{
    JceParticleEmitterComponent *c = jce_scene_get_particle_emitter(s, e);
    if (c) ser_particle_emitter(c, arr);
}






static void serw_video_player(JceScene *s, JceEntity e, cJSON *arr)
{
    JceVideoPlayerComponent *c = jce_scene_get_video_player(s, e);
    if (c) ser_video_player(c, arr);
}
































































static void serw_editor_meta(JceScene *s, JceEntity e, cJSON *arr)
{
    JceEditorMeta *m = jce_scene_get_editor_meta(s, e);
    if (m) ser_editor_meta(m, arr);
}

/* Single source of truth for the per-component emit chain.
 *
 * Appends every present component of entity `e` to the `comps` array.
 * BOTH the full-scene serializer (ser_entity_cb) and the per-entity
 * serializer (jce_scene_serialize_entity_components) route through this
 * so neither can silently drop a component type. Adding a new component
 * here automatically covers scene-save, prefab-save, and copy/paste. */
static void ser_entity_components(JceScene *s, JceEntity e, cJSON *comps)
{
    if (!s || !comps) return;

    /* Registry-driven: rows were registered in the legacy chain order
     * (Transform first), so the emitted component order — and thus the
     * saved-file diff — matches the old hard-coded chain.  Rows with
     * serialize == NULL (DirectionalLight/PointLight/SpotLight fold
     * into the unified "Light" row, Tag/Layer are entity-level fields,
     * the retired VfxGraph never re-saves) are skipped by design. */
    const int n = jce_component_count();
    for (int id = 0; id < n; id++) {
        const JceComponentDesc *d = jce_component_desc(id);
        if (!d->serialize || !d->has(s, e)) continue;
        d->serialize(s, e, comps);
    }
}

static void ser_entity_cb(JceScene *s, JceEntity e, void *ud)
{
    SerCtx *ctx = (SerCtx *)ud;
    cJSON *eobj = cJSON_CreateObject();
    if (!eobj) return;

    const char *name = jce_scene_entity_registered_name(s, e);
    cJSON_AddNumberToObject(eobj, "id", (double)e);
    cJSON_AddStringToObject(eobj, "name", name ? name : "");
    cJSON_AddNumberToObject(eobj, "parentId", (double)jce_scene_get_parent(s, e));

    /* P4-A.4 Tag & Layer — emit at entity level when present. */
    {
        uint64_t pf = jce_scene_get_component_flags(s, e);
        if (pf & JCE_COMP_FLAG_TAG) {
            const char *tn = jce_scene_get_entity_tag_name(s, e);
            if (tn && *tn && strcmp(tn, "Untagged") != 0)
                cJSON_AddStringToObject(eobj, "tag", tn);
        }
        if (pf & JCE_COMP_FLAG_LAYER) {
            uint8_t lyr = jce_scene_get_entity_layer(s, e);
            if (lyr != 0)
                cJSON_AddNumberToObject(eobj, "layer", (double)lyr);
        }
    }

    /* Per-component enable toggles — written as an array of canonical
     * component names currently disabled.  Replaces the legacy numeric
     * JCE_COMP_FLAG_* mask (the 64-bit space is exhausted); the loader
     * accepts both forms.  Omitted entirely when nothing is disabled. */
    jce_scene_write_disabled_components((JceJson *)eobj, s, e);

    cJSON *comps = cJSON_CreateArray();
    if (!comps) { cJSON_Delete(eobj); return; }

    /* Single source of truth — see ser_entity_components(). */
    ser_entity_components(s, e, comps);

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
        const JceSceneRenderingSettings *rendering =
            jce_scene_get_rendering_settings(scene);
        if (rendering) {
            cJSON *r = ser_scene_rendering_settings(rendering);
            if (r)
                cJSON_AddItemToObject(sobj, "rendering", r);
        }

        /* World-streaming settings — sibling of the "rendering" block;
         * only present when the scene actually authored them. */
        const JceSceneStreamingSettings *streaming =
            jce_scene_get_streaming_settings(scene);
        if (streaming) {
            cJSON *st = ser_scene_streaming_settings(streaming);
            if (st)
                cJSON_AddItemToObject(sobj, "streaming", st);
        }

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

/* ── Nested node-tree documents (editor-authored prefabs) ──────────────
 *
 * The editor does NOT write the flat entity array for a prefab: its
 * build_prefab_json_root() (editor/src/io/jce_editor_scene_serial.cpp)
 * emits a NESTED node tree,
 *
 *     { "contract": { ... },
 *       "prefab": { "version": 1,
 *                   "root": { "name": "...", "enabled": true,
 *                             "tag": "...", "tagColor": 0,
 *                             "disabledComponents": [ ... ],
 *                             "components": [ ... ],
 *                             "children": [ <node>, ... ] } } }
 *
 * where parenthood is structural (nesting) instead of the flat form's
 * id/parentId pairs, and a node carries no id at all.  Every .prefab.json
 * the editor saves has that shape, so the runtime loader has to accept it
 * or jce_prefab_instantiate*() silently produces zero entities.
 *
 * Returns the root NODE (never the document envelope), or NULL. */
static const cJSON *resolve_entity_tree_root(const cJSON *root)
{
    if (!root || !cJSON_IsObject(root)) return NULL;

    /* Contract envelope: root.prefab.root */
    const cJSON *prefab = cJSON_GetObjectItemCaseSensitive(root, "prefab");
    if (cJSON_IsObject(prefab)) {
        const cJSON *node = cJSON_GetObjectItemCaseSensitive(prefab, "root");
        if (cJSON_IsObject(node)) return node;
    }

    /* Bare node at the document root.  Accepted only on an unambiguous
     * entity signature so unrelated JSON assets keep failing cleanly. */
    if (cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "components")) ||
        cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "children")))
        return root;

    return NULL;
}

/* Child-list of a tree node.  The key aliases match the editor's reader
 * (load_entity_tree_node in editor/src/io/jce_editor_scene_parse.cpp) so
 * anything the editor can open, the runtime can instantiate. */
static const cJSON *tree_node_children(const cJSON *node)
{
    static const char *const keys[] = {
        "children", "nodes", "entities", "objects"
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(node, keys[i]);
        if (cJSON_IsArray(a)) return a;
    }
    return NULL;
}

static bool is_legacy_unnamed_entity_name(const char *name)
{
    return !name || name[0] == '\0' || strcmp(name, "(unnamed)") == 0;
}

static void count_existing_entity_cb(JceScene *scene, JceEntity e, void *ud)
{
    (void)scene;
    (void)e;
    uint32_t *count = (uint32_t *)ud;
    if (count)
        (*count)++;
}

/* Create one entity from its JSON object and parse its components/meta into
 * map[*loaded] (the first-pass body, extracted so the one-shot and streaming
 * loaders run byte-identical work).  Advances *loaded on success. */
static void load_one_entity(JceScene *scene, const cJSON *eobj,
                            EntityRemap *map, int *loaded)
{
    if (!cJSON_IsObject(eobj)) return;

    const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(eobj, "name");
    const char *name = (cJSON_IsString(name_item) && name_item->valuestring)
                     ? name_item->valuestring
                     : "";
    JceEntity new_e = jce_scene_create_entity(
        scene,
        is_legacy_unnamed_entity_name(name) ? NULL : name);
    if (new_e == JCE_ENTITY_INVALID) return;

    int idx = *loaded;
    map[idx].src_id = (JceEntity)j_num(eobj, "id", 0.0);
    map[idx].new_id = new_e;
    /* Accept both "parent_id" and "parentId". */
    map[idx].parent_src = (JceEntity)j_num2(eobj, "parent_id", "parentId", 0.0);
    (*loaded)++;

    /* Apply entity-level EditorMeta fields (enabled, tag, tagColor, etc.).
     * Always create EditorMeta with the entity name so the editor can see
     * hand-authored scenes that omit these optional fields. */
    {
        const cJSON *it_enabled = cJSON_GetObjectItemCaseSensitive(eobj, "enabled");
        const cJSON *it_tag     = cJSON_GetObjectItemCaseSensitive(eobj, "tag");
        const cJSON *it_tc      = cJSON_GetObjectItemCaseSensitive(eobj, "tagColor");
        const cJSON *it_pi      = cJSON_GetObjectItemCaseSensitive(eobj, "prefabInstance");
        const cJSON *it_pp      = cJSON_GetObjectItemCaseSensitive(eobj, "prefabPath");
        const cJSON *it_layer   = cJSON_GetObjectItemCaseSensitive(eobj, "layer");
        JceEditorMeta *m = jce_scene_get_editor_meta(scene, new_e);
        if (!m) {
            JceEditorMeta fresh;
            jce_editor_meta_init(&fresh);
            copy_str(fresh.name, sizeof(fresh.name),
                     is_legacy_unnamed_entity_name(name) ? "Entity" : name);
            fresh.enabled = true;
            jce_scene_set_editor_meta(scene, new_e, &fresh);
            m = jce_scene_get_editor_meta(scene, new_e);
        }
        if (m) {
            if (it_enabled) m->enabled = j_bool_it(it_enabled, true);
            if (it_tag) copy_str(m->tag, sizeof(m->tag), j_str_it(it_tag, ""));
            if (it_tc) m->tag_color = (uint8_t)j_num_it(it_tc, 0);
            if (it_pi) m->prefab_instance = j_bool_it(it_pi, false);
            if (it_pp) m->prefab_path = jce_scene_intern(scene, j_str_it(it_pp, ""));
        }
        /* P4-A.4 — mirror tag/layer into engine ECS components. */
        if (it_tag) {
            const char *tn = j_str_it(it_tag, "");
            if (tn && *tn)
                jce_scene_set_entity_tag_name(scene, new_e, tn);
        }
        if (it_layer) {
            int lyr = (int)j_num_it(it_layer, 0);
            if (lyr < 0) lyr = 0;
            if (lyr > 31) lyr = 31;
            jce_scene_set_entity_layer(scene, new_e, (uint8_t)lyr);
        }
    }

    const cJSON *comps = cJSON_GetObjectItemCaseSensitive(eobj, "components");
    if (!comps) comps = cJSON_GetObjectItemCaseSensitive(eobj, "component");
    if (cJSON_IsArray(comps)) {
        for (const cJSON *c = comps->child; c; c = c->next) {
            if (cJSON_IsObject(c))
                parse_one_component(scene, new_e, c);
        }
    }

    /* Per-component enable toggles (canonical-name array, or the
     * legacy numeric mask) — applied after the components exist. */
    parse_disabled_components(scene, new_e, eobj);
}

/* Second-pass parent + cross-entity reference fixups over the [0,loaded)
 * remap table (extracted so the one-shot and streaming loaders share it). */
static void load_fixup_refs(JceScene *scene, EntityRemap *map, int loaded)
{
    /* Fix up parent references via a src_id -> new_id hash, instead of an
       O(n^2) nested scan (matters once scenes reach hundreds of entities).
       src_id 0 = "no id" and parent_src 0 = "no parent", so 0 is a safe empty
       slot — those entities are never valid parent targets. */
    {
        struct ParentSlot { JceEntity src; JceEntity new_id; };
        int hcap = 1;
        while (hcap < (loaded > 0 ? loaded * 2 : 1)) hcap <<= 1;
        struct ParentSlot *ht =
            (struct ParentSlot *)JCE_CALLOC((size_t)hcap, sizeof(struct ParentSlot));
        if (ht) {
            uint32_t mask = (uint32_t)hcap - 1u;
            for (int i = 0; i < loaded; i++) {
                JceEntity key = map[i].src_id;
                if (key == 0) continue;
                uint32_t h = (uint32_t)key & mask;
                while (ht[h].src != 0) h = (h + 1u) & mask;
                ht[h].src    = key;
                ht[h].new_id = map[i].new_id;
            }
            for (int i = 0; i < loaded; i++) {
                JceEntity key = map[i].parent_src;
                if (key == 0) continue;
                uint32_t h = (uint32_t)key & mask;
                while (ht[h].src != 0) {
                    if (ht[h].src == key) {
                        jce_scene_set_parent(scene, map[i].new_id, ht[h].new_id);
                        break;
                    }
                    h = (h + 1u) & mask;
                }
            }
            JCE_FREE(ht);
        } else {
            /* Allocation failed — fall back to the linear scan. */
            for (int i = 0; i < loaded; i++) {
                if (map[i].parent_src == 0) continue;
                for (int j = 0; j < loaded; j++) {
                    if (map[j].src_id == map[i].parent_src) {
                        jce_scene_set_parent(scene, map[i].new_id, map[j].new_id);
                        break;
                    }
                }
            }
        }
    }

    /* Entity refs: IkConstraints carry target/pole ENTITY ids authored against
     * the SOURCE scene's ids.  Patch them through the same src_id -> new_id
     * mapping so the references survive reload / prefab instantiation.
     * Unresolvable refs are cleared (0 = none) instead of left dangling at an
     * id that may now belong to an unrelated entity.  Linear lookup is fine
     * here: only entities that actually carry the component are visited, with
     * at most 16 refs x2 each. */
    for (int i = 0; i < loaded; i++) {
        JceIkConstraintComponent *ik =
            jce_scene_get_ik_constraints(scene, map[i].new_id);
        if (!ik) continue;
        int cap = (int)(sizeof(ik->constraints) / sizeof(ik->constraints[0]));
        int cn = ik->count;
        if (cn < 0)   cn = 0;
        if (cn > cap) cn = cap;
        for (int k = 0; k < cn; k++) {
            uint32_t *refs[2] = { &ik->constraints[k].target_entity,
                                  &ik->constraints[k].pole_entity };
            for (int r = 0; r < 2; r++) {
                uint32_t src = *refs[r];
                if (src == 0) continue;
                uint32_t resolved = 0;
                for (int j = 0; j < loaded; j++) {
                    if (map[j].src_id == (JceEntity)src) {
                        resolved = (uint32_t)map[j].new_id;
                        break;
                    }
                }
                *refs[r] = resolved;
            }
        }
    }

    /* Entity refs: SequencePlayer bindings[] carry per-track entity ids
     * authored against the SOURCE scene's ids.  Patch them through the same
     * src_id -> new_id mapping (mirrors the IkConstraints pass above).
     * Unresolvable refs become 0 (= unbound; the runtime then falls back to
     * the track's bindEntityName). */
    for (int i = 0; i < loaded; i++) {
        JceSequencePlayerComponent *sp =
            jce_scene_get_sequence_player(scene, map[i].new_id);
        if (!sp) continue;
        int cn = sp->binding_count;
        if (cn < 0) cn = 0;
        if (cn > JCE_SEQ_PLAYER_MAX_BINDINGS) cn = JCE_SEQ_PLAYER_MAX_BINDINGS;
        for (int k = 0; k < cn; k++) {
            uint64_t src = sp->bindings[k];
            if (src == 0) continue;
            uint64_t resolved = 0;
            for (int j = 0; j < loaded; j++) {
                if (map[j].src_id == (JceEntity)src) {
                    resolved = (uint64_t)map[j].new_id;
                    break;
                }
            }
            sp->bindings[k] = resolved;
        }
    }

    /* Entity refs: single-ref cross-entity fields on Constraint, VirtualCamera
     * (follow/look-at), ConfigurableJoint, Joint2D, and NavAgent are authored
     * against SOURCE ids; patch them through the same map so they survive
     * reload / prefab instantiation (mirrors the IkConstraints / SequencePlayer
     * passes).  Unresolvable refs clear to 0 (= none/world). */
    for (int i = 0; i < loaded; i++) {
        JceEntity e = map[i].new_id;

        /* uint64 refs share one resolve loop. */
        uint64_t *refs64[5];
        int n64 = 0;
        JceVirtualCameraComponent *vc = jce_scene_get_virtual_camera(scene, e);
        if (vc) { refs64[n64++] = &vc->follow_target;
                  refs64[n64++] = &vc->look_at_target; }
        JceConfigurableJointComponent *cj = jce_scene_get_configurable_joint(scene, e);
        if (cj) refs64[n64++] = &cj->connected_body;
        JceJoint2DComponent *j2 = jce_scene_get_joint2d(scene, e);
        if (j2) refs64[n64++] = &j2->connected_body;
        JceNavAgentComponent *na = jce_scene_get_nav_agent(scene, e);
        if (na) refs64[n64++] = &na->target_entity;
        for (int r = 0; r < n64; r++) {
            uint64_t src = *refs64[r];
            if (src == 0) continue;
            uint64_t resolved = 0;
            for (int j = 0; j < loaded; j++)
                if (map[j].src_id == (JceEntity)src) {
                    resolved = (uint64_t)map[j].new_id; break;
                }
            *refs64[r] = resolved;
        }

        /* Constraint.target_entity is uint32. */
        JceConstraintComponent *cn = jce_scene_get_constraint(scene, e);
        if (cn && cn->target_entity != 0) {
            uint32_t src = cn->target_entity, resolved = 0;
            for (int j = 0; j < loaded; j++)
                if (map[j].src_id == (JceEntity)src) {
                    resolved = (uint32_t)map[j].new_id; break;
                }
            cn->target_entity = resolved;
        }
    }
}

/* ── Nested node-tree load (editor-authored prefabs) ──────────────────
 *
 * Same entities, same components, same fixups as the flat form — only
 * the way parenthood is expressed differs, so the node body reuses
 * load_one_entity() rather than growing a second component parser. */

static int count_tree_nodes(const cJSON *node)
{
    if (!cJSON_IsObject(node)) return 0;

    int n = 1;
    const cJSON *kids = tree_node_children(node);
    if (kids) {
        for (const cJSON *c = kids->child; c; c = c->next)
            n += count_tree_nodes(c);
    }
    return n;
}

/* Depth-first: create the node's entity, then its children parented to
 * it.  `cap` bounds the remap table (sized by count_tree_nodes). */
static void load_tree_node(JceScene *scene, const cJSON *node,
                           JceEntity parent, EntityRemap *map,
                           int *loaded, int cap)
{
    if (!cJSON_IsObject(node) || *loaded >= cap) return;

    int idx = *loaded;
    load_one_entity(scene, node, map, loaded);
    if (*loaded == idx) return;   /* entity creation failed — skip subtree */

    /* Parenthood is structural here; clear the id-based link so the
     * second pass cannot re-parent from a stray "parentId" field. */
    map[idx].parent_src = 0;
    if (parent != 0)
        jce_scene_set_parent(scene, map[idx].new_id, parent);

    const cJSON *kids = tree_node_children(node);
    if (!kids) return;
    for (const cJSON *c = kids->child; c; c = c->next)
        load_tree_node(scene, c, map[idx].new_id, map, loaded, cap);
}

/* One-shot loader for a tree document: the streaming cursor below walks
 * an entity ARRAY, which this shape does not have.  The scene-settings
 * presence gate and the second-pass reference fixups mirror
 * jce_scene_load_stream_begin/finalize so both shapes behave alike. */
static int load_entity_tree_document(JceScene *scene, const cJSON *root,
                                     const cJSON *tree_root)
{
    uint32_t existing_entities = 0;
    jce_scene_each_entity(scene, count_existing_entity_cb,
                          &existing_entities);
    if (!parse_scene_rendering_settings(scene, root) &&
        existing_entities == 0)
        jce_scene_clear_rendering_settings(scene);
    if (!parse_scene_streaming_settings(scene, root) &&
        existing_entities == 0)
        jce_scene_clear_streaming_settings(scene);

    int total = count_tree_nodes(tree_root);
    if (total <= 0) return 0;

    EntityRemap *map =
        (EntityRemap *)JCE_CALLOC((size_t)total, sizeof(EntityRemap));
    if (!map) return -1;

    matcache_begin();   /* dedup shared .mat.json reads across this load */

    int loaded = 0;
    load_tree_node(scene, tree_root, 0, map, &loaded, total);
    load_fixup_refs(scene, map, loaded);

    matcache_end();
    JCE_FREE(map);
    return loaded;
}

/* ── Incremental scene load (frame-sliced) ────────────────────────────
 *
 * The streaming loader IS the one-shot loader, split at the per-entity
 * boundary: begin() does the scene-level settings + cursor/remap setup and
 * opens the material cache; step() runs load_one_entity() for the next chunk
 * of entities; finalize() runs load_fixup_refs(), closes the material cache,
 * and frees the stream.  jce_scene_load_json() below is begin + step(all) +
 * finalize, so both paths produce a byte-identical scene. */
struct JceSceneLoadStream {
    JceScene    *scene;
    const cJSON *entities;   /* borrowed from the caller's root */
    const cJSON *cursor;     /* next entity object to process   */
    EntityRemap *map;        /* total-sized remap table         */
    int          total;      /* number of array elements        */
    int          seen;       /* array elements visited (incl. skipped) */
    int          loaded;     /* entities actually created       */
};

JceSceneLoadStream *jce_scene_load_stream_begin(JceScene *scene,
                                                const cJSON *root,
                                                int *out_total)
{
    if (out_total) *out_total = 0;
    if (!scene || !root) return NULL;

    const cJSON *entities = resolve_entities(root);
    if (!entities) {
        LOG_WARN(LOG_TAG, "scene JSON has no 'entities' array");
        return NULL;
    }

    uint32_t existing_entities = 0;
    jce_scene_each_entity(scene, count_existing_entity_cb,
                          &existing_entities);
    if (!parse_scene_rendering_settings(scene, root) &&
        existing_entities == 0)
        jce_scene_clear_rendering_settings(scene);

    /* Same presence gate for the streaming block: absent on a fresh load →
     * clear; absent on an additive load → keep the main scene's settings. */
    if (!parse_scene_streaming_settings(scene, root) &&
        existing_entities == 0)
        jce_scene_clear_streaming_settings(scene);

    int total = cJSON_GetArraySize(entities);
    if (total < 0) total = 0;

    JceSceneLoadStream *st =
        (JceSceneLoadStream *)JCE_CALLOC(1, sizeof(JceSceneLoadStream));
    if (!st) return NULL;
    st->scene    = scene;
    st->entities = entities;
    st->cursor   = entities->child;
    st->total    = total;
    st->seen     = 0;
    st->loaded   = 0;
    /* Remap table sized to the total; tolerate an empty array (no entities). */
    if (total > 0) {
        st->map = (EntityRemap *)JCE_CALLOC((size_t)total, sizeof(EntityRemap));
        if (!st->map) { JCE_FREE(st); return NULL; }
    }

    matcache_begin();   /* dedup shared .mat.json reads across this scene load */

    if (out_total) *out_total = total;
    return st;
}

int jce_scene_load_stream_step(JceSceneLoadStream *st, int max_entities)
{
    if (!st) return 0;
    int budget = (max_entities > 0) ? max_entities : st->total;
    int done_this_call = 0;
    while (st->cursor && (max_entities <= 0 || done_this_call < budget)) {
        const cJSON *eobj = st->cursor;
        st->cursor = st->cursor->next;
        st->seen++;
        if (st->map)
            load_one_entity(st->scene, eobj, st->map, &st->loaded);
        done_this_call++;
    }
    return st->loaded;
}

bool jce_scene_load_stream_done(const JceSceneLoadStream *st)
{
    return !st || st->cursor == NULL;
}

int jce_scene_load_stream_processed(const JceSceneLoadStream *st)
{
    return st ? st->seen : 0;
}

int jce_scene_load_stream_total(const JceSceneLoadStream *st)
{
    return st ? st->total : 0;
}

uint32_t jce_scene_load_stream_new_entities(const JceSceneLoadStream *st,
                                            JceEntity *out_ids, uint32_t cap)
{
    /* The remap table already records every entity created so far in
     * map[0..loaded), map[i].new_id, in creation order.  Copy that roster
     * into the caller's buffer so a streaming host gets its per-chunk entity
     * list in O(new) instead of an O(N^2) whole-scene snapshot diff.  Pass
     * out_ids=NULL (or a too-small cap) to query the count first, then call
     * again with a buffer of at least the returned size.  Valid until
     * finalize() frees the stream.
     *
     * EntityRemap is { src_id, new_id, parent_src } — not a flat JceEntity
     * array — so we copy new_id per element rather than hand back a view. */
    uint32_t n = (st && st->loaded > 0 && st->map) ? (uint32_t)st->loaded : 0;
    if (out_ids && cap > 0) {
        uint32_t m = (n < cap) ? n : cap;
        for (uint32_t i = 0; i < m; ++i) out_ids[i] = st->map[i].new_id;
    }
    return n;
}

int jce_scene_load_stream_finalize(JceSceneLoadStream *st)
{
    if (!st) return -1;
    /* Drain any entities not yet stepped so finalize is always safe to call. */
    while (st->cursor) {
        const cJSON *eobj = st->cursor;
        st->cursor = st->cursor->next;
        st->seen++;
        if (st->map)
            load_one_entity(st->scene, eobj, st->map, &st->loaded);
    }
    if (st->map)
        load_fixup_refs(st->scene, st->map, st->loaded);
    matcache_end();
    int loaded = st->loaded;
    JCE_FREE(st->map);
    JCE_FREE(st);
    return loaded;
}

int jce_scene_load_json(JceScene *scene, const cJSON *root)
{
    if (!scene || !root) return -1;

    /* Editor-authored prefabs are a nested node tree, which the streaming
     * cursor (an entity-ARRAY walker) cannot drive — dispatch those to the
     * tree loader.  The flat entity-array path below is untouched, and a
     * document that is neither shape still fails through stream_begin. */
    if (!resolve_entities(root)) {
        const cJSON *tree_root = resolve_entity_tree_root(root);
        if (tree_root)
            return load_entity_tree_document(scene, root, tree_root);
    }

    int total = 0;
    JceSceneLoadStream *st = jce_scene_load_stream_begin(scene, root, &total);
    if (!st) {
        /* begin() returns NULL for: bad args / no entity array (→ -1) or an
         * OOM on the remap table (→ -1, the original map-alloc-fail contract).
         * A valid-but-empty entity array still yields a stream, so reaching
         * here always means a hard failure. */
        return -1;
    }

    jce_scene_load_stream_step(st, /*all=*/0);
    return jce_scene_load_stream_finalize(st);
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
                jce_editor_meta_init(&fresh);
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
                if (has_pp)
                    m->prefab_path = jce_scene_intern(scene,
                        j_str(entity_obj, "prefabPath", ""));
            }
        }
        /* P4-A.4 — mirror tag/layer into engine ECS components. */
        {
            const cJSON *tag_it = cJSON_GetObjectItemCaseSensitive(entity_obj, "tag");
            if (cJSON_IsString(tag_it) && tag_it->valuestring && tag_it->valuestring[0])
                jce_scene_set_entity_tag_name(scene, e, tag_it->valuestring);
            const cJSON *layer_it = cJSON_GetObjectItemCaseSensitive(entity_obj, "layer");
            if (cJSON_IsNumber(layer_it)) {
                int lyr = (int)layer_it->valuedouble;
                if (lyr < 0) lyr = 0;
                if (lyr > 31) lyr = 31;
                jce_scene_set_entity_layer(scene, e, (uint8_t)lyr);
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

    /* Per-component enable toggles (canonical-name array, or the legacy
     * numeric mask) — applied after the components exist. */
    parse_disabled_components(scene, e, entity_obj);
}

cJSON *jce_scene_serialize_entity_components(JceScene *scene, JceEntity e)
{
    if (!scene || e == JCE_ENTITY_INVALID) return NULL;

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    /* Single source of truth — emits the SAME component set as the
     * full-scene serializer (ser_entity_cb), so prefab-save and
     * copy/paste never silently drop a component type. */
    ser_entity_components(scene, e, arr);

    return arr;
}

/* ── Public rendering-settings JSON wrappers ───────────────────────
 * Thin TDD seam used by unit tests and offline tools.  The to_json
 * output is the "rendering" object (same as what the scene serializer
 * embeds under scene.rendering).  from_json seeds from defaults so
 * absent dome / sky keys land on the golden-hour values. */

char *jce_scene_rendering_settings_to_json(const JceSceneRenderingSettings *r)
{
    if (!r) return NULL;
    cJSON *obj = ser_scene_rendering_settings(r);
    if (!obj) return NULL;
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return s;
}

bool jce_scene_rendering_settings_from_json(const char *json,
                                            JceSceneRenderingSettings *out)
{
    if (!json || !out) return false;
    cJSON *obj = cJSON_Parse(json);
    if (!obj) return false;
    *out = extract_rendering_settings_from_obj(obj);
    cJSON_Delete(obj);
    return true;
}

/* ── Component registry registration (the single site) ─────────────
 *
 * Every engine component gets a registry row: canonical JSON "type"
 * name, accepted alias spellings (from the old parse_one_component
 * strcmp chain), the legacy JCE_COMP_FLAG_* bit (0 for post-64 rows),
 * presence/removal accessors, and the parse/serialize hooks driven by
 * parse_one_component() / ser_entity_components(). */

/* has/remove adapters for the synthetic unified-"Light" row: the JSON
 * format stores ONE "Light" object while the ECS keeps three distinct
 * light components.  DirectionalLight/PointLight/SpotLight also keep
 * their own rows (serialize == NULL) so their legacy flag bits and
 * per-type enable toggles stay addressable. */
static bool reg_has_light_any(const JceScene *s, JceEntity e)
{
    return jce_scene_has_dir_light(s, e)
        || jce_scene_has_point_light(s, e)
        || jce_scene_has_spot_light(s, e);
}

static void reg_remove_light_any(JceScene *s, JceEntity e)
{
    if (jce_scene_has_dir_light(s, e))   jce_scene_remove_dir_light(s, e);
    if (jce_scene_has_point_light(s, e)) jce_scene_remove_point_light(s, e);
    if (jce_scene_has_spot_light(s, e))  jce_scene_remove_spot_light(s, e);
}

/* Type-erased get/set adapters over the public per-type accessors.
 * jce_scene_get_x returns T* and jce_scene_set_x takes const T*; the
 * registry stores void*-shaped signatures, so generate strictly-
 * conforming thin wrapper functions (casting the function pointers
 * themselves would be undefined behavior, even though the pointer
 * representations match on every supported ABI). */
#define REG_ACCESSORS(SUFFIX, T)                                            \
    static void *reg_get_##SUFFIX(JceScene *s, JceEntity e)                 \
    {                                                                       \
        return (void *)jce_scene_get_##SUFFIX(s, e);                        \
    }                                                                       \
    static void reg_set_##SUFFIX(JceScene *s, JceEntity e, const void *d)   \
    {                                                                       \
        jce_scene_set_##SUFFIX(s, e, (const T *)d);                         \
    }

REG_ACCESSORS(transform, JceTransform)
REG_ACCESSORS(pivot, JcePivotComponent)
REG_ACCESSORS(mesh_renderer, JceMeshRenderer)
REG_ACCESSORS(camera, JceCameraComponent)
REG_ACCESSORS(dir_light, JceDirectionalLight)
REG_ACCESSORS(point_light, JcePointLight)
REG_ACCESSORS(spot_light, JceSpotLight)
REG_ACCESSORS(skybox, JceSkyboxComponent)
REG_ACCESSORS(sprite_renderer, JceSpriteRendererComponent)
REG_ACCESSORS(sprite_animator, JceSpriteAnimatorComponent)
REG_ACCESSORS(animator, JceAnimatorComponent)
REG_ACCESSORS(skeletal_animator, JceSkeletalAnimatorComponent)
REG_ACCESSORS(rigidbody, JceRigidBodyComponent)
REG_ACCESSORS(rigidbody2d, JceRigidBody2DComponent)
REG_ACCESSORS(particle_emitter, JceParticleEmitterComponent)
REG_ACCESSORS(behavior_tree, JceBehaviorTree)
REG_ACCESSORS(box_collider, JceBoxColliderComponent)
REG_ACCESSORS(sphere_collider, JceSphereColliderComponent)
REG_ACCESSORS(character_controller, JceCharacterControllerComponent)
REG_ACCESSORS(audio_source, JceAudioSourceComponent)
REG_ACCESSORS(music_track, JceMusicTrackComponent)
REG_ACCESSORS(video_player, JceVideoPlayerComponent)
REG_ACCESSORS(nav_agent, JceNavAgentComponent)
REG_ACCESSORS(ik_constraints, JceIkConstraintComponent)
REG_ACCESSORS(foot_ik, JceFootIkComponent)
REG_ACCESSORS(full_body_ik, JceFullBodyIkComponent)
REG_ACCESSORS(sequence_player, JceSequencePlayerComponent)
REG_ACCESSORS(morph_weights, JceMorphWeightsComponent)
REG_ACCESSORS(ragdoll, JceRagdollComponent)
REG_ACCESSORS(fracture, JceFractureComponent)
REG_ACCESSORS(vehicle, JceVehicleComponent)
REG_ACCESSORS(soft_body, JceSoftBodyComponent)
REG_ACCESSORS(sim_lod, JceSimLodComponent)
REG_ACCESSORS(fullscreen_effect, JceSceneFullscreenEffect)
REG_ACCESSORS(network_variable, JceNetworkVariableComponent)
REG_ACCESSORS(gas, JceGameplayAbilitySystemComponent)
REG_ACCESSORS(script, JceScriptComponent)
REG_ACCESSORS(constraint, JceConstraintComponent)
REG_ACCESSORS(terrain, JceTerrainComponent)
REG_ACCESSORS(vegetation_scatter, JceVegetationScatterComponent)
REG_ACCESSORS(water, JceWaterComponent)
REG_ACCESSORS(grass_field, JceGrassFieldComponent)
REG_ACCESSORS(foliage_cluster, JceFoliageClusterComponent)
REG_ACCESSORS(buoyancy, JceBuoyancyComponent)
REG_ACCESSORS(lod_group, JceLodGroupComponent)
REG_ACCESSORS(virtual_camera, JceVirtualCameraComponent)
REG_ACCESSORS(trigger_volume, JceTriggerVolumeComponent)
REG_ACCESSORS(capsule_collider, JceCapsuleColliderComponent)
REG_ACCESSORS(mesh_collider, JceMeshColliderComponent)
REG_ACCESSORS(compound_collider, JceCompoundColliderComponent)
REG_ACCESSORS(collider2d, JceCollider2DComponent)
REG_ACCESSORS(trail_renderer, JceTrailRendererComponent)
REG_ACCESSORS(line_renderer, JceLineRendererComponent)
REG_ACCESSORS(reflection_probe, JceReflectionProbeComponent)
REG_ACCESSORS(decal, JceDecalComponent)
REG_ACCESSORS(light_probe_group, JceLightProbeGroupComponent)
REG_ACCESSORS(audio_listener, JceAudioListenerComponent)
REG_ACCESSORS(audio_reverb_zone, JceAudioReverbZoneComponent)
REG_ACCESSORS(audio_occlusion, JceAudioOcclusionComponent)
REG_ACCESSORS(spawn_manager, JceSpawnManagerComponent)
REG_ACCESSORS(weapon, JceWeaponComponent)
REG_ACCESSORS(save_point, JceSavePointComponent)
REG_ACCESSORS(wheel_collider, JceWheelColliderComponent)
REG_ACCESSORS(constant_force, JceConstantForceComponent)
REG_ACCESSORS(configurable_joint, JceConfigurableJointComponent)
REG_ACCESSORS(cloth, JceClothComponent)
REG_ACCESSORS(joint2d, JceJoint2DComponent)
REG_ACCESSORS(billboard_renderer, JceBillboardRendererComponent)
REG_ACCESSORS(canvas, JceCanvasComponent)
REG_ACCESSORS(canvas_group, JceCanvasGroupComponent)
REG_ACCESSORS(layout_group, JceLayoutGroupComponent)
REG_ACCESSORS(ui_image, JceUIImageComponent)
REG_ACCESSORS(ui_text, JceUITextComponent)
REG_ACCESSORS(ui_button, JceUIButtonComponent)
REG_ACCESSORS(ui_slider, JceUISliderComponent)
REG_ACCESSORS(ui_toggle, JceUIToggleComponent)
REG_ACCESSORS(ui_input_field, JceUIInputFieldComponent)
REG_ACCESSORS(ui_scroll_view, JceUIScrollViewComponent)
REG_ACCESSORS(ui_progress_bar, JceUIProgressBarComponent)
REG_ACCESSORS(ui_dropdown, JceUIDropdownComponent)
REG_ACCESSORS(network_object, JceNetworkObjectComponent)
REG_ACCESSORS(net_transform, JceNetTransformComponent)
REG_ACCESSORS(net_animator, JceNetAnimatorComponent)
REG_ACCESSORS(net_rigidbody, JceNetRigidbodyComponent)
REG_ACCESSORS(tilemap, JceTilemapComponent)
REG_ACCESSORS(tilemap_collider2d, JceTilemapCollider2DComponent)
REG_ACCESSORS(avatar, JceAvatarComponent)
REG_ACCESSORS(volume, JceVolumeComponent)
REG_ACCESSORS(occlusion_portal, JceOcclusionPortalComponent)
REG_ACCESSORS(editor_meta, JceEditorMeta)

#undef REG_ACCESSORS

void jce_scene_components_register_all(void)
{
    static bool done = false;
    if (done) return;
    done = true;

    /* Registration order == the old serializer-chain order (Transform
     * first): ser_entity_components() walks rows in id order, so this
     * keeps saved-scene output byte-identical to the legacy chain. */
#define REG(NAME, A0, A1, A2, FLAG, HAS, REMOVE, PARSE, SER,                \
            GET, SET, SIZE)                                                 \
    do {                                                                    \
        JceComponentDesc reg_d = {                                          \
            NAME, { A0, A1, A2, NULL }, FLAG, HAS, REMOVE, PARSE, SER,      \
            GET, SET, (uint32_t)(SIZE)                                      \
        };                                                                  \
        jce_component_register(&reg_d);                                     \
    } while (0)

    REG("Transform", "transform", NULL, NULL,
        JCE_COMP_FLAG_TRANSFORM,
        jce_scene_has_transform, jce_scene_remove_transform,
        parse_transform, serw_transform,
        reg_get_transform, reg_set_transform, sizeof(JceTransform));

    REG("Pivot", "pivot", "ObjectPivot", "object_pivot",
        0,
        jce_scene_has_pivot, jce_scene_remove_pivot,
        parse_pivot, serw_pivot,
        reg_get_pivot, reg_set_pivot, sizeof(JcePivotComponent));

    REG("MeshRenderer", "meshRenderer", "Mesh Renderer", "mesh_renderer",
        JCE_COMP_FLAG_MESH_RENDERER,
        jce_scene_has_mesh_renderer, jce_scene_remove_mesh_renderer,
        parse_mesh_renderer, serw_mesh_renderer,
        reg_get_mesh_renderer, reg_set_mesh_renderer, sizeof(JceMeshRenderer));

    REG("Camera", "camera", NULL, NULL,
        JCE_COMP_FLAG_CAMERA,
        jce_scene_has_camera, jce_scene_remove_camera,
        parse_camera, serw_camera,
        reg_get_camera, reg_set_camera, sizeof(JceCameraComponent));

    /* Unified "Light" row: serializes ONCE for any light component.
     * The three concrete rows below keep their legacy flag bits but
     * serialize NULL (they fold into the unified row). */
    REG("Light", "light", NULL, NULL,
        0,
        reg_has_light_any, reg_remove_light_any,
        parse_unified_light, serw_light_unified,
        NULL, NULL, 0);

    REG("DirectionalLight", NULL, NULL, NULL,
        JCE_COMP_FLAG_DIR_LIGHT,
        jce_scene_has_dir_light, jce_scene_remove_dir_light,
        parse_dir_light, NULL,
        reg_get_dir_light, reg_set_dir_light, sizeof(JceDirectionalLight));

    REG("PointLight", NULL, NULL, NULL,
        JCE_COMP_FLAG_POINT_LIGHT,
        jce_scene_has_point_light, jce_scene_remove_point_light,
        parse_point_light, NULL,
        reg_get_point_light, reg_set_point_light, sizeof(JcePointLight));

    REG("SpotLight", NULL, NULL, NULL,
        JCE_COMP_FLAG_SPOT_LIGHT,
        jce_scene_has_spot_light, jce_scene_remove_spot_light,
        parse_spot_light, NULL,
        reg_get_spot_light, reg_set_spot_light, sizeof(JceSpotLight));

    REG("Skybox", "skybox", NULL, NULL,
        JCE_COMP_FLAG_SKYBOX,
        jce_scene_has_skybox, jce_scene_remove_skybox,
        parse_skybox, serw_skybox,
        reg_get_skybox, reg_set_skybox, sizeof(JceSkyboxComponent));

    REG("SpriteRenderer", "spriteRenderer", "Sprite Renderer", "sprite_renderer",
        JCE_COMP_FLAG_SPRITE_RENDERER,
        jce_scene_has_sprite_renderer, jce_scene_remove_sprite_renderer,
        parse_sprite_renderer, serw_sprite_renderer,
        reg_get_sprite_renderer, reg_set_sprite_renderer, sizeof(JceSpriteRendererComponent));

    REG("SpriteAnimator", "spriteAnimator", "Sprite Animator", NULL,
        JCE_COMP_FLAG_SPRITE_ANIMATOR,
        jce_scene_has_sprite_animator, jce_scene_remove_sprite_animator,
        parse_sprite_animator, serw_sprite_animator,
        reg_get_sprite_animator, reg_set_sprite_animator, sizeof(JceSpriteAnimatorComponent));

    REG("Animator", "animator", NULL, NULL,
        JCE_COMP_FLAG_ANIMATOR,
        jce_scene_has_animator, jce_scene_remove_animator,
        parse_animator, serw_animator,
        reg_get_animator, reg_set_animator, sizeof(JceAnimatorComponent));

    REG("SkeletalAnimator", "Skeletal Animator", NULL, NULL,
        JCE_COMP_FLAG_SKELETAL_ANIMATOR,
        jce_scene_has_skeletal_animator, jce_scene_remove_skeletal_animator,
        parse_skeletal_animator, serw_skeletal_animator,
        reg_get_skeletal_animator, reg_set_skeletal_animator, sizeof(JceSkeletalAnimatorComponent));

    REG("Rigidbody", "rigidbody", NULL, NULL,
        JCE_COMP_FLAG_RIGIDBODY,
        jce_scene_has_rigidbody, jce_scene_remove_rigidbody,
        parse_rigidbody, serw_rigidbody,
        reg_get_rigidbody, reg_set_rigidbody, sizeof(JceRigidBodyComponent));

    REG("Rigidbody2D", "rigidbody2d", "Rigidbody 2D", NULL,
        JCE_COMP_FLAG_RIGIDBODY_2D,
        jce_scene_has_rigidbody2d, jce_scene_remove_rigidbody2d,
        parse_rigidbody2d, serw_rigidbody2d,
        reg_get_rigidbody2d, reg_set_rigidbody2d, sizeof(JceRigidBody2DComponent));

    REG("ParticleEmitter", "particleEmitter", "Particle Emitter", NULL,
        JCE_COMP_FLAG_PARTICLE_EMITTER,
        jce_scene_has_particle_emitter, jce_scene_remove_particle_emitter,
        parse_particle_emitter, serw_particle_emitter,
        reg_get_particle_emitter, reg_set_particle_emitter, sizeof(JceParticleEmitterComponent));

    REG("BehaviorTree", "behaviorTree", "Behavior Tree", "BehaviourTree",
        JCE_COMP_FLAG_BEHAVIOR_TREE,
        jce_scene_has_behavior_tree, jce_scene_remove_behavior_tree,
        parse_behavior_tree, serw_behavior_tree,
        reg_get_behavior_tree, reg_set_behavior_tree, sizeof(JceBehaviorTree));

    REG("BoxCollider", "Box Collider", NULL, NULL,
        JCE_COMP_FLAG_BOX_COLLIDER,
        jce_scene_has_box_collider, jce_scene_remove_box_collider,
        parse_box_collider, serw_box_collider,
        reg_get_box_collider, reg_set_box_collider, sizeof(JceBoxColliderComponent));

    REG("SphereCollider", "Sphere Collider", NULL, NULL,
        JCE_COMP_FLAG_SPHERE_COLLIDER,
        jce_scene_has_sphere_collider, jce_scene_remove_sphere_collider,
        parse_sphere_collider, serw_sphere_collider,
        reg_get_sphere_collider, reg_set_sphere_collider, sizeof(JceSphereColliderComponent));

    REG("CharacterController", "Character Controller", NULL, NULL,
        JCE_COMP_FLAG_CHARACTER_CONTROLLER,
        jce_scene_has_character_controller, jce_scene_remove_character_controller,
        parse_character_controller, serw_character_controller,
        reg_get_character_controller, reg_set_character_controller, sizeof(JceCharacterControllerComponent));

    REG("AudioSource", "Audio Source", NULL, NULL,
        JCE_COMP_FLAG_AUDIO_SOURCE,
        jce_scene_has_audio_source, jce_scene_remove_audio_source,
        parse_audio_source, serw_audio_source,
        reg_get_audio_source, reg_set_audio_source, sizeof(JceAudioSourceComponent));

    /* Post-64 rows (no legacy flag bit): id/name addressing only. */
    REG("MusicTrack", "Music Track", NULL, NULL,
        0,
        jce_scene_has_music_track, jce_scene_remove_music_track,
        parse_music_track, serw_music_track,
        reg_get_music_track, reg_set_music_track, sizeof(JceMusicTrackComponent));

    REG("VideoPlayer", "Video Player", NULL, NULL,
        0,
        jce_scene_has_video_player, jce_scene_remove_video_player,
        parse_video_player, serw_video_player,
        reg_get_video_player, reg_set_video_player, sizeof(JceVideoPlayerComponent));

    REG("NavAgent", "navAgent", NULL, NULL,
        0,
        jce_scene_has_nav_agent, jce_scene_remove_nav_agent,
        parse_nav_agent, serw_nav_agent,
        reg_get_nav_agent, reg_set_nav_agent, sizeof(JceNavAgentComponent));

    REG("IkConstraints", "ikConstraints", NULL, NULL,
        0,
        jce_scene_has_ik_constraints, jce_scene_remove_ik_constraints,
        parse_ik_constraints, serw_ik_constraints,
        reg_get_ik_constraints, reg_set_ik_constraints, sizeof(JceIkConstraintComponent));

    REG("FootIk", "footIk", NULL, NULL,
        0,
        jce_scene_has_foot_ik, jce_scene_remove_foot_ik,
        parse_foot_ik, serw_foot_ik,
        reg_get_foot_ik, reg_set_foot_ik, sizeof(JceFootIkComponent));

    REG("FullBodyIk", "fullBodyIk", NULL, NULL,
        0,
        jce_scene_has_full_body_ik, jce_scene_remove_full_body_ik,
        parse_full_body_ik, serw_full_body_ik,
        reg_get_full_body_ik, reg_set_full_body_ik, sizeof(JceFullBodyIkComponent));

    REG("SequencePlayer", "sequencePlayer", NULL, NULL,
        0,
        jce_scene_has_sequence_player, jce_scene_remove_sequence_player,
        parse_sequence_player, serw_sequence_player,
        reg_get_sequence_player, reg_set_sequence_player, sizeof(JceSequencePlayerComponent));

    REG("MorphWeights", "morphWeights", "Morph Weights", "morph_weights",
        0,
        jce_scene_has_morph_weights, jce_scene_remove_morph_weights,
        parse_morph_weights, serw_morph_weights,
        reg_get_morph_weights, reg_set_morph_weights, sizeof(JceMorphWeightsComponent));

    REG("Ragdoll", "ragdoll", "Ragdoll", "ragdoll",
        0,
        jce_scene_has_ragdoll, jce_scene_remove_ragdoll,
        parse_ragdoll, serw_ragdoll,
        reg_get_ragdoll, reg_set_ragdoll, sizeof(JceRagdollComponent));

    REG("Fracture", "fracture", "Fracture", "fracture",
        0,
        jce_scene_has_fracture, jce_scene_remove_fracture,
        parse_fracture, serw_fracture,
        reg_get_fracture, reg_set_fracture, sizeof(JceFractureComponent));

    REG("Vehicle", "vehicle", "Vehicle", "vehicle",
        0,
        jce_scene_has_vehicle, jce_scene_remove_vehicle,
        parse_vehicle, serw_vehicle,
        reg_get_vehicle, reg_set_vehicle, sizeof(JceVehicleComponent));

    REG("SoftBody", "softBody", "Soft Body", "soft_body",
        0,
        jce_scene_has_soft_body, jce_scene_remove_soft_body,
        parse_soft_body, serw_soft_body,
        reg_get_soft_body, reg_set_soft_body, sizeof(JceSoftBodyComponent));

    REG("SimLod", "simLod", "Simulation LOD", "sim_lod",
        0,
        jce_scene_has_sim_lod, jce_scene_remove_sim_lod,
        parse_sim_lod, serw_sim_lod,
        reg_get_sim_lod, reg_set_sim_lod, sizeof(JceSimLodComponent));

    REG("NetworkVariable", "networkVariable", "Network Variable", "network_variable",
        0,
        jce_scene_has_network_variable, jce_scene_remove_network_variable,
        parse_network_variable, serw_network_variable,
        reg_get_network_variable, reg_set_network_variable, sizeof(JceNetworkVariableComponent));

    REG("GameplayAbilitySystem", "gameplayAbilitySystem", "Gameplay Ability System", "gas",
        0,
        jce_scene_has_gas, jce_scene_remove_gas,
        parse_gas, serw_gas,
        reg_get_gas, reg_set_gas, sizeof(JceGameplayAbilitySystemComponent));

    REG("Script", "script", NULL, NULL,
        JCE_COMP_FLAG_SCRIPT,
        jce_scene_has_script, jce_scene_remove_script,
        parse_script, serw_script,
        reg_get_script, reg_set_script, sizeof(JceScriptComponent));

    REG("Constraint", "constraint", NULL, NULL,
        JCE_COMP_FLAG_CONSTRAINT,
        jce_scene_has_constraint, jce_scene_remove_constraint,
        parse_constraint, serw_constraint,
        reg_get_constraint, reg_set_constraint, sizeof(JceConstraintComponent));

    REG("Terrain", "terrain", NULL, NULL,
        JCE_COMP_FLAG_TERRAIN,
        jce_scene_has_terrain, jce_scene_remove_terrain,
        parse_terrain, serw_terrain,
        reg_get_terrain, reg_set_terrain, sizeof(JceTerrainComponent));

    REG("VegetationScatter", "vegetationScatter", "Vegetation Scatter", "vegetation_scatter",
        0,
        jce_scene_has_vegetation_scatter, jce_scene_remove_vegetation_scatter,
        parse_vegetation_scatter, serw_vegetation_scatter,
        reg_get_vegetation_scatter, reg_set_vegetation_scatter,
        sizeof(JceVegetationScatterComponent));

    /* Post-64 row (no legacy flag bit): id/name addressing only. */
    REG("Water", "water", NULL, NULL,
        0,
        jce_scene_has_water, jce_scene_remove_water,
        parse_water, serw_water,
        reg_get_water, reg_set_water,
        sizeof(JceWaterComponent));

    /* Post-64 row (no legacy flag bit): id/name addressing only. */
    REG("GrassField", "grassField", NULL, NULL,
        0,
        jce_scene_has_grass_field, jce_scene_remove_grass_field,
        parse_grass_field, serw_grass_field,
        reg_get_grass_field, reg_set_grass_field,
        sizeof(JceGrassFieldComponent));

    REG("FoliageCluster", "foliageCluster", NULL, NULL,
        0,
        jce_scene_has_foliage_cluster, jce_scene_remove_foliage_cluster,
        parse_foliage_cluster, serw_foliage_cluster,
        reg_get_foliage_cluster, reg_set_foliage_cluster,
        sizeof(JceFoliageClusterComponent));

    /* Post-64 row (no legacy flag bit): id/name addressing only. */
    REG("Buoyancy", "buoyancy", NULL, NULL,
        0,
        jce_scene_has_buoyancy, jce_scene_remove_buoyancy,
        parse_buoyancy, serw_buoyancy,
        reg_get_buoyancy, reg_set_buoyancy,
        sizeof(JceBuoyancyComponent));

    REG("LODGroup", "lodGroup", NULL, NULL,
        JCE_COMP_FLAG_LOD_GROUP,
        jce_scene_has_lod_group, jce_scene_remove_lod_group,
        parse_lod_group, serw_lod_group,
        reg_get_lod_group, reg_set_lod_group, sizeof(JceLodGroupComponent));

    REG("VirtualCamera", "virtualCamera", NULL, NULL,
        JCE_COMP_FLAG_VIRTUAL_CAMERA,
        jce_scene_has_virtual_camera, jce_scene_remove_virtual_camera,
        parse_virtual_camera, serw_virtual_camera,
        reg_get_virtual_camera, reg_set_virtual_camera, sizeof(JceVirtualCameraComponent));

    REG("TriggerVolume", "triggerVolume", NULL, NULL,
        JCE_COMP_FLAG_TRIGGER_VOLUME,
        jce_scene_has_trigger_volume, jce_scene_remove_trigger_volume,
        parse_trigger_volume, serw_trigger_volume,
        reg_get_trigger_volume, reg_set_trigger_volume, sizeof(JceTriggerVolumeComponent));

    REG("CapsuleCollider", "capsuleCollider", NULL, NULL,
        JCE_COMP_FLAG_CAPSULE_COLLIDER,
        jce_scene_has_capsule_collider, jce_scene_remove_capsule_collider,
        parse_capsule_collider, serw_capsule_collider,
        reg_get_capsule_collider, reg_set_capsule_collider, sizeof(JceCapsuleColliderComponent));

    REG("MeshCollider", "meshCollider", NULL, NULL,
        JCE_COMP_FLAG_MESH_COLLIDER,
        jce_scene_has_mesh_collider, jce_scene_remove_mesh_collider,
        parse_mesh_collider, serw_mesh_collider,
        reg_get_mesh_collider, reg_set_mesh_collider, sizeof(JceMeshColliderComponent));

    REG("CompoundCollider", "compoundCollider", NULL, NULL,
        0,
        jce_scene_has_compound_collider, jce_scene_remove_compound_collider,
        parse_compound_collider, serw_compound_collider,
        reg_get_compound_collider, reg_set_compound_collider, sizeof(JceCompoundColliderComponent));

    REG("Collider2D", "collider2D", NULL, NULL,
        JCE_COMP_FLAG_COLLIDER_2D,
        jce_scene_has_collider2d, jce_scene_remove_collider2d,
        parse_collider2d, serw_collider2d,
        reg_get_collider2d, reg_set_collider2d, sizeof(JceCollider2DComponent));

    REG("TrailRenderer", "trailRenderer", NULL, NULL,
        JCE_COMP_FLAG_TRAIL_RENDERER,
        jce_scene_has_trail_renderer, jce_scene_remove_trail_renderer,
        parse_trail_renderer, serw_trail_renderer,
        reg_get_trail_renderer, reg_set_trail_renderer, sizeof(JceTrailRendererComponent));

    REG("LineRenderer", "lineRenderer", NULL, NULL,
        JCE_COMP_FLAG_LINE_RENDERER,
        jce_scene_has_line_renderer, jce_scene_remove_line_renderer,
        parse_line_renderer, serw_line_renderer,
        reg_get_line_renderer, reg_set_line_renderer, sizeof(JceLineRendererComponent));

    REG("ReflectionProbe", "reflectionProbe", NULL, NULL,
        JCE_COMP_FLAG_REFLECTION_PROBE,
        jce_scene_has_reflection_probe, jce_scene_remove_reflection_probe,
        parse_reflection_probe, serw_reflection_probe,
        reg_get_reflection_probe, reg_set_reflection_probe, sizeof(JceReflectionProbeComponent));

    REG("Decal", "decal", NULL, NULL,
        JCE_COMP_FLAG_DECAL,
        jce_scene_has_decal, jce_scene_remove_decal,
        parse_decal, serw_decal,
        reg_get_decal, reg_set_decal, sizeof(JceDecalComponent));

    REG("LightProbeGroup", "lightProbeGroup", NULL, NULL,
        JCE_COMP_FLAG_LIGHT_PROBE_GROUP,
        jce_scene_has_light_probe_group, jce_scene_remove_light_probe_group,
        parse_light_probe_group, serw_light_probe_group,
        reg_get_light_probe_group, reg_set_light_probe_group, sizeof(JceLightProbeGroupComponent));

    REG("AudioListener", "audioListener", NULL, NULL,
        JCE_COMP_FLAG_AUDIO_LISTENER,
        jce_scene_has_audio_listener, jce_scene_remove_audio_listener,
        parse_audio_listener, serw_audio_listener,
        reg_get_audio_listener, reg_set_audio_listener, sizeof(JceAudioListenerComponent));

    REG("AudioReverbZone", "audioReverbZone", NULL, NULL,
        JCE_COMP_FLAG_AUDIO_REVERB_ZONE,
        jce_scene_has_audio_reverb_zone, jce_scene_remove_audio_reverb_zone,
        parse_audio_reverb_zone, serw_audio_reverb_zone,
        reg_get_audio_reverb_zone, reg_set_audio_reverb_zone, sizeof(JceAudioReverbZoneComponent));

    REG("AudioOcclusion", "audioOcclusion", NULL, NULL,
        JCE_COMP_FLAG_AUDIO_OCCLUSION,
        jce_scene_has_audio_occlusion, jce_scene_remove_audio_occlusion,
        parse_audio_occlusion, serw_audio_occlusion,
        reg_get_audio_occlusion, reg_set_audio_occlusion, sizeof(JceAudioOcclusionComponent));

    REG("SpawnManager", "spawnManager", NULL, NULL,
        JCE_COMP_FLAG_SPAWN_MANAGER,
        jce_scene_has_spawn_manager, jce_scene_remove_spawn_manager,
        parse_spawn_manager, serw_spawn_manager,
        reg_get_spawn_manager, reg_set_spawn_manager, sizeof(JceSpawnManagerComponent));

    REG("Weapon", "weapon", NULL, NULL,
        JCE_COMP_FLAG_WEAPON,
        jce_scene_has_weapon, jce_scene_remove_weapon,
        parse_weapon, serw_weapon,
        reg_get_weapon, reg_set_weapon, sizeof(JceWeaponComponent));

    REG("SavePoint", "savePoint", NULL, NULL,
        JCE_COMP_FLAG_SAVE_POINT,
        jce_scene_has_save_point, jce_scene_remove_save_point,
        parse_save_point, serw_save_point,
        reg_get_save_point, reg_set_save_point, sizeof(JceSavePointComponent));

    REG("WheelCollider", "wheelCollider", NULL, NULL,
        JCE_COMP_FLAG_WHEEL_COLLIDER,
        jce_scene_has_wheel_collider, jce_scene_remove_wheel_collider,
        parse_wheel_collider, serw_wheel_collider,
        reg_get_wheel_collider, reg_set_wheel_collider, sizeof(JceWheelColliderComponent));

    REG("ConstantForce", "constantForce", NULL, NULL,
        JCE_COMP_FLAG_CONSTANT_FORCE,
        jce_scene_has_constant_force, jce_scene_remove_constant_force,
        parse_constant_force, serw_constant_force,
        reg_get_constant_force, reg_set_constant_force, sizeof(JceConstantForceComponent));

    REG("ConfigurableJoint", "configurableJoint", NULL, NULL,
        JCE_COMP_FLAG_CONFIGURABLE_JOINT,
        jce_scene_has_configurable_joint, jce_scene_remove_configurable_joint,
        parse_configurable_joint, serw_configurable_joint,
        reg_get_configurable_joint, reg_set_configurable_joint, sizeof(JceConfigurableJointComponent));

    REG("Cloth", "cloth", NULL, NULL,
        JCE_COMP_FLAG_CLOTH,
        jce_scene_has_cloth, jce_scene_remove_cloth,
        parse_cloth, serw_cloth,
        reg_get_cloth, reg_set_cloth, sizeof(JceClothComponent));

    REG("Joint2D", "joint2D", NULL, NULL,
        JCE_COMP_FLAG_JOINT_2D,
        jce_scene_has_joint2d, jce_scene_remove_joint2d,
        parse_joint2d, serw_joint2d,
        reg_get_joint2d, reg_set_joint2d, sizeof(JceJoint2DComponent));

    REG("BillboardRenderer", "billboardRenderer", NULL, NULL,
        JCE_COMP_FLAG_BILLBOARD_RENDERER,
        jce_scene_has_billboard_renderer, jce_scene_remove_billboard_renderer,
        parse_billboard_renderer, serw_billboard_renderer,
        reg_get_billboard_renderer, reg_set_billboard_renderer, sizeof(JceBillboardRendererComponent));

    REG("Canvas", "canvas", NULL, NULL,
        JCE_COMP_FLAG_CANVAS,
        jce_scene_has_canvas, jce_scene_remove_canvas,
        parse_canvas, serw_canvas,
        reg_get_canvas, reg_set_canvas, sizeof(JceCanvasComponent));

    REG("CanvasGroup", "canvasGroup", NULL, NULL,
        JCE_COMP_FLAG_CANVAS_GROUP,
        jce_scene_has_canvas_group, jce_scene_remove_canvas_group,
        parse_canvas_group, serw_canvas_group,
        reg_get_canvas_group, reg_set_canvas_group, sizeof(JceCanvasGroupComponent));

    REG("LayoutGroup", "layoutGroup", NULL, NULL,
        JCE_COMP_FLAG_LAYOUT_GROUP,
        jce_scene_has_layout_group, jce_scene_remove_layout_group,
        parse_layout_group, serw_layout_group,
        reg_get_layout_group, reg_set_layout_group, sizeof(JceLayoutGroupComponent));

    REG("UIImage", "uiImage", NULL, NULL,
        JCE_COMP_FLAG_UI_IMAGE,
        jce_scene_has_ui_image, jce_scene_remove_ui_image,
        parse_ui_image, serw_ui_image,
        reg_get_ui_image, reg_set_ui_image, sizeof(JceUIImageComponent));

    REG("UIText", "uiText", NULL, NULL,
        JCE_COMP_FLAG_UI_TEXT,
        jce_scene_has_ui_text, jce_scene_remove_ui_text,
        parse_ui_text, serw_ui_text,
        reg_get_ui_text, reg_set_ui_text, sizeof(JceUITextComponent));

    REG("UIButton", "uiButton", NULL, NULL,
        JCE_COMP_FLAG_UI_BUTTON,
        jce_scene_has_ui_button, jce_scene_remove_ui_button,
        parse_ui_button, serw_ui_button,
        reg_get_ui_button, reg_set_ui_button, sizeof(JceUIButtonComponent));

    /* Post-64 (presence-gated, no legacy flag bit) — mirror Vehicle/SoftBody. */
    REG("UISlider", "uiSlider", "UI Slider", "ui_slider",
        0,
        jce_scene_has_ui_slider, jce_scene_remove_ui_slider,
        parse_ui_slider, serw_ui_slider,
        reg_get_ui_slider, reg_set_ui_slider, sizeof(JceUISliderComponent));

    REG("UIToggle", "uiToggle", "UI Toggle", "ui_toggle",
        0,
        jce_scene_has_ui_toggle, jce_scene_remove_ui_toggle,
        parse_ui_toggle, serw_ui_toggle,
        reg_get_ui_toggle, reg_set_ui_toggle, sizeof(JceUIToggleComponent));

    REG("UIInputField", "uiInputField", "UI Input Field", "ui_input_field",
        0,
        jce_scene_has_ui_input_field, jce_scene_remove_ui_input_field,
        parse_ui_input_field, serw_ui_input_field,
        reg_get_ui_input_field, reg_set_ui_input_field, sizeof(JceUIInputFieldComponent));

    REG("UIScrollView", "uiScrollView", "UI Scroll View", "ui_scroll_view",
        0,
        jce_scene_has_ui_scroll_view, jce_scene_remove_ui_scroll_view,
        parse_ui_scroll_view, serw_ui_scroll_view,
        reg_get_ui_scroll_view, reg_set_ui_scroll_view, sizeof(JceUIScrollViewComponent));

    REG("UIProgressBar", "uiProgressBar", "UI Progress Bar", "ui_progress_bar",
        0,
        jce_scene_has_ui_progress_bar, jce_scene_remove_ui_progress_bar,
        parse_ui_progress_bar, serw_ui_progress_bar,
        reg_get_ui_progress_bar, reg_set_ui_progress_bar, sizeof(JceUIProgressBarComponent));

    REG("UIDropdown", "uiDropdown", "UI Dropdown", "ui_dropdown",
        0,
        jce_scene_has_ui_dropdown, jce_scene_remove_ui_dropdown,
        parse_ui_dropdown, serw_ui_dropdown,
        reg_get_ui_dropdown, reg_set_ui_dropdown, sizeof(JceUIDropdownComponent));

    REG("NetworkObject", "networkObject", NULL, NULL,
        JCE_COMP_FLAG_NETWORK_OBJECT,
        jce_scene_has_network_object, jce_scene_remove_network_object,
        parse_network_object, serw_network_object,
        reg_get_network_object, reg_set_network_object, sizeof(JceNetworkObjectComponent));

    REG("NetworkTransform", "networkTransform", NULL, NULL,
        JCE_COMP_FLAG_NET_TRANSFORM,
        jce_scene_has_net_transform, jce_scene_remove_net_transform,
        parse_net_transform, serw_net_transform,
        reg_get_net_transform, reg_set_net_transform, sizeof(JceNetTransformComponent));

    REG("NetworkAnimator", "networkAnimator", NULL, NULL,
        JCE_COMP_FLAG_NET_ANIMATOR,
        jce_scene_has_net_animator, jce_scene_remove_net_animator,
        parse_net_animator, serw_net_animator,
        reg_get_net_animator, reg_set_net_animator, sizeof(JceNetAnimatorComponent));

    REG("NetworkRigidbody", "networkRigidbody", NULL, NULL,
        JCE_COMP_FLAG_NET_RIGIDBODY,
        jce_scene_has_net_rigidbody, jce_scene_remove_net_rigidbody,
        parse_net_rigidbody, serw_net_rigidbody,
        reg_get_net_rigidbody, reg_set_net_rigidbody, sizeof(JceNetRigidbodyComponent));

    /* RETIRED v0.9.9 — parse migrates graphPath onto ParticleEmitter;
     * never re-saved (serialize == NULL). */
    REG("VfxGraph", "vfxGraph", NULL, NULL,
        JCE_COMP_FLAG_VFX_GRAPH,
        jce_scene_has_vfx_graph, jce_scene_remove_vfx_graph,
        parse_vfx_graph_migrate, NULL,
        NULL, NULL, 0);

    REG("Tilemap", "tilemap", NULL, NULL,
        JCE_COMP_FLAG_TILEMAP,
        jce_scene_has_tilemap, jce_scene_remove_tilemap,
        parse_tilemap, serw_tilemap,
        reg_get_tilemap, reg_set_tilemap, sizeof(JceTilemapComponent));

    REG("TilemapCollider2D", "tilemapCollider2D", NULL, NULL,
        JCE_COMP_FLAG_TILEMAP_COLLIDER_2D,
        jce_scene_has_tilemap_collider2d, jce_scene_remove_tilemap_collider2d,
        parse_tilemap_collider2d, serw_tilemap_collider2d,
        reg_get_tilemap_collider2d, reg_set_tilemap_collider2d, sizeof(JceTilemapCollider2DComponent));

    REG("Avatar", "avatar", NULL, NULL,
        JCE_COMP_FLAG_AVATAR,
        jce_scene_has_avatar, jce_scene_remove_avatar,
        parse_avatar, serw_avatar,
        reg_get_avatar, reg_set_avatar, sizeof(JceAvatarComponent));

    REG("Volume", "volume", NULL, NULL,
        JCE_COMP_FLAG_VOLUME,
        jce_scene_has_volume, jce_scene_remove_volume,
        parse_volume, serw_volume,
        reg_get_volume, reg_set_volume, sizeof(JceVolumeComponent));

    REG("OcclusionPortal", "occlusionPortal", NULL, NULL,
        JCE_COMP_FLAG_OCCLUSION_PORTAL,
        jce_scene_has_occlusion_portal, jce_scene_remove_occlusion_portal,
        parse_occlusion_portal, serw_occlusion_portal,
        reg_get_occlusion_portal, reg_set_occlusion_portal, sizeof(JceOcclusionPortalComponent));

    REG("EditorMeta", NULL, NULL, NULL,
        JCE_COMP_FLAG_EDITOR_META,
        jce_scene_has_editor_meta, jce_scene_remove_editor_meta,
        parse_editor_meta, serw_editor_meta,
        reg_get_editor_meta, reg_set_editor_meta, sizeof(JceEditorMeta));

    /* Tag/Layer serialize at entity level (ser_entity_cb), not in the
     * components array — rows exist for presence/enable/flags only. */
    REG("Tag", NULL, NULL, NULL,
        JCE_COMP_FLAG_TAG,
        jce_scene_has_tag_component, jce_scene_remove_tag_component,
        NULL, NULL,
        NULL, NULL, 0);

    REG("Layer", NULL, NULL, NULL,
        JCE_COMP_FLAG_LAYER,
        jce_scene_has_layer_component, jce_scene_remove_layer_component,
        NULL, NULL,
        NULL, NULL, 0);

    /* Append-only dense registry row: no legacy flag bit exists, and placing
     * the component last preserves every previously shipped dense id. */
    REG("FullscreenEffect", "fullscreenEffect", "Fullscreen Effect", "fullscreen_effect",
        0,
        jce_scene_has_fullscreen_effect, jce_scene_remove_fullscreen_effect,
        parse_fullscreen_effect, serw_fullscreen_effect,
        reg_get_fullscreen_effect, reg_set_fullscreen_effect,
        sizeof(JceSceneFullscreenEffect));

#undef REG
}

/* ================================================================== */
/* Script-facing JSON bridges (jce_scene_internal.h)                   */
/*                                                                     */
/* jce.comp_get / jce.comp_set and jce.render_get / jce.render_set     */
/* reuse the registry parse/serialize rows above, so scripts read and  */
/* write exactly the authored scene-JSON schema — including epoch      */
/* bumps and derived state, because parse routes through the typed     */
/* jce_scene_set_* accessors.                                          */
/* ================================================================== */

char *jce_scene_component_to_json(JceScene *s, JceEntity e, const char *type)
{
    if (!s || !type) return NULL;
    int comp_id = jce_component_find(type);
    if (comp_id == JCE_COMP_ID_INVALID) return NULL;
    const JceComponentDesc *d = jce_component_desc(comp_id);
    if (!d || !d->serialize || !d->has || !d->has(s, e)) return NULL;

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    d->serialize(s, e, arr);
    cJSON *first = cJSON_GetArrayItem(arr, 0);
    char *out = first ? cJSON_PrintUnformatted(first) : NULL;
    cJSON_Delete(arr);
    return out;
}

bool jce_scene_component_apply_json(JceScene *s, JceEntity e,
                                    const char *type, const char *json)
{
    if (!s || !type || !json) return false;
    int comp_id = jce_component_find(type);
    if (comp_id == JCE_COMP_ID_INVALID) return false;
    const JceComponentDesc *d = jce_component_desc(comp_id);
    if (!d || !d->parse) return false;

    cJSON *props = cJSON_Parse(json);
    if (!props) return false;
    d->parse(s, e, props);
    cJSON_Delete(props);
    return true;
}

char *jce_scene_rendering_to_json(JceScene *s)
{
    if (!s) return NULL;
    cJSON *obj = ser_scene_rendering_settings(
        jce_scene_get_rendering_settings(s));
    if (!obj) return NULL;
    char *out = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return out;
}

/* Shallow-recursive overlay: objects merge key-by-key, everything else
 * (numbers, strings, bools, ARRAYS) replaces wholesale. */
static void rs_json_merge(cJSON *dst, const cJSON *patch)
{
    for (const cJSON *it = patch->child; it; it = it->next) {
        cJSON *cur = cJSON_GetObjectItemCaseSensitive(dst, it->string);
        if (cur && cJSON_IsObject(cur) && cJSON_IsObject(it)) {
            rs_json_merge(cur, it);
        } else {
            cJSON *dup = cJSON_Duplicate(it, 1);
            if (!dup) continue;
            if (cur) cJSON_ReplaceItemInObjectCaseSensitive(dst, it->string, dup);
            else     cJSON_AddItemToObject(dst, it->string, dup);
        }
    }
}

bool jce_scene_rendering_apply_json(JceScene *s, const char *json)
{
    if (!s || !json) return false;
    cJSON *patch = cJSON_Parse(json);
    if (!patch) return false;

    cJSON *cur = ser_scene_rendering_settings(
        jce_scene_get_rendering_settings(s));
    if (!cur) { cJSON_Delete(patch); return false; }

    rs_json_merge(cur, patch);
    JceSceneRenderingSettings merged = extract_rendering_settings_from_obj(cur);
    jce_scene_set_rendering_settings(s, &merged);

    cJSON_Delete(cur);
    cJSON_Delete(patch);
    return true;
}

void jce_scene_json_free(char *str)
{
    if (str) cJSON_free(str);
}
