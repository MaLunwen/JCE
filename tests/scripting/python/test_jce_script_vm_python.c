/*
 * test_jce_script_vm_python.c — the Python VM's own gate.
 *
 * The lifecycle differential is the real oracle: it compares this backend
 * against Lua over every vtable slot.  This file covers what a
 * SIDE-BY-SIDE COMPARISON IS STRUCTURALLY BLIND TO — properties that are
 * either identical on both sides while both are wrong, or invisible in a
 * recorded stream because their failure mode is a crash rather than a line.
 *
 *   1. THE SHORT-HOST CLAMP.  The plugin reads a caller-allocated
 *      JceScriptHost, and copying it at OUR sizeof reads past a host built
 *      against an older header and files whatever followed under a callback we
 *      then call.  Both languages would over-read identically, so a
 *      differential cannot see it.  Modelled on
 *      tests/middleware/script/test_jce_script_host_abi.c, whose comment says
 *      plainly that it only covers Lua.
 *
 *   2. THE GIL.  A slot that forgot PyGILState_Ensure/Release is a crash or a
 *      hang, and a crash reports less than a red test — four instances of that
 *      in this worktree in one day.  The shim converts it into counters; this
 *      file is what reads them.  Note that the assertions are on the DELTA
 *      across a known number of dispatches and not on the absolute values:
 *      creating the VM dispatches too, and a test that pinned totals would
 *      fail every time a slot was added.
 *
 *   3. INTERPRETER LIFETIME.  "Initialise once, never finalise" fails as a
 *      crash at shutdown or on the SECOND VM — after every lifecycle
 *      assertion has already passed.  Two handles at once, and a second handle
 *      after the first was destroyed, are the two shapes that catch it.
 *
 *   4. THE ONE DOCUMENTED DIVERGENCE from Lua (a named global does not
 *      outlive its instance).  A differential must not be where a known
 *      divergence is discovered, so it is asserted HERE, as the expected
 *      result, where changing it is a deliberate act.
 *
 *   5. instantiate() THROUGH A REAL read_file.  The differential's mock host
 *      has no read_file member at all, so it can only compare the REFUSAL.
 *      The success path needs a host that has one, which is this file.
 */

#include "unity.h"

#include <jce/middleware/script/jce_script.h>
#include <jce/middleware/script/jce_script_vm.h>

#include "jce_script_py_mock.h"
#include "jce_script_vm_python.h"

#include <jce/os/core/jce_alloc.h>

#include <stddef.h>
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

/* ── A host of our own, for the cases the shared mock cannot express ──── */

static char        g_log[64][512];
static int         g_log_count;
static const char *g_file_body;      /* what read_file answers with */
static int         g_read_file_calls;
static int         g_set_position_calls;

static void t_log(void *user, const char *msg)
{
    (void)user;
    if (g_log_count < 64) {
        snprintf(g_log[g_log_count], sizeof g_log[0], "%s", msg ? msg : "");
        ++g_log_count;
    }
}

static void *t_read_file(void *user, const char *path, uint64_t *out_size)
{
    size_t n;
    char  *buf;
    (void)user; (void)path;
    ++g_read_file_calls;
    if (!g_file_body) { *out_size = 0u; return NULL; }
    n = strlen(g_file_body);
    /* jce_script_instantiate frees this with jce_free, so it must come from
     * the engine allocator — the same contract script_lua_instantiate relies
     * on.  malloc here would be a heap mismatch on Windows. */
    buf = (char *)jce_malloc(n + 1u);
    if (!buf) { *out_size = 0u; return NULL; }
    memcpy(buf, g_file_body, n + 1u);
    *out_size = (uint64_t)n;
    return buf;
}

static void t_set_position(void *user, JceScriptEntity e, float x, float y,
                           float z)
{
    (void)user; (void)e; (void)x; (void)y; (void)z;
    ++g_set_position_calls;
}

static void host_reset(JceScriptHost *h)
{
    memset(h, 0, sizeof *h);
    h->log = t_log;
    h->read_file = t_read_file;
    h->set_position = t_set_position;
    g_log_count = 0;
    g_read_file_calls = 0;
    g_set_position_calls = 0;
    g_file_body = NULL;
}

static int log_contains(const char *needle)
{
    int i;
    for (i = 0; i < g_log_count; ++i)
        if (strstr(g_log[i], needle)) return 1;
    return 0;
}

/* ── One-time setup ───────────────────────────────────────────────────── */

static int g_ready;

static void ensure_registered(void)
{
    if (g_ready) return;
    g_ready = 1;
    /* The binding finds jce_script_api through $JCE_SCRIPT_API; CMake knows
     * where it is and this test does not have to guess. */
    jce_setenv("JCE_SCRIPT_API", JCE_PY_TEST_API);
    TEST_ASSERT_TRUE_MESSAGE(
        jce_script_vm_python_add_path(JCE_PY_TEST_PACKAGE_DIR),
        "jce_script_vm_python_add_path refused the package directory");
    TEST_ASSERT_TRUE_MESSAGE(
        jce_script_vm_python_register(),
        "the python VM did not register — every slot must be non-NULL and "
        "the name must be free");
}

void setUp(void)    { ensure_registered(); }
void tearDown(void) { }

/* A script the tests below reuse.  Deliberately trivial: what is under test
 * here is the shim, not the semantics. */
static const char *k_script =
    "def on_start(self):\n"
    "    jce.log('started e=%d' % self.entity)\n"
    "    jce.set_position(self.entity, 1.0, 2.0, 3.0)\n"
    "\n"
    "def on_update(self, dt):\n"
    "    jce.log('update dt=%.3f' % dt)\n"
    "    if dt > 10.0:\n"
    "        raise RuntimeError('boom')\n"
    "\n"
    "def on_named(e):\n"
    "    jce.log('named e=%d' % e)\n";

/* on_start raises from its SECOND dispatch onward.  The one case that can see
 * call_start passing the non-participating slot: every other case dispatches
 * on_start once, where disabling it and not disabling it look identical. */
static const char *k_start_raiser =
    "N = 0\n"
    "def on_start(self):\n"
    "    global N\n"
    "    N += 1\n"
    "    jce.log('start n=%d' % N)\n"
    "    if N >= 2:\n"
    "        raise RuntimeError('start boom')\n";

/* The hot-reload target for the FAILING-CALLBACK-RULE case: a DIFFERENT log
 * line, so a re-enabled handler is visibly working and not merely re-armed. */
static const char *k_reload =
    "def on_update(self, dt):\n"
    "    jce.log('v2 dt=%.3f' % dt)\n";

/* ── 1. the short-host clamp ────────────────────────────────── */

static int g_withheld_calls;

static void t_withheld(void *user, JceScriptEntity e, float x, float y,
                       float z)
{
    (void)user; (void)e; (void)x; (void)y; (void)z;
    ++g_withheld_calls;
}

static void test_a_short_host_is_not_read_past_its_end(void)
{
    /* A host whose DECLARED size stops before set_position, but whose storage
     * continues with a perfectly valid function pointer after it.
     *
     * WHY A VALID POINTER AND NOT 0xFF.  The model this is built on
     * (tests/middleware/script/test_jce_script_host_abi.c) fills the tail with
     * 0xFF and says plainly that a segfault in that file means the clamp was
     * removed.  That is a real signal and it is the WRONG KIND: a crash prints
     * nothing, including the named failures the run had already produced —
     * measured four times in this worktree in one day.  A live pointer tests
     * exactly the same rule (a member past `host_size` must be treated as
     * ABSENT, not called) and turns its violation into a counter.
     *
     * It is also the more realistic shape.  0xFF is what an uninitialised
     * caller looks like; a valid pointer is what a caller with a LONGER real
     * struct looks like when it honestly declares the size it was built
     * against.  Copying at our own sizeof calls a member that caller never
     * promised. */
    unsigned char  storage[sizeof(JceScriptHost) * 2];
    JceScriptHost *probe = (JceScriptHost *)storage;
    const size_t   cut = offsetof(JceScriptHost, set_position);
    JceScript     *s;
    JceScriptInstance inst;

    memset(storage, 0, sizeof storage);
    probe->log = t_log;
    probe->set_position = t_withheld;      /* PAST the declared size */
    g_log_count = 0;
    g_withheld_calls = 0;

    s = jce_script_vm_create("python", probe, cut);
    TEST_ASSERT_NOT_NULL_MESSAGE(s,
        "a short host was refused outright; the clamp must ACCEPT it and "
        "treat the withheld members as absent");

    inst = jce_script_instantiate_source(s, "clamped", k_script, 5u);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, inst, "the short-host VM ran no script");
    jce_script_call_start(s, inst);

    /* The script ran and DID call jce.set_position — without this the test
     * would pass on a VM that dispatched nothing at all, which is the exact
     * "nothing compares equal to nothing" shape the differential also guards
     * against. */
    TEST_ASSERT_TRUE_MESSAGE(log_contains("started e=5"),
        "the short-host VM's on_start never ran, so the assertion below "
        "proves nothing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_withheld_calls,
        "set_position was called through a host that DECLARED it did not "
        "have one. py_create_sized must copy min(host_size, sizeof s->host) "
        "over a zeroed destination — copying at our own sizeof reads past a "
        "caller built against an older header and files whatever followed "
        "under a callback the binding then invokes");

    jce_script_destroy(s);
}

/* ── 2. the GIL ───────────────────────────────────────────────────────── */

static void test_every_slot_balances_the_gil(void)
{
    JceScriptVmPythonGilStats before, after;
    JceScriptHost h;
    JceScript *s;
    JceScriptInstance inst;

    host_reset(&h);
    jce_script_vm_python_gil_stats(&before);

    s = jce_script_vm_create("python", &h, sizeof h);
    TEST_ASSERT_NOT_NULL(s);
    inst = jce_script_instantiate_source(s, "gil", k_script, 6u);
    TEST_ASSERT_NOT_EQUAL(0u, inst);
    jce_script_call_start(s, inst);
    jce_script_call_update(s, inst, 0.5f);
    jce_script_call_collision(s, inst, 9u);
    (void)jce_script_call_named(s, "on_named", 9u);
    (void)jce_script_instance_count(s);
    jce_script_update_coroutines(s, 0.5f);
    jce_script_release(s, inst);
    jce_script_destroy(s);

    jce_script_vm_python_gil_stats(&after);

    TEST_ASSERT_TRUE_MESSAGE(after.enters > before.enters,
        "no slot acquired the GIL at all — the counters cannot prove balance "
        "on a run that never entered Python");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        after.enters - before.enters, after.leaves - before.leaves,
        "PyGILState_Ensure and PyGILState_Release are not balanced across "
        "this run: a slot acquired the GIL and did not give it back, which "
        "in a process with any other Python thread is a hang rather than a "
        "wrong answer");
}

static void test_no_slot_body_ran_without_the_gil(void)
{
    JceScriptVmPythonGilStats st;
    jce_script_vm_python_gil_stats(&st);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u, st.body_unheld,
        "py_call refused at least one dispatch because the GIL was not held. "
        "That refusal only happens when a slot reached Python without going "
        "through PyGILState_Ensure — the counter exists so that failure is a "
        "named red instead of an access violation inside CPython");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u, st.still_held,
        "PyGILState_Release left the GIL at a different level than the "
        "matching Ensure found it");
}

/* ── 3. interpreter lifetime ──────────────────────────────────────────── */

static void test_a_second_vm_after_the_first_was_destroyed_still_runs(void)
{
    /* THE SHUTDOWN CRASH NO LIFECYCLE TEST WOULD NOTICE.  A backend that
     * called Py_FinalizeEx when its last handle went away would pass every
     * assertion in the differential and die here — or, worse, only in a
     * shipped game on the second scene load. */
    JceScriptHost h;
    JceScript *a, *b;
    JceScriptInstance ia, ib;
    int live_before = jce_script_vm_python_live_handles();

    host_reset(&h);
    a = jce_script_vm_create("python", &h, sizeof h);
    TEST_ASSERT_NOT_NULL(a);
    ia = jce_script_instantiate_source(a, "one", k_script, 1u);
    TEST_ASSERT_NOT_EQUAL(0u, ia);
    jce_script_call_start(a, ia);
    jce_script_destroy(a);

    TEST_ASSERT_EQUAL_INT_MESSAGE(live_before,
        jce_script_vm_python_live_handles(),
        "destroy did not release the handle: per-handle state must go even "
        "though the interpreter deliberately does not");

    host_reset(&h);
    b = jce_script_vm_create("python", &h, sizeof h);
    TEST_ASSERT_NOT_NULL_MESSAGE(b,
        "the SECOND VM could not be created after the first was destroyed — "
        "the interpreter was finalised with the last handle");
    ib = jce_script_instantiate_source(b, "two", k_script, 2u);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, ib,
        "the second VM created but could not run a script");
    jce_script_call_start(b, ib);
    TEST_ASSERT_TRUE_MESSAGE(log_contains("started e=2"),
        "the second VM's on_start did not run");
    jce_script_destroy(b);
}

static void test_two_live_vms_do_not_share_script_state(void)
{
    /* Lua gives each handle its own lua_State.  This backend has ONE
     * interpreter and gives each handle its own ScriptVM with its own module
     * namespaces; that is the property, and without it two entities running
     * the same file would trample each other's globals. */
    static const char *counter =
        "count = 0\n"
        "def on_update(self, dt):\n"
        "    global count\n"
        "    count += 1\n"
        "    jce.log('n=%d' % count)\n";
    JceScriptHost h;
    JceScript *a, *b;
    JceScriptInstance ia, ib;

    host_reset(&h);
    a = jce_script_vm_create("python", &h, sizeof h);
    b = jce_script_vm_create("python", &h, sizeof h);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL_MESSAGE(b, "a second VM could not be created while "
                                    "the first was still alive");

    ia = jce_script_instantiate_source(a, "counter", counter, 1u);
    ib = jce_script_instantiate_source(b, "counter", counter, 2u);
    TEST_ASSERT_NOT_EQUAL(0u, ia);
    TEST_ASSERT_NOT_EQUAL(0u, ib);

    jce_script_call_update(a, ia, 0.1f);
    jce_script_call_update(a, ia, 0.1f);
    jce_script_call_update(b, ib, 0.1f);

    TEST_ASSERT_TRUE_MESSAGE(log_contains("n=2"),
        "the first VM's counter did not reach 2");
    /* If the two handles shared a namespace the third update would print n=3;
     * the whole point is that it prints n=1. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, g_log_count,
        "an unexpected number of log lines");
    TEST_ASSERT_TRUE_MESSAGE(strstr(g_log[2], "n=1") != NULL,
        "the second VM saw the first VM's module-level state: the handles "
        "share a namespace");

    jce_script_destroy(a);
    jce_script_destroy(b);
}

/* ── 4. exceptions ────────────────────────────────────────────────────── */

static void test_a_raising_handler_is_logged_then_disabled_and_rebind_revives_it(void)
{
    /* THE FAILING-CALLBACK RULE (jce_script.h), which the reference Lua VM
     * implements in call_method(): the exception is caught, "<method> error:
     * <detail>" goes to host.log AND the engine log, the published notice
     * follows it, and THAT HANDLER STOPS DISPATCHING on that instance until a
     * rebind.  Four assertions, because each is passable alone by a different
     * wrong implementation: an exception that is merely swallowed passes "the
     * next call is silent"; one that propagates passes "it was logged" right
     * up until the process dies; a VM that dropped the whole instance passes
     * both of those; and a VM that never re-enables passes everything except
     * the rebind. */
    JceScriptHost h;
    JceScript *s;
    JceScriptInstance inst;
    JceScriptModule mod;
    char notice[512];

    snprintf(notice, sizeof notice, JCE_SCRIPT_DISABLED_NOTICE_FMT,
             "on_update");

    host_reset(&h);
    s = jce_script_vm_create("python", &h, sizeof h);
    TEST_ASSERT_NOT_NULL(s);
    inst = jce_script_instantiate_source(s, "raiser", k_script, 3u);
    TEST_ASSERT_NOT_EQUAL(0u, inst);

    jce_script_call_update(s, inst, 99.0f);          /* raises */
    TEST_ASSERT_TRUE_MESSAGE(log_contains("on_update error:"),
        "a raising handler produced no host.log line. Lua writes "
        "\"<method> error: <detail>\" through host.log for exactly this case; "
        "an exception that is merely swallowed is invisible to a game");
    TEST_ASSERT_TRUE_MESSAGE(log_contains("RuntimeError"),
        "the logged line does not name the exception type");
    TEST_ASSERT_TRUE_MESSAGE(log_contains(notice),
        "the disable was not announced in the wording jce_script.h publishes: "
        "a handler that goes quiet with no notice is indistinguishable from "
        "one the script never declared");

    g_log_count = 0;
    jce_script_call_update(s, inst, 0.5f);           /* must be silent now */
    TEST_ASSERT_FALSE_MESSAGE(log_contains("update dt=0.500"),
        "on_update dispatched again after it raised: the disable did not take, "
        "and a script erroring every frame writes one log line per frame");

    /* A DIFFERENT handler on the SAME instance is untouched — the disable is
     * per callback, not per instance, and an implementation that dropped the
     * instance fails exactly here. */
    jce_script_call_start(s, inst);
    TEST_ASSERT_TRUE_MESSAGE(log_contains("started e=3"),
        "on_start stopped running on an instance whose on_update was "
        "disabled: the disable took the instance instead of the callback");

    /* And a hot reload brings it back, which is what makes "fix the script and
     * save" a working repair rather than a restart. */
    mod = jce_script_compile_module(s, "raiser_v2", k_reload,
                                    strlen(k_reload));
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, mod, "the reload module did not compile");
    jce_script_rebind_instance(s, inst, mod);
    jce_script_release_module(s, mod);

    g_log_count = 0;
    jce_script_call_update(s, inst, 0.25f);
    TEST_ASSERT_TRUE_MESSAGE(log_contains("v2 dt=0.250"),
        "a rebind did not re-enable the disabled handler: the script you fixed "
        "stays dead until the process restarts");

    jce_script_destroy(s);
}

/* ── 5. instantiate through a real read_file ──────────────────────────── */

static void test_instantiate_reads_the_script_through_the_host(void)
{
    JceScriptHost h;
    JceScript *s;
    JceScriptInstance inst;

    host_reset(&h);
    g_file_body = k_script;
    s = jce_script_vm_create("python", &h, sizeof h);
    TEST_ASSERT_NOT_NULL(s);

    inst = jce_script_instantiate(s, "scripts/bob.py", 77u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_read_file_calls,
        "instantiate did not go through the host's read_file — the Lua "
        "implementation does, and a backend that opened the file itself would "
        "bypass the engine's virtual filesystem and its PAK mounts");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, inst,
        "instantiate returned no instance for a readable script");

    jce_script_call_start(s, inst);
    TEST_ASSERT_TRUE_MESSAGE(log_contains("started e=77"),
        "the instance's owner entity did not reach the script");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_set_position_calls,
        "the script's jce.set_position did not reach the host");

    jce_script_destroy(s);
}

static void test_instantiate_without_read_file_refuses(void)
{
    JceScriptHost h;
    JceScript *s;

    host_reset(&h);
    h.read_file = NULL;
    s = jce_script_vm_create("python", &h, sizeof h);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(
        0u, jce_script_instantiate(s, "scripts/bob.py", 1u),
        "instantiate invented a script for a host with no read_file");
    jce_script_destroy(s);
}

/* ── 6. the documented divergence ─────────────────────────────────────── */

static void test_a_named_handler_does_not_outlive_its_instance(void)
{
    /* THE ONE PLACE THIS BACKEND DIFFERS FROM LUA, asserted as the expected
     * result so that changing it is deliberate.  Lua's call_named reads _G,
     * which every chunk shares and nothing ever clears, so a handler survives
     * its instance.  Here each instance owns its namespace and a released
     * instance takes its handlers with it.
     *
     * The engine-visible contract is unaffected: a UISlider handler lives in a
     * script attached to a live entity, which is the case asserted first. */
    JceScriptHost h;
    JceScript *s;
    JceScriptInstance inst;

    host_reset(&h);
    s = jce_script_vm_create("python", &h, sizeof h);
    TEST_ASSERT_NOT_NULL(s);
    inst = jce_script_instantiate_source(s, "named", k_script, 8u);
    TEST_ASSERT_NOT_EQUAL(0u, inst);

    TEST_ASSERT_TRUE_MESSAGE(jce_script_call_named(s, "on_named", 8u),
        "a module-level handler was not found while its instance was live — "
        "this is the case every UISlider / UIToggle / UIDropdown handler "
        "depends on");
    TEST_ASSERT_TRUE(log_contains("named e=8"));

    jce_script_release(s, inst);
    TEST_ASSERT_FALSE_MESSAGE(jce_script_call_named(s, "on_named", 8u),
        "a named handler outlived the release of its last instance. That is "
        "Lua's behaviour and NOT this backend's: if it has been changed on "
        "purpose, change this assertion and the note in vm.py::_named "
        "together");

    jce_script_destroy(s);
}

static void test_a_missing_global_is_false_not_an_error(void)
{
    /* The two slots whose absence is invisible return false for "no such
     * global", which a correctly absent handler also returns.  A backend that
     * raised, or that returned true, would break the UI layer in opposite
     * directions; both are checked because they are different defects. */
    JceScriptHost h;
    JceScript *s;
    JceScriptInstance inst;

    host_reset(&h);
    s = jce_script_vm_create("python", &h, sizeof h);
    TEST_ASSERT_NOT_NULL(s);
    inst = jce_script_instantiate_source(s, "named", k_script, 9u);
    TEST_ASSERT_NOT_EQUAL(0u, inst);

    TEST_ASSERT_FALSE(jce_script_call_named(s, "nope", 9u));
    TEST_ASSERT_FALSE(jce_script_call_named_num(s, "nope", 9u, 1.0));
    TEST_ASSERT_FALSE(jce_script_call_named_str(s, "nope", 9u, "x"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_log_count,
        "a missing global produced a log line; the engine calls these every "
        "frame for handlers that are legitimately absent");

    jce_script_destroy(s);
}

/* ── 7. the seam itself ───────────────────────────────────────────────── */

static void test_the_language_is_python_and_a_second_registration_is_refused(
    void)
{
    JceScriptHost h;
    JceScript *s;
    const char *lang;

    TEST_ASSERT_NOT_NULL_MESSAGE(jce_script_vm_find("python"),
        "the 'python' language is not in the registry");
    TEST_ASSERT_FALSE_MESSAGE(jce_script_vm_python_register(),
        "a second registration of 'python' was accepted — live handles hold a "
        "pointer into the registry, so a replacement would repoint them");

    host_reset(&h);
    s = jce_script_vm_create("python", &h, sizeof h);
    TEST_ASSERT_NOT_NULL(s);
    lang = jce_script_vm_language_of(s);
    TEST_ASSERT_NOT_NULL_MESSAGE(lang,
        "the handle does not begin with a JceScriptVMHeader");
    TEST_ASSERT_EQUAL_STRING("python", lang);
    jce_script_destroy(s);
}

/* Registration alone does not make a .py RUN: the runtime picks a language
 * per script from the PATH, through the extension claim this backend makes
 * for itself in jce_script_vm_python_register().  A backend that registers
 * and forgets to claim is reachable only by name, and every turret.py in
 * every scene silently does nothing. */
static void test_the_backend_claims_py(void)
{
    const char *lang =
        jce_script_vm_language_for_path("assets/scripts/turret.py");

    TEST_ASSERT_NOT_NULL_MESSAGE(lang,
        "no VM claims '.py' although the python backend registered — a "
        "turret.py in a scene would resolve to nothing and never run");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("python", lang,
        "'.py' resolves to a language other than python");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("python",
        jce_script_vm_language_for_path("A/B/Turret.PY"),
        "extension matching is not case-insensitive, so a scene authored "
        "with Turret.PY would silently run nothing");

    /* The literal "python" above is also the OFFLINE asset catalog's answer
     * for ".py" (engine/src/resource/jce_asset_ext.c, {"py","python",...}),
     * which is what a cooker with no backend linked uses to pack the file.
     * The two tables are deliberately separate — the catalog must not depend
     * on build options, the registry must not be a static list — so they can
     * drift.  They are compared STATICALLY, not here: this process links no
     * jce_resource, and a dynamic comparison would only ever cover the
     * languages the running build happens to have linked.
     * *Enforced by:* tools/audit/check_script_language_catalog.py, which
     * reads every jce_script_vm_register_extension() call under scripting/
     * and fails when one claims an extension the catalog does not know, or
     * claims it for a different language. */
}

static void test_a_syntax_error_is_reported_and_returns_no_instance(void)
{
    JceScriptHost h;
    JceScript *s;

    host_reset(&h);
    s = jce_script_vm_create("python", &h, sizeof h);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(
        0u, jce_script_instantiate_source(s, "bad", "def (:\n", 1u),
        "a script that does not compile produced an instance");
    /* Lua's compile-error path writes to the engine log ONLY, never to
     * host.log (script_lua_instantiate_source), and this backend copies that
     * routing.  Asserting the ABSENCE is what keeps the routing honest. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_log_count,
        "a compile error reached host.log; Lua's compile-error path uses "
        "LOG_ERROR only, and the split is observable to anything that "
        "records host callbacks");
    jce_script_destroy(s);
}

static void test_on_start_participates_in_the_rule(void)
{
    JceScriptHost h;
    JceScript *s;
    JceScriptInstance inst;
    char notice[512];
    int i;

    snprintf(notice, sizeof notice, JCE_SCRIPT_DISABLED_NOTICE_FMT, "on_start");

    host_reset(&h);
    s = jce_script_vm_create("python", &h, sizeof h);
    TEST_ASSERT_NOT_NULL(s);
    inst = jce_script_instantiate_source(s, "starter", k_start_raiser, 7u);
    TEST_ASSERT_NOT_EQUAL(0u, inst);

    for (i = 0; i < 5; ++i) jce_script_call_start(s, inst);

    TEST_ASSERT_TRUE_MESSAGE(log_contains("start n=2"),
        "on_start did not reach its second dispatch, so nothing here is being "
        "measured");
    TEST_ASSERT_FALSE_MESSAGE(log_contains("start n=3"),
        "on_start dispatched a third time after raising: call_start is passing "
        "the non-participating slot");
    TEST_ASSERT_TRUE_MESSAGE(log_contains(notice),
        "the on_start disable was not announced");

    jce_script_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_language_is_python_and_a_second_registration_is_refused);
    RUN_TEST(test_the_backend_claims_py);
    RUN_TEST(test_a_short_host_is_not_read_past_its_end);
    RUN_TEST(test_instantiate_reads_the_script_through_the_host);
    RUN_TEST(test_instantiate_without_read_file_refuses);
    RUN_TEST(test_a_raising_handler_is_logged_then_disabled_and_rebind_revives_it);
    RUN_TEST(test_on_start_participates_in_the_rule);
    RUN_TEST(test_a_syntax_error_is_reported_and_returns_no_instance);
    RUN_TEST(test_a_named_handler_does_not_outlive_its_instance);
    RUN_TEST(test_a_missing_global_is_false_not_an_error);
    RUN_TEST(test_two_live_vms_do_not_share_script_state);
    RUN_TEST(test_a_second_vm_after_the_first_was_destroyed_still_runs);
    RUN_TEST(test_every_slot_balances_the_gil);
    /* LAST, deliberately: it reads counters that every test above contributes
     * to, so running it first would assert about an empty run. */
    RUN_TEST(test_no_slot_body_ran_without_the_gil);
    return UNITY_END();
}
