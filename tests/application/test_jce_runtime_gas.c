/* test_jce_runtime_gas.c
 *
 * GAS consumption last-mile — proves a Lua script drives the LIVE runtime
 * Gameplay Ability System built from an entity's authored
 * JceGameplayAbilitySystemComponent.
 *
 * The GAS core (jce_gas.h) and the scene component round-trip are tested
 * elsewhere.  THIS test exercises the runtime wiring end-to-end:
 *
 *   - jce_runtime_create walks the scene (rt_spawn_gameplay), sees the authored
 *     GAS component, and inits a live JceGameplayAbilitySystem from its
 *     attribute + ability tables.
 *   - rt_tick_gameplay calls jce_gas_tick each step (cooldowns drain).
 *   - A Lua script on the SAME entity calls jce.gas_activate / jce.gas_get /
 *     jce.gas_apply through the host bridge, which resolves the entity's live
 *     GAS and calls jce_gas_*.
 *
 * Asserted: activating a Mana-costed ability deducts the cost from base Mana,
 * a second activation is blocked by cooldown (no further deduction), and an
 * instant damage effect applied from script lowers Health.  We read the live
 * system back through the public jce_runtime_entity_gas API.
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/world/jce_gas.h>

#include "unity.h"

#include "jce_test_file_util.h"

#include <stdio.h>
#include <string.h>

#define GAS_SCRIPT "jce_rt_gas_selftest.lua"

void setUp(void)    {}
void tearDown(void) { remove(GAS_SCRIPT); }

/* The script:
 *   on_start  : nothing (let the runtime seed base values).
 *   on_update : on the FIRST step, activate ability 1 (Fireball, costs 20 Mana)
 *               and apply an instant -30 to Health (op 0 = ADD, dur 0 = INSTANT);
 *               on EVERY step, try to activate again (the cooldown should block
 *               the second+ attempts so Mana is only deducted once).
 *               It also reads jce.gas_get("Mana") to prove the read binding
 *               returns a number (stashed in position.x for inspection). */
static void test_script_drives_live_gas(void)
{
    jce_test_write_file(GAS_SCRIPT,
        "local M = {}\n"
        "function M:on_update(dt)\n"
        "  if not self.fired then\n"
        "    self.fired = true\n"
        "    jce.gas_activate(self.entity, 1)\n"
        "    jce.gas_apply(self.entity, 'Health', 0, -30, 0)\n"
        "  else\n"
        "    jce.gas_activate(self.entity, 1)\n"   /* cooldown should block */
        "  end\n"
        "  local mana = jce.gas_get(self.entity, 'Mana') or -1\n"
        "  jce.set_position(self.entity, mana, 0, 0)\n"
        "end\n"
        "return M\n");

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "hero");

    JceTransform t;
    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);

    /* Author the GAS component: Health[100,0..100], Mana[50,0..50], and a
     * "Fireball" ability (id 1) costing 20 Mana on a 2 s cooldown. */
    JceGameplayAbilitySystemComponent gas;
    memset(&gas, 0, sizeof gas);
    snprintf(gas.attributes[0].name, sizeof gas.attributes[0].name, "%s", "Health");
    gas.attributes[0].base = 100.0f; gas.attributes[0].min = 0.0f; gas.attributes[0].max = 100.0f;
    snprintf(gas.attributes[1].name, sizeof gas.attributes[1].name, "%s", "Mana");
    gas.attributes[1].base = 50.0f;  gas.attributes[1].min = 0.0f; gas.attributes[1].max = 50.0f;
    gas.attribute_count = 2;
    snprintf(gas.abilities[0].name, sizeof gas.abilities[0].name, "%s", "Fireball");
    gas.abilities[0].id = 1; gas.abilities[0].cost_attr_idx = 1;
    gas.abilities[0].cost_magnitude = 20.0f; gas.abilities[0].cooldown_seconds = 2.0f;
    gas.ability_count = 1;
    jce_scene_set_gas(s, e, &gas);

    /* The script on the SAME entity. */
    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", GAS_SCRIPT);
    jce_scene_set_script(s, e, &sc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_SCRIPT, true);

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;

    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    /* The runtime must have built a live GAS for the entity. */
    JceGameplayAbilitySystem *live = jce_runtime_entity_gas(rt, (uint64_t)e);
    TEST_ASSERT_NOT_NULL(live);
    int32_t mana_idx   = jce_attribute_set_find(&live->attributes, "Mana");
    int32_t health_idx = jce_attribute_set_find(&live->attributes, "Health");
    TEST_ASSERT_TRUE(mana_idx >= 0);
    TEST_ASSERT_TRUE(health_idx >= 0);

    /* Before any step: seeded base values. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 50.0f,  jce_gas_attribute_current(live, mana_idx));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 100.0f, jce_gas_attribute_current(live, health_idx));

    /* Step 1: script fires Fireball (Mana 50 -> 30) and applies -30 Health
     * (Health 100 -> 70). */
    jce_runtime_step(rt, 1.0f / 60.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 30.0f, jce_gas_attribute_current(live, mana_idx));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 70.0f, jce_gas_attribute_current(live, health_idx));

    /* The script also read Mana back through jce.gas_get and stashed it in
     * position.x — proving the read binding returns the live value (30). */
    JceTransform *after = jce_scene_get_transform(s, e);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 30.0f, after->position.x);

    /* Steps 2..30 (~0.48 s total, still inside the 2 s cooldown): each tries to
     * re-activate but is blocked, so Mana stays at 30 (no further deduction). */
    for (int i = 0; i < 29; ++i)
        jce_runtime_step(rt, 1.0f / 60.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 30.0f, jce_gas_attribute_current(live, mana_idx));

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

/* An entity with NO GAS component yields NULL from jce_runtime_entity_gas and a
 * gas_activate/gas_get from a script is a harmless no-op (byte-identical path). */
static void test_no_gas_is_noop(void)
{
    jce_test_write_file(GAS_SCRIPT,
        "local M = {}\n"
        "function M:on_update(dt)\n"
        "  local ok = jce.gas_activate(self.entity, 1)\n"
        "  local v  = jce.gas_get(self.entity, 'Mana')\n"
        "  jce.set_position(self.entity, ok and 1 or 0, v and 1 or 0, 0)\n"
        "end\n"
        "return M\n");

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "plain");
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", GAS_SCRIPT);
    jce_scene_set_script(s, e, &sc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_SCRIPT, true);

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;

    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    /* No authored GAS -> no live system. */
    TEST_ASSERT_NULL(jce_runtime_entity_gas(rt, (uint64_t)e));

    jce_runtime_step(rt, 1.0f / 60.0f);

    /* gas_activate returned false (-> 0) and gas_get returned nil (-> 0). */
    JceTransform *after = jce_scene_get_transform(s, e);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, after->position.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, after->position.y);

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_script_drives_live_gas);
    RUN_TEST(test_no_gas_is_noop);
    return UNITY_END();
}
