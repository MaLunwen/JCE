/*
 * jce_anim_sm.h -- Animator State Machine runtime (P1-K backend).
 *
 * Loads .anim_sm.json files authored by the editor's Animator SM panel
 * and evaluates the state machine each frame.  Pure logic: outputs the
 * currently active state's clip path + normalized time + transition
 * blend weight.  The caller is responsible for binding clips to a
 * skinned-mesh player via jce_animation.h.
 *
 * Layer: Animation (Layer 3) — public.
 */

#ifndef JCE_ANIM_SM_PUBLIC_H
#define JCE_ANIM_SM_PUBLIC_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceAnimSm JceAnimSm;

/* Parameter type ids -- match the editor enum. */
typedef enum {
    JCE_ANIM_SM_PARAM_FLOAT   = 0,
    JCE_ANIM_SM_PARAM_INT     = 1,
    JCE_ANIM_SM_PARAM_BOOL    = 2,
    JCE_ANIM_SM_PARAM_TRIGGER = 3,
} JceAnimSmParamType;

/* Snapshot of evaluation result for a single frame. */
typedef struct {
    int   state_index;        /* -1 if no active state */
    const char *state_name;   /* points into sm; valid until next free */
    const char *clip_path;
    float state_time;         /* seconds elapsed in current state */
    float state_speed;
    bool  state_loop;
    /* Active transition (if any). */
    int   transition_index;   /* -1 when not transitioning */
    int   from_state;
    int   to_state;
    float blend;              /* 0..1, 0 = pure from-state, 1 = pure to-state */
} JceAnimSmEval;

/* -- Lifecycle ---------------------------------------------------- */

JCE_API JceAnimSm *jce_anim_sm_load_file(const char *path);
JCE_API JceAnimSm *jce_anim_sm_load_text(const char *text, size_t len);
JCE_API void       jce_anim_sm_free(JceAnimSm *sm);

/* -- Parameters --------------------------------------------------- */

JCE_API int                jce_anim_sm_param_count(const JceAnimSm *sm);
JCE_API const char        *jce_anim_sm_param_name (const JceAnimSm *sm, int idx);
JCE_API JceAnimSmParamType jce_anim_sm_param_type (const JceAnimSm *sm, int idx);
JCE_API int                jce_anim_sm_param_find (const JceAnimSm *sm, const char *name);

/* Setters (no-op if name unknown or type mismatched). */
JCE_API void jce_anim_sm_set_float  (JceAnimSm *sm, const char *name, float v);
JCE_API void jce_anim_sm_set_int    (JceAnimSm *sm, const char *name, int v);
JCE_API void jce_anim_sm_set_bool   (JceAnimSm *sm, const char *name, bool v);
JCE_API void jce_anim_sm_set_trigger(JceAnimSm *sm, const char *name);

/* -- Evaluation --------------------------------------------------- */

/* Reset to the default state and clear active transition. */
JCE_API void jce_anim_sm_reset(JceAnimSm *sm);

/* Advance dt seconds; updates internal state, transitions and time. */
JCE_API void jce_anim_sm_update(JceAnimSm *sm, float dt);

/* Snapshot the current evaluation. */
JCE_API void jce_anim_sm_eval(const JceAnimSm *sm, JceAnimSmEval *out);

/* -- State-change polling (state-enter/exit gameplay events) ------ *
 *
 * Pure, allocation-free helper that detects when the SM's active state
 * index has changed since the last poll.  The caller owns the previous-
 * state cursor in `*prev_state_io`:
 *
 *   - Compares the SM's current active state index (the same index
 *     jce_anim_sm_eval reports as state_index) against *prev_state_io.
 *   - On a CHANGE: writes the old value into *out_from (may be the seed
 *     sentinel on the first poll), the new index into *out_to, advances
 *     *prev_state_io to the new index, and returns true.
 *   - On NO change (or NULL sm / NULL prev_state_io): returns false and
 *     touches nothing (out params untouched).
 *
 * Seed convention: callers MUST seed *prev_state_io to a SENTINEL that
 * can never equal a real state index (recommended: INT_MIN, or any
 * value < -1) before the FIRST poll.  This makes the first poll report a
 * change INTO the initial state (out_from = the sentinel, out_to =
 * initial state index), so gameplay receives an on_state_enter for the
 * start state — which is what game code almost always wants.  Re-seed the
 * sentinel whenever the SM instance rebinds/resets so the initial enter
 * re-fires.  (Seed to -1 instead if you specifically do NOT want the
 * initial state-enter to fire — then the first poll only fires once the
 * state leaves the -1 "no active state" value.)
 *
 * out_from / out_to may be NULL (the corresponding value is just not
 * written).  Finite/NULL-safe; reads only jce_anim_sm internals. */
JCE_API bool JCE_CALL jce_anim_sm_poll_state_change(const JceAnimSm *sm,
                                                    int *prev_state_io,
                                                    int *out_from,
                                                    int *out_to);

/* -- Introspection ------------------------------------------------ */

JCE_API int         jce_anim_sm_state_count(const JceAnimSm *sm);
JCE_API const char *jce_anim_sm_state_name (const JceAnimSm *sm, int idx);
JCE_API const char *jce_anim_sm_state_clip (const JceAnimSm *sm, int idx);
JCE_API float       jce_anim_sm_state_speed(const JceAnimSm *sm, int idx);
JCE_API bool        jce_anim_sm_state_loop (const JceAnimSm *sm, int idx);

JCE_EXTERN_C_END

#endif /* JCE_ANIM_SM_PUBLIC_H */
