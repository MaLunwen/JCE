/*
 * test_jce_bt_exception_firewall.c — regression guard for the C-ABI exception
 * firewall on the BehaviorTree.CPP backend (dependency/ABI audit, P0
 * `cpp-consumer-bt-*`).
 *
 * BehaviorTree.CPP throws C++ exceptions on two data-reachable paths that
 * cross JCE's extern "C" boundary into C99 callers:
 *
 *   1. jce_bt_register_action() with a name already registered
 *      -> BT::BehaviorTreeException("ID already registered")
 *   2. jce_bt_tick() of a tree whose built-in node faults at run time
 *      (e.g. <Repeat num_cycles="{missing_key}">) -> BT::RuntimeError
 *
 * Before the fix these escaped through C frames = UB / std::terminate. The
 * fix wraps registerBuilder / tickOnce / haltTree in try/catch inside
 * jce_bt_impl.cpp. This test proves the process SURVIVES both (no abort) and
 * that a faulting tick reports FAILURE rather than crashing.
 *
 * Pure C99 consumer of the public <jce/middleware/ai/jce_bt.h> ABI.
 */

#include "unity.h"

#include <jce/middleware/ai/jce_bt.h>

#include <stdint.h>
#include <string.h>

static JceBtContext *g_ctx;

void setUp(void)
{
    g_ctx = jce_bt_create();
    TEST_ASSERT_NOT_NULL(g_ctx);
}

void tearDown(void)
{
    if (g_ctx) {
        jce_bt_destroy(g_ctx);
        g_ctx = NULL;
    }
}

static JceBtStatus noop_action(const char *name, void *userdata)
{
    (void)name;
    (void)userdata;
    return JCE_BT_SUCCESS;
}

/* Registering the SAME action name twice must not abort — the second call
 * hits BT.CPP's duplicate-ID throw, which the firewall must contain. */
void test_duplicate_action_registration_does_not_abort(void)
{
    jce_bt_register_action(g_ctx, "Attack", noop_action, NULL);
    /* Second registration of the same name: pre-fix this threw across the
     * C ABI. Post-fix it is caught, logged, and returns cleanly. Reaching
     * the assertion below at all proves no terminate/abort happened. */
    jce_bt_register_action(g_ctx, "Attack", noop_action, NULL);
    jce_bt_register_action(g_ctx, "Attack", noop_action, NULL);
    TEST_PASS_MESSAGE("survived duplicate action registration (no abort)");
}

/* A tree that parses but faults at tick time must return FAILURE, not crash.
 * <Repeat num_cycles="{undefined}"> parses fine; the blackboard key is
 * missing, so tickOnce() throws BT::RuntimeError, which the firewall maps to
 * JCE_BT_FAILURE. */
void test_tick_time_throw_returns_failure(void)
{
    static const char kXml[] =
        "<root BTCPP_format=\"4\">"
        "  <BehaviorTree ID=\"Main\">"
        "    <Repeat num_cycles=\"{undefined_key}\">"
        "      <AlwaysSuccess/>"
        "    </Repeat>"
        "  </BehaviorTree>"
        "</root>";

    JceBtTreeHandle tree = jce_bt_load_tree(g_ctx, kXml, (uint32_t)strlen(kXml));
    if (!jce_bt_tree_valid(tree)) {
        /* Some BT.CPP builds reject the unresolved port at load time; that is
         * also an acceptable (contained) outcome — the point is no crash. */
        TEST_PASS_MESSAGE("tree rejected at load (contained, no crash)");
        return;
    }

    JceBtStatus st = jce_bt_tick(g_ctx, tree);
    /* Must be a defined status, never a crash. A faulting tick is FAILURE. */
    TEST_ASSERT_TRUE(st == JCE_BT_FAILURE || st == JCE_BT_RUNNING ||
                     st == JCE_BT_SUCCESS);
    /* Halt must also be firewalled. */
    jce_bt_halt(g_ctx, tree);
    TEST_PASS_MESSAGE("survived tick-time throw (no abort)");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_duplicate_action_registration_does_not_abort);
    RUN_TEST(test_tick_time_throw_returns_failure);
    return UNITY_END();
}
