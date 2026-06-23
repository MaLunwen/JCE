/*
 * jce_gas.h -- Gameplay Ability System (GAS) core.
 *
 * A pure-logic, deterministic, headless ability/attribute/effect model
 * inspired by Unreal's GameplayAbilitySystem, scoped down to the parts
 * that need no engine wiring:
 *
 *   - JceAttributeSet : a flat bag of named float attributes, each with a
 *     base value and a [min,max] clamp.  Examples: Health, Mana, Speed.
 *
 *   - JceGameplayEffect : a typed change to one attribute.  An effect has
 *     an operation (ADD / MULT / OVERRIDE), a magnitude, a duration mode
 *     (INSTANT / TIMED / INFINITE), an optional period (for poison-style
 *     repeated application) and a stacking policy (REPLACE / AGGREGATE).
 *
 *   - JceGameplayAbilitySystem : owns one attribute set, a pool of active
 *     effects, and a registry of abilities.  Abilities have a cost
 *     (an attribute + magnitude), a cooldown, and a set of effects they
 *     grant when activated.
 *
 * VALUE COMPOSITION (the contract every reader relies on):
 *
 *   An attribute's *base* value is the persistent, stored number.  It is
 *   mutated directly and permanently by INSTANT effects and by each PERIODIC
 *   tick (poison damages base; regen-over-time tick heals base).
 *
 *   An attribute's *current* value is the base value as modified by all the
 *   active *continuous* (period == 0) TIMED/INFINITE effects.  Continuous
 *   effects never touch base; they are folded in live every query:
 *
 *       add_sum  = sum  of magnitudes of continuous ADD      effects
 *       product  = prod of magnitudes of continuous MULT     effects (1 if none)
 *       override = magnitude of the *last-applied* continuous OVERRIDE effect
 *
 *       current = clamp(
 *                     has_override ? override : (base + add_sum) * product,
 *                     min, max )
 *
 *   OVERRIDE wins over ADD/MULT entirely.  If several continuous OVERRIDE
 *   effects are active, the most recently applied one wins (last writer).
 *   AGGREGATE stacking multiplies a continuous effect's contribution by its
 *   stack_count (ADD: magnitude*stack added; MULT: magnitude^stack; OVERRIDE:
 *   the value is taken as-is, stack_count only tracks lifetime).
 *
 * No RNG.  No I/O.  Fixed-dt ticking is bit-deterministic.
 *
 * UPWARD DEPS: none.  This header includes only os/core/jce_defs.h and the
 * C standard <stdbool.h>/<stdint.h>.  It pulls in NO scene / script / net /
 * GPU header.  Attributes are independent: there is no attribute-dependency
 * solver (e.g. MaxHealth driving Health) — that is a caller/game concern.
 *
 * Layer: middleware/world (Layer 4) -- public.
 */
#ifndef JCE_GAS_H
#define JCE_GAS_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Capacities (fixed, no heap allocation in the core).                 */
/* ------------------------------------------------------------------ */

#define JCE_GAS_MAX_ATTRIBUTES     32
#define JCE_GAS_MAX_ACTIVE_EFFECTS 64
#define JCE_GAS_MAX_ABILITIES      32
#define JCE_GAS_MAX_GRANTED        8   /* effects granted per ability */
#define JCE_GAS_NAME_LEN           64

/* ------------------------------------------------------------------ */
/* Attributes.                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char  name[JCE_GAS_NAME_LEN];
    float base;
    float min;
    float max;
} JceAttribute;

typedef struct {
    JceAttribute attributes[JCE_GAS_MAX_ATTRIBUTES];
    int32_t      count;
} JceAttributeSet;

JCE_API void    jce_attribute_set_init(JceAttributeSet *set);

/* Add an attribute.  Returns its index, or -1 if full / bad args.
 * If max < min the clamp range is left as authored (caller's choice). */
JCE_API int32_t jce_attribute_set_add(JceAttributeSet *set, const char *name,
                                      float base, float min, float max);

/* Linear lookup by name.  Returns index or -1. */
JCE_API int32_t jce_attribute_set_find(const JceAttributeSet *set,
                                       const char *name);

/* Compose a single attribute's current value from pre-folded modifier
 * accumulators.  This is the pure math primitive that the live query and
 * tests share:
 *
 *   current = clamp( has_override ? override
 *                                 : (base + modifier_add_sum) * modifier_mult_product,
 *                    min, max )
 *
 * Returns 0 for a bad index. */
JCE_API float   jce_attribute_set_get_current(const JceAttributeSet *set,
                                              int32_t idx,
                                              float modifier_add_sum,
                                              float modifier_mult_product,
                                              float override_value,
                                              bool  has_override);

/* ------------------------------------------------------------------ */
/* Gameplay effects.                                                   */
/* ------------------------------------------------------------------ */

typedef enum {
    JCE_GAS_OP_ADD      = 0,   /* current += magnitude (or base += for instant) */
    JCE_GAS_OP_MULT     = 1,   /* current *= magnitude */
    JCE_GAS_OP_OVERRIDE = 2    /* current  = magnitude */
} JceGameplayEffectOp;

typedef enum {
    JCE_GAS_DURATION_INSTANT  = 0,  /* applied once to base, not stored */
    JCE_GAS_DURATION_TIMED    = 1,  /* lives for duration_seconds */
    JCE_GAS_DURATION_INFINITE = 2   /* lives until removed */
} JceGameplayEffectDuration;

typedef enum {
    /* On apply, an existing active effect with the same tag_id is removed
     * first, so only the newest copy is active. */
    JCE_GAS_STACK_REPLACE   = 0,
    /* On apply, if an active effect with the same tag_id exists, its
     * stack_count is incremented (and its timers refreshed) instead of
     * adding a second slot. */
    JCE_GAS_STACK_AGGREGATE = 1
} JceGameplayEffectStackPolicy;

typedef struct {
    int32_t                      attr_idx;        /* target attribute index */
    JceGameplayEffectOp          op;
    float                        magnitude;
    JceGameplayEffectDuration    duration_mode;
    float                        duration_seconds; /* TIMED only */
    /* 0 = continuous modifier (folds into current while active);
     * > 0 = periodic: every period_seconds the magnitude is applied to
     * base (poison / regen-over-time). */
    float                        period_seconds;
    JceGameplayEffectStackPolicy stack_policy;
    uint16_t                     tag_id;           /* identity for stacking */
    char                         tag_name[JCE_GAS_NAME_LEN];
} JceGameplayEffect;

/* A live instance of a stored (TIMED/INFINITE) effect. */
typedef struct {
    JceGameplayEffect effect_def;
    float             elapsed;       /* seconds since applied */
    float             next_period;   /* next absolute elapsed time to fire */
    int32_t           stack_count;   /* >= 1 */
    uint32_t          handle;        /* non-zero unique id */
} JceActiveGameplayEffect;

/* ------------------------------------------------------------------ */
/* Abilities.                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char              name[JCE_GAS_NAME_LEN];
    uint32_t          id;                  /* caller-defined identity */
    int32_t           cost_attr_idx;       /* -1 = free */
    float             cost_magnitude;      /* deducted from base on activate */
    float             cooldown_seconds;
    JceGameplayEffect granted_effects[JCE_GAS_MAX_GRANTED];
    int32_t           granted_count;
} JceAbilityDef;

typedef struct {
    JceAbilityDef def;
    float         cooldown_remaining;
} JceAbilityInstance;

/* ------------------------------------------------------------------ */
/* The ability system instance.                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    JceAttributeSet         attributes;
    JceActiveGameplayEffect active_effects[JCE_GAS_MAX_ACTIVE_EFFECTS];
    int32_t                 active_effect_count;
    JceAbilityInstance      abilities[JCE_GAS_MAX_ABILITIES];
    int32_t                 ability_count;
    uint32_t                next_effect_handle;
} JceGameplayAbilitySystem;

JCE_API void jce_gas_init(JceGameplayAbilitySystem *gas);

/* ------------------------------------------------------------------ */
/* Effects.                                                            */
/* ------------------------------------------------------------------ */

/* Apply an effect.
 *
 *   INSTANT  : the magnitude is applied once, permanently, to the target
 *              attribute's BASE value (ADD/MULT/OVERRIDE against base),
 *              re-clamped to [min,max].  Nothing is stored; returns 0.
 *
 *   TIMED /  : the effect is stored as an active effect and contributes
 *   INFINITE   live (continuous, period==0) or fires periodically
 *              (period>0).  Honors stack_policy.  Returns the new (or, for
 *              AGGREGATE, the existing) effect's non-zero handle.
 *
 * Returns 0 on a bad attr_idx, a full active pool, or a null gas. */
JCE_API uint32_t jce_gas_apply_effect(JceGameplayAbilitySystem *gas,
                                      const JceGameplayEffect *effect);

/* Advance time by dt seconds:
 *   - For each active effect: elapsed += dt.
 *   - Periodic effects (period>0): while elapsed crossed next_period, apply
 *     the magnitude to base (per stack) and advance next_period.
 *   - TIMED effects whose elapsed >= duration_seconds are removed (after
 *     their final due periodic ticks fire).
 *   - Ability cooldowns drain toward 0. */
JCE_API void jce_gas_tick(JceGameplayAbilitySystem *gas, float dt);

/* Remove a single active effect by handle.  Returns true if found. */
JCE_API bool jce_gas_remove_effect(JceGameplayAbilitySystem *gas,
                                   uint32_t handle);

/* Remove all active effects (does not touch base values). */
JCE_API void jce_gas_clear_effects(JceGameplayAbilitySystem *gas);

/* ------------------------------------------------------------------ */
/* Queries.                                                            */
/* ------------------------------------------------------------------ */

/* The one-call value a game reads: base + all active continuous modifiers,
 * clamped.  Returns 0 for a bad index / null gas. */
JCE_API float jce_gas_attribute_current(const JceGameplayAbilitySystem *gas,
                                        int32_t attr_idx);

/* Number of attributes in the system's set (0..JCE_GAS_MAX_ATTRIBUTES).
 * Stable iteration order for serialization.  Returns 0 for a null gas. */
JCE_API int32_t jce_gas_attribute_count(const JceGameplayAbilitySystem *gas);

/* Name of the attribute at `attr_idx` (the system's own index order), or NULL
 * for a bad index / null gas.  The returned pointer is owned by the gas and
 * is valid until the attribute set is mutated. */
JCE_API const char *jce_gas_attribute_name(const JceGameplayAbilitySystem *gas,
                                           int32_t attr_idx);

/* Convenience: jce_gas_attribute_current() addressed by attribute index, for
 * order-stable serialization (mirror of the by-name reader).  Returns 0 for a
 * bad index / null gas. */
JCE_API float
jce_gas_attribute_get_current_by_index(const JceGameplayAbilitySystem *gas,
                                       int32_t attr_idx);

/* Set the BASE value of the attribute named `name`, re-clamped to its
 * [min,max].  Returns true if the attribute exists and was set, false for an
 * unknown name / null args.  This is the remote-apply path: a client pushes a
 * replicated server-authoritative value into its local GAS without touching
 * any active continuous effects (current() re-folds them over the new base). */
JCE_API bool jce_gas_attribute_set_base(JceGameplayAbilitySystem *gas,
                                        const char *name, float base);

/* Diagnostics: the summed ADD contribution of continuous effects targeting
 * attr_idx (for inspection / tests).  Periodic + instant effects are NOT
 * counted (they already live in base). */
JCE_API float jce_gas_get_modifier_sum(const JceGameplayAbilitySystem *gas,
                                       int32_t attr_idx);

JCE_API int32_t jce_gas_active_effect_count(const JceGameplayAbilitySystem *gas);

/* ------------------------------------------------------------------ */
/* Abilities.                                                          */
/* ------------------------------------------------------------------ */

/* Register an ability.  Returns its index, or -1 if full / bad args. */
JCE_API int32_t jce_gas_register_ability(JceGameplayAbilitySystem *gas,
                                         const JceAbilityDef *def);

/* Try to activate the ability with the given id.  Fails (returns false) if:
 *   - the ability is on cooldown (cooldown_remaining > 0), OR
 *   - it has a cost (cost_attr_idx >= 0) and the attribute's CURRENT value
 *     is below cost_magnitude.
 * On success: deducts the cost from base, starts the cooldown, and applies
 * every granted effect. */
JCE_API bool jce_gas_activate_ability(JceGameplayAbilitySystem *gas,
                                      uint32_t ability_id);

/* Remaining cooldown for an ability id (0 if ready / unknown). */
JCE_API float jce_gas_ability_cooldown_remaining(const JceGameplayAbilitySystem *gas,
                                                 uint32_t ability_id);

#ifdef __cplusplus
}
#endif

#endif /* JCE_GAS_H */
