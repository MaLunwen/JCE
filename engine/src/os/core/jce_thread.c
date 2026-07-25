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

/* Bridge between JceTaskFn(void*) / JceTaskRangeFn(begin,end,void*) and the
   enkiTS range callback.  Exactly one of fn / range_fn is set. */
typedef struct TaskAdapter {
    JceTaskFn      fn;
    JceTaskRangeFn range_fn;
    void          *arg;
    JceThreadPool *pool;         /* owner, for the running-pool marker below */
} TaskAdapter;

/* Which pool's task the calling thread is currently executing, or NULL.
   enkiTS's own thread number lives in ONE process-wide thread_local shared by
   every scheduler, so it cannot answer "is this thread a worker of THIS pool"
   once more than one pool exists — a worker of another pool would report a
   non-zero number here and be mistaken for ours.  SDL_TLSID is designed to be
   used zero-initialised, so this needs no lazy construction. */
static SDL_TLSID g_running_pool;

static void task_range_adapter(uint32_t start_, uint32_t end_,
                               uint32_t threadnum_, void *pArgs_)
{
    (void)threadnum_;
    TaskAdapter *a = (TaskAdapter *)pArgs_;
    void *prev = SDL_GetTLS(&g_running_pool);   /* nested tasks restore it */
    SDL_SetTLS(&g_running_pool, a->pool, NULL);
    if (a->range_fn) a->range_fn(start_, end_, a->arg);
    else             a->fn(a->arg);
    SDL_SetTLS(&g_running_pool, prev, NULL);
}

/* ================================================================== */
/* Tracked task                                                        */
/* ================================================================== */

struct JceTask {
    enkiTaskScheduler *scheduler;   /* back-reference for wait/query */
    enkiTaskSet       *task_set;
    JceThreadPool     *pool;        /* back-reference for the wait check */
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
    int                worker_count;  /* threads enkiTS spawned (excl. creator) */
    uint64_t           owner_tid;     /* creator = enkiTS thread 0              */
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
    /* enkiGetNumTaskThreads() counts the initialising thread too — it runs
       tasks while it waits but is not a thread we spawned. */
    pool->worker_count  = (int)enkiGetNumTaskThreads(pool->scheduler) - 1;
    if (pool->worker_count < 0) pool->worker_count = 0;
    pool->owner_tid     = jce_thread_current_id();
    return pool;
}

int jce_thread_pool_worker_count(const JceThreadPool *pool)
{
    return pool ? pool->worker_count : 0;
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

bool jce_thread_pool_submit(JceThreadPool *pool, JceTaskFn fn, void *arg)
{
    if (!pool || !fn) return false;

    cleanup_pending(pool);

    TaskAdapter *adapter = JCE_NEW(TaskAdapter);
    if (!adapter) return false;
    adapter->fn   = fn;
    adapter->arg  = arg;
    adapter->pool = pool;

    /* Pre-allocate tracking node BEFORE submitting so we never lose
       the adapter pointer if the allocation fails. */
    PendingTask *pending = JCE_NEW(PendingTask);
    if (!pending) {
        JCE_FREE(adapter);
        return false;
    }

    enkiTaskSet *ts = enkiCreateTaskSet(pool->scheduler, task_range_adapter);
    enkiAddTaskSetArgs(pool->scheduler, ts, adapter, 1);

    pending->task_set = ts;
    pending->adapter  = adapter;

    SDL_LockMutex(pool->pending_mutex);
    pending->next      = pool->pending_head;
    pool->pending_head = pending;
    SDL_UnlockMutex(pool->pending_mutex);
    return true;
}

JceTask *jce_thread_pool_submit_tracked(JceThreadPool *pool,
                                         JceTaskFn fn, void *arg)
{
    if (!pool || !fn) return NULL;

    JceTask *task = JCE_NEW(JceTask);
    if (!task) return NULL;

    task->scheduler    = pool->scheduler;
    task->pool         = pool;
    task->adapter.fn   = fn;
    task->adapter.arg  = arg;
    task->adapter.pool = pool;

    task->task_set = enkiCreateTaskSet(pool->scheduler, task_range_adapter);
    enkiAddTaskSetArgs(pool->scheduler, task->task_set, &task->adapter, 1);

    return task;
}

JceTask *jce_thread_pool_submit_range(JceThreadPool *pool,
                                      JceTaskRangeFn fn, void *arg,
                                      uint32_t set_size, uint32_t min_range)
{
    if (!pool || !fn || set_size == 0) return NULL;
    if (min_range == 0) min_range = 1;

    JceTask *task = JCE_NEW(JceTask);
    if (!task) return NULL;

    task->scheduler        = pool->scheduler;
    task->pool             = pool;
    task->adapter.range_fn = fn;
    task->adapter.arg      = arg;
    task->adapter.pool     = pool;

    task->task_set = enkiCreateTaskSet(pool->scheduler, task_range_adapter);
    enkiAddTaskSetMinRange(pool->scheduler, task->task_set, &task->adapter,
                           set_size, min_range);

    return task;
}

/* ── Blocking parallel-for with pinned chunk boundaries ─────────────── */

typedef struct {
    JceTaskRangeFn fn;
    void          *arg;
    uint32_t       count;
    uint32_t       chunk;
} ParallelForCtx;

/* The scheduler hands us a run of CHUNK indices, not element indices: keeping
   the chunk as the unit of work is exactly what pins the element boundaries to
   multiples of `chunk`, which consumers that key per-chunk state off begin/chunk
   (render queue, entity-cull hit pass) depend on. */
static void parallel_for_chunks(uint32_t begin, uint32_t end, void *arg)
{
    const ParallelForCtx *c = (const ParallelForCtx *)arg;
    for (uint32_t i = begin; i < end; ++i) {
        uint32_t b = i * c->chunk;
        uint32_t e = b + c->chunk;
        if (e > c->count) e = c->count;
        c->fn(b, e, c->arg);
    }
}

void jce_thread_pool_parallel_for(JceThreadPool *pool, uint32_t count,
                                  uint32_t chunk, JceTaskRangeFn fn, void *arg)
{
    if (!fn || count == 0) return;

    if (chunk == 0) {
        const int wc = jce_thread_pool_worker_count(pool);
        const uint32_t w = wc > 0 ? (uint32_t)wc : 1u;
        chunk = (count + w - 1u) / w;
        if (chunk == 0) chunk = 1u;
    }
    const uint32_t n_chunks = (count + chunk - 1u) / chunk;

    ParallelForCtx ctx;
    ctx.fn = fn; ctx.arg = arg; ctx.count = count; ctx.chunk = chunk;

    /* No pool, or a single chunk: nothing to overlap, and the cooperative wait
       would have run it on this thread anyway. */
    if (!pool || n_chunks <= 1u) { fn(0u, count, arg); return; }

    /* The handle lives on THIS stack — the wait below is what makes that safe,
       and it keeps the per-frame consumers free of the malloc/free pair a
       submit_range + jce_task_free round trip would cost every frame. */
    JceTask task;
    task.scheduler        = pool->scheduler;
    task.pool             = pool;
    task.adapter.fn       = NULL;
    task.adapter.range_fn = parallel_for_chunks;
    task.adapter.arg      = &ctx;
    task.adapter.pool     = pool;

    task.task_set = enkiCreateTaskSet(pool->scheduler, task_range_adapter);
    if (!task.task_set) {           /* OOM: run it here rather than drop it */
        parallel_for_chunks(0u, n_chunks, &ctx);
        return;
    }
    enkiAddTaskSetMinRange(pool->scheduler, task.task_set, &task.adapter,
                           n_chunks, 1u);
    jce_task_wait(&task);
    enkiDeleteTaskSet(pool->scheduler, task.task_set);
}

/* True when the caller owns a thread slot in THIS pool's scheduler and may
   therefore use the work-stealing wait: either one of its workers (marked
   while it runs one of our tasks) or the thread that initialised it, which
   enkiTS treats as thread 0.  Any other thread would run tasks under thread
   0's slot and corrupt that thread's single-producer work queue. */
static bool ets_wait_safe_here(const JceThreadPool *pool)
{
    if (!pool) return false;
    if (SDL_GetTLS(&g_running_pool) == (const void *)pool) return true;
    return jce_thread_current_id() == pool->owner_tid;
}

void jce_task_wait(JceTask *task)
{
    if (!task) return;
    if (ets_wait_safe_here(task->pool)) {
        enkiWaitForTaskSet(task->scheduler, task->task_set);
        return;
    }
    /* Foreign thread: poll instead of stealing work.  Only reached by callers
       that are already I/O paced, so 1 ms granularity is free. */
    while (!enkiIsTaskSetComplete(task->scheduler, task->task_set))
        SDL_Delay(1);
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

/* ================================================================== */
/* Process-wide shared thread pool                                     */
/* ================================================================== */

static JceThreadPool *g_shared_pool;
static int            g_shared_workers = -1;  /* <=0 = apply the auto formula */
static SDL_AtomicInt  g_shared_lock;          /* zero-init = unlocked        */

/* A spin lock rather than a JceMutex: this guards the *creation* of the one
   shared pool, so it must work before any engine object exists and cannot
   itself need lazy construction.  Held only across pool create (never across
   destroy, which blocks on worker joins). */
static void shared_lock(void)
{
    while (!SDL_CompareAndSwapAtomicInt(&g_shared_lock, 0, 1))
        SDL_Delay(0);
}

static void shared_unlock(void)
{
    SDL_SetAtomicInt(&g_shared_lock, 0);
}

void jce_thread_pool_shared_set_workers(int workers)
{
    shared_lock();
    if (!g_shared_pool) g_shared_workers = workers;  /* too late once it exists */
    shared_unlock();
}

/* Leave a core for the thread that waits, and cap at 8 — our per-frame loops
   saturate well under that, and enkiTS's own auto-detect takes every core.
   SDL-only on purpose: this layer must not reach into jce_config. */
int jce_thread_pool_default_workers(void)
{
    int workers = SDL_GetNumLogicalCPUCores() - 1;
    if (workers < 1) workers = 1;
    if (workers > 8) workers = 8;
    return workers;
}

JceThreadPool *jce_thread_pool_shared(void)
{
    shared_lock();
    if (!g_shared_pool) {
        /* A pin from jce_thread_pool_shared_set_workers() (jce.ini
           [performance] job_workers, pushed by jce_config_publish_perf) wins
           over the house policy. */
        int workers = g_shared_workers;
        if (workers <= 0) workers = jce_thread_pool_default_workers();
        /* enkiTS counts the initialising thread inside its thread total, so
           ask for workers+1 to end up with `workers` spawned threads. */
        g_shared_pool = jce_thread_pool_create(workers + 1);
    }
    JceThreadPool *p = g_shared_pool;
    shared_unlock();
    return p;
}

void jce_thread_pool_shared_shutdown(void)
{
    shared_lock();
    JceThreadPool *p = g_shared_pool;
    g_shared_pool    = NULL;
    g_shared_workers = -1;
    shared_unlock();

    /* Outside the lock: destroy waits for every outstanding task and joins the
       workers, and one of those tasks may call jce_thread_pool_shared(). */
    if (p) jce_thread_pool_destroy(p);
}
