/* test_jce_runtime_scene_transition.c
 *
 * Regression + feature test for FEATURE 9.4 — engine-level scene/level
 * transition (jce_runtime_request_scene + the FADE_OUT -> LOAD -> FADE_IN
 * state machine advanced inside jce_runtime_step).
 *
 * Before 9.4 a JceRuntime drove ONE scene for its whole lifetime: there was
 * no load_scene / change_scene API, so shipping a multi-level game meant
 * destroying + recreating the entire runtime (and its physics world / script
 * VM / audio device).  9.4 lets a live runtime swap scenes in place:
 *
 *   jce_runtime_request_scene(rt, B)  queues a transition
 *   jce_runtime_step ...              FADE_OUT (alpha 0->1)
 *                                     LOAD     (tear down A's runtime state,
 *                                               load B into the SAME scene,
 *                                               re-run the spawn walks)
 *                                     FADE_IN  (alpha 1->0)
 *                                     IDLE
 *
 * This test boots a runtime on scene A (one entity carrying a Lua script that
 * stamps a sentinel), asserts A is live, requests scene B (a DIFFERENT entity
 * with its own script), steps the REAL runtime across the fade/load frames,
 * and asserts:
 *   - is_transitioning goes true then false,
 *   - transition_alpha ramps up to 1 then back to 0,
 *   - the active scene becomes B (A's entity gone, B's present, B's script ran),
 *   - no double-free / crash (a second transition back works too).
 *
 * It exercises the REAL jce_runtime_step transition path — no mocks.
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/resource/jce_scene_serial.h>

#include "unity.h"

#include "jce_test_file_util.h"

#include <stdio.h>
#include <string.h>

#define SCENE_A_FILE   "jce_rt_trans_sceneA.scene.json"
#define SCENE_B_FILE   "jce_rt_trans_sceneB.scene.json"
#define SCRIPT_A_FILE  "jce_rt_trans_scriptA.lua"
#define SCRIPT_B_FILE  "jce_rt_trans_scriptB.lua"

/* Sentinel y-coordinates each scene's script writes in on_start.  We use the
 * Y position (which the no-physics runtime never touches) to identify which
 * scene's entity is live, since the scene loader does not round-trip tags. */
#define SCENE_A_SENTINEL_Y  111.0f
#define SCENE_B_SENTINEL_Y  222.0f

void setUp(void) {}
void tearDown(void)
{
    remove(SCENE_A_FILE);
    remove(SCENE_B_FILE);
    remove(SCRIPT_A_FILE);
    remove(SCRIPT_B_FILE);
}

/* Author a one-entity scene whose single script stamps `sentinel_y` into its
 * own Y position in on_start, then SERIALIZE it to `scene_path` using the real
 * scene serializer (so the file is byte-for-byte loadable by the runtime). */
static void author_scene_file(const char *scene_path, const char *script_path,
                              float sentinel_y)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "marker");

    JceTransform t;
    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", script_path);
    jce_scene_set_script(s, e, &sc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_SCRIPT, true);

    TEST_ASSERT_TRUE(jce_scene_serial_save_file(s, scene_path));
    jce_scene_destroy(s);

    /* on_start stamps the per-scene sentinel into Y so the live scene is
     * identifiable; on_update keeps re-stamping so it survives any sync. */
    char body[512];
    snprintf(body, sizeof body,
        "local M = {}\n"
        "function M:on_start()\n"
        "  local x,y,z = jce.get_position(self.entity)\n"
        "  jce.set_position(self.entity, x, %.1f, z)\n"
        "end\n"
        "function M:on_update(dt)\n"
        "  local x,y,z = jce.get_position(self.entity)\n"
        "  jce.set_position(self.entity, x, %.1f, z)\n"
        "end\n"
        "return M\n",
        (double)sentinel_y, (double)sentinel_y);
    jce_test_write_file(script_path, body);
}

/* Scan: count entities and capture the (unique) marker entity's Y sentinel. */
typedef struct { int count; bool found; float y; } SceneScan;
static void scene_scan_cb(JceScene *s, JceEntity e, void *u)
{
    SceneScan *sc = (SceneScan *)u;
    sc->count++;
    JceTransform *t = jce_scene_get_transform(s, e);
    if (t) { sc->found = true; sc->y = t->position.y; }
}

static SceneScan scan_scene(JceScene *s)
{
    SceneScan sc;
    memset(&sc, 0, sizeof sc);
    jce_scene_each_entity(s, scene_scan_cb, &sc);
    return sc;
}

static void test_runtime_transitions_scene_a_to_b(void)
{
    author_scene_file(SCENE_A_FILE, SCRIPT_A_FILE, SCENE_A_SENTINEL_Y);
    author_scene_file(SCENE_B_FILE, SCRIPT_B_FILE, SCENE_B_SENTINEL_Y);

    /* Boot the runtime on scene A. */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(jce_scene_serial_load_file(s, SCENE_A_FILE));

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = true;   /* exercise the physics teardown/recreate path */

    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    /* The runtime drives the same JceScene object we created. */
    TEST_ASSERT_EQUAL_PTR(s, jce_runtime_scene(rt));
    TEST_ASSERT_FALSE(jce_runtime_is_transitioning(rt));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_runtime_transition_alpha(rt));

    /* Scene A is live: one entity, A's on_start stamped its sentinel. */
    {
        SceneScan sc = scan_scene(s);
        TEST_ASSERT_EQUAL_INT(1, sc.count);
        TEST_ASSERT_TRUE(sc.found);
        TEST_ASSERT_EQUAL_FLOAT(SCENE_A_SENTINEL_Y, sc.y);
    }

    /* Request the transition to scene B. */
    TEST_ASSERT_TRUE(jce_runtime_request_scene(rt, SCENE_B_FILE));
    /* A second request while one is in flight is rejected (the first wins). */
    TEST_ASSERT_FALSE(jce_runtime_request_scene(rt, SCENE_A_FILE));

    /* Step across the fade/load frames, recording the alpha curve + whether we
     * ever observed transitioning==true and the peak alpha (full black). */
    bool saw_transitioning = false;
    bool saw_full_black     = false;
    bool saw_idle_again     = false;
    const float dt = 1.0f / 60.0f;   /* 0.25s fade each way -> ~15 frames/dir */
    for (int i = 0; i < 200; ++i) {
        jce_runtime_step(rt, dt);
        float a = jce_runtime_transition_alpha(rt);
        TEST_ASSERT_TRUE(a >= 0.0f && a <= 1.0001f);   /* alpha stays in [0,1] */
        if (jce_runtime_is_transitioning(rt)) saw_transitioning = true;
        if (a >= 0.999f) saw_full_black = true;
        if (saw_transitioning && !jce_runtime_is_transitioning(rt)) {
            saw_idle_again = true;
            break;
        }
    }

    TEST_ASSERT_TRUE(saw_transitioning);   /* is_transitioning went true */
    TEST_ASSERT_TRUE(saw_full_black);      /* alpha ramped up to ~1 (black) */
    TEST_ASSERT_TRUE(saw_idle_again);      /* and back to idle (false) */

    /* After the transition completes: alpha is 0, not transitioning. */
    TEST_ASSERT_FALSE(jce_runtime_is_transitioning(rt));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_runtime_transition_alpha(rt));

    /* The active scene is now B: still one entity, but it is B's entity (A's
     * is gone) and B's on_start ran (its sentinel is stamped).  Same scene
     * object pointer — the swap loaded B INTO the runtime's scene. */
    TEST_ASSERT_EQUAL_PTR(s, jce_runtime_scene(rt));
    {
        /* Run one more step so B's on_update has re-stamped (in case the load
         * happened on the same frame the scan would otherwise miss). */
        jce_runtime_step(rt, dt);
        SceneScan sc = scan_scene(s);
        TEST_ASSERT_EQUAL_INT(1, sc.count);            /* A's entity gone, B's present */
        TEST_ASSERT_TRUE(sc.found);
        TEST_ASSERT_EQUAL_FLOAT(SCENE_B_SENTINEL_Y, sc.y);  /* B's script ran */
    }

    /* Transition BACK to A to prove the path is reusable + no double-free: the
     * physics world / script VM were rebuilt once already, do it again. */
    TEST_ASSERT_TRUE(jce_runtime_request_scene(rt, SCENE_A_FILE));
    for (int i = 0; i < 200; ++i) {
        jce_runtime_step(rt, dt);
        if (!jce_runtime_is_transitioning(rt)) break;
    }
    TEST_ASSERT_FALSE(jce_runtime_is_transitioning(rt));
    {
        jce_runtime_step(rt, dt);
        SceneScan sc = scan_scene(s);
        TEST_ASSERT_EQUAL_INT(1, sc.count);
        TEST_ASSERT_TRUE(sc.found);
        TEST_ASSERT_EQUAL_FLOAT(SCENE_A_SENTINEL_Y, sc.y);  /* back on A */
    }

    jce_runtime_destroy(rt);   /* clean teardown after a transition */
    jce_scene_destroy(s);
}

/* A request with a NULL/empty path or NULL runtime is a clean no-op. */
static void test_request_scene_rejects_bad_input(void)
{
    TEST_ASSERT_FALSE(jce_runtime_request_scene(NULL, "x"));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_runtime_transition_alpha(NULL));
    TEST_ASSERT_FALSE(jce_runtime_is_transitioning(NULL));

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    (void)jce_scene_create_entity(s, "solo");

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;

    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    TEST_ASSERT_FALSE(jce_runtime_request_scene(rt, NULL));
    TEST_ASSERT_FALSE(jce_runtime_request_scene(rt, ""));
    TEST_ASSERT_FALSE(jce_runtime_is_transitioning(rt));

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_runtime_transitions_scene_a_to_b);
    RUN_TEST(test_request_scene_rejects_bad_input);
    return UNITY_END();
}
