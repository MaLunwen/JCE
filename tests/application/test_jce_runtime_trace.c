/* Complete shipping Runtime TU, actual Scene/Core ring, no native device.
 * Only the Runtime TU's malloc entry is a forwarding allocation-failure port. */
#include "application/jce_rt_internal.h"
#include "unity.h"
#include <jce/os/core/jce_trace.h>
static bool fail_next_allocation;
static void *runtime_malloc_port(size_t size)
{
    if (fail_next_allocation) { fail_next_allocation = false; return NULL; }
    return jce_malloc(size);
}
#define jce_malloc runtime_malloc_port
#ifndef JCE_TEST_RUNTIME_TRACE_OWNER
#define JCE_TEST_RUNTIME_TRACE_OWNER "application/jce_runtime.c"
#endif
#include JCE_TEST_RUNTIME_TRACE_OWNER
#undef jce_malloc

static JceScene *test_scene;
static JceRuntime *test_runtime;
static JceTraceEvent events[2048];
static uint32_t event_count;
void setUp(void)
{
    jce_trace_set_enabled(false);
    jce_trace_task_restore_id(0);
    jce_trace_reset();
    jce_console_shutdown();
    fail_next_allocation = false;
    test_runtime = NULL;
    test_scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(test_scene);
}
void tearDown(void)
{
    jce_trace_set_enabled(false);
    jce_runtime_destroy(test_runtime);
    test_runtime = NULL;
    jce_scene_destroy(test_scene);
    jce_console_shutdown();
    jce_trace_task_restore_id(0);
}
static JceRuntime *create_runtime(const JceRuntimeCreateOptions *options, size_t bytes)
{
    JceRuntimeDesc desc = {0};
    desc.scene = test_scene;
    test_runtime = jce_runtime_create_with_options(&desc, NULL, options, bytes);
    return test_runtime;
}
static void read_events(void)
{
    JceTraceCursor cursor;
    jce_trace_cursor_init(&cursor);
    event_count = jce_trace_read(&cursor, events, 2048);
    TEST_ASSERT_LESS_THAN_UINT(2048, event_count);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT(0, stats.active_waits);
}
static uint64_t completed(const char *name, uint64_t parent, JceTraceTaskState state)
{
    uint64_t id = 0;
    unsigned submits = 0, begins = 0, ends = 0;
    for (uint32_t i = 0; i < event_count; ++i) {
        const JceTraceEvent *event = &events[i];
        if (strcmp(event->name, name) != 0) continue;
        if (event->type == JCE_TRACE_EVENT_TASK_SUBMIT) {
            ++submits; id = event->id;
            TEST_ASSERT_EQUAL_UINT64(parent, event->parent_id);
            TEST_ASSERT_EQUAL_UINT(JCE_TRACE_TASK_SERVICE, event->value_u32[0]);
        } else if (event->type == JCE_TRACE_EVENT_TASK_BEGIN) {
            ++begins; TEST_ASSERT_EQUAL_UINT64(id, event->id);
        } else if (event->type == JCE_TRACE_EVENT_TASK_END) {
            ++ends; TEST_ASSERT_EQUAL_UINT64(id, event->id);
            TEST_ASSERT_EQUAL_UINT(state, event->value_u32[0]);
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, submits, name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, begins, name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, ends, name);
    return id;
}
static void failure_reason(uint64_t id, int expected)
{
    unsigned found = 0;
    for (uint32_t i = 0; i < event_count; ++i)
        if (events[i].type == JCE_TRACE_EVENT_COUNTER &&
            strcmp(events[i].name, "runtime.create.failure_reason") == 0) {
            ++found;
            TEST_ASSERT_EQUAL_UINT64(id, events[i].id);
            TEST_ASSERT_EQUAL_INT64(expected, (int64_t)events[i].value_u64);
        }
    TEST_ASSERT_EQUAL_UINT(1, found);
}
static void test_actual_runtime_lifecycle_restores_host_parent_and_closes_children(void)
{
    jce_trace_set_enabled(true);
    JceTraceScope host = jce_trace_scope_begin("fixture.host");
    TEST_ASSERT_NOT_NULL(create_runtime(NULL, 0));
    TEST_ASSERT_EQUAL_UINT64(host.id, jce_trace_task_current_id());
    jce_runtime_step(test_runtime, 1.0f / 60.0f);
    TEST_ASSERT_EQUAL_UINT64(host.id, jce_trace_task_current_id());
    jce_runtime_destroy(test_runtime); test_runtime = NULL;
    TEST_ASSERT_EQUAL_UINT64(host.id, jce_trace_task_current_id());
    jce_trace_scope_end(host, JCE_TRACE_TASK_SUCCEEDED);
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
    read_events();
    completed("runtime.create", host.id, JCE_TRACE_TASK_SUCCEEDED);
    const uint64_t tick = completed("runtime.tick", host.id, JCE_TRACE_TASK_SUCCEEDED);
    completed("runtime.physics", tick, JCE_TRACE_TASK_SUCCEEDED);
    completed("runtime.scene_update", tick, JCE_TRACE_TASK_SUCCEEDED);
    completed("runtime.particles", tick, JCE_TRACE_TASK_SUCCEEDED);
    completed("runtime.gameplay", tick, JCE_TRACE_TASK_SUCCEEDED);
    completed("runtime.destroy", host.id, JCE_TRACE_TASK_SUCCEEDED);
}
static void rejected_create(int reason)
{
    jce_trace_set_enabled(true);
    jce_trace_task_restore_id(7301);
    if (reason == 1) TEST_ASSERT_NULL(jce_runtime_create_with_options(NULL, NULL, NULL, 0));
    if (reason == 2) { fail_next_allocation = true; TEST_ASSERT_NULL(create_runtime(NULL, 0)); }
    if (reason == 3) {
        JceRuntimeCreateOptions options = {0};
        TEST_ASSERT_NULL(create_runtime(&options, 1));
    }
    TEST_ASSERT_EQUAL_UINT64(7301, jce_trace_task_current_id());
    read_events();
    failure_reason(completed("runtime.create", 7301, JCE_TRACE_TASK_FAILED), reason);
}
static void test_invalid_description_closes_failed_create(void) { rejected_create(1); }
static void test_actual_allocation_failure_closes_failed_create(void) { rejected_create(2); }
static void test_actual_options_refusal_closes_failed_create(void) { rejected_create(3); }
static void test_disabled_trace_and_null_calls_add_no_events(void)
{
    jce_trace_task_restore_id(8103);
    TEST_ASSERT_NOT_NULL(create_runtime(NULL, 0));
    jce_runtime_step(test_runtime, .01f);
    jce_runtime_destroy(test_runtime); test_runtime = NULL;
    TEST_ASSERT_EQUAL_UINT64(8103, jce_trace_task_current_id());
    read_events(); TEST_ASSERT_EQUAL_UINT(0, event_count);
    jce_trace_set_enabled(true);
    jce_runtime_step(NULL, .01f); jce_runtime_destroy(NULL);
    read_events(); TEST_ASSERT_EQUAL_UINT(0, event_count);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_actual_runtime_lifecycle_restores_host_parent_and_closes_children);
    RUN_TEST(test_invalid_description_closes_failed_create);
    RUN_TEST(test_actual_allocation_failure_closes_failed_create);
    RUN_TEST(test_actual_options_refusal_closes_failed_create);
    RUN_TEST(test_disabled_trace_and_null_calls_add_no_events);
    return UNITY_END();
}
