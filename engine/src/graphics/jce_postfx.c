/*
 * jce_postfx.c  Post-processing effect pipeline implementation.
 *
 * Manages a chain of full-screen passes.  Each pass is a simple
 * full-screen triangle rendered with a specific fragment shader
 * and the previous pass's output as input texture.
 */

#include <jce/graphics/jce_postfx.h>
#include <jce/core/jce_log.h>

#include <string.h>

#define LOG_TAG "postfx"

/* ── Pipeline struct ───────────────────────────────────────────────── */

struct JcePostFXPipeline {
    jce_allocator_t alloc;
    uint32_t        width;
    uint32_t        height;
    bool            enabled[JCE_POSTFX_COUNT];
    JcePostFXParams params;
    bool            shaders_loaded;
};

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

    LOG_SUCCESS(LOG_TAG, "post-fx pipeline created (%ux%u)", width, height);
    return p;
}

void jce_postfx_destroy(JcePostFXPipeline *pipeline)
{
    if (!pipeline) return;
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

/* ── Load shaders ──────────────────────────────────────────────────── */

bool jce_postfx_load_shaders(JcePostFXPipeline *pipeline)
{
    if (!pipeline) return false;

    /* Shader loading is deferred until actual bgfx integration.
       For now, mark as ready so the pipeline can be tested. */
    pipeline->shaders_loaded = true;
    LOG_INFO(LOG_TAG, "post-fx shaders loaded");
    return true;
}

/* ── Apply ─────────────────────────────────────────────────────────── */

void jce_postfx_apply(JcePostFXPipeline *pipeline,
                      JceTextureHandle scene_color,
                      JceTextureHandle scene_depth)
{
    if (!pipeline || !pipeline->shaders_loaded) return;
    (void)scene_color;
    (void)scene_depth;

    /* Count active effects for debug. */
    int active = 0;
    for (int i = 0; i < JCE_POSTFX_COUNT; i++) {
        if (pipeline->enabled[i]) active++;
    }

    if (active == 0) return;

    /*
     * Pipeline execution order (when enabled):
     *   1. Bloom — extract bright pixels, blur, blend
     *   2. Tonemap — HDR → LDR
     *   3. FXAA — anti-aliasing
     *   4. Chromatic aberration
     *   5. Vignette
     *   6. Grayscale
     *
     * Each pass reads from the previous output and writes to the next
     * intermediate framebuffer.  The final pass writes to view 0
     * (back buffer).
     *
     * Full implementation requires bgfx framebuffer creation and
     * shader uniform binding — deferred to renderer integration.
     */

    LOG_TRACE(LOG_TAG, "post-fx apply: %d effects active", active);
}
