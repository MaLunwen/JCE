/*
 * jce_reflection_probe_bake.h  Capture spec for 6-face cubemap probe
 * bakes.
 *
 * One JceReflectionProbeBake describes the six view matrices + a
 * face render target the renderer must populate for a single probe.
 * The actual draw passes live in B17.6 (jce_reflection_probe.c
 * scheduler) — this module owns the descriptor + capture-queue.
 *
 * Each enqueued bake is processed one-face-per-frame to spread
 * cost; a follow-up filter pass (IBL prefilter, jce_ibl.c) consumes
 * the captured cubemap to produce the per-mip prefiltered radiance.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_REFLECTION_PROBE_BAKE_H
#define JCE_REFLECTION_PROBE_BAKE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_REFLECTION_PROBE_QUEUE_MAX 16

typedef struct {
    uint32_t probe_id;                /* opaque scene-entity id */
    float    position[3];             /* world-space centre */
    float    near_plane;
    float    far_plane;
    uint16_t face_resolution;         /* edge length in pixels (e.g. 128) */
    /* View matrices for the six cube faces (+X, -X, +Y, -Y, +Z, -Z).
     * Populated by jce_reflection_probe_bake_build_view_matrices. */
    float    view[6][16];
    float    proj[16];                /* shared 90° fov projection */
    /* Faces remaining to capture this bake; one bit per face. */
    uint8_t  pending_faces_mask;
    bool     active;
} JceReflectionProbeBake;

/* Compute the 6 view matrices + shared projection for `pos`.
 * Right-handed, +Y up, conventional cube-face order. */
JCE_API void jce_reflection_probe_bake_build_view_matrices(
    JceReflectionProbeBake *b);

/* ── Queue ───────────────────────────────────────────────────── */

/* Enqueue a probe for baking.  Returns the queue slot or
 * UINT32_MAX if the queue is full.  `face_resolution` defaults to
 * 128 when 0. */
JCE_API uint32_t jce_reflection_probe_bake_enqueue(uint32_t probe_id,
                                                     const float position[3],
                                                     float    near_plane,
                                                     float    far_plane,
                                                     uint16_t face_resolution);

/* Drop the bake job for `probe_id` (e.g. probe deleted). */
JCE_API bool jce_reflection_probe_bake_cancel(uint32_t probe_id);

/* Iterator over the active queue. */
JCE_API uint32_t jce_reflection_probe_bake_count(void);
JCE_API JceReflectionProbeBake *jce_reflection_probe_bake_at(uint32_t idx);

/* Mark face `face` (0..5) as captured for `probe_id`.  Returns true
 * when the probe still has pending faces. */
JCE_API bool jce_reflection_probe_bake_mark_face_done(uint32_t probe_id,
                                                       uint8_t face);

/* Drain completed bakes (all faces captured). */
JCE_API void jce_reflection_probe_bake_drain_done(void);

JCE_EXTERN_C_END

#endif /* JCE_REFLECTION_PROBE_BAKE_H */
