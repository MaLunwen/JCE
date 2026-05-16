/*
 * jce_reflection_probe_apply.h  Frame-budgeted capture scheduler
 * over the reflection-probe bake queue.
 *
 * Consumes the pending bake queue exposed by
 * jce_reflection_probe_bake; emits up to N (probe_id, face_index,
 * view_matrix, proj_matrix) capture jobs each frame so the renderer
 * can submit one cubemap face per frame slot without overrunning
 * the GPU budget.
 *
 * The renderer fills in the capture, then calls
 * jce_reflection_probe_apply_face_done(probe_id, face) for each
 * completed face; drain_done evicts probes whose mask hit 0.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_REFLECTION_PROBE_APPLY_H
#define JCE_REFLECTION_PROBE_APPLY_H

#include <jce/renderer/jce_reflection_probe_bake.h>

JCE_EXTERN_C_BEGIN

#define JCE_REFL_APPLY_PER_FRAME_MAX 6  /* default budget: one probe */

typedef struct {
    uint32_t probe_id;
    uint8_t  face_index;          /* 0..5 */
    uint16_t face_resolution;
    float    view_mat4[16];
    float    proj_mat4[16];
    float    near_z;
    float    far_z;
} JceReflectionCaptureJob;

typedef struct {
    JceReflectionCaptureJob jobs[JCE_REFL_APPLY_PER_FRAME_MAX];
    uint32_t                count;
} JceReflectionCaptureBatch;

/* Emit at most `budget` capture jobs (clamped to
 * JCE_REFL_APPLY_PER_FRAME_MAX) from the bake queue, prioritising
 * probes with the fewest faces remaining (finish them first).
 * Returns the number written into out->jobs. */
JCE_API uint32_t jce_reflection_probe_apply_emit(
    uint32_t                    budget,
    JceReflectionCaptureBatch  *out);

/* Convenience: mark a face captured (delegates to bake API). */
JCE_API bool jce_reflection_probe_apply_face_done(uint32_t probe_id,
                                                    uint8_t face);

/* Drain probes whose pending_faces_mask is now zero.  Renderer
 * should call this at frame end. */
JCE_API void jce_reflection_probe_apply_drain_done(void);

JCE_EXTERN_C_END

#endif /* JCE_REFLECTION_PROBE_APPLY_H */
