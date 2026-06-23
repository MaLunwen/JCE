/*
 * jce_morph.h  Morph targets / blendshapes (FEATURE 3.1).
 *
 * A glTF primitive may carry N "morph targets" (blendshapes): each is a set
 * of per-vertex POSITION (and optionally NORMAL) deltas added on top of the
 * base mesh, scaled by a per-target weight.  The deformed vertex is:
 *
 *     pos'    = base_pos    + sum_i( w_i * dpos_i )
 *     normal' = base_normal + sum_i( w_i * dnormal_i )    (renormalized)
 *
 * This module is the bgfx-free core: it owns the imported per-target delta
 * arrays (JceMorphData), an optional keyframed weights track sampled from the
 * glTF "weights" animation channel (JceMorphWeightTrack), and the CPU evaluator
 * that applies the weighted deltas to a vertex buffer.  It deliberately holds
 * NO GPU resources so it can be unit-tested without a renderer context, and so
 * the import + CPU eval path is exercised end-to-end.
 *
 * The chosen apply strategy is CPU pre-skin: jce_morph_apply rewrites the base
 * position/normal arrays before the (existing) skinning palette runs.  A GPU
 * vertex-shader morph path is a documented follow-up.
 *
 * Layer: Animation (Layer 3) — public.
 */

#ifndef JCE_MORPH_H
#define JCE_MORPH_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Per-instance authored morph-weight ceiling.  A scene's JceMorphWeights
 * component carries up to this many indexed static weights (mirrors the
 * editor slider count); resolution against an animation track is clamped to
 * this many targets.  16 covers the typical face-rig blendshape budget while
 * keeping the component POD/serializable. */
#define JCE_MORPH_MAX_WEIGHTS 16

/* ================================================================== */
/* Morph delta storage (one primitive's blendshapes)                   */
/* ================================================================== */

/* Owns a compact morph-delta buffer for one primitive: `num_targets`
 * blendshapes, each with `num_verts` POSITION deltas and (optionally)
 * `num_verts` NORMAL deltas.  Deltas are stored target-major:
 *     positions[t * num_verts + v]   -> jce_vec3 delta for target t, vertex v
 * `base_weights[t]` are the glTF default weights for the targets (mesh- or
 * node-level), used as the initial per-instance weights. */
typedef struct JceMorphData JceMorphData;

/* Create morph storage for `num_targets` targets over `num_verts` vertices.
 * `has_normals` allocates the parallel NORMAL-delta array (else NORMAL morphing
 * is skipped).  Delta arrays start zeroed; fill them via jce_morph_set_*.
 * Returns NULL on invalid args / OOM. */
JCE_API JceMorphData *JCE_CALL jce_morph_data_create(uint32_t num_targets,
                                                     uint32_t num_verts,
                                                     bool has_normals);

JCE_API void JCE_CALL jce_morph_data_destroy(JceMorphData *m);

/* Counts / capability queries. */
JCE_API uint32_t JCE_CALL jce_morph_target_count(const JceMorphData *m);
JCE_API uint32_t JCE_CALL jce_morph_vertex_count(const JceMorphData *m);
JCE_API bool     JCE_CALL jce_morph_has_normals(const JceMorphData *m);

/* Bulk-set one target's POSITION deltas (`num_verts` jce_vec3, vertex-major).
 * No-op if target/sizes are out of range. */
JCE_API void JCE_CALL jce_morph_set_position_deltas(JceMorphData *m,
                                                    uint32_t target,
                                                    const jce_vec3 *deltas,
                                                    uint32_t count);

/* Bulk-set one target's NORMAL deltas.  No-op when the data has no normals. */
JCE_API void JCE_CALL jce_morph_set_normal_deltas(JceMorphData *m,
                                                  uint32_t target,
                                                  const jce_vec3 *deltas,
                                                  uint32_t count);

/* Read-only delta accessors (NULL if target / data invalid). */
JCE_API const jce_vec3 *JCE_CALL jce_morph_position_deltas(const JceMorphData *m,
                                                           uint32_t target);
JCE_API const jce_vec3 *JCE_CALL jce_morph_normal_deltas(const JceMorphData *m,
                                                         uint32_t target);

/* Set / get the glTF default base weight for a target. */
JCE_API void  JCE_CALL jce_morph_set_base_weight(JceMorphData *m,
                                                 uint32_t target, float w);
JCE_API float JCE_CALL jce_morph_base_weight(const JceMorphData *m,
                                             uint32_t target);

/* Copy all base weights into `out` (clamped to min(num_targets, max_out)).
 * Returns the number written.  Use to seed a per-instance weight array. */
JCE_API uint32_t JCE_CALL jce_morph_copy_base_weights(const JceMorphData *m,
                                                      float *out,
                                                      uint32_t max_out);

/* ================================================================== */
/* CPU evaluator                                                        */
/* ================================================================== */

/* Apply weighted morph deltas to a base vertex buffer, writing the deformed
 * result.  Positions/normals are interleaved with `stride` bytes between
 * vertices; `pos_offset` / `normal_offset` are byte offsets to the float[3]
 * fields within each vertex.  Pass normal_offset < 0 to skip normals.
 *
 *   out_pos[v]    = base_pos[v]    + sum_t( weights[t] * dpos[t][v] )
 *   out_normal[v] = normalize(base_normal[v] + sum_t( weights[t] * dnorm[t][v] ))
 *
 * `base` and `out` may alias (in-place morph).  `weights` has num_targets
 * entries; a NULL/empty weights or zero targets copies base -> out unchanged.
 * No-op (returns false) on invalid args. */
JCE_API bool JCE_CALL jce_morph_apply(const JceMorphData *m,
                                      const float *weights, uint32_t num_weights,
                                      const void *base, void *out,
                                      uint32_t num_verts, uint32_t stride,
                                      int32_t pos_offset, int32_t normal_offset);

/* ================================================================== */
/* Per-instance weight resolution (track ⊕ authored static weights)    */
/* ================================================================== */

/* Combine the animation-track-sampled weights with per-instance AUTHORED
 * static weights (a scene JceMorphWeights component) into the final weight
 * vector handed to jce_morph_apply.
 *
 *   for each target t < count:
 *       out[t] = (override_mask & (1<<t)) ? authored[t] : track[t]
 *
 * `track` may be NULL (treated as all-zero — no clip driving morph), and
 * `authored` may be NULL (treated as no overrides regardless of the mask).
 * `count` is clamped to JCE_MORPH_MAX_WEIGHTS.  Returns the number of weights
 * written (== clamped count), or 0 on invalid args (out == NULL).
 *
 * The override bitmask lets an authored weight of exactly 0.0 still OVERRIDE a
 * non-zero track value (a designer pinning a blendshape OFF), which a plain
 * max/add could not express. This is the deterministic, headless-testable
 * combine the scene renderer uses before calling jce_morph_apply. */
JCE_API uint32_t JCE_CALL jce_morph_resolve_weights(
    const float *track, const float *authored, uint32_t override_mask,
    uint32_t count, float *out, uint32_t max_out);

/* ================================================================== */
/* Keyframed weights track (glTF "weights" animation channel)          */
/* ================================================================== */

/* A weights track holds, for one animation, a timeline of all-target weight
 * vectors: `num_targets` weights at each of `num_keys` keyframes.  glTF packs
 * the output accessor as (num_keys * num_targets) scalars, key-major:
 *     values[k * num_targets + t]
 * Sampling at a time interpolates each target independently (LINEAR or STEP). */
typedef struct JceMorphWeightTrack JceMorphWeightTrack;

typedef enum {
    JCE_MORPH_INTERP_STEP   = 0,
    JCE_MORPH_INTERP_LINEAR = 1
} JceMorphInterp;

/* Create a weights track.  Copies `timestamps` [num_keys] and `values`
 * [num_keys * num_targets] (key-major).  Returns NULL on invalid args/OOM. */
JCE_API JceMorphWeightTrack *JCE_CALL jce_morph_weight_track_create(
    uint32_t num_targets, uint32_t num_keys,
    const float *timestamps, const float *values,
    JceMorphInterp interp);

JCE_API void JCE_CALL jce_morph_weight_track_destroy(JceMorphWeightTrack *t);

JCE_API uint32_t JCE_CALL jce_morph_weight_track_targets(const JceMorphWeightTrack *t);
JCE_API uint32_t JCE_CALL jce_morph_weight_track_keys(const JceMorphWeightTrack *t);
JCE_API float    JCE_CALL jce_morph_weight_track_duration(const JceMorphWeightTrack *t);

/* Sample the track at `time`, writing up to `max_out` per-target weights into
 * `out`.  Clamps to the track endpoints; interpolates between keys per
 * `interp`.  Returns the number of weights written (min(num_targets, max_out)).
 * Returns 0 on invalid args. */
JCE_API uint32_t JCE_CALL jce_morph_weight_track_sample(
    const JceMorphWeightTrack *t, float time,
    float *out, uint32_t max_out);

JCE_EXTERN_C_END

#endif /* JCE_MORPH_H */
