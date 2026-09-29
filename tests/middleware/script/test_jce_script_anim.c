/* test_jce_script_anim.c
 *
 * Script-driven animation API (P0): jce.anim_set_float/int/bool/trigger.
 *   1. The Lua bindings marshal (entity, name, value) to the host callbacks.
 *   2. The scene anim-command relay round-trips + drains (push then take clears).
 * (The runtime wiring just pushes onto the relay; the renderer drains it into
 *  the SM binding — covered by compile + the relay round-trip here.)
 */

#include <jce/middleware/script/jce_script.h>
#include <jce/middleware/scene/jce_scene.h>

#include "unity.h"
#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── 1. Lua bindings reach the host callbacks ─────────────────────────── */

static char g_rec[512];
static void rec_trigger(void *u, JceScriptEntity e, const char *name)
{ (void)u; size_t n=strlen(g_rec); snprintf(g_rec+n,sizeof(g_rec)-n,"trig:%llu:%s|",(unsigned long long)e,name); }
static void rec_float(void *u, JceScriptEntity e, const char *name, float v)
{ (void)u;(void)e; size_t n=strlen(g_rec); snprintf(g_rec+n,sizeof(g_rec)-n,"float:%s=%.1f|",name,(double)v); }
static void rec_bool(void *u, JceScriptEntity e, const char *name, bool v)
{ (void)u;(void)e; size_t n=strlen(g_rec); snprintf(g_rec+n,sizeof(g_rec)-n,"bool:%s=%d|",name,(int)v); }

static void test_anim_bindings_reach_host(void)
{
    g_rec[0] = 0;
    JceScriptHost host; memset(&host, 0, sizeof host);
    host.anim_set_trigger = rec_trigger;
    host.anim_set_float   = rec_float;
    host.anim_set_bool    = rec_bool;
    JceScript *s = jce_script_create(&host);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst = jce_script_instantiate_source(s, "anim",
        "local M={}\n"
        "function M:on_update(dt)\n"
        "  jce.anim_set_float(self.entity, 'Speed', 2.5)\n"
        "  jce.anim_set_bool(self.entity, 'IsGrounded', true)\n"
        "  jce.anim_set_trigger(self.entity, 'Attack')\n"
        "end\n"
        "return M", 42);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    jce_script_call_update(s, inst, 0.016f);

    TEST_ASSERT_TRUE_MESSAGE(strstr(g_rec, "float:Speed=2.5") != NULL,
        "anim_set_float should reach the host with name+value");
    TEST_ASSERT_TRUE_MESSAGE(strstr(g_rec, "bool:IsGrounded=1") != NULL,
        "anim_set_bool should reach the host");
    TEST_ASSERT_TRUE_MESSAGE(strstr(g_rec, "trig:42:Attack") != NULL,
        "anim_set_trigger should reach the host with entity id + name");

    jce_script_destroy(s);
}

/* ── 2. Scene anim-command relay round-trips + drains ─────────────────── */

static void test_scene_anim_relay(void)
{
    JceScene *sc = jce_scene_create();
    TEST_ASSERT_NOT_NULL(sc);
    JceEntity e = jce_scene_create_entity(sc, "x");

    JceAnimParamCmd c; memset(&c, 0, sizeof c);
    c.type = JCE_ANIM_PARAM_TRIGGER; snprintf(c.name, sizeof c.name, "Attack");
    jce_scene_anim_push_param(sc, e, &c);
    memset(&c, 0, sizeof c);
    c.type = JCE_ANIM_PARAM_FLOAT; snprintf(c.name, sizeof c.name, "Speed"); c.value = 3.0f;
    jce_scene_anim_push_param(sc, e, &c);

    JceAnimParamCmd out[8];
    uint32_t n = jce_scene_anim_take_params(sc, e, out, 8);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_INT(JCE_ANIM_PARAM_TRIGGER, out[0].type);
    TEST_ASSERT_EQUAL_STRING("Attack", out[0].name);
    TEST_ASSERT_EQUAL_INT(JCE_ANIM_PARAM_FLOAT, out[1].type);
    TEST_ASSERT_EQUAL_STRING("Speed", out[1].name);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, out[1].value);

    /* drained: a second take returns nothing */
    TEST_ASSERT_EQUAL_UINT32(0, jce_scene_anim_take_params(sc, e, out, 8));

    jce_scene_destroy(sc);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_anim_bindings_reach_host);
    RUN_TEST(test_scene_anim_relay);
    return UNITY_END();
}
