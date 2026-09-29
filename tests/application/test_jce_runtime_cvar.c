/* test_jce_runtime_cvar.c
 *
 * End-to-end regression for gap 9.1's last-mile: the in-game console + cvar
 * system EXISTS and is wired into the editor console panel, but no REAL engine
 * cvars were registered, so the console could not actually control any
 * subsystem.  jce_runtime_create now registers a small high-value set of cvars
 * (time_scale / paused / audio.master_volume) and jce_runtime_step APPLIES the
 * console-changed ones to runtime state each frame (one-directional:
 * cvar -> runtime).
 *
 * This test drives the EXACT path the editor console panel uses — it executes a
 * console LINE through jce_console_exec (the same call the panel makes on the
 * user's typed input) — and proves the line actually controls the runtime, not
 * just stores a value:
 *
 *   1. After jce_runtime_create the time_scale / paused / audio.master_volume
 *      cvars are registered (findable) and seeded from the runtime defaults.
 *   2. exec "time_scale 0.25" -> step -> the runtime's effective sim time-scale
 *      is 0.25 (observed BOTH via jce_runtime_get_time_scale AND via a script's
 *      on_update accumulating the SCALED dt, exactly like the script-load test).
 *   3. exec "paused 1" -> step -> the sim is frozen (the script accumulator does
 *      not advance and jce_runtime_is_paused is true).
 *   4. exec "paused 0" + "time_scale 1" -> step -> the sim advances normally.
 *
 * It must FAIL before the bridge (no cvar -> the exec resolves to "unknown" and
 * the runtime never sees the value) and PASS after.
 *
 * Mirrors tests/application/test_jce_runtime_script_load.c (headless
 * jce_runtime_create / step; LINK the full JCE aggregate).
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_console.h>

#include "unity.h"

#include "jce_test_file_util.h"

#include <stdio.h>
#include <string.h>

#define CVAR_SCRIPT_FILE  "jce_rt_cvar_selftest.lua"

void setUp(void)    {}
void tearDown(void)
{
    remove(CVAR_SCRIPT_FILE);
    /* Reset the process-global cvar registry between runs so a value set here
     * can't leak into another test sharing the same process. */
    jce_console_shutdown();
}

/* Build a one-entity scene whose Lua script accumulates the (scaled) per-step
 * dt into position.x in on_update.  Because rt_tick_gameplay calls the script's
 * on_update with sim_dt (= dt * time_scale, or 0 when paused), position.x is a
 * direct, deterministic witness of the EFFECTIVE sim time scale. */
static JceScene *make_accumulator_scene(JceEntity *out_e)
{
    jce_test_write_file(CVAR_SCRIPT_FILE,
        "local M = {}\n"
        "function M:on_start()\n"
        "  jce.set_position(self.entity, 0, 0, 0)\n"
        "end\n"
        "function M:on_update(dt)\n"
        "  local x,y,z = jce.get_position(self.entity)\n"
        "  jce.set_position(self.entity, x + dt, y, z)\n"
        "end\n"
        "return M\n");

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "accumulator");

    JceTransform t;
    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", CVAR_SCRIPT_FILE);
    jce_scene_set_script(s, e, &sc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_SCRIPT, true);

    *out_e = e;
    return s;
}

static float accum_x(JceScene *s, JceEntity e)
{
    JceTransform *t = jce_scene_get_transform(s, e);
    TEST_ASSERT_NOT_NULL(t);
    return t->position.x;
}

static void test_console_controls_runtime_time(void)
{
    JceEntity e = 0;
    JceScene *s = make_accumulator_scene(&e);

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;   /* scripts need no physics / GPU */

    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    /* (1) The real engine cvars are registered + seeded from the defaults. */
    JceCvar *cv_ts   = jce_cvar_find("time_scale");
    JceCvar *cv_paus = jce_cvar_find("paused");
    JceCvar *cv_vol  = jce_cvar_find("audio.master_volume");
    TEST_ASSERT_NOT_NULL(cv_ts);
    TEST_ASSERT_NOT_NULL(cv_paus);
    TEST_ASSERT_NOT_NULL(cv_vol);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_cvar_get_float(cv_ts));
    TEST_ASSERT_FALSE(jce_cvar_get_bool(cv_paus));
    /* default time scale is normal speed before any console command */
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_runtime_get_time_scale(rt));
    TEST_ASSERT_FALSE(jce_runtime_is_paused(rt));

    /* (2) Drive the runtime through the console exactly like the editor panel:
     * type "time_scale 0.25".  The core console resolves the cvar and sets it;
     * the next step pulls it into the runtime. */
    TEST_ASSERT_TRUE(jce_console_exec("time_scale 0.25"));
    TEST_ASSERT_EQUAL_FLOAT(0.25f, jce_cvar_get_float(cv_ts));

    const float DT = 0.1f;
    jce_runtime_step(rt, DT);                       /* applies cvar, then ticks */

    /* Runtime getter proves the console actually controlled the runtime. */
    TEST_ASSERT_EQUAL_FLOAT(0.25f, jce_runtime_get_time_scale(rt));
    /* Script accumulator proves the EFFECTIVE sim dt was scaled: 0.1 * 0.25. */
    float x_after_scaled = accum_x(s, e);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, DT * 0.25f, x_after_scaled);

    /* (3) Freeze the sim from the console: "paused 1".  on_update still runs but
     * with dt == 0, so the accumulator must NOT advance. */
    TEST_ASSERT_TRUE(jce_console_exec("paused 1"));
    jce_runtime_step(rt, DT);
    TEST_ASSERT_TRUE(jce_runtime_is_paused(rt));
    float x_after_paused = accum_x(s, e);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, x_after_scaled, x_after_paused);  /* frozen */

    /* (4) Resume + restore normal speed from the console, then a full step
     * advances the accumulator by the unscaled dt. */
    TEST_ASSERT_TRUE(jce_console_exec("paused 0"));
    TEST_ASSERT_TRUE(jce_console_exec("time_scale 1"));
    jce_runtime_step(rt, DT);
    TEST_ASSERT_FALSE(jce_runtime_is_paused(rt));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_runtime_get_time_scale(rt));
    float x_after_resume = accum_x(s, e);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, x_after_paused + DT, x_after_resume);

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

/* A cvar nobody touched must NOT perturb the sim: with the console never used,
 * the runtime advances exactly as it did before the cvar bridge existed (the
 * accumulator gains the full unscaled dt every step). */
static void test_untouched_cvars_are_inert(void)
{
    JceEntity e = 0;
    JceScene *s = make_accumulator_scene(&e);

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;

    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    const float DT = 0.1f;
    jce_runtime_step(rt, DT);
    jce_runtime_step(rt, DT);
    jce_runtime_step(rt, DT);

    /* Three full unscaled steps -> accumulator == 3*DT (no scaling/pause). */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 3.0f * DT, accum_x(s, e));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_runtime_get_time_scale(rt));
    TEST_ASSERT_FALSE(jce_runtime_is_paused(rt));

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

/* A second runtime re-seeds the (process-global) cvars to its own initial
 * state, so a value set during a prior session does not leak in.  Proves
 * create() stays authoritative for the initial value. */
static void test_reseed_on_recreate(void)
{
    JceEntity e = 0;
    JceScene *s = make_accumulator_scene(&e);

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;

    /* Session 1: change the cvar via the console. */
    JceRuntime *rt1 = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt1);
    TEST_ASSERT_TRUE(jce_console_exec("time_scale 0.5"));
    jce_runtime_step(rt1, 0.1f);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, jce_runtime_get_time_scale(rt1));
    jce_runtime_destroy(rt1);

    /* Session 2: the cvar is re-seeded to the new runtime's default (1.0). */
    JceRuntime *rt2 = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt2);
    JceCvar *cv_ts = jce_cvar_find("time_scale");
    TEST_ASSERT_NOT_NULL(cv_ts);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_cvar_get_float(cv_ts));
    jce_runtime_step(rt2, 0.1f);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_runtime_get_time_scale(rt2));

    jce_runtime_destroy(rt2);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_console_controls_runtime_time);
    RUN_TEST(test_untouched_cvars_are_inert);
    RUN_TEST(test_reseed_on_recreate);
    return UNITY_END();
}
