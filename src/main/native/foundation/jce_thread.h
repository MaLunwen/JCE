/*
 * jce_thread.h  Thread pool and synchronisation primitives.
 *
 * Provides a fixed-size worker thread pool over SDL3 threads,
 * plus lightweight mutex / condition-variable wrappers.
 *
 * Layer: Foundation (Layer 1 — depends only on SDL3).
 */

#ifndef JCE_THREAD_H
#define JCE_THREAD_H

#include <stdbool.h>
#include <stdint.h>

/* ================================================================== */
/* Mutex                                                               */
/* ================================================================== */

typedef struct JceMutex JceMutex;

JceMutex *jce_mutex_create(void);
void      jce_mutex_destroy(JceMutex *m);
void      jce_mutex_lock(JceMutex *m);
void      jce_mutex_unlock(JceMutex *m);

/* ================================================================== */
/* Condition variable                                                  */
/* ================================================================== */

typedef struct JceCondVar JceCondVar;

JceCondVar *jce_cond_create(void);
void        jce_cond_destroy(JceCondVar *c);
void        jce_cond_wait(JceCondVar *c, JceMutex *m);
void        jce_cond_signal(JceCondVar *c);
void        jce_cond_broadcast(JceCondVar *c);

/* ================================================================== */
/* Thread pool                                                         */
/* ================================================================== */

typedef struct JceThreadPool JceThreadPool;

/* Function signature for a task. */
typedef void (*JceTaskFn)(void *arg);

/* Create a pool with num_threads workers (0 = auto-detect CPU cores). */
JceThreadPool *jce_thread_pool_create(int num_threads);

/* Wait for all pending tasks to finish, then destroy the pool. */
void jce_thread_pool_destroy(JceThreadPool *pool);

/* Submit a fire-and-forget task. */
void jce_thread_pool_submit(JceThreadPool *pool, JceTaskFn fn, void *arg);

/* Submit a task and get a waitable handle.
   Caller must call jce_task_wait() and then jce_task_free(). */
typedef struct JceTask JceTask;

JceTask *jce_thread_pool_submit_tracked(JceThreadPool *pool,
                                         JceTaskFn fn, void *arg);

/* Block until the task is complete. */
void jce_task_wait(JceTask *task);

/* Non-blocking check. */
bool jce_task_done(const JceTask *task);

/* Free the task handle (must be done after the task is complete). */
void jce_task_free(JceTask *task);

#endif /* JCE_THREAD_H */
