/* test_jce_script_component.c
 *
 * Headless coverage for the lightweight runtime-reflection bindings:
 *   jce.has_component(entity, name)         -> bool
 *   jce.is_component_enabled(entity, name)  -> bool
 *   jce.set_component_enabled(entity, name, on)
 *
 * The VM (jce_script.c) marshals each call to a JceScriptHost callback; the
 * runtime backs them with the dense component registry (jce_component_find +
 * jce_scene_has_comp / comp_enabled / set_comp_enabled — already covered).  This
 * test drives a MOCK host that answers the queries + records the set call, and
 * asserts the C<->Lua marshalling AND the bool return round-trip (read back via
 * an assert-chunk on the same VM, mirroring test_jce_script_message), plus the
 * NULL-callback safe defaults.
 */

#include <jce/middleware/script/jce_script.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    int    set_calls;
    char   set_name[64];
    bool   set_on;
    JceScriptEntity last_entity;
} Recorder;

static Recorder g_rec;

/* Mock: only "Health" is present + enabled. */
static bool mock_has(void *user, JceScriptEntity e, const char *name)
{
    Recorder *r = (Recorder *)user; r->last_entity = e;
    return name && strcmp(name, "Health") == 0;
}
static bool mock_enabled(void *user, JceScriptEntity e, const char *name)
{
    Recorder *r = (Recorder *)user; r->last_entity = e;
    return name && strcmp(name, "Health") == 0;
}
static void mock_set(void *user, JceScriptEntity e, const char *name, bool on)
{
    Recorder *r = (Recorder *)user;
    r->set_calls++; r->last_entity = e; r->set_on = on;
    snprintf(r->set_name, sizeof r->set_name, "%s", name ? name : "");
}

void setUp(void)    { memset(&g_rec, 0, sizeof g_rec); }
void tearDown(void) {}

/* ── has / is_enabled return round-trip + entity marshalling ────────────── */
static void test_query_bindings_round_trip(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user                 = &g_rec;
    h.has_component        = mock_has;
    h.is_component_enabled = mock_enabled;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance a = jce_script_instantiate_source(s, "@q",
        "RES = {\n"
        "  yes = jce.has_component(42, 'Health'),\n"
        "  no  = jce.has_component(42, 'Nope'),\n"
        "  en  = jce.is_component_enabled(42, 'Health'),\n"
        "  dis = jce.is_component_enabled(42, 'Nope'),\n"
        "}\n"
        "return {}\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, a);
    TEST_ASSERT_EQUAL_UINT64(42u, g_rec.last_entity);

    JceScriptInstance chk = jce_script_instantiate_source(s, "@q_check",
        "assert(RES.yes == true,  'has Health -> true')\n"
        "assert(RES.no  == false, 'has Nope -> false')\n"
        "assert(RES.en  == true,  'Health enabled -> true')\n"
        "assert(RES.dis == false, 'Nope enabled -> false')\n"
        "return {}\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, chk);

    jce_script_destroy(s);
}

/* ── set_component_enabled marshals (entity, name, on) ──────────────────── */
static void test_set_binding_marshals(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user                  = &g_rec;
    h.set_component_enabled = mock_set;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance a = jce_script_instantiate_source(s, "@set_off",
        "jce.set_component_enabled(7, 'MeshRenderer', false)\n"
        "return {}\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, a);
    TEST_ASSERT_EQUAL_INT(1, g_rec.set_calls);
    TEST_ASSERT_EQUAL_UINT64(7u, g_rec.last_entity);
    TEST_ASSERT_EQUAL_STRING("MeshRenderer", g_rec.set_name);
    TEST_ASSERT_FALSE(g_rec.set_on);

    JceScriptInstance b = jce_script_instantiate_source(s, "@set_on",
        "jce.set_component_enabled(7, 'Light', true)\n"
        "return {}\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, b);
    TEST_ASSERT_EQUAL_INT(2, g_rec.set_calls);
    TEST_ASSERT_EQUAL_STRING("Light", g_rec.set_name);
    TEST_ASSERT_TRUE(g_rec.set_on);

    jce_script_destroy(s);
}

/* ── NULL host callbacks → safe defaults (query false, set no-op) ───────── */
static void test_null_host_safe_defaults(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user = &g_rec;             /* have_host true, all component cbs NULL */

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance a = jce_script_instantiate_source(s, "@null",
        "RES = { has = jce.has_component(1,'X'), en = jce.is_component_enabled(1,'X') }\n"
        "jce.set_component_enabled(1, 'X', true)\n"
        "return {}\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, a);          /* survived */
    TEST_ASSERT_EQUAL_INT(0, g_rec.set_calls);

    JceScriptInstance chk = jce_script_instantiate_source(s, "@null_check",
        "assert(RES.has == false, 'NULL has -> false')\n"
        "assert(RES.en  == false, 'NULL enabled -> false')\n"
        "return {}\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, chk);

    jce_script_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_query_bindings_round_trip);
    RUN_TEST(test_set_binding_marshals);
    RUN_TEST(test_null_host_safe_defaults);
    return UNITY_END();
}
