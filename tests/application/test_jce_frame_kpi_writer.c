/* Real private writer TU; injected allocation/sink providers, no copied queue. */
#include "unity.h"
#include "application/jce_frame_kpi.h"
#include "application/jce_engine_kpi_capture.h"
#include "os/core/jce_memory.h"
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_timer.h>
#include <jce/os/core/jce_timer.h>
#include <string.h>
#include <stdio.h>

static unsigned allocation_calls, fail_allocation;
static void *test_allocate(size_t size)
{
    if (++allocation_calls == fail_allocation) return NULL;
    return jce__malloc_tracked(size);
}
static void *test_allocate_zero(size_t count, size_t size)
{
    if (++allocation_calls == fail_allocation) return NULL;
    return jce__calloc_tracked(count, size);
}
#undef JCE_MALLOC
#undef JCE_CALLOC
#define JCE_MALLOC(size) test_allocate(size)
#define JCE_CALLOC(count, size) test_allocate_zero(count, size)
/* Include once to test cancellation/terminal state without a test-only ABI. */
#ifndef JCE_TEST_KPI_WRITER_OWNER
#define JCE_TEST_KPI_WRITER_OWNER "application/jce_frame_kpi_writer.c"
#endif
/* Control only this complete writer TU's clock reads. Executor/thread clocks
 * remain real; deterministic age cases use cooperative work and join first. */
static bool writer_clock_controlled;
static uint64_t writer_clock_ns;
static uint64_t test_writer_ticks_ns(void)
{
    return writer_clock_controlled ? writer_clock_ns : jce_time_ticks_ns();
}
#define jce_time_ticks_ns test_writer_ticks_ns
#include JCE_TEST_KPI_WRITER_OWNER
#undef jce_time_ticks_ns

typedef struct {
    char bytes[65536];
    size_t size;
    SDL_AtomicInt writes, flushes, closes, entered, release, flush_entered;
    unsigned short_call, short_bytes, failed_flush, delay_ms;
    bool block_data, block_flush, failed_close, bad_write_status, bad_flush_status;
    bool attempt_nested, nested_poll, nested_finish, nested_destroy, nested_active;
    uint64_t data_thread_id, owner_thread_id;
    unsigned data_callback_count;
    size_t flush_sizes[8];
    bool owner_io_violation; /* Latched OR over every data callback. */
    JceFrameKpiWriter *writer;
} Sink;
static size_t SDLCALL sink_write(void *user, const void *bytes, size_t size, SDL_IOStatus *status)
{
    Sink *sink = user;
    unsigned call = (unsigned)SDL_AddAtomicInt(&sink->writes, 1) + 1u;
    if (call > 1u) {
        sink->data_thread_id = jce_thread_current_id();
        ++sink->data_callback_count;
        sink->owner_io_violation = sink->owner_io_violation ||
            (sink->owner_thread_id && sink->data_thread_id == sink->owner_thread_id);
        SDL_SetAtomicInt(&sink->entered, 1);
        if (sink->attempt_nested) {
            JceFrameKpiWriterStatus nested;
            jce_frame_kpi_writer_get_status(sink->writer, &nested);
            sink->nested_active = nested.owner_operation_active;
            sink->nested_poll = jce_frame_kpi_writer_poll(sink->writer);
            sink->nested_finish = jce_frame_kpi_writer_finish(sink->writer);
            sink->nested_destroy = jce_frame_kpi_writer_destroy(sink->writer);
        }
        while (sink->block_data && !SDL_GetAtomicInt(&sink->release)) SDL_Delay(1);
        if (sink->delay_ms) SDL_Delay(sink->delay_ms);
    }
    size_t copied = size;
    if (call == sink->short_call) copied = sink->short_bytes < size ? sink->short_bytes : 0;
    if (copied >= sizeof sink->bytes - sink->size) copied = 0;
    memcpy(sink->bytes + sink->size, bytes, copied);
    sink->size += copied; sink->bytes[sink->size] = 0;
    *status = copied == size && !(call > 1u && sink->bad_write_status)
        ? SDL_IO_STATUS_READY : SDL_IO_STATUS_ERROR;
    if (copied != size) SDL_SetError("Injected incomplete sink write");
    return copied;
}
static bool SDLCALL sink_flush(void *user, SDL_IOStatus *status)
{
    Sink *sink = user;
    unsigned call = (unsigned)SDL_AddAtomicInt(&sink->flushes, 1) + 1u;
    if (call <= 8) sink->flush_sizes[call - 1u] = sink->size;
    if (call > 1u && sink->block_flush) {
        SDL_SetAtomicInt(&sink->flush_entered, 1);
        while (!SDL_GetAtomicInt(&sink->release)) SDL_Delay(1);
    }
    bool ok = call != sink->failed_flush;
    *status = ok && !(call > 1u && sink->bad_flush_status)
        ? SDL_IO_STATUS_READY : SDL_IO_STATUS_ERROR;
    if (!ok) SDL_SetError("Injected sink flush failure");
    return ok;
}
static bool SDLCALL sink_close(void *user)
{
    Sink *sink = user;
    SDL_AddAtomicInt(&sink->closes, 1);
    if (sink->failed_close) SDL_SetError("Injected sink close failure");
    return !sink->failed_close;
}
static SDL_IOStream *stream_for(Sink *sink)
{
    SDL_IOStreamInterface api; SDL_INIT_INTERFACE(&api);
    api.write = sink_write; api.flush = sink_flush; api.close = sink_close;
    return SDL_OpenIO(&api, sink);
}
static JceAsyncExecutor *executor_for(JceAsyncExecutionMode mode, uint32_t capacity)
{
    JceAsyncExecutorConfig config; jce_async_executor_config_init(&config);
    config.mode = mode; config.worker_count = 1; config.max_tasks = capacity;
    config.debug_name = "test-frame-kpi-writer";
    return jce_async_executor_create(&config);
}
static JceFrameKpiWriter *batch_writer_for(Sink *sink, JceAsyncExecutor *executor,
    size_t bytes, uint32_t records, uint32_t durable, size_t minimum, uint32_t delay_ms)
{
    JceFrameKpiWriterConfig config; jce_frame_kpi_writer_config_init(&config, true);
    config.stream = stream_for(sink); config.executor = executor;
    config.byte_capacity = bytes; config.record_capacity = records;
    config.drain_bytes = bytes; config.drain_records = records; config.durable_interval = durable;
    config.min_batch_bytes = minimum; config.max_delay_ms = delay_ms;
    JceFrameKpiWriter *writer = jce_frame_kpi_writer_create(&config);
    if (!writer) SDL_CloseIO(config.stream);
    return writer;
}
static JceFrameKpiWriter *writer_for(Sink *sink, JceAsyncExecutor *executor,
                                    size_t bytes, uint32_t records, uint32_t durable)
{
    return batch_writer_for(sink, executor, bytes, records, durable, 0, 0);
}
static void dispose(JceFrameKpiWriter *writer, JceAsyncExecutor *executor)
{
    jce_frame_kpi_writer_destroy(writer);
    jce_async_executor_shutdown(executor, JCE_ASYNC_SHUTDOWN_DRAIN, JCE_ASYNC_WAIT_INFINITE);
    jce_async_executor_destroy(executor);
}
/* Independent finite safety release prevents an inline-submit regression
 * from deadlocking the same owner that normally releases the sink. */
static void safety_release(void *user)
{
    Sink *sink = user;
    for (unsigned index = 0; index < 5000; ++index) {
        if (SDL_GetAtomicInt(&sink->release)) return;
        jce_thread_sleep_ms(1);
    }
    SDL_SetAtomicInt(&sink->release, 1);
}
static bool wait_flag(SDL_AtomicInt *flag)
{
    for (unsigned index = 0; index < 5000; ++index) {
        if (SDL_GetAtomicInt(flag)) return true;
        SDL_Delay(1);
    }
    return false;
}
static bool wait_entered(Sink *sink) { return wait_flag(&sink->entered); }
static bool poll_drained(JceFrameKpiWriter *writer)
{
    JceFrameKpiWriterStatus status;
    for (unsigned index = 0; index < 5000; ++index) {
        bool ok = jce_frame_kpi_writer_poll(writer);
        jce_frame_kpi_writer_get_status(writer, &status);
        if (!ok) return false;
        if (!status.queued_records && !status.task_outstanding) return true;
        SDL_Delay(1);
    }
    return false;
}
void setUp(void) { allocation_calls = fail_allocation = 0; writer_clock_controlled = false; jce_perf_phase_set_enabled(0); }
void tearDown(void) {}

static void test_disabled_has_no_allocation_or_io(void)
{
    JceFrameKpiWriterConfig config; jce_frame_kpi_writer_config_init(&config, true);
    config.enabled = false;
    TEST_ASSERT_NULL(jce_frame_kpi_writer_create(&config));
    TEST_ASSERT_EQUAL_UINT(0, allocation_calls);
}
static void test_allocation_failure_retains_caller_stream(void)
{
    JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
    TEST_ASSERT_NOT_NULL(executor);
    for (unsigned failure = 1; failure <= 3; ++failure) {
        Sink sink = {0};
        JceFrameKpiWriterConfig config; jce_frame_kpi_writer_config_init(&config, true);
        config.stream = stream_for(&sink); config.executor = executor;
        allocation_calls = 0; fail_allocation = failure;
        JceFrameKpiWriter *writer = jce_frame_kpi_writer_create(&config);
        bool caller_closed = SDL_CloseIO(config.stream);
        TEST_ASSERT_NULL(writer);
        TEST_ASSERT_EQUAL_INT(0, SDL_GetAtomicInt(&sink.writes));
        TEST_ASSERT_TRUE(caller_closed);
    }
    fail_allocation = 0;
    dispose(NULL, executor);
}
static void test_header_write_and_flush_failure_never_complete(void)
{
    for (unsigned which = 0; which < 2; ++which) {
        Sink sink = {0}; sink.short_call = which == 0 ? 1 : 0; sink.failed_flush = which == 1 ? 1 : 0;
        JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
        JceFrameKpiWriter *writer = writer_for(&sink, executor, 64, 4, 0);
        TEST_ASSERT_NOT_NULL(writer);
        bool completed = jce_frame_kpi_writer_finish(writer);
        JceFrameKpiWriterStatus status; jce_frame_kpi_writer_get_status(writer, &status);
        dispose(writer, executor);
        TEST_ASSERT_FALSE(completed);
        TEST_ASSERT_TRUE(status.closed);
        TEST_ASSERT_EQUAL_INT(which ? JCE_KPI_WRITER_ERROR_HEADER_FLUSH : JCE_KPI_WRITER_ERROR_HEADER_WRITE, status.error);
        TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&sink.closes));
    }
}
static void test_native_delayed_sink_never_runs_inside_submit_and_owns_bytes(void)
{
    Sink sink = {0}; sink.block_data = true; sink.delay_ms = 20;
    JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_THREADED, 8);
    JceFrameKpiWriter *writer = writer_for(&sink, executor, 128, 8, 0);
    TEST_ASSERT_NOT_NULL(writer);
    JceFrameKpiWriterStatus status; jce_frame_kpi_writer_get_status(writer, &status);
    if (!status.off_thread) { dispose(writer, executor); TEST_FAIL_MESSAGE("Explicit native THREADED mode unavailable; this cannot be a passing ignore"); }
    uint64_t owner_thread = jce_thread_current_id();
    sink.owner_thread_id = owner_thread;
    JceThread *safety = jce_thread_create(safety_release, &sink, "kpi-test-safety");
    if (!safety) { sink.block_data = false; dispose(writer, executor); TEST_FAIL_MESSAGE("Independent finite safety release unavailable"); }
    char first[] = "0,frame_wall,1.0000\n";
    bool submitted = jce_frame_kpi_writer_submit(writer, 0, first, sizeof first - 1);
    bool entered = wait_entered(&sink);
    memset(first, 'x', sizeof first - 1);
    bool second = jce_frame_kpi_writer_submit(writer, 1, "1,frame_wall,2.0000\n", 20);
    jce_frame_kpi_writer_get_status(writer, &status);
    SDL_SetAtomicInt(&sink.release, 1); /* Always unblock before any assertions. */
    bool completed = jce_frame_kpi_writer_finish(writer);
    jce_thread_join(safety);
    dispose(writer, executor);
    TEST_ASSERT_TRUE(submitted); TEST_ASSERT_TRUE(entered); TEST_ASSERT_TRUE(second);
    TEST_ASSERT_NOT_EQUAL(owner_thread, sink.data_thread_id);
    TEST_ASSERT_FALSE(sink.owner_io_violation);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(2, sink.data_callback_count);
    TEST_ASSERT_EQUAL_UINT(2, status.accepted_records);
    TEST_ASSERT_TRUE(status.task_outstanding);
    TEST_ASSERT_TRUE(completed);
    TEST_ASSERT_NOT_NULL(strstr(sink.bytes, "0,frame_wall,1.0000\n1,frame_wall,2.0000\n"));
}
static void test_threaded_blocked_sink_observes_real_running_write_and_strict_full(void)
{
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        bool overflow = scenario != 0, wrapped = scenario == 2;
        Sink sink = {0};
        sink.owner_thread_id = jce_thread_current_id();
        JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_THREADED, 8);
        JceFrameKpiWriter *writer = writer_for(&sink, executor, 8, 8, 0);
        TEST_ASSERT_NOT_NULL(writer);
        bool primed = !wrapped ||
            (jce_frame_kpi_writer_submit(writer, 0, "123456", 6) && poll_drained(writer));
        uint64_t base = wrapped ? 1u : 0u;
        SDL_SetAtomicInt(&sink.entered, 0); sink.block_data = true;
        JceThread *safety = jce_thread_create(safety_release, &sink, "kpi-sink-safety");
        if (!safety) { sink.block_data = false; dispose(writer, executor); TEST_FAIL_MESSAGE("Sink safety release unavailable"); }
        bool first = jce_frame_kpi_writer_submit(writer, base, "aaaa", 4);
        bool entered = wait_entered(&sink);
        bool second = jce_frame_kpi_writer_submit(writer, base + 1u, "bbbb", 4);
        bool rejected = !overflow || !jce_frame_kpi_writer_submit(writer, base + 2u, "x", 1);
        JceFrameKpiWriterStatus before; jce_frame_kpi_writer_get_status(writer, &before);
        SDL_SetAtomicInt(&sink.release, 1);
        bool finished = jce_frame_kpi_writer_finish(writer);
        JceFrameKpiWriterStatus after; jce_frame_kpi_writer_get_status(writer, &after);
        jce_thread_join(safety); dispose(writer, executor);
        TEST_ASSERT_TRUE(primed); TEST_ASSERT_TRUE(first); TEST_ASSERT_TRUE(entered); TEST_ASSERT_TRUE(second);
        TEST_ASSERT_TRUE(rejected); TEST_ASSERT_TRUE(before.off_thread);
        TEST_ASSERT_FALSE(sink.owner_io_violation);
        TEST_ASSERT_EQUAL_UINT(8, before.queued_bytes);
        TEST_ASSERT_EQUAL_UINT(2, before.queued_records);
        TEST_ASSERT_EQUAL_UINT64(wrapped ? 14u : 8u, before.accepted_bytes);
        TEST_ASSERT_TRUE(before.write.active); TEST_ASSERT_TRUE(before.write.data);
        TEST_ASSERT_FALSE(before.write.return_available);
        TEST_ASSERT_EQUAL_UINT64(base + 2u, before.write.serial);
        TEST_ASSERT_EQUAL_UINT64(base + 1u, before.write_calls);
        TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&sink.closes));
        TEST_ASSERT_FALSE(after.write.active); TEST_ASSERT_TRUE(after.write.return_available);
        TEST_ASSERT_GREATER_THAN_UINT64(0, after.write.started_ns);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT64(after.write.started_ns, after.write.returned_ns);
        if (!overflow) {
            TEST_ASSERT_TRUE(finished); TEST_ASSERT_FALSE(after.first_failure.captured);
            TEST_ASSERT_EQUAL_UINT64(8, after.written_bytes);
            TEST_ASSERT_EQUAL_UINT(0, after.queued_bytes);
            TEST_ASSERT_EQUAL_UINT64(after.submitted_tasks, after.started_tasks);
            TEST_ASSERT_EQUAL_UINT64(after.started_tasks, after.completion_callbacks);
        } else {
            const JceFrameKpiWriterFailure *f = &before.first_failure;
            TEST_ASSERT_FALSE(finished); TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_ERROR_FULL, after.error);
            TEST_ASSERT_TRUE(f->captured); TEST_ASSERT_TRUE(f->task_state_available);
            TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_RUNNING, f->task_state);
            TEST_ASSERT_EQUAL_UINT(8, f->queued_bytes); TEST_ASSERT_EQUAL_UINT(2, f->queued_records);
            TEST_ASSERT_EQUAL_UINT64(base + 1u, f->submitted_tasks);
            TEST_ASSERT_EQUAL_UINT64(base + 1u, f->started_tasks);
            TEST_ASSERT_EQUAL_UINT64(base, f->completion_callbacks);
            TEST_ASSERT_TRUE(f->write.active); TEST_ASSERT_FALSE(f->write.return_available);
            TEST_ASSERT_EQUAL_UINT(wrapped ? 2u : 4u, f->write.requested_bytes);
            TEST_ASSERT_EQUAL_UINT64(0, f->write.returned_ns); /* Explicitly unavailable. */
            TEST_ASSERT_EQUAL_UINT64(f->observed_ns, after.first_failure.observed_ns);
            TEST_ASSERT_TRUE(after.first_failure.write.active); /* Frozen after real return. */
            TEST_ASSERT_EQUAL_UINT64(base, after.first_failure.completion_callbacks);
            TEST_ASSERT_EQUAL_UINT64(base + 1u, after.completion_callbacks);
            TEST_ASSERT_TRUE(after.failure_write_return.return_available);
            TEST_ASSERT_FALSE(after.failure_write_return.active);
            TEST_ASSERT_EQUAL_UINT64(f->write.serial, after.failure_write_return.serial);
            TEST_ASSERT_EQUAL_UINT64(f->write.started_ns, after.failure_write_return.started_ns);
            TEST_ASSERT_GREATER_OR_EQUAL_UINT64(f->write.started_ns,
                                              after.failure_write_return.returned_ns);
            TEST_ASSERT_EQUAL_UINT(wrapped ? 2u : 4u, after.failure_write_return.actual_bytes);
            TEST_ASSERT_EQUAL_INT(SDL_IO_STATUS_READY, after.failure_write_return.io_status);
            if (wrapped) TEST_ASSERT_EQUAL_UINT64(f->write.serial + 1u, after.write.serial);
        }
    }
}
static JceAsyncRunResult occupy_worker(JceAsyncContext *ctx, void *user)
{
    (void)ctx; Sink *gate = user;
    SDL_SetAtomicInt(&gate->entered, 1);
    while (!SDL_GetAtomicInt(&gate->release)) SDL_Delay(1);
    return JCE_ASYNC_RUN_SUCCESS;
}
static void test_threaded_occupied_worker_observes_real_queued_task_and_strict_full(void)
{
    for (unsigned overflow = 0; overflow < 2; ++overflow) {
        Sink sink = {0}, gate = {0}; sink.owner_thread_id = jce_thread_current_id();
        JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_THREADED, 8);
        JceFrameKpiWriter *writer = writer_for(&sink, executor, 8, 8, 0);
        TEST_ASSERT_NOT_NULL(writer);
        JceThread *safety = jce_thread_create(safety_release, &gate, "kpi-worker-safety");
        if (!safety) { dispose(writer, executor); TEST_FAIL_MESSAGE("Worker safety release unavailable"); }
        JceAsyncTaskDesc desc; jce_async_task_desc_init(&desc);
        desc.work = occupy_worker; desc.user_data = &gate; desc.priority = JCE_ASYNC_PRIORITY_HIGH;
        JceAsyncTask *blocker = jce_async_submit(executor, &desc);
        bool entered = blocker && wait_entered(&gate);
        bool first = jce_frame_kpi_writer_submit(writer, 0, "aaaa", 4);
        bool second = jce_frame_kpi_writer_submit(writer, 1, "bbbb", 4);
        bool rejected = !overflow || !jce_frame_kpi_writer_submit(writer, 2, "x", 1);
        JceFrameKpiWriterStatus before; jce_frame_kpi_writer_get_status(writer, &before);
        SDL_SetAtomicInt(&gate.release, 1);
        bool finished = jce_frame_kpi_writer_finish(writer);
        JceFrameKpiWriterStatus after; jce_frame_kpi_writer_get_status(writer, &after);
        jce_thread_join(safety);
        if (blocker) jce_async_task_release(blocker);
        dispose(writer, executor);
        TEST_ASSERT_NOT_NULL(blocker); TEST_ASSERT_TRUE(entered); TEST_ASSERT_TRUE(first);
        TEST_ASSERT_TRUE(second); TEST_ASSERT_TRUE(rejected); TEST_ASSERT_TRUE(before.off_thread);
        TEST_ASSERT_EQUAL_UINT(8, before.queued_bytes); TEST_ASSERT_EQUAL_UINT(2, before.queued_records);
        TEST_ASSERT_EQUAL_UINT64(1, before.submitted_tasks);
        TEST_ASSERT_EQUAL_UINT64(0, before.started_tasks);
        TEST_ASSERT_EQUAL_UINT64(0, before.completion_callbacks);
        TEST_ASSERT_FALSE(before.write.active); TEST_ASSERT_FALSE(before.write.data);
        TEST_ASSERT_TRUE(before.write.return_available); /* Only the actual header returned. */
        TEST_ASSERT_EQUAL_UINT64(1, before.write.serial);
        TEST_ASSERT_EQUAL_UINT64(0, before.written_records);
        TEST_ASSERT_FALSE(sink.owner_io_violation);
        TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&sink.closes));
        if (!overflow) {
            TEST_ASSERT_TRUE(finished); TEST_ASSERT_FALSE(after.first_failure.captured);
            TEST_ASSERT_EQUAL_UINT64(8, after.written_bytes); TEST_ASSERT_EQUAL_UINT(0, after.queued_bytes);
            TEST_ASSERT_EQUAL_UINT64(1, after.submitted_tasks);
            TEST_ASSERT_EQUAL_UINT64(1, after.started_tasks);
            TEST_ASSERT_EQUAL_UINT64(1, after.completion_callbacks);
        } else {
            const JceFrameKpiWriterFailure *f = &before.first_failure;
            TEST_ASSERT_FALSE(finished); TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_ERROR_FULL, after.error);
            TEST_ASSERT_TRUE(f->captured); TEST_ASSERT_TRUE(f->task_state_available);
            TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_QUEUED, f->task_state);
            TEST_ASSERT_EQUAL_UINT(8, f->queued_bytes); TEST_ASSERT_EQUAL_UINT(2, f->queued_records);
            TEST_ASSERT_EQUAL_UINT64(1, f->submitted_tasks); TEST_ASSERT_EQUAL_UINT64(0, f->started_tasks);
            TEST_ASSERT_EQUAL_UINT64(0, f->completion_callbacks);
            TEST_ASSERT_FALSE(f->write.active); TEST_ASSERT_FALSE(f->write.data);
            TEST_ASSERT_EQUAL_UINT64(f->observed_ns, after.first_failure.observed_ns);
            TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_QUEUED, after.first_failure.task_state);
            TEST_ASSERT_EQUAL_UINT64(0, after.written_bytes); /* Failed queue never falls back to IO. */
            TEST_ASSERT_FALSE(after.failure_write_return.return_available);
        }
    }
}
static void test_first_failure_occupancy_includes_written_but_unreleased_flush_batch(void)
{
    Sink sink = {0}; sink.block_flush = true;
    JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_THREADED, 8);
    JceFrameKpiWriter *writer = writer_for(&sink, executor, 8, 8, 1);
    TEST_ASSERT_NOT_NULL(writer);
    JceThread *safety = jce_thread_create(safety_release, &sink, "kpi-flush-safety");
    if (!safety) { sink.block_flush = false; dispose(writer, executor); TEST_FAIL_MESSAGE("Flush safety release unavailable"); }
    bool first = jce_frame_kpi_writer_submit(writer, 0, "aaaa", 4);
    bool entered = wait_flag(&sink.flush_entered);
    bool second = jce_frame_kpi_writer_submit(writer, 1, "bbbb", 4);
    bool rejected = !jce_frame_kpi_writer_submit(writer, 2, "x", 1);
    JceFrameKpiWriterStatus before; jce_frame_kpi_writer_get_status(writer, &before);
    SDL_SetAtomicInt(&sink.release, 1);
    bool finished = jce_frame_kpi_writer_finish(writer);
    JceFrameKpiWriterStatus after; jce_frame_kpi_writer_get_status(writer, &after);
    jce_thread_join(safety); dispose(writer, executor);
    TEST_ASSERT_TRUE(first); TEST_ASSERT_TRUE(entered); TEST_ASSERT_TRUE(second);
    TEST_ASSERT_TRUE(rejected); TEST_ASSERT_FALSE(finished);
    TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_ERROR_FULL, after.error);
    TEST_ASSERT_EQUAL_UINT64(8, before.accepted_bytes); TEST_ASSERT_EQUAL_UINT64(4, before.written_bytes);
    TEST_ASSERT_TRUE(before.first_failure.captured);
    TEST_ASSERT_EQUAL_UINT(8, before.first_failure.queued_bytes); /* Not accepted-written == 4. */
    TEST_ASSERT_EQUAL_UINT(2, before.first_failure.queued_records);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_RUNNING, before.first_failure.task_state);
    TEST_ASSERT_FALSE(before.first_failure.write.active);
    TEST_ASSERT_TRUE(before.first_failure.write.return_available);
    TEST_ASSERT_EQUAL_UINT(4, before.first_failure.write.actual_bytes);
    TEST_ASSERT_EQUAL_INT(SDL_IO_STATUS_READY, before.first_failure.write.io_status);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(before.first_failure.write.started_ns,
                                      before.first_failure.write.returned_ns);
    TEST_ASSERT_EQUAL_UINT(8, after.first_failure.queued_bytes);
    TEST_ASSERT_EQUAL_UINT(4, after.queued_bytes);
    TEST_ASSERT_EQUAL_UINT64(before.first_failure.observed_ns, after.first_failure.observed_ns);
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&sink.closes));
}
static void test_real_encoder_queue_output_matches_synchronous_bytes(void)
{
    JceFrameKpiBuffer buffer = {0}; size_t size = 0;
    TEST_ASSERT_TRUE(jce_frame_kpi_encode(&buffer, 0, 0.0, NULL, false, &size));
    TEST_ASSERT_FALSE(buffer.last_profile.sampled);
    TEST_ASSERT_EQUAL_UINT(0, buffer.last_profile.write_calls);
    Sink queued = {0}, sync = {0};
    JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
    JceFrameKpiWriter *writer = writer_for(&queued, executor, 128, 8, 0);
    bool submitted = jce_frame_kpi_writer_submit(writer, 0, buffer.bytes, size);
    SDL_IOStream *direct = stream_for(&sync);
    bool exported = jce_frame_kpi_export(&buffer, direct, 0, 0.0, NULL, false);
    SDL_CloseIO(direct);
    bool completed = jce_frame_kpi_writer_finish(writer);
    JceFrameKpiWriterStatus status; jce_frame_kpi_writer_get_status(writer, &status);
    dispose(writer, executor); jce_frame_kpi_buffer_destroy(&buffer);
    TEST_ASSERT_TRUE(submitted); TEST_ASSERT_TRUE(exported); TEST_ASSERT_TRUE(completed);
    TEST_ASSERT_FALSE(status.off_thread);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_EXECUTION_COOPERATIVE, status.mode);
    TEST_ASSERT_EQUAL_STRING(sync.bytes, queued.bytes + strlen("frame_index,phase,ms\n"));
}
static void test_context_writer_uses_selected_header_and_owned_complete_record(void)
{
    JceFrameKpiContext context = {0}; JceFrameKpiBuffer buffer = {0}; size_t size = 0;
    context.window_present = true;
    context.flags_begin_valid = true;
    context.backend_frame_begin_valid = context.backend_frame_end_valid = true;
    context.backend_frame_begin = 71; context.backend_frame_end = 72;
    TEST_ASSERT_TRUE(jce_frame_kpi_encode_with_context(&buffer, 0, 0.0,
        NULL, false, &context, &size));
    char expected[2048];
    TEST_ASSERT_TRUE(size < sizeof expected);
    memcpy(expected, buffer.bytes, size + 1u);
    Sink sink = {0};
    JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
    JceFrameKpiWriterConfig config; jce_frame_kpi_writer_config_init(&config, true);
    TEST_ASSERT_FALSE(config.context_json);
    config.context_json = true;
    config.stream = stream_for(&sink); config.executor = executor;
    config.byte_capacity = 4096; config.record_capacity = 4;
    config.drain_bytes = 4096; config.drain_records = 1;
    JceFrameKpiWriter *writer = jce_frame_kpi_writer_create(&config);
    TEST_ASSERT_NOT_NULL(writer);
    bool accepted = jce_frame_kpi_writer_submit(writer, 0, buffer.bytes, size);
    memset(buffer.bytes, 'x', size); /* Queue must own the accepted encoder bytes. */
    bool finished = jce_frame_kpi_writer_finish(writer);
    JceFrameKpiWriterStatus status; jce_frame_kpi_writer_get_status(writer, &status);
    dispose(writer, executor); jce_frame_kpi_buffer_destroy(&buffer);
    size_t header_size = strlen(jce_frame_kpi_csv_header(true));
    TEST_ASSERT_TRUE(accepted); TEST_ASSERT_TRUE(finished);
    TEST_ASSERT_EQUAL_MEMORY(jce_frame_kpi_csv_header(true), sink.bytes, header_size);
    TEST_ASSERT_EQUAL_STRING(expected, sink.bytes + header_size);
    TEST_ASSERT_EQUAL_UINT64(size, status.accepted_bytes);
    TEST_ASSERT_EQUAL_UINT64(size, status.written_bytes);
    TEST_ASSERT_EQUAL_UINT64(1, status.accepted_records);
    TEST_ASSERT_EQUAL_UINT64(1, status.written_records);
    TEST_ASSERT_EQUAL_UINT(header_size + size, sink.size);
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&sink.closes));
}
static void test_wrapped_complete_records_preserve_order_and_queue_stats(void)
{
    Sink sink = {0}; char a[48], b[32]; memset(a, 'a', sizeof a); memset(b, 'b', sizeof b);
    JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
    JceFrameKpiWriter *writer = writer_for(&sink, executor, 64, 4, 0);
    bool one = jce_frame_kpi_writer_submit(writer, 0, a, sizeof a);
    bool drained = poll_drained(writer);
    bool two = jce_frame_kpi_writer_submit(writer, 1, b, sizeof b);
    bool completed = jce_frame_kpi_writer_finish(writer);
    JceFrameKpiWriterStatus status; jce_frame_kpi_writer_get_status(writer, &status);
    dispose(writer, executor);
    size_t prefix = strlen("frame_index,phase,ms\n");
    TEST_ASSERT_TRUE(one); TEST_ASSERT_TRUE(drained); TEST_ASSERT_TRUE(two); TEST_ASSERT_TRUE(completed);
    TEST_ASSERT_EQUAL_MEMORY(a, sink.bytes + prefix, sizeof a);
    TEST_ASSERT_EQUAL_MEMORY(b, sink.bytes + prefix + sizeof a, sizeof b);
    TEST_ASSERT_EQUAL_UINT(80, status.accepted_bytes); TEST_ASSERT_EQUAL_UINT(80, status.written_bytes);
    TEST_ASSERT_EQUAL_UINT(2, status.written_records); TEST_ASSERT_EQUAL_UINT(0, status.queued_bytes);
    TEST_ASSERT_EQUAL_UINT(48, status.peak_bytes);
}
static void test_byte_or_descriptor_full_fails_without_blocking_fallback(void)
{
    for (unsigned descriptor_case = 0; descriptor_case < 2; ++descriptor_case) {
        Sink sink = {0};
        JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
        JceFrameKpiWriter *writer = writer_for(&sink, executor, descriptor_case ? 128 : 8,
                                               descriptor_case ? 1 : 8, 0);
        bool first = jce_frame_kpi_writer_submit(writer, 0, "12345678", 8);
        bool overflow = jce_frame_kpi_writer_submit(writer, 1, "x", 1);
        JceFrameKpiWriterStatus status; jce_frame_kpi_writer_get_status(writer, &status);
        bool completed = jce_frame_kpi_writer_finish(writer); dispose(writer, executor);
        TEST_ASSERT_TRUE(first); TEST_ASSERT_FALSE(overflow); TEST_ASSERT_FALSE(completed);
        TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_ERROR_FULL, status.error);
        TEST_ASSERT_EQUAL_UINT(1, status.accepted_records);
        TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&sink.writes)); /* Header only, no fallback. */
    }
}
static void test_executor_rejection_is_a_retained_capture_error(void)
{
    Sink sink = {0}; JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
    JceFrameKpiWriter *writer = writer_for(&sink, executor, 64, 4, 0);
    jce_async_executor_shutdown(executor, JCE_ASYNC_SHUTDOWN_DRAIN, JCE_ASYNC_WAIT_INFINITE);
    bool accepted = jce_frame_kpi_writer_submit(writer, 0, "x", 1);
    bool completed = jce_frame_kpi_writer_finish(writer);
    JceFrameKpiWriterStatus status; jce_frame_kpi_writer_get_status(writer, &status);
    dispose(writer, executor);
    TEST_ASSERT_FALSE(accepted); TEST_ASSERT_FALSE(completed);
    TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_ERROR_SUBMIT, status.error);
    TEST_ASSERT_FALSE(status.task_outstanding);
}
static void test_cancelled_pending_task_is_not_a_successful_drain(void)
{
    Sink sink = {0}; JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
    JceFrameKpiWriter *writer = writer_for(&sink, executor, 64, 4, 0);
    bool accepted = jce_frame_kpi_writer_submit(writer, 0, "x", 1);
    bool cancelled = jce_async_task_cancel(writer->task);
    bool completed = jce_frame_kpi_writer_finish(writer);
    JceFrameKpiWriterStatus status; jce_frame_kpi_writer_get_status(writer, &status);
    dispose(writer, executor);
    TEST_ASSERT_TRUE(accepted); TEST_ASSERT_TRUE(cancelled); TEST_ASSERT_FALSE(completed);
    TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_ERROR_CANCELLED, status.error);
    TEST_ASSERT_EQUAL_UINT(1, status.completion_callbacks);
    TEST_ASSERT_TRUE(status.closed); TEST_ASSERT_FALSE(status.task_outstanding);
}
static void test_partial_wrapped_second_write_never_retries_or_completes(void)
{
    Sink sink = {0}; sink.short_call = 4; sink.short_bytes = 3;
    JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
    JceFrameKpiWriter *writer = writer_for(&sink, executor, 64, 4, 0);
    char first[48], second[32]; memset(first, 'a', sizeof first); memset(second, 'b', sizeof second);
    bool one = jce_frame_kpi_writer_submit(writer, 0, first, sizeof first);
    bool drained = poll_drained(writer);
    bool two = jce_frame_kpi_writer_submit(writer, 1, second, sizeof second);
    bool completed = jce_frame_kpi_writer_finish(writer);
    JceFrameKpiWriterStatus status; jce_frame_kpi_writer_get_status(writer, &status);
    dispose(writer, executor);
    TEST_ASSERT_TRUE(one); TEST_ASSERT_TRUE(drained); TEST_ASSERT_TRUE(two); TEST_ASSERT_FALSE(completed);
    TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_ERROR_WRITE, status.error);
    TEST_ASSERT_EQUAL_UINT(16, status.failed_requested_bytes); TEST_ASSERT_EQUAL_UINT(3, status.failed_actual_bytes);
    TEST_ASSERT_EQUAL_UINT(67, status.written_bytes); TEST_ASSERT_EQUAL_UINT(1, status.written_records);
    TEST_ASSERT_EQUAL_INT(4, SDL_GetAtomicInt(&sink.writes));
}
static void test_durability_and_final_flush_or_close_errors_are_retained(void)
{
    for (unsigned which = 0; which < 3; ++which) {
        Sink sink = {0}; sink.failed_flush = which < 2 ? 2 : 0; sink.failed_close = which == 2;
        JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
        JceFrameKpiWriter *writer = writer_for(&sink, executor, 64, 4, which == 0 ? 1 : 0);
        bool accepted = jce_frame_kpi_writer_submit(writer, 0, "x", 1);
        bool before_finish = which == 0 ? jce_frame_kpi_writer_poll(writer) : true;
        JceFrameKpiWriterStatus before; jce_frame_kpi_writer_get_status(writer, &before);
        bool completed = jce_frame_kpi_writer_finish(writer);
        JceFrameKpiWriterStatus status; jce_frame_kpi_writer_get_status(writer, &status);
        bool repeated = jce_frame_kpi_writer_finish(writer); dispose(writer, executor);
        TEST_ASSERT_TRUE(accepted); TEST_ASSERT_FALSE(completed); TEST_ASSERT_FALSE(repeated);
        if (which == 0) {
            TEST_ASSERT_FALSE(before_finish);
            TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_ERROR_FLUSH, before.error);
            TEST_ASSERT_EQUAL_UINT(2, before.flush_calls);
            TEST_ASSERT_FALSE(before.closed);
        }
        TEST_ASSERT_EQUAL_INT(which == 2 ? JCE_KPI_WRITER_ERROR_CLOSE : JCE_KPI_WRITER_ERROR_FLUSH, status.error);
        TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&sink.closes));
        TEST_ASSERT_TRUE(status.closed);
    }
}
static void test_terminal_work_still_requires_actual_completion_callback(void)
{
    Sink sink = {0}; JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
    JceFrameKpiWriter *writer = writer_for(&sink, executor, 64, 4, 0);
    bool accepted = jce_frame_kpi_writer_submit(writer, 0, "x", 1);
    JceAsyncPumpBudget budget; jce_async_pump_budget_init(&budget);
    budget.max_work_items = 1; budget.max_completions = 0; budget.max_time_us = 0;
    jce_async_executor_pump(executor, &budget);
    bool terminal = jce_async_task_is_terminal(writer->task);
    JceFrameKpiWriterStatus before; jce_frame_kpi_writer_get_status(writer, &before);
    bool completed = jce_frame_kpi_writer_finish(writer);
    JceFrameKpiWriterStatus after; jce_frame_kpi_writer_get_status(writer, &after);
    bool repeated = jce_frame_kpi_writer_finish(writer); dispose(writer, executor);
    TEST_ASSERT_TRUE(accepted); TEST_ASSERT_TRUE(terminal); TEST_ASSERT_TRUE(before.task_outstanding);
    TEST_ASSERT_EQUAL_UINT(0, before.completion_callbacks);
    TEST_ASSERT_TRUE(completed); TEST_ASSERT_TRUE(repeated);
    TEST_ASSERT_EQUAL_UINT(1, after.completion_callbacks); TEST_ASSERT_FALSE(after.task_outstanding);
    TEST_ASSERT_EQUAL_INT(2, SDL_GetAtomicInt(&sink.flushes)); TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&sink.closes));
}
static void test_invalid_record_fails_before_any_data_write(void)
{
    for (unsigned which = 0; which < 3; ++which) {
        Sink sink = {0}; JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
        JceFrameKpiWriter *writer = writer_for(&sink, executor, 8, 2, 0);
        bool accepted = which == 0 ? jce_frame_kpi_writer_submit(writer, 1, "x", 1)
            : which == 1 ? jce_frame_kpi_writer_submit(writer, 0, NULL, 1)
            : jce_frame_kpi_writer_submit(writer, 0, "123456789", 9);
        bool completed = jce_frame_kpi_writer_finish(writer);
        JceFrameKpiWriterStatus status; jce_frame_kpi_writer_get_status(writer, &status);
        dispose(writer, executor);
        TEST_ASSERT_FALSE(accepted); TEST_ASSERT_FALSE(completed);
        TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_ERROR_INVALID_RECORD, status.error);
        TEST_ASSERT_EQUAL_UINT(0, status.accepted_records);
        TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&sink.writes));
    }
}
static void test_explicit_io_error_status_cannot_claim_complete_bytes_or_flush(void)
{
    for (unsigned which = 0; which < 2; ++which) {
        Sink sink = {0}; sink.bad_write_status = which == 0; sink.bad_flush_status = which == 1;
        JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
        JceFrameKpiWriter *writer = writer_for(&sink, executor, 64, 4, 0);
        bool accepted = jce_frame_kpi_writer_submit(writer, 0, "x", 1);
        bool completed = jce_frame_kpi_writer_finish(writer);
        JceFrameKpiWriterStatus status; jce_frame_kpi_writer_get_status(writer, &status);
        dispose(writer, executor);
        TEST_ASSERT_TRUE(accepted); TEST_ASSERT_FALSE(completed);
        TEST_ASSERT_EQUAL_INT(which == 0 ? JCE_KPI_WRITER_ERROR_WRITE : JCE_KPI_WRITER_ERROR_FLUSH, status.error);
        TEST_ASSERT_EQUAL_UINT(1, status.written_bytes);
        TEST_ASSERT_EQUAL_INT(SDL_IO_STATUS_ERROR, status.failed_io_status);
    }
}
static void test_failed_task_pending_never_masks_failure(void)
{
    Sink sink = {0}; sink.short_call = 2;
    JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
    JceFrameKpiWriter *writer = writer_for(&sink, executor, 64, 4, 0);
    bool submitted = jce_frame_kpi_writer_submit(writer, 0, "x", 1);
    JceAsyncPumpBudget budget; jce_async_pump_budget_init(&budget);
    budget.max_work_items = 1; budget.max_completions = 0; budget.max_time_us = 0;
    jce_async_executor_pump(executor, &budget);
    JceFrameKpiWriterStatus before; jce_frame_kpi_writer_get_status(writer, &before);
    bool scheduled = writer_schedule(writer); /* Exact production seam while callback pending. */
    bool polled = jce_frame_kpi_writer_poll(writer);
    bool completed = jce_frame_kpi_writer_finish(writer); dispose(writer, executor);
    TEST_ASSERT_TRUE(submitted); TEST_ASSERT_TRUE(before.task_outstanding);
    TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_ERROR_WRITE, before.error);
    TEST_ASSERT_FALSE(scheduled); TEST_ASSERT_FALSE(polled); TEST_ASSERT_FALSE(completed);
}
static void test_cooperative_sink_reentry_defers_close_and_free_in_both_pumps(void)
{
    for (unsigned external = 0; external < 2; ++external) {
        Sink sink = {0}; sink.attempt_nested = true;
        JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
        JceFrameKpiWriter *writer = writer_for(&sink, executor, 64, 4, 0); sink.writer = writer;
        bool submitted = jce_frame_kpi_writer_submit(writer, 0, "x", 1);
        if (external) jce_async_executor_pump(executor, NULL);
        else jce_frame_kpi_writer_poll(writer);
        bool completed = jce_frame_kpi_writer_finish(writer);
        bool cleaned = jce_frame_kpi_writer_destroy(writer);
        dispose(NULL, executor);
        TEST_ASSERT_TRUE(submitted); TEST_ASSERT_TRUE(sink.nested_active);
        TEST_ASSERT_FALSE(sink.nested_poll); TEST_ASSERT_FALSE(sink.nested_finish);
        TEST_ASSERT_FALSE(sink.nested_destroy); TEST_ASSERT_TRUE(completed); TEST_ASSERT_TRUE(cleaned);
        TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&sink.closes));
    }
}
typedef struct {
    JceFrameKpiWriter *writer;
    bool called, active, polled, finished, destroyed;
} OtherCompletion;
static JceAsyncRunResult other_work(JceAsyncContext *ctx, void *user)
{
    (void)ctx; (void)user; return JCE_ASYNC_RUN_SUCCESS;
}
static void other_complete(JceAsyncTask *task, void *user)
{
    (void)task; OtherCompletion *other = user;
    JceFrameKpiWriterStatus status; jce_frame_kpi_writer_get_status(other->writer, &status);
    other->called = true; other->active = status.owner_operation_active;
    other->polled = jce_frame_kpi_writer_poll(other->writer);
    other->finished = jce_frame_kpi_writer_finish(other->writer);
    other->destroyed = jce_frame_kpi_writer_destroy(other->writer);
}
static void test_shared_executor_other_completion_cannot_close_outer_writer(void)
{
    Sink sink = {0}; JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
    JceFrameKpiWriter *writer = writer_for(&sink, executor, 64, 4, 0);
    bool submitted = jce_frame_kpi_writer_submit(writer, 0, "x", 1);
    OtherCompletion other = {0}; other.writer = writer;
    JceAsyncTaskDesc desc; jce_async_task_desc_init(&desc);
    desc.work = other_work; desc.complete = other_complete; desc.user_data = &other;
    desc.priority = JCE_ASYNC_PRIORITY_HIGH;
    JceAsyncTask *task = jce_async_submit(executor, &desc);
    bool polled = jce_frame_kpi_writer_poll(writer);
    bool completed = jce_frame_kpi_writer_finish(writer);
    /* This test owns the other task ref; callback metadata stays live. */
    for (unsigned index = 0; !other.called && index < 5000; ++index) {
        jce_frame_kpi_writer_poll(writer); /* Guard even after writer is closed. */
        jce_thread_sleep_ms(1);
    }
    if (!other.called) { jce_async_task_discard(task); task = NULL; }
    bool cleaned = jce_frame_kpi_writer_destroy(writer);
    if (task) jce_async_task_release(task);
    dispose(NULL, executor);
    TEST_ASSERT_TRUE(submitted); TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_TRUE(polled); TEST_ASSERT_TRUE(other.called); TEST_ASSERT_TRUE(other.active);
    TEST_ASSERT_FALSE(other.polled); TEST_ASSERT_FALSE(other.finished); TEST_ASSERT_FALSE(other.destroyed);
    TEST_ASSERT_TRUE(completed); TEST_ASSERT_TRUE(cleaned);
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&sink.closes));
}
static void test_finite_record_and_byte_drain_quotas_are_observed(void)
{
    for (unsigned byte_limit = 0; byte_limit < 2; ++byte_limit) {
        Sink sink = {0}; JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
        JceFrameKpiWriterConfig config; jce_frame_kpi_writer_config_init(&config, true);
        config.stream = stream_for(&sink); config.executor = executor;
        config.byte_capacity = 64; config.record_capacity = 8;
        config.drain_records = byte_limit ? 8 : 1; config.drain_bytes = byte_limit ? 2 : 64;
        JceFrameKpiWriter *writer = jce_frame_kpi_writer_create(&config);
        bool submitted = true;
        for (uint64_t frame = 0; frame < 5; ++frame)
            submitted = jce_frame_kpi_writer_submit(writer, frame, "x", 1) && submitted;
        bool polled = jce_frame_kpi_writer_poll(writer);
        JceFrameKpiWriterStatus status; jce_frame_kpi_writer_get_status(writer, &status);
        bool completed = jce_frame_kpi_writer_finish(writer); dispose(writer, executor);
        TEST_ASSERT_TRUE(submitted); TEST_ASSERT_TRUE(polled); TEST_ASSERT_TRUE(completed);
        TEST_ASSERT_EQUAL_UINT(byte_limit ? 2 : 1, status.written_records);
        TEST_ASSERT_EQUAL_UINT(byte_limit ? 3 : 4, status.queued_records);
        TEST_ASSERT_EQUAL_UINT(1, status.completion_callbacks);
    }
}
static void test_encoder_scratch_growth_is_clamped_and_overbudget_fails_without_io(void)
{
    JceFrameKpiBuffer buffer = {0};
    buffer.bytes = jce__malloc_tracked(8); buffer.capacity = 8; buffer.max_capacity = 32;
    TEST_ASSERT_NOT_NULL(buffer.bytes);
    size_t size = 999;
    bool small = jce_frame_kpi_encode(&buffer, 0, 1.0, NULL, false, &size);
    bool grew = buffer.capacity > 8 && buffer.capacity <= buffer.max_capacity;
    bool small_exact = size == strlen("0,frame_wall,1.0000\n") &&
        !memcmp(buffer.bytes, "0,frame_wall,1.0000\n", size);
    size_t before_capacity = buffer.capacity;
    size = 999;
    bool large = jce_frame_kpi_encode(&buffer, UINT64_MAX, 1.0, NULL, false, &size);
    bool unavailable = !buffer.last_profile.sampled && !buffer.last_profile.write_calls;
    Sink sink = {0}; SDL_IOStream *stream = stream_for(&sink);
    bool exported = jce_frame_kpi_export(&buffer, stream, UINT64_MAX, 1.0, NULL, false);
    SDL_CloseIO(stream);
    size_t after_capacity = buffer.capacity;
    jce_frame_kpi_buffer_destroy(&buffer);
    TEST_ASSERT_TRUE(small); TEST_ASSERT_TRUE(grew); TEST_ASSERT_TRUE(small_exact);
    TEST_ASSERT_FALSE(large); TEST_ASSERT_EQUAL_UINT(0, size);
    TEST_ASSERT_TRUE(unavailable); TEST_ASSERT_FALSE(exported);
    TEST_ASSERT_EQUAL_INT(0, SDL_GetAtomicInt(&sink.writes));
    TEST_ASSERT_EQUAL_UINT(before_capacity, after_capacity);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(32, after_capacity);
    TEST_ASSERT_EQUAL_UINT(0, buffer.max_capacity);
}
static void test_single_record_over_drain_target_stays_bounded_and_uses_one_task(void)
{
    Sink sink = {0}; JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
    JceFrameKpiWriterConfig config; jce_frame_kpi_writer_config_init(&config, true);
    config.stream = stream_for(&sink); config.executor = executor;
    config.byte_capacity = 64; config.record_capacity = 4;
    config.drain_bytes = 8; config.drain_records = 1;
    JceFrameKpiWriter *writer = jce_frame_kpi_writer_create(&config);
    const char record[] = "0,long_single_phase_name,1.0000\n";
    size_t length = sizeof record - 1;
    bool submitted = jce_frame_kpi_writer_submit(writer, 0, record, length);
    bool polled = jce_frame_kpi_writer_poll(writer);
    JceFrameKpiWriterStatus status; jce_frame_kpi_writer_get_status(writer, &status);
    bool completed = jce_frame_kpi_writer_finish(writer);
    bool cleaned = jce_frame_kpi_writer_destroy(writer); dispose(NULL, executor);
    printf("KPI_SINGLE_RECORD_BUDGET: bytes=%zu target=%zu capacity=%zu callbacks=%llu\n",
        length, config.drain_bytes, status.byte_capacity,
        (unsigned long long)status.completion_callbacks);
    TEST_ASSERT_GREATER_THAN_UINT(config.drain_bytes, length);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(status.byte_capacity, length);
    TEST_ASSERT_TRUE(submitted); TEST_ASSERT_TRUE(polled); TEST_ASSERT_TRUE(completed); TEST_ASSERT_TRUE(cleaned);
    TEST_ASSERT_EQUAL_UINT(1, status.written_records); TEST_ASSERT_EQUAL_UINT(length, status.written_bytes);
    TEST_ASSERT_EQUAL_UINT(1, status.completion_callbacks); TEST_ASSERT_EQUAL_UINT(0, status.queued_records);
    TEST_ASSERT_EQUAL_UINT(length, status.peak_bytes);
    TEST_ASSERT_EQUAL_UINT(1, sink.data_callback_count);
    TEST_ASSERT_EQUAL_STRING(record, sink.bytes + strlen("frame_index,phase,ms\n"));
}
static void test_low_backlog_dispatch_coalesces_real_tasks_and_physical_writes(void)
{
    Sink sink = {0};
    JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
    JceFrameKpiWriterConfig config; jce_frame_kpi_writer_config_init(&config, true);
    config.stream = stream_for(&sink); config.executor = executor;
    config.byte_capacity = 64; config.record_capacity = 16;
    config.min_batch_bytes = 16; config.max_delay_ms = 8;
    writer_clock_controlled = true; writer_clock_ns = 1000000000u;
    JceFrameKpiWriter *writer = jce_frame_kpi_writer_create(&config);
    TEST_ASSERT_NOT_NULL(writer);
    bool submitted = true, polled = true;
    JceFrameKpiWriterStatus below = {0}, drained = {0}, final = {0};
    for (uint64_t frame = 0; frame < 8; ++frame) {
        submitted = jce_frame_kpi_writer_submit(writer, frame, "abcd", 4) && submitted;
        polled = jce_frame_kpi_writer_poll(writer) && polled;
        if (frame == 2) jce_frame_kpi_writer_get_status(writer, &below);
    }
    jce_frame_kpi_writer_get_status(writer, &drained);
    bool completed = jce_frame_kpi_writer_finish(writer);
    jce_frame_kpi_writer_get_status(writer, &final);
    dispose(writer, executor);
    printf("KPI_BATCH_COUNTS: records=8 tasks=%llu data_writes=%u complete=%d\n",
        (unsigned long long)drained.submitted_tasks, sink.data_callback_count, completed);
    TEST_ASSERT_TRUE(submitted); TEST_ASSERT_TRUE(polled); TEST_ASSERT_TRUE(completed);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0, below.submitted_tasks,
        "Low backlog dispatched before byte threshold or oldest-age budget");
    TEST_ASSERT_EQUAL_UINT64(0, below.written_records);
    TEST_ASSERT_EQUAL_UINT(3, below.queued_records);
    TEST_ASSERT_EQUAL_UINT64(2, drained.submitted_tasks);
    TEST_ASSERT_EQUAL_UINT64(2, drained.started_tasks);
    TEST_ASSERT_EQUAL_UINT64(2, drained.completion_callbacks);
    TEST_ASSERT_EQUAL_UINT(2, sink.data_callback_count);
    TEST_ASSERT_EQUAL_UINT64(8, final.accepted_records);
    TEST_ASSERT_EQUAL_UINT64(8, final.written_records);
    TEST_ASSERT_EQUAL_UINT64(32, final.accepted_bytes);
    TEST_ASSERT_EQUAL_UINT64(32, final.written_bytes);
    TEST_ASSERT_EQUAL_UINT(0, final.queued_records);
    TEST_ASSERT_EQUAL_UINT(2, final.flush_calls);
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&sink.closes));
    TEST_ASSERT_EQUAL_STRING("abcdabcdabcdabcdabcdabcdabcdabcd",
        sink.bytes + strlen("frame_index,phase,ms\n"));
}
static void test_batch_oldest_age_survives_later_submissions_and_resets_after_drain(void)
{
    writer_clock_controlled = true; writer_clock_ns = 1000000000u;
    Sink sink = {0};
    JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
    JceFrameKpiWriter *writer = batch_writer_for(&sink, executor, 128, 8, 0, 64, 8);
    TEST_ASSERT_NOT_NULL(writer);
    bool ok = jce_frame_kpi_writer_submit(writer, 0, "a", 1);
    ok = jce_frame_kpi_writer_poll(writer) && ok;
    writer_clock_ns += 7000000u;
    ok = jce_frame_kpi_writer_submit(writer, 1, "bc", 2) && ok;
    ok = jce_frame_kpi_writer_poll(writer) && ok;
    JceFrameKpiWriterStatus before, first, younger, after;
    jce_frame_kpi_writer_get_status(writer, &before);
    writer_clock_ns += 1000000u;
    ok = jce_frame_kpi_writer_poll(writer) && ok; /* Age schedules; next pump executes. */
    ok = jce_frame_kpi_writer_poll(writer) && ok;
    jce_frame_kpi_writer_get_status(writer, &first);
    writer_clock_ns += 1000000u;
    ok = jce_frame_kpi_writer_submit(writer, 2, "d", 1) && ok;
    writer_clock_ns += 7999999u;
    ok = jce_frame_kpi_writer_poll(writer) && ok;
    jce_frame_kpi_writer_get_status(writer, &younger);
    ++writer_clock_ns;
    ok = jce_frame_kpi_writer_poll(writer) && ok;
    ok = jce_frame_kpi_writer_poll(writer) && ok;
    jce_frame_kpi_writer_get_status(writer, &after);
    bool completed = jce_frame_kpi_writer_finish(writer); dispose(writer, executor);
    TEST_ASSERT_TRUE(ok); TEST_ASSERT_TRUE(completed);
    TEST_ASSERT_EQUAL_UINT64(0, before.submitted_tasks);
    TEST_ASSERT_EQUAL_UINT(2, before.queued_records);
    TEST_ASSERT_EQUAL_UINT64(1, first.submitted_tasks);
    TEST_ASSERT_EQUAL_UINT64(2, first.written_records);
    TEST_ASSERT_EQUAL_UINT(0, first.queued_records);
    TEST_ASSERT_EQUAL_UINT64(1, younger.submitted_tasks);
    TEST_ASSERT_EQUAL_UINT(1, younger.queued_records);
    TEST_ASSERT_EQUAL_UINT64(2, after.submitted_tasks);
    TEST_ASSERT_EQUAL_UINT64(3, after.written_records);
    TEST_ASSERT_EQUAL_UINT(2, sink.data_callback_count);
    TEST_ASSERT_EQUAL_STRING("abcd", sink.bytes + strlen("frame_index,phase,ms\n"));
}
static void test_batch_finish_forces_unaged_tail_and_closes_once(void)
{
    writer_clock_controlled = true; writer_clock_ns = 1000000000u;
    Sink sink = {0};
    JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
    JceFrameKpiWriter *writer = batch_writer_for(&sink, executor, 128, 8, 0, 64, 8);
    TEST_ASSERT_NOT_NULL(writer);
    bool accepted = jce_frame_kpi_writer_submit(writer, 0, "a", 1) &&
                    jce_frame_kpi_writer_submit(writer, 1, "bc", 2);
    bool polled = jce_frame_kpi_writer_poll(writer);
    JceFrameKpiWriterStatus before, after, repeated;
    jce_frame_kpi_writer_get_status(writer, &before);
    bool completed = jce_frame_kpi_writer_finish(writer);
    jce_frame_kpi_writer_get_status(writer, &after);
    bool finished_again = jce_frame_kpi_writer_finish(writer);
    jce_frame_kpi_writer_get_status(writer, &repeated); dispose(writer, executor);
    TEST_ASSERT_TRUE(accepted); TEST_ASSERT_TRUE(polled);
    TEST_ASSERT_TRUE(completed); TEST_ASSERT_TRUE(finished_again);
    TEST_ASSERT_EQUAL_UINT64(0, before.submitted_tasks);
    TEST_ASSERT_EQUAL_UINT64(1, after.submitted_tasks);
    TEST_ASSERT_EQUAL_UINT64(2, after.accepted_records);
    TEST_ASSERT_EQUAL_UINT64(2, after.written_records);
    TEST_ASSERT_EQUAL_UINT64(3, after.written_bytes);
    TEST_ASSERT_EQUAL_UINT64(2, after.flush_calls);
    TEST_ASSERT_EQUAL_UINT64(after.flush_calls, repeated.flush_calls);
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&sink.closes));
    TEST_ASSERT_EQUAL_STRING("abc", sink.bytes + strlen("frame_index,phase,ms\n"));
}
static void test_batch_durable_record_boundaries_dispatch_before_bytes_or_age(void)
{
    writer_clock_controlled = true; writer_clock_ns = 1000000000u;
    Sink sink = {0};
    JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
    JceFrameKpiWriter *writer = batch_writer_for(&sink, executor, 128, 8, 2, 64, 8);
    TEST_ASSERT_NOT_NULL(writer);
    bool ok = true; JceFrameKpiWriterStatus first = {0}, second = {0}, tail = {0}, final;
    const char *records[] = {"a", "bc", "d", "ef", "g"};
    for (uint64_t index = 0; index < 5; ++index) {
        ok = jce_frame_kpi_writer_submit(writer, index, records[index], strlen(records[index])) && ok;
        ok = jce_frame_kpi_writer_poll(writer) && ok;
        if (index == 1) jce_frame_kpi_writer_get_status(writer, &first);
        if (index == 3) jce_frame_kpi_writer_get_status(writer, &second);
    }
    jce_frame_kpi_writer_get_status(writer, &tail);
    bool completed = jce_frame_kpi_writer_finish(writer);
    jce_frame_kpi_writer_get_status(writer, &final); dispose(writer, executor);
    size_t header = strlen("frame_index,phase,ms\n");
    TEST_ASSERT_TRUE(ok); TEST_ASSERT_TRUE(completed);
    TEST_ASSERT_EQUAL_UINT64(1, first.submitted_tasks);
    TEST_ASSERT_EQUAL_UINT64(2, first.written_records);
    TEST_ASSERT_EQUAL_UINT64(2, first.flush_calls);
    TEST_ASSERT_EQUAL_UINT64(2, second.submitted_tasks);
    TEST_ASSERT_EQUAL_UINT64(4, second.written_records);
    TEST_ASSERT_EQUAL_UINT64(3, second.flush_calls);
    TEST_ASSERT_EQUAL_UINT64(2, tail.submitted_tasks);
    TEST_ASSERT_EQUAL_UINT(1, tail.queued_records);
    TEST_ASSERT_EQUAL_UINT64(5, final.written_records);
    TEST_ASSERT_EQUAL_UINT64(7, final.written_bytes);
    TEST_ASSERT_EQUAL_UINT64(4, final.flush_calls);
    TEST_ASSERT_EQUAL_UINT(3, sink.data_callback_count);
    TEST_ASSERT_EQUAL_UINT(header + 3, sink.flush_sizes[1]);
    TEST_ASSERT_EQUAL_UINT(header + 6, sink.flush_sizes[2]);
    TEST_ASSERT_EQUAL_UINT(header + 7, sink.flush_sizes[3]);
    TEST_ASSERT_EQUAL_STRING("abcdefg", sink.bytes + header);
}
static void test_zero_batch_config_keeps_immediate_legacy_calls(void)
{
    Sink sink = {0};
    JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
    JceFrameKpiWriterConfig defaults; jce_frame_kpi_writer_config_init(&defaults, true);
    JceFrameKpiWriter *writer = writer_for(&sink, executor, 128, 8, 0);
    TEST_ASSERT_NOT_NULL(writer);
    bool ok = true;
    for (uint64_t index = 0; index < 4; ++index) {
        ok = jce_frame_kpi_writer_submit(writer, index, "x", 1) && ok;
        ok = jce_frame_kpi_writer_poll(writer) && ok;
    }
    JceFrameKpiWriterStatus before; jce_frame_kpi_writer_get_status(writer, &before);
    bool completed = jce_frame_kpi_writer_finish(writer); dispose(writer, executor);
    TEST_ASSERT_TRUE(ok); TEST_ASSERT_TRUE(completed);
    TEST_ASSERT_EQUAL_UINT(0, defaults.min_batch_bytes); TEST_ASSERT_EQUAL_UINT(0, defaults.max_delay_ms);
    TEST_ASSERT_EQUAL_UINT64(4, before.submitted_tasks);
    TEST_ASSERT_EQUAL_UINT64(4, before.written_records);
    TEST_ASSERT_EQUAL_UINT(4, sink.data_callback_count);
    TEST_ASSERT_EQUAL_STRING("xxxx", sink.bytes + strlen("frame_index,phase,ms\n"));
}
static void test_batch_descriptor_boundary_dispatches_before_bytes_or_age(void)
{
    writer_clock_controlled = true; writer_clock_ns = 1000000000u;
    Sink sink = {0};
    JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
    JceFrameKpiWriter *writer = batch_writer_for(&sink, executor, 64, 2, 0, 32, 8);
    TEST_ASSERT_NOT_NULL(writer);
    bool ok = jce_frame_kpi_writer_submit(writer, 0, "a", 1);
    ok = jce_frame_kpi_writer_poll(writer) && ok;
    ok = jce_frame_kpi_writer_submit(writer, 1, "b", 1) && ok;
    ok = jce_frame_kpi_writer_poll(writer) && ok;
    JceFrameKpiWriterStatus before; jce_frame_kpi_writer_get_status(writer, &before);
    ok = jce_frame_kpi_writer_submit(writer, 2, "c", 1) && ok;
    bool completed = jce_frame_kpi_writer_finish(writer); dispose(writer, executor);
    TEST_ASSERT_TRUE(ok); TEST_ASSERT_TRUE(completed);
    TEST_ASSERT_EQUAL_UINT64(1, before.submitted_tasks);
    TEST_ASSERT_EQUAL_UINT64(2, before.written_records);
    TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_ERROR_NONE, before.error);
    TEST_ASSERT_EQUAL_STRING("abc", sink.bytes + strlen("frame_index,phase,ms\n"));
}
static void test_batch_cancel_submit_write_flush_and_close_failures_stay_failed(void)
{
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        writer_clock_controlled = true; writer_clock_ns = 1000000000u;
        Sink sink = {0};
        if (scenario == 2) { sink.short_call = 2; sink.short_bytes = 1; }
        if (scenario == 3) sink.bad_write_status = true;
        if (scenario == 4) sink.failed_flush = 2;
        if (scenario == 5) sink.failed_close = true;
        JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_COOPERATIVE, 8);
        JceFrameKpiWriter *writer = batch_writer_for(&sink, executor, 64, 8,
                                                    scenario == 4 ? 2 : 0, 4, 8);
        TEST_ASSERT_NOT_NULL(writer);
        if (scenario == 1)
            jce_async_executor_shutdown(executor, JCE_ASYNC_SHUTDOWN_DRAIN, JCE_ASYNC_WAIT_INFINITE);
        bool first = jce_frame_kpi_writer_submit(writer, 0, "aa", 2);
        bool second = jce_frame_kpi_writer_submit(writer, 1, "bb", 2);
        bool cancelled = scenario != 0 || (writer->task && jce_async_task_cancel(writer->task));
        bool completed = jce_frame_kpi_writer_finish(writer);
        JceFrameKpiWriterStatus final; jce_frame_kpi_writer_get_status(writer, &final);
        bool repeated = jce_frame_kpi_writer_finish(writer); dispose(writer, executor);
        TEST_ASSERT_TRUE(first); TEST_ASSERT_EQUAL_INT(scenario != 1, second);
        TEST_ASSERT_TRUE(cancelled); TEST_ASSERT_FALSE(completed); TEST_ASSERT_FALSE(repeated);
        TEST_ASSERT_EQUAL_UINT64(2, final.accepted_records);
        TEST_ASSERT_TRUE(final.closed); TEST_ASSERT_FALSE(final.task_outstanding);
        TEST_ASSERT_TRUE(final.first_failure.captured);
        JceFrameKpiWriterError expected = scenario == 0 ? JCE_KPI_WRITER_ERROR_CANCELLED :
            scenario == 1 ? JCE_KPI_WRITER_ERROR_SUBMIT : scenario < 4 ? JCE_KPI_WRITER_ERROR_WRITE :
            scenario == 4 ? JCE_KPI_WRITER_ERROR_FLUSH : JCE_KPI_WRITER_ERROR_CLOSE;
        TEST_ASSERT_EQUAL_INT(expected, final.error);
        TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&sink.closes));
        if (scenario == 2) {
            TEST_ASSERT_EQUAL_UINT64(1, final.written_bytes);
            TEST_ASSERT_EQUAL_UINT64(0, final.written_records);
        }
    }
}
static void test_batch_threaded_running_and_queued_tasks_keep_strict_full_failure(void)
{
    for (unsigned queued = 0; queued < 2; ++queued) {
        Sink sink = {0}, gate = {0}; sink.owner_thread_id = jce_thread_current_id();
        JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_THREADED, 8);
        JceFrameKpiWriter *writer = batch_writer_for(&sink, executor, 16, 8, 0, 4, 8);
        TEST_ASSERT_NOT_NULL(writer);
        Sink *blocked = queued ? &gate : &sink;
        JceThread *safety = jce_thread_create(safety_release, blocked, "kpi-batch-safety");
        if (!safety) { dispose(writer, executor); TEST_FAIL_MESSAGE("Batch safety release unavailable"); }
        JceAsyncTask *blocker = NULL;
        if (queued) {
            JceAsyncTaskDesc desc; jce_async_task_desc_init(&desc);
            desc.work = occupy_worker; desc.user_data = &gate; desc.priority = JCE_ASYNC_PRIORITY_HIGH;
            blocker = jce_async_submit(executor, &desc);
        } else sink.block_data = true;
        bool worker_ready = !queued || (blocker && wait_entered(&gate));
        bool ok = jce_frame_kpi_writer_submit(writer, 0, "aaaa", 4);
        bool write_entered = queued || wait_entered(&sink);
        for (uint64_t index = 1; index < 4; ++index)
            ok = jce_frame_kpi_writer_submit(writer, index, "bbbb", 4) && ok;
        bool rejected = !jce_frame_kpi_writer_submit(writer, 4, "x", 1);
        JceFrameKpiWriterStatus before; jce_frame_kpi_writer_get_status(writer, &before);
        SDL_SetAtomicInt(&blocked->release, 1);
        bool completed = jce_frame_kpi_writer_finish(writer);
        JceFrameKpiWriterStatus final; jce_frame_kpi_writer_get_status(writer, &final);
        jce_thread_join(safety); if (blocker) jce_async_task_release(blocker); dispose(writer, executor);
        TEST_ASSERT_TRUE(worker_ready); TEST_ASSERT_TRUE(write_entered); TEST_ASSERT_TRUE(ok);
        TEST_ASSERT_TRUE(rejected); TEST_ASSERT_FALSE(completed); TEST_ASSERT_TRUE(before.off_thread);
        TEST_ASSERT_FALSE(sink.owner_io_violation);
        TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_ERROR_FULL, final.error);
        TEST_ASSERT_EQUAL_UINT(16, before.first_failure.queued_bytes);
        TEST_ASSERT_EQUAL_UINT(4, before.first_failure.queued_records);
        TEST_ASSERT_TRUE(before.first_failure.task_state_available);
        TEST_ASSERT_EQUAL_INT(queued ? JCE_ASYNC_STATE_QUEUED : JCE_ASYNC_STATE_RUNNING,
                              before.first_failure.task_state);
        TEST_ASSERT_EQUAL_UINT64(1, before.first_failure.submitted_tasks);
        TEST_ASSERT_EQUAL_UINT64(queued ? 0 : 1, before.first_failure.started_tasks);
        TEST_ASSERT_EQUAL_UINT64(0, before.first_failure.completion_callbacks);
        TEST_ASSERT_TRUE(final.closed); TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&sink.closes));
    }
}
/* The same burst fails the small byte budget, survives an explicit larger
 * capture budget, and still fails either independent bound. The actual shared
 * selector configures the complete writer; no mirror parser or queue is used. */
static void test_explicit_capture_budget_keeps_blocked_burst_and_strict_full(void)
{
    static const struct {
        const char *selector;
        unsigned burst, accepted;
        bool failed;
        size_t bytes;
        uint32_t records;
    } cases[] = {
        {"16384,16", 8u, 4u, true, 16384u, 16u},
        {"32768,16", 8u, 8u, false, 32768u, 16u},
        {"32768,32", 9u, 8u, true, 32768u, 32u},
        {"65536,8", 9u, 8u, true, 65536u, 8u}
    };
    char record[4096]; memset(record, 'q', sizeof record);
    for (unsigned scenario = 0; scenario < sizeof cases / sizeof cases[0]; ++scenario) {
        Sink sink = {0}; sink.block_data = true;
        sink.owner_thread_id = jce_thread_current_id();
        JceAsyncExecutor *executor = executor_for(JCE_ASYNC_EXECUTION_THREADED, 8);
        TEST_ASSERT_NOT_NULL(executor);
        JceFrameKpiWriterConfig config; jce_frame_kpi_writer_config_init(&config, false);
        bool selected = jce_engine_kpi_capture_queue_configure(&config, cases[scenario].selector);
        config.min_batch_bytes = 16384u; config.max_delay_ms = 8u;
        config.stream = stream_for(&sink); config.executor = executor;
        JceFrameKpiWriter *writer = jce_frame_kpi_writer_create(&config);
        if (!writer) {
            if (config.stream) SDL_CloseIO(config.stream);
            jce_async_executor_destroy(executor);
            TEST_FAIL_MESSAGE("cannot create selected-budget writer");
        }
        JceThread *safety = jce_thread_create(safety_release, &sink, "kpi-budget-safety");
        if (!safety) {
            sink.block_data = false; dispose(writer, executor);
            TEST_FAIL_MESSAGE("budget sink safety release unavailable");
        }
        unsigned accepted = 0;
        bool first = true;
        for (unsigned i = 0; i < 4; ++i) {
            bool ok = jce_frame_kpi_writer_submit(writer, accepted, record, sizeof record);
            first = first && ok; if (ok) ++accepted;
        }
        bool entered = wait_entered(&sink), refused = false;
        for (unsigned i = 4; i < cases[scenario].burst; ++i) {
            if (!jce_frame_kpi_writer_submit(writer, accepted, record, sizeof record)) {
                refused = true; break;
            }
            ++accepted;
        }
        JceFrameKpiWriterStatus before; jce_frame_kpi_writer_get_status(writer, &before);
        SDL_SetAtomicInt(&sink.release, 1);
        bool complete = jce_frame_kpi_writer_finish(writer);
        JceFrameKpiWriterStatus after; jce_frame_kpi_writer_get_status(writer, &after);
        jce_thread_join(safety); dispose(writer, executor);
        TEST_ASSERT_TRUE(selected); TEST_ASSERT_TRUE(first); TEST_ASSERT_TRUE(entered);
        TEST_ASSERT_EQUAL_UINT(cases[scenario].accepted, accepted);
        TEST_ASSERT_EQUAL_INT(cases[scenario].failed, refused);
        TEST_ASSERT_EQUAL_UINT64(cases[scenario].bytes, before.byte_capacity);
        TEST_ASSERT_EQUAL_UINT32(cases[scenario].records, before.record_capacity);
        TEST_ASSERT_EQUAL_UINT64((uint64_t)accepted * 4096u, before.queued_bytes);
        TEST_ASSERT_EQUAL_UINT(accepted, before.queued_records);
        TEST_ASSERT_EQUAL_INT(!cases[scenario].failed, complete);
        TEST_ASSERT_FALSE(sink.owner_io_violation);
        TEST_ASSERT_TRUE(after.closed); TEST_ASSERT_FALSE(after.task_outstanding);
        TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&sink.closes));
        if (cases[scenario].failed) {
            TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_ERROR_FULL, after.error);
            TEST_ASSERT_TRUE(before.first_failure.captured);
            TEST_ASSERT_TRUE(before.first_failure.write.active);
            TEST_ASSERT_TRUE(after.failure_write_return.return_available);
            TEST_ASSERT_EQUAL_UINT64(before.first_failure.write.serial, after.failure_write_return.serial);
            TEST_ASSERT_EQUAL_UINT64(16384u, after.failure_write_return.actual_bytes);
            TEST_ASSERT_EQUAL_UINT64(before.first_failure.observed_ns, after.first_failure.observed_ns);
            TEST_ASSERT_EQUAL_UINT64(accepted, after.accepted_records);
        } else {
            TEST_ASSERT_FALSE(after.first_failure.captured);
            TEST_ASSERT_EQUAL_UINT64(8u, after.written_records);
            TEST_ASSERT_EQUAL_UINT64(32768u, after.written_bytes);
            TEST_ASSERT_EQUAL_UINT(0, after.queued_records);
            TEST_ASSERT_EQUAL_UINT(0, after.queued_bytes);
        }
    }
}

static void test_public_empty_phase_is_rejected_without_any_raw_write(void)
{
    /* Registration is process-lifetime, so poison this test process last. */
    jce_perf_phase_set_enabled(1); jce_perf_phase_add("", 0.0); jce_perf_phase_frame_tick();
    JceFrameKpiBuffer buffer = {0}; size_t size = 999;
    bool encoded = jce_frame_kpi_encode(&buffer, 0, 0.0, NULL, true, &size);
    Sink sink = {0}; SDL_IOStream *stream = stream_for(&sink);
    bool exported = jce_frame_kpi_export(&buffer, stream, 0, 0.0, NULL, true);
    SDL_CloseIO(stream); jce_frame_kpi_buffer_destroy(&buffer);
    TEST_ASSERT_FALSE(encoded); TEST_ASSERT_EQUAL_UINT(0, size);
    TEST_ASSERT_FALSE(exported); TEST_ASSERT_EQUAL_INT(0, SDL_GetAtomicInt(&sink.writes));
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_disabled_has_no_allocation_or_io);
    RUN_TEST(test_allocation_failure_retains_caller_stream);
    RUN_TEST(test_header_write_and_flush_failure_never_complete);
    RUN_TEST(test_native_delayed_sink_never_runs_inside_submit_and_owns_bytes);
    RUN_TEST(test_threaded_blocked_sink_observes_real_running_write_and_strict_full);
    RUN_TEST(test_threaded_occupied_worker_observes_real_queued_task_and_strict_full);
    RUN_TEST(test_first_failure_occupancy_includes_written_but_unreleased_flush_batch);
    RUN_TEST(test_real_encoder_queue_output_matches_synchronous_bytes);
    RUN_TEST(test_context_writer_uses_selected_header_and_owned_complete_record);
    RUN_TEST(test_wrapped_complete_records_preserve_order_and_queue_stats);
    RUN_TEST(test_byte_or_descriptor_full_fails_without_blocking_fallback);
    RUN_TEST(test_executor_rejection_is_a_retained_capture_error);
    RUN_TEST(test_cancelled_pending_task_is_not_a_successful_drain);
    RUN_TEST(test_partial_wrapped_second_write_never_retries_or_completes);
    RUN_TEST(test_durability_and_final_flush_or_close_errors_are_retained);
    RUN_TEST(test_terminal_work_still_requires_actual_completion_callback);
    RUN_TEST(test_invalid_record_fails_before_any_data_write);
    RUN_TEST(test_explicit_io_error_status_cannot_claim_complete_bytes_or_flush);
    RUN_TEST(test_failed_task_pending_never_masks_failure);
    RUN_TEST(test_cooperative_sink_reentry_defers_close_and_free_in_both_pumps);
    RUN_TEST(test_shared_executor_other_completion_cannot_close_outer_writer);
    RUN_TEST(test_finite_record_and_byte_drain_quotas_are_observed);
    RUN_TEST(test_encoder_scratch_growth_is_clamped_and_overbudget_fails_without_io);
    RUN_TEST(test_single_record_over_drain_target_stays_bounded_and_uses_one_task);
    RUN_TEST(test_low_backlog_dispatch_coalesces_real_tasks_and_physical_writes);
    RUN_TEST(test_batch_oldest_age_survives_later_submissions_and_resets_after_drain);
    RUN_TEST(test_batch_finish_forces_unaged_tail_and_closes_once);
    RUN_TEST(test_batch_durable_record_boundaries_dispatch_before_bytes_or_age);
    RUN_TEST(test_zero_batch_config_keeps_immediate_legacy_calls);
    RUN_TEST(test_batch_descriptor_boundary_dispatches_before_bytes_or_age);
    RUN_TEST(test_batch_cancel_submit_write_flush_and_close_failures_stay_failed);
    RUN_TEST(test_batch_threaded_running_and_queued_tasks_keep_strict_full_failure);
    RUN_TEST(test_public_empty_phase_is_rejected_without_any_raw_write);
    RUN_TEST(test_explicit_capture_budget_keeps_blocked_burst_and_strict_full);
    return UNITY_END();
}
