/* test_jce_script_rpc.c
 *
 * Headless coverage for the scripted-RPC binding jce.rpc_send(entity, event
 * [, target] [, payload]).  The VM marshals the call to the host's rpc_send
 * callback; the runtime packs it onto the generic script-RPC channel and the
 * receiver dispatches `event` as a method (a thin loop over the already-tested
 * jce_rpc_send + jce_script_call_message — same scope split as the send_message
 * / broadcast binding tests).  Here a MOCK host records the marshalled (entity,
 * event, target, payload) and returns a configurable result; we assert the
 * C<->Lua marshalling (full arity, defaults: target -> 0/TO_SERVER, missing
 * payload -> NULL), the bool return round-trip, and NULL-host safety.
 */

#include <jce/middleware/script/jce_script.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    int             calls;
    JceScriptEntity entity;
    char            event[128];
    int             target;
    bool            had_payload;
    char            payload[128];
    bool            ret;          /* what the mock returns */
} Recorder;

static Recorder g_rec;

static bool mock_rpc_send(void *user, JceScriptEntity e, const char *event,
                          int target, const char *payload)
{
    Recorder *r = (Recorder *)user;
    r->calls++;
    r->entity = e;
    snprintf(r->event, sizeof r->event, "%s", event ? event : "");
    r->target = target;
    r->had_payload = (payload != NULL);
    snprintf(r->payload, sizeof r->payload, "%s", payload ? payload : "");
    return r->ret;
}

void setUp(void)    { memset(&g_rec, 0, sizeof g_rec); g_rec.ret = true; }
void tearDown(void) {}

/* ── 1. full arity marshals + true return ───────────────────────────────── */
static void test_full_arity(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user     = &g_rec;
    h.rpc_send = mock_rpc_send;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance a = jce_script_instantiate_source(s, "@rpc_full",
        "OK = jce.rpc_send(5, 'fire', 2, 'dmg=10')\n"
        "return {}\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, a);
    TEST_ASSERT_EQUAL_INT(1, g_rec.calls);
    TEST_ASSERT_EQUAL_UINT64(5u, g_rec.entity);
    TEST_ASSERT_EQUAL_STRING("fire", g_rec.event);
    TEST_ASSERT_EQUAL_INT(2, g_rec.target);
    TEST_ASSERT_TRUE(g_rec.had_payload);
    TEST_ASSERT_EQUAL_STRING("dmg=10", g_rec.payload);

    /* true return round-trips to Lua */
    JceScriptInstance chk = jce_script_instantiate_source(s, "@rpc_chk",
        "assert(OK == true, 'rpc_send should return true')\nreturn {}\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, chk);

    jce_script_destroy(s);
}

/* ── 2. defaults: omitted target -> 0, omitted payload -> NULL ──────────── */
static void test_defaults(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user     = &g_rec;
    h.rpc_send = mock_rpc_send;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance a = jce_script_instantiate_source(s, "@rpc_min",
        "jce.rpc_send(6, 'jump')\nreturn {}\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, a);
    TEST_ASSERT_EQUAL_INT(1, g_rec.calls);
    TEST_ASSERT_EQUAL_UINT64(6u, g_rec.entity);
    TEST_ASSERT_EQUAL_STRING("jump", g_rec.event);
    TEST_ASSERT_EQUAL_INT(0, g_rec.target);          /* default TO_SERVER */
    TEST_ASSERT_FALSE(g_rec.had_payload);            /* NULL payload */

    jce_script_destroy(s);
}

/* ── 3. false return round-trips ────────────────────────────────────────── */
static void test_false_return(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user     = &g_rec;
    h.rpc_send = mock_rpc_send;
    g_rec.ret  = false;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance a = jce_script_instantiate_source(s, "@rpc_false",
        "RES = jce.rpc_send(1, 'x', 0, 'p')\nreturn {}\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, a);
    TEST_ASSERT_EQUAL_INT(1, g_rec.calls);

    JceScriptInstance chk = jce_script_instantiate_source(s, "@rpc_false_chk",
        "assert(RES == false, 'false should round-trip')\nreturn {}\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, chk);

    jce_script_destroy(s);
}

/* ── 4. NULL host rpc_send -> false, nothing recorded, no crash ─────────── */
static void test_null_host_safe(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user = &g_rec;              /* have_host true, rpc_send NULL */

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance a = jce_script_instantiate_source(s, "@rpc_null",
        "R = jce.rpc_send(9, 'noop', 1, 'y')\nreturn {}\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, a);
    TEST_ASSERT_EQUAL_INT(0, g_rec.calls);

    JceScriptInstance chk = jce_script_instantiate_source(s, "@rpc_null_chk",
        "assert(R == false, 'NULL host -> false')\nreturn {}\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, chk);

    jce_script_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_full_arity);
    RUN_TEST(test_defaults);
    RUN_TEST(test_false_return);
    RUN_TEST(test_null_host_safe);
    return UNITY_END();
}
