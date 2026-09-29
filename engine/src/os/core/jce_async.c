/*
 * jce_async.c - Structured asynchronous execution over enkiTS.
 */

#include <jce/os/core/jce_async.h>

#include <jce/os/core/jce_config.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_trace.h>

#include "jce_memory.h"

#include <enkiTS/TaskScheduler_c.h>
#include <SDL3/SDL.h>

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define ASYNC_NAME_CAP 64
#define ASYNC_ERROR_CAP 192
#define ASYNC_DEFAULT_CAPACITY 4096u
#define ASYNC_DEFAULT_WORK_ITEMS 8u
#define ASYNC_DEFAULT_COMPLETIONS 64u
#define ASYNC_DEFAULT_TIME_US 2000u
#define ASYNC_MAX_WORKERS 16u

typedef struct JceAsyncDependencyLink JceAsyncDependencyLink;

struct JceAsyncContext {
    JceAsyncTask *task;
};

struct JceAsyncGroup {
    SDL_AtomicInt refs;
    SDL_AtomicInt cancel_requested;
    SDL_Mutex    *mutex;
    SDL_Condition *cond;
    uint32_t      pending;
    uint64_t      executor_id;
    JceAsyncExecutor *executor;
    JceAsyncTask *head;
};

struct JceAsyncDependencyLink {
    JceAsyncDependencyLink *next;
    JceAsyncTask           *child;
    bool                    active;
};

struct JceAsyncTask {
    SDL_AtomicInt refs;
    SDL_AtomicInt state;
    SDL_AtomicInt cancel_requested;

    SDL_Mutex     *wait_mutex;
    SDL_Condition *wait_cond;

    JceAsyncExecutor *executor;
    uint64_t          executor_id;
    enkiPinnedTask   *pinned;

    JceAsyncWorkFn     work;
    JceAsyncCompleteFn complete;
    JceAsyncCleanupFn  cleanup;
    void              *user_data;

    void                    *result;
    JceAsyncResultDestroyFn  result_destroy;
    int32_t                  error_code;
    char                     error_message[ASYNC_ERROR_CAP];
    float                    progress;
    char                     debug_name[ASYNC_NAME_CAP];

    JceAsyncPriority          priority;
    JceAsyncDependencyPolicy dependency_policy;
    uint64_t                  deadline_ms;
    uint64_t                  submitted_ns;
    uint64_t                  started_ns;
    uint64_t                  trace_task_id;
    uint64_t                  trace_parent_id;
    uint32_t                  target_thread;
    uint32_t                  pending_dependencies;
    bool                      dependency_failed;
    bool                      scheduled;
    bool                      completion_claimed;
    bool                      callback_done;
    bool                      cleanup_done;
    bool                      worker_slot_counted;

    JceAsyncDependencyLink *dependency_links;
    uint32_t                dependency_count;
    JceAsyncDependencyLink *dependents_head;

    JceAsyncGroup *group;
    JceAsyncTask  *group_prev;
    JceAsyncTask  *group_next;

    JceAsyncTask *active_prev;
    JceAsyncTask *active_next;
    JceAsyncTask *completion_next;
    JceAsyncTask *cooperative_next;
};

struct JceAsyncExecutor {
    enkiTaskScheduler *scheduler;
    SDL_Mutex         *lock;
    SDL_Condition     *idle_cond;

    JceAsyncExecutionMode mode;
    uint32_t worker_count;
    bool reserve_latency_worker;
    uint32_t max_tasks;
    uint32_t default_work_items;
    uint32_t default_completions;
    uint64_t default_time_us;
    uint64_t owner_tid;
    uint64_t id;
    char     debug_name[ASYNC_NAME_CAP];

    bool accepting;
    bool scheduler_running;
    bool shutdown_complete;
    uint32_t next_worker;
    uint32_t worker_pending[ASYNC_MAX_WORKERS];

    JceAsyncTask *active_head;
    JceAsyncTask *completion_head;
    JceAsyncTask *completion_tail;
    JceAsyncTask *cooperative_head[5];
    JceAsyncTask *cooperative_tail[5];

    uint32_t live_tasks;
    uint32_t unfinished_tasks;
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
};

static SDL_TLSID    g_current_executor;
static SDL_TLSID    g_current_task;
static SDL_AtomicInt g_executor_ids;

static bool async_state_terminal(JceAsyncState state)
{
    return state == JCE_ASYNC_STATE_SUCCEEDED ||
           state == JCE_ASYNC_STATE_FAILED ||
           state == JCE_ASYNC_STATE_CANCELLED;
}

static uint32_t async_priority_index(JceAsyncPriority priority)
{
    if ((int)priority < (int)JCE_ASYNC_PRIORITY_CRITICAL)
        return 0;
    if ((int)priority > (int)JCE_ASYNC_PRIORITY_BACKGROUND)
        return 4;
    return (uint32_t)priority;
}

static uint32_t async_process_worker_budget(void)
{
    int configured = jce_config_job_workers();
    int workers;

    if (configured > 0) {
        workers = configured;
    } else if (jce_config_machine_class() == JCE_MACHINE_CLASS_LOW) {
        workers = 1;
    } else {
        workers = jce_thread_pool_default_workers();
    }

    if (workers < 1)
        workers = 1;
    /* The cap was 4, with nothing saying why, and it was the binding
     * constraint on every machine bigger than a laptop: jce_thread_pool_
     * default_workers() already answers cores-1-capped-at-8, and this threw
     * the answer away.  Measured on a 32-core box opening
     * caged_kingdom/hidden_cove -- 49 glTF decodes, ~92 ms each, 4.5 s of CPU
     * -- the asset pool ran 4 workers and the first frame waited on work that
     * eight could have finished in half the time.
     *
     * 8 matches the shared frame pool's own ceiling, so the two agree.  Three
     * threaded executors exist (archive loader, asset decode, streaming), so
     * the worst case is 24 threads; they are I/O- and burst-bound and asleep
     * almost always, which is why this is a per-executor cap rather than a
     * process-wide budget.  LOW machine class (1) and an explicit
     * [performance] job_workers pin both still win outright -- those are the
     * knobs for a box that cannot afford this. */
    if (workers > 8)
        workers = 8;
    return (uint32_t)workers;
}

static uint32_t
async_choose_worker_locked(JceAsyncExecutor *executor,
                           JceAsyncPriority priority)
{
    uint32_t first_worker = 0;
    uint32_t candidate_count;
    uint32_t start;
    uint32_t best;
    uint32_t best_load;
    uint32_t i;

    /*
     * Keep one latency lane available when possible. Low/background work may
     * saturate its throughput lanes, but cannot occupy every worker.
     */
    if (executor->reserve_latency_worker &&
        executor->worker_count > 1 &&
        priority >= JCE_ASYNC_PRIORITY_LOW)
        first_worker = 1;

    candidate_count = executor->worker_count - first_worker;
    start = first_worker +
        (executor->next_worker % candidate_count);
    executor->next_worker++;
    best = start;
    best_load = executor->worker_pending[best];

    for (i = 1; i < candidate_count; ++i) {
        uint32_t candidate = first_worker +
            ((start - first_worker + i) % candidate_count);
        uint32_t load = executor->worker_pending[candidate];

        if (load < best_load) {
            best = candidate;
            best_load = load;
        }
    }
    return best + 1u;
}

static void async_increment_queued_locked(JceAsyncExecutor *executor)
{
    executor->queued_tasks++;
    if (executor->queued_tasks > executor->peak_queued_tasks)
        executor->peak_queued_tasks = executor->queued_tasks;
}

static int async_enki_priority(JceAsyncPriority priority)
{
    if (priority <= JCE_ASYNC_PRIORITY_HIGH)
        return 0;
    if (priority == JCE_ASYNC_PRIORITY_NORMAL)
        return 1;
    return 2;
}

static bool async_deadline_expired(const JceAsyncTask *task)
{
    return task && task->deadline_ms != 0 &&
           jce_time_ticks_ms() >= task->deadline_ms;
}

static bool async_cancel_requested(const JceAsyncTask *task)
{
    return task &&
           (SDL_GetAtomicInt((SDL_AtomicInt *)&task->cancel_requested) != 0 ||
            async_deadline_expired(task));
}

static void async_task_destroy(JceAsyncTask *task)
{
    if (!task)
        return;

    if (!task->cleanup_done && task->cleanup)
        task->cleanup(task->user_data);
    if (task->result_destroy && task->result)
        task->result_destroy(task->result);

    if (task->wait_cond)
        SDL_DestroyCondition(task->wait_cond);
    if (task->wait_mutex)
        SDL_DestroyMutex(task->wait_mutex);
    JCE_FREE(task->dependency_links);
    JCE_FREE(task);
}

JceAsyncTask *jce_async_task_retain(JceAsyncTask *task)
{
    if (task)
        SDL_AddAtomicInt(&task->refs, 1);
    return task;
}

void jce_async_task_release(JceAsyncTask *task)
{
    if (!task)
        return;
    if (SDL_AddAtomicInt(&task->refs, -1) == 1)
        async_task_destroy(task);
}

bool jce_async_task_discard(JceAsyncTask *task)
{
    JceAsyncExecutor *executor;
    bool dropped = false;

    if (!task)
        return false;

    (void)jce_async_task_cancel(task);
    if (jce_async_task_wait_timeout(task, JCE_ASYNC_WAIT_INFINITE) ==
        JCE_ASYNC_WAIT_WOULD_DEADLOCK) {
        /* Called from a worker of this very executor: waiting would deadlock,
         * and this thread cannot be the one that pumps the completion either.
         * Drop the reference and report that the callback still stands. */
        jce_async_task_release(task);
        return false;
    }

    SDL_LockMutex(task->wait_mutex);
    executor = task->executor;
    SDL_UnlockMutex(task->wait_mutex);
    if (!executor) {
        /* Already reaped: the completion has run, nothing to disarm. */
        jce_async_task_release(task);
        return false;
    }

    SDL_LockMutex(executor->lock);
    if (task->completion_claimed && !task->callback_done) {
        /* A pump on another thread is inside the callback right now.  Let it
         * finish before returning, or the caller frees under it. */
        while (!task->callback_done)
            SDL_WaitCondition(executor->idle_cond, executor->lock);
    } else if (!task->callback_done) {
        /* Still queued.  Claiming happens under this same lock, so the pump
         * cannot be past that point: clearing the hook here is race-free.
         * The task stays in the queue and is reaped normally -- disarming the
         * callback is the whole job, not unlinking the node. */
        task->complete = NULL;
        dropped = true;
    }
    SDL_UnlockMutex(executor->lock);

    jce_async_task_release(task);
    return dropped;
}

static void async_task_set_state(JceAsyncTask *task, JceAsyncState state)
{
    SDL_LockMutex(task->wait_mutex);
    SDL_SetAtomicInt(&task->state, (int)state);
    if (async_state_terminal(state))
        SDL_BroadcastCondition(task->wait_cond);
    SDL_UnlockMutex(task->wait_mutex);
}

static void async_active_add_locked(JceAsyncExecutor *executor,
                                    JceAsyncTask *task)
{
    task->active_prev = NULL;
    task->active_next = executor->active_head;
    if (executor->active_head)
        executor->active_head->active_prev = task;
    executor->active_head = task;
}

static void async_active_remove_locked(JceAsyncExecutor *executor,
                                       JceAsyncTask *task)
{
    if (task->active_prev)
        task->active_prev->active_next = task->active_next;
    else
        executor->active_head = task->active_next;
    if (task->active_next)
        task->active_next->active_prev = task->active_prev;
    task->active_prev = NULL;
    task->active_next = NULL;
}

static void async_cooperative_push_locked(JceAsyncExecutor *executor,
                                          JceAsyncTask *task)
{
    uint32_t priority = async_priority_index(task->priority);
    task->cooperative_next = NULL;
    if (executor->cooperative_tail[priority])
        executor->cooperative_tail[priority]->cooperative_next = task;
    else
        executor->cooperative_head[priority] = task;
    executor->cooperative_tail[priority] = task;
}

static JceAsyncTask *
async_cooperative_pop_locked(JceAsyncExecutor *executor)
{
    uint32_t priority;

    for (priority = 0; priority < 5; ++priority) {
        JceAsyncTask *task = executor->cooperative_head[priority];
        if (!task)
            continue;
        executor->cooperative_head[priority] = task->cooperative_next;
        if (!executor->cooperative_head[priority])
            executor->cooperative_tail[priority] = NULL;
        task->cooperative_next = NULL;
        return task;
    }
    return NULL;
}

static void async_task_execute(void *user_data);

static void async_schedule_locked(JceAsyncExecutor *executor,
                                  JceAsyncTask *task)
{
    if (!executor || !task || task->scheduled)
        return;

    task->scheduled = true;
    if (executor->mode == JCE_ASYNC_EXECUTION_COOPERATIVE) {
        async_cooperative_push_locked(executor, task);
        return;
    }

    enkiAddPinnedTaskArgs(executor->scheduler, task->pinned, task);
}

static void async_expire_waiting_locked(JceAsyncExecutor *executor)
{
    JceAsyncTask *task;

    for (task = executor->active_head; task; task = task->active_next) {
        JceAsyncState state =
            (JceAsyncState)SDL_GetAtomicInt(&task->state);

        if (state != JCE_ASYNC_STATE_WAITING ||
            !async_deadline_expired(task))
            continue;

        SDL_SetAtomicInt(&task->cancel_requested, 1);
        if (executor->waiting_tasks > 0)
            executor->waiting_tasks--;
        async_increment_queued_locked(executor);
        async_task_set_state(task, JCE_ASYNC_STATE_QUEUED);
        async_schedule_locked(executor, task);
    }
}

static void async_group_attach_locked(JceAsyncGroup *group,
                                      JceAsyncExecutor *executor,
                                      JceAsyncTask *task)
{
    if (!group)
        return;

    group->executor_id = executor->id;
    group->executor = executor;
    task->group = jce_async_group_retain(group);
    task->group_prev = NULL;
    task->group_next = group->head;
    if (group->head)
        group->head->group_prev = task;
    group->head = task;
    group->pending++;
}

static void async_group_detach(JceAsyncTask *task)
{
    JceAsyncGroup *group;

    if (!task || !task->group)
        return;
    group = task->group;

    SDL_LockMutex(group->mutex);
    if (task->group_prev)
        task->group_prev->group_next = task->group_next;
    else if (group->head == task)
        group->head = task->group_next;
    if (task->group_next)
        task->group_next->group_prev = task->group_prev;
    task->group_prev = NULL;
    task->group_next = NULL;
    task->group = NULL;
    if (group->pending > 0)
        group->pending--;
    if (group->pending == 0)
        SDL_BroadcastCondition(group->cond);
    SDL_UnlockMutex(group->mutex);

    jce_async_group_release(group);
}

static void async_dependency_resolve(JceAsyncExecutor *executor,
                                     JceAsyncDependencyLink *link,
                                     JceAsyncState predecessor_state)
{
    JceAsyncTask *child;
    JceAsyncState child_state;

    if (!link || !link->active)
        return;
    child = link->child;

    SDL_LockMutex(executor->lock);
    link->active = false;
    if (child->pending_dependencies > 0)
        child->pending_dependencies--;
    if (child->dependency_policy ==
            JCE_ASYNC_DEPENDENCY_REQUIRE_SUCCESS &&
        predecessor_state != JCE_ASYNC_STATE_SUCCEEDED)
        child->dependency_failed = true;

    child_state = (JceAsyncState)SDL_GetAtomicInt(&child->state);
    if (child_state == JCE_ASYNC_STATE_WAITING &&
        child->pending_dependencies == 0) {
        if (executor->waiting_tasks > 0)
            executor->waiting_tasks--;
        async_increment_queued_locked(executor);
        if (child->dependency_failed)
            SDL_SetAtomicInt(&child->cancel_requested, 1);
        async_task_set_state(child, JCE_ASYNC_STATE_QUEUED);
        async_schedule_locked(executor, child);
    }
    SDL_UnlockMutex(executor->lock);

    jce_async_task_release(child);
}

static void async_task_finish(JceAsyncTask *task, JceAsyncState terminal)
{
    JceAsyncExecutor *executor;
    JceAsyncDependencyLink *dependents;
    JceAsyncState old_state;
    uint64_t finished_ns;

    if (!task || !async_state_terminal(terminal))
        return;
    executor = task->executor;
    if (!executor)
        return;

    SDL_LockMutex(executor->lock);
    old_state = (JceAsyncState)SDL_GetAtomicInt(&task->state);
    if (async_state_terminal(old_state)) {
        SDL_UnlockMutex(executor->lock);
        return;
    }
    finished_ns = jce_time_ticks_ns();

    if (old_state == JCE_ASYNC_STATE_WAITING &&
        executor->waiting_tasks > 0)
        executor->waiting_tasks--;
    else if (old_state == JCE_ASYNC_STATE_QUEUED &&
             executor->queued_tasks > 0)
        executor->queued_tasks--;
    else if (old_state == JCE_ASYNC_STATE_RUNNING &&
             executor->running_tasks > 0)
        executor->running_tasks--;

    if (task->worker_slot_counted && task->target_thread > 0) {
        uint32_t worker = task->target_thread - 1u;

        if (worker < executor->worker_count &&
            executor->worker_pending[worker] > 0)
            executor->worker_pending[worker]--;
        task->worker_slot_counted = false;
    }

    if (task->started_ns >= task->submitted_ns) {
        uint64_t queue_ns = task->started_ns - task->submitted_ns;
        uint64_t run_ns = finished_ns >= task->started_ns
            ? finished_ns - task->started_ns : 0;

        executor->total_queue_time_ns += queue_ns;
        executor->total_run_time_ns += run_ns;
        if (queue_ns > executor->max_queue_time_ns)
            executor->max_queue_time_ns = queue_ns;
        if (run_ns > executor->max_run_time_ns)
            executor->max_run_time_ns = run_ns;
    }

    async_task_set_state(task, terminal);
    if (terminal == JCE_ASYNC_STATE_SUCCEEDED)
        executor->succeeded_tasks++;
    else if (terminal == JCE_ASYNC_STATE_FAILED)
        executor->failed_tasks++;
    else
        executor->cancelled_tasks++;

    if (executor->unfinished_tasks > 0)
        executor->unfinished_tasks--;

    task->completion_next = NULL;
    if (executor->completion_tail)
        executor->completion_tail->completion_next = task;
    else
        executor->completion_head = task;
    executor->completion_tail = task;
    executor->pending_completions++;

    dependents = task->dependents_head;
    task->dependents_head = NULL;
    SDL_BroadcastCondition(executor->idle_cond);
    SDL_UnlockMutex(executor->lock);

    async_group_detach(task);

    while (dependents) {
        JceAsyncDependencyLink *next = dependents->next;
        async_dependency_resolve(executor, dependents, terminal);
        dependents = next;
    }
}

static void async_task_execute(void *user_data)
{
    JceAsyncTask *task = (JceAsyncTask *)user_data;
    JceAsyncExecutor *executor = task ? task->executor : NULL;
    JceAsyncRunResult result = JCE_ASYNC_RUN_CANCELLED;
    JceAsyncState terminal;
    void *previous_executor;
    void *previous_task;
    uint64_t previous_trace_task;
    uint64_t trace_span_id = 0;
    uint64_t trace_started_ns = 0;
    bool cancelled;

    if (!task || !executor)
        return;

    previous_executor = SDL_GetTLS(&g_current_executor);
    previous_task = SDL_GetTLS(&g_current_task);
    SDL_SetTLS(&g_current_executor, executor, NULL);
    SDL_SetTLS(&g_current_task, task, NULL);
    previous_trace_task = jce_trace_task_current_id();

    if (task->target_thread > 0) {
        char thread_name[JCE_TRACE_NAME_CAP];
        SDL_snprintf(thread_name, sizeof(thread_name), "%s-%u",
                     executor->debug_name, (unsigned)task->target_thread);
        jce_trace_thread_register(thread_name);
    }

    SDL_LockMutex(executor->lock);
    if ((JceAsyncState)SDL_GetAtomicInt(&task->state) ==
        JCE_ASYNC_STATE_QUEUED) {
        task->started_ns = jce_time_ticks_ns();
        if (executor->queued_tasks > 0)
            executor->queued_tasks--;
        executor->running_tasks++;
        async_task_set_state(task, JCE_ASYNC_STATE_RUNNING);
    }
    cancelled = async_cancel_requested(task);
    SDL_UnlockMutex(executor->lock);

    if (task->trace_task_id != 0 && jce_trace_enabled()) {
        uint64_t now = jce_time_ticks_ns();
        uint64_t queue_ns = now >= task->submitted_ns
            ? now - task->submitted_ns : 0;
        trace_span_id = jce_trace_next_id();
        trace_started_ns = now;
        jce_trace_task_begin(trace_span_id, task->trace_task_id,
                             task->debug_name, queue_ns);
    }

    if (!cancelled) {
        JceAsyncContext ctx;
        ctx.task = task;
        JCE_PROFILE_ZONE_N(task->debug_name);
        result = task->work(&ctx, task->user_data);
        JCE_PROFILE_ZONE_END;
    }

    if (async_cancel_requested(task) ||
        result == JCE_ASYNC_RUN_CANCELLED)
        terminal = JCE_ASYNC_STATE_CANCELLED;
    else if (result == JCE_ASYNC_RUN_FAILED)
        terminal = JCE_ASYNC_STATE_FAILED;
    else
        terminal = JCE_ASYNC_STATE_SUCCEEDED;

    if (trace_span_id != 0) {
        uint64_t ended_ns = jce_time_ticks_ns();
        JceTraceTaskState trace_state = JCE_TRACE_TASK_SUCCEEDED;
        if (terminal == JCE_ASYNC_STATE_FAILED)
            trace_state = JCE_TRACE_TASK_FAILED;
        else if (terminal == JCE_ASYNC_STATE_CANCELLED)
            trace_state = JCE_TRACE_TASK_CANCELLED;
        jce_trace_task_end(
            trace_span_id, task->trace_task_id, task->debug_name, trace_state,
            ended_ns >= trace_started_ns ? ended_ns - trace_started_ns : 0);
        jce_trace_task_restore_id(previous_trace_task);
    }
    async_task_finish(task, terminal);
    SDL_SetTLS(&g_current_task, previous_task, NULL);
    SDL_SetTLS(&g_current_executor, previous_executor, NULL);
}

void jce_async_executor_config_init(JceAsyncExecutorConfig *config)
{
    if (!config)
        return;
    SDL_zero(*config);
    config->struct_size = (uint32_t)sizeof(*config);
    config->mode = JCE_ASYNC_EXECUTION_AUTO;
    config->reserve_latency_worker = true;
    config->max_tasks = ASYNC_DEFAULT_CAPACITY;
    config->cooperative_tasks_per_pump = ASYNC_DEFAULT_WORK_ITEMS;
    config->cooperative_completions_per_pump = ASYNC_DEFAULT_COMPLETIONS;
    config->cooperative_time_budget_us = ASYNC_DEFAULT_TIME_US;
}

void jce_async_task_desc_init(JceAsyncTaskDesc *desc)
{
    if (!desc)
        return;
    SDL_zero(*desc);
    desc->struct_size = (uint32_t)sizeof(*desc);
    desc->priority = JCE_ASYNC_PRIORITY_NORMAL;
    desc->dependency_policy = JCE_ASYNC_DEPENDENCY_REQUIRE_SUCCESS;
}

void jce_async_pump_budget_init(JceAsyncPumpBudget *budget)
{
    if (!budget)
        return;
    budget->max_work_items = ASYNC_DEFAULT_WORK_ITEMS;
    budget->max_completions = ASYNC_DEFAULT_COMPLETIONS;
    budget->max_time_us = ASYNC_DEFAULT_TIME_US;
}

static void async_copy_executor_config(JceAsyncExecutorConfig *dst,
                                       const JceAsyncExecutorConfig *src)
{
    uint32_t size;

    jce_async_executor_config_init(dst);
    if (!src || src->struct_size < sizeof(uint32_t))
        return;
    size = src->struct_size;
    if (size > sizeof(*dst))
        size = (uint32_t)sizeof(*dst);
    SDL_memcpy(dst, src, size);
    dst->struct_size = (uint32_t)sizeof(*dst);
}

static void async_copy_task_desc(JceAsyncTaskDesc *dst,
                                 const JceAsyncTaskDesc *src)
{
    uint32_t size;

    jce_async_task_desc_init(dst);
    if (!src || src->struct_size < sizeof(uint32_t))
        return;
    size = src->struct_size;
    if (size > sizeof(*dst))
        size = (uint32_t)sizeof(*dst);
    SDL_memcpy(dst, src, size);
    dst->struct_size = (uint32_t)sizeof(*dst);
}

JceAsyncExecutor *
jce_async_executor_create(const JceAsyncExecutorConfig *config)
{
    JceAsyncExecutorConfig cfg;
    JceAsyncExecutor *executor;
    uint32_t scheduler_threads;

    async_copy_executor_config(&cfg, config);
    if (cfg.mode != JCE_ASYNC_EXECUTION_AUTO &&
        cfg.mode != JCE_ASYNC_EXECUTION_THREADED &&
        cfg.mode != JCE_ASYNC_EXECUTION_COOPERATIVE)
        return NULL;

#if JCE_PLATFORM_WEB
    if (cfg.mode == JCE_ASYNC_EXECUTION_AUTO ||
        cfg.mode == JCE_ASYNC_EXECUTION_THREADED)
        cfg.mode = JCE_ASYNC_EXECUTION_COOPERATIVE;
#else
    if (cfg.mode == JCE_ASYNC_EXECUTION_AUTO)
        cfg.mode = JCE_ASYNC_EXECUTION_THREADED;
#endif

    if (cfg.max_tasks == 0)
        cfg.max_tasks = ASYNC_DEFAULT_CAPACITY;
    if (cfg.cooperative_tasks_per_pump == 0)
        cfg.cooperative_tasks_per_pump = ASYNC_DEFAULT_WORK_ITEMS;
    if (cfg.cooperative_completions_per_pump == 0)
        cfg.cooperative_completions_per_pump = ASYNC_DEFAULT_COMPLETIONS;
    if (cfg.cooperative_time_budget_us == 0)
        cfg.cooperative_time_budget_us = ASYNC_DEFAULT_TIME_US;

    executor = JCE_NEW(JceAsyncExecutor);
    if (!executor)
        return NULL;
    executor->owner_tid = jce_thread_current_id();
    executor->lock = SDL_CreateMutex();
    executor->idle_cond = SDL_CreateCondition();
    executor->scheduler = enkiNewTaskScheduler();
    if (!executor->lock || !executor->idle_cond || !executor->scheduler) {
        jce_async_executor_destroy(executor);
        return NULL;
    }

    executor->mode = cfg.mode;
    executor->reserve_latency_worker = cfg.reserve_latency_worker;
    executor->worker_count =
        cfg.mode == JCE_ASYNC_EXECUTION_COOPERATIVE ? 0 : cfg.worker_count;
    if (executor->worker_count > ASYNC_MAX_WORKERS) {
        jce_async_executor_destroy(executor);
        return NULL;
    }
    if (cfg.mode == JCE_ASYNC_EXECUTION_THREADED) {
        uint32_t budget = async_process_worker_budget();

        if (executor->worker_count == 0 ||
            executor->worker_count > budget)
            executor->worker_count = budget;
    }

    scheduler_threads = executor->worker_count + 1u;
    enkiInitTaskSchedulerNumThreads(executor->scheduler, scheduler_threads);
    executor->scheduler_running = true;
    executor->max_tasks = cfg.max_tasks;
    executor->default_work_items = cfg.cooperative_tasks_per_pump;
    executor->default_completions =
        cfg.cooperative_completions_per_pump;
    executor->default_time_us = cfg.cooperative_time_budget_us;
    executor->id =
        (uint64_t)(uint32_t)(SDL_AddAtomicInt(&g_executor_ids, 1) + 1);
    executor->accepting = true;
    SDL_strlcpy(executor->debug_name,
                cfg.debug_name ? cfg.debug_name : "jce-async",
                sizeof(executor->debug_name));
    return executor;
}

static JceAsyncTask *async_task_allocate(const JceAsyncTaskDesc *desc)
{
    JceAsyncTask *task = JCE_NEW(JceAsyncTask);

    if (!task)
        return NULL;
    task->wait_mutex = SDL_CreateMutex();
    task->wait_cond = SDL_CreateCondition();
    if (!task->wait_mutex || !task->wait_cond) {
        async_task_destroy(task);
        return NULL;
    }

    SDL_SetAtomicInt(&task->refs, 2);
    SDL_SetAtomicInt(&task->state, JCE_ASYNC_STATE_INVALID);
    SDL_SetAtomicInt(&task->cancel_requested, 0);
    task->work = desc->work;
    task->complete = desc->complete;
    task->cleanup = desc->cleanup;
    task->user_data = desc->user_data;
    task->priority = desc->priority;
    task->dependency_policy = desc->dependency_policy;
    if (desc->timeout_ms != 0) {
        uint64_t now = jce_time_ticks_ms();
        task->deadline_ms = now + (uint64_t)desc->timeout_ms;
        if (task->deadline_ms < now)
            task->deadline_ms = UINT64_MAX;
    }
    SDL_strlcpy(task->debug_name,
                desc->debug_name ? desc->debug_name : "async-task",
                sizeof(task->debug_name));
    if (desc->dependency_count > 0) {
        task->dependency_links = JCE_NEW_ARRAY(
            JceAsyncDependencyLink, desc->dependency_count);
        if (!task->dependency_links) {
            SDL_SetAtomicInt(&task->refs, 1);
            jce_async_task_release(task);
            return NULL;
        }
        task->dependency_count = desc->dependency_count;
    }
    return task;
}

JceAsyncTask *jce_async_submit(JceAsyncExecutor *executor,
                               const JceAsyncTaskDesc *input_desc)
{
    JceAsyncTaskDesc desc;
    JceAsyncTask *task;
    JceAsyncGroup *group;
    bool dependency_failed = false;
    bool rejected = false;
    uint32_t i;

    if (!executor || !input_desc)
        return NULL;
    async_copy_task_desc(&desc, input_desc);
    if (!desc.work ||
        desc.priority < JCE_ASYNC_PRIORITY_CRITICAL ||
        desc.priority > JCE_ASYNC_PRIORITY_BACKGROUND ||
        desc.dependency_policy <
            JCE_ASYNC_DEPENDENCY_REQUIRE_SUCCESS ||
        desc.dependency_policy > JCE_ASYNC_DEPENDENCY_ALWAYS_RUN ||
        (desc.dependency_count > 0 && !desc.dependencies))
        return NULL;

    task = async_task_allocate(&desc);
    if (!task)
        return NULL;
    task->executor = executor;
    task->executor_id = executor->id;
    group = desc.group;

    if (group)
        SDL_LockMutex(group->mutex);
    SDL_LockMutex(executor->lock);

    if (!executor->accepting ||
        executor->live_tasks >= executor->max_tasks ||
        (group && group->executor_id != 0 &&
         group->executor_id != executor->id)) {
        executor->rejected_tasks++;
        rejected = true;
    }

    for (i = 0; !rejected && i < desc.dependency_count; ++i) {
        JceAsyncTask *dependency = desc.dependencies[i];
        if (!dependency || dependency == task ||
            dependency->executor_id != executor->id) {
            executor->rejected_tasks++;
            rejected = true;
        }
    }

    if (!rejected) {
        if (executor->mode == JCE_ASYNC_EXECUTION_COOPERATIVE)
            task->target_thread = 0;
        else
            task->target_thread =
                async_choose_worker_locked(executor, task->priority);

        task->pinned = enkiCreatePinnedTask(executor->scheduler,
                                            async_task_execute,
                                            task->target_thread);
        if (!task->pinned) {
            executor->rejected_tasks++;
            rejected = true;
        } else {
            enkiSetPriorityPinnedTask(task->pinned,
                                     async_enki_priority(task->priority));
        }
    }

    if (!rejected) {
        if (task->target_thread > 0) {
            uint32_t worker = task->target_thread - 1u;

            executor->worker_pending[worker]++;
            task->worker_slot_counted = true;
        }

        for (i = 0; i < desc.dependency_count; ++i) {
            JceAsyncTask *dependency = desc.dependencies[i];
            JceAsyncState state =
                (JceAsyncState)SDL_GetAtomicInt(&dependency->state);
            if (async_state_terminal(state)) {
                if (desc.dependency_policy ==
                        JCE_ASYNC_DEPENDENCY_REQUIRE_SUCCESS &&
                    state != JCE_ASYNC_STATE_SUCCEEDED)
                    dependency_failed = true;
                continue;
            }

            task->dependency_links[i].child = task;
            task->dependency_links[i].active = true;
            task->dependency_links[i].next =
                dependency->dependents_head;
            dependency->dependents_head = &task->dependency_links[i];
            task->pending_dependencies++;
            jce_async_task_retain(task);
        }

        task->submitted_ns = jce_time_ticks_ns();
        if (jce_trace_enabled()) {
            task->trace_task_id = jce_trace_next_id();
            task->trace_parent_id = jce_trace_task_current_id();
            jce_trace_task_submit(task->trace_task_id,
                                  task->trace_parent_id,
                                  JCE_TRACE_TASK_ASYNC,
                                  task->debug_name);
        }
        async_active_add_locked(executor, task);
        executor->live_tasks++;
        if (executor->live_tasks > executor->peak_live_tasks)
            executor->peak_live_tasks = executor->live_tasks;
        executor->unfinished_tasks++;
        executor->submitted_tasks++;
        async_group_attach_locked(group, executor, task);

        if ((group &&
             SDL_GetAtomicInt(&group->cancel_requested) != 0) ||
            dependency_failed)
            SDL_SetAtomicInt(&task->cancel_requested, 1);

        if (task->pending_dependencies > 0 &&
            !async_cancel_requested(task)) {
            executor->waiting_tasks++;
            async_task_set_state(task, JCE_ASYNC_STATE_WAITING);
        } else {
            async_increment_queued_locked(executor);
            async_task_set_state(task, JCE_ASYNC_STATE_QUEUED);
            async_schedule_locked(executor, task);
        }
    }

    SDL_UnlockMutex(executor->lock);
    if (group)
        SDL_UnlockMutex(group->mutex);

    if (rejected) {
        if (task->pinned)
            enkiDeletePinnedTask(executor->scheduler, task->pinned);
        task->pinned = NULL;
        task->cleanup = NULL;
        SDL_SetAtomicInt(&task->refs, 1);
        jce_async_task_release(task);
        return NULL;
    }
    return task;
}

JceAsyncState jce_async_task_state(const JceAsyncTask *task)
{
    if (!task)
        return JCE_ASYNC_STATE_INVALID;
    return (JceAsyncState)SDL_GetAtomicInt(
        (SDL_AtomicInt *)&task->state);
}

bool jce_async_task_is_terminal(const JceAsyncTask *task)
{
    return async_state_terminal(jce_async_task_state(task));
}

bool jce_async_task_cancel(JceAsyncTask *task)
{
    JceAsyncExecutor *executor;
    JceAsyncState state;

    if (!task || jce_async_task_is_terminal(task))
        return false;
    if (!SDL_CompareAndSwapAtomicInt(&task->cancel_requested, 0, 1))
        return false;

    SDL_LockMutex(task->wait_mutex);
    executor = task->executor;
    SDL_UnlockMutex(task->wait_mutex);
    if (!executor)
        return true;

    SDL_LockMutex(executor->lock);
    state = (JceAsyncState)SDL_GetAtomicInt(&task->state);
    if (state == JCE_ASYNC_STATE_WAITING) {
        if (executor->waiting_tasks > 0)
            executor->waiting_tasks--;
        async_increment_queued_locked(executor);
        async_task_set_state(task, JCE_ASYNC_STATE_QUEUED);
        async_schedule_locked(executor, task);
    }
    SDL_UnlockMutex(executor->lock);
    return true;
}

static bool async_timeout_expired(uint64_t started_ms, uint32_t timeout_ms)
{
    return timeout_ms != JCE_ASYNC_WAIT_INFINITE &&
           jce_time_ticks_ms() - started_ms >= (uint64_t)timeout_ms;
}

static Sint32 async_wait_timeout_slice(uint64_t timeout_ms)
{
    return timeout_ms > (uint64_t)INT_MAX
        ? (Sint32)INT_MAX : (Sint32)timeout_ms;
}

JceAsyncWaitResult
jce_async_task_wait_timeout(JceAsyncTask *task, uint32_t timeout_ms)
{
    JceAsyncExecutor *executor;
    uint64_t started_ms;
    uint64_t trace_wait_id = 0;
    uint64_t trace_started_ns = 0;
    uint64_t trace_parent_id = 0;
    JceAsyncWaitResult result = JCE_ASYNC_WAIT_COMPLETED;

    if (!task)
        return JCE_ASYNC_WAIT_INVALID;
    if (jce_async_task_is_terminal(task))
        return JCE_ASYNC_WAIT_COMPLETED;

    executor = (JceAsyncExecutor *)SDL_GetTLS(&g_current_executor);
    if (executor && executor->id == task->executor_id)
        return JCE_ASYNC_WAIT_WOULD_DEADLOCK;

    started_ms = jce_time_ticks_ms();
    if (task->trace_task_id != 0 && jce_trace_enabled()) {
        trace_wait_id = jce_trace_next_id();
        trace_started_ns = jce_time_ticks_ns();
        trace_parent_id = jce_trace_task_current_id();
        if (trace_parent_id == 0)
            trace_parent_id = task->trace_task_id;
        jce_trace_wait_begin(trace_wait_id, trace_parent_id,
                             task->debug_name);
    }
    SDL_LockMutex(task->wait_mutex);
    executor = task->executor;
    SDL_UnlockMutex(task->wait_mutex);

    if (executor &&
        executor->mode == JCE_ASYNC_EXECUTION_COOPERATIVE &&
        jce_async_executor_is_owner(executor)) {
        JceAsyncPumpBudget budget;
        jce_async_pump_budget_init(&budget);
        budget.max_work_items = 1;
        budget.max_completions = 16;
        while (!jce_async_task_is_terminal(task)) {
            if (async_timeout_expired(started_ms, timeout_ms)) {
                result = JCE_ASYNC_WAIT_TIMED_OUT;
                goto done;
            }
            if (jce_async_executor_pump(executor, &budget) == 0)
                jce_thread_sleep_ms(1);
        }
        goto done;
    }

    SDL_LockMutex(task->wait_mutex);
    while (!async_state_terminal((JceAsyncState)
            SDL_GetAtomicInt(&task->state))) {
        uint64_t wait_ms = UINT64_MAX;

        if (async_deadline_expired(task)) {
            SDL_UnlockMutex(task->wait_mutex);
            (void)jce_async_task_cancel(task);
            SDL_LockMutex(task->wait_mutex);
            continue;
        }

        if (timeout_ms != JCE_ASYNC_WAIT_INFINITE) {
            uint64_t elapsed = jce_time_ticks_ms() - started_ms;
            if (elapsed >= timeout_ms) {
                SDL_UnlockMutex(task->wait_mutex);
                result = JCE_ASYNC_WAIT_TIMED_OUT;
                goto done;
            }
            wait_ms = (uint64_t)timeout_ms - elapsed;
        }
        if (task->deadline_ms != 0) {
            uint64_t now = jce_time_ticks_ms();
            uint64_t deadline_wait =
                task->deadline_ms > now ? task->deadline_ms - now : 1u;
            if (deadline_wait < wait_ms)
                wait_ms = deadline_wait;
        }

        if (wait_ms == UINT64_MAX) {
            SDL_WaitCondition(task->wait_cond, task->wait_mutex);
        } else {
            bool woke = SDL_WaitConditionTimeout(
                task->wait_cond, task->wait_mutex,
                async_wait_timeout_slice(wait_ms));
            if (!woke && !async_deadline_expired(task) &&
                async_timeout_expired(started_ms, timeout_ms) &&
                !async_state_terminal((JceAsyncState)
                    SDL_GetAtomicInt(&task->state))) {
                SDL_UnlockMutex(task->wait_mutex);
                result = JCE_ASYNC_WAIT_TIMED_OUT;
                goto done;
            }
        }
    }
    SDL_UnlockMutex(task->wait_mutex);

done:
    if (trace_wait_id != 0) {
        uint64_t ended_ns = jce_time_ticks_ns();
        jce_trace_wait_end(
            trace_wait_id, trace_parent_id, task->debug_name,
            ended_ns >= trace_started_ns ? ended_ns - trace_started_ns : 0);
    }
    return result;
}

void jce_async_task_wait(JceAsyncTask *task)
{
    (void)jce_async_task_wait_timeout(task, JCE_ASYNC_WAIT_INFINITE);
}

float jce_async_task_progress(const JceAsyncTask *task)
{
    float progress;

    if (!task)
        return 0.0f;
    SDL_LockMutex(task->wait_mutex);
    progress = task->progress;
    SDL_UnlockMutex(task->wait_mutex);
    return progress;
}

void *jce_async_task_result(const JceAsyncTask *task)
{
    void *result;

    if (!task)
        return NULL;
    SDL_LockMutex(task->wait_mutex);
    result = task->result;
    SDL_UnlockMutex(task->wait_mutex);
    return result;
}

int32_t jce_async_task_error_code(const JceAsyncTask *task)
{
    int32_t code;

    if (!task)
        return 0;
    SDL_LockMutex(task->wait_mutex);
    code = task->error_code;
    SDL_UnlockMutex(task->wait_mutex);
    return code;
}

const char *jce_async_task_error_message(const JceAsyncTask *task)
{
    JceAsyncTask *mutable_task = (JceAsyncTask *)task;
    const char *message;

    if (!task || !jce_async_task_is_terminal(task))
        return "";
    SDL_LockMutex(mutable_task->wait_mutex);
    message = mutable_task->error_message;
    SDL_UnlockMutex(mutable_task->wait_mutex);
    return message;
}

const char *jce_async_task_debug_name(const JceAsyncTask *task)
{
    return task ? task->debug_name : "";
}

bool jce_async_context_cancel_requested(const JceAsyncContext *ctx)
{
    return ctx && async_cancel_requested(ctx->task);
}

void jce_async_context_set_progress(JceAsyncContext *ctx, float progress)
{
    JceAsyncTask *task;

    if (!ctx || !ctx->task)
        return;
    task = ctx->task;
    if (progress < 0.0f)
        progress = 0.0f;
    if (progress > 1.0f)
        progress = 1.0f;
    SDL_LockMutex(task->wait_mutex);
    if (progress > task->progress)
        task->progress = progress;
    SDL_UnlockMutex(task->wait_mutex);
}

bool jce_async_context_set_result(JceAsyncContext *ctx,
                                  void *result,
                                  JceAsyncResultDestroyFn destroy)
{
    JceAsyncTask *task;

    if (!ctx || !ctx->task)
        return false;
    task = ctx->task;
    SDL_LockMutex(task->wait_mutex);
    if (task->result || task->result_destroy) {
        SDL_UnlockMutex(task->wait_mutex);
        return false;
    }
    task->result = result;
    task->result_destroy = destroy;
    SDL_UnlockMutex(task->wait_mutex);
    return true;
}

void jce_async_context_fail(JceAsyncContext *ctx,
                            int32_t error_code,
                            const char *message)
{
    JceAsyncTask *task;

    if (!ctx || !ctx->task)
        return;
    task = ctx->task;
    SDL_LockMutex(task->wait_mutex);
    task->error_code = error_code;
    SDL_strlcpy(task->error_message, message ? message : "",
                sizeof(task->error_message));
    SDL_UnlockMutex(task->wait_mutex);
}

static bool async_budget_elapsed(uint64_t started_ns, uint64_t max_time_us)
{
    return max_time_us != 0 &&
           (jce_time_ticks_ns() - started_ns) / 1000u >= max_time_us;
}

static uint32_t async_pump_work(JceAsyncExecutor *executor,
                                uint32_t max_items,
                                uint64_t started_ns,
                                uint64_t max_time_us)
{
    uint32_t count = 0;

    if (executor->mode != JCE_ASYNC_EXECUTION_COOPERATIVE)
        return 0;

    while (count < max_items &&
           !async_budget_elapsed(started_ns, max_time_us)) {
        JceAsyncTask *task;

        SDL_LockMutex(executor->lock);
        task = async_cooperative_pop_locked(executor);
        SDL_UnlockMutex(executor->lock);
        if (!task)
            break;

        enkiAddPinnedTaskArgs(executor->scheduler, task->pinned, task);
        enkiRunPinnedTasks(executor->scheduler);
        count++;
    }
    return count;
}

static JceAsyncTask *
async_completion_take_ready_locked(JceAsyncExecutor *executor)
{
    JceAsyncTask *previous = NULL;
    JceAsyncTask *task = executor->completion_head;

    while (task) {
        if (enkiIsPinnedTaskComplete(executor->scheduler,
                                     task->pinned)) {
            if (previous)
                previous->completion_next = task->completion_next;
            else
                executor->completion_head = task->completion_next;
            if (executor->completion_tail == task)
                executor->completion_tail = previous;
            task->completion_next = NULL;
            task->completion_claimed = true;
            if (executor->pending_completions > 0)
                executor->pending_completions--;
            return task;
        }
        previous = task;
        task = task->completion_next;
    }
    return NULL;
}

static void async_task_run_completion(JceAsyncTask *task)
{
    void *previous_executor;
    void *previous_task;

    previous_executor = SDL_GetTLS(&g_current_executor);
    previous_task = SDL_GetTLS(&g_current_task);
    SDL_SetTLS(&g_current_executor, task->executor, NULL);
    SDL_SetTLS(&g_current_task, task, NULL);

    if (task->complete)
        task->complete(task, task->user_data);
    if (!task->cleanup_done && task->cleanup) {
        task->cleanup(task->user_data);
        task->cleanup_done = true;
    }

    SDL_SetTLS(&g_current_task, previous_task, NULL);
    SDL_SetTLS(&g_current_executor, previous_executor, NULL);
}

static void async_task_reap(JceAsyncExecutor *executor,
                            JceAsyncTask *task)
{
    SDL_LockMutex(executor->lock);
    task->callback_done = true;
    async_active_remove_locked(executor, task);
    if (executor->live_tasks > 0)
        executor->live_tasks--;
    SDL_LockMutex(task->wait_mutex);
    task->executor = NULL;
    SDL_UnlockMutex(task->wait_mutex);
    enkiDeletePinnedTask(executor->scheduler, task->pinned);
    task->pinned = NULL;
    SDL_BroadcastCondition(executor->idle_cond);
    SDL_UnlockMutex(executor->lock);

    jce_async_task_release(task);
}

static uint32_t async_pump_completions(JceAsyncExecutor *executor,
                                       uint32_t max_completions,
                                       uint64_t started_ns,
                                       uint64_t max_time_us)
{
    uint32_t count = 0;

    while (count < max_completions &&
           !async_budget_elapsed(started_ns, max_time_us)) {
        JceAsyncTask *task;

        SDL_LockMutex(executor->lock);
        task = async_completion_take_ready_locked(executor);
        SDL_UnlockMutex(executor->lock);
        if (!task)
            break;

        async_task_run_completion(task);
        async_task_reap(executor, task);
        count++;
    }
    return count;
}

uint32_t jce_async_executor_pump(JceAsyncExecutor *executor,
                                 const JceAsyncPumpBudget *input_budget)
{
    JceAsyncPumpBudget budget;
    uint64_t started_ns;
    uint32_t count;

    if (!executor || !jce_async_executor_is_owner(executor))
        return 0;
    if (input_budget)
        budget = *input_budget;
    else {
        budget.max_work_items = executor->default_work_items;
        budget.max_completions = executor->default_completions;
        budget.max_time_us = executor->default_time_us;
    }

    started_ns = jce_time_ticks_ns();
    SDL_LockMutex(executor->lock);
    async_expire_waiting_locked(executor);
    SDL_UnlockMutex(executor->lock);

    JCE_PROFILE_ZONE_N("AsyncExecutor::Pump");
    count = async_pump_work(executor, budget.max_work_items,
                            started_ns, budget.max_time_us);
    count += async_pump_completions(executor, budget.max_completions,
                                    started_ns, budget.max_time_us);
    JCE_PROFILE_ZONE_END;
    return count;
}

bool jce_async_executor_is_owner(const JceAsyncExecutor *executor)
{
    return executor &&
           executor->owner_tid == jce_thread_current_id();
}

JceAsyncExecutionMode
jce_async_executor_mode(const JceAsyncExecutor *executor)
{
    return executor ? executor->mode : JCE_ASYNC_EXECUTION_AUTO;
}

void jce_async_executor_get_stats(const JceAsyncExecutor *input_executor,
                                  JceAsyncExecutorStats *out_stats)
{
    JceAsyncExecutor *executor = (JceAsyncExecutor *)input_executor;

    if (!out_stats)
        return;
    SDL_zero(*out_stats);
    if (!executor)
        return;

    SDL_LockMutex(executor->lock);
    out_stats->mode = executor->mode;
    out_stats->worker_count = executor->worker_count;
    out_stats->capacity = executor->max_tasks;
    out_stats->live_tasks = executor->live_tasks;
    out_stats->waiting_tasks = executor->waiting_tasks;
    out_stats->queued_tasks = executor->queued_tasks;
    out_stats->running_tasks = executor->running_tasks;
    out_stats->pending_completions = executor->pending_completions;
    out_stats->peak_live_tasks = executor->peak_live_tasks;
    out_stats->peak_queued_tasks = executor->peak_queued_tasks;
    out_stats->submitted_tasks = executor->submitted_tasks;
    out_stats->succeeded_tasks = executor->succeeded_tasks;
    out_stats->failed_tasks = executor->failed_tasks;
    out_stats->cancelled_tasks = executor->cancelled_tasks;
    out_stats->rejected_tasks = executor->rejected_tasks;
    out_stats->total_queue_time_ns = executor->total_queue_time_ns;
    out_stats->max_queue_time_ns = executor->max_queue_time_ns;
    out_stats->total_run_time_ns = executor->total_run_time_ns;
    out_stats->max_run_time_ns = executor->max_run_time_ns;
    SDL_UnlockMutex(executor->lock);
}

JceAsyncGroup *jce_async_group_create(void)
{
    JceAsyncGroup *group = JCE_NEW(JceAsyncGroup);

    if (!group)
        return NULL;
    SDL_SetAtomicInt(&group->refs, 1);
    group->mutex = SDL_CreateMutex();
    group->cond = SDL_CreateCondition();
    if (!group->mutex || !group->cond) {
        jce_async_group_release(group);
        return NULL;
    }
    return group;
}

JceAsyncGroup *jce_async_group_retain(JceAsyncGroup *group)
{
    if (group)
        SDL_AddAtomicInt(&group->refs, 1);
    return group;
}

void jce_async_group_release(JceAsyncGroup *group)
{
    if (!group)
        return;
    if (SDL_AddAtomicInt(&group->refs, -1) != 1)
        return;
    if (group->cond)
        SDL_DestroyCondition(group->cond);
    if (group->mutex)
        SDL_DestroyMutex(group->mutex);
    JCE_FREE(group);
}

bool jce_async_group_cancel(JceAsyncGroup *group)
{
    JceAsyncTask *task;
    bool changed;

    if (!group)
        return false;
    changed = SDL_CompareAndSwapAtomicInt(
        &group->cancel_requested, 0, 1);

    SDL_LockMutex(group->mutex);
    for (task = group->head; task; task = task->group_next)
        jce_async_task_cancel(task);
    SDL_UnlockMutex(group->mutex);
    return changed;
}

bool jce_async_group_is_cancelled(const JceAsyncGroup *group)
{
    return group &&
           SDL_GetAtomicInt(
               (SDL_AtomicInt *)&group->cancel_requested) != 0;
}

uint32_t jce_async_group_pending(const JceAsyncGroup *input_group)
{
    JceAsyncGroup *group = (JceAsyncGroup *)input_group;
    uint32_t pending;

    if (!group)
        return 0;
    SDL_LockMutex(group->mutex);
    pending = group->pending;
    SDL_UnlockMutex(group->mutex);
    return pending;
}

JceAsyncWaitResult
jce_async_group_wait_timeout(JceAsyncGroup *group, uint32_t timeout_ms)
{
    JceAsyncExecutor *executor;
    uint64_t started_ms;

    if (!group)
        return JCE_ASYNC_WAIT_INVALID;
    if (jce_async_group_pending(group) == 0)
        return JCE_ASYNC_WAIT_COMPLETED;

    executor = (JceAsyncExecutor *)SDL_GetTLS(&g_current_executor);
    if (executor && executor->id == group->executor_id)
        return JCE_ASYNC_WAIT_WOULD_DEADLOCK;

    started_ms = jce_time_ticks_ms();
    SDL_LockMutex(group->mutex);
    executor = group->executor;
    SDL_UnlockMutex(group->mutex);
    if (executor &&
        executor->mode == JCE_ASYNC_EXECUTION_COOPERATIVE &&
        jce_async_executor_is_owner(executor)) {
        JceAsyncPumpBudget budget;
        jce_async_pump_budget_init(&budget);
        budget.max_work_items = 1;
        while (jce_async_group_pending(group) != 0) {
            if (async_timeout_expired(started_ms, timeout_ms))
                return JCE_ASYNC_WAIT_TIMED_OUT;
            if (jce_async_executor_pump(executor, &budget) == 0)
                jce_thread_sleep_ms(1);
        }
        return JCE_ASYNC_WAIT_COMPLETED;
    }

    SDL_LockMutex(group->mutex);
    while (group->pending != 0) {
        if (timeout_ms == JCE_ASYNC_WAIT_INFINITE) {
            SDL_WaitCondition(group->cond, group->mutex);
        } else {
            uint64_t elapsed = jce_time_ticks_ms() - started_ms;
            uint32_t remaining;
            if (elapsed >= timeout_ms) {
                SDL_UnlockMutex(group->mutex);
                return JCE_ASYNC_WAIT_TIMED_OUT;
            }
            remaining = timeout_ms - (uint32_t)elapsed;
            if (!SDL_WaitConditionTimeout(group->cond, group->mutex,
                                          async_wait_timeout_slice(remaining)) &&
                group->pending != 0) {
                SDL_UnlockMutex(group->mutex);
                return JCE_ASYNC_WAIT_TIMED_OUT;
            }
        }
    }
    SDL_UnlockMutex(group->mutex);
    return JCE_ASYNC_WAIT_COMPLETED;
}

void jce_async_group_wait(JceAsyncGroup *group)
{
    (void)jce_async_group_wait_timeout(group, JCE_ASYNC_WAIT_INFINITE);
}

static void async_shutdown_cancel_locked(JceAsyncExecutor *executor,
                                         JceAsyncShutdownMode mode)
{
    JceAsyncTask *task;

    for (task = executor->active_head; task; task = task->active_next) {
        JceAsyncState state = (JceAsyncState)SDL_GetAtomicInt(&task->state);
        bool cancel = mode == JCE_ASYNC_SHUTDOWN_CANCEL_ALL ||
                      (mode == JCE_ASYNC_SHUTDOWN_CANCEL_PENDING &&
                       (state == JCE_ASYNC_STATE_WAITING ||
                        state == JCE_ASYNC_STATE_QUEUED));
        if (!cancel || async_state_terminal(state))
            continue;

        SDL_SetAtomicInt(&task->cancel_requested, 1);
        if (state == JCE_ASYNC_STATE_WAITING) {
            if (executor->waiting_tasks > 0)
                executor->waiting_tasks--;
            async_increment_queued_locked(executor);
            async_task_set_state(task, JCE_ASYNC_STATE_QUEUED);
            async_schedule_locked(executor, task);
        }
    }
}

bool jce_async_executor_shutdown(JceAsyncExecutor *executor,
                                 JceAsyncShutdownMode mode,
                                 uint32_t timeout_ms)
{
    uint64_t started_ms;

    if (!executor || !jce_async_executor_is_owner(executor) ||
        SDL_GetTLS(&g_current_executor) == executor)
        return false;
    if (mode < JCE_ASYNC_SHUTDOWN_DRAIN ||
        mode > JCE_ASYNC_SHUTDOWN_CANCEL_ALL)
        return false;
    if (executor->shutdown_complete)
        return true;

    started_ms = jce_time_ticks_ms();
    SDL_LockMutex(executor->lock);
    executor->accepting = false;
    async_shutdown_cancel_locked(executor, mode);
    SDL_UnlockMutex(executor->lock);

    for (;;) {
        uint32_t unfinished;

        SDL_LockMutex(executor->lock);
        unfinished = executor->unfinished_tasks;
        SDL_UnlockMutex(executor->lock);
        if (unfinished == 0)
            break;
        if (async_timeout_expired(started_ms, timeout_ms))
            return false;

        if (executor->mode == JCE_ASYNC_EXECUTION_COOPERATIVE) {
            JceAsyncPumpBudget budget;
            jce_async_pump_budget_init(&budget);
            budget.max_work_items = UINT32_MAX;
            budget.max_completions = UINT32_MAX;
            budget.max_time_us = 0;
            jce_async_executor_pump(executor, &budget);
        } else {
            SDL_LockMutex(executor->lock);
            if (executor->unfinished_tasks != 0)
                SDL_WaitConditionTimeout(executor->idle_cond,
                                         executor->lock, 1);
            SDL_UnlockMutex(executor->lock);
        }
    }

    if (executor->scheduler_running) {
        enkiWaitforAllAndShutdown(executor->scheduler);
        executor->scheduler_running = false;
    }

    for (;;) {
        JceAsyncExecutorStats stats;
        JceAsyncPumpBudget budget;

        jce_async_pump_budget_init(&budget);
        budget.max_work_items = UINT32_MAX;
        budget.max_completions = UINT32_MAX;
        budget.max_time_us = 0;
        jce_async_executor_pump(executor, &budget);
        jce_async_executor_get_stats(executor, &stats);
        if (stats.live_tasks == 0)
            break;
        if (async_timeout_expired(started_ms, timeout_ms))
            return false;
        jce_thread_sleep_ms(1);
    }

    executor->shutdown_complete = true;
    return true;
}

void jce_async_executor_destroy(JceAsyncExecutor *executor)
{
    if (!executor)
        return;

    if (executor->scheduler && !executor->shutdown_complete) {
        if (executor->lock && executor->idle_cond) {
            if (!jce_async_executor_shutdown(
                    executor, JCE_ASYNC_SHUTDOWN_DRAIN,
                    JCE_ASYNC_WAIT_INFINITE))
                return;
        } else if (executor->scheduler_running) {
            enkiWaitforAllAndShutdown(executor->scheduler);
            executor->scheduler_running = false;
        }
    }

    if (executor->scheduler)
        enkiDeleteTaskScheduler(executor->scheduler);
    if (executor->idle_cond)
        SDL_DestroyCondition(executor->idle_cond);
    if (executor->lock)
        SDL_DestroyMutex(executor->lock);
    JCE_FREE(executor);
}

static JceAsyncExecutor *g_default_executor;
static SDL_AtomicInt      g_default_lock;
static bool               g_default_closing;

static void async_default_lock(void)
{
    while (!SDL_CompareAndSwapAtomicInt(&g_default_lock, 0, 1))
        SDL_Delay(0);
}

static void async_default_unlock(void)
{
    SDL_SetAtomicInt(&g_default_lock, 0);
}

JceAsyncExecutor *jce_async_default_executor(void)
{
    JceAsyncExecutor *executor;

    async_default_lock();
    if (!g_default_executor && !g_default_closing) {
        JceAsyncExecutorConfig config;
        jce_async_executor_config_init(&config);
        config.debug_name = "jce-default-async";
        g_default_executor = jce_async_executor_create(&config);
    }
    executor = g_default_closing ? NULL : g_default_executor;
    async_default_unlock();
    return executor;
}

uint32_t jce_async_default_pump(const JceAsyncPumpBudget *budget)
{
    JceAsyncExecutor *executor;

    async_default_lock();
    executor = g_default_closing ? NULL : g_default_executor;
    async_default_unlock();
    return executor ? jce_async_executor_pump(executor, budget) : 0;
}

bool jce_async_default_shutdown(JceAsyncShutdownMode mode,
                                uint32_t timeout_ms)
{
    JceAsyncExecutor *executor;

    async_default_lock();
    if (g_default_closing) {
        async_default_unlock();
        return false;
    }
    executor = g_default_executor;
    if (executor)
        g_default_closing = true;
    async_default_unlock();
    if (!executor)
        return true;
    if (!jce_async_executor_shutdown(executor, mode, timeout_ms)) {
        async_default_lock();
        g_default_closing = false;
        async_default_unlock();
        return false;
    }

    async_default_lock();
    if (g_default_executor == executor)
        g_default_executor = NULL;
    async_default_unlock();
    jce_async_executor_destroy(executor);
    async_default_lock();
    g_default_closing = false;
    async_default_unlock();
    return true;
}
