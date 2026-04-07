/*
 * jce_thread.c  Thread pool and synchronisation primitives.
 *
 * Thread pool backed by enkiTS work-stealing task scheduler.
 * Mutex / condition-variable wrappers remain on SDL3.
 */

#include "jce_thread.h"
#include "jce_memory.h"

#include <SDL3/SDL.h>
#include <enkiTS/TaskScheduler_c.h>

/* ================================================================== */
/* Mutex  (SDL3 — enkiTS has no mutex primitive)                       */
/* ================================================================== */

struct JceMutex {
    SDL_Mutex *handle;
};

JceMutex *jce_mutex_create(void)
{
    JceMutex *m = JCE_NEW(JceMutex);
    if (!m) return NULL;
    m->handle = SDL_CreateMutex();
    if (!m->handle) { JCE_FREE(m); return NULL; }
    return m;
}

void jce_mutex_destroy(JceMutex *m)
{
    if (!m) return;
    SDL_DestroyMutex(m->handle);
    JCE_FREE(m);
}

void jce_mutex_lock(JceMutex *m)
{
    if (m) SDL_LockMutex(m->handle);
}

void jce_mutex_unlock(JceMutex *m)
{
    if (m) SDL_UnlockMutex(m->handle);
}

/* ================================================================== */
/* Condition variable  (SDL3)                                          */
/* ================================================================== */

struct JceCondVar {
    SDL_Condition *handle;
};

JceCondVar *jce_cond_create(void)
{
    JceCondVar *c = JCE_NEW(JceCondVar);
    if (!c) return NULL;
    c->handle = SDL_CreateCondition();
    if (!c->handle) { JCE_FREE(c); return NULL; }
    return c;
}

void jce_cond_destroy(JceCondVar *c)
{
    if (!c) return;
    SDL_DestroyCondition(c->handle);
    JCE_FREE(c);
}

void jce_cond_wait(JceCondVar *c, JceMutex *m)
{
    if (c && m)
        SDL_WaitCondition(c->handle, m->handle);
}

void jce_cond_signal(JceCondVar *c)
{
    if (c) SDL_SignalCondition(c->handle);
}

void jce_cond_broadcast(JceCondVar *c)
{
    if (c) SDL_BroadcastCondition(c->handle);
}

/* ================================================================== */
/* enkiTS task adapter                                                 */
/* ================================================================== */

/* Bridge between JceTaskFn(void*) and enkiTS range callback. */
typedef struct TaskAdapter {
    JceTaskFn fn;
    void     *arg;
} TaskAdapter;

static void task_range_adapter(uint32_t start_, uint32_t end_,
                               uint32_t threadnum_, void *pArgs_)
{
    (void)start_; (void)end_; (void)threadnum_;
    TaskAdapter *a = (TaskAdapter *)pArgs_;
    a->fn(a->arg);
}

/* ================================================================== */
/* Tracked task                                                        */
/* ================================================================== */

struct JceTask {
    enkiTaskScheduler *scheduler;   /* back-reference for wait/query */
    enkiTaskSet       *task_set;
    TaskAdapter        adapter;     /* embedded — no separate alloc  */
};

/* ================================================================== */
/* Fire-and-forget cleanup list                                        */
/* ================================================================== */

typedef struct PendingTask {
    enkiTaskSet        *task_set;
    TaskAdapter        *adapter;    /* heap-allocated per submit      */
    struct PendingTask *next;
} PendingTask;

/* ================================================================== */
/* Thread pool                                                         */
/* ================================================================== */

struct JceThreadPool {
    enkiTaskScheduler *scheduler;
    PendingTask       *pending_head;
    SDL_Mutex         *pending_mutex;
};

/* Garbage-collect completed fire-and-forget tasks. */
static void cleanup_pending(JceThreadPool *pool)
{
    SDL_LockMutex(pool->pending_mutex);
    PendingTask **pp = &pool->pending_head;
    while (*pp) {
        PendingTask *p = *pp;
        if (enkiIsTaskSetComplete(pool->scheduler, p->task_set)) {
            *pp = p->next;
            enkiDeleteTaskSet(pool->scheduler, p->task_set);
            JCE_FREE(p->adapter);
            JCE_FREE(p);
        } else {
            pp = &p->next;
        }
    }
    SDL_UnlockMutex(pool->pending_mutex);
}

/* ================================================================== */
/* Thread pool public API                                              */
/* ================================================================== */

JceThreadPool *jce_thread_pool_create(int num_threads)
{
    JceThreadPool *pool = JCE_NEW(JceThreadPool);
    if (!pool) return NULL;

    pool->scheduler = enkiNewTaskScheduler();
    if (!pool->scheduler) { JCE_FREE(pool); return NULL; }

    if (num_threads <= 0)
        enkiInitTaskScheduler(pool->scheduler);          /* auto-detect */
    else
        enkiInitTaskSchedulerNumThreads(pool->scheduler, (uint32_t)num_threads);

    pool->pending_head  = NULL;
    pool->pending_mutex = SDL_CreateMutex();
    return pool;
}

void jce_thread_pool_destroy(JceThreadPool *pool)
{
    if (!pool) return;

    enkiWaitforAllAndShutdown(pool->scheduler);

    /* Drain the pending list (all tasks are complete after shutdown). */
    PendingTask *p = pool->pending_head;
    while (p) {
        PendingTask *next = p->next;
        enkiDeleteTaskSet(pool->scheduler, p->task_set);
        JCE_FREE(p->adapter);
        JCE_FREE(p);
        p = next;
    }

    enkiDeleteTaskScheduler(pool->scheduler);
    SDL_DestroyMutex(pool->pending_mutex);
    JCE_FREE(pool);
}

void jce_thread_pool_submit(JceThreadPool *pool, JceTaskFn fn, void *arg)
{
    if (!pool || !fn) return;

    cleanup_pending(pool);

    TaskAdapter *adapter = JCE_NEW(TaskAdapter);
    if (!adapter) return;
    adapter->fn  = fn;
    adapter->arg = arg;

    /* Pre-allocate tracking node BEFORE submitting so we never lose
       the adapter pointer if the allocation fails. */
    PendingTask *pending = JCE_NEW(PendingTask);
    if (!pending) {
        JCE_FREE(adapter);
        return;
    }

    enkiTaskSet *ts = enkiCreateTaskSet(pool->scheduler, task_range_adapter);
    enkiAddTaskSetArgs(pool->scheduler, ts, adapter, 1);

    pending->task_set = ts;
    pending->adapter  = adapter;

    SDL_LockMutex(pool->pending_mutex);
    pending->next      = pool->pending_head;
    pool->pending_head = pending;
    SDL_UnlockMutex(pool->pending_mutex);
}

JceTask *jce_thread_pool_submit_tracked(JceThreadPool *pool,
                                         JceTaskFn fn, void *arg)
{
    if (!pool || !fn) return NULL;

    JceTask *task = JCE_NEW(JceTask);
    if (!task) return NULL;

    task->scheduler    = pool->scheduler;
    task->adapter.fn   = fn;
    task->adapter.arg  = arg;

    task->task_set = enkiCreateTaskSet(pool->scheduler, task_range_adapter);
    enkiAddTaskSetArgs(pool->scheduler, task->task_set, &task->adapter, 1);

    return task;
}

void jce_task_wait(JceTask *task)
{
    if (!task) return;
    enkiWaitForTaskSet(task->scheduler, task->task_set);
}

bool jce_task_done(const JceTask *task)
{
    if (!task) return true;
    return enkiIsTaskSetComplete(task->scheduler, task->task_set) != 0;
}

void jce_task_free(JceTask *task)
{
    if (!task) return;
    enkiDeleteTaskSet(task->scheduler, task->task_set);
    JCE_FREE(task);
}
