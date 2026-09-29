/*
 * test_jce_scene_gas_authoring.c
 *
 * GAS consumption last-mile — Gameplay Ability System AUTHORING as a scene
 * component.
 *
 * The GAS CORE (jce_gas.h — attributes/effects/abilities) is unit-tested
 * elsewhere (tests/middleware/world/test_jce_gas.c).  THIS test closes the
 * SCENE authoring gap:
 *
 *   1. The new presence-gated JceGameplayAbilitySystem scene component
 *      round-trips field-for-field through the REAL component JSON serializer
 *      (jce_scene_save_json -> jce_scene_load_json): the attribute table
 *      (name/base/min/max) and the ability table (name/id/cost-attr/cost/
 *      cooldown).
 *   2. An entity with NO component reloads with none (byte-identical default
 *      — legacy scenes stay unchanged).
 *   3. An empty (default-added) component round-trips as 0 attrs / 0 abilities
 *      and stays present.
 *
 * The live runtime ability system (active effects, cooldown draining) and the
 * editor inspector card are tested / exercised separately.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

typedef struct {
    const char *want;
    JceEntity   found;
} FindCtx;

static void find_cb(JceScene *s, JceEntity e, void *ud)
{
    FindCtx *ctx = (FindCtx *)ud;
    const char *nm = jce_scene_entity_registered_name(s, e);
    if (nm && strcmp(nm, ctx->want) == 0)
        ctx->found = e;
}

static JceEntity find_by_name(JceScene *s, const char *name)
{
    FindCtx ctx = { name, JCE_ENTITY_INVALID };
    jce_scene_each_entity(s, find_cb, &ctx);
    return ctx.found;
}

/* ── Full attribute + ability table round-trip ──────────────────────── */

static void test_gas_round_trip(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    JceEntity e = jce_scene_create_entity(src, "Hero");

    JceGameplayAbilitySystemComponent gas;
    memset(&gas, 0, sizeof gas);

    /* Two attributes. */
    snprintf(gas.attributes[0].name, sizeof gas.attributes[0].name, "%s", "Health");
    gas.attributes[0].base = 100.0f; gas.attributes[0].min = 0.0f; gas.attributes[0].max = 150.0f;
    snprintf(gas.attributes[1].name, sizeof gas.attributes[1].name, "%s", "Mana");
    gas.attributes[1].base = 50.0f;  gas.attributes[1].min = 0.0f; gas.attributes[1].max = 50.0f;
    gas.attribute_count = 2;

    /* Two abilities (one Mana-costed with cooldown, one free). */
    snprintf(gas.abilities[0].name, sizeof gas.abilities[0].name, "%s", "Fireball");
    gas.abilities[0].id = 7; gas.abilities[0].cost_attr_idx = 1;
    gas.abilities[0].cost_magnitude = 20.0f; gas.abilities[0].cooldown_seconds = 2.5f;
    snprintf(gas.abilities[1].name, sizeof gas.abilities[1].name, "%s", "Dash");
    gas.abilities[1].id = 8; gas.abilities[1].cost_attr_idx = -1;
    gas.abilities[1].cost_magnitude = 0.0f; gas.abilities[1].cooldown_seconds = 0.5f;
    gas.ability_count = 2;

    jce_scene_set_gas(src, e, &gas);

    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Hero");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    TEST_ASSERT_TRUE(jce_scene_has_gas(dst, ne));
    JceGameplayAbilitySystemComponent *out = jce_scene_get_gas(dst, ne);
    TEST_ASSERT_NOT_NULL(out);

    /* Attributes. */
    TEST_ASSERT_EQUAL_INT(2, out->attribute_count);
    TEST_ASSERT_EQUAL_STRING("Health", out->attributes[0].name);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 100.0f, out->attributes[0].base);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f,   out->attributes[0].min);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 150.0f, out->attributes[0].max);
    TEST_ASSERT_EQUAL_STRING("Mana", out->attributes[1].name);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 50.0f, out->attributes[1].base);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 50.0f, out->attributes[1].max);

    /* Abilities. */
    TEST_ASSERT_EQUAL_INT(2, out->ability_count);
    TEST_ASSERT_EQUAL_STRING("Fireball", out->abilities[0].name);
    TEST_ASSERT_EQUAL_UINT32(7u, out->abilities[0].id);
    TEST_ASSERT_EQUAL_INT(1, out->abilities[0].cost_attr_idx);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 20.0f, out->abilities[0].cost_magnitude);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.5f,  out->abilities[0].cooldown_seconds);
    TEST_ASSERT_EQUAL_STRING("Dash", out->abilities[1].name);
    TEST_ASSERT_EQUAL_UINT32(8u, out->abilities[1].id);
    TEST_ASSERT_EQUAL_INT(-1, out->abilities[1].cost_attr_idx);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* An entity with NO GAS component must reload with none — the gameplay path
 * stays byte-identical to legacy scenes. */
static void test_no_gas_default(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();

    JceEntity e = jce_scene_create_entity(src, "Plain");
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(src, e, &t);

    JceJson *root = jce_scene_save_json(src);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Plain");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    TEST_ASSERT_FALSE(jce_scene_has_gas(dst, ne));

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* An empty (default-added) GAS component round-trips as 0/0 and stays present. */
static void test_gas_empty_round_trip(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();

    JceEntity e = jce_scene_create_entity(src, "Empty");
    JceGameplayAbilitySystemComponent gas;
    memset(&gas, 0, sizeof gas);
    jce_scene_set_gas(src, e, &gas);

    JceJson *root = jce_scene_save_json(src);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Empty");
    TEST_ASSERT_TRUE(jce_scene_has_gas(dst, ne));
    JceGameplayAbilitySystemComponent *out = jce_scene_get_gas(dst, ne);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_INT(0, out->attribute_count);
    TEST_ASSERT_EQUAL_INT(0, out->ability_count);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_gas_round_trip);
    RUN_TEST(test_no_gas_default);
    RUN_TEST(test_gas_empty_round_trip);
    return UNITY_END();
}
