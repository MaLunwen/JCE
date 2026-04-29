/*
 * jce_ssao.h -- screen-space ambient occlusion module.
 *
 * Pipeline:
 *   1. Caller renders main scene as usual; depth buffer is preserved.
 *   2. Caller invokes jce_ssao_render(ssao, depth_tex, w, h, view,
 *      proj, near, far) — module performs:
 *        a) Hemispherical AO sampling pass (16 taps, jittered)
 *        b) 5x5 box blur to smooth dithering
 *      Result is stored in an internal RT.
 *   3. Caller composes by sampling jce_ssao_get_result_texture() and
 *      multiplying with main color (lit) buffer in their own postfx
 *      composite pass.
 *
 * No depth buffer normal pre-pass required — normals are reconstructed
 * from depth derivatives in the AO shader.  Quality is sufficient for
 * mid-frequency contact occlusion (corners, crevices); for production-
 * quality directional AO use a dedicated normal pass later.
 *
 * Layer: renderer (Layer 3) — public.
 */
#ifndef JCE_SSAO_H
#define JCE_SSAO_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JcePakArchive JcePakArchive;
typedef struct JceSsao       JceSsao;

typedef struct {
    const JcePakArchive *pak;
    int                  width;
    int                  height;
} JceSsaoDesc;

typedef struct {
    float radius;       /* world-space radius scale (default 1.0) */
    float bias;         /* depth-difference cut-off (default 0.005) */
    float intensity;    /* 0..2, default 1.5 */
    float near_plane;
    float far_plane;
} JceSsaoParams;

JCE_API JceSsao        *jce_ssao_create(const JceSsaoDesc *desc);
JCE_API void            jce_ssao_destroy(JceSsao *s);

JCE_API void            jce_ssao_resize(JceSsao *s, int width, int height);
JCE_API void            jce_ssao_set_params(JceSsao *s, const JceSsaoParams *p);
JCE_API JceSsaoParams   jce_ssao_default_params(void);

/* Render AO into internal RT. depth_tex must be a bgfx_texture_handle_t
 * value (uint16_t idx). first_view_id reserves 2 sequential view slots
 * (sampling pass + blur pass). */
JCE_API void            jce_ssao_render(JceSsao *s,
                                         uint16_t depth_tex_handle,
                                         uint16_t first_view_id);

/* Returns texture handle (uint16_t) of the final blurred AO RT, or
 * UINT16_MAX if not yet rendered.  Sample as RGBA8, .r = occlusion. */
JCE_API uint16_t        jce_ssao_get_result_texture(const JceSsao *s);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SSAO_H */
