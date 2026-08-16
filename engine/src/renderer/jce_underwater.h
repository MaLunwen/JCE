/*
 * jce_underwater.h -- Beer-Lambert absorption for a submerged camera.
 *
 * The water surface shader absorbs what is BEHIND the surface, which covers
 * looking into water from above.  It cannot cover looking out from inside:
 * that shader only runs where the water geometry rasterises, and underwater
 * that is the patch of surface overhead.  The sea floor, the rocks beside you,
 * the whole rest of the frame has no water polygon in front of it and would
 * render bone dry while the camera is submerged -- which does not look like a
 * missing feature, it looks like the water has no inside.
 *
 * Two submits, because the result is per-channel:
 *
 *     dst = dst * T(rgb) + tint * (1 - T(rgb))
 *
 * One fixed-function blend cannot express that; there is a single source colour
 * and it would have to be both T and the tint at once.  Splitting it into
 * `dst *= T` then `dst += tint*(1-T)` is exact, needs no read-back of the
 * target being written, and costs two trivial fullscreen quads.
 *
 * Layer: Renderer (L3).
 */

#ifndef JCE_UNDERWATER_H
#define JCE_UNDERWATER_H

#include <stdbool.h>
#include <stdint.h>

#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

typedef struct JcePakArchive JcePakArchive;
typedef struct JceUnderwater JceUnderwater;

typedef struct {
    float sigma[3];    /* per-channel extinction, 1/m                        */
    float tint[3];     /* colour the view converges to with distance         */
    float near_plane;
    float far_plane;
    float max_path;    /* metres; beyond this the medium is saturated        */
} JceUnderwaterParams;

JCE_API JceUnderwater *jce_underwater_create(const JcePakArchive *pak);
JCE_API void           jce_underwater_destroy(JceUnderwater *u);

/* Apply absorption to whatever is already in `dst_fb_idx`.
 *
 * `view_id` must come after the pass that produced the colour, and reserves
 * ONE view: both submits share it, ordered by BGFX_VIEW_MODE_SEQUENTIAL --
 * the multiply must land before the add, and any other order silently
 * produces `(dst + tint*(1-T)) * T`, which is dimmer and looks like a
 * plausible tuning of the same effect. */
JCE_API void jce_underwater_render(JceUnderwater *u,
                                   uint16_t depth_tex_handle,
                                   uint16_t dst_fb_idx,
                                   uint16_t width, uint16_t height,
                                   const JceUnderwaterParams *p,
                                   uint16_t view_id);

JCE_EXTERN_C_END

#endif /* JCE_UNDERWATER_H */
