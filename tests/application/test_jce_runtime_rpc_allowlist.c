/*
 * test_jce_runtime_rpc_allowlist.c — remote peers may only call methods a
 * script deliberately exposed (audit lua-rpc-no-method-allowlist).
 *
 * rt_script_rpc_handler pulls a method name STRAIGHT OFF THE WIRE and hands
 * it to jce_script_call_message, which does a plain lua_getfield on the
 * instance table.  With no filter that reaches every method the script or its
 * metatable exposes — on_update, on_collision, on_destroy, every private
 * helper.  The channel is registered server_authoritative = false, so it
 * flows both ways: the reach belonged to any connected client, not just the
 * server.
 *
 * Every mainstream engine requires per-method opt-in for exactly this —
 * Unity [Command]/[ClientRpc], Unreal UFUNCTION(Server), Photon [PunRPC] —
 * and none expose the whole object surface.  JCE's opt-in is the name: a
 * method is remote-callable only if the author called it rpc_<something>.
 *
 * The predicate is deliberately reachable from here rather than static: this
 * is a security boundary, and an untested security boundary is a hope.
 */

#include "unity.h"

#include <stdbool.h>

/* Declared in engine/src/application/jce_rt_internal.h; redeclared here so the
   test does not need the engine's private include path. */
bool rt_script_rpc_name_allowed(const char *name);

void setUp(void) {}
void tearDown(void) {}

/* ---- the whole point: lifecycle methods must be unreachable ---------- */

static void test_lifecycle_methods_are_refused(void)
{
    /* These are the ones an attacker actually wants: they exist on every
       script, take engine-supplied arguments, and mutate game state. */
    static const char *const kLifecycle[] = {
        "on_start", "on_update", "on_destroy", "on_collision",
        "on_trigger_enter", "on_message", "on_anim_event",
    };
    for (unsigned i = 0; i < sizeof kLifecycle / sizeof kLifecycle[0]; ++i) {
        TEST_ASSERT_FALSE_MESSAGE(rt_script_rpc_name_allowed(kLifecycle[i]),
                                  kLifecycle[i]);
    }
}

/* Anything the author did not opt in is refused, including perfectly
   innocent-looking helper names — absence of opt-in IS the refusal. */
static void test_unprefixed_names_are_refused(void)
{
    TEST_ASSERT_FALSE(rt_script_rpc_name_allowed("fire"));
    TEST_ASSERT_FALSE(rt_script_rpc_name_allowed("take_damage"));
    TEST_ASSERT_FALSE(rt_script_rpc_name_allowed("_internal_reset"));
    TEST_ASSERT_FALSE(rt_script_rpc_name_allowed("update"));
}

static void test_opted_in_names_are_allowed(void)
{
    TEST_ASSERT_TRUE(rt_script_rpc_name_allowed("rpc_fire"));
    TEST_ASSERT_TRUE(rt_script_rpc_name_allowed("rpc_take_damage"));
    TEST_ASSERT_TRUE(rt_script_rpc_name_allowed("rpc_x"));
    TEST_ASSERT_TRUE(rt_script_rpc_name_allowed("rpc_A1_b2"));
}

/* The prefix alone is not a method name — it must actually name something,
   or "rpc_" would dispatch to a field called exactly that. */
static void test_bare_prefix_is_refused(void)
{
    TEST_ASSERT_FALSE(rt_script_rpc_name_allowed("rpc_"));
}

/* Near-misses that a naive strncmp-only check would let through. */
static void test_prefix_must_be_at_the_start(void)
{
    TEST_ASSERT_FALSE(rt_script_rpc_name_allowed("do_rpc_fire"));
    TEST_ASSERT_FALSE(rt_script_rpc_name_allowed("Rpc_fire"));   /* case */
    TEST_ASSERT_FALSE(rt_script_rpc_name_allowed("RPC_fire"));
    TEST_ASSERT_FALSE(rt_script_rpc_name_allowed(" rpc_fire"));  /* padded */
}

/* Wire-supplied strings are arbitrary bytes.  Restrict to an identifier
   charset so a malformed or padded name never reaches the VM, and so the
   refusal log stays readable. */
static void test_non_identifier_characters_are_refused(void)
{
    TEST_ASSERT_FALSE(rt_script_rpc_name_allowed("rpc_fire()"));
    TEST_ASSERT_FALSE(rt_script_rpc_name_allowed("rpc_fire;drop"));
    TEST_ASSERT_FALSE(rt_script_rpc_name_allowed("rpc_a.b"));
    TEST_ASSERT_FALSE(rt_script_rpc_name_allowed("rpc_a b"));
    TEST_ASSERT_FALSE(rt_script_rpc_name_allowed("rpc_\x01"));
}

static void test_degenerate_inputs_are_refused(void)
{
    TEST_ASSERT_FALSE(rt_script_rpc_name_allowed(NULL));
    TEST_ASSERT_FALSE(rt_script_rpc_name_allowed(""));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lifecycle_methods_are_refused);
    RUN_TEST(test_unprefixed_names_are_refused);
    RUN_TEST(test_opted_in_names_are_allowed);
    RUN_TEST(test_bare_prefix_is_refused);
    RUN_TEST(test_prefix_must_be_at_the_start);
    RUN_TEST(test_non_identifier_characters_are_refused);
    RUN_TEST(test_degenerate_inputs_are_refused);
    return UNITY_END();
}
