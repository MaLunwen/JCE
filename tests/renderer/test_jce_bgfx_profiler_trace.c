/* Invoke the actual production callback vtbl against the real trace ring and
 * JCE OS threads. No renderer is created and no GPU callback is manufactured.
 * This validates CPU callback consumption, not upstream profiler availability. */
#include "unity.h"
#include <jce/os/core/jce_thread.h>
#include "os/core/jce_trace_scope_internal.h"
#include "os/core/jce_trace_capture_internal.h"
#include <string.h>
#include <jce/renderer/jce_render_readback.h>
bool jce_render_readback_format_supported(JceRenderFormat format)
{ (void)format; return false; }


/* Unrelated host hooks in the complete callback translation unit. */
uint32_t jce_rcb_host_frame_index(void) { return 0; }
void jce_renderer_caps_api_capture_trace(const char *line) { (void)line; }

/* Configuration ports vary only generated package metadata, before the
 * complete actual callback owner is compiled. They do not create a device. */
#ifdef JCE_TEST_ANNOTATION_CASE
#undef JCE_BGFX_DEBUG_ANNOTATION_REQUEST
#undef JCE_BGFX_DEBUG_ANNOTATION_OPTION
#undef JCE_BGFX_DEBUG_ANNOTATION_COMPILED
#if JCE_TEST_ANNOTATION_CASE >= 0
#if JCE_TEST_ANNOTATION_CASE == 1
#define JCE_BGFX_DEBUG_ANNOTATION_REQUEST 1
#define JCE_BGFX_DEBUG_ANNOTATION_OPTION 1
#else
#define JCE_BGFX_DEBUG_ANNOTATION_REQUEST 0
#define JCE_BGFX_DEBUG_ANNOTATION_OPTION 0
#endif
#if JCE_TEST_ANNOTATION_CASE == 0
#define JCE_BGFX_DEBUG_ANNOTATION_COMPILED 0
#else
#define JCE_BGFX_DEBUG_ANNOTATION_COMPILED 1
#endif
#endif
#endif

#ifndef JCE_BGFX_PROFILER_CALLBACK_OWNER
#define JCE_BGFX_PROFILER_CALLBACK_OWNER "renderer/jce_renderer_bgfx_callback.c"
#endif
#include JCE_BGFX_PROFILER_CALLBACK_OWNER

static bgfx_callback_interface_t *callback;
static JceTraceEvent events[512];
static uint32_t event_count;
static JceSemaphore *worker_entered, *worker_release;
static JceThread *worker;

static void begin(const char *name)
{
    callback->vtbl->profiler_begin(callback, name, 0xff00ffffu,
                                    "callback-fixture.c", 17);
}
static void literal(const char *name)
{
    callback->vtbl->profiler_begin_literal(callback, name, 0xff00ffffu,
                                            "callback-fixture.c", 18);
}
static void end(void) { callback->vtbl->profiler_end(callback); }

static void drain(void)
{
    JceTraceCursor cursor;
    jce_trace_cursor_init(&cursor);
    event_count = jce_trace_read(&cursor, events, 512);
    TEST_ASSERT_TRUE(event_count < 512);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_waits);
}

static const JceTraceEvent *find(JceTraceEventType type, const char *name)
{
    for (uint32_t i = 0; i < event_count; ++i)
        if (events[i].type == type && strcmp(events[i].name, name) == 0)
            return &events[i];
    TEST_FAIL_MESSAGE(name);
    return NULL;
}

static unsigned count(JceTraceEventType type, const char *name)
{
    unsigned result = 0;
    for (uint32_t i = 0; i < event_count; ++i)
        if (events[i].type == type && (!name || strcmp(events[i].name, name) == 0))
            ++result;
    return result;
}

void setUp(void)
{
    jce_trace_set_enabled(true);
    jce_trace_reset();
    callback = jce_rcb_callback_interface();
    /* This CPU-only fixture has no live backend producer. Begin assertions
     * after the completed init-attempt capability publication. */
    jce_trace_reset();
    worker = NULL;
    worker_entered = jce_semaphore_create(0);
    worker_release = jce_semaphore_create(0);
    TEST_ASSERT_NOT_NULL(worker_entered);
    TEST_ASSERT_NOT_NULL(worker_release);
    event_count = 0;
}

void tearDown(void)
{
    if (worker) {
        jce_semaphore_signal(worker_release);
        jce_thread_join(worker);
        worker = NULL;
    }
    jce_semaphore_destroy(worker_entered);
    jce_semaphore_destroy(worker_release);
    jce_trace_set_enabled(false);
}

static void test_actual_callbacks_nest_copy_names_and_restore_parent(void)
{
    char dynamic_name[] = "dynamic.upload";
    JceTraceScope host = jce_trace_scope_begin("fixture.host");
    begin(dynamic_name);
    uint64_t outer = jce_trace_task_current_id();
    literal("literal.draw");
    uint64_t inner = jce_trace_task_current_id();
    memset(dynamic_name, 'x', sizeof(dynamic_name) - 1);
    end();
    uint64_t after_inner = jce_trace_task_current_id();
    end();
    uint64_t after_outer = jce_trace_task_current_id();
    jce_trace_scope_end(host, JCE_TRACE_TASK_SUCCEEDED);
    drain();
    TEST_ASSERT_NOT_EQUAL(host.id, outer);
    TEST_ASSERT_NOT_EQUAL(outer, inner);
    TEST_ASSERT_EQUAL_UINT64(outer, after_inner);
    TEST_ASSERT_EQUAL_UINT64(host.id, after_outer);
    TEST_ASSERT_EQUAL_UINT64(host.id,
        find(JCE_TRACE_EVENT_TASK_SUBMIT, "bgfx.cpu.dynamic.upload")->parent_id);
    TEST_ASSERT_EQUAL_UINT64(outer,
        find(JCE_TRACE_EVENT_TASK_SUBMIT, "bgfx.cpu.literal.draw")->parent_id);
    TEST_ASSERT_EQUAL_UINT64(outer,
        find(JCE_TRACE_EVENT_TASK_END, "bgfx.cpu.dynamic.upload")->id);
    TEST_ASSERT_EQUAL_UINT64(inner,
        find(JCE_TRACE_EVENT_TASK_END, "bgfx.cpu.literal.draw")->id);
    TEST_ASSERT_EQUAL_UINT64(jce_thread_current_id(),
        find(JCE_TRACE_EVENT_TASK_BEGIN, "bgfx.cpu.literal.draw")->thread_id);
    TEST_ASSERT_EQUAL_UINT32(0, count(JCE_TRACE_EVENT_THREAD_REGISTER, NULL));
}

typedef struct WorkerResult {
    uint64_t thread_id, outer, inner, restored;
} WorkerResult;

static void worker_callbacks(void *argument)
{
    WorkerResult *result = (WorkerResult *)argument;
    result->thread_id = jce_thread_current_id();
    begin("worker.outer");
    result->outer = jce_trace_task_current_id();
    jce_semaphore_signal(worker_entered);
    jce_semaphore_wait(worker_release);
    literal("worker.inner");
    result->inner = jce_trace_task_current_id();
    end();
    end();
    result->restored = jce_trace_task_current_id();
}

static void test_real_worker_has_independent_stack_and_real_thread_identity(void)
{
    WorkerResult result = {0};
    worker = jce_thread_create(worker_callbacks, &result, "fixture.bgfx.worker");
    TEST_ASSERT_NOT_NULL(worker);
    bool entered = jce_semaphore_wait_timeout(worker_entered, 2000);
    begin("main.outer");
    uint64_t main_scope = jce_trace_task_current_id();
    end();
    jce_semaphore_signal(worker_release);
    jce_thread_join(worker);
    worker = NULL;
    drain();
    TEST_ASSERT_TRUE(entered);
    TEST_ASSERT_NOT_EQUAL(jce_thread_current_id(), result.thread_id);
    TEST_ASSERT_NOT_EQUAL(0, result.outer);
    TEST_ASSERT_NOT_EQUAL(result.outer, main_scope);
    TEST_ASSERT_NOT_EQUAL(result.outer, result.inner);
    TEST_ASSERT_EQUAL_UINT64(0, result.restored);
    TEST_ASSERT_EQUAL_UINT64(result.thread_id,
        find(JCE_TRACE_EVENT_TASK_BEGIN, "bgfx.cpu.worker.outer")->thread_id);
    TEST_ASSERT_EQUAL_UINT64(result.thread_id,
        find(JCE_TRACE_EVENT_TASK_END, "bgfx.cpu.worker.inner")->thread_id);
    TEST_ASSERT_EQUAL_UINT64(jce_thread_current_id(),
        find(JCE_TRACE_EVENT_TASK_BEGIN, "bgfx.cpu.main.outer")->thread_id);
    TEST_ASSERT_EQUAL_UINT64(result.outer,
        find(JCE_TRACE_EVENT_TASK_SUBMIT, "bgfx.cpu.worker.inner")->parent_id);
    TEST_ASSERT_EQUAL_UINT32(1, count(JCE_TRACE_EVENT_THREAD_REGISTER, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, count(JCE_TRACE_EVENT_THREAD_UNREGISTER, NULL));
}

static void test_depth_overflow_does_not_pop_admitted_parent(void)
{
    JceTraceScope host = jce_trace_scope_begin("fixture.host");
    /* The private bridge admits 64 fixed entries and skips deeper nesting. */
    for (unsigned i = 0; i < 80; ++i) begin("overflow");
    uint64_t last_admitted = jce_trace_task_current_id();
    for (unsigned i = 0; i < 16; ++i) end();
    uint64_t after_skipped = jce_trace_task_current_id();
    for (unsigned i = 0; i < 64; ++i) end();
    uint64_t restored = jce_trace_task_current_id();
    jce_trace_scope_end(host, JCE_TRACE_TASK_SUCCEEDED);
    drain();
    TEST_ASSERT_NOT_EQUAL(host.id, last_admitted);
    TEST_ASSERT_EQUAL_UINT64(last_admitted, after_skipped);
    TEST_ASSERT_EQUAL_UINT64(host.id, restored);
    TEST_ASSERT_EQUAL_UINT32(64,
        count(JCE_TRACE_EVENT_TASK_SUBMIT, "bgfx.cpu.overflow"));
    TEST_ASSERT_EQUAL_UINT32(64,
        count(JCE_TRACE_EVENT_TASK_END, "bgfx.cpu.overflow"));
    TEST_ASSERT_GREATER_THAN_UINT32(0,
        count(JCE_TRACE_EVENT_COUNTER, "bgfx.cpu.stack_overflow"));
}

static void test_complete_disabled_callbacks_emit_nothing(void)
{
    jce_trace_set_enabled(false);
    begin("off.dynamic");
    literal("off.literal");
    end(); end(); end();
    drain();
    TEST_ASSERT_EQUAL_UINT32(0, event_count);
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
    /* Re-enable only once all producers are quiescent. */
    jce_trace_set_enabled(true);
    begin("after.off"); end();
    drain();
    TEST_ASSERT_EQUAL_UINT32(1,
        count(JCE_TRACE_EVENT_TASK_END, "bgfx.cpu.after.off"));
}

static void test_nested_suppression_does_not_close_visible_parent(void)
{
    begin("visible.outer");
    uint64_t outer = jce_trace_task_current_id();
    bool suppressed = jce_trace_suppress_begin();
    begin("hidden.inner"); literal("hidden.literal");
    end(); end();
    bool unsuppressed = jce_trace_suppress_end();
    uint64_t restored = jce_trace_task_current_id();
    end();
    drain();
    TEST_ASSERT_TRUE(suppressed);
    TEST_ASSERT_TRUE(unsuppressed);
    TEST_ASSERT_NOT_EQUAL(0, outer);
    TEST_ASSERT_EQUAL_UINT64(outer, restored);
    TEST_ASSERT_EQUAL_UINT32(1,
        count(JCE_TRACE_EVENT_TASK_END, "bgfx.cpu.visible.outer"));
    TEST_ASSERT_EQUAL_UINT32(0,
        count(JCE_TRACE_EVENT_TASK_SUBMIT, "bgfx.cpu.hidden.inner"));
}

static void test_empty_null_long_names_and_unmatched_end_are_bounded(void)
{
    char name[160]; memset(name, 'a', sizeof(name)); name[159] = '\0';
    end();
    begin(NULL); end();
    literal(""); end();
    begin(name); end();
    end();
    drain();
    TEST_ASSERT_EQUAL_UINT32(3, count(JCE_TRACE_EVENT_TASK_END, NULL));
    TEST_ASSERT_EQUAL_UINT32(2,
        count(JCE_TRACE_EVENT_TASK_END, "bgfx.cpu.unnamed"));
    for (uint32_t i = 0; i < event_count; ++i) {
        TEST_ASSERT_EQUAL_CHAR('\0', events[i].name[JCE_TRACE_NAME_CAP - 1]);
        TEST_ASSERT_EQUAL_UINT64(jce_thread_current_id(), events[i].thread_id);
    }
}

static void test_witness_separates_build_metadata_trace_enable_and_observed_callbacks(void)
{
    JceBgfxCpuProfilerWitness before = jce_rcb_cpu_profiler_witness();
#if defined(JCE_BGFX_CPU_PROFILER_COMPILED) && (JCE_BGFX_CPU_PROFILER_COMPILED == 1)
    const int compiled = 1;
#elif defined(JCE_BGFX_CPU_PROFILER_COMPILED) && (JCE_BGFX_CPU_PROFILER_COMPILED == 0)
    const int compiled = 0;
#else
    const int compiled = -1;
#endif
    jce_trace_set_enabled(false);
    begin("off.observed"); literal("off.literal.observed"); end(); end();
    JceBgfxCpuProfilerWitness observed = jce_rcb_cpu_profiler_witness();
    jce_rcb_after_backend_shutdown();
    JceBgfxCpuProfilerWitness retired = jce_rcb_cpu_profiler_witness();
    drain();
    TEST_ASSERT_EQUAL_INT(compiled, before.compiled);
    TEST_ASSERT_EQUAL_INT(JCE_BGFX_CPU_TLS_AVAILABLE, before.bridge_supported);
    TEST_ASSERT_EQUAL_UINT32(0, before.begin_observed);
    TEST_ASSERT_EQUAL_UINT32(2, observed.begin_observed);
    TEST_ASSERT_EQUAL_UINT32(2, retired.begin_observed);
    TEST_ASSERT_FALSE(observed.begin_count_saturated);
    TEST_ASSERT_EQUAL_UINT32(0, event_count);

    jce_trace_set_enabled(true); /* All callback scopes are already closed. */
    callback = jce_rcb_callback_interface();
    JceBgfxCpuProfilerWitness reset = jce_rcb_cpu_profiler_witness();
    begin("observed"); end();
    jce_rcb_after_backend_shutdown();
    jce_rcb_after_backend_shutdown(); /* The owner publishes retirement once. */
    drain();
    TEST_ASSERT_EQUAL_UINT32(0, reset.begin_observed);
    TEST_ASSERT_EQUAL_UINT32(1, jce_rcb_cpu_profiler_witness().begin_observed);
    TEST_ASSERT_EQUAL_UINT32(1, count(JCE_TRACE_EVENT_TASK_BEGIN,
        "bgfx.cpu.capability.init_attempt"));
    TEST_ASSERT_EQUAL_UINT32(1, count(JCE_TRACE_EVENT_TASK_END,
        "bgfx.cpu.capability.after_backend_shutdown"));
    TEST_ASSERT_EQUAL_UINT32(2,
        count(JCE_TRACE_EVENT_COUNTER, "bgfx.cpu.compiled"));
    TEST_ASSERT_EQUAL_UINT32(2,
        count(JCE_TRACE_EVENT_COUNTER, "bgfx.cpu.begin_observed"));
    TEST_ASSERT_EQUAL_UINT32(2,
        count(JCE_TRACE_EVENT_COUNTER, "bgfx.gpu.annotation.compiled"));
    TEST_ASSERT_EQUAL_UINT32(2,
        count(JCE_TRACE_EVENT_COUNTER, "bgfx.gpu.annotation.request"));
    TEST_ASSERT_EQUAL_UINT32(2,
        count(JCE_TRACE_EVENT_COUNTER, "bgfx.gpu.annotation.option"));
    for (uint32_t i = 0; i < event_count; ++i) {
        if (events[i].type != JCE_TRACE_EVENT_COUNTER) continue;
        TEST_ASSERT_NOT_EQUAL(0, events[i].id);
        TEST_ASSERT_EQUAL_UINT64(events[i].id, events[i].parent_id);
        if (strcmp(events[i].name, "bgfx.cpu.compiled") == 0)
            TEST_ASSERT_EQUAL_INT64(compiled, (int64_t)events[i].value_u64);
        if (strcmp(events[i].name, "bgfx.gpu.annotation.request") == 0) {
#if defined(JCE_BGFX_DEBUG_ANNOTATION_REQUEST) && (JCE_BGFX_DEBUG_ANNOTATION_REQUEST == 1)
            TEST_ASSERT_EQUAL_INT64(1, (int64_t)events[i].value_u64);
#elif defined(JCE_BGFX_DEBUG_ANNOTATION_REQUEST) && (JCE_BGFX_DEBUG_ANNOTATION_REQUEST == 0)
            TEST_ASSERT_EQUAL_INT64(0, (int64_t)events[i].value_u64);
#else
            TEST_ASSERT_EQUAL_INT64(-1, (int64_t)events[i].value_u64);
#endif
        }
        if (strcmp(events[i].name, "bgfx.gpu.annotation.option") == 0) {
#if defined(JCE_BGFX_DEBUG_ANNOTATION_OPTION) && (JCE_BGFX_DEBUG_ANNOTATION_OPTION == 1)
            TEST_ASSERT_EQUAL_INT64(1, (int64_t)events[i].value_u64);
#elif defined(JCE_BGFX_DEBUG_ANNOTATION_OPTION) && (JCE_BGFX_DEBUG_ANNOTATION_OPTION == 0)
            TEST_ASSERT_EQUAL_INT64(0, (int64_t)events[i].value_u64);
#else
            TEST_ASSERT_EQUAL_INT64(-1, (int64_t)events[i].value_u64);
#endif
        }
        if (strcmp(events[i].name, "bgfx.gpu.annotation.compiled") == 0) {
#if defined(JCE_BGFX_DEBUG_ANNOTATION_COMPILED) && (JCE_BGFX_DEBUG_ANNOTATION_COMPILED == 1)
            TEST_ASSERT_EQUAL_INT64(1, (int64_t)events[i].value_u64);
#elif defined(JCE_BGFX_DEBUG_ANNOTATION_COMPILED) && (JCE_BGFX_DEBUG_ANNOTATION_COMPILED == 0)
            TEST_ASSERT_EQUAL_INT64(0, (int64_t)events[i].value_u64);
#else
            TEST_ASSERT_EQUAL_INT64(-1, (int64_t)events[i].value_u64);
#endif
        }
    }
}

static void test_observation_saturates_without_wrapping_to_zero(void)
{
    jce_atomic_i32_store(&g_rcb_host.profiler_begins, INT32_MAX - 1);
    begin("saturate"); literal("saturate"); end(); end();
    JceBgfxCpuProfilerWitness witness = jce_rcb_cpu_profiler_witness();
    drain();
    TEST_ASSERT_EQUAL_UINT32(INT32_MAX, witness.begin_observed);
    TEST_ASSERT_TRUE(witness.begin_count_saturated);
}

static void test_next_attempt_preserves_unfinalized_count_as_failed_not_joined(void)
{
    begin("abandoned"); end();
    callback = jce_rcb_callback_interface();
    drain();
    const JceTraceEvent *retired = find(JCE_TRACE_EVENT_TASK_END,
        "bgfx.cpu.capability.superseded_unfinalized");
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED, retired->value_u32[0]);
    TEST_ASSERT_EQUAL_UINT32(0, count(JCE_TRACE_EVENT_TASK_END,
        "bgfx.cpu.capability.after_backend_shutdown"));
    unsigned prior_counts = 0, next_counts = 0;
    const uint64_t next_id = find(JCE_TRACE_EVENT_TASK_BEGIN,
        "bgfx.cpu.capability.init_attempt")->id;
    for (uint32_t i = 0; i < event_count; ++i) {
        if (events[i].type != JCE_TRACE_EVENT_COUNTER ||
            strcmp(events[i].name, "bgfx.cpu.begin_observed")) continue;
        if (events[i].id == retired->id) {
            ++prior_counts; TEST_ASSERT_EQUAL_UINT64(1, events[i].value_u64);
        }
        if (events[i].id == next_id) {
            ++next_counts; TEST_ASSERT_EQUAL_UINT64(0, events[i].value_u64);
        }
    }
    TEST_ASSERT_EQUAL_UINT(1, prior_counts); TEST_ASSERT_EQUAL_UINT(1, next_counts);
    TEST_ASSERT_EQUAL_UINT32(0, jce_rcb_cpu_profiler_witness().begin_observed);
    jce_rcb_after_backend_shutdown(); jce_rcb_after_backend_shutdown();
    drain();
    TEST_ASSERT_EQUAL_UINT32(1, count(JCE_TRACE_EVENT_TASK_END,
        "bgfx.cpu.capability.superseded_unfinalized"));
    TEST_ASSERT_EQUAL_UINT32(1, count(JCE_TRACE_EVENT_TASK_END,
        "bgfx.cpu.capability.after_backend_shutdown"));
}

static void test_unattempted_cleanup_cannot_publish_successful_terminal(void)
{
    jce_rcb_cpu_profiler_attempt_end(JCE_BGFX_CPU_ATTEMPT_INIT_FAILED);
    jce_trace_reset();
    jce_rcb_after_backend_shutdown(); jce_rcb_after_backend_shutdown();
    drain();
    TEST_ASSERT_EQUAL_UINT32(0, event_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_actual_callbacks_nest_copy_names_and_restore_parent);
    RUN_TEST(test_real_worker_has_independent_stack_and_real_thread_identity);
    RUN_TEST(test_depth_overflow_does_not_pop_admitted_parent);
    RUN_TEST(test_complete_disabled_callbacks_emit_nothing);
    RUN_TEST(test_nested_suppression_does_not_close_visible_parent);
    RUN_TEST(test_empty_null_long_names_and_unmatched_end_are_bounded);
    RUN_TEST(test_witness_separates_build_metadata_trace_enable_and_observed_callbacks);
    RUN_TEST(test_observation_saturates_without_wrapping_to_zero);
    RUN_TEST(test_next_attempt_preserves_unfinalized_count_as_failed_not_joined);
    RUN_TEST(test_unattempted_cleanup_cannot_publish_successful_terminal);
    return UNITY_END();
}
