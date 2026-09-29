/* test_jce_anim_event_dispatch.c
 *
 * Headless coverage for the animation frame-event → script dispatch primitive
 * jce_script_call_anim_event(s, inst, id, name, f0, f1, i0), which delivers a
 * fired animation event to a script instance's `on_anim_event` method.
 *
 * This tests the VM primitive DIRECTLY — no scene renderer, no runtime, no
 * entity→instance map.  The primitive dispatches a fixed-name method on a
 * specific INSTANCE table (resolved via its metatable __index=module), exactly
 * like jce_script_call_message.  Instances are built by
 * jce_script_instantiate_source; receipt is observed through Lua GLOBALS the
 * handler writes, read back by a follow-up assert-chunk run on the same VM (an
 * assert() failure blows that chunk's load up -> a 0 instance, so a non-zero
 * instance proves every assertion held).
 *
 * Mirrors the harness of tests/middleware/script/test_jce_script_message.c.
 */

#include <jce/middleware/script/jce_script.h>

#include "unity.h"

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* A receiver module: on_anim_event records the full payload (id, name, f0, f1,
 * i0) into a global table RECV that a follow-up assert-chunk reads.  Defining it
 * as a method (M:on_anim_event) means the instance table indexes it through its
 * metatable, exactly how jce_script_call_anim_event resolves it. */
static const char *RECEIVER_SRC =
    "local M = {}\n"
    "function M:on_anim_event(id, name, f0, f1, i0)\n"
    "  RECV = { id = id, name = name, f0 = f0, f1 = f1, i0 = i0,\n"
    "           hit = (RECV and RECV.hit or 0) + 1 }\n"
    "end\n"
    "return M\n";

/* A module that defines NO on_anim_event handler (case 3). */
static const char *NO_HANDLER_SRC =
    "local M = {}\n"
    "function M:on_update(dt) end\n"
    "return M\n";

/* ── 1. event delivered to handler (full payload) ───────────────────────── */
static void test_anim_event_delivered(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);             /* primitive needs no host callbacks */
    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst =
        jce_script_instantiate_source(s, "@recv", RECEIVER_SRC, 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    jce_script_call_anim_event(s, inst, 7u, "footstep", 0.5f, 1.5f, 3);

    /* Read the receipt back via an assert-chunk on the same VM: a tripped
     * assert blows the chunk's load up (returns 0), so non-zero == all held. */
    JceScriptInstance chk = jce_script_instantiate_source(s, "@recv_check",
        "assert(RECV ~= nil, 'handler did not run')\n"
        "assert(RECV.id == 7, 'id arg')\n"
        "assert(RECV.name == 'footstep', 'name arg')\n"
        "assert(RECV.f0 == 0.5, 'f0 arg')\n"
        "assert(RECV.f1 == 1.5, 'f1 arg')\n"
        "assert(RECV.i0 == 3, 'i0 arg')\n"
        "assert(RECV.hit == 1, 'hit count')\n"
        "return {}\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, chk);

    jce_script_destroy(s);
}

/* ── 2. empty name -> nil on the receiver side ──────────────────────────── */
static void test_anim_event_empty_name_is_nil(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst =
        jce_script_instantiate_source(s, "@recv", RECEIVER_SRC, 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    /* Numeric-only event: empty name must arrive as Lua nil. */
    jce_script_call_anim_event(s, inst, 9u, "", 0.0f, 0.0f, 0);

    JceScriptInstance chk = jce_script_instantiate_source(s, "@nil_check",
        "assert(RECV ~= nil, 'handler did not run')\n"
        "assert(RECV.id == 9, 'id arg')\n"
        "assert(RECV.name == nil, 'empty name must arrive as nil')\n"
        "assert(type(RECV.name) == 'nil', 'name type must be nil')\n"
        "return {}\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, chk);

    jce_script_destroy(s);
}

/* ── 3. missing handler is a clean no-op; VM still usable afterwards ─────── */
static void test_anim_event_missing_handler_is_noop(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance no_handler =
        jce_script_instantiate_source(s, "@nohandler", NO_HANDLER_SRC, 1);
    TEST_ASSERT_NOT_EQUAL(0, no_handler);

    /* No on_anim_event method -> silent no-op (must not error / corrupt stack). */
    jce_script_call_anim_event(s, no_handler, 1u, "x", 0.0f, 0.0f, 0);

    /* A subsequent VALID dispatch on a receiver instance still works, proving
     * the VM stayed intact through the missing-handler call. */
    JceScriptInstance inst =
        jce_script_instantiate_source(s, "@recv", RECEIVER_SRC, 2);
    TEST_ASSERT_NOT_EQUAL(0, inst);
    jce_script_call_anim_event(s, inst, 5u, "ok", 2.0f, 4.0f, 6);
    JceScriptInstance chk = jce_script_instantiate_source(s, "@noop_check",
        "assert(RECV ~= nil and RECV.id == 5 and RECV.name == 'ok'\n"
        "       and RECV.f0 == 2.0 and RECV.f1 == 4.0 and RECV.i0 == 6,\n"
        "       'valid call after missing handler must still deliver')\n"
        "return {}\n", 3);
    TEST_ASSERT_NOT_EQUAL(0, chk);

    jce_script_destroy(s);
}

/* ── 4. invalid instance -> no crash ────────────────────────────────────── */
static void test_anim_event_invalid_instance_is_safe(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    /* 0 is the documented invalid instance handle: must be a no-op, no crash. */
    jce_script_call_anim_event(s, 0, 1u, "boom", 1.0f, 2.0f, 3);

    /* The VM remains usable after the invalid-handle call. */
    JceScriptInstance inst =
        jce_script_instantiate_source(s, "@recv", RECEIVER_SRC, 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);
    jce_script_call_anim_event(s, inst, 2u, "after", 0.0f, 0.0f, 0);
    JceScriptInstance chk = jce_script_instantiate_source(s, "@inv_check",
        "assert(RECV ~= nil and RECV.id == 2 and RECV.name == 'after',\n"
        "       'VM must stay usable after an invalid-instance call')\n"
        "return {}\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, chk);

    jce_script_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_anim_event_delivered);
    RUN_TEST(test_anim_event_empty_name_is_nil);
    RUN_TEST(test_anim_event_missing_handler_is_noop);
    RUN_TEST(test_anim_event_invalid_instance_is_safe);
    return UNITY_END();
}
