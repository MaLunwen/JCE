/* test_jce_script_broadcast.c
 *
 * Headless coverage for the global event bus binding jce.broadcast(msg
 * [, number] [, string]).  The VM (jce_script.c) marshals the Lua call to the
 * host's broadcast callback; the runtime's rt_script_broadcast then fans it out
 * to every live instance via jce_script_call_message (a thin loop over the
 * primitive already covered by test_jce_script_message — same scope split as the
 * send_message binding test).  Here we drive a MOCK host that records the
 * marshalled (msg, number, string) arguments and assert the C<->Lua marshalling
 * (full arity, optional-arg defaults: number -> 0, missing string -> NULL) plus
 * the NULL-callback safe no-op.
 */

#include <jce/middleware/script/jce_script.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    int    calls;
    char   msg[128];
    double num;
    bool   had_str;
    char   str[128];
} Recorder;

static Recorder g_rec;

static void mock_broadcast(void *user, const char *msg,
                           double number_arg, const char *str_arg)
{
    Recorder *r = (Recorder *)user;
    r->calls++;
    snprintf(r->msg, sizeof r->msg, "%s", msg ? msg : "");
    r->num = number_arg;
    r->had_str = (str_arg != NULL);
    snprintf(r->str, sizeof r->str, "%s", str_arg ? str_arg : "");
}

void setUp(void)    { memset(&g_rec, 0, sizeof g_rec); }
void tearDown(void) {}

/* ── 1. full arity marshals to the host ─────────────────────────────────── */
static void test_broadcast_marshals_full_arity(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user      = &g_rec;
    h.broadcast = mock_broadcast;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance a = jce_script_instantiate_source(s, "@bcast_full",
        "jce.broadcast('player_died', 3, 'red')\n"
        "return {}\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, a);
    TEST_ASSERT_EQUAL_INT(1, g_rec.calls);
    TEST_ASSERT_EQUAL_STRING("player_died", g_rec.msg);
    TEST_ASSERT_EQUAL_DOUBLE(3.0, g_rec.num);
    TEST_ASSERT_TRUE(g_rec.had_str);
    TEST_ASSERT_EQUAL_STRING("red", g_rec.str);

    jce_script_destroy(s);
}

/* ── 2. optional-arg defaults: number -> 0, missing string -> NULL ──────── */
static void test_broadcast_defaults(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user      = &g_rec;
    h.broadcast = mock_broadcast;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance b = jce_script_instantiate_source(s, "@bcast_min",
        "jce.broadcast('wave_start')\n"
        "return {}\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, b);
    TEST_ASSERT_EQUAL_INT(1, g_rec.calls);
    TEST_ASSERT_EQUAL_STRING("wave_start", g_rec.msg);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, g_rec.num);
    TEST_ASSERT_FALSE(g_rec.had_str);

    jce_script_destroy(s);
}

/* ── 3. NULL host broadcast callback is a safe no-op ────────────────────── */
static void test_broadcast_null_host_safe(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user = &g_rec;             /* have_host true, but broadcast NULL */

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst = jce_script_instantiate_source(s, "@bcast_null",
        "jce.broadcast('noop', 1, 'y')\n"
        "return {}\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);            /* survived: no crash/error */
    TEST_ASSERT_EQUAL_INT(0, g_rec.calls);     /* nothing recorded */

    jce_script_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_broadcast_marshals_full_arity);
    RUN_TEST(test_broadcast_defaults);
    RUN_TEST(test_broadcast_null_host_safe);
    return UNITY_END();
}
