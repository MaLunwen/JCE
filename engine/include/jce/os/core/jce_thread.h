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

JceMutex *jce_mutex_create(void);
JCE_API void JCE_CALL jce_mutex_destroy(JceMutex *m);
JCE_API void JCE_CALL jce_mutex_lock(JceMutex *m);
JCE_API void JCE_CALL jce_mutex_unlock(JceMutex *m);

/* ================================================================== */
/* Condition variable                                                  */
/* ================================================================== */

typedef struct JceCondVar JceCondVar;

JceCondVar *jce_cond_create(void);
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
JceThread *jce_thread_create(JceThreadFn fn, void *arg, const char *name);

/* Block until the thread function returns; releases all resources.
   Must be called exactly once per JceThread. */
JCE_API void JCE_CALL jce_thread_join(JceThread *t);

/* Cross-platform sleep for the calling thread. Uses SDL3 internally. */
JCE_API void JCE_CALL jce_thread_sleep_ms(uint32_t ms);

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

JceAtomicI32 *jce_atomic_i32_create(int32_t initial);
JCE_API void JCE_CALL jce_atomic_i32_destroy(JceAtomicI32 *a);
JCE_API int32_t JCE_CALL jce_atomic_i32_load(const JceAtomicI32 *a);
JCE_API void JCE_CALL jce_atomic_i32_store(JceAtomicI32 *a, int32_t v);
JCE_API int32_t JCE_CALL jce_atomic_i32_exchange(JceAtomicI32 *a, int32_t v);
JCE_API int32_t JCE_CALL jce_atomic_i32_add(JceAtomicI32 *a, int32_t v); /* returns previous */

JceAtomicU64 *jce_atomic_u64_create(uint64_t initial);
JCE_API void JCE_CALL jce_atomic_u64_destroy(JceAtomicU64 *a);
JCE_API uint64_t JCE_CALL jce_atomic_u64_load(const JceAtomicU64 *a);
JCE_API void JCE_CALL jce_atomic_u64_store(JceAtomicU64 *a, uint64_t v);
JCE_API uint64_t JCE_CALL jce_atomic_u64_add(JceAtomicU64 *a, uint64_t v); /* returns previous */

/* ================================================================== */
/* Semaphore                                                           */
/* ================================================================== */

typedef struct JceSemaphore JceSemaphore;

JceSemaphore *jce_semaphore_create(uint32_t initial);
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
JceThreadPool *jce_thread_pool_create(int num_threads);

/* Wait for all pending tasks to finish, then destroy the pool. */
JCE_API void JCE_CALL jce_thread_pool_destroy(JceThreadPool *pool);

/* Submit a fire-and-forget task. */
JCE_API void JCE_CALL jce_thread_pool_submit(JceThreadPool *pool, JceTaskFn fn, void *arg);

/* Submit a task and get a waitable handle.
   Caller must call jce_task_wait() and then jce_task_free(). */
typedef struct JceTask JceTask;

JceTask *jce_thread_pool_submit_tracked(JceThreadPool *pool,
                                         JceTaskFn fn, void *arg);

/* Block until the task is complete. */
JCE_API void JCE_CALL jce_task_wait(JceTask *task);

/* Non-blocking check. */
JCE_API bool JCE_CALL jce_task_done(const JceTask *task);

/* Free the task handle (must be done after the task is complete). */
JCE_API void JCE_CALL jce_task_free(JceTask *task);

JCE_EXTERN_C_END

#endif /* JCE_THREAD_H */
