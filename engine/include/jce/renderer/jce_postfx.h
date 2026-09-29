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
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_auto_exposure.h>
#include <jce/renderer/jce_gfx_types.h>
#include <jce/renderer/jce_texture_types.h>

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

JCE_API JcePostFXPipeline *jce_postfx_create(jce_allocator_t alloc,
                                     uint32_t width, uint32_t height);
JCE_API void               jce_postfx_destroy(JcePostFXPipeline *pipeline);

/* Recreate internal framebuffers after window resize. */
JCE_API void jce_postfx_resize(JcePostFXPipeline *pipeline,
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

/* ── Tonemap operator (selectable; same 0/1/2 as JceSceneTonemapOp) ── */
typedef enum {
    JCE_POSTFX_TONEMAP_ACES    = 0,
    JCE_POSTFX_TONEMAP_NEUTRAL = 1,
    JCE_POSTFX_TONEMAP_AGX     = 2,
} JcePostFXTonemap;

JCE_API void jce_postfx_set_tonemap_op(JcePostFXPipeline *pipeline, int op);
JCE_API int  jce_postfx_get_tonemap_op(const JcePostFXPipeline *pipeline);

/* 3D-LUT colour grade applied inside the composite pass (after tonemap).
 * lut_size = N (edge length). strength 0 = neutral. Invalid lut handle or
 * strength 0 => the composite's grade branch is an algebraic no-op. */
JCE_API void jce_postfx_set_lut(JcePostFXPipeline *pipeline, JceTexture lut,
                                int lut_size, float strength);
JCE_API void jce_postfx_get_lut(const JcePostFXPipeline *pipeline,
                                JceTexture *out_lut, int *out_size,
                                float *out_strength);

/* Soft-knee bloom: knee 0 = current hard cutoff (byte-identical). */
JCE_API void  jce_postfx_set_bloom_knee(JcePostFXPipeline *pipeline, float knee);
JCE_API float jce_postfx_get_bloom_knee(const JcePostFXPipeline *pipeline);

/* Bloom quality: 0 = legacy single-mip path (LOW/MID, byte-identical);
 * >0 = number of downsample mips for the HIGH/ULTRA dual-filter pyramid. */
JCE_API void jce_postfx_set_bloom_quality(JcePostFXPipeline *pipeline, int mip_count);
JCE_API int  jce_postfx_get_bloom_quality(const JcePostFXPipeline *pipeline);

/* ================================================================== */
/* Motion blur                                                         */
/* ================================================================== */
/*
 * Per-pixel motion blur, folded into the composite pass.
 *
 * WHAT WAS MISSING.  JceRenderPipelineDesc.enable_motion_blur has been set by
 * three editor UIs, serialised to .rp.json, defaulted ON by the ULTRA preset,
 * cooked into the shipped PAK and answered by
 * jce_render_pipeline_feature_enabled("motion_blur") since it was written --
 * with no render pass reading it.  Turning it on changed nothing on screen.
 *
 * IT NEEDS A MOTION SOURCE and says so rather than degrading quietly: either
 * the renderer's per-object velocity buffer (jce_postfx_set_taa_motion_tex --
 * the accurate one, it moves with animated and skinned geometry) or the
 * camera-only reprojection pass, which needs jce_postfx_set_taa_matrices()
 * and a valid scene depth.  With neither, jce_postfx_get_motion_blur_active()
 * returns false and the chain is byte-identical.  The camera-only source
 * blurs a turning camera correctly and leaves a car crossing a static frame
 * perfectly sharp; that is a property of the source, not a bug in the blur.
 *
 * `intensity` scales the trail length: 0 disables, 1 smears a pixel across the
 * full frame-to-frame displacement, which is the physically-motivated value
 * for a 360-degree shutter.  The trail is also capped in UV units inside the
 * engine so a camera CUT -- where the motion vector is both enormous and
 * meaningless -- cannot turn the frame to soup; that cap is not a parameter
 * because nothing authors it, and an unauthored knob is a knob somebody finds
 * unread a year later.
 */
JCE_API void jce_postfx_set_motion_blur(JcePostFXPipeline *pipeline,
                                        bool enabled, float intensity);
JCE_API bool jce_postfx_get_motion_blur(const JcePostFXPipeline *pipeline);
/* True only when it is enabled AND a motion source was available on the last
 * apply().  This is the honest question -- "is it on" and "is it doing
 * anything" are different, and every unwired-feature defect in this engine
 * lived in the gap between them. */
JCE_API bool jce_postfx_get_motion_blur_active(const JcePostFXPipeline *pipeline);

/* ================================================================== */
/* Depth of field                                                      */
/* ================================================================== */
/*
 * Rack focus: everything at `focus_distance` metres is sharp, everything
 * further than `focus_range` metres from it is progressively blurred.  Unity's
 * DoF volume, UE's post-process focal distance, Godot's CameraAttributes --
 * this is the same knob under the names those three use.
 *
 * NOT an entry in JcePostFXType, deliberately.  That enum ends in a COUNT
 * that sizes public arrays (JceSceneRenderingSettings.postfx_enabled among
 * them), so growing it moves every member after it -- a real ABI break to add
 * an effect.  Motion blur set the precedent here for the same reason: an
 * effect that lives inside the composite pass is a pipeline parameter, and it
 * keeps that pass alive on its own.
 *
 * THE CAMERA'S PROJECTION IS REQUIRED, and it is not decoration.  The shader
 * compares raw stored depth, so the focus distances have to be projected
 * through the SAME matrix that filled the depth buffer.  Doing that here
 * rather than in the shader is what lets the shader work under reverse-Z and
 * under both clip-range conventions with no per-backend branch -- the exact
 * class of bug this renderer has shipped twelve times in one campaign.
 * `proj16` is 16 floats in the engine's jce_mat4 order; NULL disables.
 *
 * `max_coc` is the widest blur radius, as a fraction of screen width.  0.02
 * is a strong cinematic defocus; the gather is 16 taps, so far above that the
 * disk thins out into visible speckle rather than getting blurrier.
 */
JCE_API void jce_postfx_set_depth_of_field(JcePostFXPipeline *pipeline,
                                           bool enabled,
                                           float focus_distance,
                                           float focus_range,
                                           float max_coc,
                                           const float *proj16);
JCE_API bool jce_postfx_get_depth_of_field(const JcePostFXPipeline *pipeline);
/* True only when it is enabled AND a usable projection and depth buffer were
 * present on the last apply().  "Is it on" and "is it doing anything" are
 * different questions, and every unwired-feature defect in this engine lived
 * in the gap between them. */
JCE_API bool jce_postfx_get_depth_of_field_active(const JcePostFXPipeline *pipeline);

/* ================================================================== */
/* Auto exposure (eye adaptation)                                      */
/* ================================================================== */
/*
 * The scene decides the exposure instead of an artist pinning it per level.
 *
 * OFF BY DEFAULT, and that is a decision, not an oversight: every scene
 * authored before this was lit against a FIXED exposure, so switching
 * adaptation on for all of them would change how all of them look.
 *
 * WHAT RUNS.  One extra full-screen downsample of the chain INPUT (the HDR
 * scene colour, before tonemap -- after it, a bright scene and a dim one
 * compress to nearly the same numbers and the measurement is worthless) into
 * a small RGBA16F target on the pipeline's own view band, then a blit into a
 * READ_BACK staging copy that the CPU harvests a few frames later.  The
 * readback is on a cadence (JCE_AE_READBACK, default every 4th apply) because
 * bgfx_read_texture maps the staging texture immediately on D3D11 -- the same
 * hard sync jce_gi_probes.c measured at ~10 ms under load.  Sampling the
 * scene at 15 Hz is ample for something whose time constant is a second.
 *
 * WHAT params.exposure MEANS WHEN THIS IS ON.  It stops being the exposure
 * and becomes a MULTIPLIER on the adapted one, so an artist who dialled in
 * 1.3 keeps their relative offset rather than having it silently ignored.
 *
 * The adaptation LAW -- direction, convergence, the asymmetric speeds, the
 * clamp, log(0) -- is <jce/renderer/jce_auto_exposure.h> and is asserted
 * headlessly there.  Everything here is the measurement and the wiring.
 *
 * `desc` NULL means jce_auto_exposure_desc_default().
 */
JCE_API void jce_postfx_set_auto_exposure(JcePostFXPipeline *pipeline,
                                          bool enabled,
                                          const JceAutoExposureDesc *desc);
JCE_API bool jce_postfx_get_auto_exposure(const JcePostFXPipeline *pipeline);
/* Reads back the description in force; `out` untouched when there is no
 * pipeline.  Used to mirror one pipeline onto another (scene viewport ->
 * game viewport), the same way the custom pass and the tonemap op are. */
JCE_API void jce_postfx_get_auto_exposure_desc(const JcePostFXPipeline *pipeline,
                                               JceAutoExposureDesc *out);

/* The exposure ACTUALLY in use this frame: the adapted value times
 * params.exposure when auto exposure is on, params.exposure when it is off.
 * Exposed because "why is this shot dark" is a question about a number, and
 * without this the only way to answer it is to guess. */
JCE_API float jce_postfx_get_effective_exposure(const JcePostFXPipeline *pipeline);

/* The scene luminance the last COMPLETED readback measured (log-average, a
 * geometric mean -- a handful of blown-out speculars must not drag the whole
 * frame dark), and the adapted EV.  Both are 0 before the first readback
 * lands.  For a HUD/debug readout; the adaptation does not need either. */
JCE_API float jce_postfx_get_measured_luminance(const JcePostFXPipeline *pipeline);
JCE_API float jce_postfx_get_exposure_ev(const JcePostFXPipeline *pipeline);

/* ================================================================== */
/* Temporal Anti-Aliasing (TAA)                                        */
/* ================================================================== */
/*
 * TAA runs as the FIRST link of the chain (before tonemap/bloom/etc.):
 * it resolves the current jittered scene colour against a persistent
 * history buffer using per-pixel camera-reprojection motion vectors, and
 * the resolved colour becomes the input to the remaining chain.
 *
 * Opt-in: nothing runs unless jce_postfx_set_taa(..., enabled=true) was
 * called AND both the fs_taa / fs_motion_vec programs loaded AND a valid
 * scene depth is passed to jce_postfx_apply().  When disabled the history
 * and motion framebuffers are never allocated, so the rest of the chain is
 * byte-identical to a build without TAA.
 *
 * The renderer must, every frame TAA is on:
 *   1) jitter the MAIN colour pass's projection (jce_taa_apply_jitter),
 *   2) push the UN-jittered scene inverse-view-proj + previous-frame
 *      view*proj via jce_postfx_set_taa_matrices(), and
 *   3) jce_postfx_set_taa(enabled=true, ...) before jce_postfx_apply().
 * Motion vectors are jitter-free (they use the un-jittered matrices), so
 * the jitter only sub-pixel-shifts the sampled image, not the reproject.
 */

/* Enable/disable TAA and set its resolve parameters.
 *   feedback     : history blend weight (0.85-0.97 typical; higher = more
 *                  temporal accumulation / softer, lower = sharper/noisier).
 *   luma_clamp   : variance-box softening scale (1.0 default; 0 = hard box).
 *   motion_clamp : how aggressively to drop feedback in high-motion regions
 *                  (1.0 default). */
JCE_API void jce_postfx_set_taa(JcePostFXPipeline *pipeline, bool enabled,
                                float feedback, float luma_clamp,
                                float motion_clamp);

/* Supply the camera matrices the motion-vector pass needs, for the frame
 * about to be resolved:
 *   scene_inv_view_proj : inverse of the UN-jittered scene view*proj used
 *                         for the main colour pass this frame.
 *   prev_view_proj      : the previous frame's UN-jittered view*proj
 *                         (pass the SAME as scene_inv_view_proj's source on
 *                         the first frame; the on-screen test in fs_taa then
 *                         rejects the empty history and outputs ~current). */
JCE_API void jce_postfx_set_taa_matrices(JcePostFXPipeline *pipeline,
                                         const jce_mat4 *scene_inv_view_proj,
                                         const jce_mat4 *prev_view_proj);

/* Supply a STANDARD per-object motion-vector texture for the frame about to be
 * resolved (the scene renderer's velocity G-buffer, encoded identically to
 * fs_motion_vec.sc).  When set to a valid handle, the TAA pass SKIPS its
 * internal camera-only motion-vec pass and reprojects history with THIS texture
 * instead — so moving / animated / skinned geometry stops ghosting.  One-shot:
 * cleared at the end of every jce_postfx_apply().  Pass JCE_TEXTURE_INVALID (or
 * never call it) to fall back to the camera-only reprojection. */
JCE_API void jce_postfx_set_taa_motion_tex(JcePostFXPipeline *pipeline,
                                           JceTextureHandle tex);

/* ================================================================== */
/* Rendering                                                           */
/* ================================================================== */

/* Load the shaders required by the enabled effects.
   Must be called after renderer and shader system are ready.
   pak: the PAK archive containing compiled shader binaries. */
JCE_API bool jce_postfx_load_shaders(JcePostFXPipeline *pipeline,
                             const JcePakArchive *pak);

/* Execute the enabled post-processing chain.
   scene_fb: the framebuffer containing the rendered scene.
   The final result is written to the back buffer. */
JCE_API void jce_postfx_apply(JcePostFXPipeline *pipeline,
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

/* Resolve `src` (rendered at src_w x src_h) into the framebuffer `dst_fb_idx`
 * at out_w x out_h using a contrast-adaptive sharpen (RCAS/FSR1/CAS parity)
 * when upscaling, else a plain copy. `view_id` is a free view slot the caller
 * owns. The runtime present path (jce_postfx_present) sharpens in place; this
 * is the equivalent for consumers that composite into their own offscreen
 * target (the editor Scene View's dynamic-resolution upscale). Orientation is
 * a UV-identity copy (optionally V-flipped for bottom-left-origin backends),
 * so the destination texture reads the same way the source would. Returns true
 * only when it actually sharpened into the destination (upscaling + RCAS
 * program present); returns false without drawing otherwise, so the caller can
 * keep showing its source directly rather than a blank/mis-oriented copy. */
JCE_API bool jce_postfx_upscale_resolve(JcePostFXPipeline *pipeline,
                                        uint16_t view_id,
                                        uint16_t dst_fb_idx,
                                        JceTextureHandle src,
                                        uint32_t src_w, uint32_t src_h,
                                        uint32_t out_w, uint32_t out_h,
                                        bool flip_v);

/* Temporal super-resolution upscale (FSR2/UE-TSR-style), v2. Reconstructs an
 * out_w x out_h image from the render_w x render_h jittered `color` frame by
 * depositing each frame's sub-pixel samples into an internal output-res
 * ping-pong history, REPROJECTED by camera motion so accumulation survives
 * camera movement. The caller renders the scene JITTERED (jitter_u/jitter_v =
 * this frame's jitter offset in render-UV space) and supplies the scene `depth`
 * plus `inv_view_proj` (inverse of the current UN-jittered view*proj) and
 * `prev_view_proj` (previous frame's UN-jittered view*proj) to drive the motion
 * pass; pass depth.idx==UINT16_MAX / NULL matrices for a static-only fallback.
 * `feedback` is the base history weight (~0.9); the shader tapers it by motion
 * speed and drops it on disocclusion. Uses `view_base` for the motion pass and
 * view_base+1 for the resolve. Owns its buffers (lazily at out_w x out_h);
 * returns THIS frame's reconstruction texture idx to display, or UINT16_MAX. */
/* `ext_motion` (optional, UINT16_MAX to omit): a render-res per-object velocity
 * buffer (the scene renderer's gbuffer_vel prepass, RG = (cur-prev)*0.5+0.5) so
 * animated/skinned geometry reprojects too — not just the camera. When omitted,
 * the resolve generates camera-only motion from `depth` + the VP matrices. */
JCE_API uint16_t jce_postfx_tsr_resolve(JcePostFXPipeline *pipeline,
                                        uint16_t view_base,
                                        JceTextureHandle color,
                                        JceTextureHandle depth,
                                        const float *inv_view_proj,
                                        const float *prev_view_proj,
                                        JceTextureHandle ext_motion,
                                        uint32_t render_w, uint32_t render_h,
                                        uint32_t out_w, uint32_t out_h,
                                        float jitter_u, float jitter_v,
                                        float feedback, bool flip_v);

JCE_EXTERN_C_END

#endif /* JCE_POSTFX_H */
