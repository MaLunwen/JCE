/*
 * jce_quality_apply.c  Plug active quality preset into the render
 * subsystems.
 *
 * Shader keyword toggling: defers to jce_shader_variant.h.  When
 * that module isn't present (legacy build), the apply call no-ops.
 */

#include <jce/middleware/scene/jce_quality_apply.h>

#include <string.h>

/* Forward decl: shader variant API.  We do NOT include the real
 * header here because jce_shader_variant.h is in a heavier compile
 * graph; the host build links these symbols for real. */
extern void jce_shader_variant_set_keyword(const char *name, bool enabled);

void jce_quality_apply_active_keywords(void)
{
    const JceQualityLevel *lv = jce_quality_active_level();
    if (!lv) return;
    for (uint32_t i = 0; i < lv->keyword_count; ++i)
        jce_shader_variant_set_keyword(lv->keywords[i], true);
}

void jce_quality_apply_get_knobs(JceQualityKnobs *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    const JceQualityLevel *lv = jce_quality_active_level();
    if (!lv) {
        /* Sane defaults. */
        out->shadow_resolution = 1024;
        out->shadow_distance   = 50.0f;
        out->msaa              = 0;
        out->vsync_mode        = 1;
        out->pixel_light_count = 4;
        out->lod_bias          = 1.0f;
        out->render_scale      = 1.0f;
        return;
    }
    out->shadow_resolution = lv->shadow_resolution;
    out->shadow_distance   = lv->shadow_distance;
    out->msaa              = lv->msaa;
    out->vsync_mode        = lv->vsync_mode;
    out->pixel_light_count = lv->pixel_light_count;
    out->lod_bias          = lv->lod_bias;
    out->render_scale      = lv->render_scale;
}
