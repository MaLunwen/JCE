/* Actual bootstrap scope trace contract; no window or renderer. */
#include "unity.h"
#include "application/jce_engine_trace_scope.h"

void setUp(void)
{
    jce_trace_set_enabled(true);
    jce_trace_reset();
}

void tearDown(void)
{
    jce_trace_set_enabled(false);
}

static void test_disabled_scope_emits_nothing(void)
{
    JceTraceCursor cursor;
    JceTraceEvent event;
    JceEngineTraceScope scope;
    jce_trace_set_enabled(false);
    scope = engine_trace_begin("disabled");
    engine_trace_end(scope, JCE_TRACE_TASK_FAILED);
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT64(0, scope.id);
    TEST_ASSERT_EQUAL_UINT32(0, jce_trace_read(&cursor, &event, 1));
}

static void test_nested_scopes_restore_parent_and_preserve_failure(void)
{
    JceEngineTraceScope outer = engine_trace_begin("outer");
    JceEngineTraceScope inner = engine_trace_begin("inner");
    JceTraceCursor cursor;
    JceTraceEvent events[6];
    JceTraceStats stats;
    TEST_ASSERT_NOT_EQUAL(outer.id, inner.id);
    TEST_ASSERT_EQUAL_UINT64(outer.id, inner.parent);
    engine_trace_end(inner, JCE_TRACE_TASK_FAILED);
    TEST_ASSERT_EQUAL_UINT64(outer.id, jce_trace_task_current_id());
    engine_trace_end(outer, JCE_TRACE_TASK_SUCCEEDED);
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT64(2, stats.work_items_completed);
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(6, jce_trace_read(&cursor, events, 6));
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_EVENT_TASK_SUBMIT, events[0].type);
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_EVENT_TASK_BEGIN, events[1].type);
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_EVENT_TASK_SUBMIT, events[2].type);
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_EVENT_TASK_BEGIN, events[3].type);
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_EVENT_TASK_END, events[4].type);
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_EVENT_TASK_END, events[5].type);
    TEST_ASSERT_EQUAL_UINT64(inner.id, events[3].id);
    TEST_ASSERT_EQUAL_UINT64(inner.id, events[4].id);
    TEST_ASSERT_EQUAL_UINT64(outer.id, events[1].id);
    TEST_ASSERT_EQUAL_UINT64(outer.id, events[5].id);
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED, events[4].value_u32[0]);
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_SUCCEEDED, events[5].value_u32[0]);
}

static int scope_expression(int *calls, int value)
{
    (*calls)++;
    return value;
}

static bool scope_result(int *checks, int value)
{
    (*checks)++;
    return value != 0;
}

static void test_result_macro_evaluates_once_and_records_actual_outcome(void)
{
    JceTraceCursor cursor;
    JceTraceEvent events[6];
    JceTraceStats stats;
    int calls = 0;
    int checks = 0;
    int value = -1;

    JCE_ENGINE_TRACE_RESULT("failed-expression",
        value = scope_expression(&calls, 0), scope_result(&checks, value));
    TEST_ASSERT_EQUAL_INT(1, calls);
    TEST_ASSERT_EQUAL_INT(1, checks);
    TEST_ASSERT_EQUAL_INT(0, value);
    JCE_ENGINE_TRACE_RESULT("successful-expression",
        value = scope_expression(&calls, 1), scope_result(&checks, value));
    TEST_ASSERT_EQUAL_INT(2, calls);
    TEST_ASSERT_EQUAL_INT(2, checks);
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(6, jce_trace_read(&cursor, events, 6));
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_EVENT_TASK_END, events[2].type);
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED, events[2].value_u32[0]);
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_EVENT_TASK_END, events[5].type);
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_SUCCEEDED, events[5].value_u32[0]);
    TEST_ASSERT_EQUAL_UINT64(events[1].id, events[2].id);
    TEST_ASSERT_EQUAL_UINT64(events[4].id, events[5].id);
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);

    jce_trace_set_enabled(false);
    JCE_ENGINE_TRACE_RESULT("disabled-expression",
        value = scope_expression(&calls, 0), scope_result(&checks, value));
    TEST_ASSERT_EQUAL_INT(3, calls);
    TEST_ASSERT_EQUAL_INT(3, checks);
    TEST_ASSERT_EQUAL_UINT32(0, jce_trace_read(&cursor, events, 6));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_disabled_scope_emits_nothing);
    RUN_TEST(test_nested_scopes_restore_parent_and_preserve_failure);
    RUN_TEST(test_result_macro_evaluates_once_and_records_actual_outcome);
    return UNITY_END();
}
