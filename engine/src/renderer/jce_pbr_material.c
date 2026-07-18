/*
 * jce_pbr_material.c  PBR material bind + JSON I/O implementation.
 */

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_texture_types.h>

#include "jce_renderer_internal.h"
#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include "renderer/jce_render_encoder.h"

#define LOG_TAG "jce_pbr_material"

/* ================================================================== */
/* Static uniforms and fallback textures (lazy-initialized)            */
/* ================================================================== */

static bgfx_uniform_handle_t s_u_base_color;
static bgfx_uniform_handle_t s_u_pbr_params;    /* metallic, roughness, aoStrength, alphaCutoff */
static bgfx_uniform_handle_t s_u_emissive;       /* emissive RGB + alphaMode */
static bgfx_uniform_handle_t s_u_normal_scale;   /* x=normalScale (x<0 => checker fallback), y=doubleSided */

static bgfx_uniform_handle_t s_albedo;
static bgfx_uniform_handle_t s_metal_rough;
static bgfx_uniform_handle_t s_normal_map;
static bgfx_uniform_handle_t s_ao_map;
static bgfx_uniform_handle_t s_emissive_map;

static bgfx_texture_handle_t s_white_tex;        /* 1x1 white fallback */
static bgfx_texture_handle_t s_flat_normal_tex;   /* 1x1 (128,128,255,255) fallback */

static bool s_uniforms_init = false;

/* Global view mode for unlit/textured editor view; set by scene renderer
 * before each frame. 0 = SHADED (PBR default). */
static float s_view_mode = 0.0f;

void jce_pbr_material_set_view_mode(int mode)
{
    s_view_mode = (float)mode;
}

static void ensure_uniforms(void)
{
    if (s_uniforms_init) return;

    s_u_base_color    = bgfx_create_uniform("u_baseColorFactor", BGFX_UNIFORM_TYPE_VEC4, 1);
    s_u_pbr_params    = bgfx_create_uniform("u_pbrParams",       BGFX_UNIFORM_TYPE_VEC4, 1);
    s_u_emissive      = bgfx_create_uniform("u_emissiveFactor",  BGFX_UNIFORM_TYPE_VEC4, 1);
    s_u_normal_scale  = bgfx_create_uniform("u_normalScale",     BGFX_UNIFORM_TYPE_VEC4, 1);

    s_albedo      = bgfx_create_uniform("s_albedo",     BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s_metal_rough = bgfx_create_uniform("s_metalRough", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s_normal_map  = bgfx_create_uniform("s_normalMap",  BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s_ao_map      = bgfx_create_uniform("s_aoMap",      BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s_emissive_map = bgfx_create_uniform("s_emissive",  BGFX_UNIFORM_TYPE_SAMPLER, 1);

    /* 1x1 white fallback texture (RGBA8). */
    {
        uint32_t white = 0xFFFFFFFF;
        const bgfx_memory_t *mem = bgfx_copy(&white, 4);
        s_white_tex = bgfx_create_texture_2d(1, 1, false, 1,
                                              BGFX_TEXTURE_FORMAT_RGBA8, 0, mem);
    }

    /* 1x1 flat normal fallback (128, 128, 255, 255). */
    {
        uint8_t normal_data[4] = { 128, 128, 255, 255 };
        const bgfx_memory_t *mem = bgfx_copy(normal_data, 4);
        s_flat_normal_tex = bgfx_create_texture_2d(1, 1, false, 1,
                                                    BGFX_TEXTURE_FORMAT_RGBA8, 0, mem);
    }

    s_uniforms_init = true;
    LOG_DEBUG(LOG_TAG, "PBR uniforms and fallback textures initialized");
}

/* ================================================================== */
/* API                                                                 */
/* ================================================================== */

JcePbrMaterial jce_pbr_material_default(void)
{
    JcePbrMaterial mat;
    mat.albedo_map              = JCE_TEXTURE_INVALID;
    mat.metallic_roughness_map  = JCE_TEXTURE_INVALID;
    mat.normal_map              = JCE_TEXTURE_INVALID;
    mat.ao_map                  = JCE_TEXTURE_INVALID;
    mat.emissive_map            = JCE_TEXTURE_INVALID;
    mat.base_color_factor[0]    = 1.0f;
    mat.base_color_factor[1]    = 1.0f;
    mat.base_color_factor[2]    = 1.0f;
    mat.base_color_factor[3]    = 1.0f;
    mat.metallic_factor         = 0.0f;
    mat.roughness_factor        = 1.0f;
    mat.emissive_factor[0]      = 0.0f;
    mat.emissive_factor[1]      = 0.0f;
    mat.emissive_factor[2]      = 0.0f;
    mat.normal_scale            = 1.0f;
    mat.ao_strength             = 1.0f;
    mat.alpha_mode              = JCE_ALPHA_OPAQUE;
    mat.alpha_cutoff            = 0.5f;
    mat.double_sided            = false;
    mat.receive_shadows_off     = false;   /* Unity default: receive ON */
    mat.custom_program          = UINT16_MAX;
    return mat;
}

uint16_t jce_pbr_material_effective_program(const JcePbrMaterial *mat,
                                             uint16_t default_program)
{
    if (mat && mat->custom_program != UINT16_MAX)
        return mat->custom_program;
    return default_program;
}

bool jce_pbr_material_is_transparent(const JcePbrMaterial *mat)
{
    return mat && mat->alpha_mode == JCE_ALPHA_BLEND;
}

uint64_t jce_pbr_material_render_state(const JcePbrMaterial *mat)
{
    /* Base: colour write + MSAA, always.  OPAQUE / MASK keep the default
     * depth test + write + back-face cull.  BLEND enables standard
     * src-alpha / inv-src-alpha blending and disables depth write so
     * overlapping transparent surfaces composite correctly (they are
     * already sorted back-to-front by the caller). */
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                   | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA;

    if (mat && mat->alpha_mode == JCE_ALPHA_BLEND) {
        state |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                       BGFX_STATE_BLEND_INV_SRC_ALPHA);
        /* No BGFX_STATE_WRITE_Z: transparent surfaces must not occlude
         * each other in the depth buffer. */
    } else {
        state |= BGFX_STATE_WRITE_Z;
    }

    /* Double-sided materials skip back-face culling (default cull is CW). */
    if (!mat || !mat->double_sided)
        state |= BGFX_STATE_CULL_CW;

    return state;
}

void jce_pbr_material_bind(const JcePbrMaterial *mat,
                            const JceRenderer *r, uint16_t view_id)
{
    if (!mat) return;
    (void)r;
    (void)view_id;

    ensure_uniforms();

    /* Set uniform vec4s. */
    jce_enc_set_uniform(s_u_base_color, mat->base_color_factor, 1);

    /* u_pbrParams: x=metallic, y=roughness, z=aoStrength, w=alphaCutoff
       (matches fs_pbr.sc uniform declaration) */
    float pbr_params[4] = {
        mat->metallic_factor,
        mat->roughness_factor,
        mat->ao_strength,
        mat->alpha_cutoff
    };
    jce_enc_set_uniform(s_u_pbr_params, pbr_params, 1);

    /* u_emissiveFactor: xyz=emissive, w=alphaMode (0=opaque,1=mask,2=blend) */
    float emissive[4] = {
        mat->emissive_factor[0],
        mat->emissive_factor[1],
        mat->emissive_factor[2],
        (float)mat->alpha_mode
    };
    jce_enc_set_uniform(s_u_emissive, emissive, 1);

    /* u_normalScale: x=normalScale (x<0 => checker fallback),
     *                y=doubleSided flag,
     *                z=view_mode (0=shaded, 1=wireframe, 2=textured/unlit, 3=wf+tex)
     *                w=receiveShadowsOff (1 = skip all shadow sampling)
     */
    float normal_scale[4] = {
        mat->normal_scale,
        mat->double_sided ? 1.0f : 0.0f,
        s_view_mode,
        mat->receive_shadows_off ? 1.0f : 0.0f
    };
    jce_enc_set_uniform(s_u_normal_scale, normal_scale, 1);

    /* Bind textures to sampler stages, using fallbacks for missing maps. */
    bgfx_texture_handle_t albedo_h = jce_texture_valid(mat->albedo_map)
        ? (bgfx_texture_handle_t){ mat->albedo_map.idx } : s_white_tex;
    jce_enc_set_texture(0, s_albedo, albedo_h, UINT32_MAX);

    bgfx_texture_handle_t mr_h = jce_texture_valid(mat->metallic_roughness_map)
        ? (bgfx_texture_handle_t){ mat->metallic_roughness_map.idx } : s_white_tex;
    jce_enc_set_texture(1, s_metal_rough, mr_h, UINT32_MAX);

    bgfx_texture_handle_t norm_h = jce_texture_valid(mat->normal_map)
        ? (bgfx_texture_handle_t){ mat->normal_map.idx } : s_flat_normal_tex;
    jce_enc_set_texture(2, s_normal_map, norm_h, UINT32_MAX);

    bgfx_texture_handle_t ao_h = jce_texture_valid(mat->ao_map)
        ? (bgfx_texture_handle_t){ mat->ao_map.idx } : s_white_tex;
    jce_enc_set_texture(3, s_ao_map, ao_h, UINT32_MAX);

    bgfx_texture_handle_t em_h = jce_texture_valid(mat->emissive_map)
        ? (bgfx_texture_handle_t){ mat->emissive_map.idx } : s_white_tex;
    jce_enc_set_texture(4, s_emissive_map, em_h, UINT32_MAX);
}

/* ================================================================== */
/* JSON I/O helpers                                                    */
/* ================================================================== */

static double json_number(const JceJson *obj, const char *key, double def)
{
    return jce_json_get_number(obj, key, def);
}

static const char *json_string(const JceJson *obj, const char *key)
{
    return jce_json_get_string(obj, key, NULL);
}

static void json_float_array(const JceJson *obj, const char *key,
                             float *out, int n, const float *def)
{
    jce_json_get_floats(obj, key, out, n, def);
}

static void safe_copy(char *dst, size_t dst_sz, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    size_t len = strlen(src);
    if (len >= dst_sz) len = dst_sz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Resolve a (possibly relative) sibling path against the directory of
 * `base_path`.  Absolute paths and bare paths that already exist as given
 * pass through unchanged.  Mirrors the texture-resolution logic in
 * jce_scene_components_json.c so custom-shader .bin blobs load regardless
 * of the runtime cwd. */
static bool material_path_exists(const JceFileSystem *fs, const char *path)
{
    return fs ? jce_fs_exists(fs, path) : jce_fs_host_exists_file(path);
}

static void resolve_sibling_path(const JceFileSystem *fs,
                                 const char *base_path, char *io,
                                 size_t io_sz)
{
    if (!io[0]) return;
    /* Absolute path or already-existing relative path: keep as-is. */
    if (io[0] == '/' || io[0] == '\\' ||
        (io[0] && io[1] == ':') || material_path_exists(fs, io))
        return;

    const char *slash = strrchr(base_path, '/');
    const char *bslash = strrchr(base_path, '\\');
    if (bslash > slash) slash = bslash;
    if (!slash) return;   /* base has no directory component */

    size_t dir_len = (size_t)(slash - base_path) + 1;   /* keep trailing sep */
    char joined[512];
    if (dir_len >= sizeof(joined)) return;
    memcpy(joined, base_path, dir_len);
    snprintf(joined + dir_len, sizeof(joined) - dir_len, "%s", io);
    if (material_path_exists(fs, joined))
        safe_copy(io, io_sz, joined);
}

/* Process-wide cache of graph-generated programs keyed by material path.
 *
 * jce_pbr_material_load_json has many callers (scene loader, thumbnails,
 * inspector reload, material registry) that load a material, copy out what
 * they need, then discard the JcePbrMaterial.  Creating a fresh bgfx program
 * on every such load would leak.  Caching by material path makes the load
 * idempotent: the first load links the program; subsequent loads (and every
 * other caller) return the same handle.  The cache owns the programs and
 * frees them in jce_pbr_material_shutdown(). */
#define PBR_PROG_CACHE_MAX 64
static struct {
    char     mat_path[256];
    char     vs_path[256];
    char     fs_path[256];
    uint16_t program;
    bool     used;
} s_prog_cache[PBR_PROG_CACHE_MAX];
static int s_prog_cache_count = 0;

/* Load + link a custom shader program from two compiled bgfx .bin blobs.
 * Returns UINT16_MAX on any failure (missing files, bad blobs, renderer
 * not ready).  Both paths are resolved relative to `mat_path` and the
 * result is cached by (mat_path, vs, fs) so repeat loads never leak. */
static uint16_t load_custom_program(const JceFileSystem *fs,
                                    const char *mat_path,
                                    const char *vs_rel, const char *fs_rel)
{
    if (!vs_rel || !vs_rel[0] || !fs_rel || !fs_rel[0])
        return UINT16_MAX;

    /* Cache hit: same material + same blob paths → reuse the linked program. */
    for (int i = 0; i < s_prog_cache_count; i++) {
        if (s_prog_cache[i].used &&
            strncmp(s_prog_cache[i].mat_path, mat_path,
                    sizeof(s_prog_cache[i].mat_path)) == 0 &&
            strncmp(s_prog_cache[i].vs_path, vs_rel,
                    sizeof(s_prog_cache[i].vs_path)) == 0 &&
            strncmp(s_prog_cache[i].fs_path, fs_rel,
                    sizeof(s_prog_cache[i].fs_path)) == 0)
            return s_prog_cache[i].program;
    }

    char vs_path[512], fs_path[512];
    safe_copy(vs_path, sizeof(vs_path), vs_rel);
    safe_copy(fs_path, sizeof(fs_path), fs_rel);
    resolve_sibling_path(fs, mat_path, vs_path, sizeof(vs_path));
    resolve_sibling_path(fs, mat_path, fs_path, sizeof(fs_path));

    uint64_t vs_sz = 0, fs_sz = 0;
    void *vs_blob = fs ? jce_fs_read_all(fs, vs_path, &vs_sz)
                       : jce_fs_host_read_all(vs_path, &vs_sz);
    void *fs_blob = fs ? jce_fs_read_all(fs, fs_path, &fs_sz)
                       : jce_fs_host_read_all(fs_path, &fs_sz);
    uint16_t prog = UINT16_MAX;
    if (vs_blob && fs_blob && vs_sz > 0 && fs_sz > 0) {
        JceShaderHandle h = jce_renderer_create_program_from_blobs(
            vs_blob, (size_t)vs_sz, fs_blob, (size_t)fs_sz);
        prog = h.idx;
        if (prog == UINT16_MAX)
            LOG_WARN(LOG_TAG, "custom program link failed: %s", mat_path);
    } else {
        LOG_WARN(LOG_TAG, "custom shader blob(s) missing for %s", mat_path);
    }
    JCE_FREE(vs_blob);
    JCE_FREE(fs_blob);

    /* Cache the result (including failures, so we don't retry a broken blob
     * every frame).  When the table is full, fall through uncached. */
    if (s_prog_cache_count < PBR_PROG_CACHE_MAX) {
        int idx = s_prog_cache_count++;
        safe_copy(s_prog_cache[idx].mat_path,
                  sizeof(s_prog_cache[idx].mat_path), mat_path);
        safe_copy(s_prog_cache[idx].vs_path,
                  sizeof(s_prog_cache[idx].vs_path), vs_rel);
        safe_copy(s_prog_cache[idx].fs_path,
                  sizeof(s_prog_cache[idx].fs_path), fs_rel);
        s_prog_cache[idx].program = prog;
        s_prog_cache[idx].used    = true;
    }
    return prog;
}

void jce_pbr_material_shutdown(void)
{
    for (int i = 0; i < s_prog_cache_count; i++) {
        if (s_prog_cache[i].used && s_prog_cache[i].program != UINT16_MAX) {
            JceShaderHandle h = { s_prog_cache[i].program };
            jce_renderer_destroy_program(h);
        }
        s_prog_cache[i].used = false;
    }
    s_prog_cache_count = 0;
}

/* ================================================================== */
/* Load .mat.json                                                      */
/* ================================================================== */

static bool pbr_material_load_json(const JceFileSystem *fs, const char *path,
                                   JcePbrMaterial *out,
                                   char out_tex_paths[5][256])
{
    if (!path || !out || !out_tex_paths) return false;

    uint64_t sz = 0;
    char *buf = (char *)(fs ? jce_fs_read_all(fs, path, &sz)
                            : jce_fs_host_read_all(path, &sz));
    if (!buf) {
        LOG_WARN(LOG_TAG, "cannot open material file: %s", path);
        return false;
    }
    if (sz == 0 || sz > (1 << 20)) { JCE_FREE(buf); return false; }

    JceJson *root = jce_json_parse(buf, sz);
    JCE_FREE(buf);
    if (!root) {
        LOG_WARN(LOG_TAG, "invalid JSON in material: %s", path);
        return false;
    }

    /* Start from defaults. */
    *out = jce_pbr_material_default();
    memset(out_tex_paths, 0, 5 * 256);

    /* Unity-style exporters wrap params under "properties": { ... }.
     * Engine-native files put params at root. Accept both transparently. */
    JceJson *props_obj = jce_json_get(root, "properties");
    JceJson *props = jce_json_is_object(props_obj) ? props_obj : root;

    /* Texture paths (primary keys + fallback aliases). */
    const char *tex_keys[5] = {
        "albedoMap", "metallicRoughnessMap", "normalMap", "aoMap", "emissiveMap"
    };
    const char *tex_keys_alt[5] = {
        NULL, "metallicMap", NULL, "occlusionMap", "emissionMap"
    };
    for (int i = 0; i < 5; i++) {
        const char *v = json_string(props, tex_keys[i]);
        if (!v && tex_keys_alt[i])
            v = json_string(props, tex_keys_alt[i]);
        if (!v && props != root) {
            v = json_string(root, tex_keys[i]);
            if (!v && tex_keys_alt[i])
                v = json_string(root, tex_keys_alt[i]);
        }
        if (v) safe_copy(out_tex_paths[i], 256, v);
    }

    /* Scalar/vector parameters. */
    static const float def_bc[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    static const float def_em[3] = { 0.0f, 0.0f, 0.0f };
    /* Accept "baseColorFactor" or Unity-style "baseColor". */
    if (jce_json_has(props, "baseColorFactor"))
        json_float_array(props, "baseColorFactor", out->base_color_factor, 4, def_bc);
    else
        json_float_array(props, "baseColor",       out->base_color_factor, 4, def_bc);
    /* Accept "emissiveFactor" or Unity-style "emissionColor" (4-comp, drop alpha). */
    if (jce_json_has(props, "emissiveFactor")) {
        json_float_array(props, "emissiveFactor", out->emissive_factor, 3, def_em);
    } else {
        float em4[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        const float def_em4[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        json_float_array(props, "emissionColor", em4, 4, def_em4);
        out->emissive_factor[0] = em4[0];
        out->emissive_factor[1] = em4[1];
        out->emissive_factor[2] = em4[2];
    }

    /* Metallic: "metallicFactor" or legacy/unity "metallic". */
    {
        static const char *const keys[] = { "metallicFactor", "metallic" };
        out->metallic_factor = (float)jce_json_get_number_any(
            props, keys, (int)(sizeof(keys) / sizeof(keys[0])), 0.0);
    }

    /* Roughness: "roughnessFactor" or legacy/unity "smoothness" (inverted). */
    const JceJson *jr = jce_json_get(props, "roughnessFactor");
    if (jce_json_is_number(jr)) {
        out->roughness_factor = (float)jce_json_number_value(jr, 1.0);
    } else {
        const JceJson *js = jce_json_get(props, "smoothness");
        if (jce_json_is_number(js))
            out->roughness_factor = 1.0f - (float)jce_json_number_value(js, 0.0);
        else
            out->roughness_factor = 1.0f;
    }

    out->normal_scale     = (float)json_number(props, "normalScale",     1.0);
    out->ao_strength      = (float)json_number(props, "aoStrength",      1.0);
    out->alpha_cutoff     = (float)json_number(props, "alphaCutoff",     0.5);
    out->double_sided     = jce_json_get_bool(props, "doubleSided", false);

    /* Alpha mode. */
    const char *am = json_string(props, "alphaMode");
    if (am) {
        if (strcmp(am, "MASK") == 0)       out->alpha_mode = JCE_ALPHA_MASK;
        else if (strcmp(am, "BLEND") == 0)  out->alpha_mode = JCE_ALPHA_BLEND;
        else                                out->alpha_mode = JCE_ALPHA_OPAQUE;
    }

    /* Shader Graph custom shader (compiled .bin blobs persisted by the
     * editor's "Compile & Bind").  Optional — absent on plain materials. */
    {
        const char *vs_bin = json_string(props, "customProgramVs");
        const char *fs_bin = json_string(props, "customProgramFs");
        if ((!vs_bin || !fs_bin) && props != root) {
            if (!vs_bin) vs_bin = json_string(root, "customProgramVs");
            if (!fs_bin) fs_bin = json_string(root, "customProgramFs");
        }
        if (vs_bin && fs_bin)
            out->custom_program = load_custom_program(fs, path, vs_bin, fs_bin);
    }

    jce_json_free(root);
    LOG_DEBUG(LOG_TAG, "loaded material: %s", path);
    return true;
}

bool jce_pbr_material_load_json(const char *path, JcePbrMaterial *out,
                                 char out_tex_paths[5][256])
{
    return pbr_material_load_json(NULL, path, out, out_tex_paths);
}

bool jce_pbr_material_load_json_vfs(const JceFileSystem *fs,
                                    const char *path, JcePbrMaterial *out,
                                    char out_tex_paths[5][256])
{
    return fs && pbr_material_load_json(fs, path, out, out_tex_paths);
}

/* ================================================================== */
/* Save .mat.json                                                      */
/* ================================================================== */

bool jce_pbr_material_save_json(const char *path,
                                 const JcePbrMaterial *mat,
                                 const char tex_paths[5][256])
{
    if (!path || !mat || !tex_paths) return false;

    JceJson *root = jce_json_object();
    if (!root) return false;

    jce_json_set_string(root, "type", "pbr");

    /* Texture paths. */
    const char *tex_keys[5] = {
        "albedoMap", "metallicRoughnessMap", "normalMap", "aoMap", "emissiveMap"
    };
    for (int i = 0; i < 5; i++) {
        if (tex_paths[i][0])
            jce_json_set_string(root, tex_keys[i], tex_paths[i]);
    }

    /* Base color factor. */
    jce_json_set_float_array(root, "baseColorFactor", mat->base_color_factor, 4);

    jce_json_set_number(root, "metallicFactor",  mat->metallic_factor);
    jce_json_set_number(root, "roughnessFactor", mat->roughness_factor);

    /* Emissive factor. */
    jce_json_set_float_array(root, "emissiveFactor", mat->emissive_factor, 3);

    jce_json_set_number(root, "normalScale",  mat->normal_scale);
    jce_json_set_number(root, "aoStrength",   mat->ao_strength);
    jce_json_set_number(root, "alphaCutoff",  mat->alpha_cutoff);
    jce_json_set_bool(root, "doubleSided", mat->double_sided);

    /* Alpha mode. */
    const char *am_str = "OPAQUE";
    if (mat->alpha_mode == JCE_ALPHA_MASK)  am_str = "MASK";
    if (mat->alpha_mode == JCE_ALPHA_BLEND) am_str = "BLEND";
    jce_json_set_string(root, "alphaMode", am_str);

    /* Preserve an existing Shader Graph reference: callers that only edit
     * PBR factors (inspector "Save Material", graph "Compile") rewrite the
     * whole file, which would otherwise drop the graph-shader keys. */
    if (jce_fs_host_exists_file(path)) {
        uint64_t prev_sz = 0;
        char *prev_buf = (char *)jce_fs_host_read_all(path, &prev_sz);
        if (prev_buf) {
            JceJson *prev = (prev_sz > 0 && prev_sz <= (1 << 20))
                ? jce_json_parse(prev_buf, prev_sz) : NULL;
            jce_fs_buffer_free(prev_buf);
            if (prev) {
                static const char *const keep[3] = {
                    "shaderGraph", "customProgramVs", "customProgramFs"
                };
                for (int i = 0; i < 3; i++) {
                    const char *v = jce_json_get_string(prev, keep[i], NULL);
                    if (v && v[0]) jce_json_set_string(root, keep[i], v);
                }
                jce_json_free(prev);
            }
        }
    }

    char *json_str = jce_json_print(root, true);
    jce_json_free(root);
    if (!json_str) return false;

    size_t len = strlen(json_str);
    bool ok = jce_fs_host_write_all(path, json_str, len);
    jce_json_free_string(json_str);
    if (!ok) {
        LOG_WARN(LOG_TAG, "cannot write material file: %s", path);
        return false;
    }

    LOG_INFO(LOG_TAG, "saved material: %s", path);
    return true;
}

/* ================================================================== */
/* Shader Graph reference (read-modify-write)                          */
/* ================================================================== */

bool jce_pbr_material_set_graph_shader(const char *mat_path,
                                       const char *graph_path,
                                       const char *vs_bin_path,
                                       const char *fs_bin_path)
{
    if (!mat_path || !mat_path[0]) return false;

    /* Load existing material so every other field round-trips untouched.
     * If the file does not exist yet, start from a default material so the
     * graph reference can still be attached. */
    JceJson *root = NULL;
    if (jce_fs_host_exists_file(mat_path)) {
        uint64_t sz = 0;
        char *buf = (char *)jce_fs_host_read_all(mat_path, &sz);
        if (buf) {
            if (sz > 0 && sz <= (1 << 20))
                root = jce_json_parse(buf, sz);
            jce_fs_buffer_free(buf);
        }
    }
    if (!root || !jce_json_is_object(root)) {
        if (root) jce_json_free(root);
        root = jce_json_object();
        if (!root) return false;
        jce_json_set_string(root, "type", "pbr");
    }

    /* Replace cleanly: set helpers append, so drop any prior copies first. */
    jce_json_remove(root, "shaderGraph");
    jce_json_remove(root, "customProgramVs");
    jce_json_remove(root, "customProgramFs");

    bool attach = (vs_bin_path && vs_bin_path[0]) ||
                  (fs_bin_path && fs_bin_path[0]);
    if (attach) {
        if (graph_path && graph_path[0])
            jce_json_set_string(root, "shaderGraph", graph_path);
        if (vs_bin_path && vs_bin_path[0])
            jce_json_set_string(root, "customProgramVs", vs_bin_path);
        if (fs_bin_path && fs_bin_path[0])
            jce_json_set_string(root, "customProgramFs", fs_bin_path);
    }

    char *json_str = jce_json_print(root, true);
    jce_json_free(root);
    if (!json_str) return false;

    size_t len = strlen(json_str);
    bool ok = jce_fs_host_write_all(mat_path, json_str, len);
    jce_json_free_string(json_str);
    if (!ok) {
        LOG_WARN(LOG_TAG, "cannot write material file: %s", mat_path);
        return false;
    }
    LOG_INFO(LOG_TAG, "%s graph shader -> %s",
             attach ? "attached" : "detached", mat_path);
    return true;
}
