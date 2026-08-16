/*
 * jce_thread.c  Thread pool and synchronisation primitives.
 *
 * Thread pool backed by enkiTS work-stealing task scheduler.
 * Mutex / condition-variable wrappers remain on SDL3.
 */

#include <jce/os/core/jce_thread.h>

#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_trace.h>

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
    char        name[JCE_TRACE_NAME_CAP];
};

static int sdl_thread_trampoline(void *user)
{
    JceThread *t = (JceThread *)user;
    if (t)
        jce_trace_thread_register(t->name);
    if (t && t->fn) t->fn(t->arg);
    if (t)
        jce_trace_thread_unregister();
    return 0;
}

JceThread *jce_thread_create(JceThreadFn fn, void *arg, const char *name)
{
    if (!fn) return NULL;
    JceThread *t = JCE_NEW(JceThread);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    SDL_strlcpy(t->name, name && name[0] ? name : "jce-thread",
                sizeof(t->name));
    t->handle = SDL_CreateThread(sdl_thread_trampoline,
                                 t->name, t);
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
    uint64_t       trace_task_id;
    uint64_t       trace_parent_id;
    uint64_t       trace_submitted_ns;
    char           debug_name[JCE_TRACE_NAME_CAP];
    char           pool_name[JCE_TRACE_NAME_CAP];
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
    TaskAdapter *a = (TaskAdapter *)pArgs_;
    void *prev = SDL_GetTLS(&g_running_pool);   /* nested tasks restore it */
    uint64_t previous_trace_id = jce_trace_task_current_id();
    uint64_t span_id = 0;
    uint64_t started_ns = 0;

    if (threadnum_ > 0 && a->pool) {
        char thread_name[JCE_TRACE_NAME_CAP];
        SDL_snprintf(thread_name, sizeof(thread_name), "%s-%u",
                     a->pool_name, (unsigned)threadnum_);
        jce_trace_thread_register(thread_name);
    }
    if (a->trace_task_id != 0 && jce_trace_enabled()) {
        uint64_t now = jce_time_ticks_ns();
        uint64_t queue_ns = now >= a->trace_submitted_ns
            ? now - a->trace_submitted_ns : 0;
        span_id = jce_trace_next_id();
        started_ns = now;
        jce_trace_task_begin(span_id, a->trace_task_id,
                             a->debug_name, queue_ns);
    }
    SDL_SetTLS(&g_running_pool, a->pool, NULL);
    {
        JCE_PROFILE_ZONE_N(a->debug_name);
        if (a->range_fn) a->range_fn(start_, end_, a->arg);
        else             a->fn(a->arg);
        JCE_PROFILE_ZONE_END;
    }
    SDL_SetTLS(&g_running_pool, prev, NULL);
    if (span_id != 0) {
        uint64_t ended_ns = jce_time_ticks_ns();
        jce_trace_task_end(span_id, a->trace_task_id, a->debug_name,
                           JCE_TRACE_TASK_SUCCEEDED,
                           ended_ns >= started_ns ? ended_ns - started_ns : 0);
        jce_trace_task_restore_id(previous_trace_id);
    }
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
    char               debug_name[JCE_TRACE_NAME_CAP];
};

static void task_adapter_init(TaskAdapter *adapter,
                              JceThreadPool *pool,
                              const char *debug_name,
                              JceTaskFn fn,
                              JceTaskRangeFn range_fn,
                              void *arg)
{
    SDL_zero(*adapter);
    adapter->fn = fn;
    adapter->range_fn = range_fn;
    adapter->arg = arg;
    adapter->pool = pool;
    SDL_strlcpy(adapter->debug_name,
                debug_name && debug_name[0] ? debug_name : "frame-job",
                sizeof(adapter->debug_name));
    SDL_strlcpy(adapter->pool_name,
                pool && pool->debug_name[0] ? pool->debug_name : "jce-jobs",
                sizeof(adapter->pool_name));
    if (jce_trace_enabled()) {
        adapter->trace_task_id = jce_trace_next_id();
        adapter->trace_parent_id = jce_trace_task_current_id();
    }
}

static void task_adapter_trace_submit(TaskAdapter *adapter)
{
    if (!adapter || adapter->trace_task_id == 0 || !jce_trace_enabled())
        return;
    adapter->trace_submitted_ns = jce_time_ticks_ns();
    jce_trace_task_submit(adapter->trace_task_id,
                          adapter->trace_parent_id,
                          JCE_TRACE_TASK_FRAME_JOB,
                          adapter->debug_name);
}

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

JceThreadPool *jce_thread_pool_create_named(int num_threads,
                                             const char *debug_name)
{
    JceThreadPool *pool = JCE_NEW(JceThreadPool);
    if (!pool) return NULL;

    pool->scheduler = enkiNewTaskScheduler();
    if (!pool->scheduler) { JCE_FREE(pool); return NULL; }

#if JCE_PLATFORM_WEB
    /* The web build links without a pthread pool (BGFX_CONFIG_MULTITHREADED=0
       single-threaded contract): enkiTS StartThreads() would throw
       system_error 138 ("thread constructor failed") and abort the runtime.
       Force every pool onto the calling thread; enkiTS executes tasks inline
       when the creator waits, which is the same degraded mode the async pool
       already ships on this platform. */
    num_threads = 1;
#endif

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
    SDL_strlcpy(pool->debug_name,
                debug_name && debug_name[0] ? debug_name : "jce-jobs",
                sizeof(pool->debug_name));
    return pool;
}

JceThreadPool *jce_thread_pool_create(int num_threads)
{
    return jce_thread_pool_create_named(num_threads, "jce-jobs");
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
    return jce_thread_pool_submit_named(pool, "frame-job", fn, arg);
}

bool jce_thread_pool_submit_named(JceThreadPool *pool,
                                  const char *debug_name,
                                  JceTaskFn fn, void *arg)
{
    if (!pool || !fn) return false;

    cleanup_pending(pool);

    TaskAdapter *adapter = JCE_NEW(TaskAdapter);
    if (!adapter) return false;
    task_adapter_init(adapter, pool, debug_name, fn, NULL, arg);

    /* Pre-allocate tracking node BEFORE submitting so we never lose
       the adapter pointer if the allocation fails. */
    PendingTask *pending = JCE_NEW(PendingTask);
    if (!pending) {
        JCE_FREE(adapter);
        return false;
    }

    enkiTaskSet *ts = enkiCreateTaskSet(pool->scheduler, task_range_adapter);
    if (!ts) {
        JCE_FREE(pending);
        JCE_FREE(adapter);
        return false;
    }
    task_adapter_trace_submit(adapter);
    enkiAddTaskSetArgs(pool->scheduler, ts, adapter, 1);
#if JCE_PLATFORM_WEB
    /* Zero-worker pools (web build) have nobody to pick queued work up and
       fire-and-forget submitters never wait: execute inline right now, on
       the calling thread, so async decode work still completes. */
    if (pool->worker_count == 0)
        enkiWaitForTaskSet(pool->scheduler, ts);
#endif

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
    return jce_thread_pool_submit_tracked_named(
        pool, "tracked-frame-job", fn, arg);
}

JceTask *jce_thread_pool_submit_tracked_named(
    JceThreadPool *pool, const char *debug_name, JceTaskFn fn, void *arg)
{
    if (!pool || !fn) return NULL;

    JceTask *task = JCE_NEW(JceTask);
    if (!task) return NULL;

    task->scheduler    = pool->scheduler;
    task->pool         = pool;
    task_adapter_init(&task->adapter, pool, debug_name, fn, NULL, arg);

    task->task_set = enkiCreateTaskSet(pool->scheduler, task_range_adapter);
    if (!task->task_set) {
        JCE_FREE(task);
        return NULL;
    }
    task_adapter_trace_submit(&task->adapter);
    enkiAddTaskSetArgs(pool->scheduler, task->task_set, &task->adapter, 1);
#if JCE_PLATFORM_WEB
    /* Zero-worker pools: complete inline so pollers see it finish. */
    if (pool->worker_count == 0)
        enkiWaitForTaskSet(pool->scheduler, task->task_set);
#endif

    return task;
}

JceTask *jce_thread_pool_submit_range(JceThreadPool *pool,
                                      JceTaskRangeFn fn, void *arg,
                                      uint32_t set_size, uint32_t min_range)
{
    return jce_thread_pool_submit_range_named(
        pool, "range-frame-job", fn, arg, set_size, min_range);
}

JceTask *jce_thread_pool_submit_range_named(
    JceThreadPool *pool, const char *debug_name,
    JceTaskRangeFn fn, void *arg,
    uint32_t set_size, uint32_t min_range)
{
    if (!pool || !fn || set_size == 0) return NULL;
    if (min_range == 0) min_range = 1;

    JceTask *task = JCE_NEW(JceTask);
    if (!task) return NULL;

    task->scheduler        = pool->scheduler;
    task->pool             = pool;
    task_adapter_init(&task->adapter, pool, debug_name, NULL, fn, arg);

    task->task_set = enkiCreateTaskSet(pool->scheduler, task_range_adapter);
    if (!task->task_set) {
        JCE_FREE(task);
        return NULL;
    }
    task_adapter_trace_submit(&task->adapter);
    enkiAddTaskSetMinRange(pool->scheduler, task->task_set, &task->adapter,
                           set_size, min_range);
#if JCE_PLATFORM_WEB
    /* Zero-worker pools: complete inline so pollers see it finish. */
    if (pool->worker_count == 0)
        enkiWaitForTaskSet(pool->scheduler, task->task_set);
#endif

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
    jce_thread_pool_parallel_for_named(
        pool, "parallel-for", count, chunk, fn, arg);
}

void jce_thread_pool_parallel_for_named(JceThreadPool *pool,
                                        const char *debug_name,
                                        uint32_t count,
                                        uint32_t chunk,
                                        JceTaskRangeFn fn,
                                        void *arg)
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
    if (!pool || n_chunks <= 1u) {
        TaskAdapter direct;

        task_adapter_init(&direct, pool, debug_name, NULL, fn, arg);
        task_adapter_trace_submit(&direct);
        task_range_adapter(0u, count, 0u, &direct);
        return;
    }

    /* The handle lives on THIS stack — the wait below is what makes that safe,
       and it keeps the per-frame consumers free of the malloc/free pair a
       submit_range + jce_task_free round trip would cost every frame. */
    JceTask task;
    task.scheduler        = pool->scheduler;
    task.pool             = pool;
    task_adapter_init(&task.adapter, pool, debug_name,
                      NULL, parallel_for_chunks, &ctx);

    task.task_set = enkiCreateTaskSet(pool->scheduler, task_range_adapter);
    if (!task.task_set) {           /* OOM: run it here rather than drop it */
        task_adapter_trace_submit(&task.adapter);
        task_range_adapter(0u, n_chunks, 0u, &task.adapter);
        return;
    }
    task_adapter_trace_submit(&task.adapter);
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
    uint64_t wait_id = 0;
    uint64_t wait_started_ns = 0;

    if (!task) return;
    if (task->adapter.trace_task_id != 0 && jce_trace_enabled()) {
        wait_id = jce_trace_next_id();
        wait_started_ns = jce_time_ticks_ns();
        jce_trace_wait_begin(wait_id, task->adapter.trace_task_id,
                             task->adapter.debug_name);
    }
    if (ets_wait_safe_here(task->pool)) {
        enkiWaitForTaskSet(task->scheduler, task->task_set);
    } else {
        /* Foreign thread: poll instead of stealing work.  Only reached by callers
           that are already I/O paced, so 1 ms granularity is free. */
        while (!enkiIsTaskSetComplete(task->scheduler, task->task_set))
            SDL_Delay(1);
    }
    if (wait_id != 0) {
        uint64_t ended_ns = jce_time_ticks_ns();
        jce_trace_wait_end(wait_id, task->adapter.trace_task_id,
                           task->adapter.debug_name,
                           ended_ns >= wait_started_ns
                               ? ended_ns - wait_started_ns : 0);
    }
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
        g_shared_pool = jce_thread_pool_create_named(
            workers + 1, "jce-frame");
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
