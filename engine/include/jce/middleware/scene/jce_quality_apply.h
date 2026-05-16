/*
 * jce_quality_apply.h  Push the active quality level's shader
 * keyword preset through jce_shader_variant.
 *
 * Layer: scene (Layer 4) — public.
 */

#ifndef JCE_QUALITY_APPLY_H
#define JCE_QUALITY_APPLY_H

#include <jce/middleware/scene/jce_quality_settings.h>

JCE_EXTERN_C_BEGIN

/* Walk the active level's keywords[] and toggle each one in the
 * shader-variant manager.  No bgfx; the variant manager owns its
 * own bitmask. */
JCE_API void jce_quality_apply_active_keywords(void);

/* Apply the active level's render knobs to a JceQualityKnobs out
 * struct (shadow_resolution, msaa, render_scale, etc.) — callers
 * read from it to drive their own bgfx RT setup. */
typedef struct {
    uint32_t shadow_resolution;
    float    shadow_distance;
    uint8_t  msaa;
    uint8_t  vsync_mode;
    uint8_t  pixel_light_count;
    float    lod_bias;
    float    render_scale;
} JceQualityKnobs;

JCE_API void jce_quality_apply_get_knobs(JceQualityKnobs *out);

JCE_EXTERN_C_END

#endif /* JCE_QUALITY_APPLY_H */
