/*
 * jce_gas.c -- Gameplay Ability System (GAS) core.
 *
 * Pure logic, deterministic, headless.  See jce_gas.h for the value
 * composition contract.  No RNG, no I/O, no upward dependencies.
 */

#include <jce/middleware/world/jce_gas.h>

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small helpers.                                                      */
/* ------------------------------------------------------------------ */

static float clampf(float v, float lo, float hi)
{
    if (lo <= hi) {
        if (v < lo) return lo;
        if (v > hi) return hi;
    }
    return v;
}

/* Apply one op against a scalar `cur` and return the result (no clamp). */
static float apply_op(float cur, JceGameplayEffectOp op, float magnitude)
{
    switch (op) {
        case JCE_GAS_OP_ADD:      return cur + magnitude;
        case JCE_GAS_OP_MULT:     return cur * magnitude;
        case JCE_GAS_OP_OVERRIDE: return magnitude;
        default:                  return cur;
    }
}

/* True if this effect is a continuous (live-folded) modifier: stored
 * (TIMED/INFINITE) with no period. */
static bool is_continuous(const JceGameplayEffect *e)
{
    return e->period_seconds <= 0.0f &&
           e->duration_mode != JCE_GAS_DURATION_INSTANT;
}

/* ------------------------------------------------------------------ */
/* Attributes.                                                         */
/* ------------------------------------------------------------------ */

void jce_attribute_set_init(JceAttributeSet *set)
{
    if (!set) return;
    memset(set, 0, sizeof(*set));
}

int32_t jce_attribute_set_add(JceAttributeSet *set, const char *name,
                              float base, float min, float max)
{
    if (!set || !name) return -1;
    if (set->count >= JCE_GAS_MAX_ATTRIBUTES) return -1;

    int32_t idx = set->count;
    JceAttribute *a = &set->attributes[idx];
    memset(a, 0, sizeof(*a));
    strncpy(a->name, name, JCE_GAS_NAME_LEN - 1);
    a->name[JCE_GAS_NAME_LEN - 1] = '\0';
    a->min  = min;
    a->max  = max;
    a->base = clampf(base, min, max);
    set->count = idx + 1;
    return idx;
}

int32_t jce_attribute_set_find(const JceAttributeSet *set, const char *name)
{
    if (!set || !name) return -1;
    for (int32_t i = 0; i < set->count; ++i) {
        if (strncmp(set->attributes[i].name, name, JCE_GAS_NAME_LEN) == 0)
            return i;
    }
    return -1;
}

float jce_attribute_set_get_current(const JceAttributeSet *set, int32_t idx,
                                    float modifier_add_sum,
                                    float modifier_mult_product,
                                    float override_value, bool has_override)
{
    if (!set || idx < 0 || idx >= set->count) return 0.0f;
    const JceAttribute *a = &set->attributes[idx];
    float v = has_override
                  ? override_value
                  : (a->base + modifier_add_sum) * modifier_mult_product;
    return clampf(v, a->min, a->max);
}

/* ------------------------------------------------------------------ */
/* System lifecycle.                                                   */
/* ------------------------------------------------------------------ */

void jce_gas_init(JceGameplayAbilitySystem *gas)
{
    if (!gas) return;
    memset(gas, 0, sizeof(*gas));
    jce_attribute_set_init(&gas->attributes);
    gas->next_effect_handle = 1u; /* 0 is reserved as "invalid / instant" */
}

/* ------------------------------------------------------------------ */
/* Base mutation (instant + periodic application).                     */
/* ------------------------------------------------------------------ */

/* Apply `op(magnitude)` to a single attribute's BASE value `count` times
 * (count == stack_count for AGGREGATE periodic effects), re-clamping. */
static void apply_to_base(JceGameplayAbilitySystem *gas, int32_t attr_idx,
                          JceGameplayEffectOp op, float magnitude,
                          int32_t count)
{
    if (attr_idx < 0 || attr_idx >= gas->attributes.count) return;
    JceAttribute *a = &gas->attributes.attributes[attr_idx];
    float v = a->base;
    for (int32_t i = 0; i < count; ++i)
        v = apply_op(v, op, magnitude);
    a->base = clampf(v, a->min, a->max);
}

/* ------------------------------------------------------------------ */
/* Effect application.                                                 */
/* ------------------------------------------------------------------ */

/* Find the active-effect slot index with a matching tag_id, or -1. */
static int32_t find_active_by_tag(const JceGameplayAbilitySystem *gas,
                                  uint16_t tag_id)
{
    for (int32_t i = 0; i < gas->active_effect_count; ++i) {
        if (gas->active_effects[i].effect_def.tag_id == tag_id)
            return i;
    }
    return -1;
}

/* Remove the active-effect slot at index i (swap-back compaction is NOT
 * used: order is preserved so that OVERRIDE last-writer semantics stay
 * intuitive). */
static void remove_active_at(JceGameplayAbilitySystem *gas, int32_t i)
{
    if (i < 0 || i >= gas->active_effect_count) return;
    for (int32_t j = i; j < gas->active_effect_count - 1; ++j)
        gas->active_effects[j] = gas->active_effects[j + 1];
    gas->active_effect_count--;
}

uint32_t jce_gas_apply_effect(JceGameplayAbilitySystem *gas,
                              const JceGameplayEffect *effect)
{
    if (!gas || !effect) return 0u;
    if (effect->attr_idx < 0 || effect->attr_idx >= gas->attributes.count)
        return 0u;

    /* INSTANT: permanently change base, store nothing. */
    if (effect->duration_mode == JCE_GAS_DURATION_INSTANT) {
        apply_to_base(gas, effect->attr_idx, effect->op, effect->magnitude, 1);
        return 0u;
    }

    /* AGGREGATE: fold into an existing same-tag effect if present. */
    if (effect->stack_policy == JCE_GAS_STACK_AGGREGATE) {
        int32_t existing = find_active_by_tag(gas, effect->tag_id);
        if (existing >= 0) {
            JceActiveGameplayEffect *ae = &gas->active_effects[existing];
            ae->stack_count++;
            /* Refresh timers so the stack shares one lifetime. */
            ae->elapsed     = 0.0f;
            ae->next_period = effect->period_seconds > 0.0f
                                  ? effect->period_seconds
                                  : 0.0f;
            return ae->handle;
        }
    } else { /* REPLACE: drop the existing same-tag effect first. */
        int32_t existing = find_active_by_tag(gas, effect->tag_id);
        if (existing >= 0)
            remove_active_at(gas, existing);
    }

    if (gas->active_effect_count >= JCE_GAS_MAX_ACTIVE_EFFECTS)
        return 0u;

    int32_t slot = gas->active_effect_count;
    JceActiveGameplayEffect *ae = &gas->active_effects[slot];
    memset(ae, 0, sizeof(*ae));
    ae->effect_def  = *effect;
    ae->elapsed     = 0.0f;
    ae->stack_count = 1;
    ae->next_period = effect->period_seconds > 0.0f ? effect->period_seconds
                                                    : 0.0f;
    ae->handle      = gas->next_effect_handle++;
    if (gas->next_effect_handle == 0u) gas->next_effect_handle = 1u;
    gas->active_effect_count = slot + 1;
    return ae->handle;
}

/* ------------------------------------------------------------------ */
/* Tick.                                                               */
/* ------------------------------------------------------------------ */

void jce_gas_tick(JceGameplayAbilitySystem *gas, float dt)
{
    if (!gas || dt < 0.0f) return;

    /* Iterate backwards-safe: we may remove the current slot. */
    for (int32_t i = 0; i < gas->active_effect_count; /* advance inside */) {
        JceActiveGameplayEffect *ae = &gas->active_effects[i];
        const JceGameplayEffect *e  = &ae->effect_def;
        ae->elapsed += dt;

        /* Periodic application to base. */
        if (e->period_seconds > 0.0f) {
            /* Fire each period boundary that elapsed has crossed.  For a
             * TIMED effect, do not fire ticks scheduled past its lifetime. */
            while (ae->next_period <= ae->elapsed + 1e-6f) {
                if (e->duration_mode == JCE_GAS_DURATION_TIMED &&
                    ae->next_period > e->duration_seconds + 1e-6f)
                    break;
                apply_to_base(gas, e->attr_idx, e->op, e->magnitude,
                              ae->stack_count);
                ae->next_period += e->period_seconds;
            }
        }

        /* Expire TIMED effects. */
        if (e->duration_mode == JCE_GAS_DURATION_TIMED &&
            ae->elapsed + 1e-6f >= e->duration_seconds) {
            remove_active_at(gas, i);
            continue; /* slot i now holds the next effect */
        }
        ++i;
    }

    /* Drain ability cooldowns. */
    for (int32_t i = 0; i < gas->ability_count; ++i) {
        float c = gas->abilities[i].cooldown_remaining - dt;
        gas->abilities[i].cooldown_remaining = c < 0.0f ? 0.0f : c;
    }
}

bool jce_gas_remove_effect(JceGameplayAbilitySystem *gas, uint32_t handle)
{
    if (!gas || handle == 0u) return false;
    for (int32_t i = 0; i < gas->active_effect_count; ++i) {
        if (gas->active_effects[i].handle == handle) {
            remove_active_at(gas, i);
            return true;
        }
    }
    return false;
}

void jce_gas_clear_effects(JceGameplayAbilitySystem *gas)
{
    if (!gas) return;
    gas->active_effect_count = 0;
}

/* ------------------------------------------------------------------ */
/* Queries.                                                            */
/* ------------------------------------------------------------------ */

/* Fold all continuous effects targeting attr_idx into accumulators. */
static void fold_continuous(const JceGameplayAbilitySystem *gas,
                            int32_t attr_idx, float *add_sum,
                            float *mult_product, float *override_value,
                            bool *has_override)
{
    *add_sum        = 0.0f;
    *mult_product   = 1.0f;
    *override_value = 0.0f;
    *has_override   = false;

    for (int32_t i = 0; i < gas->active_effect_count; ++i) {
        const JceActiveGameplayEffect *ae = &gas->active_effects[i];
        const JceGameplayEffect *e = &ae->effect_def;
        if (e->attr_idx != attr_idx) continue;
        if (!is_continuous(e)) continue;

        float sc = (float)ae->stack_count;
        switch (e->op) {
            case JCE_GAS_OP_ADD:
                *add_sum += e->magnitude * sc;
                break;
            case JCE_GAS_OP_MULT:
                /* AGGREGATE multiplies the factor once per stack. */
                *mult_product *= powf(e->magnitude, sc);
                break;
            case JCE_GAS_OP_OVERRIDE:
                /* Last writer (latest in apply order) wins. */
                *override_value = e->magnitude;
                *has_override   = true;
                break;
            default:
                break;
        }
    }
}

float jce_gas_attribute_current(const JceGameplayAbilitySystem *gas,
                                int32_t attr_idx)
{
    if (!gas) return 0.0f;
    float add_sum, mult_product, override_value;
    bool  has_override;
    fold_continuous(gas, attr_idx, &add_sum, &mult_product,
                    &override_value, &has_override);
    return jce_attribute_set_get_current(&gas->attributes, attr_idx,
                                         add_sum, mult_product,
                                         override_value, has_override);
}

float jce_gas_get_modifier_sum(const JceGameplayAbilitySystem *gas,
                               int32_t attr_idx)
{
    if (!gas) return 0.0f;
    float add_sum, mult_product, override_value;
    bool  has_override;
    fold_continuous(gas, attr_idx, &add_sum, &mult_product,
                    &override_value, &has_override);
    return add_sum;
}

int32_t jce_gas_active_effect_count(const JceGameplayAbilitySystem *gas)
{
    return gas ? gas->active_effect_count : 0;
}

int32_t jce_gas_attribute_count(const JceGameplayAbilitySystem *gas)
{
    return gas ? gas->attributes.count : 0;
}

const char *jce_gas_attribute_name(const JceGameplayAbilitySystem *gas,
                                   int32_t attr_idx)
{
    if (!gas || attr_idx < 0 || attr_idx >= gas->attributes.count) return NULL;
    return gas->attributes.attributes[attr_idx].name;
}

float jce_gas_attribute_get_current_by_index(const JceGameplayAbilitySystem *gas,
                                             int32_t attr_idx)
{
    /* Same composition as jce_gas_attribute_current(); the by-index name is
     * just an explicit alias for serialization callers. */
    return jce_gas_attribute_current(gas, attr_idx);
}

bool jce_gas_attribute_set_base(JceGameplayAbilitySystem *gas,
                                const char *name, float base)
{
    int32_t idx;
    JceAttribute *a;
    if (!gas || !name) return false;
    idx = jce_attribute_set_find(&gas->attributes, name);
    if (idx < 0) return false;
    a = &gas->attributes.attributes[idx];
    a->base = clampf(base, a->min, a->max);
    return true;
}

/* ------------------------------------------------------------------ */
/* Abilities.                                                          */
/* ------------------------------------------------------------------ */

static int32_t find_ability(const JceGameplayAbilitySystem *gas, uint32_t id)
{
    for (int32_t i = 0; i < gas->ability_count; ++i) {
        if (gas->abilities[i].def.id == id)
            return i;
    }
    return -1;
}

int32_t jce_gas_register_ability(JceGameplayAbilitySystem *gas,
                                 const JceAbilityDef *def)
{
    if (!gas || !def) return -1;
    if (gas->ability_count >= JCE_GAS_MAX_ABILITIES) return -1;
    if (def->granted_count < 0 || def->granted_count > JCE_GAS_MAX_GRANTED)
        return -1;

    int32_t idx = gas->ability_count;
    JceAbilityInstance *inst = &gas->abilities[idx];
    memset(inst, 0, sizeof(*inst));
    inst->def                = *def;
    inst->cooldown_remaining = 0.0f;
    gas->ability_count = idx + 1;
    return idx;
}

bool jce_gas_activate_ability(JceGameplayAbilitySystem *gas, uint32_t ability_id)
{
    if (!gas) return false;
    int32_t idx = find_ability(gas, ability_id);
    if (idx < 0) return false;

    JceAbilityInstance *inst = &gas->abilities[idx];
    const JceAbilityDef *def = &inst->def;

    /* Cooldown gate. */
    if (inst->cooldown_remaining > 0.0f) return false;

    /* Cost gate (against the live current value). */
    if (def->cost_attr_idx >= 0) {
        float cur = jce_gas_attribute_current(gas, def->cost_attr_idx);
        if (cur < def->cost_magnitude) return false;
    }

    /* Commit: deduct cost from base, start cooldown. */
    if (def->cost_attr_idx >= 0) {
        apply_to_base(gas, def->cost_attr_idx, JCE_GAS_OP_ADD,
                      -def->cost_magnitude, 1);
    }
    inst->cooldown_remaining = def->cooldown_seconds;

    /* Apply granted effects. */
    for (int32_t i = 0; i < def->granted_count; ++i)
        jce_gas_apply_effect(gas, &def->granted_effects[i]);

    return true;
}

float jce_gas_ability_cooldown_remaining(const JceGameplayAbilitySystem *gas,
                                         uint32_t ability_id)
{
    if (!gas) return 0.0f;
    int32_t idx = find_ability(gas, ability_id);
    if (idx < 0) return 0.0f;
    return gas->abilities[idx].cooldown_remaining;
}
