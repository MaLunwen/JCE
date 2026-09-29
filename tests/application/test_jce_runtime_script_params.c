/*
 * test_jce_runtime_script_params.c — jce.get_param / jce.get_param_text must
 * answer the REAL component, through the runtime's own host table.
 *
 * WHY THIS EXISTS.  An exposed field is wired in six places -- the component,
 * the serialiser, the Inspector, the host callback, the manifest and seven
 * generated language bindings -- and five of those can be right while the
 * sixth leaves every script reading its default.  That failure is invisible:
 * `jce.get_param(e, "speed") or 3.0` returns 3.0 both when the author set
 * nothing AND when the whole feature is inert, and the two are byte-identical
 * at the call site.  This repository has shipped that exact shape more than
 * once (host.is_key_down with no writer; the cookie array nothing ever filled).
 *
 * The mock-host binding tests cannot catch it: they install a mock whose
 * callbacks are all non-NULL, so they measure the marshalling and never the
 * runtime's own table.  This one runs the REAL jce_runtime_create /
 * jce_runtime_step path, and the sentinel positions below are values NEITHER
 * answer can produce -- so "the script never ran" can never be read as "the
 * binding answered".
 *
 * SIX properties, and the ones worth the file are 3 and 5:
 *
 *   1. a name the author never set     -> nil (all three absences alike)
 *   2. a NUMBER row                    -> its value
 *   3. a BOOL row authored as 0.5      -> 1.0, NOT 0.5.  A bool slot must not
 *      hand a script a number that is neither true nor false.
 *   4. a TEXT row through get_param    -> kind 2, and number 0: the value is
 *      NOT carried here, but the KIND is, so a script that reached for the
 *      wrong accessor can tell the parameter exists.
 *   5. get_param_text on a NUMBER row  -> "", not the number spelled out.
 *   6. an ENTITY row                   -> the id itself, usable as an entity.
 *      Property 6 is asserted by USING it: the script writes through the id
 *      it was handed, so a wrong id moves nothing and the sink keeps its
 *      sentinel.
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>

#include "unity.h"

#include "jce_test_file_util.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define LUA_FILE "test_script_params.lua"

static const char *const PROBE_LUA =
    "local M = {}\n"
    "function M:on_update(dt)\n"
    "  local mk       = jce.get_param(self.entity, 'nope')\n"
    "  local nk, nv   = jce.get_param(self.entity, 'speed')\n"
    "  local bk, bv   = jce.get_param(self.entity, 'armed')\n"
    "  local tk, tv   = jce.get_param(self.entity, 'label')\n"
    "  local sk, sn, sink = jce.get_param(self.entity, 'sink')\n"
    "  local txt      = jce.get_param_text(self.entity, 'label')\n"
    "  local numtxt   = jce.get_param_text(self.entity, 'speed')\n"
    "  jce.set_position(self.entity, (mk == nil) and 1 or 0, nv or -9, bv or -9)\n"
    "  jce.set_scale(self.entity, tk or -9, tv or -9, sk or -9)\n"
    "  if sink ~= nil and sink ~= 0 then\n"
    "    jce.set_position(sink, (txt == 'Watchtower') and 1 or 0,\n"
    "                           (numtxt == '') and 1 or 0, 1)\n"
    "  end\n"
    "end\n"
    "return M\n";

void setUp(void)    {}
void tearDown(void) { remove(LUA_FILE); }

/* -7 on every axis.  Not a value either answer can produce, so a script that
 * never ran cannot be mistaken for one that answered. */
static JceEntity spawn(JceScene *s, const char *name)
{
    JceEntity e = jce_scene_create_entity(s, name);
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = -7.0f;
    t.position.x = t.position.y = t.position.z = -7.0f;
    jce_scene_set_transform(s, e, &t);
    return e;
}

static void set_param(JceScriptComponent *sc, int i, const char *name,
                      JceScriptParamKind kind, float number,
                      uint64_t entity, const char *text)
{
    JceScriptParam *pm = &sc->params[i];
    snprintf(pm->name, sizeof pm->name, "%s", name);
    pm->kind   = (uint32_t)kind;
    pm->number = number;
    pm->entity = entity;
    snprintf(pm->text, sizeof pm->text, "%s", text);
}

static JceTransform *xform(JceScene *s, JceEntity e)
{
    JceTransform *t = jce_scene_get_transform(s, e);
    TEST_ASSERT_NOT_NULL(t);
    return t;
}

static void test_get_param_answers_the_authored_component(void)
{
    JceScene      *s;
    JceEntity      probe, sink;
    JceRuntimeDesc desc;
    JceRuntime    *rt;

    jce_test_write_file(LUA_FILE, PROBE_LUA);

    s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    sink  = spawn(s, "sink");
    probe = spawn(s, "probe");

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", LUA_FILE);
    set_param(&sc, 0, "speed", JCE_SCRIPT_PARAM_NUMBER, 12.5f, 0, "");
    /* 0.5 in a BOOL slot.  Authored by hand or by a tool, and the host must
     * normalise it -- a script branching on it would otherwise get a number
     * that is neither true nor false. */
    set_param(&sc, 1, "armed", JCE_SCRIPT_PARAM_BOOL, 0.5f, 0, "");
    set_param(&sc, 2, "label", JCE_SCRIPT_PARAM_TEXT, 0.0f, 0, "Watchtower");
    set_param(&sc, 3, "sink",  JCE_SCRIPT_PARAM_ENTITY, 0.0f, (uint64_t)sink, "");
    sc.param_count = 4;
    jce_scene_set_script(s, probe, &sc);
    jce_scene_set_component_enabled(s, probe, JCE_COMP_FLAG_SCRIPT, true);

    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;
    rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    jce_runtime_step(rt, 0.016f);

    const JceTransform *p = xform(s, probe);
    printf("  probe pos=(%.3f, %.3f, %.3f)  scale=(%.3f, %.3f, %.3f)\n",
           p->position.x, p->position.y, p->position.z,
           p->scale.x, p->scale.y, p->scale.z);

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, p->position.x,
        "jce.get_param answered something for a name the author never set -- "
        "and -7 here means the probe script never ran at all, so nothing "
        "below would mean anything");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(12.5f, p->position.y,
        "the NUMBER parameter did not reach the script; the host callback is "
        "inert, which is the defect this test exists for");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, p->position.z,
        "a BOOL parameter authored as 0.5 reached the script un-normalised");

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE((float)JCE_SCRIPT_PARAM_TEXT, p->scale.x,
        "get_param did not report the KIND of a text parameter -- a script "
        "that reached for the wrong accessor cannot then tell the parameter "
        "exists from the author never having set it");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, p->scale.y,
        "get_param carried a value for a TEXT row; text goes through "
        "get_param_text, and a number here would be whatever happened to sit "
        "in the number slot");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE((float)JCE_SCRIPT_PARAM_ENTITY, p->scale.z,
        "the ENTITY parameter reported the wrong kind");

    const JceTransform *k = xform(s, sink);
    printf("  sink  pos=(%.3f, %.3f, %.3f)\n",
           k->position.x, k->position.y, k->position.z);

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, k->position.z,
        "the sink still holds its sentinel: the script was handed no usable "
        "entity for the ENTITY parameter, so it wrote nowhere");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, k->position.x,
        "jce.get_param_text did not return the authored text");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, k->position.y,
        "jce.get_param_text on a NUMBER row returned something other than "
        "'' -- a script comparing strings would branch on it");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_get_param_answers_the_authored_component);
    return UNITY_END();
}
