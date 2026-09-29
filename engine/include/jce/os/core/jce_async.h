/*
 * jce_async.h - Structured asynchronous execution.
 *
 * Long-running, independently-lived work belongs here. Short frame-bound
 * fork/join loops continue to use jce_thread_pool_parallel_for().
 */

#ifndef JCE_ASYNC_H
#define JCE_ASYNC_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_ASYNC_WAIT_INFINITE UINT32_MAX

typedef struct JceAsyncExecutor JceAsyncExecutor;
typedef struct JceAsyncTask     JceAsyncTask;
typedef struct JceAsyncGroup    JceAsyncGroup;
typedef struct JceAsyncContext  JceAsyncContext;

typedef enum JceAsyncExecutionMode {
    JCE_ASYNC_EXECUTION_AUTO        = 0,
    JCE_ASYNC_EXECUTION_THREADED    = 1,
    JCE_ASYNC_EXECUTION_COOPERATIVE = 2
} JceAsyncExecutionMode;

typedef enum JceAsyncPriority {
    JCE_ASYNC_PRIORITY_CRITICAL   = 0,
    JCE_ASYNC_PRIORITY_HIGH       = 1,
    JCE_ASYNC_PRIORITY_NORMAL     = 2,
    JCE_ASYNC_PRIORITY_LOW        = 3,
    JCE_ASYNC_PRIORITY_BACKGROUND = 4
} JceAsyncPriority;

/*
 * Threaded executors reserve a latency lane from LOW and BACKGROUND work
 * when at least two workers are available and `reserve_latency_worker` is
 * enabled. Disable it only for a private throughput executor whose entire
 * workload belongs to one subsystem. Priorities express scheduling urgency,
 * not correctness or task ownership.
 */

typedef enum JceAsyncState {
    JCE_ASYNC_STATE_INVALID   = 0,
    JCE_ASYNC_STATE_WAITING   = 1,
    JCE_ASYNC_STATE_QUEUED    = 2,
    JCE_ASYNC_STATE_RUNNING   = 3,
    JCE_ASYNC_STATE_SUCCEEDED = 4,
    JCE_ASYNC_STATE_FAILED    = 5,
    JCE_ASYNC_STATE_CANCELLED = 6
} JceAsyncState;

typedef enum JceAsyncRunResult {
    JCE_ASYNC_RUN_SUCCESS   = 0,
    JCE_ASYNC_RUN_FAILED    = 1,
    JCE_ASYNC_RUN_CANCELLED = 2
} JceAsyncRunResult;

typedef enum JceAsyncWaitResult {
    JCE_ASYNC_WAIT_COMPLETED      = 0,
    JCE_ASYNC_WAIT_TIMED_OUT      = 1,
    JCE_ASYNC_WAIT_WOULD_DEADLOCK = 2,
    JCE_ASYNC_WAIT_INVALID        = 3
} JceAsyncWaitResult;

typedef enum JceAsyncDependencyPolicy {
    JCE_ASYNC_DEPENDENCY_REQUIRE_SUCCESS = 0,
    JCE_ASYNC_DEPENDENCY_ALWAYS_RUN      = 1
} JceAsyncDependencyPolicy;

typedef enum JceAsyncShutdownMode {
    JCE_ASYNC_SHUTDOWN_DRAIN          = 0,
    JCE_ASYNC_SHUTDOWN_CANCEL_PENDING = 1,
    JCE_ASYNC_SHUTDOWN_CANCEL_ALL     = 2
} JceAsyncShutdownMode;

typedef JceAsyncRunResult (*JceAsyncWorkFn)(JceAsyncContext *ctx,
                                            void *user_data);
/* `task` is borrowed for the callback. Retain it to keep another reference. */
typedef void (*JceAsyncCompleteFn)(JceAsyncTask *task, void *user_data);
typedef void (*JceAsyncCleanupFn)(void *user_data);
typedef void (*JceAsyncResultDestroyFn)(void *result);

typedef struct JceAsyncExecutorConfig {
    uint32_t              struct_size;
    JceAsyncExecutionMode mode;
    /* Requested spawned workers. Zero uses the process policy. Explicit
     * requests are capped by [performance] job_workers, the low-memory tier,
     * and the async service ceiling so a subsystem cannot oversubscribe the
     * machine on its own. Ignored by cooperative executors. */
    uint32_t              worker_count;
    uint32_t              max_tasks;
    uint32_t              cooperative_tasks_per_pump;
    uint32_t              cooperative_completions_per_pump;
    uint64_t              cooperative_time_budget_us;
    const char           *debug_name;
    bool                  reserve_latency_worker;
} JceAsyncExecutorConfig;

typedef struct JceAsyncTaskDesc {
    uint32_t                  struct_size;
    JceAsyncWorkFn            work;
    JceAsyncCompleteFn        complete;
    JceAsyncCleanupFn         cleanup;
    void                     *user_data;
    const char               *debug_name;
    JceAsyncPriority          priority;
    JceAsyncDependencyPolicy dependency_policy;
    uint32_t                  timeout_ms;
    JceAsyncGroup            *group;
    JceAsyncTask *const      *dependencies;
    uint32_t                  dependency_count;
} JceAsyncTaskDesc;

typedef struct JceAsyncPumpBudget {
    uint32_t max_work_items;
    uint32_t max_completions;
    uint64_t max_time_us;
} JceAsyncPumpBudget;

typedef struct JceAsyncExecutorStats {
    JceAsyncExecutionMode mode;
    uint32_t worker_count;
    uint32_t capacity;
    uint32_t live_tasks;
    uint32_t waiting_tasks;
    uint32_t queued_tasks;
    uint32_t running_tasks;
    uint32_t pending_completions;
    uint32_t peak_live_tasks;
    uint32_t peak_queued_tasks;
    uint64_t submitted_tasks;
    uint64_t succeeded_tasks;
    uint64_t failed_tasks;
    uint64_t cancelled_tasks;
    uint64_t rejected_tasks;
    uint64_t total_queue_time_ns;
    uint64_t max_queue_time_ns;
    uint64_t total_run_time_ns;
    uint64_t max_run_time_ns;
} JceAsyncExecutorStats;

/* Fill a versioned configuration or task descriptor with stable defaults. */
JCE_API void JCE_CALL
jce_async_executor_config_init(JceAsyncExecutorConfig *config);
JCE_API void JCE_CALL
jce_async_task_desc_init(JceAsyncTaskDesc *desc);
JCE_API void JCE_CALL
jce_async_pump_budget_init(JceAsyncPumpBudget *budget);

/* Create an executor. The calling thread becomes its lifecycle and pump
 * owner. That same thread must pump, shut down, and destroy it. */
JCE_API JceAsyncExecutor *JCE_CALL
jce_async_executor_create(const JceAsyncExecutorConfig *config);

/* Stop accepting submissions and apply the requested shutdown policy.
 * Owner-thread only. A timed-out shutdown is retryable. */
JCE_API bool JCE_CALL
jce_async_executor_shutdown(JceAsyncExecutor *executor,
                            JceAsyncShutdownMode mode,
                            uint32_t timeout_ms);

/* Owner-thread only. Destroy after shutdown. If needed, performs an infinite
 * drain first. A non-owner call is ignored so it cannot free live storage. */
JCE_API void JCE_CALL
jce_async_executor_destroy(JceAsyncExecutor *executor);

/* Run cooperative work and owner-thread completions within a budget.
 * NULL uses the executor defaults. Returns work + completions processed. */
JCE_API uint32_t JCE_CALL
jce_async_executor_pump(JceAsyncExecutor *executor,
                        const JceAsyncPumpBudget *budget);

JCE_API bool JCE_CALL
jce_async_executor_is_owner(const JceAsyncExecutor *executor);
JCE_API JceAsyncExecutionMode JCE_CALL
jce_async_executor_mode(const JceAsyncExecutor *executor);
JCE_API void JCE_CALL
jce_async_executor_get_stats(const JceAsyncExecutor *executor,
                             JceAsyncExecutorStats *out_stats);

/* Thread-safe, non-inline submission. Returns one caller-owned reference.
 * NULL means the executor is closing, at capacity, or allocation failed.
 * On rejection no user callback, cleanup, or result destructor is invoked. */
JCE_API JceAsyncTask *JCE_CALL
jce_async_submit(JceAsyncExecutor *executor,
                 const JceAsyncTaskDesc *desc);

/* Task handle ownership. */
JCE_API JceAsyncTask *JCE_CALL
jce_async_task_retain(JceAsyncTask *task);
JCE_API void JCE_CALL
jce_async_task_release(JceAsyncTask *task);

/* Tear-down: cancel the task, wait for its work to stop, disarm its
 * completion callback, and release this reference.  Use it wherever the
 * caller is about to free the memory the callback would read -- which is
 * every "cancel, wait, release, free the job" site.
 *
 * Returns true when a pending callback was actually disarmed; false when
 * there was none left to disarm (it had already run, or this is a worker of
 * the same executor and waiting would deadlock).  The task is released
 * either way, and a `cleanup` hook still runs.
 *
 * Safe from any thread: if a pump on another thread is already inside the
 * callback, this blocks until that callback returns. */
JCE_API bool JCE_CALL
jce_async_task_discard(JceAsyncTask *task);

JCE_API JceAsyncState JCE_CALL
jce_async_task_state(const JceAsyncTask *task);
JCE_API bool JCE_CALL
jce_async_task_is_terminal(const JceAsyncTask *task);
JCE_API bool JCE_CALL
jce_async_task_cancel(JceAsyncTask *task);
/* Waits for the task to reach a TERMINAL STATE.  That is not the same as
 * "the task is finished with you": a terminal task is queued for its
 * completion callback, which runs later, from the owner thread pump.  So
 * after this returns:
 *
 *   - anything the completion callback publishes is NOT yet published;
 *   - anything the completion callback reads is still live from its side.
 *
 * Tearing down with cancel -> wait -> release therefore leaves a queued
 * callback pointing at whatever the caller frees next.  Use
 * jce_async_task_discard() for that; use this one to wait for a RESULT, and
 * pump before reading anything the completion writes. */
JCE_API JceAsyncWaitResult JCE_CALL
jce_async_task_wait_timeout(JceAsyncTask *task, uint32_t timeout_ms);
JCE_API void JCE_CALL
jce_async_task_wait(JceAsyncTask *task);

/* Progress is safe to poll while work runs. Result/error fields are stable
 * after the task reaches a terminal state and while the caller retains it. */
JCE_API float JCE_CALL
jce_async_task_progress(const JceAsyncTask *task);
JCE_API void *JCE_CALL
jce_async_task_result(const JceAsyncTask *task);
JCE_API int32_t JCE_CALL
jce_async_task_error_code(const JceAsyncTask *task);
JCE_API const char *JCE_CALL
jce_async_task_error_message(const JceAsyncTask *task);
JCE_API const char *JCE_CALL
jce_async_task_debug_name(const JceAsyncTask *task);

/* Work-context publication. These functions are valid only during `work`.
 * Progress is monotonic. A task owns an accepted result until final release. */
JCE_API bool JCE_CALL
jce_async_context_cancel_requested(const JceAsyncContext *ctx);
JCE_API void JCE_CALL
jce_async_context_set_progress(JceAsyncContext *ctx, float progress);
JCE_API bool JCE_CALL
jce_async_context_set_result(JceAsyncContext *ctx,
                             void *result,
                             JceAsyncResultDestroyFn destroy);
JCE_API void JCE_CALL
jce_async_context_fail(JceAsyncContext *ctx,
                       int32_t error_code,
                       const char *message);

/* Structured task groups. A group binds to the first executor used with it. */
JCE_API JceAsyncGroup *JCE_CALL jce_async_group_create(void);
JCE_API JceAsyncGroup *JCE_CALL
jce_async_group_retain(JceAsyncGroup *group);
JCE_API void JCE_CALL
jce_async_group_release(JceAsyncGroup *group);
JCE_API bool JCE_CALL
jce_async_group_cancel(JceAsyncGroup *group);
JCE_API bool JCE_CALL
jce_async_group_is_cancelled(const JceAsyncGroup *group);
JCE_API uint32_t JCE_CALL
jce_async_group_pending(const JceAsyncGroup *group);
JCE_API JceAsyncWaitResult JCE_CALL
jce_async_group_wait_timeout(JceAsyncGroup *group, uint32_t timeout_ms);
JCE_API void JCE_CALL
jce_async_group_wait(JceAsyncGroup *group);

/* Process-wide background executor. The returned pointer is borrowed and is
 * valid until owner-thread default shutdown. The engine initializes it on the
 * main thread before background consumers can submit. Returns NULL while an
 * owner-thread shutdown is in progress; recreation is enabled after destroy. */
JCE_API JceAsyncExecutor *JCE_CALL jce_async_default_executor(void);
JCE_API uint32_t JCE_CALL
jce_async_default_pump(const JceAsyncPumpBudget *budget);
JCE_API bool JCE_CALL
jce_async_default_shutdown(JceAsyncShutdownMode mode, uint32_t timeout_ms);

JCE_EXTERN_C_END

#endif /* JCE_ASYNC_H */
