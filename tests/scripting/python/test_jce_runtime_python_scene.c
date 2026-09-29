/*
 * test_jce_runtime_python_scene.c — bob.lua and turret.py IN THE SAME SCENE,
 * both running, through the real jce_runtime_create / jce_runtime_step path
 * and the real CPython backend.
 *
 * This is the end the whole multi-language batch was aimed at, and until
 * per-script selection shipped it was unreachable: the runtime held ONE
 * JceScript handle and chose its language once per process from
 * JCE_SCRIPT_LANGUAGE, so a .py attached to an entity ran nothing and a
 * process that chose "python" ran no .lua either.
 *
 * WHY IT LIVES UNDER tests/scripting/python AND NOT tests/application.
 * It needs jce_script_vm_python, which exists only when CMake found an
 * embeddable CPython — this whole directory is already gated on that, and
 * putting it in tests/application would either drag that condition into the
 * application suite or make the suite fail to configure on a machine without
 * the Python development headers.  The build-independent half of the same
 * property (two languages in one scene, routed to their own VMs, with a
 * second language that needs no toolchain) is
 * tests/application/test_jce_runtime_multilang.c, which runs everywhere.
 *
 * WHAT THIS FILE FORBIDS, precisely:
 *
 *   1. A scene mixing .lua and .py running only one of them.
 *   2. Either language's instance being dispatched through the other's VM.
 *      Both VMs number instances from a small integer, so a mis-route is not
 *      an error return; here it would show up as one entity's sentinel not
 *      moving, which is what the per-entity assertions read.
 *   3. The two VMs sharing anything: each language's on_update must advance
 *      only its own entity.
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/script/jce_script_vm.h>

#include "jce_script_vm_python.h"

#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define jce_setenv(k, v) _putenv_s((k), (v))
#else
#  define jce_setenv(k, v) setenv((k), (v), 1)
#endif

#ifndef JCE_PY_TEST_PACKAGE_DIR
#  error "JCE_PY_TEST_PACKAGE_DIR must be defined by CMake"
#endif
#ifndef JCE_PY_TEST_API
#  error "JCE_PY_TEST_API must be defined by CMake"
#endif

#define LUA_FILE "jce_rt_pyscene_bob.lua"
#define PY_FILE  "jce_rt_pyscene_turret.py"

static void write_file(const char *path, const char *body)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs(body, f);
    fclose(f);
}

static int g_ready;

void setUp(void)
{
    if (g_ready) return;
    g_ready = 1;
    jce_setenv("JCE_SCRIPT_API", JCE_PY_TEST_API);
    TEST_ASSERT_TRUE_MESSAGE(
        jce_script_vm_python_add_path(JCE_PY_TEST_PACKAGE_DIR),
        "jce_script_vm_python_add_path refused the package directory");
    TEST_ASSERT_TRUE_MESSAGE(jce_script_vm_python_register(),
        "the python backend did not register, so this test would be "
        "measuring the absent-backend path instead of the mixed-scene one");
}

void tearDown(void)
{
    remove(LUA_FILE);
    remove(PY_FILE);
}

static JceEntity make_scripted(JceScene *s, const char *name, const char *path)
{
    JceEntity          e = jce_scene_create_entity(s, name);
    JceTransform       t;
    JceScriptComponent sc;

    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);

    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", path);
    jce_scene_set_script(s, e, &sc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_SCRIPT, true);
    return e;
}

static void test_a_lua_and_a_py_in_one_scene_both_run(void)
{
    JceScene      *s;
    JceRuntime    *rt;
    JceRuntimeDesc desc;
    JceEntity      e_lua, e_py;
    JceTransform  *a, *b;

    /* The claim is what SELECTS python for turret.py; without it the runtime
     * resolves the path to nothing and the entity never loads.  Asserted
     * here so a failure below cannot be blamed on CPython. */
    TEST_ASSERT_EQUAL_STRING_MESSAGE("python",
        jce_script_vm_language_for_path(PY_FILE),
        "the python backend registered but does not claim '.py'");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(JCE_SCRIPT_VM_DEFAULT_LANGUAGE,
        jce_script_vm_language_for_path(LUA_FILE),
        "'.lua' no longer resolves to the built-in");

    write_file(LUA_FILE,
        "local M = {}\n"
        "function M:on_start()\n"
        "  jce.set_position(self.entity, 11, 22, 33)\n"
        "end\n"
        "function M:on_update(dt)\n"
        "  local x,y,z = jce.get_position(self.entity)\n"
        "  jce.set_position(self.entity, x + 100, y, z)\n"
        "end\n"
        "return M\n");

    /* Per-instance state on `self`, not a module global: the point is that
     * the .py entity's own instance advanced, and a module global would keep
     * counting if the dispatch reached the wrong instance. */
    write_file(PY_FILE,
        "def on_start(self):\n"
        "    self.x = 44.0\n"
        "    jce.set_position(self.entity, self.x, 55.0, 66.0)\n"
        "\n"
        "def on_update(self, dt):\n"
        "    self.x = self.x + 7.0\n"
        "    jce.set_position(self.entity, self.x, 55.0, 66.0)\n");

    s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    e_lua = make_scripted(s, "bob",    LUA_FILE);
    e_py  = make_scripted(s, "turret", PY_FILE);

    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;

    rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    a = jce_scene_get_transform(s, e_lua);
    b = jce_scene_get_transform(s, e_py);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(11.0f, a->position.x,
        "the .lua entity's on_start did not run");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(44.0f, b->position.x,
        "the .py entity's on_start did not run — a scene mixing Lua and "
        "Python ran only the Lua half, which is exactly the state this "
        "feature was built to end");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(55.0f, b->position.y,
        "the .py entity's on_start wrote the wrong values");

    jce_runtime_step(rt, 0.016f);
    a = jce_scene_get_transform(s, e_lua);
    b = jce_scene_get_transform(s, e_py);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(111.0f, a->position.x,
        "the .lua entity's on_update did not run, or ran through the wrong VM");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(51.0f, b->position.x,
        "the .py entity's on_update did not run, or ran through the wrong VM");

    jce_runtime_step(rt, 0.016f);
    a = jce_scene_get_transform(s, e_lua);
    b = jce_scene_get_transform(s, e_py);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(211.0f, a->position.x,
        "the .lua entity stopped advancing on the second step");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(58.0f, b->position.x,
        "the .py entity stopped advancing on the second step");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_lua_and_a_py_in_one_scene_both_run);
    return UNITY_END();
}
