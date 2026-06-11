/*
 * jce_postfx.h  Post-processing effect pipeline.
 *
 * Manages a chain of full-screen passes applied after the main
 * scene render.  Each effect reads from the previous pass's
 * framebuffer and writes to the next (or the back buffer).
 *
 * Effects are implemented as bgfx fragment shaders; this module
 * owns the intermediate framebuffers and orchestrates the draw
 * order.
 *
 * Layer: Graphics (Layer 3 — optional subsystem, priority 160).
 */

#ifndef JCE_POSTFX_H
#define JCE_POSTFX_H


#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_gfx_types.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct JcePakArchive JcePakArchive;

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Built-in effect types                                               */
/* ================================================================== */

typedef enum {
    JCE_POSTFX_TONEMAP    = 0,   /* HDR → LDR tone mapping (ACES) */
    JCE_POSTFX_BLOOM      = 1,   /* Bloom (threshold + blur + blend) */
    JCE_POSTFX_FXAA       = 2,   /* Fast approximate anti-aliasing */
    JCE_POSTFX_VIGNETTE   = 3,   /* Edge darkening */
    JCE_POSTFX_CHROMATIC  = 4,   /* Chromatic aberration */
    JCE_POSTFX_GRAYSCALE  = 5,   /* Desaturation */
    JCE_POSTFX_CUSTOM     = 6,   /* Client-supplied full-screen pass (data-driven; see below) */
    JCE_POSTFX_COUNT
} JcePostFXType;

/* Number of generic vec4 parameters available to the custom pass shader. */
#define JCE_POSTFX_CUSTOM_PARAMS 8

/* ================================================================== */
/* Effect parameters                                                   */
/* ================================================================== */

typedef struct {
    /* Tonemap */
    float exposure;              /* default: 1.0 */
    float gamma;                 /* default: 2.2 */

    /* Bloom */
    float bloom_threshold;       /* default: 1.0 */
    float bloom_intensity;       /* default: 0.5 */

    /* FXAA */
    float fxaa_span_max;         /* default: 8.0 */
    float fxaa_reduce_min;       /* default: 1.0/128.0 */
    float fxaa_reduce_mul;       /* default: 1.0/8.0 */

    /* Vignette */
    float vignette_intensity;    /* default: 0.3 */
    float vignette_smoothness;   /* default: 2.0 */

    /* Chromatic */
    float chromatic_strength;    /* default: 0.005 */
} JcePostFXParams;

/* ================================================================== */
/* Pipeline lifecycle                                                  */
/* ================================================================== */

typedef struct JcePostFXPipeline JcePostFXPipeline;

JcePostFXPipeline *jce_postfx_create(jce_allocator_t alloc,
                                     uint32_t width, uint32_t height);
JCE_API void               jce_postfx_destroy(JcePostFXPipeline *pipeline);

/* Recreate internal framebuffers after window resize. */
void jce_postfx_resize(JcePostFXPipeline *pipeline,
                       uint32_t width, uint32_t height);

/* Override the first bgfx view ID used by jce_postfx_apply().
 * Default is JCE_VIEW_POST_BASE (20).  Use a different base when two
 * PostFX pipelines must coexist in the same frame (e.g. scene viewport
 * + game viewport) to avoid view-ID conflicts. */
JCE_API void jce_postfx_set_view_base(JcePostFXPipeline *pipeline, uint16_t base);

/* ================================================================== */
/* Effect chain                                                        */
/* ================================================================== */

/* Enable / disable individual effects.  Order is fixed by enum. */
JCE_API void jce_postfx_enable(JcePostFXPipeline *pipeline, JcePostFXType type, bool enabled);
JCE_API bool jce_postfx_is_enabled(const JcePostFXPipeline *pipeline, JcePostFXType type);

/* Set parameters for the entire pipeline. */
JCE_API void jce_postfx_set_params(JcePostFXPipeline *pipeline, const JcePostFXParams *params);
JCE_API void jce_postfx_get_params(const JcePostFXPipeline *pipeline, JcePostFXParams *out);

/* Return default parameter values. */
JCE_API JcePostFXParams jce_postfx_default_params(void);

/* ================================================================== */
/* Custom pass (data-driven, engine is style-agnostic)                 */
/* ================================================================== */
/*
 * The JCE_POSTFX_CUSTOM slot runs a client-supplied full-screen fragment
 * shader as the final link of the chain.  The engine knows nothing about
 * what the shader does — it simply binds a generic contract and submits a
 * full-screen pass.  This lets an application add bespoke screen-space looks
 * (NPR / stylize / scanlines / …) purely through data + an asset shader,
 * without baking the effect into the engine.
 *
 * Shader contract (the fs the client names must declare):
 *   SAMPLER2D(s_texColor, 0);          // current chain colour
 *   SAMPLER2D(s_texDepth, 1);          // scene depth (valid iff u_postfxTime.y > 0.5)
 *   uniform vec4 u_texelSize;          // (1/w, 1/h, w, h)
 *   uniform vec4 u_postfxTime;         // x = elapsed seconds, y = has_depth (0/1)
 *   uniform vec4 u_postfxParams[JCE_POSTFX_CUSTOM_PARAMS];  // meaning defined by the shader
 */

/* Select the custom-pass fragment shader by base name (loaded from the
 * "postfx" shader set in the PAK passed to jce_postfx_load_shaders()).
 * Pass NULL or "" to clear.  needs_depth requests the scene depth at stage 1.
 * The program is (re)loaded lazily on the next apply() when the name changes. */
JCE_API void jce_postfx_set_custom_shader(JcePostFXPipeline *pipeline,
                                          const char *fs_name, bool needs_depth);

/* Upload the generic vec4 parameter array consumed by the custom shader.
 * `count` is the number of vec4s (clamped to JCE_POSTFX_CUSTOM_PARAMS);
 * `vec4s` points to count*4 floats. */
JCE_API void jce_postfx_set_custom_params(JcePostFXPipeline *pipeline,
                                          const float *vec4s, int count);

/* Read back the custom-pass configuration — used to mirror one pipeline's
 * settings onto another (e.g. scene viewport → game viewport). */
JCE_API void jce_postfx_get_custom_shader(const JcePostFXPipeline *pipeline,
                                          char *out_name, int out_size,
                                          bool *out_needs_depth);
JCE_API int  jce_postfx_get_custom_params(const JcePostFXPipeline *pipeline,
                                          float *out_vec4s, int max_count);

/* ================================================================== */
/* Rendering                                                           */
/* ================================================================== */

/* Load the shaders required by the enabled effects.
   Must be called after renderer and shader system are ready.
   pak: the PAK archive containing compiled shader binaries. */
bool jce_postfx_load_shaders(JcePostFXPipeline *pipeline,
                             const JcePakArchive *pak);

/* Execute the enabled post-processing chain.
   scene_fb: the framebuffer containing the rendered scene.
   The final result is written to the back buffer. */
void jce_postfx_apply(JcePostFXPipeline *pipeline,
                      JceTextureHandle scene_color,
                      JceTextureHandle scene_depth);

/* Get the output texture after jce_postfx_apply.
   Returns JCE_TEXTURE_INVALID if no effects were active. */
JCE_API JceTextureHandle jce_postfx_get_output(const JcePostFXPipeline *pipeline);

/* Draw the chain's output texture to the BACKBUFFER as a fullscreen pass.
 * The runtime presentation step for consumers that render their scene into
 * an offscreen target and post-process it (the editor composites the output
 * texture into its viewport instead and never calls this). No-op when
 * apply() produced no output this frame. */
JCE_API void jce_postfx_present(JcePostFXPipeline *pipeline,
                                uint32_t width, uint32_t height);

/* Returns the bgfx framebuffer handle (as raw uint16) currently holding the
 * postfx output texture, or UINT16_MAX if no apply has run.
 * Editors can submit overlay passes to this FBO so gizmos render *after*
 * tone-mapping / bloom instead of being filtered through PostFX. */
JCE_API uint16_t jce_postfx_get_output_framebuffer(const JcePostFXPipeline *pipeline);

JCE_EXTERN_C_END

#endif /* JCE_POSTFX_H */
