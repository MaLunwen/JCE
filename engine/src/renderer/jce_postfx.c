/*
 * jce_postfx.c  Post-processing effect pipeline implementation.
 *
 * Manages a chain of full-screen passes.  Each pass is a simple
 * full-screen triangle rendered with a specific fragment shader
 * and the previous pass's output as input texture.
 */

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_timer.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/renderer/jce_views.h>

#include <bgfx/c99/bgfx.h>
#include <string.h>

#define LOG_TAG "postfx"

/* Max intermediate FBOs for ping-pong rendering. */
#define POSTFX_MAX_FBOS  4

/* ── Pipeline struct ───────────────────────────────────────────────── */

struct JcePostFXPipeline {
    jce_allocator_t alloc;
    uint32_t        width;
    uint32_t        height;
    bool            enabled[JCE_POSTFX_COUNT];
    JcePostFXParams params;
    bool            shaders_loaded;
    uint16_t        view_base;  /* First bgfx view ID used by jce_postfx_apply(). */

    /* Intermediate framebuffers for ping-pong rendering. */
    bgfx_texture_handle_t    fbo_tex[POSTFX_MAX_FBOS];
    bgfx_frame_buffer_handle_t fbo[POSTFX_MAX_FBOS];
    bool                     fbos_valid;

    /* Shader programs. */
    bgfx_program_handle_t prog_tonemap;
    bgfx_program_handle_t prog_bloom_extract;
    bgfx_program_handle_t prog_bloom_blur;
    bgfx_program_handle_t prog_bloom_combine;
    bgfx_program_handle_t prog_fxaa;
    bgfx_program_handle_t prog_vignette;
    bgfx_program_handle_t prog_chromatic;
    bgfx_program_handle_t prog_grayscale;
    bgfx_program_handle_t prog_composite;  /* uber: combine+tonemap+chromatic+vignette+grayscale */
    bgfx_program_handle_t prog_present;    /* pass-through: output -> backbuffer (runtime path) */

    /* Uniforms. */
    bgfx_uniform_handle_t u_texColor;
    bgfx_uniform_handle_t u_texBloom;
    bgfx_uniform_handle_t u_tonemapParams;
    bgfx_uniform_handle_t u_bloomParams;
    bgfx_uniform_handle_t u_blurDir;
    bgfx_uniform_handle_t u_fxaaParams;
    bgfx_uniform_handle_t u_texelSize;
    bgfx_uniform_handle_t u_vignetteParams;
    bgfx_uniform_handle_t u_chromaticParams;
    bgfx_uniform_handle_t u_compositeFlags;   /* x=bloom y=tonemap z=chromatic w=vignette */
    bgfx_uniform_handle_t u_compositeFlags2;  /* x=grayscale */
    bgfx_uniform_handle_t u_texDepth;         /* scene depth sampler (stage 1, custom pass) */
    bgfx_uniform_handle_t u_postfxTime;       /* (elapsed_s, has_depth, 0, 0) — custom pass */
    bgfx_uniform_handle_t u_postfxParams;     /* generic vec4[JCE_POSTFX_CUSTOM_PARAMS] */

    /* Custom (client) pass — data-driven; the engine is style-agnostic. */
    const JcePakArchive  *shader_pak;         /* retained from load_shaders for lazy custom load */
    bgfx_program_handle_t prog_custom;        /* lazily (re)loaded when custom_name changes */
    char  custom_name[64];                    /* requested custom fs base name ("" = none) */
    char  custom_loaded[64];                  /* name currently compiled into prog_custom */
    bool  custom_needs_depth;
    int   custom_param_count;
    float custom_params[JCE_POSTFX_CUSTOM_PARAMS * 4];

    /* Full-screen quad vertex buffer. */
    bgfx_vertex_buffer_handle_t quad_vb;
    bgfx_index_buffer_handle_t  quad_ib;
    bgfx_vertex_layout_t        quad_layout;

    /* Output texture and framebuffer from the last apply. */
    bgfx_texture_handle_t      output_tex;
    bgfx_frame_buffer_handle_t output_fb;
    int                        output_ping;   /* index into fbo[] */
};

static void reset_output_state(JcePostFXPipeline *pipeline)
{
    if (!pipeline) return;

    pipeline->output_tex = (bgfx_texture_handle_t){ UINT16_MAX };
    pipeline->output_fb = (bgfx_frame_buffer_handle_t){ UINT16_MAX };
    pipeline->output_ping = -1;
}

/* ── Default parameters ────────────────────────────────────────────── */

JcePostFXParams jce_postfx_default_params(void)
{
    JcePostFXParams p;
    memset(&p, 0, sizeof(p));
    p.exposure             = 1.0f;
    p.gamma                = 2.2f;
    p.bloom_threshold      = 1.0f;
    p.bloom_intensity      = 0.5f;
    p.fxaa_span_max        = 8.0f;
    p.fxaa_reduce_min      = 1.0f / 128.0f;
    p.fxaa_reduce_mul      = 1.0f / 8.0f;
    p.vignette_intensity   = 0.3f;
    p.vignette_smoothness  = 2.0f;
    p.chromatic_strength   = 0.005f;
    return p;
}

/* ── Full-screen quad vertex data ───────────────────────────────────── */

typedef struct {
    float x, y, z;
    float u, v;
} PostfxVertex;

static const PostfxVertex s_quad_verts[4] = {
    { -1.0f,  1.0f, 0.0f,   0.0f, 0.0f },
    {  1.0f,  1.0f, 0.0f,   1.0f, 0.0f },
    { -1.0f, -1.0f, 0.0f,   0.0f, 1.0f },
    {  1.0f, -1.0f, 0.0f,   1.0f, 1.0f },
};

static const uint16_t s_quad_indices[6] = { 0, 2, 1, 1, 2, 3 };

/* ── FBO helpers ───────────────────────────────────────────────────── */

/* Allocate one full-res RGBA16F intermediate FBO (color texture + framebuffer).
 * destroyTextures=true so destroying the FB also frees the attached texture —
 * otherwise destroy_fbos() leaks handles, which during ImGui drag-resize
 * exhausts bgfx's texture pool and yields recycled-handle AVs in the driver. */
static void alloc_one_fbo(JcePostFXPipeline *p, int i)
{
    p->fbo_tex[i] = bgfx_create_texture_2d(
        (uint16_t)p->width, (uint16_t)p->height, false, 1,
        BGFX_TEXTURE_FORMAT_RGBA16F,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        NULL);
    bgfx_attachment_t at;
    memset(&at, 0, sizeof(at));
    bgfx_attachment_init(&at, p->fbo_tex[i], BGFX_ACCESS_WRITE, 0, 1, 0, BGFX_RESOLVE_NONE);
    p->fbo[i] = bgfx_create_frame_buffer_from_attachment(1, &at, true);
}

static void create_fbos(JcePostFXPipeline *p)
{
    if (p->fbos_valid) return;
    /* Standard "allocate only what the enabled chain needs": create just the
     * first composite target (FBO 0) up front.  The second ping-pong target
     * (FBO 1) is allocated lazily by ensure_composite_fbo() only when the chain
     * has a 2nd full-screen pass (FXAA / custom); the bloom buffers (2/3)
     * lazily by ensure_bloom_fbos().  So a single-pass chain (tonemap-only — the
     * LOW-tier case) uses ONE full-res RGBA16F target instead of four
     * (~48MB saved at 1080p). */
    alloc_one_fbo(p, 0);
    p->fbos_valid = true;
}

/* Lazily allocate a composite ping-pong target (FBO 0 or 1); no-op if present. */
static void ensure_composite_fbo(JcePostFXPipeline *p, int i)
{
    if (p->fbo[i].idx != UINT16_MAX) return;
    alloc_one_fbo(p, i);
}

/* Lazily allocate the bloom downsample/blur buffers (FBO 2/3); no-op once
 * present.  Freed together with the composite pair in destroy_fbos(). */
static void ensure_bloom_fbos(JcePostFXPipeline *p)
{
    if (p->fbo[2].idx != UINT16_MAX) return;
    alloc_one_fbo(p, 2);
    alloc_one_fbo(p, 3);
}

static void destroy_fbos(JcePostFXPipeline *p)
{
    if (!p->fbos_valid) return;
    for (int i = 0; i < POSTFX_MAX_FBOS; i++) {
        if (p->fbo[i].idx != UINT16_MAX)
            bgfx_destroy_frame_buffer(p->fbo[i]);
        p->fbo[i].idx = UINT16_MAX;
        /* Textures are owned by the FBs (destroyTextures=true). */
        p->fbo_tex[i].idx = UINT16_MAX;
    }
    p->fbos_valid = false;
}

/* ── Full-screen quad draw ─────────────────────────────────────────── */

static void draw_fullscreen(JcePostFXPipeline *p, uint16_t view_id,
                            bgfx_program_handle_t prog)
{
    bgfx_set_vertex_buffer(0, p->quad_vb, 0, 4);
    bgfx_set_index_buffer(p->quad_ib, 0, 6);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A, 0);
    bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
}

/* ── Create / Destroy ──────────────────────────────────────────────── */

JcePostFXPipeline *jce_postfx_create(jce_allocator_t alloc,
                                     uint32_t width, uint32_t height)
{
    JcePostFXPipeline *p = (JcePostFXPipeline *)alloc.alloc(
        sizeof(JcePostFXPipeline), alloc.ctx);
    if (!p) return NULL;

    memset(p, 0, sizeof(*p));
    p->alloc  = alloc;
    p->width  = width;
    p->height = height;
    p->params = jce_postfx_default_params();
    p->shaders_loaded = false;
    p->fbos_valid = false;

    /* Initialize handles to invalid. */
    p->quad_vb.idx = UINT16_MAX;
    p->quad_ib.idx = UINT16_MAX;
    for (int i = 0; i < POSTFX_MAX_FBOS; i++) {
        p->fbo[i].idx     = UINT16_MAX;
        p->fbo_tex[i].idx = UINT16_MAX;
    }
    p->prog_tonemap.idx       = UINT16_MAX;
    p->prog_bloom_extract.idx = UINT16_MAX;
    p->prog_bloom_blur.idx    = UINT16_MAX;
    p->prog_bloom_combine.idx = UINT16_MAX;
    p->prog_fxaa.idx          = UINT16_MAX;
    p->prog_vignette.idx      = UINT16_MAX;
    p->prog_chromatic.idx     = UINT16_MAX;
    p->prog_grayscale.idx     = UINT16_MAX;
    p->prog_composite.idx     = UINT16_MAX;
    p->prog_present.idx       = UINT16_MAX;
    p->prog_custom.idx        = UINT16_MAX;
    p->shader_pak             = NULL;
    p->custom_name[0]         = '\0';
    p->custom_loaded[0]       = '\0';
    p->custom_needs_depth     = false;
    p->custom_param_count     = 0;
    reset_output_state(p);
    p->view_base = JCE_VIEW_POST_BASE;

    /* Create full-screen quad geometry. */
    bgfx_vertex_layout_begin(&p->quad_layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&p->quad_layout, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&p->quad_layout, BGFX_ATTRIB_TEXCOORD0, 2,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&p->quad_layout);

    const bgfx_memory_t *vb_mem = bgfx_copy(s_quad_verts, sizeof(s_quad_verts));
    p->quad_vb = bgfx_create_vertex_buffer(vb_mem, &p->quad_layout, BGFX_BUFFER_NONE);

    const bgfx_memory_t *ib_mem = bgfx_copy(s_quad_indices, sizeof(s_quad_indices));
    p->quad_ib = bgfx_create_index_buffer(ib_mem, BGFX_BUFFER_NONE);

    /* Create uniforms. */
    p->u_texColor       = bgfx_create_uniform("s_texColor",       BGFX_UNIFORM_TYPE_SAMPLER, 1);
    p->u_texBloom       = bgfx_create_uniform("s_texBloom",       BGFX_UNIFORM_TYPE_SAMPLER, 1);
    p->u_tonemapParams  = bgfx_create_uniform("u_tonemapParams",  BGFX_UNIFORM_TYPE_VEC4, 1);
    p->u_bloomParams    = bgfx_create_uniform("u_bloomParams",    BGFX_UNIFORM_TYPE_VEC4, 1);
    p->u_blurDir        = bgfx_create_uniform("u_blurDir",        BGFX_UNIFORM_TYPE_VEC4, 1);
    p->u_fxaaParams     = bgfx_create_uniform("u_fxaaParams",     BGFX_UNIFORM_TYPE_VEC4, 1);
    p->u_texelSize      = bgfx_create_uniform("u_texelSize",      BGFX_UNIFORM_TYPE_VEC4, 1);
    p->u_vignetteParams = bgfx_create_uniform("u_vignetteParams", BGFX_UNIFORM_TYPE_VEC4, 1);
    p->u_chromaticParams= bgfx_create_uniform("u_chromaticParams",BGFX_UNIFORM_TYPE_VEC4, 1);
    p->u_compositeFlags = bgfx_create_uniform("u_compositeFlags", BGFX_UNIFORM_TYPE_VEC4, 1);
    p->u_compositeFlags2= bgfx_create_uniform("u_compositeFlags2",BGFX_UNIFORM_TYPE_VEC4, 1);
    p->u_texDepth       = bgfx_create_uniform("s_texDepth",       BGFX_UNIFORM_TYPE_SAMPLER, 1);
    p->u_postfxTime     = bgfx_create_uniform("u_postfxTime",     BGFX_UNIFORM_TYPE_VEC4, 1);
    p->u_postfxParams   = bgfx_create_uniform("u_postfxParams",   BGFX_UNIFORM_TYPE_VEC4,
                                              JCE_POSTFX_CUSTOM_PARAMS);

    LOG_SUCCESS(LOG_TAG, "post-fx pipeline created (%ux%u)", width, height);
    return p;
}

void jce_postfx_destroy(JcePostFXPipeline *pipeline)
{
    if (!pipeline) return;

    destroy_fbos(pipeline);

    if (pipeline->quad_vb.idx != UINT16_MAX)
        bgfx_destroy_vertex_buffer(pipeline->quad_vb);
    if (pipeline->quad_ib.idx != UINT16_MAX)
        bgfx_destroy_index_buffer(pipeline->quad_ib);

    /* Destroy uniforms. */
    bgfx_destroy_uniform(pipeline->u_texColor);
    bgfx_destroy_uniform(pipeline->u_texBloom);
    bgfx_destroy_uniform(pipeline->u_tonemapParams);
    bgfx_destroy_uniform(pipeline->u_bloomParams);
    bgfx_destroy_uniform(pipeline->u_blurDir);
    bgfx_destroy_uniform(pipeline->u_fxaaParams);
    bgfx_destroy_uniform(pipeline->u_texelSize);
    bgfx_destroy_uniform(pipeline->u_vignetteParams);
    bgfx_destroy_uniform(pipeline->u_chromaticParams);
    bgfx_destroy_uniform(pipeline->u_compositeFlags);
    bgfx_destroy_uniform(pipeline->u_compositeFlags2);
    bgfx_destroy_uniform(pipeline->u_texDepth);
    bgfx_destroy_uniform(pipeline->u_postfxTime);
    bgfx_destroy_uniform(pipeline->u_postfxParams);

    /* Destroy shader programs. */
    if (pipeline->prog_tonemap.idx       != UINT16_MAX) bgfx_destroy_program(pipeline->prog_tonemap);
    if (pipeline->prog_bloom_extract.idx != UINT16_MAX) bgfx_destroy_program(pipeline->prog_bloom_extract);
    if (pipeline->prog_bloom_blur.idx    != UINT16_MAX) bgfx_destroy_program(pipeline->prog_bloom_blur);
    if (pipeline->prog_bloom_combine.idx != UINT16_MAX) bgfx_destroy_program(pipeline->prog_bloom_combine);
    if (pipeline->prog_fxaa.idx          != UINT16_MAX) bgfx_destroy_program(pipeline->prog_fxaa);
    if (pipeline->prog_vignette.idx      != UINT16_MAX) bgfx_destroy_program(pipeline->prog_vignette);
    if (pipeline->prog_chromatic.idx     != UINT16_MAX) bgfx_destroy_program(pipeline->prog_chromatic);
    if (pipeline->prog_grayscale.idx     != UINT16_MAX) bgfx_destroy_program(pipeline->prog_grayscale);
    if (pipeline->prog_composite.idx     != UINT16_MAX) bgfx_destroy_program(pipeline->prog_composite);
    if (pipeline->prog_present.idx       != UINT16_MAX) bgfx_destroy_program(pipeline->prog_present);
    if (pipeline->prog_custom.idx        != UINT16_MAX) bgfx_destroy_program(pipeline->prog_custom);

    jce_allocator_t a = pipeline->alloc;
    a.free(pipeline, a.ctx);
}

/* ── Resize ────────────────────────────────────────────────────────── */

void jce_postfx_resize(JcePostFXPipeline *pipeline,
                       uint32_t width, uint32_t height)
{
    if (!pipeline) return;
    if (pipeline->width == width && pipeline->height == height) return;

    pipeline->width  = width;
    pipeline->height = height;

    /* Recreate FBOs at new resolution. */
    destroy_fbos(pipeline);
    reset_output_state(pipeline);

    LOG_DEBUG(LOG_TAG, "post-fx resized to %ux%u", width, height);
}

/* ── Effect chain ──────────────────────────────────────────────────── */

void jce_postfx_enable(JcePostFXPipeline *pipeline, JcePostFXType type,
                       bool enabled)
{
    if (!pipeline || type >= JCE_POSTFX_COUNT) return;
    pipeline->enabled[type] = enabled;
}

bool jce_postfx_is_enabled(const JcePostFXPipeline *pipeline, JcePostFXType type)
{
    if (!pipeline || type >= JCE_POSTFX_COUNT) return false;
    return pipeline->enabled[type];
}

void jce_postfx_set_params(JcePostFXPipeline *pipeline,
                           const JcePostFXParams *params)
{
    if (!pipeline || !params) return;
    pipeline->params = *params;
}

void jce_postfx_get_params(const JcePostFXPipeline *pipeline,
                           JcePostFXParams *out)
{
    if (!pipeline || !out) return;
    *out = pipeline->params;
}

/* ── Custom (client) pass ──────────────────────────────────────────── */

void jce_postfx_set_custom_shader(JcePostFXPipeline *pipeline,
                                  const char *fs_name, bool needs_depth)
{
    if (!pipeline) return;
    pipeline->custom_needs_depth = needs_depth;
    size_t i = 0;
    if (fs_name) {
        for (; fs_name[i] && i + 1 < sizeof(pipeline->custom_name); i++)
            pipeline->custom_name[i] = fs_name[i];
    }
    pipeline->custom_name[i] = '\0';
}

void jce_postfx_set_custom_params(JcePostFXPipeline *pipeline,
                                  const float *vec4s, int count)
{
    if (!pipeline) return;
    if (count < 0) count = 0;
    if (count > JCE_POSTFX_CUSTOM_PARAMS) count = JCE_POSTFX_CUSTOM_PARAMS;
    pipeline->custom_param_count = count;
    if (vec4s && count > 0)
        memcpy(pipeline->custom_params, vec4s,
               (size_t)count * 4u * sizeof(float));
}

void jce_postfx_get_custom_shader(const JcePostFXPipeline *pipeline,
                                  char *out_name, int out_size,
                                  bool *out_needs_depth)
{
    if (out_needs_depth)
        *out_needs_depth = pipeline ? pipeline->custom_needs_depth : false;
    if (out_name && out_size > 0) {
        int i = 0;
        if (pipeline) {
            for (; pipeline->custom_name[i] && i + 1 < out_size; i++)
                out_name[i] = pipeline->custom_name[i];
        }
        out_name[i] = '\0';
    }
}

int jce_postfx_get_custom_params(const JcePostFXPipeline *pipeline,
                                 float *out_vec4s, int max_count)
{
    if (!pipeline) return 0;
    int count = pipeline->custom_param_count;
    if (max_count < count) count = max_count;
    if (out_vec4s && count > 0)
        memcpy(out_vec4s, pipeline->custom_params,
               (size_t)count * 4u * sizeof(float));
    return count;
}

/* ── Load shaders ──────────────────────────────────────────────────── */

/* Helper: load a postfx program (vs_postfx + fs_<effect>) and log on failure. */
static bgfx_program_handle_t load_postfx_prog(const JcePakArchive *pak,
                                              const char *fs_name)
{
    bgfx_program_handle_t invalid;
    invalid.idx = UINT16_MAX;
    JceShaderHandle h = shader_load_program_named(pak, "postfx", fs_name);
    if (h.idx == UINT16_MAX) {
        LOG_WARN(LOG_TAG, "post-fx shader '%s' not found in PAK", fs_name);
        return invalid;
    }
    bgfx_program_handle_t prog;
    prog.idx = h.idx;
    return prog;
}

bool jce_postfx_load_shaders(JcePostFXPipeline *pipeline,
                             const JcePakArchive *pak)
{
    if (!pipeline) return false;
    if (!pak) {
        LOG_ERROR(LOG_TAG, "cannot load post-fx shaders: NULL PAK");
        return false;
    }

    /* Retain the PAK so the custom pass can lazily resolve a client-named
     * shader at apply() time. The shader PAK lives for the app's lifetime. */
    pipeline->shader_pak = pak;

    pipeline->prog_tonemap       = load_postfx_prog(pak, "tonemap");
    pipeline->prog_bloom_extract = load_postfx_prog(pak, "bloom_extract");
    pipeline->prog_bloom_blur    = load_postfx_prog(pak, "bloom_blur");
    pipeline->prog_bloom_combine = load_postfx_prog(pak, "bloom_combine");
    pipeline->prog_fxaa          = load_postfx_prog(pak, "fxaa");
    pipeline->prog_vignette      = load_postfx_prog(pak, "vignette");
    pipeline->prog_chromatic     = load_postfx_prog(pak, "chromatic");
    pipeline->prog_grayscale     = load_postfx_prog(pak, "grayscale");
    pipeline->prog_composite     = load_postfx_prog(pak, "composite");
    pipeline->prog_present       = load_postfx_prog(pak, "present");

    /* Count how many loaded successfully. */
    int loaded = 0;
    if (pipeline->prog_tonemap.idx       != UINT16_MAX) loaded++;
    if (pipeline->prog_bloom_extract.idx != UINT16_MAX) loaded++;
    if (pipeline->prog_bloom_blur.idx    != UINT16_MAX) loaded++;
    if (pipeline->prog_bloom_combine.idx != UINT16_MAX) loaded++;
    if (pipeline->prog_fxaa.idx          != UINT16_MAX) loaded++;
    if (pipeline->prog_vignette.idx      != UINT16_MAX) loaded++;
    if (pipeline->prog_chromatic.idx     != UINT16_MAX) loaded++;
    if (pipeline->prog_grayscale.idx     != UINT16_MAX) loaded++;
    if (pipeline->prog_composite.idx     != UINT16_MAX) loaded++;

    pipeline->shaders_loaded = (loaded > 0);
    LOG_INFO(LOG_TAG, "post-fx shaders loaded: %d/9", loaded);
    return pipeline->shaders_loaded;
}

/* ── View base ─────────────────────────────────────────────────────── */

void jce_postfx_set_view_base(JcePostFXPipeline *pipeline, uint16_t base)
{
    if (pipeline) pipeline->view_base = base;
}

/* ── Apply ─────────────────────────────────────────────────────────── */

void jce_postfx_apply(JcePostFXPipeline *pipeline,
                      JceTextureHandle scene_color,
                      JceTextureHandle scene_depth)
{
    if (!pipeline) return;

    reset_output_state(pipeline);

    if (!pipeline->shaders_loaded) return;

    if (!jce_gfx_texture_valid(scene_color)) {
        LOG_WARN(LOG_TAG, "post-fx skipped: invalid scene color texture");
        return;
    }

    /* Count active effects. */
    int active = 0;
    for (int i = 0; i < JCE_POSTFX_COUNT; i++) {
        if (pipeline->enabled[i]) active++;
    }
    if (active == 0) return;

    /* Ensure FBOs are created.  Composite ping-pong pair (0/1) always; the
       bloom buffers (2/3) only when bloom is enabled this frame — standard
       "allocate only what the enabled chain needs" (saves ~33MB at 1080p on
       the bloom-off LOW/MEDIUM-tier path). */
    if (!pipeline->fbos_valid)
        create_fbos(pipeline);
    if (!pipeline->fbos_valid) return;
    if (pipeline->enabled[JCE_POSTFX_BLOOM])
        ensure_bloom_fbos(pipeline);

    JCE_PROFILE_ZONE_N("PostFX::Apply");

    /* Texel size uniform (shared by several effects). */
    float texel_size[4] = {
        1.0f / (float)pipeline->width,
        1.0f / (float)pipeline->height,
        (float)pipeline->width,
        (float)pipeline->height
    };
    bgfx_set_uniform(pipeline->u_texelSize, texel_size, 1);

    /* Track current input texture. Start with the scene color. */
    bgfx_texture_handle_t current_tex = { scene_color.idx };
    uint16_t view_id = pipeline->view_base;
    int ping = 0; /* ping-pong FBO index (0 or 1) */
    int current_fb_index = -1;

/* Helper macro: set up view for a post-processing pass.
 *
 * Always issues a hardware clear before the fullscreen quad. The quad
 * does fully overwrite RGB+A across the view rect, but on the very
 * first frame after create_fbos() the underlying GPU texture contains
 * uninitialised VRAM (visible as rainbow noise) and any tile/region
 * the fullscreen pass doesn't perfectly touch (driver edge cases on
 * Vulkan/D3D12 after handle recycling) leaks through. The clear is a
 * fast tile-init on modern GPUs and removes the resize artifact. */
#define POSTFX_SETUP_VIEW(vid, fb)                                                                 \
    do {                                                                                           \
        bgfx_set_view_rect((vid), 0, 0, (uint16_t)pipeline->width, (uint16_t)pipeline->height);    \
        bgfx_set_view_frame_buffer((vid), (fb));                                                   \
        bgfx_set_view_clear((vid), BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);                         \
    } while (0)

#define POSTFX_LABEL(vid, name) bgfx_set_view_name((vid), (name), INT32_MAX)

    /* ── 1. Bloom ─────────────────────────────────────────────────── */
    if (pipeline->enabled[JCE_POSTFX_BLOOM] &&
        pipeline->prog_bloom_extract.idx != UINT16_MAX)
    {
        /* Bloom extract pass → FBO 2. */
        float bloom_params[4] = {
            pipeline->params.bloom_threshold,
            pipeline->params.bloom_intensity,
            0.0f, 0.0f
        };
        bgfx_set_uniform(pipeline->u_bloomParams, bloom_params, 1);

        POSTFX_SETUP_VIEW(view_id, pipeline->fbo[2]);
        bgfx_set_view_name(view_id, "PostFX/BloomExtract", INT32_MAX);
        bgfx_set_texture(0, pipeline->u_texColor, current_tex, UINT32_MAX);
        draw_fullscreen(pipeline, view_id, pipeline->prog_bloom_extract);
        view_id++;

        /* Horizontal blur → FBO 3. */
        if (pipeline->prog_bloom_blur.idx != UINT16_MAX) {
            float blur_h[4] = { texel_size[0], 0.0f, 0.0f, 0.0f };
            bgfx_set_uniform(pipeline->u_blurDir, blur_h, 1);
            POSTFX_SETUP_VIEW(view_id, pipeline->fbo[3]);
            bgfx_set_view_name(view_id, "PostFX/BloomBlurH", INT32_MAX);
            bgfx_set_texture(0, pipeline->u_texColor, pipeline->fbo_tex[2], UINT32_MAX);
            draw_fullscreen(pipeline, view_id, pipeline->prog_bloom_blur);
            view_id++;

            /* Vertical blur → FBO 2. */
            float blur_v[4] = { 0.0f, texel_size[1], 0.0f, 0.0f };
            bgfx_set_uniform(pipeline->u_blurDir, blur_v, 1);
            POSTFX_SETUP_VIEW(view_id, pipeline->fbo[2]);
            bgfx_set_view_name(view_id, "PostFX/BloomBlurV", INT32_MAX);
            bgfx_set_texture(0, pipeline->u_texColor, pipeline->fbo_tex[3], UINT32_MAX);
            draw_fullscreen(pipeline, view_id, pipeline->prog_bloom_blur);
            view_id++;
        }

        /* Bloom is COMBINED in the uber composite pass below (fbo_tex[2]
           holds the blurred bloom; the composite reads it at stage 1). */
    }

    /* ── 2. Uber composite ────────────────────────────────────────────
     * Folds bloom-combine + tonemap + chromatic + vignette + grayscale into
     * ONE fullscreen pass (each gated by a flag). Runs whenever any of those
     * effects is on. FXAA (below) stays separate — it needs the LDR result. */
    {
        const bool c_bloom   = pipeline->enabled[JCE_POSTFX_BLOOM];
        const bool c_tonemap = pipeline->enabled[JCE_POSTFX_TONEMAP];
        const bool c_chroma  = pipeline->enabled[JCE_POSTFX_CHROMATIC];
        const bool c_vig     = pipeline->enabled[JCE_POSTFX_VIGNETTE];
        const bool c_gray    = pipeline->enabled[JCE_POSTFX_GRAYSCALE];
        const bool any_comp  = c_bloom || c_tonemap || c_chroma || c_vig || c_gray;

        if (any_comp && pipeline->prog_composite.idx != UINT16_MAX) {
            float flags[4]   = { c_bloom ? 1.0f : 0.0f, c_tonemap ? 1.0f : 0.0f,
                                 c_chroma ? 1.0f : 0.0f, c_vig ? 1.0f : 0.0f };
            float flags2[4]  = { c_gray ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
            float bloom_p[4] = { pipeline->params.bloom_threshold,
                                 pipeline->params.bloom_intensity, 0.0f, 0.0f };
            float tonemap_p[4] = { pipeline->params.exposure,
                                   pipeline->params.gamma, 0.0f, 0.0f };
            float chrom_p[4] = { pipeline->params.chromatic_strength, 0.0f, 0.0f, 0.0f };
            float vig_p[4]   = { pipeline->params.vignette_intensity,
                                 pipeline->params.vignette_smoothness, 0.0f, 0.0f };
            bgfx_set_uniform(pipeline->u_compositeFlags,  flags,  1);
            bgfx_set_uniform(pipeline->u_compositeFlags2, flags2, 1);
            bgfx_set_uniform(pipeline->u_bloomParams,     bloom_p, 1);
            bgfx_set_uniform(pipeline->u_tonemapParams,   tonemap_p, 1);
            bgfx_set_uniform(pipeline->u_chromaticParams, chrom_p, 1);
            bgfx_set_uniform(pipeline->u_vignetteParams,  vig_p, 1);

            ensure_composite_fbo(pipeline, ping);
            POSTFX_SETUP_VIEW(view_id, pipeline->fbo[ping]);
            bgfx_set_view_name(view_id, "PostFX/Composite", INT32_MAX);
            bgfx_set_texture(0, pipeline->u_texColor, current_tex, UINT32_MAX);
            /* stage 1 = blurred bloom (fbo_tex[2]) when bloom is on; otherwise
               bind a valid handle that the shader's flag-gate won't sample. */
            bgfx_set_texture(1, pipeline->u_texBloom,
                             c_bloom ? pipeline->fbo_tex[2] : current_tex, UINT32_MAX);
            draw_fullscreen(pipeline, view_id, pipeline->prog_composite);
            current_tex = pipeline->fbo_tex[ping];
            current_fb_index = ping;
            ping = 1 - ping;
            view_id++;
        }
    }

    /* ── 3. FXAA (separate — edge AA on the composited LDR image) ────── */
    if (pipeline->enabled[JCE_POSTFX_FXAA] &&
        pipeline->prog_fxaa.idx != UINT16_MAX)
    {
        float fxaa_p[4] = {
            pipeline->params.fxaa_span_max,
            pipeline->params.fxaa_reduce_min,
            pipeline->params.fxaa_reduce_mul,
            0.0f
        };
        bgfx_set_uniform(pipeline->u_fxaaParams, fxaa_p, 1);

        ensure_composite_fbo(pipeline, ping);
        POSTFX_SETUP_VIEW(view_id, pipeline->fbo[ping]);
        bgfx_set_view_name(view_id, "PostFX/FXAA", INT32_MAX);
        bgfx_set_texture(0, pipeline->u_texColor, current_tex, UINT32_MAX);
        draw_fullscreen(pipeline, view_id, pipeline->prog_fxaa);
        current_tex = pipeline->fbo_tex[ping];
        current_fb_index = ping;
        ping = 1 - ping;
        view_id++;
    }

    /* ── 4. Custom client pass (data-driven, runs last) ───────────────
     * The engine is style-agnostic: it lazily loads the client-named shader,
     * binds the generic contract (colour + optional depth + texel + time +
     * param array) and submits one full-screen pass. */
    if (pipeline->enabled[JCE_POSTFX_CUSTOM] && pipeline->custom_name[0] != '\0') {
        /* (Re)load lazily when the requested shader name changes. */
        if (strcmp(pipeline->custom_loaded, pipeline->custom_name) != 0) {
            if (pipeline->prog_custom.idx != UINT16_MAX) {
                bgfx_destroy_program(pipeline->prog_custom);
                pipeline->prog_custom.idx = UINT16_MAX;
            }
            if (pipeline->shader_pak)
                pipeline->prog_custom =
                    load_postfx_prog(pipeline->shader_pak, pipeline->custom_name);
            /* Remember the attempt (even on failure) so we don't retry every frame. */
            size_t ci = 0;
            for (; pipeline->custom_name[ci] &&
                   ci + 1 < sizeof(pipeline->custom_loaded); ci++)
                pipeline->custom_loaded[ci] = pipeline->custom_name[ci];
            pipeline->custom_loaded[ci] = '\0';
            LOG_INFO(LOG_TAG, "custom pass shader '%s' %s", pipeline->custom_name,
                     (pipeline->prog_custom.idx != UINT16_MAX) ? "loaded" : "NOT FOUND");
        }

        if (pipeline->prog_custom.idx != UINT16_MAX) {
            const bool has_depth =
                pipeline->custom_needs_depth && jce_gfx_texture_valid(scene_depth);

            float elapsed_s =
                (float)((double)(jce_time_ticks_ms() % 100000000ull) * 0.001);
            float tparams[4] = { elapsed_s, has_depth ? 1.0f : 0.0f, 0.0f, 0.0f };
            bgfx_set_uniform(pipeline->u_postfxTime, tparams, 1);
            bgfx_set_uniform(pipeline->u_postfxParams, pipeline->custom_params,
                             JCE_POSTFX_CUSTOM_PARAMS);

            ensure_composite_fbo(pipeline, ping);
            POSTFX_SETUP_VIEW(view_id, pipeline->fbo[ping]);
            bgfx_set_view_name(view_id, "PostFX/Custom", INT32_MAX);
            bgfx_set_texture(0, pipeline->u_texColor, current_tex, UINT32_MAX);
            /* Stage 1: real depth when requested+valid, else a harmless dummy
             * (the shader gates on u_postfxTime.y so it won't sample it). */
            bgfx_texture_handle_t depth_tex = {
                has_depth ? scene_depth.idx : current_tex.idx
            };
            bgfx_set_texture(1, pipeline->u_texDepth, depth_tex, UINT32_MAX);
            draw_fullscreen(pipeline, view_id, pipeline->prog_custom);
            current_tex = pipeline->fbo_tex[ping];
            current_fb_index = ping;
            ping = 1 - ping;
            view_id++;
        }
    }

    #undef POSTFX_SETUP_VIEW
    #undef POSTFX_LABEL

    /* Store the final output texture for the caller. */
    pipeline->output_tex = current_tex;
    if (current_fb_index >= 0) {
        pipeline->output_fb = pipeline->fbo[current_fb_index];
        pipeline->output_ping = current_fb_index;
    }

    LOG_TRACE(LOG_TAG, "post-fx apply: %d effects active, %d views used",
              active, view_id - JCE_VIEW_POST_BASE);
    JCE_PROFILE_ZONE_END;
}

JceTextureHandle jce_postfx_get_output(const JcePostFXPipeline *pipeline)
{
    JceTextureHandle h = { UINT16_MAX };
    if (pipeline)
        h.idx = pipeline->output_tex.idx;
    return h;
}

void jce_postfx_present(JcePostFXPipeline *pipeline,
                        uint32_t width, uint32_t height)
{
    bgfx_frame_buffer_handle_t backbuffer = { UINT16_MAX };
    uint16_t view_id;
    if (!pipeline || pipeline->prog_present.idx == UINT16_MAX)
        return;
    if (pipeline->output_tex.idx == UINT16_MAX)
        return;
    /* One view past the chain's worst case (bloom 3 + composite + fxaa +
     * custom = 6 views from view_base) so submission order is preserved. */
    view_id = (uint16_t)(pipeline->view_base + 8);
    bgfx_set_view_rect(view_id, 0, 0, (uint16_t)width, (uint16_t)height);
    bgfx_set_view_frame_buffer(view_id, backbuffer);
    bgfx_set_view_clear(view_id, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    bgfx_set_view_name(view_id, "PostFX/Present", INT32_MAX);
    bgfx_set_texture(0, pipeline->u_texColor, pipeline->output_tex,
                     UINT32_MAX);
    draw_fullscreen(pipeline, view_id, pipeline->prog_present);
}

uint16_t jce_postfx_get_output_framebuffer(const JcePostFXPipeline *pipeline)
{
    if (!pipeline) return UINT16_MAX;
    return pipeline->output_fb.idx;
}
