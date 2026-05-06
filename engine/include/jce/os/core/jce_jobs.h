/*
 * jce_jobs.h  Fixed-size worker-thread job system (Sprint 4 #18).
 *
 * Bare-bones equivalent of Unity's Job System (no Burst): a fixed
 * worker pool drains a FIFO of small (function pointer + arg) tasks.
 * Producers can either fire-and-forget (jce_jobs_dispatch) or batch
 * a number of related jobs into a JceJobGroup and wait on the group
 * for synchronization.  Cooperative wait: while waiting on a group
 * the calling thread also pops jobs from the queue, which both keeps
 * the main thread from idling and avoids deadlocks if all workers
 * are busy on dependent work.
 *
 * Built on top of jce_thread / jce_mutex / jce_cond / jce_atomic_i32.
 */

#ifndef JCE_JOBS_H
#define JCE_JOBS_H

#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceJobSystem JceJobSystem;
typedef struct JceJobGroup  JceJobGroup;

typedef void (*JceJobFn)(void *user);

JCE_API JceJobSystem *JCE_CALL jce_jobs_create(int worker_count);
JCE_API void          JCE_CALL jce_jobs_destroy(JceJobSystem *sys);
JCE_API int           JCE_CALL jce_jobs_worker_count(const JceJobSystem *sys);

/* Fire-and-forget single job. */
JCE_API void          JCE_CALL jce_jobs_dispatch(JceJobSystem *sys, JceJobFn fn, void *user);

/* Group dispatch + wait. */
JCE_API JceJobGroup  *JCE_CALL jce_jobs_group_create(JceJobSystem *sys);
JCE_API void          JCE_CALL jce_jobs_group_dispatch(JceJobGroup *g, JceJobFn fn, void *user);
JCE_API void          JCE_CALL jce_jobs_group_wait(JceJobGroup *g);
JCE_API void          JCE_CALL jce_jobs_group_destroy(JceJobGroup *g);

/* Convenience: split [0, count) across N jobs of `chunk` indices each. */
typedef void (*JceParallelForFn)(int begin, int end, void *user);
JCE_API void          JCE_CALL jce_jobs_parallel_for(JceJobSystem *sys,
                                                     int count, int chunk,
                                                     JceParallelForFn fn,
                                                     void *user);

JCE_EXTERN_C_END

#endif /* JCE_JOBS_H */
