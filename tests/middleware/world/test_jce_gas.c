/*
 * test_jce_gas.c — Unit tests for jce_gas.h (L4 world).
 *
 * Pure logic, deterministic, headless.  All time advances use fixed dt
 * and float comparisons use EPS = 1e-4.
 *
 * Coverage:
 *   - attribute init / add / clamp on add
 *   - jce_attribute_set_find / get_current math primitive
 *   - INSTANT effect permanently changes base (-10 health -> 90)
 *   - continuous TIMED ADD modifier shows in current, then expires
 *   - PERIODIC poison ticks at the right cadence and expires
 *   - MULT op (continuous)
 *   - OVERRIDE op wins over ADD/MULT
 *   - ability cooldown blocks reactivation until elapsed
 *   - ability cost deducted from base + blocks when insufficient
 *   - AGGREGATE stacking sums; REPLACE replaces
 *   - clamp at min / max
 *   - NULL safety
 */

#include "unity.h"

#include <jce/middleware/world/jce_gas.h>

#include <string.h>

#define EPS 1e-4f

void setUp(void)    {}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

/* Build a gas with Health[100,0..200], Mana[50,0..50], Speed[10,0..100]. */
static void make_gas(JceGameplayAbilitySystem *gas,
                     int32_t *health, int32_t *mana, int32_t *speed)
{
    jce_gas_init(gas);
    *health = jce_attribute_set_add(&gas->attributes, "Health", 100.0f, 0.0f, 200.0f);
    *mana   = jce_attribute_set_add(&gas->attributes, "Mana",    50.0f, 0.0f,  50.0f);
    *speed  = jce_attribute_set_add(&gas->attributes, "Speed",   10.0f, 0.0f, 100.0f);
}

static JceGameplayEffect make_effect(int32_t attr, JceGameplayEffectOp op,
                                     float mag, JceGameplayEffectDuration dur,
                                     float dur_s, float period,
                                     JceGameplayEffectStackPolicy stack,
                                     uint16_t tag)
{
    JceGameplayEffect e;
    memset(&e, 0, sizeof(e));
    e.attr_idx         = attr;
    e.op               = op;
    e.magnitude        = mag;
    e.duration_mode    = dur;
    e.duration_seconds = dur_s;
    e.period_seconds   = period;
    e.stack_policy     = stack;
    e.tag_id           = tag;
    return e;
}

/* ------------------------------------------------------------------ */
/* Attributes                                                          */
/* ------------------------------------------------------------------ */

static void test_attribute_init_add_find(void)
{
    JceAttributeSet set;
    jce_attribute_set_init(&set);
    TEST_ASSERT_EQUAL_INT(0, set.count);

    int32_t h = jce_attribute_set_add(&set, "Health", 100.0f, 0.0f, 200.0f);
    int32_t m = jce_attribute_set_add(&set, "Mana", 50.0f, 0.0f, 50.0f);
    TEST_ASSERT_EQUAL_INT(0, h);
    TEST_ASSERT_EQUAL_INT(1, m);
    TEST_ASSERT_EQUAL_INT(2, set.count);

    TEST_ASSERT_EQUAL_INT(0, jce_attribute_set_find(&set, "Health"));
    TEST_ASSERT_EQUAL_INT(1, jce_attribute_set_find(&set, "Mana"));
    TEST_ASSERT_EQUAL_INT(-1, jce_attribute_set_find(&set, "Nope"));
}

static void test_attribute_clamp_on_add(void)
{
    JceAttributeSet set;
    jce_attribute_set_init(&set);
    /* base above max -> clamped down to max. */
    int32_t i = jce_attribute_set_add(&set, "Over", 500.0f, 0.0f, 100.0f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 100.0f, set.attributes[i].base);
    /* base below min -> clamped up to min. */
    int32_t j = jce_attribute_set_add(&set, "Under", -5.0f, 10.0f, 100.0f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 10.0f, set.attributes[j].base);
}

static void test_get_current_primitive(void)
{
    JceAttributeSet set;
    jce_attribute_set_init(&set);
    int32_t i = jce_attribute_set_add(&set, "H", 100.0f, 0.0f, 200.0f);

    /* (base + add) * mult */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 110.0f,
        jce_attribute_set_get_current(&set, i, 10.0f, 1.0f, 0.0f, false));
    /* (100 + 100) * 1.1 = 220 -> clamped to max 200 */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 200.0f,
        jce_attribute_set_get_current(&set, i, 100.0f, 1.1f, 0.0f, false));
    /* override wins, then clamped */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 200.0f,
        jce_attribute_set_get_current(&set, i, 999.0f, 999.0f, 9999.0f, true));
}

/* ------------------------------------------------------------------ */
/* INSTANT                                                             */
/* ------------------------------------------------------------------ */

static void test_instant_changes_base(void)
{
    JceGameplayAbilitySystem gas;
    int32_t h, m, s;
    make_gas(&gas, &h, &m, &s);

    JceGameplayEffect dmg = make_effect(h, JCE_GAS_OP_ADD, -10.0f,
                                        JCE_GAS_DURATION_INSTANT,
                                        0.0f, 0.0f, JCE_GAS_STACK_REPLACE, 0);
    uint32_t handle = jce_gas_apply_effect(&gas, &dmg);
    /* INSTANT returns 0 (not stored). */
    TEST_ASSERT_EQUAL_UINT32(0u, handle);
    TEST_ASSERT_EQUAL_INT(0, jce_gas_active_effect_count(&gas));
    /* base directly changed 100 -> 90, and current matches base. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 90.0f, gas.attributes.attributes[h].base);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 90.0f, jce_gas_attribute_current(&gas, h));
}

/* ------------------------------------------------------------------ */
/* Continuous TIMED modifier                                          */
/* ------------------------------------------------------------------ */

static void test_continuous_timed_add_then_expires(void)
{
    JceGameplayAbilitySystem gas;
    int32_t h, m, s;
    make_gas(&gas, &h, &m, &s);

    /* +20 speed for 2s, continuous (period 0). Speed base = 10. */
    JceGameplayEffect buff = make_effect(s, JCE_GAS_OP_ADD, 20.0f,
                                         JCE_GAS_DURATION_TIMED,
                                         2.0f, 0.0f, JCE_GAS_STACK_REPLACE, 1);
    uint32_t handle = jce_gas_apply_effect(&gas, &buff);
    TEST_ASSERT_NOT_EQUAL(0u, handle);
    TEST_ASSERT_EQUAL_INT(1, jce_gas_active_effect_count(&gas));

    /* Current reflects the modifier immediately; base untouched. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 30.0f, jce_gas_attribute_current(&gas, s));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 10.0f, gas.attributes.attributes[s].base);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 20.0f, jce_gas_get_modifier_sum(&gas, s));

    /* Half-life: still active. */
    jce_gas_tick(&gas, 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 30.0f, jce_gas_attribute_current(&gas, s));

    /* Reach 2.0s -> expires; current falls back to base. */
    jce_gas_tick(&gas, 1.0f);
    TEST_ASSERT_EQUAL_INT(0, jce_gas_active_effect_count(&gas));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 10.0f, jce_gas_attribute_current(&gas, s));
}

/* ------------------------------------------------------------------ */
/* PERIODIC poison cadence                                            */
/* ------------------------------------------------------------------ */

static void test_periodic_poison_cadence(void)
{
    JceGameplayAbilitySystem gas;
    int32_t h, m, s;
    make_gas(&gas, &h, &m, &s);

    /* Poison: -5 health each 1.0s, TIMED 2.0s.  Health base 100. */
    JceGameplayEffect poison = make_effect(h, JCE_GAS_OP_ADD, -5.0f,
                                           JCE_GAS_DURATION_TIMED,
                                           2.0f, 1.0f, JCE_GAS_STACK_REPLACE, 2);
    jce_gas_apply_effect(&gas, &poison);

    /* tick 0.5: no boundary crossed -> no change. */
    jce_gas_tick(&gas, 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 100.0f, gas.attributes.attributes[h].base);

    /* tick +0.6 (elapsed 1.1): crosses 1.0 -> 1 tick (-5 -> 95). */
    jce_gas_tick(&gas, 0.6f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 95.0f, gas.attributes.attributes[h].base);

    /* tick +1.0 (elapsed 2.1): crosses 2.0 -> 2nd tick (-5 -> 90), expires. */
    jce_gas_tick(&gas, 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 90.0f, gas.attributes.attributes[h].base);
    TEST_ASSERT_EQUAL_INT(0, jce_gas_active_effect_count(&gas));

    /* Periodic effects live in base, so current == base now. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 90.0f, jce_gas_attribute_current(&gas, h));
}

/* ------------------------------------------------------------------ */
/* MULT + OVERRIDE                                                     */
/* ------------------------------------------------------------------ */

static void test_mult_op(void)
{
    JceGameplayAbilitySystem gas;
    int32_t h, m, s;
    make_gas(&gas, &h, &m, &s);

    /* x1.5 speed, continuous infinite. base 10 -> current 15. */
    JceGameplayEffect haste = make_effect(s, JCE_GAS_OP_MULT, 1.5f,
                                          JCE_GAS_DURATION_INFINITE,
                                          0.0f, 0.0f, JCE_GAS_STACK_REPLACE, 3);
    jce_gas_apply_effect(&gas, &haste);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 15.0f, jce_gas_attribute_current(&gas, s));
}

static void test_override_wins(void)
{
    JceGameplayAbilitySystem gas;
    int32_t h, m, s;
    make_gas(&gas, &h, &m, &s);

    /* +50 add and x2 mult would give (10+50)*2 = 120, but override -> 1. */
    JceGameplayEffect add = make_effect(s, JCE_GAS_OP_ADD, 50.0f,
                                        JCE_GAS_DURATION_INFINITE,
                                        0.0f, 0.0f, JCE_GAS_STACK_REPLACE, 10);
    JceGameplayEffect mul = make_effect(s, JCE_GAS_OP_MULT, 2.0f,
                                        JCE_GAS_DURATION_INFINITE,
                                        0.0f, 0.0f, JCE_GAS_STACK_REPLACE, 11);
    JceGameplayEffect ovr = make_effect(s, JCE_GAS_OP_OVERRIDE, 1.0f,
                                        JCE_GAS_DURATION_INFINITE,
                                        0.0f, 0.0f, JCE_GAS_STACK_REPLACE, 12);
    jce_gas_apply_effect(&gas, &add);
    jce_gas_apply_effect(&gas, &mul);
    jce_gas_apply_effect(&gas, &ovr);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, jce_gas_attribute_current(&gas, s));
}

/* ------------------------------------------------------------------ */
/* Stacking                                                            */
/* ------------------------------------------------------------------ */

static void test_aggregate_stacking_sums(void)
{
    JceGameplayAbilitySystem gas;
    int32_t h, m, s;
    make_gas(&gas, &h, &m, &s);

    /* +5 speed, AGGREGATE, same tag applied 3 times -> +15, one slot. */
    JceGameplayEffect buff = make_effect(s, JCE_GAS_OP_ADD, 5.0f,
                                         JCE_GAS_DURATION_INFINITE,
                                         0.0f, 0.0f, JCE_GAS_STACK_AGGREGATE, 20);
    jce_gas_apply_effect(&gas, &buff);
    jce_gas_apply_effect(&gas, &buff);
    jce_gas_apply_effect(&gas, &buff);

    TEST_ASSERT_EQUAL_INT(1, jce_gas_active_effect_count(&gas));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 15.0f, jce_gas_get_modifier_sum(&gas, s));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 25.0f, jce_gas_attribute_current(&gas, s));
}

static void test_replace_stacking(void)
{
    JceGameplayAbilitySystem gas;
    int32_t h, m, s;
    make_gas(&gas, &h, &m, &s);

    JceGameplayEffect a = make_effect(s, JCE_GAS_OP_ADD, 5.0f,
                                      JCE_GAS_DURATION_INFINITE,
                                      0.0f, 0.0f, JCE_GAS_STACK_REPLACE, 30);
    JceGameplayEffect b = make_effect(s, JCE_GAS_OP_ADD, 7.0f,
                                      JCE_GAS_DURATION_INFINITE,
                                      0.0f, 0.0f, JCE_GAS_STACK_REPLACE, 30);
    jce_gas_apply_effect(&gas, &a);
    jce_gas_apply_effect(&gas, &b);
    /* Same tag -> the first is replaced; only b (+7) remains. */
    TEST_ASSERT_EQUAL_INT(1, jce_gas_active_effect_count(&gas));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 7.0f, jce_gas_get_modifier_sum(&gas, s));
}

/* ------------------------------------------------------------------ */
/* Clamp                                                               */
/* ------------------------------------------------------------------ */

static void test_clamp_min_max(void)
{
    JceGameplayAbilitySystem gas;
    int32_t h, m, s;
    make_gas(&gas, &h, &m, &s);

    /* Massive add to speed -> clamp to max 100. */
    JceGameplayEffect big = make_effect(s, JCE_GAS_OP_ADD, 9999.0f,
                                        JCE_GAS_DURATION_INFINITE,
                                        0.0f, 0.0f, JCE_GAS_STACK_REPLACE, 40);
    jce_gas_apply_effect(&gas, &big);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 100.0f, jce_gas_attribute_current(&gas, s));

    /* Massive instant damage to health -> clamp to min 0. */
    JceGameplayEffect kill = make_effect(h, JCE_GAS_OP_ADD, -9999.0f,
                                         JCE_GAS_DURATION_INSTANT,
                                         0.0f, 0.0f, JCE_GAS_STACK_REPLACE, 0);
    jce_gas_apply_effect(&gas, &kill);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, gas.attributes.attributes[h].base);
}

/* ------------------------------------------------------------------ */
/* Abilities: cooldown + cost                                          */
/* ------------------------------------------------------------------ */

static void test_ability_cooldown_blocks_reactivation(void)
{
    JceGameplayAbilitySystem gas;
    int32_t h, m, s;
    make_gas(&gas, &h, &m, &s);

    JceAbilityDef def;
    memset(&def, 0, sizeof(def));
    strncpy(def.name, "Dash", JCE_GAS_NAME_LEN - 1);
    def.id               = 1001u;
    def.cost_attr_idx    = -1;          /* free */
    def.cooldown_seconds = 2.0f;
    def.granted_count    = 0;
    TEST_ASSERT_TRUE(jce_gas_register_ability(&gas, &def) >= 0);

    TEST_ASSERT_TRUE(jce_gas_activate_ability(&gas, 1001u));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f,
        jce_gas_ability_cooldown_remaining(&gas, 1001u));

    /* On cooldown -> blocked. */
    TEST_ASSERT_FALSE(jce_gas_activate_ability(&gas, 1001u));

    jce_gas_tick(&gas, 1.0f);
    TEST_ASSERT_FALSE(jce_gas_activate_ability(&gas, 1001u));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f,
        jce_gas_ability_cooldown_remaining(&gas, 1001u));

    /* Cooldown elapsed -> ready again. */
    jce_gas_tick(&gas, 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f,
        jce_gas_ability_cooldown_remaining(&gas, 1001u));
    TEST_ASSERT_TRUE(jce_gas_activate_ability(&gas, 1001u));
}

static void test_ability_cost_deducts_and_blocks(void)
{
    JceGameplayAbilitySystem gas;
    int32_t h, m, s;
    make_gas(&gas, &h, &m, &s);

    /* Mana base 50; spell costs 30. */
    JceAbilityDef def;
    memset(&def, 0, sizeof(def));
    strncpy(def.name, "Fireball", JCE_GAS_NAME_LEN - 1);
    def.id               = 2002u;
    def.cost_attr_idx    = m;
    def.cost_magnitude   = 30.0f;
    def.cooldown_seconds = 0.0f;
    def.granted_count    = 0;
    jce_gas_register_ability(&gas, &def);

    /* First cast: 50 -> 20. */
    TEST_ASSERT_TRUE(jce_gas_activate_ability(&gas, 2002u));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 20.0f, gas.attributes.attributes[m].base);

    /* Second cast: 20 < 30 -> blocked, base unchanged. */
    TEST_ASSERT_FALSE(jce_gas_activate_ability(&gas, 2002u));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 20.0f, gas.attributes.attributes[m].base);
}

static void test_ability_grants_effect(void)
{
    JceGameplayAbilitySystem gas;
    int32_t h, m, s;
    make_gas(&gas, &h, &m, &s);

    JceAbilityDef def;
    memset(&def, 0, sizeof(def));
    strncpy(def.name, "Sprint", JCE_GAS_NAME_LEN - 1);
    def.id            = 3003u;
    def.cost_attr_idx = -1;
    def.granted_count = 1;
    def.granted_effects[0] = make_effect(s, JCE_GAS_OP_ADD, 20.0f,
                                         JCE_GAS_DURATION_TIMED,
                                         3.0f, 0.0f, JCE_GAS_STACK_REPLACE, 50);
    jce_gas_register_ability(&gas, &def);

    TEST_ASSERT_TRUE(jce_gas_activate_ability(&gas, 3003u));
    TEST_ASSERT_EQUAL_INT(1, jce_gas_active_effect_count(&gas));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 30.0f, jce_gas_attribute_current(&gas, s));
}

/* ------------------------------------------------------------------ */
/* Remove / clear / NULL                                               */
/* ------------------------------------------------------------------ */

static void test_remove_and_clear(void)
{
    JceGameplayAbilitySystem gas;
    int32_t h, m, s;
    make_gas(&gas, &h, &m, &s);

    JceGameplayEffect e = make_effect(s, JCE_GAS_OP_ADD, 5.0f,
                                      JCE_GAS_DURATION_INFINITE,
                                      0.0f, 0.0f, JCE_GAS_STACK_REPLACE, 60);
    uint32_t handle = jce_gas_apply_effect(&gas, &e);
    TEST_ASSERT_EQUAL_INT(1, jce_gas_active_effect_count(&gas));
    TEST_ASSERT_TRUE(jce_gas_remove_effect(&gas, handle));
    TEST_ASSERT_EQUAL_INT(0, jce_gas_active_effect_count(&gas));
    TEST_ASSERT_FALSE(jce_gas_remove_effect(&gas, handle));

    jce_gas_apply_effect(&gas, &e);
    jce_gas_apply_effect(&gas, &e); /* REPLACE same tag -> still 1 */
    TEST_ASSERT_EQUAL_INT(1, jce_gas_active_effect_count(&gas));
    jce_gas_clear_effects(&gas);
    TEST_ASSERT_EQUAL_INT(0, jce_gas_active_effect_count(&gas));
}

static void test_bad_attr_and_null_safety(void)
{
    JceGameplayAbilitySystem gas;
    int32_t h, m, s;
    make_gas(&gas, &h, &m, &s);

    /* Bad attr index -> apply returns 0, nothing stored. */
    JceGameplayEffect bad = make_effect(99, JCE_GAS_OP_ADD, 5.0f,
                                        JCE_GAS_DURATION_INFINITE,
                                        0.0f, 0.0f, JCE_GAS_STACK_REPLACE, 70);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_gas_apply_effect(&gas, &bad));
    TEST_ASSERT_EQUAL_INT(0, jce_gas_active_effect_count(&gas));

    /* NULL safety: must not crash, sane returns. */
    jce_gas_init(NULL);
    jce_attribute_set_init(NULL);
    TEST_ASSERT_EQUAL_INT(-1, jce_attribute_set_add(NULL, "X", 0.0f, 0.0f, 1.0f));
    TEST_ASSERT_EQUAL_INT(-1, jce_attribute_set_find(NULL, "X"));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_gas_apply_effect(NULL, &bad));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_gas_apply_effect(&gas, NULL));
    jce_gas_tick(NULL, 0.1f);
    TEST_ASSERT_FALSE(jce_gas_remove_effect(NULL, 1u));
    jce_gas_clear_effects(NULL);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, jce_gas_attribute_current(NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, jce_gas_active_effect_count(NULL));
    TEST_ASSERT_EQUAL_INT(-1, jce_gas_register_ability(NULL, NULL));
    TEST_ASSERT_FALSE(jce_gas_activate_ability(NULL, 1u));
    TEST_ASSERT_FALSE(jce_gas_activate_ability(&gas, 99999u)); /* unknown id */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f,
        jce_gas_ability_cooldown_remaining(NULL, 1u));
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_attribute_init_add_find);
    RUN_TEST(test_attribute_clamp_on_add);
    RUN_TEST(test_get_current_primitive);
    RUN_TEST(test_instant_changes_base);
    RUN_TEST(test_continuous_timed_add_then_expires);
    RUN_TEST(test_periodic_poison_cadence);
    RUN_TEST(test_mult_op);
    RUN_TEST(test_override_wins);
    RUN_TEST(test_aggregate_stacking_sums);
    RUN_TEST(test_replace_stacking);
    RUN_TEST(test_clamp_min_max);
    RUN_TEST(test_ability_cooldown_blocks_reactivation);
    RUN_TEST(test_ability_cost_deducts_and_blocks);
    RUN_TEST(test_ability_grants_effect);
    RUN_TEST(test_remove_and_clear);
    RUN_TEST(test_bad_attr_and_null_safety);
    return UNITY_END();
}
