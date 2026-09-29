/*
 * jce_ssgi.h -- screen-space global illumination: one diffuse bounce.
 *
 * The lit colour buffer already holds, at every pixel, the radiance that
 * surface sends toward the camera.  A diffuse receiver gathers that radiance
 * over its hemisphere, and the screen is a partial sample of it.  This module
 * marches short rays through the depth buffer, collects what they hit, and
 * hands back an RT the caller adds onto the scene colour -- red light off a
 * red wall, sky bounce under an overhang, the colour a probe grid is too
 * coarse to carry.
 *
 * SAME SHAPE AS jce_ssr.h ON PURPOSE.  Both take (colour, depth, normal) and
 * produce one RT the caller composites; both claim one view id to march and
 * one to composite.  The scene renderer already assembles those three inputs
 * for SSR and SSAO, so this adds a pass and no plumbing.
 *
 * WHAT IT IS NOT.  It is not a replacement for probes or the sky IBL: light
 * from a surface that is off-screen, behind the camera, or facing away is
 * simply not in the buffer, so this ADDS to the existing indirect term
 * instead of standing in for it.  Unity's SSGI, UE's Lumen screen traces and
 * Godot's SSIL all carry the same caveat and for the same reason.
 *
 * IT IS DENOISED, SPATIALLY AND ONLY SPATIALLY.  Four rays per pixel is a
 * noisy estimator, so the composite gathers the bounce RT through a joint
 * bilateral kernel guided by the same depth and normal the march used --
 * neighbours on the same surface average, neighbours across an edge do not.
 * It is in the composite rather than in a pass of its own because SSGI owns
 * exactly two view ids; see fs_ssgi_composite.sc for that and for why hit
 * coverage is deliberately NOT one of the weights.
 *
 * There is no TEMPORAL half, and that is a constraint rather than an omission:
 * the ray pattern is a stable function of the pixel (no frame counter -- see
 * the shader's DETERMINISM note) precisely so two runs of one frame are
 * byte-identical, which is what this tree's capture harness compares.  Unity's
 * SSGI and UE's Lumen both accumulate over frames; that is still ahead of this.
 *
 * Thread-safety: single-threaded, like JceSsr -- create/destroy/resize/render
 * on the bgfx-owning thread.
 *
 * Layer: renderer (Layer 3) -- public.
 */
#ifndef JCE_SSGI_H
#define JCE_SSGI_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_gfx_types.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JcePakArchive JcePakArchive;
typedef struct JceSsgi       JceSsgi;

typedef struct {
    const JcePakArchive *pak;
    int                  width;
    int                  height;
} JceSsgiDesc;

typedef struct {
    /* World-space length of one gather ray.  This is the bounce distance: a
     * small radius gives tight contact bleed, a large one gathers across a
     * room and costs the same (the step count is fixed, the steps get longer
     * and the estimate gets coarser). */
    float radius;
    /* Depth-test tolerance for calling a march step a hit, in linear world
     * units.  Too small and rays pass through thin geometry; too large and a
     * distant surface behind a foreground object counts as a hit. */
    float thickness;
    /* Rays per pixel, 1..4.  Clamped: the shader's loop bound is fixed and
     * SMALL.  A dynamic bound is a divergent loop on every backend here, and
     * the compiler unrolls a fixed one -- at 8 rays x 16 steps fxc ground for
     * minutes and the unrolled body would have issued up to 384 texture
     * fetches per pixel, against a stated integrated-GPU baseline. */
    float ray_count;
    /* March steps per ray, 1..8.  Same reason; 4x8 = 32 fetches, the same
     * order as the SSR march. */
    float step_count;
    /* Multiplier on the gathered radiance.  1.0 is the physically-shaped
     * value; the default is lower because the receiver-colour proxy for
     * albedo (see the shader header) over-counts on bright surfaces. */
    float intensity;
    float near_plane;
    float far_plane;
} JceSsgiParams;

JCE_API JceSsgi       *jce_ssgi_create(const JceSsgiDesc *desc);
JCE_API void           jce_ssgi_destroy(JceSsgi *s);

JCE_API void           jce_ssgi_resize(JceSsgi *s, int width, int height);
JCE_API void           jce_ssgi_set_params(JceSsgi *s, const JceSsgiParams *p);
JCE_API JceSsgiParams  jce_ssgi_default_params(void);

/* March the bounce into the internal RT.
 *   color/depth/normal : TYPED handles, not raw indices -- see the note on
 *                        JceFrameBufferHandle.  `normal` must be the depth
 *                        pre-pass's normal target (rgb = world normal * 0.5 +
 *                        0.5), NOT the depth texture: unlike SSR, which
 *                        reconstructs normals and is handed depth twice, a
 *                        diffuse gather needs the receiver's real normal or
 *                        the hemisphere is wrong.
 *   view/proj          : the matrices the main render used.
 *   first_view_id      : claims exactly ONE view. */
JCE_API void           jce_ssgi_render(JceSsgi *s,
                                       JceTextureHandle color,
                                       JceTextureHandle depth,
                                       JceTextureHandle normal,
                                       const jce_mat4 *view,
                                       const jce_mat4 *proj,
                                       uint16_t first_view_id);

/* jce_ssgi_render, plus the receiver's ALBEDO.
 *
 * The bounce is albedo/PI * SUM(L_i cos).  Without an albedo target the
 * receiver's LIT COLOUR stood in for its albedo and counted the direct
 * lighting twice -- a brightly lit surface gathered more bounce than it
 * should, brightest exactly where the error shows most.  `albedo` is the
 * depth pre-pass's attachment 1: the material's base-colour FACTOR.
 *
 * An INVALID albedo handle binds the colour target in its place, which makes
 * this byte-identical to jce_ssgi_render above -- that is how the old entry
 * point keeps its behaviour without a branch in the shader, and it is also
 * the honest fallback for a caller that has no such target. */
JCE_API void           jce_ssgi_render_albedo(JceSsgi *s,
                                              JceTextureHandle color,
                                              JceTextureHandle depth,
                                              JceTextureHandle normal,
                                              JceTextureHandle albedo,
                                              const jce_mat4 *view,
                                              const jce_mat4 *proj,
                                              uint16_t first_view_id);

JCE_API JceTextureHandle jce_ssgi_get_result_texture(const JceSsgi *s);

/* Denoise the bounce RT and add it onto a destination colour framebuffer.
 * `view_id` must be strictly greater than the march view so the RT is filled
 * first.  The filter's depth/normal guides are the ones jce_ssgi_render was
 * given, remembered internally -- so this stays a two-argument composite and
 * the editor and the shipped runtime cannot pass it different inputs.  Without
 * a preceding successful march it degrades to an unguided gather, never to
 * garbage. */
JCE_API void           jce_ssgi_composite(JceSsgi *s, uint16_t view_id,
                                          JceFrameBufferHandle dst);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SSGI_H */
