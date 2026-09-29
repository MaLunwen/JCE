/*
 * jce_sr_debug_targets.h -- the debug-target table, separated from the
 * renderer so it can be tested without a GPU.
 *
 * WHY THE SPLIT.  The public entry points take a JceSceneRenderer, and a
 * JceSceneRenderer cannot be constructed in a unit test: it owns bgfx
 * resources.  So the part that MATTERS -- which targets are listed for a
 * given set of validity bits -- would have been reachable only by running the
 * editor and looking, which is how a stale-target bug stays invisible.
 *
 * The state below is a parameter object, NOT a second list of targets.  The
 * table itself exists exactly once, in jce_sr_debug_targets_build; the public
 * functions do a field-for-field copy out of the renderer and call it.  That
 * copy is the one place a new target could be forgotten, which is why
 * test_jce_sr_debug_targets.c pins the full all-valid name list: a row added
 * to the table without a handle plumbed into the state simply would not
 * appear, and the golden list is what notices.
 */

#ifndef JCE_SR_DEBUG_TARGETS_H
#define JCE_SR_DEBUG_TARGETS_H

#include <jce/renderer/jce_scene_renderer.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Everything the table needs, and nothing else.  Handles are raw bgfx
 * indices: UINT16_MAX means "no such texture", and 0 is a PERFECTLY VALID
 * handle -- this engine has twice shipped a bug from treating bgfx index 0 as
 * a sentinel, so the tests cover it explicitly. */
typedef struct JceSrDebugTargetState {
    /* Depth pre-pass / G-buffer. */
    bool     ssao_valid;           /* the render target is ALLOCATED */
    bool     depth_prepass_frame;  /* ...and THIS frame wrote it */
    bool     ssao_has_velocity;    /* the FBO was created with a velocity RT */
    bool     velocity_valid_frame; /* ...and this frame wrote motion into it */
    uint16_t ssao_w, ssao_h;
    uint16_t depth_tex, normal_tex, albedo_tex, velocity_tex;

    /* Shadows. */
    bool     shadow_valid;
    bool     shadow_use_csm;       /* picks WHICH of the two was filled */
    uint16_t shadow_map_size, dyn_csm_atlas_size;
    uint16_t shadow_tex, dyn_csm_atlas_tex, local_atlas_tex, cloud_shadow_tex;

    /* Lookup tables. */
    uint16_t brdf_lut, sky_transmittance_tex, sky_multiscatter_tex;
} JceSrDebugTargetState;

/* The number of rows the table can ever emit.  A caller sizes its array by
 * this; the build refuses to write past `cap`. */
#define JCE_SR_DEBUG_TARGET_MAX 16

/* Fill `out` with the targets that hold real pixels for this state, in a
 * stable order, and return how many.  Pure: no globals, no renderer, no
 * bgfx. */
int jce_sr_debug_targets_build(const JceSrDebugTargetState *st,
                               JceRenderTargetInfo *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SR_DEBUG_TARGETS_H */
