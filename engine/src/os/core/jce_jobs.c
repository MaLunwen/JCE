/*
 * jce_jobs.c  Worker-thread job system (Sprint 4 #18).
 */

#include <jce/os/core/jce_jobs.h>
#include <jce/os/core/jce_thread.h>

#include "jce_memory.h"

#include <SDL3/SDL_cpuinfo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JCE_JOB_QUEUE_CAP 4096

typedef struct {
    JceJobFn      fn;
    void         *user;
    JceJobGroup  *group;          /* NULL for fire-and-forget */
} JceJobItem;

struct JceJobGroup {
    JceJobSystem *sys;
    JceAtomicI32 *pending;        /* outstanding jobs */
};

struct JceJobSystem {
    JceThread   **workers;
    int           worker_count;

    JceMutex     *mu;
    JceCondVar   *cv_have;        /* signal: queue non-empty */
    JceCondVar   *cv_done;        /* signal: a job finished */

    JceJobItem    queue[JCE_JOB_QUEUE_CAP];
    int           head, tail, size;

    int           shutdown;
};

static int try_pop_locked(JceJobSystem *s, JceJobItem *out)
{
    if (s->size == 0) return 0;
    *out = s->queue[s->head];
    s->head = (s->head + 1) % JCE_JOB_QUEUE_CAP;
    --s->size;
    return 1;
}

static void run_one(JceJobItem *it)
{
    if (it->fn) it->fn(it->user);
    if (it->group && it->group->pending) jce_atomic_i32_add(it->group->pending, -1);
}

static void worker_main(void *arg)
{
    JceJobSystem *s = (JceJobSystem *)arg;
    for (;;) {
        JceJobItem it;
        jce_mutex_lock(s->mu);
        while (!s->shutdown && s->size == 0) jce_cond_wait(s->cv_have, s->mu);
        if (s->shutdown && s->size == 0) { jce_mutex_unlock(s->mu); return; }
        try_pop_locked(s, &it);
        jce_mutex_unlock(s->mu);

        run_one(&it);
        jce_cond_broadcast(s->cv_done);
    }
}

JCE_API JceJobSystem *JCE_CALL
jce_jobs_create(int worker_count)
{
    if (worker_count < 1) worker_count = 1;
    if (worker_count > 64) worker_count = 64;

    JceJobSystem *s = (JceJobSystem *)JCE_CALLOC(1, sizeof *s);
    if (!s) return NULL;
    s->mu      = jce_mutex_create();
    s->cv_have = jce_cond_create();
    s->cv_done = jce_cond_create();
    s->worker_count = worker_count;
    s->workers = (JceThread **)JCE_CALLOC((size_t)worker_count, sizeof(JceThread *));

    char name[32];
    for (int i = 0; i < worker_count; ++i) {
        snprintf(name, sizeof name, "jce_job_%d", i);
        s->workers[i] = jce_thread_create(worker_main, s, name);
    }
    return s;
}

JCE_API void JCE_CALL
jce_jobs_destroy(JceJobSystem *s)
{
    if (!s) return;
    jce_mutex_lock(s->mu);
    s->shutdown = 1;
    jce_cond_broadcast(s->cv_have);
    jce_mutex_unlock(s->mu);

    for (int i = 0; i < s->worker_count; ++i) jce_thread_join(s->workers[i]);

    JCE_FREE(s->workers);
    jce_cond_destroy(s->cv_done);
    jce_cond_destroy(s->cv_have);
    jce_mutex_destroy(s->mu);
    JCE_FREE(s);
}

JCE_API int JCE_CALL
jce_jobs_worker_count(const JceJobSystem *s) { return s ? s->worker_count : 0; }

static void enqueue(JceJobSystem *s, JceJobFn fn, void *user, JceJobGroup *g)
{
    jce_mutex_lock(s->mu);
    /* If full, run inline to avoid drop. */
    if (s->size >= JCE_JOB_QUEUE_CAP) {
        jce_mutex_unlock(s->mu);
        JceJobItem it = { fn, user, g };
        run_one(&it);
        jce_cond_broadcast(s->cv_done);
        return;
    }
    s->queue[s->tail].fn    = fn;
    s->queue[s->tail].user  = user;
    s->queue[s->tail].group = g;
    s->tail = (s->tail + 1) % JCE_JOB_QUEUE_CAP;
    ++s->size;
    jce_cond_signal(s->cv_have);
    jce_mutex_unlock(s->mu);
}

JCE_API void JCE_CALL
jce_jobs_dispatch(JceJobSystem *s, JceJobFn fn, void *user)
{
    if (!s || !fn) return;
    enqueue(s, fn, user, NULL);
}

JCE_API JceJobGroup *JCE_CALL
jce_jobs_group_create(JceJobSystem *s)
{
    if (!s) return NULL;
    JceJobGroup *g = (JceJobGroup *)JCE_CALLOC(1, sizeof *g);
    if (!g) return NULL;
    g->sys     = s;
    g->pending = jce_atomic_i32_create(0);
    return g;
}

JCE_API void JCE_CALL
jce_jobs_group_destroy(JceJobGroup *g)
{
    if (!g) return;
    if (g->pending) jce_atomic_i32_destroy(g->pending);
    JCE_FREE(g);
}

JCE_API void JCE_CALL
jce_jobs_group_dispatch(JceJobGroup *g, JceJobFn fn, void *user)
{
    if (!g || !fn) return;
    jce_atomic_i32_add(g->pending, +1);
    enqueue(g->sys, fn, user, g);
}

JCE_API void JCE_CALL
jce_jobs_group_wait(JceJobGroup *g)
{
    if (!g) return;
    JceJobSystem *s = g->sys;
    for (;;) {
        if (jce_atomic_i32_load(g->pending) <= 0) return;

        /* Cooperative drain: pop and run while waiting. */
        JceJobItem it; int got = 0;
        jce_mutex_lock(s->mu);
        got = try_pop_locked(s, &it);
        if (!got) {
            /* Block until something finishes, then re-check pending. */
            jce_cond_wait(s->cv_done, s->mu);
        }
        jce_mutex_unlock(s->mu);

        if (got) {
            run_one(&it);
            jce_cond_broadcast(s->cv_done);
        }
    }
}

typedef struct {
    JceParallelForFn fn;
    void            *user;
    int              begin, end;
} JcePForChunk;

static void pfor_worker(void *arg)
{
    JcePForChunk *c = (JcePForChunk *)arg;
    c->fn(c->begin, c->end, c->user);
}

JCE_API void JCE_CALL
jce_jobs_parallel_for(JceJobSystem *s, int count, int chunk,
                      JceParallelForFn fn, void *user)
{
    if (!s || !fn || count <= 0) return;
    if (chunk <= 0) {
        int wc = s->worker_count > 0 ? s->worker_count : 1;
        chunk = (count + wc - 1) / wc;
        if (chunk < 1) chunk = 1;
    }
    int n_chunks = (count + chunk - 1) / chunk;
    JcePForChunk *chunks = (JcePForChunk *)JCE_MALLOC(sizeof(JcePForChunk) * (size_t)n_chunks);
    JceJobGroup *g = jce_jobs_group_create(s);
    for (int i = 0; i < n_chunks; ++i) {
        int b = i * chunk;
        int e = b + chunk; if (e > count) e = count;
        chunks[i].fn = fn; chunks[i].user = user; chunks[i].begin = b; chunks[i].end = e;
        jce_jobs_group_dispatch(g, pfor_worker, &chunks[i]);
    }
    jce_jobs_group_wait(g);
    jce_jobs_group_destroy(g);
    JCE_FREE(chunks);
}

/* ---- Process-wide shared job system (main-thread lazy init) ---- */

static JceJobSystem *g_default_jobs       = NULL;
static int           g_default_jobs_tried = 0;

JCE_API JceJobSystem *JCE_CALL
jce_jobs_default(void)
{
    if (g_default_jobs || g_default_jobs_tried) return g_default_jobs;
    g_default_jobs_tried = 1;   /* don't retry creation every frame */

    int cores   = SDL_GetNumLogicalCPUCores();
    int workers  = cores - 1;   /* leave a core for the main thread */
    if (workers < 1) workers = 1;
    if (workers > 8) workers = 8;   /* our per-frame loops saturate well under this */

    g_default_jobs = jce_jobs_create(workers);
    return g_default_jobs;
}

JCE_API void JCE_CALL
jce_jobs_shutdown_default(void)
{
    if (g_default_jobs) {
        jce_jobs_destroy(g_default_jobs);
        g_default_jobs = NULL;
    }
    g_default_jobs_tried = 0;
}
