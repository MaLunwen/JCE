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

/* Logic combiner inside a transition condition group. */
typedef enum {
    JCE_ANIM_SM_LOGIC_AND = 0,
    JCE_ANIM_SM_LOGIC_OR  = 1,
} JceAnimSmCondLogic;

/* Transition interruption policy.  Matches Unity Mecanim. */
typedef enum {
    JCE_ANIM_SM_INTERRUPT_NONE              = 0, /* queue; finish current first */
    JCE_ANIM_SM_INTERRUPT_FROM_CURRENT      = 1, /* current state may re-fire */
    JCE_ANIM_SM_INTERRUPT_FROM_NEXT         = 2, /* destination's transitions win */
    JCE_ANIM_SM_INTERRUPT_CURRENT_THEN_NEXT = 3,
} JceAnimSmInterruptSource;

/* Sentinel state index meaning "AnyState" for a transition's `from`. */
#define JCE_ANIM_SM_ANYSTATE (-2)

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

/* -- Introspection ------------------------------------------------ */

JCE_API int         jce_anim_sm_state_count(const JceAnimSm *sm);
JCE_API const char *jce_anim_sm_state_name (const JceAnimSm *sm, int idx);
JCE_API const char *jce_anim_sm_state_clip (const JceAnimSm *sm, int idx);

JCE_EXTERN_C_END

#endif /* JCE_ANIM_SM_PUBLIC_H */
