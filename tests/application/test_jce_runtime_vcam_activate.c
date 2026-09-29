/*
 * test_jce_runtime_vcam_activate.c — the binding, through the real runtime.
 *
 * jce_vcam_system_set_active_by_name() has its own unit test; this one asks a
 * different question: does a LUA SCRIPT reach it.  The distinction is the one
 * this tree has already paid for once -- jce.is_key_down was registered,
 * callable, documented in seven languages and returned false for every key,
 * because its JceScriptHost slot was never assigned and the generated binding
 * takes the `: false` branch on a NULL slot.  A mock-host test cannot see
 * that: it installs non-NULL callbacks by construction.
 *
 * So the probe runs the real jce_runtime_create / jce_runtime_step path and
 * reports the binding's answer as position.x, which the public scene API can
 * read back.  The transform starts at -7, a value the script writes for
 * neither answer, so "the script never ran" cannot be misread as "it answered
 * 0" -- the same guard the is_key_down probe uses and for the same reason.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_vcam_system.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

#define LUA_FILE "test_vcam_activate.lua"

/* Asks for a shot that exists, then one that does not, and reports both
 * answers: x = the hit, y = the miss.  Both matter -- a binding that returned
 * a constant 1 would satisfy the first assertion on its own. */
static const char *const PROBE_LUA =
    "local M = {}\n"
    "function M:on_update(dt)\n"
    "  local hit  = jce.vcam_activate('BossIntro')\n"
    "  local miss = jce.vcam_activate('NoSuchShot')\n"
    "  jce.set_position(self.entity, hit, miss, 0)\n"
    "end\n"
    "return M\n";

void setUp(void)    { jce_vcam_system_reset(); }
void tearDown(void) { jce_vcam_system_reset(); remove(LUA_FILE); }

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "could not write the probe script");
    fputs(text, f);
    fclose(f);
}

static JceEntity make_vcam(JceScene *s, const char *name)
{
    JceEntity                 e = jce_scene_create_entity(s, name);
    JceTransform              t;
    JceVirtualCameraComponent vc;

    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);

    memset(&vc, 0, sizeof vc);
    snprintf(vc.vcam_name, sizeof vc.vcam_name, "%s", name);
    vc.active  = true;
    vc.fov_deg = 60.0f;
    jce_scene_set_virtual_camera(s, e, &vc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_VIRTUAL_CAMERA, true);
    return e;
}

static void test_a_lua_script_can_cut_to_a_named_camera(void)
{
    JceScene      *s = jce_scene_create();
    JceRuntimeDesc rd;
    JceRuntime    *rt;
    JceEntity      probe;
    JceTransform   t;
    JceScriptComponent sc;

    TEST_ASSERT_NOT_NULL(s);
    write_file(LUA_FILE, PROBE_LUA);
    make_vcam(s, "BossIntro");

    probe = jce_scene_create_entity(s, "probe");
    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    t.position.x = -7.0f;   /* neither answer, so a dead script is visible */
    t.position.y = -7.0f;
    jce_scene_set_transform(s, probe, &t);
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", LUA_FILE);
    jce_scene_set_script(s, probe, &sc);
    jce_scene_set_component_enabled(s, probe, JCE_COMP_FLAG_SCRIPT, true);

    memset(&rd, 0, sizeof rd);
    rd.scene = s;
    rt = jce_runtime_create(&rd);
    TEST_ASSERT_NOT_NULL_MESSAGE(rt, "the runtime did not come up");
    jce_runtime_step(rt, 1.0f / 60.0f);

    {
        JceTransform *pt = jce_scene_get_transform(s, probe);
        TEST_ASSERT_NOT_NULL(pt);
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, pt->position.x,
            "jce.vcam_activate did not reach the engine: the host slot is "
            "NULL and the generated binding took its fallback branch");
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, pt->position.y,
            "a name that matches no camera must answer 0 -- otherwise the "
            "first assertion is satisfied by a constant");
    }

    /* And the effect, not just the answer: the override is what the vcam
     * system will act on next frame. */
    TEST_ASSERT_EQUAL_STRING_MESSAGE("NoSuchShot",
        jce_vcam_system_get_active_name(),
        "the LAST request must stand, hit or miss: a cut aimed at a cell that "
        "has not streamed in yet must not silently fall back to priority");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_lua_script_can_cut_to_a_named_camera);
    return UNITY_END();
}
