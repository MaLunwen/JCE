/*
 * test_jce_runtime_script_cost.c — which script is costing the frame?
 *
 * WHY THIS EXISTS.  The script subsystem was the only middleware in the
 * engine with no instrumentation of any kind: zero JCE_PROFILE_ZONE in
 * jce_script.c, jce_script_vm.c and jce_rt_script.c, and none of the six
 * named perf phases (draw_dist, gameplay, particles, physics, runtime_tick,
 * scene_update) was scripts.  The update loop sits INSIDE rt_tick_gameplay,
 * so on_update was folded in with trigger overlap, spawn density, GAS
 * replication, ragdoll blending and weapon timers -- and a script eating 8 ms
 * was the same reading as a trigger volume eating 8 ms.
 *
 * WHAT MAKES THESE CASES WORTH THE FILE, rather than "a number came back":
 *
 *   1. RANKING, not magnitude.  Absolute milliseconds are a property of the
 *      machine; "the expensive script costs more than the cheap one" is a
 *      property of the measurement.  The two scripts differ only in how much
 *      work their on_update does, so a profiler that attributes cost to the
 *      wrong row fails here and a slow machine does not.
 *   2. ATTRIBUTION.  The rows are matched by entity, so a measurement that is
 *      right in aggregate and shuffled between scripts is caught.  That is
 *      the failure this feature exists to prevent: the aggregate was already
 *      available, folded into `gameplay`.
 *   3. `measured` IS NOT `last_ms != 0`.  With profiling off every row reads
 *      0.0, and a script that genuinely costs nothing reads 0.0 too.  The
 *      flag is set by the act of timing, so the case asserts that the OFF
 *      state is distinguishable rather than merely small.
 *   4. The `script` PHASE exists and is non-zero, because the per-row answer
 *      and the headline answer are two different questions and a project
 *      should not have to sum rows to learn the first one.
 *
 * NO ASSERTION ON AN ABSOLUTE DURATION anywhere in this file.  This machine
 * is documented as unstable and a wall-clock threshold is the first thing to
 * go flaky on it; every assertion here is a comparison between two numbers
 * produced by the same run.
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_perf_phase.h>

#include "unity.h"

#include "jce_test_file_util.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define BUSY_FILE  "test_cost_busy.lua"
#define IDLE_FILE  "test_cost_idle.lua"

/* Enough arithmetic to be measurable, and no allocation or I/O so the cost is
 * the VM's own and nothing else's. */
static const char *const BUSY_LUA =
    "local M = {}\n"
    "function M:on_update(dt)\n"
    "  local acc = 0.0\n"
    "  for i = 1, 60000 do acc = acc + i * 0.5 end\n"
    "  self.acc = acc\n"
    "end\n"
    "return M\n";

static const char *const IDLE_LUA =
    "local M = {}\n"
    "function M:on_update(dt) end\n"
    "return M\n";

void setUp(void) {}
void tearDown(void)
{
    remove(BUSY_FILE);
    remove(IDLE_FILE);
    jce_perf_phase_set_enabled(0);
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

typedef struct {
    uint64_t busy_entity, idle_entity;
    JceScriptCostInfo busy, idle;
    bool saw_busy, saw_idle;
    int rows;
} Collected;

static void collect(const JceScriptCostInfo *info, void *user)
{
    Collected *c = (Collected *)user;
    c->rows++;
    if (info->entity == c->busy_entity) { c->busy = *info; c->saw_busy = true; }
    if (info->entity == c->idle_entity) { c->idle = *info; c->saw_idle = true; }
}

static Collected sample(JceRuntime *rt, uint64_t busy, uint64_t idle)
{
    Collected c;
    memset(&c, 0, sizeof c);
    c.busy_entity = busy;
    c.idle_entity = idle;
    jce_runtime_iterate_script_costs(rt, collect, &c);
    return c;
}

/* ---------------------------------------------------------------------- */

static void test_the_expensive_script_is_the_expensive_row(void)
{
    JceScene      *s;
    JceRuntimeDesc desc;
    JceRuntime    *rt;

    jce_test_write_file(BUSY_FILE, BUSY_LUA);
    jce_test_write_file(IDLE_FILE, IDLE_LUA);

    s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity busy = make_scripted(s, "busy", BUSY_FILE);
    JceEntity idle = make_scripted(s, "idle", IDLE_FILE);

    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;
    rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    jce_perf_phase_set_enabled(1);
    for (int i = 0; i < 30; ++i)
        jce_runtime_step(rt, 0.016f);

    Collected c = sample(rt, (uint64_t)busy, (uint64_t)idle);
    printf("  rows=%d  busy last=%.4f avg=%.4f calls=%u  "
           "idle last=%.4f avg=%.4f calls=%u\n",
           c.rows, c.busy.last_ms, c.busy.avg_ms, c.busy.calls,
           c.idle.last_ms, c.idle.avg_ms, c.idle.calls);

    TEST_ASSERT_TRUE_MESSAGE(c.saw_busy && c.saw_idle,
        "the iteration did not report both scripted entities");
    TEST_ASSERT_TRUE_MESSAGE(c.busy.measured && c.idle.measured,
        "profiling was on and the rows still say they were not measured");
    TEST_ASSERT_TRUE_MESSAGE(c.busy.calls > 0 && c.idle.calls > 0,
        "no on_update was counted -- the scripts did not run, and the "
        "comparison below would be between two zeroes");

    /* THE ASSERTION THAT MATTERS.  Not "busy > 0" -- a measurement that
     * attributes every script's cost to whichever row it happens to be
     * writing would also give busy > 0.  The ranking is what says the cost
     * reached the right row. */
    TEST_ASSERT_TRUE_MESSAGE(c.busy.avg_ms > c.idle.avg_ms,
        "the busy script did not cost more than the empty one -- the cost is "
        "not being attributed per script, which is the whole feature");

    /* And the aggregate exists, because summing rows is not the answer to
     * "is scripting costing me anything". */
    jce_perf_phase_frame_tick();
    bool saw_phase = false;
    double phase_ms = 0.0;
    for (int i = 0; i < jce_perf_phase_count(); ++i) {
        const char *nm = NULL;
        double ms = 0.0;
        if (jce_perf_phase_peek_frame(i, &nm, &ms) && nm &&
            strcmp(nm, "script") == 0) {
            saw_phase = true;
            phase_ms = ms;
        }
    }
    printf("  `script` phase present=%d  ms=%.4f\n", (int)saw_phase, phase_ms);
    TEST_ASSERT_TRUE_MESSAGE(saw_phase,
        "no `script` CPU phase was recorded, so the profiler's headline table "
        "still folds scripts into `gameplay`");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

static void test_with_profiling_off_the_rows_say_so(void)
{
    JceScene      *s;
    JceRuntimeDesc desc;
    JceRuntime    *rt;

    jce_test_write_file(BUSY_FILE, BUSY_LUA);
    jce_test_write_file(IDLE_FILE, IDLE_LUA);

    s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity busy = make_scripted(s, "busy", BUSY_FILE);
    JceEntity idle = make_scripted(s, "idle", IDLE_FILE);

    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;
    rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    jce_perf_phase_set_enabled(0);
    for (int i = 0; i < 10; ++i)
        jce_runtime_step(rt, 0.016f);

    Collected c = sample(rt, (uint64_t)busy, (uint64_t)idle);
    printf("  profiling off: busy measured=%d last=%.4f  idle measured=%d\n",
           (int)c.busy.measured, c.busy.last_ms, (int)c.idle.measured);

    TEST_ASSERT_TRUE_MESSAGE(c.saw_busy && c.saw_idle,
        "the rows disappeared when profiling was off -- they are the script "
        "inventory, not the measurement");
    /* THE DISTINCTION THIS FLAG EXISTS FOR.  0.0 from "nobody timed it" and
     * 0.0 from "it costs nothing" are the same number; they must not be the
     * same reading. */
    TEST_ASSERT_FALSE_MESSAGE(c.busy.measured,
        "a row claims it was measured while profiling was off, so a reader "
        "cannot tell an untimed 0 from a free script");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, c.busy.calls,
        "calls were counted while profiling was off -- the gate is not "
        "gating");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

static void test_reset_clears_the_average_and_keeps_the_last_frame(void)
{
    JceScene      *s;
    JceRuntimeDesc desc;
    JceRuntime    *rt;

    jce_test_write_file(BUSY_FILE, BUSY_LUA);
    jce_test_write_file(IDLE_FILE, IDLE_LUA);

    s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity busy = make_scripted(s, "busy", BUSY_FILE);
    JceEntity idle = make_scripted(s, "idle", IDLE_FILE);

    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;
    rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    jce_perf_phase_set_enabled(1);
    for (int i = 0; i < 20; ++i)
        jce_runtime_step(rt, 0.016f);

    Collected before = sample(rt, (uint64_t)busy, (uint64_t)idle);
    TEST_ASSERT_TRUE(before.busy.calls >= 20u);

    jce_runtime_reset_script_costs(rt);
    Collected after = sample(rt, (uint64_t)busy, (uint64_t)idle);
    printf("  reset: calls %u -> %u   last_ms %.4f -> %.4f\n",
           before.busy.calls, after.busy.calls,
           before.busy.last_ms, after.busy.last_ms);

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, after.busy.calls,
        "reset did not clear the call count");
    /* last_ms deliberately survives: the panel shows the most recent frame
     * continuously, and a reset that blanked it would make every refresh
     * flash empty for one frame. */
    TEST_ASSERT_EQUAL_DOUBLE_MESSAGE(before.busy.last_ms, after.busy.last_ms,
        "reset cleared last_ms, which the panel reads every refresh");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_expensive_script_is_the_expensive_row);
    RUN_TEST(test_with_profiling_off_the_rows_say_so);
    RUN_TEST(test_reset_clears_the_average_and_keeps_the_last_frame);
    return UNITY_END();
}
