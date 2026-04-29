/*
 * jce_ssr.h -- screen-space reflections module.
 *
 * Pipeline:
 *   1. Caller renders main scene and produces:
 *        - color   buffer  (lit, RGBA)
 *        - depth   buffer  (non-linear, sampled)
 *        - normal  buffer  (world-space, RGBA8 encoded as xyz*0.5+0.5)
 *   2. Caller invokes jce_ssr_render(ssr, color, depth, normal, view,
 *      proj, first_view_id). Module produces a single RGBA RT containing
 *      the screen-space reflection result with edge-fade in alpha.
 *   3. Caller composites by sampling jce_ssr_get_result_texture() and
 *      blending into the final color (typically lerp by surface
 *      reflectivity * result.a).
 *
 * Generic — knows nothing about materials.  A subsequent shading pass
 * decides which pixels actually deserve reflections (mirrors, water,
 * polished floors) based on its own roughness/metalness.
 *
 * Thread-safety: a JceSsr instance is single-threaded — call create
 * / destroy / resize / render from the bgfx-owning thread (typically
 * the render thread).  bgfx itself serializes the GPU submission.
 *
 * Example:
 *   JceSsrDesc desc = { pak, screen_w, screen_h };
 *   JceSsr *ssr = jce_ssr_create(&desc);
 *   ...
 *   // each frame, after the lit/depth/normal G-buffer is filled:
 *   jce_ssr_render(ssr, color_tex.idx, depth_tex.idx, normal_tex.idx,
 *                  &view, &proj, view_id_for_ssr);
 *   uint16_t refl_idx = jce_ssr_get_result_texture(ssr);
 *   // composite refl_idx into final color in your shading pass.
 *   jce_ssr_destroy(ssr);
 *
 * Layer: renderer (Layer 3) — public.
 */
#ifndef JCE_SSR_H
#define JCE_SSR_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JcePakArchive JcePakArchive;
typedef struct JceSsr        JceSsr;

typedef struct {
    const JcePakArchive *pak;
    int                  width;
    int                  height;
} JceSsrDesc;

typedef struct {
    float max_distance;     /* world-space ray length, default 50.0  */
    float thickness;        /* depth-test tolerance,  default 0.5    */
    float step_count;       /* march iteration cap,   default 32     */
    float intensity;        /* result multiplier,     default 1.0    */
    float near_plane;
    float far_plane;
} JceSsrParams;

JCE_API JceSsr        *jce_ssr_create(const JceSsrDesc *desc);
JCE_API void           jce_ssr_destroy(JceSsr *s);

JCE_API void           jce_ssr_resize(JceSsr *s, int width, int height);
JCE_API void           jce_ssr_set_params(JceSsr *s, const JceSsrParams *p);
JCE_API JceSsrParams   jce_ssr_default_params(void);

/* Render reflections into internal RT.
 *   color_tex/depth_tex/normal_tex : bgfx texture handle .idx values
 *   view/proj                      : 4x4 matrices used by the main render
 *   first_view_id                  : reserves 1 view slot
 */
JCE_API void           jce_ssr_render(JceSsr *s,
                                      uint16_t color_tex_handle,
                                      uint16_t depth_tex_handle,
                                      uint16_t normal_tex_handle,
                                      const jce_mat4 *view,
                                      const jce_mat4 *proj,
                                      uint16_t first_view_id);

JCE_API uint16_t       jce_ssr_get_result_texture(const JceSsr *s);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SSR_H */
