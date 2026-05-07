/*
 * jce_thread.c  Thread pool and synchronisation primitives.
 *
 * Thread pool backed by enkiTS work-stealing task scheduler.
 * Mutex / condition-variable wrappers remain on SDL3.
 */

#include <jce/os/core/jce_thread.h>

#include "jce_memory.h"

#include <enkiTS/TaskScheduler_c.h>
#include <SDL3/SDL.h>

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
/* Long-lived dedicated thread (SDL3 SDL_Thread)                       */
/* ================================================================== */

struct JceThread {
    SDL_Thread *handle;
    JceThreadFn fn;
    void       *arg;
};

static int sdl_thread_trampoline(void *user)
{
    JceThread *t = (JceThread *)user;
    if (t && t->fn) t->fn(t->arg);
    return 0;
}

JceThread *jce_thread_create(JceThreadFn fn, void *arg, const char *name)
{
    if (!fn) return NULL;
    JceThread *t = JCE_NEW(JceThread);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    t->handle = SDL_CreateThread(sdl_thread_trampoline,
                                 name ? name : "jce_thread", t);
    if (!t->handle) { JCE_FREE(t); return NULL; }
    return t;
}

void jce_thread_join(JceThread *t)
{
    if (!t) return;
    if (t->handle) SDL_WaitThread(t->handle, NULL);
    JCE_FREE(t);
}

void jce_thread_sleep_ms(uint32_t ms)
{
    SDL_Delay(ms);
}

/* ── Main-thread tracking ─────────────────────────────────────────── */

static SDL_ThreadID g_main_thread_id;
static int          g_main_thread_marked;

void jce_thread_mark_main(void)
{
    g_main_thread_id     = SDL_GetCurrentThreadID();
    g_main_thread_marked = 1;
}

bool jce_thread_is_main(void)
{
    if (!g_main_thread_marked) return true; /* unmarked → single-threaded */
    return SDL_GetCurrentThreadID() == g_main_thread_id;
}

uint64_t jce_thread_current_id(void)
{
    return (uint64_t)SDL_GetCurrentThreadID();
}

/* ── Thread-local storage ─────────────────────────────────────────── */

struct JceTLS {
    SDL_TLSID                 id;   /* SDL_AtomicInt; zero-init OK */
    SDL_TLSDestructorCallback dtor;
};

JceTLS *jce_tls_create(JceTLSDestructor dtor)
{
    JceTLS *tls = (JceTLS *)JCE_MALLOC(sizeof(*tls));
    if (!tls) return NULL;
    SDL_memset(&tls->id, 0, sizeof(tls->id));
    tls->dtor = (SDL_TLSDestructorCallback)dtor;
    return tls;
}

void *jce_tls_get(JceTLS *tls)
{
    if (!tls) return NULL;
    return SDL_GetTLS(&tls->id);
}

void jce_tls_set(JceTLS *tls, void *value)
{
    if (!tls) return;
    SDL_SetTLS(&tls->id, value, tls->dtor);
}

/* ================================================================== */
/* Atomics                                                             */
/*                                                                     */
/* Int32 backed by SDL3 lock-free atomics; U64 backed by mutex (SDL3   */
/* does not expose 64-bit atomics on all targets). Operations are      */
/* sequentially consistent.                                            */
/* ================================================================== */

struct JceAtomicI32 {
    SDL_AtomicInt v;
};

JceAtomicI32 *jce_atomic_i32_create(int32_t initial)
{
    JceAtomicI32 *a = JCE_NEW(JceAtomicI32);
    if (!a) return NULL;
    SDL_SetAtomicInt(&a->v, (int)initial);
    return a;
}

void jce_atomic_i32_destroy(JceAtomicI32 *a)
{
    if (a) JCE_FREE(a);
}

int32_t jce_atomic_i32_load(const JceAtomicI32 *a)
{
    if (!a) return 0;
    return (int32_t)SDL_GetAtomicInt((SDL_AtomicInt *)&a->v);
}

void jce_atomic_i32_store(JceAtomicI32 *a, int32_t v)
{
    if (a) SDL_SetAtomicInt(&a->v, (int)v);
}

int32_t jce_atomic_i32_exchange(JceAtomicI32 *a, int32_t v)
{
    if (!a) return 0;
    int prev;
    do {
        prev = SDL_GetAtomicInt(&a->v);
    } while (!SDL_CompareAndSwapAtomicInt(&a->v, prev, (int)v));
    return (int32_t)prev;
}

int32_t jce_atomic_i32_add(JceAtomicI32 *a, int32_t v)
{
    if (!a) return 0;
    return (int32_t)SDL_AddAtomicInt(&a->v, (int)v);
}

struct JceAtomicU64 {
    SDL_Mutex *m;
    uint64_t   v;
};

JceAtomicU64 *jce_atomic_u64_create(uint64_t initial)
{
    JceAtomicU64 *a = JCE_NEW(JceAtomicU64);
    if (!a) return NULL;
    a->m = SDL_CreateMutex();
    if (!a->m) { JCE_FREE(a); return NULL; }
    a->v = initial;
    return a;
}

void jce_atomic_u64_destroy(JceAtomicU64 *a)
{
    if (!a) return;
    SDL_DestroyMutex(a->m);
    JCE_FREE(a);
}

uint64_t jce_atomic_u64_load(const JceAtomicU64 *a)
{
    if (!a) return 0;
    SDL_LockMutex(a->m);
    uint64_t v = a->v;
    SDL_UnlockMutex(a->m);
    return v;
}

void jce_atomic_u64_store(JceAtomicU64 *a, uint64_t v)
{
    if (!a) return;
    SDL_LockMutex(a->m);
    a->v = v;
    SDL_UnlockMutex(a->m);
}

uint64_t jce_atomic_u64_add(JceAtomicU64 *a, uint64_t v)
{
    if (!a) return 0;
    SDL_LockMutex(a->m);
    uint64_t prev = a->v;
    a->v += v;
    SDL_UnlockMutex(a->m);
    return prev;
}

/* ================================================================== */
/* Semaphore (SDL3)                                                    */
/* ================================================================== */

struct JceSemaphore {
    SDL_Semaphore *handle;
};

JceSemaphore *jce_semaphore_create(uint32_t initial)
{
    JceSemaphore *s = JCE_NEW(JceSemaphore);
    if (!s) return NULL;
    s->handle = SDL_CreateSemaphore(initial);
    if (!s->handle) { JCE_FREE(s); return NULL; }
    return s;
}

void jce_semaphore_destroy(JceSemaphore *s)
{
    if (!s) return;
    if (s->handle) SDL_DestroySemaphore(s->handle);
    JCE_FREE(s);
}

void jce_semaphore_signal(JceSemaphore *s)
{
    if (s && s->handle) SDL_SignalSemaphore(s->handle);
}

void jce_semaphore_wait(JceSemaphore *s)
{
    if (s && s->handle) SDL_WaitSemaphore(s->handle);
}

bool jce_semaphore_wait_timeout(JceSemaphore *s, uint32_t timeout_ms)
{
    if (!s || !s->handle) return false;
    return SDL_WaitSemaphoreTimeout(s->handle, (Sint32)timeout_ms);
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
