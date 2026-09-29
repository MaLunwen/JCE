/* test_jce_script_coroutine.c
 *
 * Cooperative coroutine timers — jce.start_coroutine / jce.wait_seconds /
 * jce.stop_coroutine, driven by jce_script_update_coroutines().
 *
 * Exercises the REAL Lua-thread scheduler on a headless VM (a binding-less host
 * carrying only a log callback that accumulates every logged marker), with no
 * runtime / scene.  Asserts Unity-style semantics:
 *   - start_coroutine runs the body synchronously up to the first wait_seconds;
 *   - the body resumes only after the accumulated dt passes the wait;
 *   - chained waits step in order;
 *   - a coroutine with no wait completes immediately (nothing parked);
 *   - stop_coroutine cancels a parked coroutine before it resumes;
 *   - wait_seconds outside a coroutine is a caught error (no crash).
 */

#include <jce/middleware/script/jce_script.h>

#include "unity.h"
#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* Accumulate every logged marker into one buffer for substring assertions. */
static char g_log[2048];

static void accum_log(void *user, const char *msg)
{
    (void)user;
    if (!msg) return;
    size_t n = strlen(g_log);
    snprintf(g_log + n, sizeof(g_log) - n, "%s|", msg);
}

static bool logged(const char *marker)
{
    return strstr(g_log, marker) != NULL;
}

static JceScript *make_vm(void)
{
    JceScriptHost host;
    memset(&host, 0, sizeof host);
    host.log = accum_log;
    JceScript *vm = jce_script_create(&host);
    TEST_ASSERT_NOT_NULL(vm);

    static const char *SRC =
        "function co_basic()\n"
        "  jce.start_coroutine(function()\n"
        "    jce.log('A')\n"
        "    jce.wait_seconds(1.0)\n"
        "    jce.log('B')\n"
        "  end)\n"
        "end\n"
        "function co_chain()\n"
        "  jce.start_coroutine(function()\n"
        "    jce.log('C1')\n"
        "    jce.wait_seconds(0.5)\n"
        "    jce.log('C2')\n"
        "    jce.wait_seconds(0.5)\n"
        "    jce.log('C3')\n"
        "  end)\n"
        "end\n"
        "function co_instant()\n"
        "  jce.start_coroutine(function() jce.log('INSTANT') end)\n"
        "end\n"
        "cancel_handle = 0\n"
        "function co_cancel()\n"
        "  cancel_handle = jce.start_coroutine(function()\n"
        "    jce.log('X1')\n"
        "    jce.wait_seconds(1.0)\n"
        "    jce.log('X2')\n"
        "  end)\n"
        "end\n"
        "function do_cancel() jce.stop_coroutine(cancel_handle) end\n"
        "function co_misuse() jce.wait_seconds(0.5) end\n";
    (void)jce_script_instantiate_source(vm, "co_handlers", SRC, /*owner*/0);
    return vm;
}

/* ── basic wait then resume ───────────────────────────────────────────── */
static void test_wait_then_resume(void)
{
    g_log[0] = '\0';
    JceScript *vm = make_vm();

    jce_script_call_named(vm, "co_basic", 0);
    TEST_ASSERT_TRUE(logged("A"));          /* ran to first wait immediately */
    TEST_ASSERT_FALSE(logged("B"));

    jce_script_update_coroutines(vm, 0.5f);  /* 0.5 < 1.0 → still parked */
    TEST_ASSERT_FALSE(logged("B"));

    jce_script_update_coroutines(vm, 0.6f);  /* 1.1 ≥ 1.0 → resume */
    TEST_ASSERT_TRUE(logged("B"));

    /* No further resumes after completion. */
    g_log[0] = '\0';
    jce_script_update_coroutines(vm, 5.0f);
    TEST_ASSERT_FALSE(logged("B"));

    jce_script_destroy(vm);
}

/* ── chained waits step in order ──────────────────────────────────────── */
static void test_chained_waits(void)
{
    g_log[0] = '\0';
    JceScript *vm = make_vm();

    jce_script_call_named(vm, "co_chain", 0);
    TEST_ASSERT_TRUE(logged("C1"));
    TEST_ASSERT_FALSE(logged("C2"));

    jce_script_update_coroutines(vm, 0.5f);  /* first wait elapses → C2 */
    TEST_ASSERT_TRUE(logged("C2"));
    TEST_ASSERT_FALSE(logged("C3"));

    jce_script_update_coroutines(vm, 0.5f);  /* second wait elapses → C3 */
    TEST_ASSERT_TRUE(logged("C3"));

    jce_script_destroy(vm);
}

/* ── a coroutine with no wait completes immediately ───────────────────── */
static void test_instant_completes(void)
{
    g_log[0] = '\0';
    JceScript *vm = make_vm();

    jce_script_call_named(vm, "co_instant", 0);
    TEST_ASSERT_TRUE(logged("INSTANT"));     /* ran fully at start */

    /* Nothing parked → update is a clean no-op (and must not crash). */
    jce_script_update_coroutines(vm, 1.0f);

    jce_script_destroy(vm);
}

/* ── stop_coroutine cancels before resume ─────────────────────────────── */
static void test_stop_cancels(void)
{
    g_log[0] = '\0';
    JceScript *vm = make_vm();

    jce_script_call_named(vm, "co_cancel", 0);
    TEST_ASSERT_TRUE(logged("X1"));
    jce_script_call_named(vm, "do_cancel", 0);   /* cancel while parked */

    jce_script_update_coroutines(vm, 5.0f);      /* would have resumed */
    TEST_ASSERT_FALSE(logged("X2"));             /* but it was cancelled */

    jce_script_destroy(vm);
}

/* ── wait_seconds outside a coroutine is a caught error (no crash) ─────── */
static void test_misuse_is_caught(void)
{
    g_log[0] = '\0';
    JceScript *vm = make_vm();

    /* Calling jce.wait_seconds outside start_coroutine raises a Lua error that
     * the call_named pcall catches + logs — the process must survive. */
    bool fired = jce_script_call_named(vm, "co_misuse", 0);
    TEST_ASSERT_TRUE(fired);   /* handler existed (error caught internally) */

    /* And a subsequent normal coroutine still works (VM not wedged). */
    g_log[0] = '\0';
    jce_script_call_named(vm, "co_basic", 0);
    jce_script_update_coroutines(vm, 1.5f);
    TEST_ASSERT_TRUE(logged("B"));

    jce_script_destroy(vm);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_wait_then_resume);
    RUN_TEST(test_chained_waits);
    RUN_TEST(test_instant_completes);
    RUN_TEST(test_stop_cancels);
    RUN_TEST(test_misuse_is_caught);
    return UNITY_END();
}
