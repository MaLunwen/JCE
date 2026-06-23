/*
 * jce_world_origin.h -- Floating-origin large-world coordinate core.
 *
 * Dependency-light (math only): a tiny, allocation-free, RNG-free helper that
 * tracks a DOUBLE-precision world origin while every per-frame transform the
 * engine touches stays float32.  Large maps push the camera thousands of
 * metres from (0,0,0), where float32 mantissa spacing degrades to centimetres
 * and beyond, producing visible jitter/swimming.  The classic fix is the
 * "floating origin": periodically re-base the world so the camera returns
 * toward (0,0,0), accumulating the removed offset into a double `origin`.
 *
 * This module is PURE math — it owns no scene, no camera, no physics; it just
 * decides WHEN to rebase and BY HOW MUCH, and converts between an entity's
 * float `local` coordinate (relative to the current origin) and its true
 * `double` absolute world coordinate.  The scene/runtime layers apply the
 * returned shift (see jce_scene_apply_world_shift + the runtime rebase pass).
 *
 * ── The load-bearing invariant ─────────────────────────────────────────────
 *     absolute = origin + local
 *
 *   stays exact across a rebase: a rebase adds `shift` to the origin and
 *   `-shift` to every local position, so origin and local move by equal and
 *   opposite amounts and their sum (the absolute world coordinate of any fixed
 *   point) is unchanged.  jce_world_origin_update returns out_shift = -shift,
 *   i.e. exactly the delta a caller must ADD to all local positions so the
 *   camera drifts back toward the origin.
 *
 * ── Determinism ────────────────────────────────────────────────────────────
 *   No globals, no allocation, no RNG.  The chosen shift is QUANTIZED to a
 *   fixed grid (JCE_WORLD_ORIGIN_QUANTUM metres per axis) so that two runs
 *   that reach the same camera position produce a bit-identical shift, the
 *   rebase does not micro-thrash around the threshold, and the accumulated
 *   double origin lands on exact grid multiples (no slow precision creep in
 *   the origin itself).
 *
 *   Default OFF at the engine layer: the runtime only invokes this when a
 *   scene opts in (floating_origin_enabled), so existing content is unchanged.
 */

#ifndef JCE_WORLD_ORIGIN_H
#define JCE_WORLD_ORIGIN_H

#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

/* Quantization grid (metres per axis) for the rebase shift.  The chosen shift
 * is floored to a multiple of this so the rebase is deterministic, does not
 * thrash near the threshold, and the accumulated double origin stays on exact
 * grid multiples.  256 is small enough to keep |local| well inside float32's
 * comfortable range after a rebase yet large enough to avoid frequent shifts. */
#define JCE_WORLD_ORIGIN_QUANTUM 256.0f

typedef struct JceWorldOrigin {
    double origin[3];        /* absolute world position of the local origin */
    float  rebase_threshold; /* metres; rebase when |camera_local| exceeds this */
} JceWorldOrigin;

/* Construct an origin at absolute (0,0,0) with the given rebase threshold
 * (metres).  A threshold of ~4096 keeps |camera_local| comfortably inside the
 * float32 sweet spot between rebases.  Non-positive thresholds are clamped to a
 * small positive value so update() can never divide the world into a thrash. */
JCE_API JceWorldOrigin jce_world_origin_default(float threshold);

/* Decide whether to rebase given the camera's CURRENT local position.
 *
 *   camera_local : the camera position in the current local frame (float[3]).
 *   out_shift    : receives the delta to ADD to every local position so the
 *                  camera moves back toward the origin (i.e. -chosen_shift).
 *                  Set to {0,0,0} when no rebase occurs.  May be NULL.
 *
 * Returns 1 if a rebase occurred (origin advanced, out_shift non-zero), else 0.
 *
 * Rebase rule: if length(camera_local) > rebase_threshold, the chosen shift is
 * camera_local with each axis FLOORED to a multiple of JCE_WORLD_ORIGIN_QUANTUM
 * (toward zero in magnitude via floor of the signed value), origin += shift,
 * out_shift = -shift.  Flooring to the grid (rather than using the raw camera
 * position) keeps the result deterministic and grid-aligned.  If the quantized
 * shift is exactly zero on all axes (camera barely over the threshold but
 * within one quantum of the grid origin), no rebase is performed.
 *
 * NULL-safe: a NULL `wo` or `camera_local` returns 0 and zeroes out_shift. */
JCE_API int jce_world_origin_update(JceWorldOrigin *wo,
                                    const float camera_local[3],
                                    float out_shift[3]);

/* Convert a float local coordinate to its true double absolute coordinate:
 *     out_abs = origin + local
 * NULL-safe (NULL wo treats origin as 0; NULL out_abs is a no-op). */
JCE_API void jce_world_origin_to_absolute(const JceWorldOrigin *wo,
                                          const float local[3],
                                          double out_abs[3]);

/* Convert a double absolute coordinate back to the current float local frame:
 *     out_local = (float)(abs - origin)
 * NULL-safe (NULL wo treats origin as 0; NULL out_local is a no-op). */
JCE_API void jce_world_origin_to_local(const JceWorldOrigin *wo,
                                       const double abs[3],
                                       float out_local[3]);

JCE_EXTERN_C_END

#endif /* JCE_WORLD_ORIGIN_H */
