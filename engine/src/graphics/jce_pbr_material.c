/*
 * jce_pbr_material.c  PBR material bind implementation.
 */

#include "jce_pbr_material.h"
#include "jce_renderer_internal.h"
#include <jce/graphics/jce_texture_types.h>
#include <jce/core/jce_log.h>

#include <bgfx/c99/bgfx.h>

#define LOG_TAG "jce_pbr_material"

/* ================================================================== */
/* Static uniforms and fallback textures (lazy-initialized)            */
/* ================================================================== */

static bgfx_uniform_handle_t s_u_base_color;
static bgfx_uniform_handle_t s_u_pbr_params;    /* metallic, roughness, aoStrength, alphaCutoff */
static bgfx_uniform_handle_t s_u_emissive;       /* emissive RGB + alphaMode */
static bgfx_uniform_handle_t s_u_normal_scale;   /* x=normalScale, y=doubleSided */

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

    /* u_normalScale: x=normalScale, y=doubleSided flag */
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
