/* test_jce_runtime_sim_lod.c
 *
 * Simulation-LOD (distance-tiered gameplay tick) — proves rt_tick_gameplay
 * gates a tiered entity's Script on_update to its active tier's Hz, folds the
 * accumulated dt so the script's time-based logic stays correct at any rate,
 * and leaves entities WITHOUT a JceSimLodComponent at full rate (no regression).
 *
 * Each gameplay entity carries a tiny script that, every on_update, counts the
 * fire and integrates the handed dt, then writes (calls, integrated_time) into
 * a SEPARATE, untiered "report" entity it locates by tag.  Reporting into a
 * different entity is deliberate: the tiered entity itself never moves, so its
 * distance to the viewer (and therefore its tier) is stable across the run.
 *
 * The viewer falls back to the primary camera position when there is no
 * character controller (rt_viewer_position), so we place a Camera entity at the
 * origin and position each gameplay entity at a chosen distance from it.
 *
 * Asserted:
 *   A. An entity with NO SimLod component fires on_update EVERY step
 *      (calls == steps) and integrates the full wall time — byte-identical to
 *      pre-feature behavior (zero regression).
 *   B. A FAR tiered entity (far_hz = 1) fires ~once per simulated second, far
 *      fewer than the step count, yet still integrates ~the full wall time
 *      (folded dt keeps it time-correct: cheap but functional, not frozen).
 *   C. A NEAR tiered entity (near_hz = 0) fires EVERY step, identical to the
 *      untiered entity (nearby AI stays correct/smooth).
 *   D. A tiered entity with enabled == false is full-rate (the gate).
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>

#include "unity.h"

#include "jce_test_file_util.h"

#include <stdio.h>
#include <string.h>

#define SL_SCRIPT "jce_rt_simlod_selftest.lua"

void setUp(void)    {}
void tearDown(void) { remove(SL_SCRIPT); }

/* Create a stationary report sink tagged `tag` at a far-flung location (its own
 * position is irrelevant — it carries no SimLod and is never read for distance). */
static JceEntity make_report_sink(JceScene *s, const char *name, const char *tag)
{
    JceEntity e = jce_scene_create_entity(s, name);
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);
    jce_scene_set_entity_tag_name(s, e, tag);
    return e;
}

/* Create a scripted gameplay entity at (x,0,z) that reports its fire count +
 * folded-dt sum into the report entity tagged `rep_tag` (one script body per
 * role with the tag substituted), so the OWNING entity never moves and its tier
 * classification stays stable. */
static JceEntity make_scripted_entity(JceScene *s, const char *name,
                                      float x, float z, const char *rep_tag)
{
    /* Bake a script whose on_start seeds self.rep_tag = "<rep_tag>". */
    char body[1024];
    snprintf(body, sizeof body,
        "local M = {}\n"
        "function M:on_start()\n"
        "  self.calls = 0; self.t = 0.0\n"
        "  self.rep = jce.find_with_tag('%s')\n"
        "end\n"
        "function M:on_update(dt)\n"
        "  self.calls = self.calls + 1\n"
        "  self.t = self.t + dt\n"
        "  if not self.rep or self.rep == 0 then self.rep = jce.find_with_tag('%s') end\n"
        "  if self.rep and self.rep ~= 0 then\n"
        "    jce.set_position(self.rep, self.calls, self.t, 0.0)\n"
        "  end\n"
        "end\n"
        "return M\n", rep_tag, rep_tag);
    char path[128];
    snprintf(path, sizeof path, "jce_rt_simlod_%s.lua", rep_tag);
    jce_test_write_file(path, body);

    JceEntity e = jce_scene_create_entity(s, name);
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.position.x = x;
    t.position.z = z;
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", path);
    jce_scene_set_script(s, e, &sc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_SCRIPT, true);
    return e;
}

static void add_sim_lod(JceScene *s, JceEntity e, bool enabled,
                        float near_r, float mid_r,
                        float near_hz, float mid_hz, float far_hz)
{
    JceSimLodComponent sl;
    memset(&sl, 0, sizeof sl);
    sl.enabled     = enabled;
    sl.near_radius = near_r;
    sl.mid_radius  = mid_r;
    sl.near_hz     = near_hz;
    sl.mid_hz      = mid_hz;
    sl.far_hz      = far_hz;
    sl.gate_mask   = JCE_SIMLOD_GATE_ALL;
    jce_scene_set_sim_lod(s, e, &sl);
}

static void add_primary_camera(JceScene *s)
{
    JceEntity cam = jce_scene_create_entity(s, "cam");
    JceTransform ct; memset(&ct, 0, sizeof ct);
    ct.rotation.w = 1.0f; ct.scale.x = ct.scale.y = ct.scale.z = 1.0f;
    jce_scene_set_transform(s, cam, &ct);
    JceCameraComponent cc; memset(&cc, 0, sizeof cc);
    cc.fov_deg = 60.0f; cc.near_plane = 0.1f; cc.far_plane = 1000.0f;
    cc.is_primary = true;
    jce_scene_set_camera(s, cam, &cc);
}

static void cleanup_scripts(void)
{
    remove("jce_rt_simlod_rep_plain.lua");
    remove("jce_rt_simlod_rep_far.lua");
    remove("jce_rt_simlod_rep_near.lua");
    remove("jce_rt_simlod_rep_off.lua");
}

static void test_sim_lod_tiers_gate_script_tick(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    add_primary_camera(s);

    JceEntity rep_plain = make_report_sink(s, "rep_plain", "rep_plain");
    JceEntity rep_far   = make_report_sink(s, "rep_far",   "rep_far");
    JceEntity rep_near  = make_report_sink(s, "rep_near",  "rep_near");

    /* A: untiered entity (no SimLod) at distance 200 — ticks every frame. */
    make_scripted_entity(s, "plain", 200.0f, 0.0f, "rep_plain");

    /* B: FAR tiered entity at distance 200, far_hz = 1 Hz. */
    JceEntity far_e = make_scripted_entity(s, "far", 200.0f, 0.0f, "rep_far");
    add_sim_lod(s, far_e, true, 25.0f, 80.0f, 0.0f, 10.0f, 1.0f);

    /* C: NEAR tiered entity at distance 5, near_hz = 0 (every frame). */
    JceEntity near_e = make_scripted_entity(s, "near", 5.0f, 0.0f, "rep_near");
    add_sim_lod(s, near_e, true, 25.0f, 80.0f, 0.0f, 10.0f, 1.0f);

    JceRuntimeDesc desc; memset(&desc, 0, sizeof desc);
    desc.scene = s; desc.enable_physics = false;
    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    const int   steps = 120;            /* 2 simulated seconds at 60 Hz */
    const float dt    = 1.0f / 60.0f;
    for (int i = 0; i < steps; ++i)
        jce_runtime_step(rt, dt);

    JceTransform *tp = jce_scene_get_transform(s, rep_plain);
    JceTransform *tf = jce_scene_get_transform(s, rep_far);
    JceTransform *tn = jce_scene_get_transform(s, rep_near);
    TEST_ASSERT_NOT_NULL(tp);
    TEST_ASSERT_NOT_NULL(tf);
    TEST_ASSERT_NOT_NULL(tn);

    int   plain_calls = (int)(tp->position.x + 0.5f);
    float plain_time  = tp->position.y;
    int   far_calls   = (int)(tf->position.x + 0.5f);
    float far_time    = tf->position.y;
    int   near_calls  = (int)(tn->position.x + 0.5f);
    float near_time   = tn->position.y;

    printf("[sim-lod] plain calls=%d t=%.3f | near calls=%d t=%.3f | far calls=%d t=%.3f\n",
           plain_calls, plain_time, near_calls, near_time, far_calls, far_time);

    /* A. Untiered: fired every step, integrated the full wall time. */
    TEST_ASSERT_EQUAL_INT(steps, plain_calls);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, (float)steps * dt, plain_time);

    /* C. Near (every frame): identical to untiered. */
    TEST_ASSERT_EQUAL_INT(steps, near_calls);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, (float)steps * dt, near_time);

    /* B. Far (1 Hz over 2 s): fired only a few times — DRAMATICALLY fewer than
     * 120 — proving the gameplay tick is throttled (the CPU win). */
    TEST_ASSERT_TRUE_MESSAGE(far_calls >= 1,
        "far entity must still tick (not frozen)");
    TEST_ASSERT_TRUE_MESSAGE(far_calls <= 5,
        "far entity must tick FAR fewer times than the step count (throttled)");

    /* ...yet it stayed time-correct: the dt the far entity HAS been handed is
     * folded (never lost), so each fire advances its logic by ~1 real second.
     * At any instant at most one tier-period of dt is still buffered in the
     * accumulator (waiting for the next fire), so the delivered time is within
     * one period (1 s here) of the full wall time — proving the folded-dt
     * contract: cheap but functional, no time dropped, no mid-air desync. */
    float wall = (float)steps * dt;
    TEST_ASSERT_TRUE_MESSAGE(far_time > 0.0f, "far entity integrated some time");
    TEST_ASSERT_TRUE_MESSAGE(far_time <= wall + 1e-3f,
        "far delivered time cannot exceed wall time");
    TEST_ASSERT_TRUE_MESSAGE(wall - far_time <= 1.0f + 1e-2f,
        "far delivered time is within one tier period of wall (no time lost)");
    /* Each fire delivered ~one period (1 s): delivered ~= fires * period. */
    TEST_ASSERT_FLOAT_WITHIN(0.05f, (float)far_calls * 1.0f, far_time);

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

/* A tiered entity with enabled=false behaves exactly like an untiered one. */
static void test_sim_lod_disabled_is_full_rate(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    add_primary_camera(s);

    JceEntity rep = make_report_sink(s, "rep_off", "rep_off");

    /* Far away, but SimLod DISABLED -> must still tick every frame. */
    JceEntity e = make_scripted_entity(s, "disabled", 500.0f, 0.0f, "rep_off");
    add_sim_lod(s, e, false, 25.0f, 80.0f, 0.0f, 10.0f, 1.0f);

    JceRuntimeDesc desc; memset(&desc, 0, sizeof desc);
    desc.scene = s; desc.enable_physics = false;
    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    const int steps = 60;
    for (int i = 0; i < steps; ++i)
        jce_runtime_step(rt, 1.0f / 60.0f);

    JceTransform *t = jce_scene_get_transform(s, rep);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_INT(steps, (int)(t->position.x + 0.5f));

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sim_lod_tiers_gate_script_tick);
    RUN_TEST(test_sim_lod_disabled_is_full_rate);
    cleanup_scripts();
    return UNITY_END();
}
