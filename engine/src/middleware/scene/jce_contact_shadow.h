/*
 * jce_contact_shadow.h -- screen-space contact shadows: march parameters.
 *
 * Contact shadows are the industrial answer to peter-panning.  A shadow map
 * needs a large normal-offset bias to kill acne, and that bias is exactly what
 * detaches a shadow from the object casting it -- the object floats.  A short
 * view-space raymarch against the depth buffer reattaches the contact point at
 * full screen resolution, independent of shadow-map texel size, and picks up
 * small-scale detail no affordable cascade reaches.
 *
 * WHERE IT LIVES, and why that is the whole design:
 *
 * The obvious implementation is a new fullscreen pass writing a new mask into a
 * new sampler.  This engine cannot do that.  The PBR fragment shader already
 * occupies all 16 sampler stages, which is the WebGL2 budget the project
 * charter requires -- so "one extra sampler" is not available at any tier that
 * has to run on the web.
 *
 * Instead the march runs inside the SSAO pass and writes the GREEN channel of
 * the render target SSAO already produces (RGBA8, of which only red was used).
 * That costs no sampler, no render target, no view id -- and view ids are worth
 * avoiding for their own reasons in this renderer.  It also comes out right
 * conceptually: both are screen-space visibility terms computed from depth
 * before shading, and they differ only in which light they attenuate.  AO
 * multiplies the ambient term; contact shadow multiplies the sun.
 *
 * Layer: Scene (L4) -- PRIVATE.  Pure policy, no bgfx.
 */

#ifndef JCE_CONTACT_SHADOW_H
#define JCE_CONTACT_SHADOW_H

#include <stdbool.h>
#include <stdint.h>

#include "jce/renderer/jce_renderer_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The shader's loop bound is this constant, unconditionally.  A uniform step
 * count is the tempting alternative and it is a portability trap: a dynamically
 * bounded loop compiles on desktop GL and is where GLES drivers historically
 * either refuse or silently unroll to something else.  The tier controls how
 * many steps are TAKEN via an early exit inside a constant-bounded loop. */
#define JCE_CONTACT_SHADOW_MAX_STEPS 16

/*
 * Steps this tier may take.  0 disables the march entirely -- the shader then
 * writes 1.0 (fully lit) into the green channel and costs one store.
 *
 * LOW gets 0, not "a few": the tier exists for hardware where the SSAO pass is
 * already a real fraction of the frame, and doubling its depth-fetch count to
 * fix a bias artefact is the wrong trade there.
 */
uint32_t jce_contact_shadow_steps(JceGpuTier tier);

/*
 * Should the march be jittered per pixel?
 *
 * Jitter turns the march's banding into noise, which is an improvement ONLY if
 * something averages the noise over time.  Same rule as cascade dither, same
 * reason, and it must be answered from the same source: the render pipeline's
 * TAA state, not the tier.  Without a temporal resolve the march is fixed and
 * unjittered -- banding a viewer can misread as geometry beats noise a viewer
 * reads as a broken renderer.
 */
bool jce_contact_shadow_jitter(bool has_temporal_resolve);

/*
 * How far the ray reaches, in world units.
 *
 * Contact shadows exist to fix the last few centimetres the shadow map got
 * wrong, so the ray is short by design: a long ray does the cascade's job
 * badly, at screen resolution, with no information behind the first depth
 * layer.  It is scaled by the near cascade's texel size, because the size of
 * the error being corrected IS a texel -- a sharper map needs a shorter ray,
 * and hardcoding metres would make the artefact reappear at 4096.
 *
 * Returns 0 for degenerate input, and 0 means "no march": the shader writes
 * fully lit rather than marching a ray of unknown length.
 */
float jce_contact_shadow_ray_length(float cascade0_radius_m, uint32_t map_size);

#ifdef __cplusplus
}
#endif

#endif /* JCE_CONTACT_SHADOW_H */
