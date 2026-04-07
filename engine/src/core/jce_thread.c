/*
 * jce_thread.c  Thread pool and synchronisation primitives.
 *
 * Implementation uses SDL3 threads, mutexes, and conditions.
 * The pool maintains a work queue; workers sleep until signalled.
 */

#include "jce_thread.h"
#include "jce_memory.h"

#include <SDL3/SDL.h>

/* ================================================================== */
/* Mutex                                                               */
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
/* Condition variable                                                  */
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
/* Thread pool internals                                               */
/* ================================================================== */

/* Linked-list node for the work queue. */
typedef struct WorkItem {
    JceTaskFn       fn;
    void           *arg;
    JceTask        *tracked;   /* NULL for fire-and-forget tasks */
    struct WorkItem *next;
} WorkItem;

struct JceTask {
    SDL_AtomicInt  done;
    JceMutex      *mutex;
    JceCondVar    *cond;
};

struct JceThreadPool {
    SDL_Thread  **threads;
    int           num_threads;

    JceMutex    *queue_mutex;
    JceCondVar  *queue_cond;
    WorkItem    *queue_head;
    WorkItem    *queue_tail;
    bool         shutdown;
};

/* Worker thread entry point. */
static int pool_worker(void *data)
{
    JceThreadPool *pool = (JceThreadPool *)data;

    for (;;) {
        jce_mutex_lock(pool->queue_mutex);

        /* Wait for work or shutdown. */
        while (!pool->queue_head && !pool->shutdown)
            jce_cond_wait(pool->queue_cond, pool->queue_mutex);

        if (pool->shutdown && !pool->queue_head) {
            jce_mutex_unlock(pool->queue_mutex);
            break;
        }

        /* Dequeue. */
        WorkItem *item = pool->queue_head;
        pool->queue_head = item->next;
        if (!pool->queue_head)
            pool->queue_tail = NULL;

        jce_mutex_unlock(pool->queue_mutex);

        /* Execute. */
        item->fn(item->arg);

        /* Signal tracked task completion. */
        if (item->tracked) {
            SDL_SetAtomicInt(&item->tracked->done, 1);
            jce_mutex_lock(item->tracked->mutex);
            jce_cond_broadcast(item->tracked->cond);
            jce_mutex_unlock(item->tracked->mutex);
        }

        JCE_FREE(item);
    }

    return 0;
}

/* ================================================================== */
/* Thread pool public API                                              */
/* ================================================================== */

JceThreadPool *jce_thread_pool_create(int num_threads)
{
    if (num_threads <= 0)
        num_threads = SDL_GetNumLogicalCPUCores();
    if (num_threads < 1)
        num_threads = 2;

    JceThreadPool *pool = JCE_NEW(JceThreadPool);
    if (!pool) return NULL;

    pool->queue_mutex = jce_mutex_create();
    pool->queue_cond  = jce_cond_create();
    if (!pool->queue_mutex || !pool->queue_cond) {
        jce_mutex_destroy(pool->queue_mutex);
        jce_cond_destroy(pool->queue_cond);
        JCE_FREE(pool);
        return NULL;
    }

    pool->num_threads = num_threads;
    pool->threads = JCE_NEW_ARRAY(SDL_Thread *, num_threads);

    for (int i = 0; i < num_threads; i++) {
        char name[32];
        SDL_snprintf(name, sizeof(name), "JCE-Worker-%d", i);
        pool->threads[i] = SDL_CreateThread(pool_worker, name, pool);
    }

    return pool;
}

void jce_thread_pool_destroy(JceThreadPool *pool)
{
    if (!pool) return;

    /* Signal shutdown. */
    jce_mutex_lock(pool->queue_mutex);
    pool->shutdown = true;
    jce_cond_broadcast(pool->queue_cond);
    jce_mutex_unlock(pool->queue_mutex);

    /* Join all workers. */
    for (int i = 0; i < pool->num_threads; i++) {
        if (pool->threads[i])
            SDL_WaitThread(pool->threads[i], NULL);
    }

    /* Drain remaining items. */
    WorkItem *item = pool->queue_head;
    while (item) {
        WorkItem *next = item->next;
        JCE_FREE(item);
        item = next;
    }

    JCE_FREE(pool->threads);
    jce_mutex_destroy(pool->queue_mutex);
    jce_cond_destroy(pool->queue_cond);
    JCE_FREE(pool);
}

static void enqueue(JceThreadPool *pool, JceTaskFn fn, void *arg,
                    JceTask *tracked)
{
    WorkItem *item = JCE_NEW(WorkItem);
    if (!item) return;

    item->fn      = fn;
    item->arg     = arg;
    item->tracked = tracked;
    item->next    = NULL;

    jce_mutex_lock(pool->queue_mutex);
    if (pool->queue_tail)
        pool->queue_tail->next = item;
    else
        pool->queue_head = item;
    pool->queue_tail = item;
    jce_cond_signal(pool->queue_cond);
    jce_mutex_unlock(pool->queue_mutex);
}

void jce_thread_pool_submit(JceThreadPool *pool, JceTaskFn fn, void *arg)
{
    if (!pool || !fn) return;
    enqueue(pool, fn, arg, NULL);
}

JceTask *jce_thread_pool_submit_tracked(JceThreadPool *pool,
                                         JceTaskFn fn, void *arg)
{
    if (!pool || !fn) return NULL;

    JceTask *task = JCE_NEW(JceTask);
    if (!task) return NULL;

    SDL_SetAtomicInt(&task->done, 0);
    task->mutex = jce_mutex_create();
    task->cond  = jce_cond_create();

    enqueue(pool, fn, arg, task);
    return task;
}

void jce_task_wait(JceTask *task)
{
    if (!task) return;
    jce_mutex_lock(task->mutex);
    while (!SDL_GetAtomicInt(&task->done))
        jce_cond_wait(task->cond, task->mutex);
    jce_mutex_unlock(task->mutex);
}

bool jce_task_done(const JceTask *task)
{
    if (!task) return true;
    return SDL_GetAtomicInt((SDL_AtomicInt *)&task->done) != 0;
}

void jce_task_free(JceTask *task)
{
    if (!task) return;
    jce_mutex_destroy(task->mutex);
    jce_cond_destroy(task->cond);
    JCE_FREE(task);
}
