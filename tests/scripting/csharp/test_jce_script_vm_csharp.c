/*
 * test_jce_script_vm_csharp.c — the "csharp" JceScriptVM, end to end.
 *
 * EVERY TEST GOES THROUGH jce_script_vm_create(), not through the backend's
 * own functions: the point of a JceScriptVM is that the engine reaches it the
 * same way it reaches lua, and a test that called the backend directly would
 * pass with the registry wired wrong.
 *
 * The observables are the HOST, not managed state.  Nothing native can read a
 * C# static, so the diagnostic scripts in JceScript.dll drive the engine
 * through the generated `Jce` surface and this file's host records what
 * arrived.  That makes one assertion cover the whole chain: hostfxr started
 * the runtime, the bridge bound, the type resolved, the callback dispatched,
 * and the P/Invoke surface reached the C ABI and came back.
 */

#include <jce/middleware/script/jce_script.h>
#include <jce/middleware/script/jce_script_vm.h>
#include <jce/script_vm/jce_script_vm_csharp.h>
#include <jce/os/core/jce_alloc.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

#define SELF_TEST "JceScript.Diagnostics.SelfTestScript"
#define THROWER   "JceScript.Diagnostics.ThrowingScript"
#define NOT_A     "JceScript.Diagnostics.NotAScript"

typedef struct Rec {
    int   log_calls;
    char  last[512];

    int   set_pos_calls;
    unsigned last_pos_entity;
    float last_pos[3];

    int   get_pos_calls;

    int   time_scale_calls;
    float last_time_scale;

    int   ui_text_calls;
    char  last_ui_text[128];
} Rec;

static Rec g_rec;

static void rec_log(void *user, const char *msg)
{
    Rec *r = (Rec *)user;
    if (!r) return;
    ++r->log_calls;
    snprintf(r->last, sizeof r->last, "%s", msg ? msg : "");
}

/* Answers a KNOWN triple, so the script's arithmetic on it is checkable: the
 * test asserts on (10+1, 20+2, 30+3) and a surface that silently returned
 * zeroes could not produce that. */
static bool rec_get_position(void *user, JceScriptEntity e, float out[3])
{
    Rec *r = (Rec *)user;
    (void)e;
    if (r) ++r->get_pos_calls;
    out[0] = 10.0f; out[1] = 20.0f; out[2] = 30.0f;
    return true;
}

static void rec_set_position(void *user, JceScriptEntity e, float x, float y,
                             float z)
{
    Rec *r = (Rec *)user;
    if (!r) return;
    ++r->set_pos_calls;
    r->last_pos_entity = (unsigned)e;
    r->last_pos[0] = x; r->last_pos[1] = y; r->last_pos[2] = z;
}

static void rec_set_time_scale(void *user, float s)
{
    Rec *r = (Rec *)user;
    if (!r) return;
    ++r->time_scale_calls;
    r->last_time_scale = s;
}

static void rec_ui_set_text(void *user, JceScriptEntity e, const char *t)
{
    Rec *r = (Rec *)user;
    (void)e;
    if (!r) return;
    ++r->ui_text_calls;
    snprintf(r->last_ui_text, sizeof r->last_ui_text, "%s", t ? t : "");
}

static JceScript *open_cs(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    memset(&g_rec, 0, sizeof g_rec);
    h.user           = &g_rec;
    h.log            = rec_log;
    h.get_position   = rec_get_position;
    h.set_position   = rec_set_position;
    h.set_time_scale = rec_set_time_scale;
    h.ui_set_text    = rec_ui_set_text;
    return jce_script_vm_create(JCE_SCRIPT_VM_CSHARP_LANGUAGE, &h, sizeof h);
}

void setUp(void) {}
void tearDown(void) {}

/* ── 1. the language and its extension are claimed ───────────────────── */
static void test_csharp_registers_and_claims_cs(void)
{
    TEST_ASSERT_TRUE_MESSAGE(jce_script_vm_csharp_register(),
        "register failed: no .NET runtime, or JceScript.dll is not beside the "
        "test executable (the test's CMakeLists stages it)");
    TEST_ASSERT_TRUE_MESSAGE(jce_script_vm_csharp_register(),
        "register must be idempotent, like every other backend's");

    TEST_ASSERT_EQUAL_STRING_MESSAGE(
        JCE_SCRIPT_VM_CSHARP_LANGUAGE,
        jce_script_vm_language_for_path("Assets/Turret."
                                        JCE_SCRIPT_VM_CSHARP_EXTENSION),
        "the extension claim is what routes a scene's Script component to "
        "this VM; without it a .cs is a file nothing will run");
}

/* ── 2. a script instantiates, runs, and reaches the engine ──────────── */
static void test_a_script_runs_and_calls_the_engine(void)
{
    JceScript *s = open_cs();
    JceScriptInstance inst;
    TEST_ASSERT_NOT_NULL(s);

    inst = jce_script_instantiate(s, SELF_TEST, 7u);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, inst,
        "instantiate refused a type that ships inside JceScript.dll");

    jce_script_call_start(s, inst);

    /* ONE assertion for the whole chain: OnStart read the position through
     * the generated TryGetPosition, added (1,2,3), and wrote it back through
     * SetPosition.  A surface that failed to open would have written
     * (-1,-1,-1); one that returned zeroes would have written (1,2,3). */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_rec.get_pos_calls,
        "the script never read the engine: the P/Invoke surface did not open");
    TEST_ASSERT_EQUAL_INT(1, g_rec.set_pos_calls);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(7u, g_rec.last_pos_entity,
        "the instance did not carry its owner entity");
    TEST_ASSERT_EQUAL_FLOAT(11.0f, g_rec.last_pos[0]);
    TEST_ASSERT_EQUAL_FLOAT(22.0f, g_rec.last_pos[1]);
    TEST_ASSERT_EQUAL_FLOAT(33.0f, g_rec.last_pos[2]);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, g_rec.log_calls, g_rec.last);

    jce_script_call_update(s, inst, 0.25f);
    TEST_ASSERT_EQUAL_INT(1, g_rec.time_scale_calls);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, g_rec.last_time_scale);

    jce_script_call_message(s, inst, "hello", 1.0, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_rec.ui_text_calls,
        "on_message did not run, or the string never crossed back");
    TEST_ASSERT_EQUAL_STRING("hello", g_rec.last_ui_text);

    TEST_ASSERT_EQUAL_INT(1, jce_script_instance_count(s));
    jce_script_release(s, inst);
    TEST_ASSERT_EQUAL_INT(0, jce_script_instance_count(s));
    jce_script_destroy(s);
}

/* ── 3. a throw is contained, reported, and does not kill the process ── */
static void test_a_throw_is_contained_and_reported(void)
{
    JceScript *s = open_cs();
    JceScriptInstance bad, good;
    TEST_ASSERT_NOT_NULL(s);

    bad = jce_script_instantiate(s, THROWER, 1u);
    TEST_ASSERT_NOT_EQUAL(0u, bad);

    /* THE POINT: an exception escaping an [UnmanagedCallersOnly] frame does
     * not unwind into C — the runtime fails fast and kills the process.  So
     * "this test still runs" is half the assertion; the other half is that
     * the throw was REPORTED rather than swallowed. */
    jce_script_call_start(s, bad);
    TEST_ASSERT_TRUE_MESSAGE(g_rec.log_calls > 0,
        "the throw reached nobody: OnStart stopped running and the editor "
        "console says nothing about why");

    /* And the VM is still usable next to it. */
    g_rec.log_calls = 0;
    good = jce_script_instantiate(s, SELF_TEST, 5u);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, good,
        "the VM was left unusable by a throwing script");
    jce_script_call_start(s, good);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, g_rec.log_calls, g_rec.last);
    TEST_ASSERT_EQUAL_UINT(5u, g_rec.last_pos_entity);

    jce_script_release(s, bad);
    jce_script_release(s, good);
    jce_script_destroy(s);
}

/* ── 4. what is refused, is refused with a reason ────────────────────── */
static void test_refusals_are_reported_not_silent(void)
{
    JceScript *s = open_cs();
    TEST_ASSERT_NOT_NULL(s);

    /* A compiled language has no source to run; answering anything but 0
     * would mean silently ignoring the source the caller passed. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u,
        jce_script_instantiate_source(s, "@x", "class X {}", 1u),
        "instantiate_source must refuse: a C# script is compiled by "
        "`dotnet build` before the process starts");

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u,
        jce_script_instantiate(s, "NoSuchTypeAnywhere", 1u),
        "a type that does not exist must not produce an instance");
    TEST_ASSERT_TRUE_MESSAGE(g_rec.log_calls > 0,
        "an unresolvable type was refused SILENTLY: a designer sees a script "
        "that does nothing and no reason anywhere");

    g_rec.log_calls = 0;
    memset(g_rec.last, 0, sizeof g_rec.last);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, jce_script_instantiate(s, NOT_A, 1u),
        "a type with no lifecycle must be refused, not instantiated");
    TEST_ASSERT_TRUE_MESSAGE(g_rec.log_calls > 0, "refused silently");
    /* WHAT was logged, not merely that something was.  Without the base-class
     * check the cast throws InvalidCastException, the barrier reports THAT,
     * and every assertion above still passes -- so "a line was logged" cannot
     * tell a reasoned refusal from a crash caught on the way out. */
    /* "no lifecycle", not "JceEntityScript": the InvalidCastException thrown
     * when the base-class check is REMOVED also names JceEntityScript, so
     * that substring cannot tell the two apart -- measured, the mutation
     * survived it.  This phrase exists only in the reasoned refusal. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(g_rec.last, "no lifecycle"),
        "the refusal did not explain itself; it is a caught exception "
        "wearing a refusal's clothes");

    jce_script_destroy(s);
}

/* ── 5. the three named dispatchers ──────────────────────────────────── */
static void test_named_dispatchers_report_presence_honestly(void)
{
    JceScript *s = open_cs();
    TEST_ASSERT_NOT_NULL(s);

    /* Every PUBLIC STATIC method of a script type is a named handler, which
     * is what the Java backend does and what Lua's global table amounts to.
     * The answer is "a handler of that name existed", NOT "the call
     * succeeded" — a handler that throws still counts as invoked. */
    TEST_ASSERT_TRUE_MESSAGE(jce_script_call_named(s, "RecordEntity", 3u),
        "a public static method of a script type must be a named handler");
    TEST_ASSERT_TRUE(jce_script_call_named_num(s, "RecordNumber", 3u, 1.5));
    TEST_ASSERT_TRUE(jce_script_call_named_str(s, "RecordText", 3u, "x"));

    TEST_ASSERT_FALSE_MESSAGE(
        jce_script_call_named(s, "NoHandlerOfThisName", 1u),
        "an absent handler must answer false; these three fail SILENTLY by "
        "contract, so false is the only signal a caller ever gets");

    jce_script_destroy(s);
}

/* ── 6. reload rebinds and preserves the entity ──────────────────────── */
static void test_rebind_preserves_the_entity(void)
{
    JceScript *s = open_cs();
    JceScriptInstance inst;
    JceScriptModule   mod;
    TEST_ASSERT_NOT_NULL(s);

    inst = jce_script_instantiate(s, SELF_TEST, 42u);
    TEST_ASSERT_NOT_EQUAL(0u, inst);

    /* A type RE-LOOKUP, not a recompile: `source` is ignored on purpose, so
     * this is the shape an editor uses after reloading an assembly. */
    mod = jce_script_compile_module(s, SELF_TEST, NULL, 0u);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, mod,
        "compile_module refused a type it had just instantiated");

    jce_script_rebind_instance(s, inst, mod);
    jce_script_call_start(s, inst);

    /* The rebound object is a NEW instance, so its fields are gone — but the
     * entity carries over, and that is the whole contract of a rebind. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(42u, g_rec.last_pos_entity,
        "the rebound instance lost its entity");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, g_rec.log_calls, g_rec.last);

    jce_script_release_module(s, mod);
    jce_script_release(s, inst);
    jce_script_destroy(s);
}

/* ── 7. one runtime for the whole process ────────────────────────────── */
static void test_the_dotnet_runtime_starts_once(void)
{
    JceScript *a = open_cs();
    JceScript *b = open_cs();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);

    /* hostfxr initialises once per process; a backend that started a runtime
     * per VM would pay ~50 ms and tens of MB for every scene load, and two
     * runtimes could not share a type. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, jce_script_vm_csharp_runtime_starts(),
        "the .NET runtime was started more than once");

    jce_script_destroy(a);
    jce_script_destroy(b);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_csharp_registers_and_claims_cs);
    RUN_TEST(test_a_script_runs_and_calls_the_engine);
    RUN_TEST(test_a_throw_is_contained_and_reported);
    RUN_TEST(test_refusals_are_reported_not_silent);
    RUN_TEST(test_named_dispatchers_report_presence_honestly);
    RUN_TEST(test_rebind_preserves_the_entity);
    RUN_TEST(test_the_dotnet_runtime_starts_once);
    return UNITY_END();
}
