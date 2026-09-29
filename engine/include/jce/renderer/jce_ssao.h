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

    /* ── Contact shadows ────────────────────────────────────────────────
     * A short view-space raymarch toward the sun, written into the GREEN
     * channel of this pass's target (.r stays ambient occlusion).
     *
     * It rides along here rather than in a pass of its own because the PBR
     * fragment shader occupies all 16 sampler stages -- the WebGL2 budget the
     * charter requires -- so a separate mask has nowhere to bind at the tiers
     * the design wants contact shadows on.  This pass already samples depth and
     * already writes an RGBA8 target whose green channel duplicated red.
     *
     * CONSEQUENCE, and it is not hidden: contact shadows exist only when the
     * SSAO pass runs.  Turning SSAO off turns them off.  That coupling is real
     * and is the price of the sampler budget. */
    float cs_steps;         /* 0 disables the march (LOW tier, and default) */
    float cs_ray_length;    /* world units; 0 disables                       */
    float cs_jitter;        /* 1 only where a temporal resolve exists        */
    float cs_sun_view[3];   /* direction TOWARD the sun, VIEW space, unit    */
    float cs_proj_scale[2]; /* (tan(fovY/2)*aspect, tan(fovY/2))             */

    /* ── Cloud shadows ──────────────────────────────────────────────────
     * A top-down transmittance map (jce_cloud_shadow.h) sampled by world XZ
     * and written into the BLUE channel of this pass's target.  Same reason
     * contact shadows ride in green: the PBR fragment shader has all 16
     * sampler stages occupied, and both PBR and terrain already bind this
     * target at stage 3.
     *
     * The alternative the design named -- the directional light cookie -- does
     * not reach terrain at all (fs_terrain.sc has no cookie path), and terrain
     * is where a cloud shadow is most visible. */
    uint16_t cloud_tex;     /* bgfx handle idx; UINT16_MAX = no cloud shadow */
    float    cloud_extent;  /* world size the map covers; 0 = off            */
    float    cloud_center[2];
    float    cloud_strength; /* 0..1 lerp toward full shadow                 */
} JceSsaoParams;

JCE_API JceSsao        *jce_ssao_create(const JceSsaoDesc *desc);
JCE_API void            jce_ssao_destroy(JceSsao *s);

JCE_API void            jce_ssao_resize(JceSsao *s, int width, int height);
JCE_API void            jce_ssao_set_params(JceSsao *s, const JceSsaoParams *p);
JCE_API JceSsaoParams   jce_ssao_default_params(void);

/* Render AO into internal RT. depth_tex must be a bgfx_texture_handle_t
 * value (uint16_t idx). first_view_id reserves 2 sequential view slots
 * (sampling pass + blur pass) -- one view id each, both named by the caller.
 *
 * view/proj are the matrices this frame was rendered with.  They are set on
 * the view so bgfx publishes u_invViewProj, which the contact-shadow march and
 * the cloud-shadow lookup both need -- the latter is a WORLD-space map, and
 * without the inverse there is no way back from a depth sample to a world XZ.
 * NULL for either falls back to identity, which disables anything that needs
 * world space rather than reconstructing garbage positions. */
/* TWO view ids, named, not one and an implied neighbour.
 *
 * This took a single `first_view_id` and silently used that one AND the next.
 * The scene renderer passed base+2, so SSAO owned base+2 and base+3 -- and
 * base+3 was declared "free" in an engine-private comment and handed to the
 * GPU-cull counter reset, which the order builder pushes FIRST.  With SSAO and
 * r.gpu_driven both on, SSAO's blur therefore sorted ahead of SSAO's own
 * sample pass.  A view id a function consumes without naming is a view id
 * nobody else can see it consuming. */
JCE_API void            jce_ssao_render(JceSsao *s,
                                         uint16_t depth_tex_handle,
                                         const jce_mat4 *view,
                                         const jce_mat4 *proj,
                                         uint16_t sample_view_id,
                                         uint16_t blur_view_id);

/* Returns texture handle (uint16_t) of the final blurred AO RT, or
 * UINT16_MAX if not yet rendered.  Sample as RGBA8, .r = occlusion. */
JCE_API uint16_t        jce_ssao_get_result_texture(const JceSsao *s);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SSAO_H */
