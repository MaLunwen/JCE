/* Current-frame physical clouds for transparent water below the full layer. */
#ifndef JCE_SR_WATER_CLOUDS_H
#define JCE_SR_WATER_CLOUDS_H

#include "jce_sr_internal.h"
#include <math.h>

static void sr_water_clouds_bind(JceSceneRenderer *sr, const JceScene *scene,
    const JceCamera *camera, uint16_t view, bgfx_texture_handle_t *texture,
    float params[4], jce_mat4 *view_proj)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!camera || !caps || sr->sky_mode != JCE_SCENE_SKY_PHYSICAL ||
        !BGFX_HANDLE_IS_VALID(sr->u_sky_stylise) ||
        !BGFX_HANDLE_IS_VALID(sr->u_sky_tint_shadow) ||
        !BGFX_HANDLE_IS_VALID(sr->u_sky_tint_mid) ||
        !BGFX_HANDLE_IS_VALID(sr->u_sky_tint_high)) return;
    const float bottom = sr->cloud_bottom_km > 0.0f ? sr->cloud_bottom_km : 1.5f;
    const float top = sr->cloud_top_km > bottom ? sr->cloud_top_km : 4.0f;
    // Below-layer water views do not need a cloud read. The per-pixel shader
    // proves the complete shell interval is before the actual water surface.
    if (!(jce_camera_get_position(camera).y > bottom * 1000.0f)) return;
    const JceSkyStylise *style = &sr->sky_stylise;
    // Posterisation and nonconstant palette grades do not commute with
    // radiance composition. Retain the existing path for those authored skies.
    if (style->bands > 1) return;
    for (unsigned i = 0; i < 3; ++i)
        if (style->shadow_tint[i] != style->mid_tint[i] ||
            style->mid_tint[i] != style->highlight_tint[i]) return;
    uint16_t base = view;
    if (view == JCE_VIEW_EDITOR_SCENE + JCE_VIEW_SR_TRANSPARENCY_OFFSET ||
        view == JCE_VIEW_EDITOR_GAME + JCE_VIEW_SR_TRANSPARENCY_OFFSET ||
        view == JCE_VIEW_RUNTIME_GAME + JCE_VIEW_SR_TRANSPARENCY_OFFSET)
        base = (uint16_t)(view - JCE_VIEW_SR_TRANSPARENCY_OFFSET);
    float exposure = 0.0f;
    bool opaque_composited = false;
    const bgfx_texture_handle_t cloud = sr_cloud_temporal_surface(
        sr, scene, camera, base, view_proj, &exposure, &opaque_composited);
    if (!BGFX_HANDLE_IS_VALID(cloud) || !isfinite(exposure) || exposure <= 0.0f)
        return;
    *texture = cloud;
    params[0] = (opaque_composited ? -3.0f : -1.0f) - (caps->originBottomLeft ? 1.0f : 0.0f);
    params[1] = exposure;
    params[2] = bottom;
    params[3] = top;
    const bool grade = !jce_sky_stylise_is_identity(style);
    const float control[4] = {grade ? (float)style->bands : 0.0f,
        grade ? style->rim_strength : 0.0f, grade ? style->saturation : 1.0f,
        grade ? 1.0f : 0.0f};
    const float tint[4] = {style->shadow_tint[0], style->shadow_tint[1],
                            style->shadow_tint[2], 0.0f};
    bgfx_set_uniform(sr->u_sky_stylise, control, 1);
    bgfx_set_uniform(sr->u_sky_tint_shadow, tint, 1);
    bgfx_set_uniform(sr->u_sky_tint_mid, tint, 1);
    bgfx_set_uniform(sr->u_sky_tint_high, tint, 1);
}

#endif
