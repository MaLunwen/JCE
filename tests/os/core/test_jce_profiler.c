/*
 * test_jce_profiler.c — Unit tests for jce_profiler.h macros.
 *
 * Layer: L1.  When Tracy is disabled (default test build) every macro
 * compiles to a no-op — verifying that user code still compiles and
 * the macros do not have side effects.
 */

#include "unity.h"

#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_trace.h>
#include "os/core/jce_trace_scope_internal.h"
#include <string.h>

void setUp(void) { jce_trace_set_enabled(true); jce_trace_reset(); }
void tearDown(void) { jce_trace_set_enabled(false); }

static void test_macros_compile_and_run(void)
{
    int side_effect = 0;
    {
        JCE_PROFILE_ZONE;
        side_effect += 1;
        JCE_PROFILE_ZONE_END;
    }
    {
        JCE_PROFILE_ZONE_N("named");
        side_effect += 1;
        JCE_PROFILE_ZONE_END;
    }
    {
        JCE_PROFILE_ZONE_C(0xFF0000);
        side_effect += 1;
        JCE_PROFILE_ZONE_END;
    }

    JCE_PROFILE_FRAME_MARK;
    JCE_PROFILE_FRAME_MARK_N("custom");
    JCE_PROFILE_ALLOC((void*)0x1234, 64);
    JCE_PROFILE_FREE ((void*)0x1234);
    JCE_PROFILE_PLOT  ("plot",  3.14);
    JCE_PROFILE_PLOT_I("ploti", 42);
    JCE_PROFILE_MSG   ("hi", 2);
    JCE_PROFILE_THREAD_NAME("worker");

    TEST_ASSERT_EQUAL_INT(3, side_effect);
}

static void test_profile_zone_restores_service_parent_and_child_causality(void)
{
#if defined(JCE_TRACY_ENABLED) && (JCE_TRACY_ENABLED + 0 == 1)
    JceTraceScope parent = jce_trace_scope_begin("profile-parent-service");
    JceProfileZone zone = jce_profile_zone_begin("profile-nested-zone", 0,
                                                __FILE__, __LINE__, __func__);
    uint64_t zone_task = jce_trace_task_current_id();
    JceTraceScope child = jce_trace_scope_begin("profile-child-service");
    jce_trace_scope_end(child, JCE_TRACE_TASK_SUCCEEDED);
    uint64_t restored_zone = jce_trace_task_current_id();
    jce_profile_zone_end(zone);
    uint64_t restored_parent = jce_trace_task_current_id();
    JceTraceScope sibling = jce_trace_scope_begin("profile-sibling-service");
    jce_trace_scope_end(sibling, JCE_TRACE_TASK_SUCCEEDED);
    jce_trace_scope_end(parent, JCE_TRACE_TASK_SUCCEEDED);
    JceTraceEvent events[32];
    JceTraceCursor cursor;
    jce_trace_cursor_init(&cursor);
    uint32_t count = jce_trace_read(&cursor, events, 32);
    uint64_t child_parent = 0, sibling_parent = 0, zone_parent = 0;
    uint32_t zone_kind = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].type != JCE_TRACE_EVENT_TASK_SUBMIT) continue;
        if (!strcmp(events[i].name, "profile-nested-zone")) {
            zone_parent = events[i].parent_id;
            zone_kind = events[i].value_u32[0];
        }
        if (!strcmp(events[i].name, "profile-child-service"))
            child_parent = events[i].parent_id;
        if (!strcmp(events[i].name, "profile-sibling-service"))
            sibling_parent = events[i].parent_id;
    }
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_NOT_EQUAL(0, zone_task);
    TEST_ASSERT_NOT_EQUAL(parent.id, zone_task);
    TEST_ASSERT_EQUAL_UINT64(parent.id, zone_parent);
    TEST_ASSERT_EQUAL_UINT(JCE_TRACE_TASK_SERVICE, zone_kind);
    TEST_ASSERT_EQUAL_UINT64(zone_task, child_parent);
    TEST_ASSERT_EQUAL_UINT64(zone_task, restored_zone);
    TEST_ASSERT_EQUAL_UINT64(parent.id, restored_parent);
    TEST_ASSERT_EQUAL_UINT64(parent.id, sibling_parent);
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
    TEST_ASSERT_EQUAL_UINT(0, stats.active_work_items);
#else
    TEST_IGNORE_MESSAGE("Tracy zone bridge disabled in this configuration");
#endif
}

static void test_nested_profile_zones_and_depth_overflow_preserve_parent(void)
{
#if defined(JCE_TRACY_ENABLED) && (JCE_TRACY_ENABLED + 0 == 1)
    JceTraceScope parent = jce_trace_scope_begin("profile-depth-parent");
    JceProfileZone zones[66];
    uint64_t tasks[66];
    for (unsigned i = 0; i < 66; ++i) {
        zones[i] = jce_profile_zone_begin("profile-depth-zone", 0,
                                         __FILE__, __LINE__, __func__);
        tasks[i] = jce_trace_task_current_id();
    }
    TEST_ASSERT_EQUAL_UINT64(tasks[63], tasks[64]);
    TEST_ASSERT_EQUAL_UINT64(tasks[63], tasks[65]);
    for (unsigned i = 66; i > 0; --i) {
        jce_profile_zone_end(zones[i - 1]);
        uint64_t expected = i > 64 ? tasks[63]
            : (i > 1 ? tasks[i - 2] : parent.id);
        TEST_ASSERT_EQUAL_UINT64(expected, jce_trace_task_current_id());
    }
    JceTraceScope sibling = jce_trace_scope_begin("profile-depth-sibling");
    TEST_ASSERT_EQUAL_UINT64(parent.id, sibling.parent);
    jce_trace_scope_end(sibling, JCE_TRACE_TASK_SUCCEEDED);
    jce_trace_scope_end(parent, JCE_TRACE_TASK_SUCCEEDED);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
    TEST_ASSERT_EQUAL_UINT(66, stats.tasks_submitted);
#else
    TEST_IGNORE_MESSAGE("Tracy zone bridge disabled in this configuration");
#endif
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_macros_compile_and_run);
    RUN_TEST(test_profile_zone_restores_service_parent_and_child_causality);
    RUN_TEST(test_nested_profile_zones_and_depth_overflow_preserve_parent);
    return UNITY_END();
}
