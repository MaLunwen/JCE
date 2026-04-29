/*
 * jce_anim_sm_binding.h -- Per-instance SM binding (P3-33 closeout).
 *
 * The raw JceAnimSm runtime is stateless w.r.t. ownership: every game
 * entity that wants its own state machine instance needs its own copy
 * of the loaded definition (parameters, current state, time elapsed).
 * This module owns one such instance and provides:
 *
 *   - load-once-on-create lifecycle keyed off a definition path,
 *   - a thin parameter-setter forwarder (so client code doesn't need
 *     to thread JceAnimSm pointers around),
 *   - a frame tick that wraps update + eval into one call,
 *   - a layering-safe helper that resolves the currently active clip
 *     name to an INDEX inside an externally-owned name table (so
 *     animation/ stays free of any scene/ dependency).
 *
 * Layer: Animation (Layer 3) — public, depends only on jce_anim_sm.h.
 */

#ifndef JCE_ANIM_SM_BINDING_H
#define JCE_ANIM_SM_BINDING_H

#include <jce/middleware/animation/jce_anim_sm.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceAnimSmBinding JceAnimSmBinding;

/* Load the .anim_sm.json at `definition_path` and create a per-entity
 * binding owning a fresh runtime instance.  Returns NULL on failure. */
JCE_API JceAnimSmBinding *jce_anim_sm_binding_create(const char *definition_path);

JCE_API void              jce_anim_sm_binding_destroy(JceAnimSmBinding *b);

/* The path passed to create(); useful for editor inspector / save. */
JCE_API const char *jce_anim_sm_binding_path(const JceAnimSmBinding *b);

/* Underlying runtime — exposed for parameter introspection only.
 * Do NOT call jce_anim_sm_free() on it; the binding owns the lifetime. */
JCE_API JceAnimSm *jce_anim_sm_binding_runtime(JceAnimSmBinding *b);

/* ── Parameter setters (no-op on unknown name / type mismatch) ───── */

JCE_API void jce_anim_sm_binding_set_float  (JceAnimSmBinding *b, const char *name, float v);
JCE_API void jce_anim_sm_binding_set_int    (JceAnimSmBinding *b, const char *name, int v);
JCE_API void jce_anim_sm_binding_set_bool   (JceAnimSmBinding *b, const char *name, bool v);
JCE_API void jce_anim_sm_binding_set_trigger(JceAnimSmBinding *b, const char *name);

/* ── Frame tick / eval ───────────────────────────────────────────── */

/* Reset to the SM's default state (clears time + active transition). */
JCE_API void jce_anim_sm_binding_reset(JceAnimSmBinding *b);

/* Advance dt seconds and refresh the cached eval snapshot. */
JCE_API void jce_anim_sm_binding_tick(JceAnimSmBinding *b, float dt);

/* Most-recent eval snapshot from the last tick (or jce_anim_sm_reset).
 * The returned pointer is valid until the next tick or destroy. */
JCE_API const JceAnimSmEval *jce_anim_sm_binding_eval(const JceAnimSmBinding *b);

/* ── Integration helpers (layer-safe) ─────────────────────────────── */

/* Match the current state's clip_path against an externally-owned
 * `clip_names` table of `count` entries.  Returns the matching index,
 * or -1 if not found / no active state.  Useful when binding the SM
 * to a JceSkeletalAnimatorComponent without dragging scene/ into the
 * animation layer. */
JCE_API int jce_anim_sm_binding_resolve_clip_index(const JceAnimSmBinding *b,
                                                    const char           *const *clip_names,
                                                    int                          count);

JCE_EXTERN_C_END

#endif /* JCE_ANIM_SM_BINDING_H */
