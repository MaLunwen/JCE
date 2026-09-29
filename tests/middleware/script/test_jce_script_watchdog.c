/* test_jce_script_watchdog.c
 *
 * Script VM watchdog: an infinite loop in a dispatched handler must be ABORTED
 * (luaL_error out of the pcall via a re-armed LUA_MASKCOUNT hook), not
 * hard-hang the single-threaded runtime.  Headless VM, binding-less host with a
 * log accumulator.  The test simply RETURNING (not hitting the 60s CTest
 * timeout) proves recovery; we also assert the error was reported, and that a
 * normal script does NOT trip the watchdog.
 */

#include <jce/middleware/script/jce_script.h>

#include "unity.h"
#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static char g_log[2048];

static void accum_log(void *user, const char *msg)
{
    (void)user;
    if (!msg) return;
    size_t n = strlen(g_log);
    snprintf(g_log + n, sizeof(g_log) - n, "%s|", msg);
}

static JceScript *make_vm(void)
{
    JceScriptHost host;
    memset(&host, 0, sizeof host);
    host.log = accum_log;
    return jce_script_create(&host);
}

/* `while true do end` in on_update must be aborted by the watchdog. */
static void test_watchdog_aborts_infinite_loop(void)
{
    g_log[0] = 0;
    JceScript *s = make_vm();
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst = jce_script_instantiate_source(s, "wd",
        "local M={}\n"
        "function M:on_update(dt) while true do end end\n"
        "return M", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    jce_script_call_update(s, inst, 0.016f);   /* MUST return (not hang) */

    TEST_ASSERT_TRUE_MESSAGE(strstr(g_log, "watchdog") != NULL,
        "watchdog should have aborted the infinite loop and logged the error");

    jce_script_destroy(s);
}

/* A normal bounded loop must NOT trip the watchdog. */
static void test_watchdog_allows_normal_script(void)
{
    g_log[0] = 0;
    JceScript *s = make_vm();
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst = jce_script_instantiate_source(s, "ok",
        "local M={}\n"
        "function M:on_update(dt) local x=0 for i=1,10000 do x=x+i end end\n"
        "return M", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    jce_script_call_update(s, inst, 0.016f);

    TEST_ASSERT_TRUE_MESSAGE(strstr(g_log, "watchdog") == NULL,
        "a normal bounded script must not trip the watchdog");

    jce_script_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_watchdog_aborts_infinite_loop);
    RUN_TEST(test_watchdog_allows_normal_script);
    return UNITY_END();
}
