/*
 * jce_pbr_material.c  PBR material bind + JSON I/O implementation.
 */

#include <jce/graphics/jce_pbr_material.h>
#include "jce_renderer_internal.h"
#include <jce/graphics/jce_texture_types.h>
#include <jce/core/jce_log.h>

#include <bgfx/c99/bgfx.h>
#include <cjson/cJSON.h>

#include <stdio.h>
#include <string.h>
#include "core/jce_memory.h"

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
    return mat;
}

void jce_pbr_material_bind(const JcePbrMaterial *mat,
                            const JceRenderer *r, uint16_t view_id)
{
    if (!mat) return;
    (void)r;
    (void)view_id;

    ensure_uniforms();

    /* Set uniform vec4s. */
    bgfx_set_uniform(s_u_base_color, mat->base_color_factor, 1);

    /* u_pbrParams: x=metallic, y=roughness, z=aoStrength, w=alphaCutoff
       (matches fs_pbr.sc uniform declaration) */
    float pbr_params[4] = {
        mat->metallic_factor,
        mat->roughness_factor,
        mat->ao_strength,
        mat->alpha_cutoff
    };
    bgfx_set_uniform(s_u_pbr_params, pbr_params, 1);

    /* u_emissiveFactor: xyz=emissive, w=alphaMode (0=opaque,1=mask,2=blend) */
    float emissive[4] = {
        mat->emissive_factor[0],
        mat->emissive_factor[1],
        mat->emissive_factor[2],
        (float)mat->alpha_mode
    };
    bgfx_set_uniform(s_u_emissive, emissive, 1);

    /* u_normalScale: x=normalScale (x<0 => checker fallback), y=doubleSided flag */
    float normal_scale[4] = {
        mat->normal_scale,
        mat->double_sided ? 1.0f : 0.0f,
        0.0f,
        0.0f
    };
    bgfx_set_uniform(s_u_normal_scale, normal_scale, 1);

    /* Bind textures to sampler stages, using fallbacks for missing maps. */
    bgfx_texture_handle_t albedo_h = jce_texture_valid(mat->albedo_map)
        ? (bgfx_texture_handle_t){ mat->albedo_map.idx } : s_white_tex;
    bgfx_set_texture(0, s_albedo, albedo_h, UINT32_MAX);

    bgfx_texture_handle_t mr_h = jce_texture_valid(mat->metallic_roughness_map)
        ? (bgfx_texture_handle_t){ mat->metallic_roughness_map.idx } : s_white_tex;
    bgfx_set_texture(1, s_metal_rough, mr_h, UINT32_MAX);

    bgfx_texture_handle_t norm_h = jce_texture_valid(mat->normal_map)
        ? (bgfx_texture_handle_t){ mat->normal_map.idx } : s_flat_normal_tex;
    bgfx_set_texture(2, s_normal_map, norm_h, UINT32_MAX);

    bgfx_texture_handle_t ao_h = jce_texture_valid(mat->ao_map)
        ? (bgfx_texture_handle_t){ mat->ao_map.idx } : s_white_tex;
    bgfx_set_texture(3, s_ao_map, ao_h, UINT32_MAX);

    bgfx_texture_handle_t em_h = jce_texture_valid(mat->emissive_map)
        ? (bgfx_texture_handle_t){ mat->emissive_map.idx } : s_white_tex;
    bgfx_set_texture(4, s_emissive_map, em_h, UINT32_MAX);
}

/* ================================================================== */
/* JSON I/O helpers                                                    */
/* ================================================================== */

static double json_number(const cJSON *obj, const char *key, double def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) return item->valuedouble;
    return def;
}

static const char *json_string(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) return item->valuestring;
    return NULL;
}

static void json_float_array(const cJSON *obj, const char *key,
                             float *out, int n, const float *def)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsArray(arr)) {
        int count = cJSON_GetArraySize(arr);
        for (int i = 0; i < n; i++) {
            if (i < count) {
                const cJSON *e = cJSON_GetArrayItem(arr, i);
                out[i] = cJSON_IsNumber(e) ? (float)e->valuedouble : def[i];
            } else {
                out[i] = def[i];
            }
        }
    } else {
        for (int i = 0; i < n; i++) out[i] = def[i];
    }
}

static void safe_copy(char *dst, size_t dst_sz, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    size_t len = strlen(src);
    if (len >= dst_sz) len = dst_sz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* ================================================================== */
/* Load .mat.json                                                      */
/* ================================================================== */

bool jce_pbr_material_load_json(const char *path, JcePbrMaterial *out,
                                 char out_tex_paths[5][256])
{
    if (!path || !out || !out_tex_paths) return false;

    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_WARN(LOG_TAG, "cannot open material file: %s", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > (1 << 20)) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)JCE_MALLOC((size_t)sz + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    JCE_FREE(buf);
    if (!root) {
        LOG_WARN(LOG_TAG, "invalid JSON in material: %s", path);
        return false;
    }

    /* Start from defaults. */
    *out = jce_pbr_material_default();
    memset(out_tex_paths, 0, 5 * 256);

    /* Texture paths (primary keys + fallback aliases). */
    const char *tex_keys[5] = {
        "albedoMap", "metallicRoughnessMap", "normalMap", "aoMap", "emissiveMap"
    };
    const char *tex_keys_alt[5] = {
        NULL, "metallicMap", NULL, "occlusionMap", "emissionMap"
    };
    for (int i = 0; i < 5; i++) {
        const char *v = json_string(root, tex_keys[i]);
        if (!v && tex_keys_alt[i])
            v = json_string(root, tex_keys_alt[i]);
        if (v) safe_copy(out_tex_paths[i], 256, v);
    }

    /* Scalar/vector parameters. */
    static const float def_bc[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    static const float def_em[3] = { 0.0f, 0.0f, 0.0f };
    json_float_array(root, "baseColorFactor", out->base_color_factor, 4, def_bc);
    json_float_array(root, "emissiveFactor", out->emissive_factor, 3, def_em);

    /* Metallic: "metallicFactor" or legacy "metallic". */
    cJSON *jm = cJSON_GetObjectItemCaseSensitive(root, "metallicFactor");
    if (!jm) jm = cJSON_GetObjectItemCaseSensitive(root, "metallic");
    out->metallic_factor = (jm && cJSON_IsNumber(jm)) ? (float)jm->valuedouble : 0.0f;

    /* Roughness: "roughnessFactor" or legacy "smoothness" (inverted). */
    cJSON *jr = cJSON_GetObjectItemCaseSensitive(root, "roughnessFactor");
    if (jr && cJSON_IsNumber(jr)) {
        out->roughness_factor = (float)jr->valuedouble;
    } else {
        cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "smoothness");
        if (js && cJSON_IsNumber(js))
            out->roughness_factor = 1.0f - (float)js->valuedouble;
        else
            out->roughness_factor = 1.0f;
    }

    out->normal_scale     = (float)json_number(root, "normalScale",     1.0);
    out->ao_strength      = (float)json_number(root, "aoStrength",      1.0);
    out->alpha_cutoff     = (float)json_number(root, "alphaCutoff",     0.5);
    out->double_sided     = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "doubleSided"));

    /* Alpha mode. */
    const char *am = json_string(root, "alphaMode");
    if (am) {
        if (strcmp(am, "MASK") == 0)       out->alpha_mode = JCE_ALPHA_MASK;
        else if (strcmp(am, "BLEND") == 0)  out->alpha_mode = JCE_ALPHA_BLEND;
        else                                out->alpha_mode = JCE_ALPHA_OPAQUE;
    }

    cJSON_Delete(root);
    LOG_DEBUG(LOG_TAG, "loaded material: %s", path);
    return true;
}

/* ================================================================== */
/* Save .mat.json                                                      */
/* ================================================================== */

bool jce_pbr_material_save_json(const char *path,
                                 const JcePbrMaterial *mat,
                                 const char tex_paths[5][256])
{
    if (!path || !mat || !tex_paths) return false;

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;

    cJSON_AddStringToObject(root, "type", "pbr");

    /* Texture paths. */
    const char *tex_keys[5] = {
        "albedoMap", "metallicRoughnessMap", "normalMap", "aoMap", "emissiveMap"
    };
    for (int i = 0; i < 5; i++) {
        if (tex_paths[i][0])
            cJSON_AddStringToObject(root, tex_keys[i], tex_paths[i]);
    }

    /* Base color factor. */
    cJSON *bc = cJSON_CreateFloatArray(mat->base_color_factor, 4);
    cJSON_AddItemToObject(root, "baseColorFactor", bc);

    cJSON_AddNumberToObject(root, "metallicFactor",  mat->metallic_factor);
    cJSON_AddNumberToObject(root, "roughnessFactor", mat->roughness_factor);

    /* Emissive factor. */
    cJSON *em = cJSON_CreateFloatArray(mat->emissive_factor, 3);
    cJSON_AddItemToObject(root, "emissiveFactor", em);

    cJSON_AddNumberToObject(root, "normalScale",  mat->normal_scale);
    cJSON_AddNumberToObject(root, "aoStrength",   mat->ao_strength);
    cJSON_AddNumberToObject(root, "alphaCutoff",  mat->alpha_cutoff);
    cJSON_AddBoolToObject(root, "doubleSided", mat->double_sided);

    /* Alpha mode. */
    const char *am_str = "OPAQUE";
    if (mat->alpha_mode == JCE_ALPHA_MASK)  am_str = "MASK";
    if (mat->alpha_mode == JCE_ALPHA_BLEND) am_str = "BLEND";
    cJSON_AddStringToObject(root, "alphaMode", am_str);

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json_str) return false;

    FILE *f = fopen(path, "wb");
    if (!f) {
        LOG_WARN(LOG_TAG, "cannot write material file: %s", path);
        cJSON_free(json_str);
        return false;
    }
    fputs(json_str, f);
    fclose(f);
    cJSON_free(json_str);

    LOG_INFO(LOG_TAG, "saved material: %s", path);
    return true;
}
