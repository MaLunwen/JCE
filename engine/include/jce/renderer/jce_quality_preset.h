/*
 * jce_quality_preset.h  Quality-settings tier presets.
 *
 * Extends JceRenderPipelineDesc with additional knobs (mip LOD bias,
 * max draw distance, max active dynamic lights) and exposes four named
 * tiers — LOW, MED, HIGH, ULTRA — that map to the GPU-tier baseline.
 *
 *   LOW   — 2008/2010 baseline: single-core, 512 MB, integrated GPU.
 *   MED   — integrated GPU with decent driver (Intel Iris, Vega iGPU).
 *   HIGH  — modern discrete GPU (GTX 1060 / RX 580 class).
 *   ULTRA — high-end discrete; throw everything on.
 *
 * Call jce_quality_preset_apply() to simultaneously push the RP desc
 * to the renderer and cache the extra fields.  Query the cached extras
 * via the jce_quality_get_*() helpers.
 *
 * Layer: renderer (L3) — public.
 */

#ifndef JCE_QUALITY_PRESET_H
#define JCE_QUALITY_PRESET_H

#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_render_pipeline.h>

JCE_EXTERN_C_BEGIN

/* Quality tier enum mirrors JceRpQuality for convenience. */
typedef enum JceQualityTier {
    JCE_QUALITY_LOW   = JCE_RP_QUALITY_LOW,
    JCE_QUALITY_MED   = JCE_RP_QUALITY_MID,
    JCE_QUALITY_HIGH  = JCE_RP_QUALITY_HIGH,
    JCE_QUALITY_ULTRA = JCE_RP_QUALITY_ULTRA
} JceQualityTier;

/* Quality preset: RP pipeline descriptor + additional knobs. */
typedef struct {
    JceRenderPipelineDesc rp;          /* embedded RP desc (all RP features) */
    float mip_lod_bias;                /* negative = sharper; 0.0 = neutral  */
    float max_draw_distance;           /* scene draw distance cap (metres)    */
    int   max_active_lights;           /* max dynamic lights per cluster      */
} JceQualityPreset;

/* Fill `out` with the built-in preset for `tier`. */
JCE_API void JCE_CALL jce_quality_preset_get(JceQualityTier tier,
                                              JceQualityPreset *out);

/* Apply a preset: pushes rp to the renderer and caches the extra fields.
   Equivalent to jce_render_pipeline_apply(&preset->rp) plus caching. */
JCE_API void JCE_CALL jce_quality_preset_apply(const JceQualityPreset *preset);

/* Query the cached extra fields (set by the last jce_quality_preset_apply). */
JCE_API float JCE_CALL jce_quality_get_mip_lod_bias(void);
JCE_API float JCE_CALL jce_quality_get_max_draw_distance(void);
JCE_API int   JCE_CALL jce_quality_get_max_active_lights(void);

JCE_EXTERN_C_END

#endif /* JCE_QUALITY_PRESET_H */
