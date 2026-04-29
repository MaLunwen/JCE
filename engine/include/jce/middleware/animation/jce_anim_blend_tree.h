/*
 * jce_anim_blend_tree.h  Animation blend trees (1D motion field).
 *
 * Provides a lightweight data structure for blending between multiple
 * animation clips driven by a scalar parameter (e.g. movement speed).
 *
 * A 1D blend tree is an ordered list of (clip, threshold) entries.
 * Given a parameter value `v`, evaluate() returns the two adjacent
 * entries whose thresholds bracket `v` plus a [0..1] blend weight.
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

/* ── Construction ────────────────────────────────────────────────── */

/* Create an empty 1D blend tree with `capacity` slots. */
JCE_API JceAnimBlendTree *jce_anim_blend_tree_create_1d(uint32_t capacity);

JCE_API void jce_anim_blend_tree_destroy(JceAnimBlendTree *bt);

/* Append an entry: (clip name + threshold).
 * Entries should be appended in ascending threshold order; a final
 * sort pass runs at first evaluate() if needed.
 * Returns the new entry index, or -1 if the tree is full. */
JCE_API int jce_anim_blend_tree_add(JceAnimBlendTree *bt,
                                     const char *clip_name,
                                     float threshold);

/* Bind a clip pointer to an entry (the clip is borrowed; lifetime is
 * managed by the caller / model).  Lookup by name set in add(). */
JCE_API bool jce_anim_blend_tree_set_clip(JceAnimBlendTree *bt,
                                           const char *clip_name,
                                           const JceAnimClip *clip);

JCE_API uint32_t    jce_anim_blend_tree_count(const JceAnimBlendTree *bt);
JCE_API const char *jce_anim_blend_tree_name (const JceAnimBlendTree *bt, uint32_t i);
JCE_API float       jce_anim_blend_tree_threshold(const JceAnimBlendTree *bt,
                                                  uint32_t i);

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

JCE_EXTERN_C_END

#endif /* JCE_ANIM_BLEND_TREE_PUBLIC_H */
