/*
 * test_jce_runtime_curve.c — jce.curve_eval must answer a REAL curve file,
 * through the runtime's own host table.
 *
 * WHY THIS EXISTS.  The Curve Editor has been writing multi-channel curve
 * documents since it was built and NOTHING read them: no loader, no
 * evaluator, no component field, no script call.  A designer could draw a
 * curve and there was no way for the game to use it.  Closing that means five
 * things have to be right at once -- the parser, the umbrella header, the
 * host callback, the manifest, and seven generated language bindings -- and
 * four of them can be right while the fifth leaves every script reading nil.
 *
 * THE ASSERTION THIS FILE IS FOR is case 2.  A binding shaped
 *
 *     float curve_eval(...)          -- 0 when anything goes wrong
 *
 * passes every other case here.  It differs from the real one at exactly one
 * point: a curve that legitimately evaluates to ZERO.  So the probe samples
 * `kick` at t=0, where its authored value is 0.0, and asserts the call came
 * back as a NUMBER rather than nil -- which is the only place the fallible
 * shape is distinguishable from the lossy one, and therefore the only place
 * worth calling the point of the file.
 *
 * The mock-host binding tests cannot catch any of this: they install a mock
 * whose callbacks are all non-NULL, so they measure the marshalling and never
 * the runtime's own table.  This one runs the real jce_runtime_create /
 * jce_runtime_step path against a file on disk.
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>

#include "unity.h"

#include "jce_test_file_util.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define LUA_FILE   "test_curve_probe.lua"
#define CURVE_FILE "test_curve_probe.curve.json"

/* Channel 'kick' is LINEAR from (0,0) to (2,4): it evaluates to exactly 0.0
 * at t=0 and 2.0 at t=1.  The zero is the whole reason for the first key's
 * value -- see the header. */
static const char *const CURVE_DOC =
    "{\"tMin\":0,\"tMax\":2,\"vMin\":0,\"vMax\":4,\"active\":0,"
    " \"channels\":["
    "  {\"name\":\"kick\",\"visible\":true,\"color\":[1,0,0],\"keys\":["
    "     {\"t\":0.0,\"v\":0.0,\"tanIn\":0,\"tanOut\":0,\"interp\":0},"
    "     {\"t\":2.0,\"v\":4.0,\"tanIn\":0,\"tanOut\":0,\"interp\":0}]}]}";

static const char *const PROBE_LUA =
    "local M = {}\n"
    "local F = '" CURVE_FILE "'\n"
    "function M:on_update(dt)\n"
    "  local a = jce.curve_eval(F, 'kick', 1.0)\n"
    "  local z = jce.curve_eval(F, 'kick', 0.0)\n"
    "  local m = jce.curve_eval(F, 'no_such_channel', 1.0)\n"
    "  local f = jce.curve_eval('no_such_curve.json', 'kick', 1.0)\n"
    "  local d = jce.curve_eval(F, '', 1.0)\n"
    "  jce.set_position(self.entity, (z ~= nil) and 1 or 0, z or -9, a or -9)\n"
    "  jce.set_scale(self.entity, (m == nil) and 1 or 0,\n"
    "                             (f == nil) and 1 or 0, d or -9)\n"
    "end\n"
    "return M\n";

void setUp(void) {}
void tearDown(void)
{
    remove(LUA_FILE);
    remove(CURVE_FILE);
}

/* -7 on every axis: a value NEITHER answer can produce, so "the script never
 * ran" can never be read as "the binding answered". */
static JceEntity spawn_probe(JceScene *s)
{
    JceEntity e = jce_scene_create_entity(s, "probe");
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.position.x = t.position.y = t.position.z = -7.0f;
    t.scale.x    = t.scale.y    = t.scale.z    = -7.0f;
    jce_scene_set_transform(s, e, &t);
    return e;
}

static void test_curve_eval_answers_a_real_curve_file(void)
{
    JceScene      *s;
    JceEntity      probe;
    JceRuntimeDesc desc;
    JceRuntime    *rt;
    JceScriptComponent sc;

    jce_test_write_file(CURVE_FILE, CURVE_DOC);
    jce_test_write_file(LUA_FILE,   PROBE_LUA);

    s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    probe = spawn_probe(s);

    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", LUA_FILE);
    jce_scene_set_script(s, probe, &sc);
    jce_scene_set_component_enabled(s, probe, JCE_COMP_FLAG_SCRIPT, true);

    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;
    rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    /* Twice: the second step must come out of the cache and give the same
     * answers.  A cache that stored the wrong thing, or that cached a
     * FAILURE for a path that works, shows up here and nowhere else. */
    jce_runtime_step(rt, 0.016f);
    jce_runtime_step(rt, 0.016f);

    const JceTransform *p = jce_scene_get_transform(s, probe);
    TEST_ASSERT_NOT_NULL(p);
    printf("  pos=(%.4f, %.4f, %.4f)  scale=(%.4f, %.4f, %.4f)\n",
           p->position.x, p->position.y, p->position.z,
           p->scale.x, p->scale.y, p->scale.z);

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, p->position.x,
        "sampling a curve where its authored value is ZERO came back nil -- "
        "which means 'the curve is missing' and 'the curve says 0' are one "
        "reading, and every `jce.curve_eval(...) or default` in a project "
        "silently takes the default at every zero crossing.  -7 here instead "
        "means the probe script never ran at all, so nothing below means "
        "anything either");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, p->position.y,
        "the zero sample did not come back as 0.0");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2.0f, p->position.z,
        "the authored curve did not reach the script: the host callback is "
        "inert, or the file did not resolve, which is the defect this test "
        "exists for");

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, p->scale.x,
        "asking for a channel the document does not have returned a number "
        "instead of nil -- probably 0, which is a legitimate curve value");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, p->scale.y,
        "a curve file that does not exist returned a number instead of nil");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2.0f, p->scale.z,
        "an empty channel name did not resolve to the FIRST channel");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_curve_eval_answers_a_real_curve_file);
    return UNITY_END();
}
