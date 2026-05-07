/*
 * jce_volumetric_fog.h -- analytic + raymarched homogeneous volumetric
 * fog over the depth buffer.
 *
 * Output RT (RGBA8): rgb = in-scattered light, a = transmittance.
 * Caller composites:  final = scene.rgb * a + rgb;
 *
 * Knows nothing about light sources — uses a single "fog color" param
 * which acts as a bulk in-scattering term.  Suitable for atmospheric
 * haze, distance fog, low-lying fog (height_falloff > 0), god-ray
 * fakes when combined with a separate shadow-aware multiplier.
 *
 * Thread-safety: single-threaded; call from the bgfx-owning thread.
 *
 * Example:
 *   JceVolumetricFogDesc desc = { pak, screen_w, screen_h };
 *   JceVolumetricFog *fog = jce_volumetric_fog_create(&desc);
 *   JceVolumetricFogParams p = jce_volumetric_fog_default_params();
 *   p.density = 0.04f; p.color_r = 0.7f; p.color_g = 0.7f; p.color_b = 0.8f;
 *   jce_volumetric_fog_set_params(fog, &p);
 *   jce_volumetric_fog_render(fog, depth_tex.idx, &view, &proj, view_id);
 *   uint16_t fog_idx = jce_volumetric_fog_get_result_texture(fog);
 *   // composite: final = scene * fog.a + fog.rgb
 *
 * Layer: renderer (Layer 3) — public.
 */
#ifndef JCE_VOLUMETRIC_FOG_H
#define JCE_VOLUMETRIC_FOG_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JcePakArchive    JcePakArchive;
typedef struct JceVolumetricFog JceVolumetricFog;

typedef struct {
    const JcePakArchive *pak;
    int                  width;
    int                  height;
} JceVolumetricFogDesc;

typedef struct {
    float density;          /* base extinction at height_origin, default 0.02 */
    float scattering;       /* in-scatter strength,             default 1.0  */
    float height_falloff;   /* 1/m exp falloff above origin,    default 0.05 */
    float height_origin;    /* world-space y of dense layer,    default 0.0  */
    float max_distance;     /* clamp ray length,                default 800  */
    float step_count;       /* march samples (clamped 64),      default 32   */
    float near_plane;
    float far_plane;
    float color_r, color_g, color_b;
    float ambient_lift;     /* added to scattering term,        default 0.05 */
} JceVolumetricFogParams;

JCE_API JceVolumetricFog       *jce_volumetric_fog_create(const JceVolumetricFogDesc *desc);
JCE_API void                    jce_volumetric_fog_destroy(JceVolumetricFog *f);

JCE_API void                    jce_volumetric_fog_resize(JceVolumetricFog *f, int width, int height);
JCE_API void                    jce_volumetric_fog_set_params(JceVolumetricFog *f, const JceVolumetricFogParams *p);
JCE_API JceVolumetricFogParams  jce_volumetric_fog_default_params(void);

/* Render fog into internal RT.
 *   depth_tex_handle : bgfx texture handle .idx of scene depth
 *   view/proj        : same matrices used to render the scene
 *   first_view_id    : reserves 1 view slot
 */
JCE_API void                    jce_volumetric_fog_render(JceVolumetricFog *f,
                                                          uint16_t depth_tex_handle,
                                                          const jce_mat4 *view,
                                                          const jce_mat4 *proj,
                                                          uint16_t first_view_id);

JCE_API uint16_t                jce_volumetric_fog_get_result_texture(const JceVolumetricFog *f);

/* Composite the most recently rendered fog RT into the destination
 * frame buffer using blend equation:
 *   dst.rgb = dst.rgb * fog.a + fog.rgb
 * Submits one fullscreen quad on `view_id`, leaving the bound view
 * frame buffer to be configured by the caller (or BGFX_INVALID_HANDLE
 * to draw to the back buffer).  Caller is expected to set the view's
 * frame buffer + viewport before calling.
 *
 * No-op when fog was not rendered this frame, or the program failed
 * to load. */
JCE_API void                    jce_volumetric_fog_composite(JceVolumetricFog *f,
                                                             uint16_t view_id);

#ifdef __cplusplus
}
#endif

#endif /* JCE_VOLUMETRIC_FOG_H */
