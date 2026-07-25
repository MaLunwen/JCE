/*
 * jce_thread.h  Thread pool and synchronisation primitives.
 *
 * Provides a fixed-size worker thread pool over SDL3 threads,
 * plus lightweight mutex / condition-variable wrappers.
 *
 * Layer: Utilities (Layer 0 — depends only on SDL3).
 */

#ifndef JCE_THREAD_H
#define JCE_THREAD_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Mutex                                                               */
/* ================================================================== */

typedef struct JceMutex JceMutex;

JCE_API JceMutex *jce_mutex_create(void);
JCE_API void JCE_CALL jce_mutex_destroy(JceMutex *m);
JCE_API void JCE_CALL jce_mutex_lock(JceMutex *m);
JCE_API void JCE_CALL jce_mutex_unlock(JceMutex *m);

/* ================================================================== */
/* Condition variable                                                  */
/* ================================================================== */

typedef struct JceCondVar JceCondVar;

JCE_API JceCondVar *jce_cond_create(void);
JCE_API void JCE_CALL jce_cond_destroy(JceCondVar *c);
JCE_API void JCE_CALL jce_cond_wait(JceCondVar *c, JceMutex *m);
JCE_API void JCE_CALL jce_cond_signal(JceCondVar *c);
JCE_API void JCE_CALL jce_cond_broadcast(JceCondVar *c);

/* ================================================================== */
/* Long-lived dedicated thread (SDL3-backed, cross-platform)            */
/*                                                                     */
/* Use this for streams that must own a worker for their full lifetime  */
/* (e.g. video decode loops). Do NOT submit such loops to the thread    */
/* pool — they would starve enkiTS workers.                             */
/* ================================================================== */

typedef struct JceThread JceThread;

typedef void (*JceThreadFn)(void *arg);

/* Spawn a dedicated OS thread. `name` is optional (debug label).
   Returns NULL on failure. */
JCE_API JceThread *jce_thread_create(JceThreadFn fn, void *arg, const char *name);

/* Block until the thread function returns; releases all resources.
   Must be called exactly once per JceThread. */
JCE_API void JCE_CALL jce_thread_join(JceThread *t);

/* Cross-platform sleep for the calling thread. Uses SDL3 internally. */
JCE_API void JCE_CALL jce_thread_sleep_ms(uint32_t ms);

/* ── Current thread id / main-thread tracking ──────────────────────
 *
 * Helper used by debug-only invariants (e.g., "this slot must only be
 * mutated from the main thread").  Implementation is cross-platform
 * via SDL3.  Cost: single TLS load.
 *
 *   jce_thread_mark_main()     Records the calling thread as the
 *                               application main thread.  Should be
 *                               called once near engine startup.
 *   jce_thread_is_main()       Returns true if the caller is the same
 *                               thread that called jce_thread_mark_main().
 *                               Returns true if mark was never called
 *                               (treated as single-threaded).
 *   jce_thread_current_id()    Returns the OS-level thread id of the
 *                               caller (opaque, comparable). */
JCE_API void     JCE_CALL jce_thread_mark_main(void);
JCE_API bool     JCE_CALL jce_thread_is_main(void);
JCE_API uint64_t JCE_CALL jce_thread_current_id(void);

/* ================================================================== */
/* Thread-local storage                                                */
/*                                                                     */
/* Cross-platform per-thread slot backed by SDL3.  Use instead of      */
/* C `thread_local` so middleware does not depend on per-toolchain     */
/* TLS semantics (e.g. dlopen() interaction on glibc/macOS).           */
/*                                                                     */
/* Pattern:                                                            */
/*   static JceTLS *g_slot = NULL;                                     */
/*   if (!g_slot) g_slot = jce_tls_create(my_dtor);                    */
/*   MyState *s = (MyState *)jce_tls_get(g_slot);                      */
/*   if (!s) { s = ...; jce_tls_set(g_slot, s); }                      */
/*                                                                     */
/* The destructor (if non-NULL) is invoked on the owning thread when   */
/* the thread terminates.                                              */
/* ================================================================== */

typedef struct JceTLS JceTLS;
typedef void (*JceTLSDestructor)(void *value);

JCE_API JceTLS *jce_tls_create(JceTLSDestructor dtor);
JCE_API void *  JCE_CALL jce_tls_get(JceTLS *tls);
JCE_API void    JCE_CALL jce_tls_set(JceTLS *tls, void *value);

/* ================================================================== */
/* Atomic primitives                                                   */
/*                                                                     */
/* Engine-managed atomics so callers never include <atomic> /          */
/* <stdatomic.h>. All operations have sequentially consistent          */
/* semantics — sufficient for control flags, counters, EOF signals.    */
/* For high-throughput hot paths prefer mutex-protected batching.      */
/* ================================================================== */

typedef struct JceAtomicI32 JceAtomicI32;
typedef struct JceAtomicU64 JceAtomicU64;

JCE_API JceAtomicI32 *jce_atomic_i32_create(int32_t initial);
JCE_API void JCE_CALL jce_atomic_i32_destroy(JceAtomicI32 *a);
JCE_API int32_t JCE_CALL jce_atomic_i32_load(const JceAtomicI32 *a);
JCE_API void JCE_CALL jce_atomic_i32_store(JceAtomicI32 *a, int32_t v);
JCE_API int32_t JCE_CALL jce_atomic_i32_exchange(JceAtomicI32 *a, int32_t v);
JCE_API int32_t JCE_CALL jce_atomic_i32_add(JceAtomicI32 *a, int32_t v); /* returns previous */

JCE_API JceAtomicU64 *jce_atomic_u64_create(uint64_t initial);
JCE_API void JCE_CALL jce_atomic_u64_destroy(JceAtomicU64 *a);
JCE_API uint64_t JCE_CALL jce_atomic_u64_load(const JceAtomicU64 *a);
JCE_API void JCE_CALL jce_atomic_u64_store(JceAtomicU64 *a, uint64_t v);
JCE_API uint64_t JCE_CALL jce_atomic_u64_add(JceAtomicU64 *a, uint64_t v); /* returns previous */

/* ================================================================== */
/* Semaphore                                                           */
/* ================================================================== */

typedef struct JceSemaphore JceSemaphore;

JCE_API JceSemaphore *jce_semaphore_create(uint32_t initial);
JCE_API void JCE_CALL jce_semaphore_destroy(JceSemaphore *s);
JCE_API void JCE_CALL jce_semaphore_signal(JceSemaphore *s);
JCE_API void JCE_CALL jce_semaphore_wait(JceSemaphore *s);
/* Wait up to `timeout_ms`. Returns true if acquired, false on timeout. */
JCE_API bool JCE_CALL jce_semaphore_wait_timeout(JceSemaphore *s, uint32_t timeout_ms);

/* ================================================================== */
/* Thread pool                                                         */
/* ================================================================== */

typedef struct JceThreadPool JceThreadPool;

/* Function signature for a task. */
typedef void (*JceTaskFn)(void *arg);

/* Create a pool with num_threads workers (0 = auto-detect CPU cores). */
JCE_API JceThreadPool *jce_thread_pool_create(int num_threads);

/* The house worker-count policy: logical cores - 1, clamped to 1..8.
 *
 * Leaves a core for the thread that is waiting and caps the count, because
 * enkiTS's own auto-detect (jce_thread_pool_create(0)) takes EVERY core —
 * fine for a batch tool, ruinous for the editor, which is still drawing while
 * a build cooks.  The shared pool sizes itself with this, and so should any
 * private pool that wants the same behaviour, so the formula has one home
 * instead of a copy per call site. */
JCE_API int JCE_CALL jce_thread_pool_default_workers(void);

/* Wait for all pending tasks to finish, then destroy the pool. */
JCE_API void JCE_CALL jce_thread_pool_destroy(JceThreadPool *pool);

/* Submit a fire-and-forget task.  Returns false when the task could not be
   queued (no pool, or out of memory) — `fn` then never runs, so a caller that
   tracks outstanding work must run it inline rather than leak the count. */
JCE_API bool JCE_CALL jce_thread_pool_submit(JceThreadPool *pool, JceTaskFn fn, void *arg);

/* Submit a task and get a waitable handle.
   Caller must call jce_task_wait() and then jce_task_free(). */
typedef struct JceTask JceTask;

JCE_API JceTask *jce_thread_pool_submit_tracked(JceThreadPool *pool,
                                         JceTaskFn fn, void *arg);

/* Submit a data-parallel range and get a waitable handle.
   The scheduler splits [0, set_size) into partitions of at least `min_range`
   indices (the grain size — pick one worth >= ~10k cycles of work) and calls
   `fn` once per partition with a disjoint [begin, end); the partitions
   together cover the whole set.  Caller must jce_task_wait() then
   jce_task_free(); `arg` must outlive the wait. */
typedef void (*JceTaskRangeFn)(uint32_t begin, uint32_t end, void *arg);

JCE_API JceTask *jce_thread_pool_submit_range(JceThreadPool *pool,
                                              JceTaskRangeFn fn, void *arg,
                                              uint32_t set_size,
                                              uint32_t min_range);

/* Blocking data-parallel loop over [0, count) with FIXED chunk boundaries.
   The range is cut into ceil(count/chunk) chunks of exactly `chunk` indices
   (the last one carries the remainder) and `fn` is called once per chunk, so
   `begin` is ALWAYS a multiple of `chunk`.  That guarantee is what separates
   this from jce_thread_pool_submit_range, where the scheduler picks partition
   boundaries: several consumers derive a chunk slot as begin/chunk and index
   fixed-size per-chunk arrays with it, which arbitrary boundaries would alias.
   `chunk` 0 = one chunk per worker.  count 0 or a NULL pool run nothing / run
   serially; the calling thread also runs chunks (see jce_task_wait), so this
   never deadlocks on a busy pool and nesting is safe.  Allocation-free apart
   from enkiTS's own task-set node — the handle lives on the caller's stack. */
JCE_API void JCE_CALL jce_thread_pool_parallel_for(JceThreadPool *pool,
                                                   uint32_t count, uint32_t chunk,
                                                   JceTaskRangeFn fn, void *arg);

/* Number of dedicated background worker threads.  Excludes the thread that
   created the pool, which also runs tasks while it waits — so a pool created
   with N reports N-1.  Use it to size work chunks. */
JCE_API int JCE_CALL jce_thread_pool_worker_count(const JceThreadPool *pool);

/* Block until the task is complete.
   On the pool's own workers and on the thread that created the pool this is a
   work-stealing wait: the caller keeps running tasks instead of idling, which
   is also what makes nested submit-then-wait safe.  Any other thread has no
   scheduler slot of its own, so it polls (it would otherwise run tasks under
   the creating thread's slot and corrupt that thread's work queue). */
JCE_API void JCE_CALL jce_task_wait(JceTask *task);

/* Non-blocking check. */
JCE_API bool JCE_CALL jce_task_done(const JceTask *task);

/* Free the task handle (must be done after the task is complete). */
JCE_API void JCE_CALL jce_task_free(JceTask *task);

/* ================================================================== */
/* Process-wide shared thread pool                                     */
/* ================================================================== */
/*
 * ONE set of worker threads for every piece of CPU-bound, sub-frame
 * parallelism in the process: frustum cull, draw gather, anim sampling,
 * particles, mip downsample, render-queue submit.  All of it borrows this
 * pool rather than spawning its own threads — a second set of workers for
 * that class of work would double the thread count, which the single-core /
 * 512 MB baseline cannot afford.  This IS the engine's job system for frame
 * work.
 *
 * What does NOT belong here: anything that blocks longer than a frame.
 * Whole-file reads, glTF/FBX parsing, image and audio decode, asset cooking
 * run on a small private pool of their own (jce_thread_pool_create, sized by
 * jce_thread_pool_default_workers) — the world streamers, the async asset
 * pool, the archive loader, the bundle cook and the editor's material
 * extractor each keep one, and each says so at its create site.  The reason
 * is enkiTS's cooperative wait, not thread budget:
 * jce_thread_pool_parallel_for() ends in enkiWaitForTaskSet(), which runs ANY
 * queued task from ANY worker's pipe at ANY priority until the set it waits
 * on completes.  The per-frame consumers above call it from the main thread
 * several times a frame, so a queued 200 ms disk read is not "one worker
 * busy" — it is 200 ms of disk read executed by the main thread in the middle
 * of a cull.  Worse when the blocking job takes a lock (the archive loader's
 * io_lock): the main thread then blocks on a mutex a worker holds across disk
 * I/O, which is priority inversion, not a hitch.  Separate schedulers are
 * what stop the two classes of work from running on each other's threads.
 *
 * The way out of that split is priority tiers, not more threads: give the
 * blocking class a low enkiTS priority and let parallel_for wait with
 * enkiWaitForTaskSetPriority() so the frame loop only ever runs frame work.
 * Nothing sets a priority today, so every task is TASK_PRIORITY_HIGH and the
 * wait is unrestricted; until that lands, the private pools stay.
 *
 * Ordering:
 *   - jce_thread_pool_shared() creates the pool on first call and returns a
 *     BORROWED handle; never pass it to jce_thread_pool_destroy().  It is
 *     safe before and after engine init, and after a shutdown (which simply
 *     causes the next call to build a fresh pool).
 *   - The first caller becomes the scheduler's own thread and is therefore
 *     the only non-worker thread that may use the work-stealing wait, so
 *     make that first call on the main thread (the frame loop does).
 *   - Submit from that thread or from a worker — nowhere else.  enkiTS routes
 *     a submission into the pipe of the submitting thread's scheduler slot,
 *     and a thread it never registered reports slot 0, i.e. the first caller's
 *     pipe.  Those pipes are single-producer, so a helper thread that borrows
 *     the pool concurrently with the frame loop is racing the frame loop's own
 *     writes.  A long-lived helper that must fan out needs to register with
 *     the scheduler first; one that merely wants work off the main thread
 *     should own a private pool instead.
 *   - Worker count: cores-1, clamped to 1..8, unless
 *     jce_thread_pool_shared_set_workers() pinned it first — which only takes
 *     effect while the pool does not exist yet, so it must be called before
 *     the first jce_thread_pool_shared().  jce_config_publish_perf() does
 *     exactly that for the jce.ini [performance] job_workers knob.
 *   - jce_thread_pool_shared_shutdown() waits for every outstanding task,
 *     joins the workers and invalidates every JceTask handle taken from the
 *     pool — call it at engine teardown, once nothing holds one.
 */
JCE_API JceThreadPool *jce_thread_pool_shared(void);
JCE_API void JCE_CALL  jce_thread_pool_shared_set_workers(int workers);
JCE_API void JCE_CALL  jce_thread_pool_shared_shutdown(void);

JCE_EXTERN_C_END

#endif /* JCE_THREAD_H */
