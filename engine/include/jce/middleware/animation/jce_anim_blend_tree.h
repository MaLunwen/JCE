/*
 * jce_anim_blend_tree.h  Animation blend trees (1D + 2D motion field).
 *
 * Provides a lightweight data structure for blending between multiple
 * animation clips driven by a scalar parameter (e.g. movement speed) or
 * a 2D vector parameter (e.g. forward/strafe movement, look pitch/yaw).
 *
 * A 1D blend tree is an ordered list of (clip, threshold) entries.
 * Given a parameter value `v`, evaluate() returns the two adjacent
 * entries whose thresholds bracket `v` plus a [0..1] blend weight.
 *
 * A 2D blend tree positions each sample at a (x,y) point in parameter
 * space.  Given a parameter point (x,y), eval_2d() returns a normalised
 * weight per sample (summing to 1).  Two industry-standard weighting
 * schemes are supported (Unity-style):
 *   - Freeform Cartesian:    Gradient-Band Interpolation over (x,y).
 *   - Freeform Directional:  Gradient-Band over (angle, magnitude), so
 *                            opposite-facing motions never bleed.
 *
 * The blend tree is pure logic — it does not own the clips.  Callers
 * resolve clip pointers (typically by name lookup in a JceModel) and
 * bind them via jce_anim_blend_tree_set_clip().  Sampling happens via
 * jce_anim_player_blend() in jce_animation.h.
 *
 * Layer: Animation (Layer 3) — public.
 */

#ifndef JCE_ANIM_BLEND_TREE_PUBLIC_H
#define JCE_ANIM_BLEND_TREE_PUBLIC_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceAnimClip      JceAnimClip;
typedef struct JceAnimBlendTree JceAnimBlendTree;

/* Blend-tree dimensionality / weighting mode. */
typedef enum {
    JCE_BLEND_TREE_1D = 0,           /* scalar threshold field            */
    JCE_BLEND_TREE_2D_CARTESIAN,     /* Freeform Cartesian (gradient band)*/
    JCE_BLEND_TREE_2D_DIRECTIONAL    /* Freeform Directional (angle/mag)  */
} JceAnimBlendTreeMode;

/* ── Construction ────────────────────────────────────────────────── */

/* Create an empty 1D blend tree with `capacity` slots. */
JCE_API JceAnimBlendTree *jce_anim_blend_tree_create_1d(uint32_t capacity);

/* Create an empty 2D blend tree with `capacity` sample slots.
 * `directional` selects the weighting scheme:
 *   false → Freeform Cartesian   (gradient band in (x,y) plane)
 *   true  → Freeform Directional (gradient band in (angle,magnitude))
 * Add samples with jce_anim_blend_tree_add_2d(); bind clip pointers
 * with jce_anim_blend_tree_set_clip(); evaluate with
 * jce_anim_blend_tree_eval_2d(). */
JCE_API JceAnimBlendTree *jce_anim_blend_tree_create_2d(uint32_t capacity,
                                                        bool directional);

JCE_API void jce_anim_blend_tree_destroy(JceAnimBlendTree *bt);

/* Append an entry: (clip name + threshold).
 * Entries should be appended in ascending threshold order; a final
 * sort pass runs at first evaluate() if needed.
 * Returns the new entry index, or -1 if the tree is full.
 * (1D trees only.) */
JCE_API int jce_anim_blend_tree_add(JceAnimBlendTree *bt,
                                     const char *clip_name,
                                     float threshold);

/* Append a 2D sample: (clip name + position (x,y) in parameter space).
 * Order does not matter for 2D trees.
 * Returns the new sample index, or -1 if the tree is full / not 2D. */
JCE_API int jce_anim_blend_tree_add_2d(JceAnimBlendTree *bt,
                                        const char *clip_name,
                                        float x, float y);

/* Bind a clip pointer to an entry (the clip is borrowed; lifetime is
 * managed by the caller / model).  Lookup by name set in add(). */
JCE_API bool jce_anim_blend_tree_set_clip(JceAnimBlendTree *bt,
                                           const char *clip_name,
                                           const JceAnimClip *clip);

JCE_API uint32_t    jce_anim_blend_tree_count(const JceAnimBlendTree *bt);
JCE_API const char *jce_anim_blend_tree_name (const JceAnimBlendTree *bt, uint32_t i);
JCE_API float       jce_anim_blend_tree_threshold(const JceAnimBlendTree *bt,
                                                  uint32_t i);

JCE_API JceAnimBlendTreeMode jce_anim_blend_tree_mode(const JceAnimBlendTree *bt);

/* Position (x,y) of a 2D sample (zeroed for non-2D trees / bad index). */
JCE_API void jce_anim_blend_tree_position(const JceAnimBlendTree *bt,
                                          uint32_t i,
                                          float *out_x, float *out_y);

/* ── Evaluation ──────────────────────────────────────────────────── */

/* Result of a blend tree evaluation: two clips with normalised weights.
 * When the value sits exactly at one entry's threshold (or outside the
 * range), `clip_b == clip_a` and `weight_b == 0`. */
typedef struct {
    const JceAnimClip *clip_a;
    const JceAnimClip *clip_b;
    float              weight_a;     /* [0..1], weight_a + weight_b == 1 */
    float              weight_b;
    int                index_a;
    int                index_b;
} JceAnimBlendTreeEval;

/* Evaluate the blend tree at scalar parameter `value`.
 * Clamps `value` to the threshold range. */
JCE_API void jce_anim_blend_tree_evaluate(const JceAnimBlendTree *bt,
                                           float value,
                                           JceAnimBlendTreeEval *out);

/* Result of a 2D blend tree evaluation: a normalised weight per sample.
 * `weights[i]` corresponds to sample index `i` (same order as add_2d).
 * The first `count` entries are valid; they sum to 1 (unless count==0).
 * `clips[i]` mirrors the bound clip pointer for convenience. */
typedef struct {
    uint32_t           count;          /* number of valid weights         */
    float              weights[32];    /* per-sample weight, sums to 1     */
    const JceAnimClip *clips[32];      /* bound clip per sample (may NULL) */
} JceAnimBlendTreeEval2D;

/* Evaluate a 2D blend tree at parameter point (x,y), producing a
 * per-sample weight vector (Unity-style gradient-band interpolation).
 *
 * Properties guaranteed:
 *   - At a sample position the weight there is ~1 and all others ~0.
 *   - Between samples weights interpolate smoothly and always sum to 1.
 *   - Symmetric samples around a centred query give equal weights.
 *   - Queries outside the sample hull clamp sanely (nearest samples
 *     dominate; weights still sum to 1; no negative weights).
 *
 * No-op (count=0) if `bt` is NULL or not a 2D tree. */
JCE_API void jce_anim_blend_tree_eval_2d(const JceAnimBlendTree *bt,
                                          float x, float y,
                                          JceAnimBlendTreeEval2D *out);

#define JCE_BLEND_TREE_2D_MAX_SAMPLES 32

JCE_EXTERN_C_END

#endif /* JCE_ANIM_BLEND_TREE_PUBLIC_H */
