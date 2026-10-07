/* Public structured task publication; upload stays on the main thread. */
#include "async_load_witness.h"
#include <stdlib.h>
#include <string.h>

typedef struct AsyncLoadResult {
    BenchLoadCpuPhase phase;
    uint64_t worker_started, worker_finished, worker_thread;
    bool worker_main;
} AsyncLoadResult;

static void result_destroy(void *value)
{
    AsyncLoadResult *result = value;
    bench_load_cpu_destroy(&result->phase);
    jce_free(result);
}

static JceAsyncRunResult decode_worker(JceAsyncContext *context, void *user)
{
    BenchAsyncLoad *state = user;
    if (jce_async_context_cancel_requested(context)) return JCE_ASYNC_RUN_CANCELLED;
    AsyncLoadResult *result = jce_malloc(sizeof(*result));
    if (!result) return JCE_ASYNC_RUN_FAILED;
    memset(result, 0, sizeof(*result));
    result->worker_started = jce_time_ticks_ns();
    result->worker_thread = jce_thread_current_id();
    result->worker_main = jce_thread_is_main();
    bool ok = bench_load_cpu_decode(&result->phase, state->kind, state->path, state->entry);
    result->worker_finished = jce_time_ticks_ns();
    if (jce_async_context_cancel_requested(context)) {
        result_destroy(result);
        return JCE_ASYNC_RUN_CANCELLED;
    }
    if (!jce_async_context_set_result(context, result, result_destroy)) {
        result_destroy(result);
        return JCE_ASYNC_RUN_FAILED;
    }
    if (!ok) {
        jce_async_context_fail(context, -1, "benchmark CPU decode failed");
        return JCE_ASYNC_RUN_FAILED;
    }
    return JCE_ASYNC_RUN_SUCCESS;
}

static bool copy_input(char *destination, size_t capacity, const char *source)
{
    if (!source) source = "";
    size_t size = strlen(source);
    if (size >= capacity) return false;
    memcpy(destination, source, size + 1);
    return true;
}

bool bench_async_load_tick(BenchAsyncLoad *state, const JceServices *services,
                            unsigned frame)
{
    const char *enabled = getenv("JCE_BENCH_LOAD_ASYNC");
    if (!enabled) return true;
    if (strcmp(enabled, "1")) return false;
    if (!state->requested && frame == 20) {
        JceAsyncExecutor *executor = jce_async_default_executor();
        JceAsyncExecutorStats stats = {0};
        if (executor) jce_async_executor_get_stats(executor, &stats);
        LOG_INFO("benchmark_load", "benchmark-load-async-executor: mode=%u workers=%u",
            (unsigned)stats.mode, stats.worker_count);
        if (!executor || stats.mode != JCE_ASYNC_EXECUTION_THREADED || !stats.worker_count ||
            !bench_load_kind_valid(getenv("JCE_BENCH_LOAD_KIND")) ||
            !copy_input(state->kind, sizeof(state->kind), getenv("JCE_BENCH_LOAD_KIND")) ||
            !copy_input(state->path, sizeof(state->path), getenv("JCE_BENCH_LOAD_FILE")) ||
            !copy_input(state->entry, sizeof(state->entry), getenv("JCE_BENCH_LOAD_ENTRY")) ||
            !state->path[0] || !jce_thread_is_main()) return false;
        state->requested = true;
        state->main_thread = jce_thread_current_id();
        state->requested_ns = jce_time_ticks_ns();
        JceAsyncTaskDesc descriptor;
        jce_async_task_desc_init(&descriptor);
        descriptor.work = decode_worker;
        descriptor.user_data = state;
        descriptor.debug_name = "benchmark.load.decode";
        descriptor.priority = JCE_ASYNC_PRIORITY_HIGH;
        state->task = jce_async_submit(executor, &descriptor);
        if (!state->task) return false;
    }
    if (!state->task || state->complete) return true;
    if (jce_time_ticks_ns() - state->requested_ns > 60000000000ull) return false;
    if (!jce_async_task_is_terminal(state->task)) return true; /* Never wait in a frame. */
    AsyncLoadResult *result = jce_async_task_result(state->task);
    bool ok = result && jce_async_task_state(state->task) == JCE_ASYNC_STATE_SUCCEEDED &&
        !result->worker_main && result->worker_thread != state->main_thread;
    unsigned upload_frame = frame;
    if (ok) ok = bench_load_submit(&state->loaded, &result->phase, services, false);
    const uint64_t submitted = jce_time_ticks_ns();
    if (result) {
        bench_load_phase_log(&result->phase, state->kind, 20, ok);
        LOG_INFO("benchmark_load", "benchmark-load-async: request_frame=20 completion_frame=%u worker_main=%u main_thread=%llu worker_thread=%llu queue_ns=%llu worker_wall_ns=%llu poll_gap_ns=%llu ready_elapsed_ns=%llu submit_ns=%llu ok=%u",
            upload_frame, (unsigned)result->worker_main,
            (unsigned long long)state->main_thread, (unsigned long long)result->worker_thread,
            (unsigned long long)(result->worker_started - state->requested_ns),
            (unsigned long long)(result->worker_finished - result->worker_started),
            (unsigned long long)(submitted - result->worker_finished - result->phase.submit_ns),
            (unsigned long long)(submitted - state->requested_ns),
            (unsigned long long)result->phase.submit_ns, (unsigned)ok);
    }
    state->complete = true;
    /* Task owns the CPU payload destructor; upload has cleared consumed members. */
    jce_async_task_release(state->task);
    state->task = NULL;
    return ok;
}

void bench_async_load_destroy(BenchAsyncLoad *state)
{
    if (state->requested && !state->complete)
        LOG_ERROR("benchmark_load", "benchmark-load-async: shutdown before completion");
    if (state->task) (void)jce_async_task_discard(state->task);
    bench_load_destroy(&state->loaded);
    memset(state, 0, sizeof(*state));
}
