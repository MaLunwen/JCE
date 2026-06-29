/*
 * jce_render_pipeline.h  Unity-style Render Pipeline Asset.
 *
 * A single JSON descriptor (`.rp.json`) that turns rendering features
 * on or off and dials in quality knobs.  Games (and platforms) bind
 * one at engine boot — analogous to URP/HDRP Asset selection in Unity.
 *
 * The descriptor pairs cleanly with the GPU tier (`jce_renderer_caps`):
 *   - LOW    preset honours the 512 MB / no-discrete-GPU baseline.
 *   - MID    sane defaults for integrated GPUs.
 *   - HIGH   modern desktop discrete.
 *   - ULTRA  everything on; reserved for high-end profiles.
 *
 * Boot order:
 *   1. Engine init calls jce_render_pipeline_apply_boot(path).
 *   2. If `path` exists on the host filesystem it is loaded and applied.
 *   3. Otherwise the preset matching jce_renderer_get_tier() is used.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_RENDER_PIPELINE_H
#define JCE_RENDER_PIPELINE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum JceRpQuality {
    JCE_RP_QUALITY_LOW   = 0,
    JCE_RP_QUALITY_MID   = 1,
    JCE_RP_QUALITY_HIGH  = 2,
    JCE_RP_QUALITY_ULTRA = 3
} JceRpQuality;

typedef struct JceRenderPipelineDesc {
    /* Features */
    bool enable_csm;            /* cascaded shadow maps        */
    bool enable_ssao;
    bool enable_ssr;
    bool enable_taa;
    bool enable_bloom;
    bool enable_volumetric_fog;
    bool enable_gpu_particles;
    bool enable_motion_blur;
    bool enable_cloth;          /* P3-C.4: soft-body / cloth sim */
    bool enable_stylized_sky;   /* stylized sky dome (mode 3); LOW tier off */

    /* Quality knobs */
    uint16_t shadow_resolution; /* 512 / 1024 / 2048 / 4096    */
    uint8_t  csm_cascade_count; /* 1..4                        */
    /* Shadow FILTER cost tier (per-pixel tap count, not map size):
     *   0 = 1 tap, cascade blend off  (local + CSM hard shadows)
     *   1 = 3x3 PCF everywhere
     *   2 = full (local 3x3, CSM rotated 5x5 + cascade blending)
     * Worst case per pixel under 4 shadowed local lights drops from
     * ~86 taps (tier 2) to ~5 (tier 0) — the dominant fragment cost
     * on the 2008-2010 iGPU baseline. Consumed by the scene renderer
     * via the u_shadowQuality uniform (frame-constant branch in
     * fs_pbr/fs_terrain — no shader permutations). */
    uint8_t  shadow_filter_quality;
    uint8_t  msaa_samples;      /* 1, 2, 4, 8                  */
    float    render_scale;      /* 0.25..2.0; 1.0 = native     */
    JceRpQuality post_quality;

    /* Targets / format */
    bool hdr_color;             /* true = R11G11B10F, false = RGBA8 */
    bool depth_prepass;
} JceRenderPipelineDesc;

/* ── Apply / query ─────────────────────────────────────────────── */

/* Apply the descriptor.  Calls the per-feature toggles below and
 * caches the result for jce_render_pipeline_get() / feature queries.
 * NULL is treated as the LOW preset. */
JCE_API void jce_render_pipeline_apply(const JceRenderPipelineDesc *desc);

/* Copy the currently-applied descriptor (or LOW preset on cold call). */
JCE_API void jce_render_pipeline_get(JceRenderPipelineDesc *out);

/* Per-feature query — render-graph code can gate passes off this. */
JCE_API bool jce_render_pipeline_is_feature_enabled(const char *feature);

/* ── P4-E.2: deferred feature toggle ─────────────────────────────
 * jce_render_pipeline_set_feature_enabled() writes into a shadow
 * (pending) descriptor so mid-frame changes don't race with in-flight
 * draw calls.  jce_render_pipeline_end_frame() promotes the pending
 * descriptor to the active one at a safe point (end of frame) and fires
 * the observer if anything changed.  Callers that update the full
 * descriptor at once should continue to use jce_render_pipeline_apply(). */
JCE_API void jce_render_pipeline_set_feature_enabled(const char *feature, bool enabled);
JCE_API void jce_render_pipeline_end_frame(void);

/* ── P3-C.4: observer hook ─────────────────────────────────────────
 * Upper-layer subsystems (e.g. cloth/soft-body) register a callback
 * fired whenever the pipeline asset is applied.  Lets them honour
 * HW-tier feature flags without forcing the renderer layer to take
 * a downward dependency on middleware. */
typedef void (*JceRenderPipelineApplyFn)(const JceRenderPipelineDesc *desc,
                                         void *ud);
JCE_API void jce_render_pipeline_set_observer(JceRenderPipelineApplyFn fn,
                                              void *ud);

/* ── JSON I/O ──────────────────────────────────────────────────── */

/* Read `.rp.json` from the host filesystem.  On failure `out` is set
 * to the LOW preset and false is returned. */
JCE_API bool jce_render_pipeline_load(const char *host_path,
                                      JceRenderPipelineDesc *out);

/* Pretty-print `desc` to `host_path`. */
JCE_API bool jce_render_pipeline_save(const char *host_path,
                                      const JceRenderPipelineDesc *desc);

/* ── Built-in presets ──────────────────────────────────────────── */

JCE_API void jce_render_pipeline_preset_low  (JceRenderPipelineDesc *out);
JCE_API void jce_render_pipeline_preset_mid  (JceRenderPipelineDesc *out);
JCE_API void jce_render_pipeline_preset_high (JceRenderPipelineDesc *out);
JCE_API void jce_render_pipeline_preset_ultra(JceRenderPipelineDesc *out);

/* Convenience: write `out` with the preset matching the current
 * (effective) GPU tier reported by jce_renderer_get_tier(). */
JCE_API void jce_render_pipeline_preset_for_current_tier(
    JceRenderPipelineDesc *out);

/* ── Boot helper ───────────────────────────────────────────────── */

/* Engine-boot autopick: load `host_path` if present, else pick the
 * tier preset.  Logs which path was taken via jce_log_info().
 * Always applies the result; never fails (worst case is LOW preset). */
JCE_API void jce_render_pipeline_apply_boot(const char *host_path);

JCE_EXTERN_C_END

#endif /* JCE_RENDER_PIPELINE_H */
