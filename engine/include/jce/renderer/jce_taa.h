/*
 * jce_taa.h  Temporal Anti-Aliasing primitives.
 *
 * STATUS (P3-27): Foundation + GPU shaders.
 *   ✅ Halton-(2,3) jitter sequence
 *   ✅ Sub-pixel projection patcher
 *   ✅ Per-frame jitter state (current + previous, for reproject)
 *   ✅ Previous view/proj matrix tracking (drives motion vectors)
 *   ✅ fs_taa.sc + fs_motion_vec.sc shader pair
 *   ⏳ History colour buffer ping-pong (renderer-side wiring)
 *
 * Usage (foundation):
 *   JceTaaState s = {0};
 *   jce_taa_advance(&s, target_w, target_h);
 *   jce_mat4 proj = jce_camera_proj(cam, aspect, w, h);
 *   jce_taa_apply_jitter(&proj, s.current_jitter);
 *   bgfx_set_view_transform(view_id, view, &proj);
 *   // After all draw calls for the frame:
 *   jce_taa_record_camera(&s, &view, &proj);  // stash for next frame
 *
 * Layer: Renderer.
 */

#ifndef JCE_TAA_H
#define JCE_TAA_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Per-frame TAA state.  Caller owns; zero-initialised is valid. */
typedef struct JceTaaState {
    /* NDC-space jitter for THIS frame: ±0.5/target_dim. */
    float    current_jitter[2];
    /* Previous frame's jitter (needed for motion-vector reproject). */
    float    previous_jitter[2];
    /* Halton sequence index (wraps every 8 frames; period 16 is also valid). */
    uint32_t frame_index;

    /* Previous-frame camera matrices (un-jittered) — uploaded to the
     * motion-vector pass via u_prevViewProj.  Updated by
     * jce_taa_record_camera() at end-of-frame. */
    jce_mat4 prev_view;
    jce_mat4 prev_proj;
    /* JCE_TAA_JITTER_PHASE, resolved once. phase_pin < 0 means "advance
       normally"; 0..7 pins the Halton index there so a screenshot A/B is not
       comparing two different sub-pixel offsets. Zero-init is correct: the
       env has not been read yet. */
    int32_t phase_pin;
    bool    phase_pin_read;

    /* Set false after jce_taa_advance() until a record_camera() call —
     * shader code should treat history as invalid on the first frame. */
    bool     prev_valid;
} JceTaaState;

/* Advance to the next frame: computes current jitter, rotates previous.
   target_w/target_h are render-target dimensions in pixels. */
JCE_API void jce_taa_advance(JceTaaState *state,
                             uint32_t target_w, uint32_t target_h);

/* Mutate a column-major projection matrix to add the jitter offset.
   For a standard perspective matrix this nudges m[2][0] and m[2][1]. */
JCE_API void jce_taa_apply_jitter(jce_mat4 *proj, const float jitter[2]);

/* Stash this frame's UN-JITTERED view + proj for next frame's
 * reproject.  Call after the main draw pass (or at end-of-frame).
 * Sets state->prev_valid = true.  Pass the camera matrices BEFORE
 * jce_taa_apply_jitter() — motion vectors must be jitter-free. */
JCE_API void jce_taa_record_camera(JceTaaState  *state,
                                   const jce_mat4 *view,
                                   const jce_mat4 *proj);

/* Halton sequence sample for (i, base).  Exposed for testing. */
JCE_API float jce_taa_halton(uint32_t i, uint32_t base);

JCE_EXTERN_C_END

#endif /* JCE_TAA_H */
