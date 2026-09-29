/* test_jce_script_message.c
 *
 * Headless coverage for script-to-script messaging (jce.send_message):
 *   - the VM primitive jce_script_call_message(s, inst, name, num, str), and
 *   - the Lua binding jce.send_message(target, msg [, number] [, string]).
 *
 * The VM (jce_script.c) is a GENERIC Lua host; the messaging primitive dispatches
 * a named method on a specific INSTANCE table (resolved via its metatable
 * __index=module), mirroring jce_script_call_collision.  This test drives that
 * primitive DIRECTLY on instances built by jce_script_instantiate_source — no
 * runtime, no scene, no entity->instance map needed.  Receipt is observed through
 * Lua GLOBALS the handler writes, read back by a follow-up assert-chunk run on the
 * same VM (an assert() failure blows that chunk's load up -> a 0 instance, so a
 * non-zero instance proves every assertion held).
 *
 * The binding-level case installs a MOCK host whose send_message records its
 * marshalled (target, msg, number, string) arguments, mirroring the mock-host
 * approach in test_jce_script_bindings.c, and asserts the C<->Lua marshalling
 * (including optional-arg defaults: number -> 0, missing string -> NULL).
 */

#include <jce/middleware/script/jce_script.h>

#include "unity.h"

#include <stdio.h>   /* snprintf */
#include <string.h>

/* ── Mock host recorder (binding-level case) ────────────────────────────── */
typedef struct {
    int             send_calls;
    JceScriptEntity send_target;
    char            send_msg[128];
    double          send_num;
    bool            send_had_str;
    char            send_str[128];
} Recorder;

static Recorder g_rec;

static void mock_send_message(void *user, JceScriptEntity target,
                              const char *msg, double number_arg,
                              const char *str_arg)
{
    Recorder *r = (Recorder *)user;
    r->send_calls++;
    r->send_target = target;
    snprintf(r->send_msg, sizeof r->send_msg, "%s", msg ? msg : "");
    r->send_num = number_arg;
    r->send_had_str = (str_arg != NULL);
    snprintf(r->send_str, sizeof r->send_str, "%s", str_arg ? str_arg : "");
}

void setUp(void)    { memset(&g_rec, 0, sizeof g_rec); }
void tearDown(void) {}

/* A receiver module: on_ping records the payload (and a running hit-count) into a
 * global table RECV that a follow-up assert-chunk reads.  Defining it as a method
 * (M:on_ping) means the instance table indexes it through its metatable, exactly
 * how jce_script_call_message resolves it. */
static const char *RECEIVER_SRC =
    "local M = {}\n"
    "function M:on_ping(n, s)\n"
    "  RECV = { n = n, s = s, hit = (RECV and RECV.hit or 0) + 1 }\n"
    "end\n"
    "return M\n";

/* ── 1. message delivered to handler (number + string payload) ──────────── */
static void test_message_delivered_to_handler(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);             /* primitive needs no host callbacks */
    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst =
        jce_script_instantiate_source(s, "@recv", RECEIVER_SRC, 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    jce_script_call_message(s, inst, "on_ping", 42.0, "hello");

    /* Read the receipt back via an assert-chunk on the same VM: a tripped
     * assert blows the chunk's load up (returns 0), so non-zero == all held. */
    JceScriptInstance chk = jce_script_instantiate_source(s, "@recv_check",
        "assert(RECV ~= nil, 'handler did not run')\n"
        "assert(RECV.n == 42, 'number arg')\n"
        "assert(RECV.s == 'hello', 'string arg')\n"
        "assert(RECV.hit == 1, 'hit count')\n"
        "return {}\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, chk);

    jce_script_destroy(s);
}

/* ── 2. missing handler is a clean no-op; VM still usable afterwards ─────── */
static void test_missing_handler_is_noop(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst =
        jce_script_instantiate_source(s, "@recv", RECEIVER_SRC, 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    /* No such method -> silent no-op (must not error / corrupt the stack). */
    jce_script_call_message(s, inst, "on_nonexistent", 1.0, NULL);

    /* A subsequent VALID call still works, proving the VM is intact. */
    jce_script_call_message(s, inst, "on_ping", 5.0, "ok");
    JceScriptInstance chk = jce_script_instantiate_source(s, "@noop_check",
        "assert(RECV ~= nil and RECV.n == 5 and RECV.s == 'ok',\n"
        "       'valid call after missing handler must still deliver')\n"
        "return {}\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, chk);

    jce_script_destroy(s);
}

/* ── 3. NULL string arg -> nil on the receiver side ─────────────────────── */
static void test_null_string_arg_is_nil(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst =
        jce_script_instantiate_source(s, "@recv", RECEIVER_SRC, 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    jce_script_call_message(s, inst, "on_ping", 7.0, NULL);

    JceScriptInstance chk = jce_script_instantiate_source(s, "@nil_check",
        "assert(RECV.n == 7, 'number arg')\n"
        "assert(RECV.s == nil, 'NULL string must arrive as nil')\n"
        "assert(type(RECV.s) == 'nil', 'string type must be nil')\n"
        "return {}\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, chk);

    jce_script_destroy(s);
}

/* ── 4. invalid instance -> no crash ────────────────────────────────────── */
static void test_invalid_instance_is_safe(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    /* 0 is the documented invalid instance handle: must be a no-op, no crash. */
    jce_script_call_message(s, 0, "on_ping", 1.0, NULL);

    /* The VM remains usable after the invalid-handle call. */
    JceScriptInstance inst =
        jce_script_instantiate_source(s, "@recv", RECEIVER_SRC, 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);
    jce_script_call_message(s, inst, "on_ping", 2.0, "after");
    JceScriptInstance chk = jce_script_instantiate_source(s, "@inv_check",
        "assert(RECV ~= nil and RECV.n == 2 and RECV.s == 'after',\n"
        "       'VM must stay usable after an invalid-instance call')\n"
        "return {}\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, chk);

    jce_script_destroy(s);
}

/* ── 5. binding-level: jce.send_message marshals to the host (arities) ──── */
static void test_send_message_binding_marshals(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user         = &g_rec;
    h.send_message = mock_send_message;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    /* Full arity: target 5, msg 'go', number 3, string 'x'. */
    JceScriptInstance a = jce_script_instantiate_source(s, "@send_full",
        "jce.send_message(5, 'go', 3, 'x')\n"
        "return {}\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, a);
    TEST_ASSERT_EQUAL_INT(1, g_rec.send_calls);
    TEST_ASSERT_EQUAL_UINT64(5u, g_rec.send_target);
    TEST_ASSERT_EQUAL_STRING("go", g_rec.send_msg);
    TEST_ASSERT_EQUAL_DOUBLE(3.0, g_rec.send_num);
    TEST_ASSERT_TRUE(g_rec.send_had_str);
    TEST_ASSERT_EQUAL_STRING("x", g_rec.send_str);

    /* Defaults: omitted number -> 0, omitted string -> NULL. */
    JceScriptInstance b = jce_script_instantiate_source(s, "@send_min",
        "jce.send_message(6, 'stop')\n"
        "return {}\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, b);
    TEST_ASSERT_EQUAL_INT(2, g_rec.send_calls);
    TEST_ASSERT_EQUAL_UINT64(6u, g_rec.send_target);
    TEST_ASSERT_EQUAL_STRING("stop", g_rec.send_msg);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, g_rec.send_num);
    TEST_ASSERT_FALSE(g_rec.send_had_str);

    jce_script_destroy(s);
}

/* ── 6. binding-level: NULL host send_message is a safe no-op ────────────── */
static void test_send_message_binding_null_host_safe(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user = &g_rec;             /* have_host true, but send_message NULL */

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst = jce_script_instantiate_source(s, "@send_null",
        "jce.send_message(9, 'noop', 1, 'y')\n"
        "return {}\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);   /* survived: no crash, no error */
    TEST_ASSERT_EQUAL_INT(0, g_rec.send_calls);   /* nothing recorded */

    jce_script_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_message_delivered_to_handler);
    RUN_TEST(test_missing_handler_is_noop);
    RUN_TEST(test_null_string_arg_is_nil);
    RUN_TEST(test_invalid_instance_is_safe);
    RUN_TEST(test_send_message_binding_marshals);
    RUN_TEST(test_send_message_binding_null_host_safe);
    return UNITY_END();
}
