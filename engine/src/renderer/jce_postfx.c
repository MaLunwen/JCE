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
#include <stdio.h>
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

    /* Shader programs.
     * NOTE: prog_tonemap was removed — fs_composite.sc now handles all
     * tonemapping; the standalone fs_tonemap program is no longer submitted. */
    bgfx_program_handle_t prog_bloom_extract;
    bgfx_program_handle_t prog_bloom_blur;
    bgfx_program_handle_t prog_bloom_combine;
    bgfx_program_handle_t prog_fxaa;
    bgfx_program_handle_t prog_vignette;
    bgfx_program_handle_t prog_chromatic;
    bgfx_program_handle_t prog_grayscale;
    bgfx_program_handle_t prog_composite;  /* uber: combine+tonemap+chromatic+vignette+grayscale */
    bgfx_program_handle_t prog_present;    /* pass-through: output -> backbuffer (runtime path) */
    bgfx_program_handle_t prog_motion_vec; /* TAA: depth -> NDC motion delta (RG) */
    bgfx_program_handle_t prog_taa;        /* TAA: resolve current + history -> output */

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

    /* TAA uniforms + samplers. */
    bgfx_uniform_handle_t u_taaParams;        /* (feedback, luma_clamp, motion_clamp, 0) */
    bgfx_uniform_handle_t u_taaInvViewProj;   /* inverse UN-jittered scene view*proj (s_texDepth recon) */
    bgfx_uniform_handle_t u_taaPrevViewProj;  /* previous frame UN-jittered view*proj */
    bgfx_uniform_handle_t u_texHistory;       /* TAA history sampler (stage 1, resolve) */
    bgfx_uniform_handle_t u_texMotion;        /* TAA motion sampler  (stage 2, resolve) */

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

    /* ── TAA state (opt-in; nothing allocated until first enabled apply) ──
     * The history buffer is PERSISTENT across frames (it must survive the
     * ping-pong chain) and full-res HDR (RGBA16F) to match the scene colour.
     * The motion buffer is regenerated every TAA frame.  Both are sized to
     * the apply target and reallocated on resize (history_valid resets then,
     * so the first post-resize resolve treats history as empty → ~current). */
    bool                       taa_enabled;       /* requested by the renderer */
    float                      taa_params[4];     /* feedback, luma_clamp, motion_clamp, 0 */
    bool                       taa_have_matrices; /* matrices pushed this frame */
    float                      taa_inv_view_proj[16];
    float                      taa_prev_view_proj[16];
    bgfx_texture_handle_t      taa_history_tex;
    bgfx_frame_buffer_handle_t taa_history_fb;
    bgfx_texture_handle_t      taa_motion_tex;
    bgfx_frame_buffer_handle_t taa_motion_fb;
    bool                       taa_fbos_valid;    /* history/motion allocated */
    bool                       history_valid;     /* a resolve has written history */
    /* EXTERNAL per-object motion-vector texture supplied by the renderer for the
     * frame about to be resolved (the scene renderer's velocity G-buffer).  When
     * valid the TAA pass SKIPS its internal camera-only motion-vec pass and binds
     * THIS texture as s_texMotion instead — so animated/skinned geometry stops
     * ghosting.  Reset to invalid every apply() (one-shot per frame). */
    bgfx_texture_handle_t      taa_ext_motion_tex;

    /* Selectable tonemap + 3D-LUT grade + soft bloom (Stage 1a.5). */
    int                    tonemap_op;     /* JcePostFXTonemap; 0=ACES default */
    bgfx_texture_handle_t  lut_tex;        /* 3D LUT (invalid = no grade) */
    bgfx_texture_handle_t  dummy_lut3d;    /* 1x1x1 placeholder: parks the
                                            * SAMPLER3D when no LUT is loaded
                                            * (WebGL2 rejects dangling samplers) */
    int                    lut_size;       /* N */
    float                  lut_strength;   /* 0 = neutral */
    float                  bloom_knee;     /* 0 = hard cutoff (legacy) */
    int                    bloom_quality;  /* 0 = single-mip; >0 = pyramid mips */
    bgfx_uniform_handle_t  u_gradeParams;  /* x=enabled y=strength z=N w=0 */
    bgfx_uniform_handle_t  s_texLUT;       /* SAMPLER (stage 2) */
    bgfx_program_handle_t  prog_bloom_down;
    bgfx_program_handle_t  prog_bloom_up;
    /* Bloom mip pyramid (HIGH/ULTRA). Up to 6 half-res-chain mips. */
    #define POSTFX_BLOOM_MAX_MIPS 6
    bgfx_texture_handle_t      bloom_mip_tex[POSTFX_BLOOM_MAX_MIPS];
    bgfx_frame_buffer_handle_t bloom_mip_fb[POSTFX_BLOOM_MAX_MIPS];
    int                        bloom_mip_count;   /* allocated */
    uint32_t                   bloom_mip_w[POSTFX_BLOOM_MAX_MIPS];
    uint32_t                   bloom_mip_h[POSTFX_BLOOM_MAX_MIPS];
    bool                       bloom_mips_valid;
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

/* Color format for every post-FX intermediate target (ping-pong chain, bloom
 * pyramid, TAA history/motion).  RGBA16F keeps the HDR range through the
 * chain, but not every backend can render to it (ES2/WebGL1-class devices,
 * some ANGLE configs lack the FRAMEBUFFER cap bit) — creating the FBO anyway
 * makes bgfx fail the frame-buffer, and the whole post chain silently goes
 * black on just those backends.  Fall back to RGBA8 like the editor's
 * offscreen bridge does (jce_offscreen_target.c): bloom/TAA still run,
 * merely LDR-clamped (bloom extraction over threshold 1.0 mostly no-ops). */
static bgfx_texture_format_t postfx_color_format(void)
{
    static bgfx_texture_format_t s_fmt = BGFX_TEXTURE_FORMAT_COUNT; /* unresolved */
    if (s_fmt == BGFX_TEXTURE_FORMAT_COUNT) {
        const bgfx_caps_t *caps = bgfx_get_caps();
        if (!caps)
            return BGFX_TEXTURE_FORMAT_RGBA16F; /* pre-init probe: don't cache */
        if ((caps->formats[BGFX_TEXTURE_FORMAT_RGBA16F]
             & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER) == 0) {
            s_fmt = BGFX_TEXTURE_FORMAT_RGBA8;
            LOG_WARN(LOG_TAG, "RGBA16F render target unsupported; post-FX "
                              "chain falls back to RGBA8 (LDR bloom/TAA)");
        } else {
            s_fmt = BGFX_TEXTURE_FORMAT_RGBA16F;
        }
    }
    return s_fmt;
}

/* Allocate one full-res RGBA16F intermediate FBO (color texture + framebuffer).
 * destroyTextures=true so destroying the FB also frees the attached texture —
 * otherwise destroy_fbos() leaks handles, which during ImGui drag-resize
 * exhausts bgfx's texture pool and yields recycled-handle AVs in the driver. */
static void alloc_one_fbo(JcePostFXPipeline *p, int i)
{
    p->fbo_tex[i] = bgfx_create_texture_2d(
        (uint16_t)p->width, (uint16_t)p->height, false, 1,
        postfx_color_format(),
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

/* Lazily allocate the bloom-input/accumulation buffer (FBO 2); no-op once
 * present.  FBO 3 (the legacy single-mip horizontal-blur scratch) is allocated
 * separately by ensure_bloom_blur_h_fbo() ONLY on the legacy bloom path — the
 * HIGH/ULTRA dual-filter pyramid never touches it, so it stays unallocated
 * there (saves a full-res RGBA16F, ~16.6 MB @1080p).  Freed together with the
 * composite pair in destroy_fbos(). */
static void ensure_bloom_fbos(JcePostFXPipeline *p)
{
    if (p->fbo[2].idx != UINT16_MAX) return;
    alloc_one_fbo(p, 2);
}

/* Lazily allocate FBO 3 (legacy bloom horizontal-blur scratch); no-op once
 * present.  Returns true when fbo[3] is valid for use this frame. */
static bool ensure_bloom_blur_h_fbo(JcePostFXPipeline *p)
{
    if (p->fbo[3].idx == UINT16_MAX) alloc_one_fbo(p, 3);
    return p->fbo[3].idx != UINT16_MAX;
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

/* ── Bloom mip pyramid FBO helpers ──────────────────────────────────── */

static void destroy_bloom_mips(JcePostFXPipeline *p)
{
    if (!p->bloom_mips_valid) return;
    for (int i = 0; i < POSTFX_BLOOM_MAX_MIPS; i++) {
        if (p->bloom_mip_fb[i].idx != UINT16_MAX)
            bgfx_destroy_frame_buffer(p->bloom_mip_fb[i]);  /* destroyTextures=true */
        p->bloom_mip_fb[i].idx  = UINT16_MAX;
        p->bloom_mip_tex[i].idx = UINT16_MAX;  /* owned by the FB */
    }
    p->bloom_mip_count = 0;
    p->bloom_mips_valid = false;
}

/* Lazily allocate (or reallocate if count changed) the bloom mip pyramid
 * FBOs.  Each mip is half the resolution of the previous level, so mip 0
 * is width/2 x height/2, mip 1 is width/4 x height/4, etc.
 * Uses RGBA16F to keep the full HDR range through the pyramid.
 * destroyTextures=true so the texture is freed with the frame buffer. */
static void ensure_bloom_mips(JcePostFXPipeline *p, int mip_count)
{
    if (mip_count < 0) mip_count = 0;
    if (mip_count > POSTFX_BLOOM_MAX_MIPS) mip_count = POSTFX_BLOOM_MAX_MIPS;

    /* If already allocated with the same count and valid, nothing to do. */
    if (p->bloom_mips_valid && p->bloom_mip_count == mip_count) return;

    /* Free existing pyramid (resize or count change). */
    destroy_bloom_mips(p);
    if (mip_count == 0) return;

    for (int i = 0; i < mip_count; i++) {
        uint32_t w = p->width  >> (i + 1);
        uint32_t h = p->height >> (i + 1);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        p->bloom_mip_w[i] = w;
        p->bloom_mip_h[i] = h;

        p->bloom_mip_tex[i] = bgfx_create_texture_2d(
            (uint16_t)w, (uint16_t)h, false, 1,
            postfx_color_format(),
            BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, NULL);

        bgfx_attachment_t at;
        memset(&at, 0, sizeof(at));
        bgfx_attachment_init(&at, p->bloom_mip_tex[i], BGFX_ACCESS_WRITE,
                             0, 1, 0, BGFX_RESOLVE_NONE);
        p->bloom_mip_fb[i] = bgfx_create_frame_buffer_from_attachment(1, &at, true);
    }
    p->bloom_mip_count  = mip_count;
    p->bloom_mips_valid = true;
}

/* ── TAA persistent FBO helpers ─────────────────────────────────────── */

/* Allocate the persistent TAA history (RGBA16F, HDR like the scene colour)
 * and motion (RGBA16F; RG would suffice but RGBA16F matches the existing
 * FBO format helper and is universally RT-able) framebuffers at the current
 * pipeline size.  History is the previous resolve; the resolve reprojects
 * into it, so it must NOT participate in the ping-pong chain. */
static void create_taa_fbos(JcePostFXPipeline *p)
{
    if (p->taa_fbos_valid) return;

    p->taa_history_tex = bgfx_create_texture_2d(
        (uint16_t)p->width, (uint16_t)p->height, false, 1,
        postfx_color_format(),
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, NULL);
    {
        bgfx_attachment_t at;
        memset(&at, 0, sizeof(at));
        bgfx_attachment_init(&at, p->taa_history_tex, BGFX_ACCESS_WRITE,
                             0, 1, 0, BGFX_RESOLVE_NONE);
        p->taa_history_fb = bgfx_create_frame_buffer_from_attachment(1, &at, true);
    }

    /* NOTE: the camera-only motion buffer (taa_motion_tex/fb) is NOT allocated
     * here — it is DEAD whenever the renderer supplies an external per-object
     * velocity G-buffer (jce_postfx_set_taa_motion_tex), which the editor and
     * runtime always do when TAA is on.  It is allocated lazily by
     * ensure_taa_motion_fbo() only when the camera-only fallback actually runs,
     * saving a full-res RGBA16F (~16.6 MB @1080p) in the common path. */
    p->taa_fbos_valid = true;
    p->history_valid  = false;  /* fresh buffers → no usable history yet */
}

/* Lazily allocate the camera-only motion buffer (full-res RGBA16F) the first
 * time the no-external-velocity TAA fallback needs it.  Returns true when
 * taa_motion_fb is valid for use this frame. */
static bool ensure_taa_motion_fbo(JcePostFXPipeline *p)
{
    if (p->taa_motion_fb.idx != UINT16_MAX) return true;
    p->taa_motion_tex = bgfx_create_texture_2d(
        (uint16_t)p->width, (uint16_t)p->height, false, 1,
        postfx_color_format(),
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, NULL);
    bgfx_attachment_t at;
    memset(&at, 0, sizeof(at));
    bgfx_attachment_init(&at, p->taa_motion_tex, BGFX_ACCESS_WRITE,
                         0, 1, 0, BGFX_RESOLVE_NONE);
    p->taa_motion_fb = bgfx_create_frame_buffer_from_attachment(1, &at, true);
    return p->taa_motion_fb.idx != UINT16_MAX;
}

static void destroy_taa_fbos(JcePostFXPipeline *p)
{
    if (!p->taa_fbos_valid) return;
    if (p->taa_history_fb.idx != UINT16_MAX)
        bgfx_destroy_frame_buffer(p->taa_history_fb);
    if (p->taa_motion_fb.idx != UINT16_MAX)
        bgfx_destroy_frame_buffer(p->taa_motion_fb);
    p->taa_history_fb.idx  = UINT16_MAX;
    p->taa_motion_fb.idx   = UINT16_MAX;
    p->taa_history_tex.idx = UINT16_MAX;  /* owned by the FBs */
    p->taa_motion_tex.idx  = UINT16_MAX;
    p->taa_fbos_valid = false;
    p->history_valid  = false;
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
    p->prog_bloom_extract.idx = UINT16_MAX;
    p->prog_bloom_blur.idx    = UINT16_MAX;
    p->prog_bloom_combine.idx = UINT16_MAX;
    p->prog_fxaa.idx          = UINT16_MAX;
    p->prog_vignette.idx      = UINT16_MAX;
    p->prog_chromatic.idx     = UINT16_MAX;
    p->prog_grayscale.idx     = UINT16_MAX;
    p->prog_composite.idx     = UINT16_MAX;
    p->prog_present.idx        = UINT16_MAX;
    p->prog_motion_vec.idx    = UINT16_MAX;
    p->prog_taa.idx           = UINT16_MAX;
    p->prog_custom.idx        = UINT16_MAX;
    p->shader_pak             = NULL;

    /* TAA: nothing allocated until the renderer first enables it. */
    p->taa_enabled       = false;
    p->taa_have_matrices = false;
    p->taa_fbos_valid    = false;
    p->history_valid     = false;
    p->taa_history_tex.idx = UINT16_MAX;
    p->taa_history_fb.idx  = UINT16_MAX;
    p->taa_motion_tex.idx  = UINT16_MAX;
    p->taa_motion_fb.idx   = UINT16_MAX;
    p->taa_ext_motion_tex.idx = UINT16_MAX;
    p->taa_params[0] = 0.9f;   /* feedback */
    p->taa_params[1] = 1.0f;   /* luma_clamp */
    p->taa_params[2] = 1.0f;   /* motion_clamp */
    p->taa_params[3] = 0.0f;
    p->custom_name[0]         = '\0';
    p->custom_loaded[0]       = '\0';
    p->custom_needs_depth     = false;
    p->custom_param_count     = 0;
    reset_output_state(p);
    p->view_base = JCE_VIEW_POST_BASE;

    /* Stage-1a.5: tonemap-op / 3D-LUT / soft bloom init. */
    p->tonemap_op    = 0;
    p->lut_tex.idx   = UINT16_MAX;
    /* Sentinel-init the OWNED placeholder LUT too: it is the only destroyed
     * handle omitted from this block (lut_tex above is borrowed, never
     * destroyed).  bgfx idx==0 is a valid handle — a future early-return
     * before its creation (line ~553) would make jce_postfx_destroy free
     * foreign 3D-texture 0. */
    p->dummy_lut3d.idx = UINT16_MAX;
    p->lut_size      = 0;
    p->lut_strength  = 0.0f;
    p->bloom_knee    = 0.0f;
    p->bloom_quality = 0;
    p->u_gradeParams.idx = UINT16_MAX;
    p->s_texLUT.idx      = UINT16_MAX;
    p->prog_bloom_down.idx = UINT16_MAX;
    p->prog_bloom_up.idx   = UINT16_MAX;
    for (int i = 0; i < POSTFX_BLOOM_MAX_MIPS; i++) {
        p->bloom_mip_tex[i].idx = UINT16_MAX;
        p->bloom_mip_fb[i].idx  = UINT16_MAX;
    }
    p->bloom_mip_count = 0;
    p->bloom_mips_valid = false;

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

    /* TAA uniforms/samplers (created unconditionally — cheap; the passes
     * that consume them only run when TAA is enabled). The sampler names
     * match fs_taa.sc (s_texColor/s_texHistory/s_texMotion) and
     * fs_motion_vec.sc (s_texDepth + u_jceInvViewProj/u_jcePrevViewProj). */
    p->u_taaParams       = bgfx_create_uniform("u_taaParams",        BGFX_UNIFORM_TYPE_VEC4, 1);
    p->u_taaInvViewProj  = bgfx_create_uniform("u_jceInvViewProj",   BGFX_UNIFORM_TYPE_MAT4, 1);
    p->u_taaPrevViewProj = bgfx_create_uniform("u_jcePrevViewProj",  BGFX_UNIFORM_TYPE_MAT4, 1);
    p->u_texHistory      = bgfx_create_uniform("s_texHistory",       BGFX_UNIFORM_TYPE_SAMPLER, 1);
    p->u_texMotion       = bgfx_create_uniform("s_texMotion",        BGFX_UNIFORM_TYPE_SAMPLER, 1);

    /* Stage-1a.5 grade/LUT uniforms. */
    p->u_gradeParams = bgfx_create_uniform("u_gradeParams", BGFX_UNIFORM_TYPE_VEC4, 1);
    p->s_texLUT      = bgfx_create_uniform("s_texLUT",      BGFX_UNIFORM_TYPE_SAMPLER, 1);

    /* 1x1x1 neutral placeholder for the 3D-LUT sampler.  When no grade LUT is
     * loaded, stage 2 used to stay unbound — "safe on all backends" was wrong:
     * on WebGL2 the dangling SAMPLER3D uniform defaults to texture unit 0,
     * clashing with s_texColor (2D) there, and ANGLE rejects the ENTIRE
     * composite draw ("Two textures of different types use the same sampler
     * location") — the scene never reached the backbuffer (web black-screen). */
    {
        uint32_t texel = 0xFFFFFFFFu;
        const bgfx_memory_t *mem = bgfx_copy(&texel, 4);
        p->dummy_lut3d = bgfx_create_texture_3d(1, 1, 1, false,
                                                BGFX_TEXTURE_FORMAT_RGBA8, 0, mem);
    }

    LOG_SUCCESS(LOG_TAG, "post-fx pipeline created (%ux%u)", width, height);
    return p;
}

void jce_postfx_destroy(JcePostFXPipeline *pipeline)
{
    if (!pipeline) return;

    destroy_fbos(pipeline);
    destroy_taa_fbos(pipeline);
    destroy_bloom_mips(pipeline);

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
    bgfx_destroy_uniform(pipeline->u_taaParams);
    bgfx_destroy_uniform(pipeline->u_taaInvViewProj);
    bgfx_destroy_uniform(pipeline->u_taaPrevViewProj);
    bgfx_destroy_uniform(pipeline->u_texHistory);
    bgfx_destroy_uniform(pipeline->u_texMotion);
    /* Stage-1a.5 grade uniforms. */
    if (pipeline->u_gradeParams.idx != UINT16_MAX) bgfx_destroy_uniform(pipeline->u_gradeParams);
    if (pipeline->s_texLUT.idx      != UINT16_MAX) bgfx_destroy_uniform(pipeline->s_texLUT);
    if (pipeline->dummy_lut3d.idx   != UINT16_MAX) bgfx_destroy_texture(pipeline->dummy_lut3d);

    /* Destroy shader programs. */
    if (pipeline->prog_bloom_extract.idx != UINT16_MAX) bgfx_destroy_program(pipeline->prog_bloom_extract);
    if (pipeline->prog_bloom_blur.idx    != UINT16_MAX) bgfx_destroy_program(pipeline->prog_bloom_blur);
    if (pipeline->prog_bloom_combine.idx != UINT16_MAX) bgfx_destroy_program(pipeline->prog_bloom_combine);
    if (pipeline->prog_fxaa.idx          != UINT16_MAX) bgfx_destroy_program(pipeline->prog_fxaa);
    if (pipeline->prog_vignette.idx      != UINT16_MAX) bgfx_destroy_program(pipeline->prog_vignette);
    if (pipeline->prog_chromatic.idx     != UINT16_MAX) bgfx_destroy_program(pipeline->prog_chromatic);
    if (pipeline->prog_grayscale.idx     != UINT16_MAX) bgfx_destroy_program(pipeline->prog_grayscale);
    if (pipeline->prog_composite.idx     != UINT16_MAX) bgfx_destroy_program(pipeline->prog_composite);
    if (pipeline->prog_present.idx       != UINT16_MAX) bgfx_destroy_program(pipeline->prog_present);
    if (pipeline->prog_motion_vec.idx    != UINT16_MAX) bgfx_destroy_program(pipeline->prog_motion_vec);
    if (pipeline->prog_taa.idx           != UINT16_MAX) bgfx_destroy_program(pipeline->prog_taa);
    if (pipeline->prog_custom.idx        != UINT16_MAX) bgfx_destroy_program(pipeline->prog_custom);
    /* Stage-1a.5 bloom pyramid programs. */
    if (pipeline->prog_bloom_down.idx    != UINT16_MAX) bgfx_destroy_program(pipeline->prog_bloom_down);
    if (pipeline->prog_bloom_up.idx      != UINT16_MAX) bgfx_destroy_program(pipeline->prog_bloom_up);

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
    /* TAA history/motion are also size-locked — free them so the next enabled
     * apply lazily reallocates at the new size; history_valid resets so the
     * first post-resize resolve treats the (empty) history as invalid. */
    destroy_taa_fbos(pipeline);
    /* Bloom mip pyramid is size-locked — free so it reallocates at the new size. */
    destroy_bloom_mips(pipeline);
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

/* ── Stage-1a.5: tonemap-op / 3D-LUT / soft-knee bloom setters/getters ── */

void jce_postfx_set_tonemap_op(JcePostFXPipeline *p, int op) {
    if (!p) return; if (op < 0) op = 0; if (op > 2) op = 0; p->tonemap_op = op;
}
int jce_postfx_get_tonemap_op(const JcePostFXPipeline *p) { return p ? p->tonemap_op : 0; }

void jce_postfx_set_lut(JcePostFXPipeline *p, JceTexture lut, int n, float s) {
    if (!p) return;
    p->lut_tex.idx = lut.idx; p->lut_size = n;
    if (s < 0.0f) s = 0.0f; if (s > 1.0f) s = 1.0f; p->lut_strength = s;
}
void jce_postfx_get_lut(const JcePostFXPipeline *p, JceTexture *ol, int *on, float *os) {
    if (ol) ol->idx = p ? p->lut_tex.idx : UINT16_MAX;
    if (on) *on = p ? p->lut_size : 0;
    if (os) *os = p ? p->lut_strength : 0.0f;
}
void  jce_postfx_set_bloom_knee(JcePostFXPipeline *p, float k) {
    if (!p) return; if (k < 0.0f) k = 0.0f; if (k > 1.0f) k = 1.0f; p->bloom_knee = k;
}
float jce_postfx_get_bloom_knee(const JcePostFXPipeline *p) { return p ? p->bloom_knee : 0.0f; }
void jce_postfx_set_bloom_quality(JcePostFXPipeline *p, int m) {
    if (!p) return; if (m < 0) m = 0; if (m > POSTFX_BLOOM_MAX_MIPS) m = POSTFX_BLOOM_MAX_MIPS;
    p->bloom_quality = m;
}
int jce_postfx_get_bloom_quality(const JcePostFXPipeline *p) { return p ? p->bloom_quality : 0; }

/* ── TAA configuration ─────────────────────────────────────────────── */

void jce_postfx_set_taa(JcePostFXPipeline *pipeline, bool enabled,
                        float feedback, float luma_clamp, float motion_clamp)
{
    if (!pipeline) return;
    /* TAA needs the RGBA16F targets: the motion buffer stores SIGNED NDC
     * deltas, which the RGBA8 fallback (see postfx_color_format) would clamp
     * to [0,1] and smear the resolve.  On such backends TAA stays off. */
    if (enabled && postfx_color_format() != BGFX_TEXTURE_FORMAT_RGBA16F) {
        LOG_WARN(LOG_TAG, "TAA requested but RGBA16F targets are unavailable; "
                          "keeping TAA off");
        enabled = false;
    }
    pipeline->taa_enabled    = enabled;
    pipeline->taa_params[0]  = feedback;
    pipeline->taa_params[1]  = luma_clamp;
    pipeline->taa_params[2]  = motion_clamp;
    pipeline->taa_params[3]  = 0.0f;
}

void jce_postfx_set_taa_matrices(JcePostFXPipeline *pipeline,
                                 const jce_mat4 *scene_inv_view_proj,
                                 const jce_mat4 *prev_view_proj)
{
    if (!pipeline) return;
    if (!scene_inv_view_proj || !prev_view_proj) {
        pipeline->taa_have_matrices = false;
        return;
    }
    memcpy(pipeline->taa_inv_view_proj,  JCE_M4_PTR(*scene_inv_view_proj),
           sizeof(pipeline->taa_inv_view_proj));
    memcpy(pipeline->taa_prev_view_proj, JCE_M4_PTR(*prev_view_proj),
           sizeof(pipeline->taa_prev_view_proj));
    pipeline->taa_have_matrices = true;
}

void jce_postfx_set_taa_motion_tex(JcePostFXPipeline *pipeline,
                                   JceTextureHandle tex)
{
    if (!pipeline) return;
    pipeline->taa_ext_motion_tex.idx = tex.idx;
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

    pipeline->prog_bloom_extract = load_postfx_prog(pak, "bloom_extract");
    pipeline->prog_bloom_blur    = load_postfx_prog(pak, "bloom_blur");
    pipeline->prog_bloom_combine = load_postfx_prog(pak, "bloom_combine");
    pipeline->prog_fxaa          = load_postfx_prog(pak, "fxaa");
    pipeline->prog_vignette      = load_postfx_prog(pak, "vignette");
    pipeline->prog_chromatic     = load_postfx_prog(pak, "chromatic");
    pipeline->prog_grayscale     = load_postfx_prog(pak, "grayscale");
    pipeline->prog_composite     = load_postfx_prog(pak, "composite");
    pipeline->prog_present       = load_postfx_prog(pak, "present");
    /* TAA pair (optional; absence just means r.taa is a no-op on this build). */
    pipeline->prog_motion_vec    = load_postfx_prog(pak, "motion_vec");
    pipeline->prog_taa           = load_postfx_prog(pak, "taa");

    /* Stage-1a.5 bloom pyramid (optional: absence -> single-mip legacy path). */
    pipeline->prog_bloom_down = load_postfx_prog(pak, "bloom_down");
    pipeline->prog_bloom_up   = load_postfx_prog(pak, "bloom_up");

    /* Count how many loaded successfully. */
    int loaded = 0;
    if (pipeline->prog_bloom_extract.idx != UINT16_MAX) loaded++;
    if (pipeline->prog_bloom_blur.idx    != UINT16_MAX) loaded++;
    if (pipeline->prog_bloom_combine.idx != UINT16_MAX) loaded++;
    if (pipeline->prog_fxaa.idx          != UINT16_MAX) loaded++;
    if (pipeline->prog_vignette.idx      != UINT16_MAX) loaded++;
    if (pipeline->prog_chromatic.idx     != UINT16_MAX) loaded++;
    if (pipeline->prog_grayscale.idx     != UINT16_MAX) loaded++;
    if (pipeline->prog_composite.idx     != UINT16_MAX) loaded++;

    pipeline->shaders_loaded = (loaded > 0);
    LOG_INFO(LOG_TAG, "post-fx shaders loaded: %d/8", loaded);
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

    /* TAA runs iff requested AND the resolve program is loaded.  Motion comes
       from EITHER the renderer's external velocity buffer (per-object motion;
       needs no depth or camera matrices here) OR the internal camera-only
       reprojection pass (which needs prog_motion_vec + matrices + depth).  With
       neither motion source available we silently skip TAA (the rest of the
       chain is unaffected). */
    const bool have_ext_motion = (pipeline->taa_ext_motion_tex.idx != UINT16_MAX);
    const bool have_cam_motion =
        pipeline->prog_motion_vec.idx != UINT16_MAX &&
        pipeline->taa_have_matrices &&
        jce_gfx_texture_valid(scene_depth);
    const bool taa_run =
        pipeline->taa_enabled &&
        pipeline->prog_taa.idx != UINT16_MAX &&
        (have_ext_motion || have_cam_motion);

    /* Count active effects. */
    int active = 0;
    for (int i = 0; i < JCE_POSTFX_COUNT; i++) {
        if (pipeline->enabled[i]) active++;
    }
    /* TAA alone (no other effect) still needs the chain to run so its resolve
       becomes the pipeline output. */
    if (active == 0 && !taa_run) return;

    /* Ensure FBOs are created.  Composite ping-pong pair (0/1) always; the
       bloom buffers (2/3) only when bloom is enabled this frame — standard
       "allocate only what the enabled chain needs" (saves ~33MB at 1080p on
       the bloom-off LOW/MEDIUM-tier path). */
    if (!pipeline->fbos_valid)
        create_fbos(pipeline);
    if (!pipeline->fbos_valid) return;
    if (pipeline->enabled[JCE_POSTFX_BLOOM])
        ensure_bloom_fbos(pipeline);
    /* TAA history/motion are lazily allocated ONLY when TAA actually runs —
       a TAA-off frame never touches them, keeping the chain byte-identical. */
    if (taa_run) {
        create_taa_fbos(pipeline);
        if (!pipeline->taa_fbos_valid) return;
    }

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
    /* Reserve the first 3 view IDs of the post block for TAA (motion +
       resolve + history-copy) when it runs, so they order BEFORE the chain
       (bgfx renders views in ascending ID order). */
    uint16_t view_id = (uint16_t)(pipeline->view_base + (taa_run ? 3 : 0));
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

    /* ── 0. TAA (runs FIRST; resolves the jittered scene against history) ──
     * Three sub-passes on the reserved view_base+0..+2 block:
     *   (a) motion-vec : reconstruct world pos from depth, project through the
     *       previous camera, write NDC motion delta (RG) into the motion FBO.
     *   (b) resolve    : blend current colour with reprojected, neighbourhood-
     *       clamped history into a ping-pong target → becomes the chain input.
     *   (c) history-copy: copy the resolve into the persistent history FBO so
     *       next frame reprojects against it. */
    if (taa_run) {
        const uint16_t v_motion  = (uint16_t)(pipeline->view_base + 0);
        const uint16_t v_resolve = (uint16_t)(pipeline->view_base + 1);
        const uint16_t v_copy    = (uint16_t)(pipeline->view_base + 2);

        /* (a) motion-vec pass: depth → motion FBO.  STANDARD per-object motion:
         * when the renderer supplied an EXTERNAL velocity buffer (its geometry-
         * pass velocity G-buffer, written with the SAME encoding as
         * fs_motion_vec.sc), we SKIP this camera-only full-screen pass entirely
         * and bind that texture as s_texMotion in the resolve below — so moving
         * /animated/skinned geometry reprojects correctly instead of ghosting.
         * Without an external buffer we run the legacy camera-only pass so the
         * camera-reprojection fallback still works. */
        const bool use_ext_motion =
            (pipeline->taa_ext_motion_tex.idx != UINT16_MAX);
        /* Camera-only fallback: lazily allocate taa_motion_fb here (the common
         * external-velocity path never touches it, so it stays unallocated and
         * saves a full-res RGBA16F). */
        bgfx_texture_handle_t motion_tex;
        if (use_ext_motion) {
            motion_tex = pipeline->taa_ext_motion_tex;
        } else if (ensure_taa_motion_fbo(pipeline)) {
            motion_tex = pipeline->taa_motion_tex;
            /* We supply BOTH camera matrices as explicit uniforms (the postfx
             * fullscreen path can't use bgfx_set_view_transform without
             * corrupting vs_postfx's quad), so the view leaves u_modelViewProj
             * at identity like every other pass. */
            bgfx_set_uniform(pipeline->u_taaInvViewProj,  pipeline->taa_inv_view_proj,  1);
            bgfx_set_uniform(pipeline->u_taaPrevViewProj, pipeline->taa_prev_view_proj, 1);
            bgfx_texture_handle_t depth_tex = { scene_depth.idx };
            POSTFX_SETUP_VIEW(v_motion, pipeline->taa_motion_fb);
            bgfx_set_view_name(v_motion, "PostFX/TAA_Motion", INT32_MAX);
            bgfx_set_texture(0, pipeline->u_texDepth, depth_tex, UINT32_MAX);
            draw_fullscreen(pipeline, v_motion, pipeline->prog_motion_vec);
        } else {
            /* OOM: bind a valid handle so the resolve sampler is satisfied
             * (TAA degrades to ~no reprojection rather than crashing). */
            motion_tex = pipeline->taa_history_tex;
        }

        /* (b) resolve pass: current colour + history + motion → fbo[ping].
         * On the first frame (history_valid false) the history buffer is empty;
         * fs_taa's on-screen test + a large/zero motion keeps the output ≈
         * current, so no garbage history leaks in.  We still bind a valid
         * history handle (the buffer exists) to satisfy the sampler. */
        bgfx_set_uniform(pipeline->u_taaParams, pipeline->taa_params, 1);
        ensure_composite_fbo(pipeline, ping);
        POSTFX_SETUP_VIEW(v_resolve, pipeline->fbo[ping]);
        bgfx_set_view_name(v_resolve, "PostFX/TAA_Resolve", INT32_MAX);
        bgfx_set_texture(0, pipeline->u_texColor,   current_tex,             UINT32_MAX);
        bgfx_set_texture(1, pipeline->u_texHistory, pipeline->taa_history_tex, UINT32_MAX);
        bgfx_set_texture(2, pipeline->u_texMotion,  motion_tex,              UINT32_MAX);
        draw_fullscreen(pipeline, v_resolve, pipeline->prog_taa);

        bgfx_texture_handle_t resolved = pipeline->fbo_tex[ping];

        /* (c) copy resolve → history (pass-through fullscreen) for next frame. */
        if (pipeline->prog_present.idx != UINT16_MAX) {
            POSTFX_SETUP_VIEW(v_copy, pipeline->taa_history_fb);
            bgfx_set_view_name(v_copy, "PostFX/TAA_HistoryCopy", INT32_MAX);
            bgfx_set_texture(0, pipeline->u_texColor, resolved, UINT32_MAX);
            draw_fullscreen(pipeline, v_copy, pipeline->prog_present);
        }

        /* Resolved colour feeds the remaining chain; advance the ping-pong. */
        current_tex = resolved;
        current_fb_index = ping;
        ping = 1 - ping;
        pipeline->history_valid = true;
    }

    /* ── 1. Bloom ─────────────────────────────────────────────────── */
    if (pipeline->enabled[JCE_POSTFX_BLOOM] &&
        pipeline->prog_bloom_extract.idx != UINT16_MAX)
    {
        /* Bloom extract pass → FBO 2.
         * z=knee (Stage-1a.5 soft-knee; 0 = hard cutoff byte-identical).
         * w=Karis-avg flag for bloom_down (mip0 uses weighted avg; here unused). */
        float bloom_params[4] = {
            pipeline->params.bloom_threshold,
            pipeline->params.bloom_intensity,
            pipeline->bloom_knee,   /* z = soft knee (0 = legacy hard cutoff) */
            0.0f                    /* w = mip0 Karis flag (only used by bloom_down) */
        };
        bgfx_set_uniform(pipeline->u_bloomParams, bloom_params, 1);

        POSTFX_SETUP_VIEW(view_id, pipeline->fbo[2]);
        bgfx_set_view_name(view_id, "PostFX/BloomExtract", INT32_MAX);
        bgfx_set_texture(0, pipeline->u_texColor, current_tex, UINT32_MAX);
        draw_fullscreen(pipeline, view_id, pipeline->prog_bloom_extract);
        view_id++;

        /* ── HIGH/ULTRA: dual-filter pyramid (downsample + upsample) ─── */
        const bool use_pyramid =
            pipeline->bloom_quality > 0 &&
            pipeline->prog_bloom_down.idx != UINT16_MAX &&
            pipeline->prog_bloom_up.idx   != UINT16_MAX;

        if (use_pyramid) {
            ensure_bloom_mips(pipeline, pipeline->bloom_quality);

            if (pipeline->bloom_mips_valid) {
                int n = pipeline->bloom_mip_count;
                char view_label[64];

                /* Downsample chain: extract result → mip[0] → mip[1] → ... → mip[n-1]. */
                bgfx_texture_handle_t down_src = pipeline->fbo_tex[2]; /* extract result */
                for (int mi = 0; mi < n; mi++) {
                    uint32_t src_w = (mi == 0) ? pipeline->width  : pipeline->bloom_mip_w[mi - 1];
                    uint32_t src_h = (mi == 0) ? pipeline->height : pipeline->bloom_mip_h[mi - 1];
                    float ts[4] = { 1.0f / (float)src_w, 1.0f / (float)src_h, 0.0f, 0.0f };
                    bgfx_set_uniform(pipeline->u_texelSize, ts, 1);

                    /* mip0 uses Karis luma-weighted average to kill fireflies. */
                    float bp[4] = { 0.0f, 0.0f, 0.0f, (mi == 0) ? 1.0f : 0.0f };
                    bgfx_set_uniform(pipeline->u_bloomParams, bp, 1);

                    bgfx_set_view_rect(view_id, 0, 0,
                        (uint16_t)pipeline->bloom_mip_w[mi],
                        (uint16_t)pipeline->bloom_mip_h[mi]);
                    bgfx_set_view_frame_buffer(view_id, pipeline->bloom_mip_fb[mi]);
                    bgfx_set_view_clear(view_id, BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);
                    snprintf(view_label, sizeof(view_label), "PostFX/BloomDown%d", mi);
                    bgfx_set_view_name(view_id, view_label, INT32_MAX);
                    bgfx_set_texture(0, pipeline->u_texColor, down_src, UINT32_MAX);
                    bgfx_set_vertex_buffer(0, pipeline->quad_vb, 0, 4);
                    bgfx_set_index_buffer(pipeline->quad_ib, 0, 6);
                    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A, 0);
                    bgfx_submit(view_id, pipeline->prog_bloom_down, 0, BGFX_DISCARD_ALL);
                    down_src = pipeline->bloom_mip_tex[mi];
                    view_id++;
                }

                /* Upsample chain: mip[n-1] → mip[n-2] → ... → mip[0] → fbo[2].
                 * Each step adds onto the higher-res target (BGFX_STATE_BLEND_ADD). */
                for (int mi = n - 1; mi >= 0; mi--) {
                    bgfx_texture_handle_t up_src = pipeline->bloom_mip_tex[mi];
                    bgfx_frame_buffer_handle_t up_dst;
                    uint32_t dst_w, dst_h;
                    if (mi == 0) {
                        /* Final upsample: write into fbo[2] (the composite's bloom input). */
                        up_dst = pipeline->fbo[2];
                        dst_w  = pipeline->width;
                        dst_h  = pipeline->height;
                    } else {
                        up_dst = pipeline->bloom_mip_fb[mi - 1];
                        dst_w  = pipeline->bloom_mip_w[mi - 1];
                        dst_h  = pipeline->bloom_mip_h[mi - 1];
                    }

                    float ts[4] = { 1.0f / (float)dst_w, 1.0f / (float)dst_h, 0.0f, 0.0f };
                    bgfx_set_uniform(pipeline->u_texelSize, ts, 1);

                    bgfx_set_view_rect(view_id, 0, 0, (uint16_t)dst_w, (uint16_t)dst_h);
                    bgfx_set_view_frame_buffer(view_id, up_dst);
                    bgfx_set_view_clear(view_id, BGFX_CLEAR_NONE, 0, 1.0f, 0);
                    snprintf(view_label, sizeof(view_label), "PostFX/BloomUp%d", mi);
                    bgfx_set_view_name(view_id, view_label, INT32_MAX);
                    bgfx_set_texture(0, pipeline->u_texColor, up_src, UINT32_MAX);
                    bgfx_set_vertex_buffer(0, pipeline->quad_vb, 0, 4);
                    bgfx_set_index_buffer(pipeline->quad_ib, 0, 6);
                    /* Additive blend: accumulate mip layers onto the higher-res target. */
                    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                                   BGFX_STATE_BLEND_ADD, 0);
                    bgfx_submit(view_id, pipeline->prog_bloom_up, 0, BGFX_DISCARD_ALL);
                    view_id++;
                }

                /* Restore texel_size for downstream passes. */
                bgfx_set_uniform(pipeline->u_texelSize, texel_size, 1);
            }
        } else {
            /* ── LOW/MID: legacy single-mip Gaussian blur (byte-identical) ── */
            if (pipeline->prog_bloom_blur.idx != UINT16_MAX &&
                ensure_bloom_blur_h_fbo(pipeline)) {
                float blur_h[4] = { texel_size[0], 0.0f, 0.0f, 0.0f };
                bgfx_set_uniform(pipeline->u_blurDir, blur_h, 1);
                POSTFX_SETUP_VIEW(view_id, pipeline->fbo[3]);
                bgfx_set_view_name(view_id, "PostFX/BloomBlurH", INT32_MAX);
                bgfx_set_texture(0, pipeline->u_texColor, pipeline->fbo_tex[2], UINT32_MAX);
                draw_fullscreen(pipeline, view_id, pipeline->prog_bloom_blur);
                view_id++;

                float blur_v[4] = { 0.0f, texel_size[1], 0.0f, 0.0f };
                bgfx_set_uniform(pipeline->u_blurDir, blur_v, 1);
                POSTFX_SETUP_VIEW(view_id, pipeline->fbo[2]);
                bgfx_set_view_name(view_id, "PostFX/BloomBlurV", INT32_MAX);
                bgfx_set_texture(0, pipeline->u_texColor, pipeline->fbo_tex[3], UINT32_MAX);
                draw_fullscreen(pipeline, view_id, pipeline->prog_bloom_blur);
                view_id++;
            }
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
                                 pipeline->params.bloom_intensity,
                                 pipeline->bloom_knee, 0.0f };
            /* tonemap_p.z = selectable op id (0=ACES, 1=Neutral, 2=AgX).
             * The shader dispatches on this via step() comparisons (no ==). */
            float tonemap_p[4] = { pipeline->params.exposure,
                                   pipeline->params.gamma,
                                   (float)pipeline->tonemap_op, 0.0f };
            float chrom_p[4] = { pipeline->params.chromatic_strength, 0.0f, 0.0f, 0.0f };
            float vig_p[4]   = { pipeline->params.vignette_intensity,
                                 pipeline->params.vignette_smoothness, 0.0f, 0.0f };
            bgfx_set_uniform(pipeline->u_compositeFlags,  flags,  1);
            bgfx_set_uniform(pipeline->u_compositeFlags2, flags2, 1);
            bgfx_set_uniform(pipeline->u_bloomParams,     bloom_p, 1);
            bgfx_set_uniform(pipeline->u_tonemapParams,   tonemap_p, 1);
            bgfx_set_uniform(pipeline->u_chromaticParams, chrom_p, 1);
            bgfx_set_uniform(pipeline->u_vignetteParams,  vig_p, 1);

            /* Stage-1a.5: 3D-LUT grade + tonemap-op (gated: needs valid handle +
             * strength > 0 + tonemap on — grade is in LDR space after tonemap). */
            const bool grade_on = (pipeline->lut_tex.idx != UINT16_MAX) &&
                                   pipeline->lut_strength > 0.0f && c_tonemap;
            float grade_p[4] = { grade_on ? 1.0f : 0.0f,
                                  pipeline->lut_strength,
                                  (float)(pipeline->lut_size > 0 ? pipeline->lut_size : 1),
                                  0.0f };
            bgfx_set_uniform(pipeline->u_gradeParams, grade_p, 1);
            if (grade_on)
                bgfx_set_texture(2, pipeline->s_texLUT, pipeline->lut_tex, UINT32_MAX);
            else
                /* Grade off: u_gradeParams.x < 0.5 so the shader never SAMPLES
                 * s_texLUT, but the SAMPLER3D uniform must still be parked on a
                 * real 3D texture — unbound it dangles at unit 0 next to
                 * s_texColor (2D) and WebGL2 rejects the whole composite draw. */
                bgfx_set_texture(2, pipeline->s_texLUT, pipeline->dummy_lut3d,
                                 UINT32_MAX);

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

    /* External motion vectors are one-shot per frame: clear so a later apply()
       on this pipeline (pick / preview / thumbnail) does not reuse a stale
       velocity buffer. */
    pipeline->taa_ext_motion_tex.idx = UINT16_MAX;

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
    /* One view past the chain's worst case so submission order is preserved.
     * Worst case = TAA (3 views: motion/resolve/history-copy from view_base+0..2)
     * + bloom extract (1) + pyramid 2*N (up to 12 at quality=6)
     * + composite (1) + fxaa (1) + custom (1) = ends at view_base+18,
     * so +20 stays clear and below JCE_VIEW_EDITOR_OVERLAY (= 50 for scene base 3).
     * LOW/MID (bloom_quality=0): ends at view_base+8, still safely below +20. */
    view_id = (uint16_t)(pipeline->view_base + 20);
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
