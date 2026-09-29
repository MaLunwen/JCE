/*
 * jce_pbr_material.c  PBR material bind + JSON I/O implementation.
 */

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/renderer/jce_shader_variants.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_shaders.h>   /* backend suffix for graph blobs */
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

/* THE MATERIAL'S UNIFORM HANDLES, in one object.
 *
 * Nine separate file statics is nine globals, and the dedup audit counts them
 * -- adding the two the extended lobes needed took the count past its
 * baseline.  Folding them is not a workaround for that gate: they are created
 * together, live for the process together, and are only ever used together,
 * which is what one object means.  It also gives the set a name, so the next
 * uniform is added to something rather than beside everything.
 *
 *   u_pbrLobes  : x=clearcoat, y=clearcoatRoughness, z=sheenRoughness, w=unused
 *   u_sheenColor: xyz=sheen colour (linear), w=unused
 *   u_pbrLobes2 : x=anisotropy, y=anisotropyRotation (turns),
 *                 z=translucency, w=translucencyThickness
 *   u_translucencyColor: xyz=tint, w=unused
 */
static struct {
    bgfx_uniform_handle_t base_color;
    bgfx_uniform_handle_t pbr_params;    /* metallic, roughness, aoStrength, alphaCutoff */
    bgfx_uniform_handle_t emissive;      /* emissive RGB + alphaMode */
    bgfx_uniform_handle_t normal_scale;  /* x=normalScale (x<0 => checker fallback), y=doubleSided */
    bgfx_uniform_handle_t uv_transform;
    bgfx_uniform_handle_t pbr_lobes;
    bgfx_uniform_handle_t sheen_color;
    bgfx_uniform_handle_t pbr_lobes2;
    bgfx_uniform_handle_t translucency_color;
} s_mu;
/* Per-draw UV transform override; see the header.  Same shape and lifetime as
 * jce_model.c's s_material_override, and armed by the same code.
 *
 * ONE static, not a value plus an `active` flag: x <= 0 means DISARMED,
 * because a tiling of zero collapses every map to a single texel and is
 * therefore not a value anyone can mean.  The bind below already substitutes
 * 1 for a zero tiling for exactly that reason, so the sentinel costs no
 * expressiveness -- and a second file-scope global to say "the first one is
 * valid" is a second thing that can be left stale. */
static float s_uv_override[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

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

/* Defined below; the view-mode bind above it needs the handles created. */
static void ensure_uniforms(void);

void jce_pbr_material_set_view_mode(int mode)
{
    s_view_mode = (float)mode;
}

void jce_pbr_material_bind_view_mode(void)
{
    ensure_uniforms();
    /* .z only -- see the header. */
    float ns[4] = { 0.0f, 0.0f, s_view_mode, 0.0f };
    jce_enc_set_uniform(s_mu.normal_scale, ns, 1);
}

static void ensure_uniforms(void)
{
    if (s_uniforms_init) return;

    s_mu.base_color    = bgfx_create_uniform("u_baseColorFactor", BGFX_UNIFORM_TYPE_VEC4, 1);
    s_mu.pbr_params    = bgfx_create_uniform("u_pbrParams",       BGFX_UNIFORM_TYPE_VEC4, 1);
    s_mu.emissive      = bgfx_create_uniform("u_emissiveFactor",  BGFX_UNIFORM_TYPE_VEC4, 1);
    s_mu.normal_scale  = bgfx_create_uniform("u_normalScale",     BGFX_UNIFORM_TYPE_VEC4, 1);
    s_mu.uv_transform  = bgfx_create_uniform("u_uvTransform",     BGFX_UNIFORM_TYPE_VEC4, 1);
    s_mu.pbr_lobes     = bgfx_create_uniform("u_pbrLobes",        BGFX_UNIFORM_TYPE_VEC4, 1);
    s_mu.sheen_color   = bgfx_create_uniform("u_sheenColor",      BGFX_UNIFORM_TYPE_VEC4, 1);
    s_mu.pbr_lobes2    = bgfx_create_uniform("u_pbrLobes2",       BGFX_UNIFORM_TYPE_VEC4, 1);
    s_mu.translucency_color = bgfx_create_uniform("u_translucencyColor", BGFX_UNIFORM_TYPE_VEC4, 1);

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
                                              BGFX_TEXTURE_FORMAT_RGBA8, 0, mem, 0);
    }

    /* 1x1 flat normal fallback (128, 128, 255, 255). */
    {
        uint8_t normal_data[4] = { 128, 128, 255, 255 };
        const bgfx_memory_t *mem = bgfx_copy(normal_data, 4);
        s_flat_normal_tex = bgfx_create_texture_2d(1, 1, false, 1,
                                                    BGFX_TEXTURE_FORMAT_RGBA8, 0, mem, 0);
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
    mat.render_priority         = 0;   /* neutral: sort by depth alone */
    mat.double_sided            = false;
    mat.receive_shadows_off     = false;   /* Unity default: receive ON */
    mat.custom_program          = UINT16_MAX;
    /* (1,1)/(0,0) = sample the mesh's UVs unchanged.  See the header: a
     * zeroed tiling collapses every texture to a single texel, so this is the
     * one field pair here that must not come from a memset. */
    mat.uv_tiling[0]            = 1.0f;
    mat.uv_tiling[1]            = 1.0f;
    mat.uv_offset[0]            = 0.0f;
    mat.uv_offset[1]            = 0.0f;
    mat.blend_mode              = JCE_BLEND_ALPHA;   /* the historical equation */
    /* Stencil OFF, and every op KEEP: a material must not touch a buffer
     * other draws read unless it says so. */
    mat.stencil_func            = JCE_STENCIL_OFF;
    mat.stencil_ref             = 0;
    mat.stencil_read_mask       = 0xFF;
    mat.stencil_fail_op         = JCE_STENCIL_OP_KEEP;
    mat.stencil_zfail_op        = JCE_STENCIL_OP_KEEP;
    mat.stencil_pass_op         = JCE_STENCIL_OP_KEEP;
    /* Both lobes OFF.  Zero is not merely the neutral value here, it is the
     * branch predicate in the shader: the extra evaluation is skipped
     * entirely, so an untouched material costs nothing. */
    mat.clearcoat               = 0.0f;
    mat.clearcoat_roughness     = 0.0f;
    mat.anisotropy              = 0.0f;
    mat.anisotropy_rotation     = 0.0f;
    mat.translucency            = 0.0f;
    mat.translucency_thickness  = 0.0f;
    mat.translucency_color[0]   = 1.0f;
    mat.translucency_color[1]   = 1.0f;
    mat.translucency_color[2]   = 1.0f;
    mat.sheen_color[0]          = 0.0f;
    mat.sheen_color[1]          = 0.0f;
    mat.sheen_color[2]          = 0.0f;
    mat.sheen_roughness         = 0.0f;
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


/* JceStencilFunc -> bgfx test bits.  A table rather than arithmetic on the
 * bgfx constants: they are a bit field, not an enumeration, and the mapping
 * has to survive bgfx renumbering them. */
static uint32_t pbr_stencil_test(uint8_t f)
{
    switch (f) {
    case JCE_STENCIL_NEVER:    return BGFX_STENCIL_TEST_NEVER;
    case JCE_STENCIL_LESS:     return BGFX_STENCIL_TEST_LESS;
    case JCE_STENCIL_LEQUAL:   return BGFX_STENCIL_TEST_LEQUAL;
    case JCE_STENCIL_EQUAL:    return BGFX_STENCIL_TEST_EQUAL;
    case JCE_STENCIL_GEQUAL:   return BGFX_STENCIL_TEST_GEQUAL;
    case JCE_STENCIL_GREATER:  return BGFX_STENCIL_TEST_GREATER;
    case JCE_STENCIL_NOTEQUAL: return BGFX_STENCIL_TEST_NOTEQUAL;
    case JCE_STENCIL_ALWAYS:   return BGFX_STENCIL_TEST_ALWAYS;
    default:                   return 0u;   /* OFF */
    }
}

static uint32_t pbr_stencil_op_fail_s(uint8_t o)
{
    switch (o) {
    case JCE_STENCIL_OP_ZERO:      return BGFX_STENCIL_OP_FAIL_S_ZERO;
    case JCE_STENCIL_OP_REPLACE:   return BGFX_STENCIL_OP_FAIL_S_REPLACE;
    case JCE_STENCIL_OP_INCR:      return BGFX_STENCIL_OP_FAIL_S_INCRSAT;
    case JCE_STENCIL_OP_INCR_WRAP: return BGFX_STENCIL_OP_FAIL_S_INCR;
    case JCE_STENCIL_OP_DECR:      return BGFX_STENCIL_OP_FAIL_S_DECRSAT;
    case JCE_STENCIL_OP_DECR_WRAP: return BGFX_STENCIL_OP_FAIL_S_DECR;
    case JCE_STENCIL_OP_INVERT:    return BGFX_STENCIL_OP_FAIL_S_INVERT;
    default:                       return BGFX_STENCIL_OP_FAIL_S_KEEP;
    }
}

static uint32_t pbr_stencil_op_fail_z(uint8_t o)
{
    switch (o) {
    case JCE_STENCIL_OP_ZERO:      return BGFX_STENCIL_OP_FAIL_Z_ZERO;
    case JCE_STENCIL_OP_REPLACE:   return BGFX_STENCIL_OP_FAIL_Z_REPLACE;
    case JCE_STENCIL_OP_INCR:      return BGFX_STENCIL_OP_FAIL_Z_INCRSAT;
    case JCE_STENCIL_OP_INCR_WRAP: return BGFX_STENCIL_OP_FAIL_Z_INCR;
    case JCE_STENCIL_OP_DECR:      return BGFX_STENCIL_OP_FAIL_Z_DECRSAT;
    case JCE_STENCIL_OP_DECR_WRAP: return BGFX_STENCIL_OP_FAIL_Z_DECR;
    case JCE_STENCIL_OP_INVERT:    return BGFX_STENCIL_OP_FAIL_Z_INVERT;
    default:                       return BGFX_STENCIL_OP_FAIL_Z_KEEP;
    }
}

static uint32_t pbr_stencil_op_pass_z(uint8_t o)
{
    switch (o) {
    case JCE_STENCIL_OP_ZERO:      return BGFX_STENCIL_OP_PASS_Z_ZERO;
    case JCE_STENCIL_OP_REPLACE:   return BGFX_STENCIL_OP_PASS_Z_REPLACE;
    case JCE_STENCIL_OP_INCR:      return BGFX_STENCIL_OP_PASS_Z_INCRSAT;
    case JCE_STENCIL_OP_INCR_WRAP: return BGFX_STENCIL_OP_PASS_Z_INCR;
    case JCE_STENCIL_OP_DECR:      return BGFX_STENCIL_OP_PASS_Z_DECRSAT;
    case JCE_STENCIL_OP_DECR_WRAP: return BGFX_STENCIL_OP_PASS_Z_DECR;
    case JCE_STENCIL_OP_INVERT:    return BGFX_STENCIL_OP_PASS_Z_INVERT;
    default:                       return BGFX_STENCIL_OP_PASS_Z_KEEP;
    }
}

uint32_t jce_pbr_material_stencil(const JcePbrMaterial *mat)
{
    if (!mat) return 0u;
    const uint32_t test = pbr_stencil_test(mat->stencil_func);
    if (test == 0u) return 0u;   /* OFF: no stencil word at all */

    /* A mask of 0 would test and write NOTHING, which is not a setting anyone
     * means -- it is the value a zeroed struct holds.  0xFF is the neutral
     * "all bits", and it is what Unity's fields default to. */
    return test
         | BGFX_STENCIL_FUNC_REF((uint32_t)mat->stencil_ref)
         | BGFX_STENCIL_FUNC_RMASK((uint32_t)(mat->stencil_read_mask
                                              ? mat->stencil_read_mask : 0xFFu))
         | pbr_stencil_op_fail_s(mat->stencil_fail_op)
         | pbr_stencil_op_fail_z(mat->stencil_zfail_op)
         | pbr_stencil_op_pass_z(mat->stencil_pass_op);
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
        /* WHICH equation, not whether.  NONE is treated as ALPHA: a
         * transparent surface that does not blend is a contradiction, and an
         * unset field must land on the behaviour every material had before
         * this was read rather than on "invisible". */
        switch (mat->blend_mode) {
        case JCE_BLEND_ADD:
            /* src*a + dst.  The source is still weighted by alpha, so the
             * material's opacity still controls how much it adds -- an
             * additive surface at alpha 0 contributes nothing, which is what
             * a fade-out needs. */
            state |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                           BGFX_STATE_BLEND_ONE);
            break;
        case JCE_BLEND_MULTIPLY:
            /* src*dst.  DST_COLOR/ZERO rather than the "lerp toward white by
             * alpha" form: this is the equation a shadow decal or a tint
             * layer means, and the alpha-weighted variant is expressible by
             * authoring the texture instead. */
            state |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_DST_COLOR,
                                           BGFX_STATE_BLEND_ZERO);
            break;
        case JCE_BLEND_NONE:
        case JCE_BLEND_ALPHA:
        default:
            state |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                           BGFX_STATE_BLEND_INV_SRC_ALPHA);
            break;
        }
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

void jce_pbr_material_bind_texture_overrides(
    const JcePbrMaterial *mat, const JceRenderer *r, uint16_t view_id,
    JceTexture albedo_override, JceTexture emissive_override)
{
    if (!mat) return;
    (void)r;
    (void)view_id;

    ensure_uniforms();

    /* Set uniform vec4s. */
    jce_enc_set_uniform(s_mu.base_color, mat->base_color_factor, 1);

    /* u_pbrParams: x=metallic, y=roughness, z=aoStrength, w=alphaCutoff
       (matches fs_pbr.sc uniform declaration) */
    float pbr_params[4] = {
        mat->metallic_factor,
        mat->roughness_factor,
        mat->ao_strength,
        mat->alpha_cutoff
    };
    jce_enc_set_uniform(s_mu.pbr_params, pbr_params, 1);

    /* Extended lobes.  Uploaded UNCONDITIONALLY: the shader reads .x and .z
     * to decide whether to evaluate them at all, so a material with no coat
     * must be told zero rather than inherit the previous material run's
     * values -- which is how a single car-paint material would have made
     * every object after it in the frame glossy. */
    {
        const float lobes[4] = {
            mat->clearcoat, mat->clearcoat_roughness, mat->sheen_roughness, 0.0f
        };
        jce_enc_set_uniform(s_mu.pbr_lobes, lobes, 1);
        const float sheen[4] = {
            mat->sheen_color[0], mat->sheen_color[1], mat->sheen_color[2], 0.0f
        };
        jce_enc_set_uniform(s_mu.sheen_color, sheen, 1);
        /* Unconditionally, for the reason above: a material with no
         * anisotropy must be TOLD zero, or it inherits the brushed metal that
         * was drawn before it. */
        const float lobes2[4] = {
            mat->anisotropy, mat->anisotropy_rotation,
            mat->translucency, mat->translucency_thickness
        };
        jce_enc_set_uniform(s_mu.pbr_lobes2, lobes2, 1);
        const float tcol[4] = {
            mat->translucency_color[0], mat->translucency_color[1],
            mat->translucency_color[2], 0.0f
        };
        jce_enc_set_uniform(s_mu.translucency_color, tcol, 1);
    }

    /* u_emissiveFactor: xyz=emissive, w=alphaMode (0=opaque,1=mask,2=blend) */
    float emissive[4] = {
        mat->emissive_factor[0],
        mat->emissive_factor[1],
        mat->emissive_factor[2],
        (float)mat->alpha_mode
    };
    jce_enc_set_uniform(s_mu.emissive, emissive, 1);

    /* u_normalScale: x=normalScale (x<0 => checker fallback),
     *                y=doubleSided flag,
     *                z=view_mode (0=shaded, 1=wireframe, 2=textured/unlit, 3=wf+tex)
     *                w=receiveShadowsOff (1 = skip all shadow sampling)
     */
    /* Normal-map scale is ZERO when no normal map is bound.
     *
     * The shader perturbs the shading normal under `normalScale > 0`, and
     * normal_scale defaults to 1.0 -- so that branch ran for every material,
     * including the overwhelming majority that have no normal map at all. It
     * sampled the flat fallback texture and then built a tangent basis out of
     * v_tangent / v_bitangent to transform a vector that was (0,0,1) anyway.
     *
     * Costly, and worse, WRONG for any mesh whose vertex layout has no
     * TANGENT: the missing attribute reads back as (0,0,0,1) on GLES, so the
     * basis was built from normalize(vec3(0)) and the resulting NaN wiped out
     * every lit term. That is what rendered the space demo's Earth -- a
     * procedural sphere, position+normal+texcoord only -- black on WebGL2
     * while the glTF models beside it lit correctly.
     *
     * The binder already knows the answer (it picks the fallback texture two
     * dozen lines below); it simply never told the shader. Saying so here
     * fixes the whole CLASS -- any untangented mesh, on any backend -- and
     * removes a texture fetch, a normalize and a mat3 build per fragment from
     * every material without a normal map.
     *
     * The sign is load-bearing: x < 0 selects the checker fallback (see
     * useCheckerFallback in fs_pbr_body.sh), so only a POSITIVE scale with no
     * map is zeroed. */
    const bool has_normal_map = jce_texture_valid(mat->normal_map);
    const float effective_normal_scale =
        (!has_normal_map && mat->normal_scale > 0.0f) ? 0.0f
                                                      : mat->normal_scale;
    float normal_scale[4] = {
        effective_normal_scale,
        mat->double_sided ? 1.0f : 0.0f,
        s_view_mode,
        mat->receive_shadows_off ? 1.0f : 0.0f
    };
    jce_enc_set_uniform(s_mu.normal_scale, normal_scale, 1);

    /* Tiling of exactly 0 would make every map a single texel, which reads as
     * "the textures are gone" rather than as "the tiling is 0".  Treat it as
     * the neutral 1 -- an unset value in an old asset and a typed 0 are
     * indistinguishable here, and only one of them is a thing anyone wants. */
    const bool uv_armed = (s_uv_override[0] > 0.0f);
    const float *tile = uv_armed ? &s_uv_override[0] : mat->uv_tiling;
    const float *off  = uv_armed ? &s_uv_override[2] : mat->uv_offset;
    float uv_xform[4] = {
        (tile[0] != 0.0f) ? tile[0] : 1.0f,
        (tile[1] != 0.0f) ? tile[1] : 1.0f,
        off[0], off[1]
    };
    jce_enc_set_uniform(s_mu.uv_transform, uv_xform, 1);

    /* Bind textures to sampler stages, using fallbacks for missing maps. */
    bgfx_texture_handle_t albedo_h = jce_texture_valid(albedo_override)
        ? (bgfx_texture_handle_t){ albedo_override.idx }
        : (jce_texture_valid(mat->albedo_map)
            ? (bgfx_texture_handle_t){ mat->albedo_map.idx } : s_white_tex);
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

    bgfx_texture_handle_t em_h = jce_texture_valid(emissive_override)
        ? (bgfx_texture_handle_t){ emissive_override.idx }
        : (jce_texture_valid(mat->emissive_map)
            ? (bgfx_texture_handle_t){ mat->emissive_map.idx } : s_white_tex);
    jce_enc_set_texture(4, s_emissive_map, em_h, UINT32_MAX);
}

void jce_pbr_material_set_uv_override(const float tiling[2],
                                     const float offset[2])
{
    if (!tiling && !offset) { s_uv_override[0] = 0.0f; return; }
    s_uv_override[0] = (tiling && tiling[0] > 0.0f) ? tiling[0] : 1.0f;
    s_uv_override[1] = (tiling && tiling[1] != 0.0f) ? tiling[1] : 1.0f;
    s_uv_override[2] = offset ? offset[0] : 0.0f;
    s_uv_override[3] = offset ? offset[1] : 0.0f;
}

uint32_t jce_pbr_material_shader_keys(const JcePbrMaterial *mat)
{
    if (!mat) return 0u;
    uint32_t keys = 0u;
    if (mat->receive_shadows_off) keys |= JCE_SHADER_KEY_NOSHADOW;
    return keys;
}

void jce_pbr_material_bind(const JcePbrMaterial *mat,
                           const JceRenderer *r, uint16_t view_id)
{
    jce_pbr_material_bind_texture_overrides(
        mat, r, view_id, JCE_TEXTURE_INVALID, JCE_TEXTURE_INVALID);
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
/* Resolve one graph blob: `<stem>_<backend>.bin` if it exists, else the bare
 * name.  Both are resolved relative to the material, as before. */
static void custom_blob_path(const JceFileSystem *fs, const char *mat_path,
                             const char *rel, char *out, size_t out_sz)
{
    const char *sfx = jce_shaders_backend_suffix();
    if (sfx && sfx[0]) {
        char stem[512];
        safe_copy(stem, sizeof(stem), rel);
        char *dot = strrchr(stem, '.');
        if (dot && strcmp(dot, ".bin") == 0) *dot = 0;
        char cand[512];
        snprintf(cand, sizeof(cand), "%s_%s.bin", stem, sfx);
        char resolved[512];
        safe_copy(resolved, sizeof(resolved), cand);
        resolve_sibling_path(fs, mat_path, resolved, sizeof(resolved));
        if (material_path_exists(fs, resolved)) {
            safe_copy(out, out_sz, resolved);
            return;
        }
    }
    safe_copy(out, out_sz, rel);
    resolve_sibling_path(fs, mat_path, out, out_sz);
}

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
    /* PREFER THE BACKEND-SUFFIXED BLOB.  A graph blob is bgfx bytecode for one
     * backend -- D3D bytecode, SPIR-V, GLSL source -- and the editor used to
     * write it under a bare name compiled for whatever backend IT was running.
     * The same file then meant different things on different machines, so a
     * graph material rendered where it was authored and link-failed anywhere
     * else, which is one of the two reasons a Shader Graph look did not
     * survive the cook.
     *
     * The engine's own shaders have always been per-backend
     * (`fs_pbr_dx11.bin`, `_glsl`, `_spv`); this is the same convention and
     * the same suffix function.  The bare name is still tried second, so a
     * material authored before this keeps working on the machine that made
     * it. */
    custom_blob_path(fs, mat_path, vs_rel, vs_path, sizeof(vs_path));
    custom_blob_path(fs, mat_path, fs_rel, fs_path, sizeof(fs_path));

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
/* Material texture keys (single authority)                            */
/* ================================================================== */

/* Per-slot .mat.json key list: index 0 is the canonical key that
 * jce_pbr_material_save_json writes, the rest are aliases the loader also
 * accepts, in the order it tries them.  Every consumer that has to find a
 * texture reference inside a .mat.json goes through
 * jce_pbr_material_texture_keys() instead of hard-coding its own spellings:
 * the editor's material preview used to carry a DIFFERENT albedo alias list
 * (baseColorMap / diffuseMap / mainTexture), so a preview could resolve a
 * texture this loader never read for the very same file, and the bundle
 * packer (jce_bundle_deps.c kAssetKeys) cooked those aliases into the PAK
 * where the runtime then ignored them. */
static const char *const kAlbedoKeys[] = {
    "albedoMap", "baseColorMap", "diffuseMap", "mainTexture", NULL
};
static const char *const kMetalRoughKeys[] = {
    "metallicRoughnessMap", "metallicMap", NULL
};
static const char *const kNormalKeys[]   = { "normalMap", NULL };
static const char *const kAoKeys[]       = { "aoMap", "occlusionMap", NULL };
static const char *const kEmissiveKeys[] = { "emissiveMap", "emissionMap", NULL };

static const char *const *const kTexKeys[5] = {
    kAlbedoKeys, kMetalRoughKeys, kNormalKeys, kAoKeys, kEmissiveKeys
};

const char *const *jce_pbr_material_texture_keys(int slot)
{
    if (slot < 0 || slot >= 5)
        return NULL;
    return kTexKeys[slot];
}

/* ================================================================== */
/* Load .mat.json                                                      */
/* ================================================================== */

/* How deep a parent chain may go.  UE allows instance-of-instance without a
 * stated limit; a bound is here because a cycle is a hang and a deep chain is
 * a file-open per level on every material load. */
#define PBR_PARENT_MAX_DEPTH 8

/* Same path by a different spelling is still the same path: compare with
 * separators normalised and case folded, because a cycle written as
 * "a/b.mat.json" and "a\\B.mat.json" is still a cycle. */
static bool pbr_path_equal(const char *a, const char *b)
{
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca == '\\') ca = '/';
        if (cb == '\\') cb = '/';
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        if (!ca) return true;
    }
}

static bool pbr_material_load_json_depth(const JceFileSystem *fs,
                                         const char *path,
                                         JcePbrMaterial *out,
                                         char out_tex_paths[5][256],
                                         int depth,
                                         const char *const *chain, int n_chain);

static bool pbr_material_load_json(const JceFileSystem *fs, const char *path,
                                   JcePbrMaterial *out,
                                   char out_tex_paths[5][256])
{
    return pbr_material_load_json_depth(fs, path, out, out_tex_paths, 0, NULL, 0);
}

static bool pbr_material_load_json_depth(const JceFileSystem *fs,
                                         const char *path,
                                         JcePbrMaterial *out,
                                         char out_tex_paths[5][256],
                                         int depth,
                                         const char *const *chain, int n_chain)
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

    /* Unity-style exporters wrap params under "properties": { ... }.
     * Engine-native files put params at root. Accept both transparently. */
    JceJson *props_obj = jce_json_get(root, "properties");
    JceJson *props = jce_json_is_object(props_obj) ? props_obj : root;

    /* PARENT.  Start from it instead of from the defaults, so every key this
     * file omits keeps the parent's value.  Resolved against THIS file's
     * directory, the same way the texture and shader-blob paths are, so a
     * chain works from any working directory.
     *
     * The key is read at the root as well as under "properties" because a
     * Unity-style export puts parameters in the latter -- but it is written
     * at the root, where it describes the FILE rather than one of its
     * parameters. */
    char parent_path[256] = { 0 };
    bool inherited = false;
    {
        const char *pv = json_string(root, "parent");
        if (!pv && props != root) pv = json_string(props, "parent");
        if (pv && pv[0]) {
            safe_copy(parent_path, sizeof parent_path, pv);
            resolve_sibling_path(fs, path, parent_path, sizeof parent_path);

            bool cycle = pbr_path_equal(parent_path, path);
            for (int i = 0; i < n_chain && !cycle; i++)
                if (pbr_path_equal(parent_path, chain[i])) cycle = true;

            if (cycle) {
                LOG_WARN(LOG_TAG,
                         "material parent cycle ignored: %s -> %s",
                         path, parent_path);
            } else if (depth >= PBR_PARENT_MAX_DEPTH) {
                LOG_WARN(LOG_TAG,
                         "material parent chain deeper than %d, stopping at %s",
                         PBR_PARENT_MAX_DEPTH, path);
            } else {
                /* `path` joins the chain, not `parent_path`: what must not
                 * reappear below is the file we are loading NOW.  Both live
                 * on this frame and stay alive for the whole recursive call. */
                const char *next[PBR_PARENT_MAX_DEPTH + 1];
                int nn = 0;
                for (; nn < n_chain && nn < PBR_PARENT_MAX_DEPTH; nn++)
                    next[nn] = chain[nn];
                next[nn++] = path;
                inherited = pbr_material_load_json_depth(
                    fs, parent_path, out, out_tex_paths, depth + 1, next, nn);
                if (!inherited)
                    LOG_WARN(LOG_TAG, "material parent failed to load: %s (of %s)",
                             parent_path, path);
            }
        }
    }
    if (!inherited) {
        *out = jce_pbr_material_default();
        memset(out_tex_paths, 0, 5 * 256);
    }

    /* Texture paths: try every key the slot accepts (canonical first, then
     * aliases — see kTexKeys) under "properties", then again at the root.
     * An empty string counts as absent so a blank primary key falls through
     * to its aliases instead of masking them. */
    for (int i = 0; i < 5; i++) {
        const char *const *keys = jce_pbr_material_texture_keys(i);
        const char *v = NULL;
        for (int k = 0; keys[k] && !v; k++) {
            const char *s = json_string(props, keys[k]);
            if (s && s[0]) v = s;
        }
        if (!v && props != root) {
            for (int k = 0; keys[k] && !v; k++) {
                const char *s = json_string(root, keys[k]);
                if (s && s[0]) v = s;
            }
        }
        if (v) safe_copy(out_tex_paths[i], 256, v);
    }

    /* Scalar/vector parameters.
     *
     * EVERY default below is the value already in *out -- which is the
     * parent's when this file names one, and jce_pbr_material_default()'s
     * when it does not.  A literal here would read as "the default" and
     * behave as "discard what the parent said", and half of this block used
     * to be written that way. */
    /* Accept "baseColorFactor" or Unity-style "baseColor". */
    if (jce_json_has(props, "baseColorFactor")) {
        const float def[4] = { out->base_color_factor[0], out->base_color_factor[1],
                               out->base_color_factor[2], out->base_color_factor[3] };
        json_float_array(props, "baseColorFactor", out->base_color_factor, 4, def);
    } else if (jce_json_has(props, "baseColor")) {
        const float def[4] = { out->base_color_factor[0], out->base_color_factor[1],
                               out->base_color_factor[2], out->base_color_factor[3] };
        json_float_array(props, "baseColor", out->base_color_factor, 4, def);
    }
    /* Accept "emissiveFactor" or Unity-style "emissionColor" (4-comp, drop alpha). */
    if (jce_json_has(props, "emissiveFactor")) {
        const float def[3] = { out->emissive_factor[0], out->emissive_factor[1],
                               out->emissive_factor[2] };
        json_float_array(props, "emissiveFactor", out->emissive_factor, 3, def);
    } else if (jce_json_has(props, "emissionColor")) {
        float em4[4] = { out->emissive_factor[0], out->emissive_factor[1],
                         out->emissive_factor[2], 1.0f };
        const float def_em4[4] = { em4[0], em4[1], em4[2], 1.0f };
        json_float_array(props, "emissionColor", em4, 4, def_em4);
        out->emissive_factor[0] = em4[0];
        out->emissive_factor[1] = em4[1];
        out->emissive_factor[2] = em4[2];
    }

    /* Metallic: "metallicFactor" or legacy/unity "metallic". */
    {
        static const char *const keys[] = { "metallicFactor", "metallic" };
        out->metallic_factor = (float)jce_json_get_number_any(
            props, keys, (int)(sizeof(keys) / sizeof(keys[0])),
            (double)out->metallic_factor);
    }

    /* Roughness: "roughnessFactor" or legacy/unity "smoothness" (inverted). */
    const JceJson *jr = jce_json_get(props, "roughnessFactor");
    if (jce_json_is_number(jr)) {
        out->roughness_factor = (float)jce_json_number_value(jr, 1.0);
    } else {
        const JceJson *js = jce_json_get(props, "smoothness");
        if (jce_json_is_number(js))
            out->roughness_factor = 1.0f - (float)jce_json_number_value(js, 0.0);
    }

    out->normal_scale     = (float)json_number(props, "normalScale",
                                               (double)out->normal_scale);
    out->ao_strength      = (float)json_number(props, "aoStrength",
                                               (double)out->ao_strength);
    out->alpha_cutoff     = (float)json_number(props, "alphaCutoff",
                                               (double)out->alpha_cutoff);
    /* Transparent draw-order override; absent key -> whatever is already
     * there, so a material authored before this existed sorts exactly as it
     * did (default 0) and a child keeps its parent's priority. */
    out->render_priority  = (int16_t)json_number(props, "renderPriority",
                                                 (double)out->render_priority);
    out->uv_tiling[0]     = (float)json_number(props, "uvTilingX",  (double)out->uv_tiling[0]);
    out->uv_tiling[1]     = (float)json_number(props, "uvTilingY",  (double)out->uv_tiling[1]);
    out->uv_offset[0]     = (float)json_number(props, "uvOffsetX",  (double)out->uv_offset[0]);
    out->uv_offset[1]     = (float)json_number(props, "uvOffsetY",  (double)out->uv_offset[1]);
    out->blend_mode       = (JceBlendMode)(int)json_number(props, "blendMode",
                                                           (double)out->blend_mode);
    out->stencil_func       = (uint8_t)json_number(props, "stencilFunc",      (double)out->stencil_func);
    out->stencil_ref        = (uint8_t)json_number(props, "stencilRef",       (double)out->stencil_ref);
    out->stencil_read_mask  = (uint8_t)json_number(props, "stencilReadMask",  (double)out->stencil_read_mask);
    out->stencil_fail_op    = (uint8_t)json_number(props, "stencilFailOp",    (double)out->stencil_fail_op);
    out->stencil_zfail_op   = (uint8_t)json_number(props, "stencilZFailOp",   (double)out->stencil_zfail_op);
    out->stencil_pass_op    = (uint8_t)json_number(props, "stencilPassOp",    (double)out->stencil_pass_op);
    /* glTF spells these clearcoatFactor / clearcoatRoughnessFactor /
     * sheenColorFactor / sheenRoughnessFactor; the short forms are what this
     * project's own writer emits, and both are accepted. */
    out->clearcoat = (float)jce_json_get_number(
        props, "clearcoatFactor",
        json_number(props, "clearcoat", (double)out->clearcoat));
    out->clearcoat_roughness = (float)jce_json_get_number(
        props, "clearcoatRoughnessFactor",
        json_number(props, "clearcoatRoughness", (double)out->clearcoat_roughness));
    out->sheen_roughness = (float)jce_json_get_number(
        props, "sheenRoughnessFactor",
        json_number(props, "sheenRoughness", (double)out->sheen_roughness));
    /* glTF spells these anisotropyStrength / anisotropyRotation; the short
     * forms are what this project's writer emits.  Translucency has no glTF
     * extension -- KHR_materials_transmission and _volume describe something
     * else -- so it carries only its own names. */
    out->anisotropy = (float)jce_json_get_number(
        props, "anisotropyStrength",
        json_number(props, "anisotropy", (double)out->anisotropy));
    out->anisotropy_rotation = (float)jce_json_get_number(
        props, "anisotropyRotation",
        json_number(props, "anisotropyRot", (double)out->anisotropy_rotation));
    out->translucency = (float)json_number(props, "translucency",
                                           (double)out->translucency);
    out->translucency_thickness = (float)json_number(
        props, "translucencyThickness", (double)out->translucency_thickness);
    if (jce_json_has(props, "translucencyColor")) {
        const float def[3] = { out->translucency_color[0],
                               out->translucency_color[1],
                               out->translucency_color[2] };
        json_float_array(props, "translucencyColor",
                         out->translucency_color, 3, def);
    }
    if (jce_json_has(props, "sheenColorFactor")) {
        const float def[3] = { out->sheen_color[0], out->sheen_color[1],
                               out->sheen_color[2] };
        json_float_array(props, "sheenColorFactor", out->sheen_color, 3, def);
    } else if (jce_json_has(props, "sheenColor")) {
        const float def[3] = { out->sheen_color[0], out->sheen_color[1],
                               out->sheen_color[2] };
        json_float_array(props, "sheenColor", out->sheen_color, 3, def);
    }
    out->double_sided     = jce_json_get_bool(props, "doubleSided",
                                              out->double_sided);

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

    /* Texture paths — always the CANONICAL key of each slot (index 0 of the
     * loader's alias list, so save and load can never drift apart); aliases
     * read on load are normalised away and never written back out. */
    for (int i = 0; i < 5; i++) {
        if (tex_paths[i][0])
            jce_json_set_string(root, jce_pbr_material_texture_keys(i)[0],
                                tex_paths[i]);
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
    jce_json_set_number(root, "renderPriority", (double)mat->render_priority);
    jce_json_set_number(root, "uvTilingX", (double)mat->uv_tiling[0]);
    jce_json_set_number(root, "uvTilingY", (double)mat->uv_tiling[1]);
    jce_json_set_number(root, "uvOffsetX", (double)mat->uv_offset[0]);
    jce_json_set_number(root, "uvOffsetY", (double)mat->uv_offset[1]);
    jce_json_set_number(root, "blendMode", (double)(int)mat->blend_mode);
    if (mat->stencil_func != JCE_STENCIL_OFF) {
        /* Written only when the material USES the stencil: an off block is
         * seven keys of noise in every .mat.json in the project. */
        jce_json_set_number(root, "stencilFunc",      (double)mat->stencil_func);
        jce_json_set_number(root, "stencilRef",       (double)mat->stencil_ref);
        jce_json_set_number(root, "stencilReadMask",  (double)mat->stencil_read_mask);
        jce_json_set_number(root, "stencilFailOp",    (double)mat->stencil_fail_op);
        jce_json_set_number(root, "stencilZFailOp",   (double)mat->stencil_zfail_op);
        jce_json_set_number(root, "stencilPassOp",    (double)mat->stencil_pass_op);
    }
    jce_json_set_bool(root, "doubleSided", mat->double_sided);
    /* Written only when USED, for the reason the stencil block above is:
     * every .mat.json in the tree predates these, and four keys of zeroes on
     * each of them is noise. */
    if (mat->clearcoat > 0.0f) {
        jce_json_set_number(root, "clearcoatFactor", (double)mat->clearcoat);
        jce_json_set_number(root, "clearcoatRoughnessFactor",
                            (double)mat->clearcoat_roughness);
    }
    if (mat->sheen_color[0] > 0.0f || mat->sheen_color[1] > 0.0f ||
        mat->sheen_color[2] > 0.0f) {
        jce_json_set_float_array(root, "sheenColorFactor", mat->sheen_color, 3);
        jce_json_set_number(root, "sheenRoughnessFactor",
                            (double)mat->sheen_roughness);
    }

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
                /* "parent" is in this list for the same reason the graph keys
                 * are, and it matters more: every caller here rewrites the
                 * whole file from a JcePbrMaterial, which has no parent field
                 * -- so without this the first Save silently severs the link
                 * and the child becomes an unrelated copy that still looks
                 * like a variant. */
                static const char *const keep[4] = {
                    "shaderGraph", "customProgramVs", "customProgramFs", "parent"
                };
                for (int i = 0; i < 4; i++) {
                    const char *v = jce_json_get_string(prev, keep[i], NULL);
                    if (v && v[0]) jce_json_set_string(root, keep[i], v);
                }
                jce_json_free(prev);
            }
        }
    }

    /* A CHILD RECORDS ONLY WHAT DIFFERS.  Everything above wrote the fully
     * resolved value set, which is right for a root material and wrong for a
     * variant: keep it and the first Save freezes today's parent values into
     * the child, so editing the parent afterwards reaches nothing and the
     * link is decorative.  UE's Material Instances and Unity's Material
     * Variants both store overrides only, and this is that.
     *
     * Exact comparison, deliberately: "the child says what the parent says"
     * is a byte-identical round-trip through the same writer, and a tolerance
     * would silently drop an override an author had typed. */
    {
        const char *pv = jce_json_get_string(root, "parent", NULL);
        if (pv && pv[0]) {
            char pp[256];
            safe_copy(pp, sizeof pp, pv);
            resolve_sibling_path(NULL, path, pp, sizeof pp);

            JcePbrMaterial pm;
            char ptex[5][256];
            if (!pbr_path_equal(pp, path) &&
                pbr_material_load_json(NULL, pp, &pm, ptex)) {
                for (int i = 0; i < 5; i++) {
                    const char *const *keys = jce_pbr_material_texture_keys(i);
                    if (keys && keys[0] &&
                        strcmp(tex_paths[i], ptex[i]) == 0)
                        jce_json_remove(root, keys[0]);
                }
                if (mat->base_color_factor[0] == pm.base_color_factor[0] &&
                    mat->base_color_factor[1] == pm.base_color_factor[1] &&
                    mat->base_color_factor[2] == pm.base_color_factor[2] &&
                    mat->base_color_factor[3] == pm.base_color_factor[3])
                    jce_json_remove(root, "baseColorFactor");
                if (mat->emissive_factor[0] == pm.emissive_factor[0] &&
                    mat->emissive_factor[1] == pm.emissive_factor[1] &&
                    mat->emissive_factor[2] == pm.emissive_factor[2])
                    jce_json_remove(root, "emissiveFactor");
                if (mat->metallic_factor  == pm.metallic_factor)
                    jce_json_remove(root, "metallicFactor");
                if (mat->roughness_factor == pm.roughness_factor)
                    jce_json_remove(root, "roughnessFactor");
                if (mat->normal_scale     == pm.normal_scale)
                    jce_json_remove(root, "normalScale");
                if (mat->ao_strength      == pm.ao_strength)
                    jce_json_remove(root, "aoStrength");
                if (mat->alpha_cutoff     == pm.alpha_cutoff)
                    jce_json_remove(root, "alphaCutoff");
                if (mat->render_priority  == pm.render_priority)
                    jce_json_remove(root, "renderPriority");
                if (mat->uv_tiling[0] == pm.uv_tiling[0])
                    jce_json_remove(root, "uvTilingX");
                if (mat->uv_tiling[1] == pm.uv_tiling[1])
                    jce_json_remove(root, "uvTilingY");
                if (mat->uv_offset[0] == pm.uv_offset[0])
                    jce_json_remove(root, "uvOffsetX");
                if (mat->uv_offset[1] == pm.uv_offset[1])
                    jce_json_remove(root, "uvOffsetY");
                if (mat->blend_mode   == pm.blend_mode)
                    jce_json_remove(root, "blendMode");
                if (mat->double_sided == pm.double_sided)
                    jce_json_remove(root, "doubleSided");
                if (mat->alpha_mode   == pm.alpha_mode)
                    jce_json_remove(root, "alphaMode");
                if (mat->stencil_func      == pm.stencil_func &&
                    mat->stencil_ref       == pm.stencil_ref &&
                    mat->stencil_read_mask == pm.stencil_read_mask &&
                    mat->stencil_fail_op   == pm.stencil_fail_op &&
                    mat->stencil_zfail_op  == pm.stencil_zfail_op &&
                    mat->stencil_pass_op   == pm.stencil_pass_op) {
                    jce_json_remove(root, "stencilFunc");
                    jce_json_remove(root, "stencilRef");
                    jce_json_remove(root, "stencilReadMask");
                    jce_json_remove(root, "stencilFailOp");
                    jce_json_remove(root, "stencilZFailOp");
                    jce_json_remove(root, "stencilPassOp");
                }
            } else {
                /* The link is unusable.  The values stay in the file, so the
                 * material still renders as authored -- a broken parent must
                 * not turn a saved material into the defaults. */
                LOG_WARN(LOG_TAG,
                         "material parent unreadable, saving flat: %s (of %s)",
                         pp, path);
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

bool jce_pbr_material_get_parent(const char *mat_path, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return false;
    out[0] = '\0';
    if (!mat_path || !mat_path[0]) return false;

    uint64_t sz = 0;
    char *buf = (char *)jce_fs_host_read_all(mat_path, &sz);
    if (!buf) return false;
    JceJson *root = (sz > 0 && sz <= (1 << 20)) ? jce_json_parse(buf, sz) : NULL;
    jce_fs_buffer_free(buf);
    if (!root) return false;

    /* Root first, then "properties": written at the root, but a Unity-style
     * export puts everything under properties and the loader accepts both. */
    const char *v = jce_json_get_string(root, "parent", NULL);
    if (!v || !v[0]) {
        JceJson *props = jce_json_get(root, "properties");
        if (jce_json_is_object(props))
            v = jce_json_get_string(props, "parent", NULL);
    }
    bool ok = (v && v[0]);
    if (ok) safe_copy(out, out_sz, v);
    jce_json_free(root);
    return ok;
}

bool jce_pbr_material_set_parent(const char *mat_path, const char *parent_path)
{
    if (!mat_path || !mat_path[0]) return false;
    /* Self-parenting is the one cycle this layer can see; the rest needs the
     * whole chain and is caught by the loader. */
    if (parent_path && parent_path[0] && pbr_path_equal(mat_path, parent_path)) {
        LOG_WARN(LOG_TAG, "material cannot be its own parent: %s", mat_path);
        return false;
    }

    JceJson *root = NULL;
    if (jce_fs_host_exists_file(mat_path)) {
        uint64_t sz = 0;
        char *buf = (char *)jce_fs_host_read_all(mat_path, &sz);
        if (buf) {
            if (sz > 0 && sz <= (1 << 20)) root = jce_json_parse(buf, sz);
            jce_fs_buffer_free(buf);
        }
    }
    if (!root || !jce_json_is_object(root)) {
        if (root) jce_json_free(root);
        root = jce_json_object();
        if (!root) return false;
        jce_json_set_string(root, "type", "pbr");
    }

    /* Both places, because the loader reads both and a file that says two
     * different parents has no defined answer. */
    jce_json_remove(root, "parent");
    {
        JceJson *props = jce_json_get(root, "properties");
        if (jce_json_is_object(props)) jce_json_remove(props, "parent");
    }
    if (parent_path && parent_path[0])
        jce_json_set_string(root, "parent", parent_path);

    char *json_str = jce_json_print(root, true);
    jce_json_free(root);
    if (!json_str) return false;
    bool ok = jce_fs_host_write_all(mat_path, json_str, strlen(json_str));
    jce_json_free_string(json_str);
    if (!ok) {
        LOG_WARN(LOG_TAG, "cannot write material file: %s", mat_path);
        return false;
    }
    LOG_INFO(LOG_TAG, "%s parent -> %s",
             (parent_path && parent_path[0]) ? "set" : "cleared", mat_path);
    return true;
}

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
