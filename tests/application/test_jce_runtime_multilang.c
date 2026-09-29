/*
 * test_jce_runtime_multilang.c — two languages in ONE scene, through the real
 * jce_runtime_create / jce_runtime_step path.
 *
 * Until this shipped, a project was all-Lua or all-Python: the runtime held
 * ONE JceScript handle and chose its language once per process from
 * JCE_SCRIPT_LANGUAGE.  A `.py` attached to an entity ran nothing.  Now the
 * language comes from the script PATH — through the extension claims backends
 * make for themselves — and the runtime holds one VM per language.
 *
 * THE SECOND LANGUAGE HERE IS A TEST-OWNED JceScriptVM, and that is
 * deliberate rather than a shortcut:
 *
 *   - the Python and Java backends are OFF in the default build
 *     (JCE_BUILD_SCRIPT_JAVA defaults OFF; the Python VM is skipped when no
 *     embeddable CPython is present), and the feature this file guards must
 *     work in the configuration most people actually build.  A test that only
 *     runs where CPython is installed would be green-where-nobody-looks.
 *   - what is under test is the RUNTIME's selection and routing, not
 *     CPython's lifecycle.  The real backends already have their own
 *     cross-language lifecycle differentials against Lua.
 *
 * The companion test_backend_absent_does_not_run_and_is_not_silently_lua
 * covers the OTHER half — a `.py` in a build with no Python backend — which
 * is the common shipping configuration and the one requirement 3 is about.
 *
 * WHAT THIS FILE FORBIDS, precisely:
 *
 *   1. A scene mixing two languages running only one of them.
 *   2. Dispatching an instance to a VM that did not issue it.  A
 *      JceScriptInstance is a bare uint32 private to its VM and both VMs here
 *      number from a small integer, so their id spaces OVERLAP — asserted
 *      below rather than assumed, because a test that cannot tell the two
 *      apart proves nothing about routing.  The probe VM refuses and COUNTS
 *      any id it did not issue.
 *   3. An unresolvable script path silently becoming Lua.
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/script/jce_script_vm.h>
#include <jce/os/core/jce_alloc.h>

#include "unity.h"

#include "jce_test_file_util.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LUA_FILE   "jce_rt_multilang_a.lua"
#define PROBE_FILE "jce_rt_multilang_b.tvm"
#define ORPHAN_FILE "jce_rt_multilang_c.py"

/* ── The probe language ───────────────────────────────────────────────────
 *
 * A whole JceScriptVM in ~120 lines.  Its "source language" is one line of
 * three numbers: on_start writes them as the entity's position, on_update
 * adds PROBE_STEP to x.  That mirrors what the Lua script below does, so the
 * two entities are distinguishable ONLY by which VM ran them.
 */
#define PROBE_LANGUAGE  "tvm"
#define PROBE_EXTENSION "tvm"
#define PROBE_STEP      7.0f
#define PROBE_MAX_INST  8

/* "x y z" -> three floats.  strtod rather than sscanf: MSVC deprecates the
 * scanf family at the warning level this repo builds at, and the noise would
 * land on this file. */
static bool probe_parse_xyz(const char *text, float *x, float *y, float *z)
{
    char  *end = NULL;
    double a, b, c;
    if (!text) return false;
    a = strtod(text, &end);  if (end == text) return false;
    text = end;
    b = strtod(text, &end);  if (end == text) return false;
    text = end;
    c = strtod(text, &end);  if (end == text) return false;
    *x = (float)a; *y = (float)b; *z = (float)c;
    return true;
}

typedef struct ProbeInst {
    JceScriptEntity owner;
    float           x, y, z;
} ProbeInst;

typedef struct ProbeScript {
    JceScriptVMHeader hdr;          /* MUST be first */
    JceScriptHost     host;
    bool              have_host;
    ProbeInst         inst[PROBE_MAX_INST];
    int               inst_count;   /* ids are 1..inst_count */
} ProbeScript;

/* Cross-call bookkeeping the test asserts on. */
static int g_probe_created;
static int g_probe_destroyed;
static int g_probe_starts;
static int g_probe_updates;
/* Every dispatch carrying an instance id THIS VM never issued.  With one VM
 * per process this could not happen; with several it is the failure the ref
 * type exists to make inexpressible, so it is counted rather than ignored. */
static int g_probe_foreign;
/* Global-handler and coroutine fan-out: both are per-LANGUAGE, not per
 * instance, so both are invisible to every per-entity assertion above. */
static int g_probe_named;
static int g_probe_coroutines;

static const JceScriptVM *g_probe_self;

static ProbeInst *probe_resolve(ProbeScript *s, JceScriptInstance i)
{
    if (i == 0 || (int)i > s->inst_count) {
        ++g_probe_foreign;
        return NULL;
    }
    return &s->inst[i - 1];
}

static JceScript *probe_create_sized(const JceScriptHost *host, size_t host_size)
{
    ProbeScript *s = (ProbeScript *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->hdr.vm = g_probe_self;
    if (host && host_size > 0) {
        const size_t n = host_size < sizeof s->host ? host_size : sizeof s->host;
        memcpy(&s->host, host, n);          /* never sizeof(*host) */
        s->have_host = true;
    }
    ++g_probe_created;
    return (JceScript *)s;
}

static void probe_destroy(JceScript *sc)
{
    if (!sc) return;
    ++g_probe_destroyed;
    free(sc);
}

static JceScriptInstance probe_instantiate(JceScript *sc, const char *path,
                                           JceScriptEntity owner)
{
    ProbeScript *s = (ProbeScript *)sc;
    uint64_t     size = 0;
    void        *buf;
    char         text[128];
    float        x = 0.0f, y = 0.0f, z = 0.0f;

    if (!s || !path || !s->have_host || !s->host.read_file) return 0;
    if (s->inst_count >= PROBE_MAX_INST) return 0;

    /* Through the HOST's reader, like every other backend: the point is that a
     * .tvm lives in the same mounted directory / PAK a .lua does. */
    buf = s->host.read_file(s->host.user, path, &size);
    if (!buf || size == 0) {
        if (buf) jce_free(buf);
        return 0;
    }
    if (size >= sizeof text) size = sizeof text - 1;
    memcpy(text, buf, (size_t)size);
    text[size] = '\0';
    jce_free(buf);

    if (!probe_parse_xyz(text, &x, &y, &z)) return 0;

    s->inst[s->inst_count].owner = owner;
    s->inst[s->inst_count].x = x;
    s->inst[s->inst_count].y = y;
    s->inst[s->inst_count].z = z;
    ++s->inst_count;
    return (JceScriptInstance)s->inst_count;
}

static JceScriptInstance probe_instantiate_source(JceScript *sc, const char *n,
                                                  const char *src,
                                                  JceScriptEntity o)
{
    ProbeScript *s = (ProbeScript *)sc;
    float        x = 0.0f, y = 0.0f, z = 0.0f;
    (void)n;
    if (!s || s->inst_count >= PROBE_MAX_INST) return 0;
    if (!probe_parse_xyz(src, &x, &y, &z)) return 0;
    s->inst[s->inst_count].owner = o;
    s->inst[s->inst_count].x = x;
    s->inst[s->inst_count].y = y;
    s->inst[s->inst_count].z = z;
    ++s->inst_count;
    return (JceScriptInstance)s->inst_count;
}

static void probe_call_start(JceScript *sc, JceScriptInstance i)
{
    ProbeScript *s = (ProbeScript *)sc;
    ProbeInst   *in;
    if (!s) return;
    in = probe_resolve(s, i);
    if (!in) return;
    ++g_probe_starts;
    if (s->have_host && s->host.set_position)
        s->host.set_position(s->host.user, in->owner, in->x, in->y, in->z);
}

/* An explicit no-op.  jce_script_vm_register refuses a table with any
 * NULL slot, so a probe VM must fill the appended one even though this
 * test is about two languages coexisting rather than about the fixed
 * clock. */
static void probe_call_fixed_update(JceScript *sc, JceScriptInstance i,
                                    float dt)
{ (void)sc; (void)i; (void)dt; }

static void probe_call_update(JceScript *sc, JceScriptInstance i, float dt)
{
    ProbeScript *s = (ProbeScript *)sc;
    ProbeInst   *in;
    float        p[3] = {0.0f, 0.0f, 0.0f};
    (void)dt;
    if (!s) return;
    in = probe_resolve(s, i);
    if (!in) return;
    ++g_probe_updates;
    if (!s->have_host || !s->host.get_position || !s->host.set_position) return;
    if (!s->host.get_position(s->host.user, in->owner, p)) return;
    s->host.set_position(s->host.user, in->owner, p[0] + PROBE_STEP, p[1], p[2]);
}

static void probe_release(JceScript *sc, JceScriptInstance i)
{
    ProbeScript *s = (ProbeScript *)sc;
    if (s) (void)probe_resolve(s, i);   /* still counts a foreign id */
}

static void probe_call_collision(JceScript *sc, JceScriptInstance i,
                                 JceScriptEntity o)
{ ProbeScript *s = (ProbeScript *)sc; (void)o; if (s) (void)probe_resolve(s, i); }

static void probe_call_message(JceScript *sc, JceScriptInstance i,
                               const char *m, double n, const char *str)
{
    ProbeScript *s = (ProbeScript *)sc;
    (void)m; (void)n; (void)str;
    if (s) (void)probe_resolve(s, i);
}

static void probe_call_anim_event(JceScript *sc, JceScriptInstance i,
                                  uint32_t id, const char *n,
                                  float f0, float f1, int i0)
{
    ProbeScript *s = (ProbeScript *)sc;
    (void)id; (void)n; (void)f0; (void)f1; (void)i0;
    if (s) (void)probe_resolve(s, i);
}

/* "no such global" is false, which is what makes the runtime's ask-each-live-
 * language loop correct: false means "not mine, ask the next one".  This
 * probe owns exactly one name, so a dispatch that only ever asks the FIRST
 * language can never find it. */
#define PROBE_HANDLER "probe_only_handler"

static bool probe_call_named(JceScript *sc, const char *f, JceScriptEntity e)
{
    (void)sc; (void)e;
    if (f && strcmp(f, PROBE_HANDLER) == 0) { ++g_probe_named; return true; }
    return false;
}
static bool probe_call_named_num(JceScript *sc, const char *f,
                                 JceScriptEntity e, double v)
{ (void)sc; (void)f; (void)e; (void)v; return false; }
static bool probe_call_named_str(JceScript *sc, const char *f,
                                 JceScriptEntity e, const char *str)
{ (void)sc; (void)f; (void)e; (void)str; return false; }

static int probe_instance_count(const JceScript *sc)
{
    const ProbeScript *s = (const ProbeScript *)sc;
    return s ? s->inst_count : 0;
}
static void probe_update_coroutines(JceScript *sc, float dt)
{ (void)sc; (void)dt; ++g_probe_coroutines; }
static JceScriptModule probe_compile_module(JceScript *sc, const char *n,
                                            const char *src, size_t len)
{ (void)sc; (void)n; (void)src; (void)len; return 0u; }
static void probe_rebind_instance(JceScript *sc, JceScriptInstance i,
                                  JceScriptModule m)
{ ProbeScript *s = (ProbeScript *)sc; (void)m; if (s) (void)probe_resolve(s, i); }
static void probe_release_module(JceScript *sc, JceScriptModule m)
{ (void)sc; (void)m; }

static const JceScriptVM k_probe_vm = {
    sizeof(JceScriptVM),
    PROBE_LANGUAGE,
    probe_create_sized,
    probe_destroy,
    probe_instantiate,
    probe_instantiate_source,
    probe_call_start,
    probe_call_update,
    probe_release,
    probe_call_collision,
    probe_call_message,
    probe_call_anim_event,
    probe_call_named,
    probe_call_named_num,
    probe_call_named_str,
    probe_instance_count,
    probe_update_coroutines,
    probe_compile_module,
    probe_rebind_instance,
    probe_release_module,
    probe_call_fixed_update,
};

/* Registered once for the process — the registry never unregisters, because
 * live handles hold pointers into it. */
static void register_probe_once(void)
{
    static bool done;
    if (done) return;
    done = true;
    g_probe_self = &k_probe_vm;
    TEST_ASSERT_TRUE_MESSAGE(jce_script_vm_register(&k_probe_vm),
        "the probe VM was refused; every assertion below would then be "
        "measuring the absence of a second language rather than routing");
    TEST_ASSERT_TRUE_MESSAGE(
        jce_script_vm_register_extension(PROBE_EXTENSION, PROBE_LANGUAGE),
        "the probe VM could not claim its extension");
}

void setUp(void)
{
    register_probe_once();
    g_probe_starts = g_probe_updates = g_probe_foreign = 0;
    g_probe_named = g_probe_coroutines = 0;
    g_probe_created = g_probe_destroyed = 0;
}

void tearDown(void)
{
    remove(LUA_FILE);
    remove(PROBE_FILE);
    remove(ORPHAN_FILE);
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

/* ------------------------------------------------------------------ *
 *  (1) + (2): a .lua and a second language in ONE scene both run, each
 *  through its own VM.
 * ------------------------------------------------------------------ */
static void test_two_languages_in_one_scene_both_run(void)
{
    JceTransform *a, *b;
    JceEntity     ea, eb;
    JceScene     *s;
    JceRuntimeDesc desc;
    JceRuntime   *rt;

    jce_test_write_file(LUA_FILE,
        "local M = {}\n"
        "function M:on_start()\n"
        "  jce.set_position(self.entity, 11, 22, 33)\n"
        "end\n"
        "function M:on_update(dt)\n"
        "  local x,y,z = jce.get_position(self.entity)\n"
        "  jce.set_position(self.entity, x + 100, y, z)\n"
        "end\n"
        "return M\n");
    jce_test_write_file(PROBE_FILE, "44 55 66\n");

    s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    ea = make_scripted(s, "lua_entity",   LUA_FILE);
    eb = make_scripted(s, "probe_entity", PROBE_FILE);

    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;

    rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    /* Both on_start ran during create(), each in its own language. */
    a = jce_scene_get_transform(s, ea);
    b = jce_scene_get_transform(s, eb);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(11.0f, a->position.x,
        "the .lua script's on_start did not run");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(44.0f, b->position.x,
        "the second language's on_start did not run — a scene mixing two "
        "languages ran only one of them");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(55.0f, b->position.y,
        "the second language's on_start wrote the wrong values");
    TEST_ASSERT_EQUAL_MESSAGE(1, g_probe_created,
        "the probe VM was created a number of times other than once — it must "
        "be stood up lazily, on the first script of its language");
    TEST_ASSERT_EQUAL_MESSAGE(1, g_probe_starts,
        "the probe VM received a number of on_start dispatches other than one");

    /* One step: each language's own on_update runs on its own instance. */
    jce_runtime_step(rt, 0.016f);
    a = jce_scene_get_transform(s, ea);
    b = jce_scene_get_transform(s, eb);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(111.0f, a->position.x,
        "the .lua entity's on_update did not run, or ran through the wrong VM");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(44.0f + PROBE_STEP, b->position.x,
        "the second language's on_update did not run, or ran through the "
        "wrong VM");
    TEST_ASSERT_EQUAL_MESSAGE(1, g_probe_updates,
        "the probe VM received a number of on_update dispatches other than "
        "one per step — it must see ITS instance and no other");

    /* THE ROUTING ASSERTION.  A JceScriptInstance is private to its issuer;
     * the probe counts every id it never issued. */
    TEST_ASSERT_EQUAL_MESSAGE(0, g_probe_foreign,
        "the probe VM was handed an instance id it never issued — an instance "
        "reached a VM that did not create it, which is a wrong-pointer "
        "dereference and not an error return");

    /* Cooperative coroutines are per-VM, not per instance: a scene with two
     * languages has two VMs and BOTH must be advanced, or one language's
     * jce.wait_seconds never resumes and nothing anywhere reports it. */
    TEST_ASSERT_EQUAL_MESSAGE(1, g_probe_coroutines,
        "the probe VM's coroutine scheduler was not advanced exactly once "
        "per step — the runtime is ticking only one language's timers");

    /* A GLOBAL handler (UIButton on_click, sequencer EVENT key) names a
     * function, not an instance, so there is no ref to route it with: every
     * live language is asked in creation order and the first that HANDLED it
     * wins.  The probe owns this name and Lua does not, so a dispatch that
     * stops at the first VM cannot find it. */
    TEST_ASSERT_TRUE_MESSAGE(
        jce_runtime_dispatch_ui_click(rt, (uint64_t)eb, PROBE_HANDLER),
        "a global handler defined only in the SECOND language was not found "
        "— the runtime asked only the first VM, so every UI callback written "
        "in the second language is a silent no-op");
    TEST_ASSERT_EQUAL_MESSAGE(1, g_probe_named,
        "the probe VM's call_named ran a number of times other than once");
    TEST_ASSERT_FALSE_MESSAGE(
        jce_runtime_dispatch_ui_click(rt, (uint64_t)eb, "nobody_defines_this"),
        "a handler no language defines reported HANDLED — 'false' is what "
        "makes ask-the-next-one correct, and a true here would stop the walk "
        "at the first VM for every real handler too");

    jce_runtime_destroy(rt);
    TEST_ASSERT_EQUAL_MESSAGE(1, g_probe_destroyed,
        "the probe VM was not destroyed exactly once with the runtime");
    jce_scene_destroy(s);
}

/* ------------------------------------------------------------------ *
 *  The premise of (2): the two VMs' instance id spaces OVERLAP, so
 *  "dispatched to the wrong VM" is a state the probe can actually
 *  distinguish.  Asserted, not assumed — a routing test whose two id
 *  spaces happened to be disjoint would pass while proving nothing.
 * ------------------------------------------------------------------ */
static void test_the_two_id_spaces_overlap(void)
{
    JceScript        *lua_vm;
    JceScript        *probe_vm;
    JceScriptInstance lua_ids[5];
    JceScriptInstance probe_ids[5];
    int               i, j, overlap = 0;

    lua_vm   = jce_script_vm_create(JCE_SCRIPT_VM_DEFAULT_LANGUAGE, NULL, 0);
    probe_vm = jce_script_vm_create(PROBE_LANGUAGE, NULL, 0);
    TEST_ASSERT_NOT_NULL_MESSAGE(lua_vm,   "could not create the built-in VM");
    TEST_ASSERT_NOT_NULL_MESSAGE(probe_vm, "could not create the probe VM");

    for (i = 0; i < 5; ++i) {
        lua_ids[i] = jce_script_instantiate_source(
            lua_vm, "=idspace", "local M = {}\nreturn M\n", (JceScriptEntity)(i + 1));
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, lua_ids[i],
            "the built-in VM refused a trivial source chunk, so this test "
            "would compare an empty id set against a full one");
    }
    for (i = 0; i < 5; ++i) {
        probe_ids[i] = jce_script_instantiate_source(
            probe_vm, "=idspace", "1 2 3", (JceScriptEntity)(i + 1));
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, probe_ids[i],
            "the probe VM refused a trivial source chunk, so this test would "
            "compare a full id set against an empty one — silence comparing "
            "equal to silence");
    }

    for (i = 0; i < 5; ++i)
        for (j = 0; j < 5; ++j)
            if (lua_ids[i] == probe_ids[j]) ++overlap;

    TEST_ASSERT_TRUE_MESSAGE(overlap > 0,
        "the two VMs issue DISJOINT instance ids, so test_two_languages_in_"
        "one_scene_both_run cannot detect a cross-VM dispatch and its "
        "g_probe_foreign assertion proves nothing");

    jce_script_destroy(lua_vm);
    jce_script_destroy(probe_vm);
}

/* ------------------------------------------------------------------ *
 *  (3) A script whose extension nobody claims does NOT become Lua.
 *      This is the common shipping configuration for a `.py`: the
 *      Python backend is not linked into this executable.
 * ------------------------------------------------------------------ */
static void test_backend_absent_does_not_run_and_is_not_silently_lua(void)
{
    JceTransform  *a, *c;
    JceEntity      ea, ec;
    JceScene      *s;
    JceRuntimeDesc desc;
    JceRuntime    *rt;

    TEST_ASSERT_NULL_MESSAGE(jce_script_vm_language_for_path("x.py"),
        "a backend claims '.py' in this executable, so this test is not "
        "measuring the absent-backend case it was written for");

    jce_test_write_file(LUA_FILE,
        "local M = {}\n"
        "function M:on_start() jce.set_position(self.entity, 11, 22, 33) end\n"
        "return M\n");
    /* Valid Lua on purpose.  If the runtime fell back to the built-in, this
     * would LOAD and move the entity — which is exactly the silent wrong-
     * language run the no-fallback rule exists to prevent, and asserting on
     * a file that could not compile anywhere would not distinguish the two. */
    jce_test_write_file(ORPHAN_FILE,
        "local M = {}\n"
        "function M:on_start() jce.set_position(self.entity, 99, 98, 97) end\n"
        "return M\n");

    s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    ea = make_scripted(s, "lua_entity",    LUA_FILE);
    ec = make_scripted(s, "orphan_entity", ORPHAN_FILE);

    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;

    rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    a = jce_scene_get_transform(s, ea);
    c = jce_scene_get_transform(s, ec);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(11.0f, a->position.x,
        "the .lua entity stopped running because an unrelated entity's "
        "language could not be resolved — one unresolvable script must not "
        "disable the scene");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, c->position.x,
        "a script whose extension nobody claims RAN — it was silently loaded "
        "as Lua, which is the wrong-language run the no-fallback rule exists "
        "to prevent");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_two_id_spaces_overlap);
    RUN_TEST(test_two_languages_in_one_scene_both_run);
    RUN_TEST(test_backend_absent_does_not_run_and_is_not_silently_lua);
    return UNITY_END();
}
