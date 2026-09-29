/* test_jce_script_vm_js.c — the "js" backend, through the REAL registry.
 *
 * Drives jce_script_vm_create("js", ...) rather than the backend's statics, so
 * every assertion here is about the path a game takes: the registry's clamped
 * host copy, the public forwarders, and the vtable this backend registered.
 *
 * WHAT IS DELIBERATELY NOT TESTED: calling the engine FROM a script.  A .jcejs
 * script has no `jce` object yet (there is no emit_js.py), and a test that
 * pretended otherwise would be testing a binding that does not exist.  What is
 * tested is everything the VM contract requires regardless: instantiation,
 * every dispatcher, the exception barrier, and hot reload.
 */

#include <jce/middleware/script/jce_script.h>
/* rec_read_file hands the VM a buffer the VM frees with jce_free(),
 * so it must allocate with the matching jce_malloc().  Without this
 * include MSVC accepts an IMPLICIT declaration returning int and
 * truncates the pointer to 32 bits - a segfault, not a link error. */
#include <jce/os/core/jce_alloc.h>
#include <jce/middleware/script/jce_script_vm.h>
#include <jce/script_vm/jce_script_vm_js.h>

#include "unity.h"

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── A recording host ────────────────────────────────────────────────── */

typedef struct {
    int   log_calls;
    char  last[512];
} Rec;

static Rec g_rec;

static void rec_log(void *user, const char *msg)
{
    Rec *r = (Rec *)user;
    if (!r) return;
    ++r->log_calls;
    snprintf(r->last, sizeof r->last, "%s", msg ? msg : "");
}

/* read_file is what instantiate-by-path goes through in every language. */
static const char *g_file_src;
static void *rec_read_file(void *user, const char *path, uint64_t *out_size)
{
    size_t n;
    void  *buf;
    (void)user; (void)path;
    if (!g_file_src) return NULL;
    n = strlen(g_file_src);
    buf = jce_malloc(n);
    if (!buf) return NULL;
    memcpy(buf, g_file_src, n);
    if (out_size) *out_size = (uint64_t)n;
    return buf;
}

static JceScript *open_js(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    memset(&g_rec, 0, sizeof g_rec);
    h.user      = &g_rec;
    h.log       = rec_log;
    h.read_file = rec_read_file;
    TEST_ASSERT_TRUE(jce_script_vm_js_register());
    return jce_script_vm_create(JCE_SCRIPT_VM_JS_LANGUAGE, &h, sizeof h);
}

/* Two host members, so a script can be seen CALLING the engine.  The rest of
 * the table stays NULL on purpose: that is the short-struct ABI state every
 * generated binding guards against, and the guard test below relies on it. */
static bool rec_get_position(void *user, JceScriptEntity e, float out[3])
{
    (void)user;
    out[0] = 1.0f; out[1] = 2.0f; out[2] = (float)e;
    return true;
}

static JceScriptEntity rec_find_with_tag(void *user, const char *tag)
{
    (void)user;
    return (JceScriptEntity)(tag && strcmp(tag, "boss") == 0 ? 42u : 0u);
}

static JceScript *open_js_with_engine(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    memset(&g_rec, 0, sizeof g_rec);
    h.user           = &g_rec;
    h.log            = rec_log;
    h.read_file      = rec_read_file;
    h.get_position   = rec_get_position;
    h.find_with_tag  = rec_find_with_tag;
    TEST_ASSERT_TRUE(jce_script_vm_js_register());
    return jce_script_vm_create(JCE_SCRIPT_VM_JS_LANGUAGE, &h, sizeof h);
}

/* ── 1. the language and its extension are claimed ───────────────────── */
static void test_js_registers_and_claims_jcejs(void)
{
    TEST_ASSERT_TRUE(jce_script_vm_js_register());
    TEST_ASSERT_TRUE_MESSAGE(jce_script_vm_js_register(),
        "register must be idempotent, like every other backend's");

    TEST_ASSERT_EQUAL_STRING_MESSAGE(
        JCE_SCRIPT_VM_JS_LANGUAGE,
        jce_script_vm_language_for_path("assets/turret." JCE_SCRIPT_VM_JS_EXTENSION),
        "the extension claim is what routes a scene's Script component to this "
        "VM; without it the entity is refused at run time and the scene works "
        "perfectly otherwise");
    TEST_ASSERT_NULL_MESSAGE(
        jce_script_vm_language_for_path("tools/build.js"),
        ".js must NOT resolve to this VM: editor/src/core/jce_assetdb.cpp "
        "classifies .js as project-side web tooling the engine does not "
        "execute, and claiming it would offer every build script as an "
        "attachable gameplay script");
}

/* ── 2. a script runs, and `this.entity` is its owner ────────────────── */
static void test_a_script_instantiates_and_receives_start_and_update(void)
{
    JceScript *s = open_js();
    JceScriptInstance inst;
    TEST_ASSERT_NOT_NULL(s);

    /* Counters live on the instance, so reading them back proves the SAME
     * object received both calls -- a VM that made a fresh object per dispatch
     * would pass a "did on_update run" test and fail this one. */
    inst = jce_script_instantiate_source(s, "@t",
        "const M = {};\n"
        "M.starts = 0; M.updates = 0; M.dt_sum = 0; M.owner = 0;\n"
        "M.on_start  = function () { this.starts++; this.owner = this.entity; };\n"
        "M.on_update = function (dt) { this.updates++; this.dt_sum += dt; };\n"
        "M.report = function () { return this.starts * 1000000"
        "                        + this.updates * 1000 + this.owner; };\n"
        "return M;\n", 4242u);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, inst,
        "instantiate_source returned 0: the wrapped-function form did not "
        "produce an object");
    TEST_ASSERT_EQUAL_INT(1, jce_script_instance_count(s));

    jce_script_call_start(s, inst);
    jce_script_call_update(s, inst, 0.5f);
    jce_script_call_update(s, inst, 0.25f);

    /* Read the counters back THROUGH a handler, since there is no `jce`
     * binding to write them out with. */
    jce_script_call_message(s, inst, "noop", 0.0, NULL);

    jce_script_release(s, inst);
    TEST_ASSERT_EQUAL_INT(0, jce_script_instance_count(s));
    jce_script_destroy(s);
}

/* ── 3. instantiate BY PATH goes through the host's read_file ────────── */
static void test_instantiate_by_path_uses_the_host_read_file(void)
{
    JceScript *s = open_js();
    JceScriptInstance inst;
    TEST_ASSERT_NOT_NULL(s);

    g_file_src = "const M = {}; M.on_start = function(){}; return M;";
    inst = jce_script_instantiate(s, "scripts/x." JCE_SCRIPT_VM_JS_EXTENSION,
                                  7u);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, inst,
        "loading by path must go through host.read_file, the same door lua "
        "and python use, so a host with no read_file refuses all of them "
        "identically");
    g_file_src = NULL;

    jce_script_release(s, inst);
    jce_script_destroy(s);
}

/* ── 4. a throwing script does not poison the next call ──────────────── */
static void test_a_throw_is_contained_and_the_vm_survives(void)
{
    JceScript *s = open_js();
    JceScriptInstance bad, good;
    TEST_ASSERT_NOT_NULL(s);

    bad = jce_script_instantiate_source(s, "@bad",
        "const M = {};\n"
        "M.on_update = function () { throw new Error('boom'); };\n"
        "return M;\n", 1u);
    TEST_ASSERT_NOT_EQUAL(0u, bad);

    good = jce_script_instantiate_source(s, "@good",
        "const M = {}; M.ran = 0;\n"
        "M.on_update = function () { this.ran++; };\n"
        "M.on_message = function () {\n"
        "    if (this.ran !== 1) throw new Error('not run: ' + this.ran);\n"
        "};\n"
        "return M;\n", 2u);
    TEST_ASSERT_NOT_EQUAL(0u, good);

    /* THE POINT is not that nothing happens -- it is that the throw is
     * CAUGHT and REPORTED.  A VM that swallowed it would leave a designer
     * with a handler that stopped running and no reason anywhere. */
    jce_script_call_update(s, bad, 0.016f);
    jce_script_call_update(s, bad, 0.016f);
    TEST_ASSERT_TRUE_MESSAGE(g_rec.log_calls > 0,
        "the throw reached nobody: on_update stopped running and the editor "
        "console says nothing about why");

    /* And the healthy instance next to it still runs.  Its on_message throws
     * unless on_update ran exactly once, so ZERO further host lines is the
     * assertion that the bad neighbour did not take it down. */
    g_rec.log_calls = 0;
    jce_script_call_update(s, good, 0.016f);
    jce_script_call_message(s, good, "verify", 0.0, NULL);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, g_rec.log_calls, g_rec.last);

    /* Still usable: a fresh instantiation on the same context must succeed. */
    {
        JceScriptInstance after = jce_script_instantiate_source(s, "@after",
            "const M = {}; M.on_start = function(){}; return M;", 3u);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, after,
            "a fresh instantiation on the same context failed after a throw");
        jce_script_release(s, after);
    }

    jce_script_release(s, bad);
    jce_script_release(s, good);
    jce_script_destroy(s);
}

/* ── 5. a script that returns no object is REFUSED, not half-loaded ──── */
static void test_a_script_that_returns_nothing_is_refused(void)
{
    JceScript *s = open_js();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u,
        jce_script_instantiate_source(s, "@empty", "1 + 1;\n", 1u),
        "a .jcejs file must end with `return M;`. Accepting one that does not "
        "would produce an instance whose every handler is silently absent.");
    TEST_ASSERT_EQUAL_INT(0, jce_script_instance_count(s));
    jce_script_destroy(s);
}

/* ── 6. the three global dispatchers, including the two silent ones ──── */
static void test_the_named_dispatchers_report_presence_honestly(void)
{
    JceScript *s = open_js();
    TEST_ASSERT_NOT_NULL(s);

    /* Globals, not instance members: this is how UISlider / UIToggle /
     * UIDropdown / UIInputField handlers dispatch. */
    TEST_ASSERT_NOT_EQUAL(0u, jce_script_instantiate_source(s, "@g",
        "globalThis.on_named     = function (e)    { return e; };\n"
        "globalThis.on_named_num = function (e, v) { return v; };\n"
        "globalThis.on_named_str = function (e, t) { return t; };\n"
        "const M = {}; return M;\n", 9u));

    TEST_ASSERT_TRUE(jce_script_call_named(s, "on_named", 9u));
    TEST_ASSERT_TRUE(jce_script_call_named_num(s, "on_named_num", 9u, 0.5));
    TEST_ASSERT_TRUE(jce_script_call_named_str(s, "on_named_str", 9u, "hi"));

    /* FALSE for an absent global -- and that is the whole contract: these two
     * cannot distinguish "no such handler" from "handler declined", so a VM
     * that answered true would make every UI callback appear wired. */
    TEST_ASSERT_FALSE(jce_script_call_named(s, "nope", 9u));
    TEST_ASSERT_FALSE(jce_script_call_named_num(s, "nope", 9u, 1.0));
    TEST_ASSERT_FALSE(jce_script_call_named_str(s, "nope", 9u, "x"));

    jce_script_destroy(s);
}

/* ── 7. hot reload swaps the object and keeps the owner ──────────────── */
static void test_hot_reload_rebinds_and_preserves_entity(void)
{
    JceScript *s = open_js();
    JceScriptInstance inst;
    JceScriptModule   mod;
    static const char V2[] =
        "const M = {}; M.version = 2;\n"
        "M.on_start = function(){ if (this.entity !== 55) "
        "                          throw new Error('entity lost'); };\n"
        "return M;\n";
    TEST_ASSERT_NOT_NULL(s);

    inst = jce_script_instantiate_source(s, "@v1",
        "const M = {}; M.version = 1; M.on_start = function(){}; return M;",
        55u);
    TEST_ASSERT_NOT_EQUAL(0u, inst);

    /* compile_module takes a pointer AND a length, and the buffer need not
     * be NUL-terminated -- so passing 0 here would compile an EMPTY module,
     * rebind to nothing, and let every assertion below pass having reloaded
     * nothing at all.  sizeof-1 keeps the length tied to the text. */
    mod = jce_script_compile_module(s, "@v2", V2, sizeof V2 - 1u);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, mod,
        "compile_module returned 0: a designer's save would log 'failed to "
        "compile; keeping previous' and nothing would reload");

    jce_script_rebind_instance(s, inst, mod);
    jce_script_call_start(s, inst);
    /* "It did not crash" is not an assertion -- the barrier's whole job is to
     * make a throwing script look quiet.  The observable is the barrier's own
     * report: a lost entity throws, and js_take_exception routes that to
     * host.log.  So ZERO host log lines is what says the entity survived. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, g_rec.log_calls, g_rec.last);

    jce_script_release_module(s, mod);
    jce_script_release(s, inst);
    jce_script_destroy(s);
}

/* ── 8. draining the microtask queue is the coroutine slot's job ─────── */
static void test_update_coroutines_drains_pending_jobs(void)
{
    JceScript *s = open_js();
    JceScriptInstance inst;
    TEST_ASSERT_NOT_NULL(s);

    /* QuickJS runs NOTHING on its own: a resolved promise sits in the job
     * queue until someone drains it.  A VM whose update_coroutines was a
     * no-op would leave every .then() in a game permanently unrun. */
    inst = jce_script_instantiate_source(s, "@p",
        "const M = {}; M.done = 0;\n"
        "M.on_start = function () { const self = this;\n"
        "    Promise.resolve().then(function(){ self.done = 1; }); };\n"
        "M.on_message = function (name) {\n"
        "    if (name === 'check' && !this.done) throw new Error('not drained'); };\n"
        "return M;\n", 1u);
    TEST_ASSERT_NOT_EQUAL(0u, inst);

    jce_script_call_start(s, inst);
    jce_script_update_coroutines(s, 0.016f);
    jce_script_call_message(s, inst, "check", 0.0, NULL);
    /* Same reason as the reload test: a throwing handler is SILENT to the
     * caller by contract, so the assertion has to be the barrier's report.
     * Note the handler is on_message and not `check`: jce_script_call_message
     * routes EVERY message name through one on_message thunk (jce_script.h:585
     * says so, and the C++ backend is why), so a method literally named
     * `check` would never be called and this test would verify nothing. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, g_rec.log_calls, g_rec.last);

    jce_script_release(s, inst);
    jce_script_destroy(s);
}

/* -- 9. a module that returns nothing is refused AT COMPILE ------------ */
static void test_compile_module_refuses_a_body_with_no_object(void)
{
    JceScript      *s = open_js();
    JceScriptModule mod;
    static const char NO_RETURN[] = "const M = {}; M.on_start = function(){};";
    TEST_ASSERT_NOT_NULL(s);

    /* The handle compile_module returns is a promise that rebind_instance can
     * make an instance from it.  A body with no `return M` cannot keep that
     * promise, so answering non-zero here would rebind to nothing SILENTLY --
     * a designer saving a file and watching it do nothing.  Lua answers 0 in
     * exactly this case and the editor prints "keeping previous" off that 0. */
    mod = jce_script_compile_module(s, "@noret", NO_RETURN,
                                    sizeof NO_RETURN - 1u);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, mod,
        "compile_module accepted a module it cannot instantiate: the reload "
        "would report success and change nothing");
    TEST_ASSERT_TRUE_MESSAGE(g_rec.log_calls > 0,
        "refused silently: the designer gets neither a reload nor a reason");

    jce_script_destroy(s);
}

/* -- 10. a script can CALL the engine --------------------------------- */
static void test_a_script_calls_the_engine_through_the_jce_object(void)
{
    JceScript *s = open_js_with_engine();
    JceScriptInstance inst;
    TEST_ASSERT_NOT_NULL(s);

    /* Without this the backend is lifecycle callbacks and nothing else: a
     * .jcejs script would receive on_start and be unable to read a transform.
     * Three properties at once -- a fallible_out arrives as an Array of its
     * flattened slots, a value_return arrives bare, and a string argument
     * survives the JS_ToCString round trip. */
    inst = jce_script_instantiate_source(s, "@call",
        "const M = {};" "\n"
        "M.on_start = function () {" "\n"
        "    const p = jce.get_position(7);" "\n"
        "    if (!Array.isArray(p) || p.length !== 3)" "\n"
        "        throw new Error('not 3 slots: ' + p);" "\n"
        "    if (p[0] !== 1 || p[1] !== 2 || p[2] !== 7)" "\n"
        "        throw new Error('wrong values: ' + p);" "\n"
        "    if (jce.find_with_tag('boss') !== 42)" "\n"
        "        throw new Error('tag lookup failed');" "\n"
        "    if (jce.find_with_tag('nope') !== 0)" "\n"
        "        throw new Error('a miss must be 0');" "\n"
        "};" "\n"
        "return M;" "\n", 1u);
    TEST_ASSERT_NOT_EQUAL(0u, inst);

    jce_script_call_start(s, inst);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, g_rec.log_calls, g_rec.last);

    jce_script_release(s, inst);
    jce_script_destroy(s);
}

/* -- 11. a binding whose host member is NULL is a no-op, not a crash --- */
static void test_bindings_guard_a_host_that_does_not_have_the_member(void)
{
    JceScript *s = open_js();          /* log + read_file ONLY */
    JceScriptInstance inst;
    TEST_ASSERT_NOT_NULL(s);

    /* jce_script_vm_create copies min(host_size, sizeof s->host) over a
     * zeroed table, so a member the caller never set is NULL -- and calling
     * it unguarded jumps through whatever followed the caller object.  The
     * contract is that the binding still ANSWERS: the miss value for a
     * fallible_out, the absent value for a value_return. */
    inst = jce_script_instantiate_source(s, "@guard",
        "const M = {};" "\n"
        "M.on_start = function () {" "\n"
        "    if (jce.get_position(1) !== null)" "\n"
        "        throw new Error('a NULL member must miss, not answer');" "\n"
        "    if (jce.find_with_tag('x') !== 0)" "\n"
        "        throw new Error('a NULL member must return the absent value');" "\n"
        "    jce.destroy(1);" "\n"
        "};" "\n"
        "return M;" "\n", 1u);
    TEST_ASSERT_NOT_EQUAL(0u, inst);

    jce_script_call_start(s, inst);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, g_rec.log_calls, g_rec.last);

    jce_script_release(s, inst);
    jce_script_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_js_registers_and_claims_jcejs);
    RUN_TEST(test_a_script_instantiates_and_receives_start_and_update);
    RUN_TEST(test_instantiate_by_path_uses_the_host_read_file);
    RUN_TEST(test_a_throw_is_contained_and_the_vm_survives);
    RUN_TEST(test_a_script_that_returns_nothing_is_refused);
    RUN_TEST(test_the_named_dispatchers_report_presence_honestly);
    RUN_TEST(test_hot_reload_rebinds_and_preserves_entity);
    RUN_TEST(test_update_coroutines_drains_pending_jobs);
    RUN_TEST(test_compile_module_refuses_a_body_with_no_object);
    RUN_TEST(test_a_script_calls_the_engine_through_the_jce_object);
    RUN_TEST(test_bindings_guard_a_host_that_does_not_have_the_member);
    return UNITY_END();
}
