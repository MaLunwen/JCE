/*
 * jce_taa_apply.h  TAA history + reprojection state machine.
 *
 * Owns the per-camera frame-to-frame state that turns one rendered
 * frame into the input for the next TAA resolve:
 *   - Stored previous-frame view + projection (un-jittered) for
 *     reproject matrix construction.
 *   - Halton jitter offset for the current frame (consumed by the
 *     camera setup before submission).
 *   - History RT slot index ping-pong (0 / 1).
 *   - Reproject matrix = prev_view_proj × inverse(curr_view_proj).
 *
 * The renderer calls jce_taa_apply_step(state, curr_view, curr_proj,
 * frame_index) once per frame; the returned descriptor carries the
 * jitter offset + reproject matrix + history slot the resolve pass
 * should sample.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_TAA_APPLY_H
#define JCE_TAA_APPLY_H

#include <jce/renderer/jce_taa_jitter.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct {
    /* Persistent state — caller owns this struct, hands it back each
     * frame.  Zero-initialise before first use. */
    float    prev_view[16];
    float    prev_proj[16];
    uint32_t prev_frame_index;
    uint8_t  history_slot;       /* 0 or 1 — write here, read 1^slot */
    bool     has_prev;
} JceTaaApplyState;

typedef struct {
    /* Sub-pixel jitter in NDC clip-space (caller adds to projection). */
    float jitter_offset[2];
    /* Reprojection matrix: prev_view_proj × inverse(curr_view_proj).
     * Sample history at (uv_curr → uv_prev) by multiplying clip-space
     * coords through this matrix. */
    float reproject_mat4[16];
    /* RT slot to render to this frame (caller binds as target). */
    uint8_t  write_slot;
    /* RT slot to sample history from (caller binds as input). */
    uint8_t  history_slot;
    /* True when a prior frame's history exists.  On first frame, the
     * resolve pass must fall back to "no-op" (write source unchanged). */
    bool     has_history;
} JceTaaApplyFrame;

/* Initialise state.  Equivalent to memset(0) — provided so callers
 * don't have to know the layout. */
JCE_API void jce_taa_apply_init(JceTaaApplyState *state);

/* Advance one frame.  Updates state in place; fills out.  `width` /
 * `height` are the render target dimensions used to size the jitter.
 * `frame_index` should monotonically increase. */
JCE_API void jce_taa_apply_step(JceTaaApplyState *state,
                                  const float       view_mat4[16],
                                  const float       proj_mat4[16],
                                  uint32_t          width,
                                  uint32_t          height,
                                  uint32_t          frame_index,
                                  JceTaaApplyFrame *out);

/* Reset history (e.g. on scene reload).  Next frame will report
 * has_history = false. */
JCE_API void jce_taa_apply_reset_history(JceTaaApplyState *state);

JCE_EXTERN_C_END

#endif /* JCE_TAA_APPLY_H */
