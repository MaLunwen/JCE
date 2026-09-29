/* test_jce_runtime_script_load.c
 *
 * Regression test for the Phase-0 gameplay-scripting ORDERING bug.
 *
 * jce_runtime_create() walks the scene (rt_spawn_gameplay) to load each
 * entity's authored JceScriptComponent into the runtime-owned Lua VM and call
 * on_start; rt_tick_gameplay then calls on_update each step.  The VM
 * (rt->script_vm) and the behavior-tree context (rt->bt_ctx) used to be created
 * AFTER that walk, so the `rt->script_vm &&` / `rt->bt_ctx &&` guards inside
 * rt_spawn_gameplay were ALWAYS false: authored scripts (and behavior trees)
 * silently never loaded or ran at runtime.  The keystone's earlier self-test
 * drove the VM directly through a mock host, so it never exercised this path
 * and the bug hid in plain sight.
 *
 * This test builds a one-entity scene whose Lua script writes a sentinel
 * position in on_start and advances it in on_update, runs it through the REAL
 * jce_runtime_create / jce_runtime_step path, and asserts the writes happened —
 * i.e. the script actually loaded and its lifecycle ran.  It must FAIL before
 * the ordering fix and PASS after.
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_prefab.h>

#include "unity.h"

#include "jce_test_file_util.h"

#include <stdio.h>
#include <string.h>

#define SCRIPT_FILE   "jce_rt_script_selftest.lua"
#define COLLIDE_FILE  "jce_rt_collide_selftest.lua"
#define SPAWNER_FILE  "jce_rt_spawner_selftest.lua"
#define SPAWNED_FILE  "jce_rt_spawned_selftest.lua"
#define PREFAB_FILE   "jce_rt_spawned_selftest.prefab.json"
#define RELOAD_FILE   "jce_rt_reload_selftest.lua"

void setUp(void)    {}
void tearDown(void)
{
    remove(SCRIPT_FILE);
    remove(COLLIDE_FILE);
    remove(SPAWNER_FILE);
    remove(SPAWNED_FILE);
    remove(PREFAB_FILE);
    remove(RELOAD_FILE);
}

/* on_start writes a sentinel position; on_update adds 100 to x each step, so we
 * prove both lifecycle hooks run through the real runtime. */
static void test_script_loads_and_runs_through_runtime(void)
{
    jce_test_write_file(SCRIPT_FILE,
        "local M = {}\n"
        "function M:on_start()\n"
        "  jce.set_position(self.entity, 11, 22, 33)\n"
        "end\n"
        "function M:on_update(dt)\n"
        "  local x,y,z = jce.get_position(self.entity)\n"
        "  jce.set_position(self.entity, x + 100, y, z)\n"
        "end\n"
        "return M\n");

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "scripted");

    JceTransform t;
    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;                 /* valid identity quaternion */
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", SCRIPT_FILE);
    jce_scene_set_script(s, e, &sc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_SCRIPT, true);

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;        /* scripts need no physics / GPU */

    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    /* on_start ran during create() and wrote the sentinel. */
    JceTransform *after = jce_scene_get_transform(s, e);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_FLOAT(11.0f, after->position.x);
    TEST_ASSERT_EQUAL_FLOAT(22.0f, after->position.y);
    TEST_ASSERT_EQUAL_FLOAT(33.0f, after->position.z);

    /* one step → on_update adds 100 to x. */
    jce_runtime_step(rt, 0.016f);
    after = jce_scene_get_transform(s, e);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_FLOAT(111.0f, after->position.x);

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

/* Spawn a dynamic-rigidbody entity (default box collider) at `x` carrying the
 * shared collision script, enabled for physics. */
static JceEntity make_box(JceScene *s, const char *name, float x)
{
    JceEntity e = jce_scene_create_entity(s, name);

    JceTransform t;
    memset(&t, 0, sizeof t);
    t.position.x = x;
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);

    JceRigidBodyComponent rb;
    memset(&rb, 0, sizeof rb);
    rb.mass        = 1.0f;          /* >0 → dynamic */
    rb.use_gravity = false;         /* stay put so the spawn overlap is the contact */
    jce_scene_set_rigidbody(s, e, &rb);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_RIGIDBODY, true);

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", COLLIDE_FILE);
    jce_scene_set_script(s, e, &sc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_SCRIPT, true);
    return e;
}

/* Two overlapping dynamic boxes must each receive on_collision(self, other)
 * with the OTHER box's entity id — exercising the full physics-contact →
 * script bridge (jce_physics contact listener → rt_script_collision_cb →
 * jce_script_call_collision) end-to-end. */
static void test_on_collision_dispatches_to_both(void)
{
    /* on_collision records the other entity in scale.x and a 9 marker in
     * scale.y (scale is not overwritten by the physics transform sync). */
    jce_test_write_file(COLLIDE_FILE,
        "local M = {}\n"
        "function M:on_collision(other)\n"
        "  jce.set_scale(self.entity, other, 9, 0)\n"
        "end\n"
        "return M\n");

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity a = make_box(s, "boxA", 0.0f);
    JceEntity b = make_box(s, "boxB", 0.9f);  /* boxes are 1u → [−.5,.5] & [.4,1.4] overlap */

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = true;

    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    /* Step until the BEGIN contact has dispatched (marker appears), then stop
     * BEFORE another step would push the sentinel scale into the physics body. */
    for (int i = 0; i < 30; ++i) {
        jce_runtime_step(rt, 1.0f / 60.0f);
        JceTransform *ta = jce_scene_get_transform(s, a);
        if (ta && ta->scale.y == 9.0f) break;
    }

    JceTransform *ta = jce_scene_get_transform(s, a);
    JceTransform *tb = jce_scene_get_transform(s, b);
    TEST_ASSERT_NOT_NULL(ta);
    TEST_ASSERT_NOT_NULL(tb);
    TEST_ASSERT_EQUAL_FLOAT(9.0f, ta->scale.y);          /* A's on_collision ran */
    TEST_ASSERT_EQUAL_FLOAT(9.0f, tb->scale.y);          /* B's on_collision ran */
    TEST_ASSERT_EQUAL_FLOAT((float)b, ta->scale.x);      /* A saw B as `other`   */
    TEST_ASSERT_EQUAL_FLOAT((float)a, tb->scale.x);      /* B saw A as `other`   */

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

/* Scan helper: count entities and capture the position of the one that is not
 * the spawner (i.e. the spawned prefab root). */
typedef struct { int count; JceEntity exclude; bool found; float x, y, z; } SpawnScan;
static int spawn_scan_cb(JceScene *s, JceEntity e, void *u)
{
    SpawnScan *sc = (SpawnScan *)u;
    sc->count++;
    if (e != sc->exclude) {
        JceTransform *t = jce_scene_get_transform(s, e);
        if (t) { sc->found = true; sc->x = t->position.x; sc->y = t->position.y; sc->z = t->position.z; }
    }
    return 0;
}

/* jce.spawn instantiates a prefab at runtime, places its root, and the deferred
 * wiring runs the spawned entity's own script (on_start) — exercising
 * rt_script_spawn → rt_flush_pending_spawns → rt_spawn_gameplay end-to-end. */
static void test_spawn_instantiates_and_wires(void)
{
    /* The spawned prefab's script teleports itself to a sentinel in on_start,
     * proving the spawned entity got fully wired (not just created). */
    jce_test_write_file(SPAWNED_FILE,
        "local M = {}\n"
        "function M:on_start()\n"
        "  jce.set_position(self.entity, 777, 777, 777)\n"
        "end\n"
        "return M\n");

    /* Author the prefab by saving a one-entity subtree carrying that script. */
    {
        JceScene *ps = jce_scene_create();
        TEST_ASSERT_NOT_NULL(ps);
        JceEntity pe = jce_scene_create_entity(ps, "prefabRoot");
        JceTransform t; memset(&t, 0, sizeof t);
        t.rotation.w = 1.0f; t.scale.x = t.scale.y = t.scale.z = 1.0f;
        jce_scene_set_transform(ps, pe, &t);
        JceScriptComponent sc; memset(&sc, 0, sizeof sc);
        snprintf(sc.script_path, sizeof sc.script_path, "%s", SPAWNED_FILE);
        jce_scene_set_script(ps, pe, &sc);
        jce_scene_set_component_enabled(ps, pe, JCE_COMP_FLAG_SCRIPT, true);
        TEST_ASSERT_TRUE(jce_prefab_save_subtree(ps, pe, PREFAB_FILE));
        jce_scene_destroy(ps);
    }

    /* The spawner script spawns the prefab once, from on_update. */
    jce_test_write_file(SPAWNER_FILE,
        "local M = {}\n"
        "function M:on_update(dt)\n"
        "  if not self.done then\n"
        "    self.done = true\n"
        "    self.child = jce.spawn(\"" PREFAB_FILE "\", 5, 6, 7)\n"
        "  end\n"
        "end\n"
        "return M\n");

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity spawner = jce_scene_create_entity(s, "spawner");
    JceTransform t; memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f; t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, spawner, &t);
    JceScriptComponent sc; memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", SPAWNER_FILE);
    jce_scene_set_script(s, spawner, &sc);
    jce_scene_set_component_enabled(s, spawner, JCE_COMP_FLAG_SCRIPT, true);

    JceRuntimeDesc desc; memset(&desc, 0, sizeof desc);
    desc.scene = s;
    desc.enable_physics = false;

    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    /* One step: on_update spawns the prefab; the post-tick flush wires it and
     * runs its on_start. */
    jce_runtime_step(rt, 1.0f / 60.0f);

    SpawnScan scan; memset(&scan, 0, sizeof scan);
    scan.exclude = spawner;
    jce_scene_each_entity(s, spawn_scan_cb, &scan);

    TEST_ASSERT_EQUAL_INT(2, scan.count);          /* spawner + spawned root */
    TEST_ASSERT_TRUE(scan.found);
    TEST_ASSERT_EQUAL_FLOAT(777.0f, scan.x);       /* spawned script's on_start ran */
    TEST_ASSERT_EQUAL_FLOAT(777.0f, scan.y);
    TEST_ASSERT_EQUAL_FLOAT(777.0f, scan.z);

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

/* Hot-reload recompiles a script and rebinds live instances IN PLACE: the new
 * on_update logic takes effect, per-instance `self` state is preserved, and
 * on_start is NOT re-run.  The sentinel arithmetic distinguishes all three. */
static void test_hot_reload_preserves_self(void)
{
    /* v1: on_start seeds self.base=50; on_update writes x = base + 100. */
    jce_test_write_file(RELOAD_FILE,
        "local M = {}\n"
        "function M:on_start() self.base = 50 end\n"
        "function M:on_update(dt) jce.set_position(self.entity, (self.base or -1) + 100, 0, 0) end\n"
        "return M\n");

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "live");
    JceTransform t; memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f; t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);
    JceScriptComponent sc; memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", RELOAD_FILE);
    jce_scene_set_script(s, e, &sc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_SCRIPT, true);

    JceRuntimeDesc desc; memset(&desc, 0, sizeof desc);
    desc.scene = s;
    desc.enable_physics = false;

    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    /* v1 active: on_update → x = base(50) + 100 = 150. */
    jce_runtime_step(rt, 1.0f / 60.0f);
    JceTransform *after = jce_scene_get_transform(s, e);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_FLOAT(150.0f, after->position.x);

    /* v2: would re-seed base=999 IF on_start re-ran; on_update writes base+200. */
    jce_test_write_file(RELOAD_FILE,
        "local M = {}\n"
        "function M:on_start() self.base = 999 end\n"
        "function M:on_update(dt) jce.set_position(self.entity, (self.base or -1) + 200, 0, 0) end\n"
        "return M\n");

    TEST_ASSERT_TRUE_MESSAGE(jce_runtime_reload_script(rt, RELOAD_FILE),
        "hot-reload reported failure — the assertions below would be "
        "checking the OLD script and passing for the wrong reason");

    /* v2 active, self preserved, on_start NOT re-run → x = base(still 50) + 200 = 250.
     * (999+200=1199 would mean on_start re-ran; -1+200=199 would mean self lost.) */
    jce_runtime_step(rt, 1.0f / 60.0f);
    after = jce_scene_get_transform(s, e);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_FLOAT(250.0f, after->position.x);

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

/* -- a script drives a UIProgressBar through the REAL host ---------------
 *
 * The mock-host test in tests/middleware/script proves the Lua binding reaches
 * the right vtable slot.  This one proves the other half: that the slot's
 * implementation finds the component and writes it, and that the write is
 * clamped into the component's OWN [min_value, max_value] -- which is the part
 * that is easy to get wrong, because a progress bar's value is NOT 0..1.  The
 * bar draws clamp((value-min)/(max-min)), so 0..1 is the FILL.
 *
 * Three writes in one script, each falsifiable on its own:
 *   in range   50  -> stored as 50
 *   above max 999  -> clamped to 100, NOT stored raw and NOT refused
 *   below min -7   -> clamped to 0
 * A clamp to 0..1 -- the plausible wrong reading -- gives 1, 1 and 0, so it
 * cannot pass; nor can "store whatever you are given", which gives 999. */
static void test_a_script_drives_a_progress_bar_through_the_real_host(void)
{
    jce_test_write_file(SCRIPT_FILE,
        "local M = {}\n"
        "function M:on_start()\n"
        "  jce.ui_set_progress(self.entity, 50)\n"
        "  assert(jce.ui_get_progress(self.entity) == 50, 'in-range readback')\n"
        "  jce.ui_set_progress(self.entity, 999)\n"
        "  assert(jce.ui_get_progress(self.entity) == 100, 'clamp to max')\n"
        "  jce.ui_set_progress(self.entity, -7)\n"
        "end\n"
        "return M\n");

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "bar");

    JceUIProgressBarComponent pb;
    memset(&pb, 0, sizeof pb);
    pb.value     = 0.0f;
    pb.min_value = 0.0f;
    pb.max_value = 100.0f;      /* deliberately NOT 0..1 */
    jce_scene_set_ui_progress_bar(s, e, &pb);

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", SCRIPT_FILE);
    jce_scene_set_script(s, e, &sc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_SCRIPT, true);

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;

    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    JceUIProgressBarComponent *after = jce_scene_get_ui_progress_bar(s, e);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, after->value,
        "a value below min_value was not clamped to min_value");

    /* An entity with NO progress bar answers nil rather than zero, so a script
     * can tell "no bar" from "a bar reading zero" -- the same shape
     * ui_get_slider uses, and the reason the getter is fallible at all. */
    jce_test_write_file(SCRIPT_FILE,
        "local M = {}\n"
        "function M:on_start()\n"
        "  assert(jce.ui_get_progress(self.entity) == nil, 'absent bar')\n"
        "  jce.ui_set_progress(self.entity, 5)\n"   /* must not crash */
        "end\n"
        "return M\n");
    JceEntity bare = jce_scene_create_entity(s, "no_bar");
    JceScriptComponent sc2;
    memset(&sc2, 0, sizeof sc2);
    snprintf(sc2.script_path, sizeof sc2.script_path, "%s", SCRIPT_FILE);
    jce_scene_set_script(s, bare, &sc2);
    jce_scene_set_component_enabled(s, bare, JCE_COMP_FLAG_SCRIPT, true);
    jce_runtime_step(rt, 0.016f);

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

/* ── The three widgets that had no accessor ─────────────────────────────
 *
 * Each drives the REAL runtime host from a REAL Lua script, because that is
 * the whole chain a shipped game uses: the manifest, the generated binding,
 * the host callback and the component.  A test of rt_script_ui_* directly
 * would pass with the binding unregistered.
 *
 * The assertions are inside the script AND on the component afterwards: a
 * getter that echoed back whatever the setter was handed would satisfy the
 * readback alone, and the component check is what refuses it. */
static void test_a_script_drives_a_dropdown_through_the_real_host(void)
{
    jce_test_write_file(SCRIPT_FILE,
        "local M = {}\n"
        "function M:on_start()\n"
        "  assert(jce.ui_get_dropdown(self.entity) == 1, 'authored index')\n"
        "  jce.ui_set_dropdown(self.entity, 2)\n"
        "  assert(jce.ui_get_dropdown(self.entity) == 2, 'readback')\n"
        "  jce.ui_set_dropdown(self.entity, 99)\n"
        "end\n"
        "return M\n");

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "dd");

    JceUIDropdownComponent dd;
    memset(&dd, 0, sizeof dd);
    snprintf(dd.options[0], sizeof dd.options[0], "%s", "Low");
    snprintf(dd.options[1], sizeof dd.options[1], "%s", "Medium");
    snprintf(dd.options[2], sizeof dd.options[2], "%s", "High");
    dd.option_count   = 3;
    dd.selected_index = 1;
    dd.interactable   = true;
    jce_scene_set_ui_dropdown(s, e, &dd);

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", SCRIPT_FILE);
    jce_scene_set_script(s, e, &sc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_SCRIPT, true);

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;
    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    JceUIDropdownComponent *after = jce_scene_get_ui_dropdown(s, e);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, after->selected_index,
        "an index past the last option was not clamped: the component and "
        "the picture would disagree, and a script reading back what it just "
        "wrote would get an option the dropdown is not showing");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

static void test_a_script_drives_an_input_field_through_the_real_host(void)
{
    jce_test_write_file(SCRIPT_FILE,
        "local M = {}\n"
        "function M:on_start()\n"
        "  assert(jce.ui_get_input_text(self.entity) == 'hello', 'authored')\n"
        "  jce.ui_set_input_text(self.entity, 'world')\n"
        "  assert(jce.ui_get_input_text(self.entity) == 'world', 'readback')\n"
        "end\n"
        "return M\n");

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "field");

    JceUIInputFieldComponent f;
    memset(&f, 0, sizeof f);
    snprintf(f.text, sizeof f.text, "%s", "hello");
    f.interactable = true;
    jce_scene_set_ui_input_field(s, e, &f);

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", SCRIPT_FILE);
    jce_scene_set_script(s, e, &sc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_SCRIPT, true);

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;
    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    JceUIInputFieldComponent *after = jce_scene_get_ui_input_field(s, e);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("world", after->text,
        "the script wrote into a COPY: the component is the source of truth "
        "and a setter that did not reach it would leave the canvas drawing "
        "the old value");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

static void test_a_script_drives_a_scroll_view_through_the_real_host(void)
{
    jce_test_write_file(SCRIPT_FILE,
        "local M = {}\n"
        "function M:on_start()\n"
        "  local x, y = jce.ui_get_scroll(self.entity)\n"
        "  assert(x == 0 and y == 0, 'authored offset')\n"
        "  jce.ui_set_scroll(self.entity, 5, 40)\n"
        "  local nx, ny = jce.ui_get_scroll(self.entity)\n"
        "  assert(nx == 0, 'a disabled axis stays pinned')\n"
        "  assert(ny == 40, 'the enabled axis moved')\n"
        "  jce.ui_set_scroll(self.entity, 0, 9999)\n"
        "end\n"
        "return M\n");

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "sv");

    JceUIScrollViewComponent sv;
    memset(&sv, 0, sizeof sv);
    sv.content_size[1] = 500.0f;   /* vertical only, on purpose */
    sv.vertical        = true;
    sv.horizontal      = false;
    sv.interactable    = true;
    jce_scene_set_ui_scroll_view(s, e, &sv);

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", SCRIPT_FILE);
    jce_scene_set_script(s, e, &sc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_SCRIPT, true);

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;
    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    JceUIScrollViewComponent *after = jce_scene_get_ui_scroll_view(s, e);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, after->scroll_position[0],
        "a disabled axis was moved: a script could scroll somewhere the "
        "wheel cannot");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(500.0f, after->scroll_position[1],
        "an offset past the content extent was not clamped");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_script_loads_and_runs_through_runtime);
    RUN_TEST(test_a_script_drives_a_progress_bar_through_the_real_host);
    RUN_TEST(test_a_script_drives_a_dropdown_through_the_real_host);
    RUN_TEST(test_a_script_drives_an_input_field_through_the_real_host);
    RUN_TEST(test_a_script_drives_a_scroll_view_through_the_real_host);
    RUN_TEST(test_on_collision_dispatches_to_both);
    RUN_TEST(test_spawn_instantiates_and_wires);
    RUN_TEST(test_hot_reload_preserves_self);
    return UNITY_END();
}
