/* jce_archive_loader.c  Runtime resource loader for the JPAK archive format.
 *
 * Implements spec §13 (loader behavioural contract) and §14 (asynchronous,
 * frame-budgeted loading with a reference-counted, LRU-evicted cache).
 *
 * Structure:
 *   - A chained hash table keyed by path hash holds CacheEntry records.  Each
 *     entry carries the public {hash,data,size} view, a reference count, an
 *     LRU stamp, a load state, and whether `data` is a zero-copy mapping
 *     (borrowed, never freed) or an owned decompressed buffer.
 *   - Synchronous acquire() loads inline (serialised by io_lock for the shared
 *     decode context) and caches the result.
 *   - Asynchronous request()/poll()/tick(): loads run on a private structured
 *     executor. Threaded and cooperative modes share the same completion and
 *     ownership path; tick() performs all cache integration on the owner.
 *
 * Concurrency: `lock` guards all cache/table/queue/counter state; `io_lock`
 * serialises jce_archive_read (shared zstd DCtx).  Workers never touch the
 * cache directly, so all table mutation happens on the owner under `lock`.
 */

#include <jce/resource/jce_archive_loader.h>

#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_thread.h>

#include "os/core/jce_memory.h"

#include <string.h>

#define ARCHIVE_LOADER_MAX_WORKERS 16u
#define ARCHIVE_LOADER_MAX_TASKS   1024u

/* ── Load state ─────────────────────────────────────────────────────────── */
enum { LS_LOADING = 0, LS_READY = 1, LS_FAILED = 2 };

/* ── Cache entry (JceArchiveResource MUST be the first member so a public
 *    pointer can be cast back to the owning entry in release()) ──────────── */
typedef struct CacheEntry {
    JceArchiveResource pub;     /* {hash, data, size}                        */
    int32_t            refcount;
    uint64_t           lru;     /* last-access stamp; lower == older         */
    int                state;   /* LS_*                                      */
    int                borrowed;/* data points into the mmap; do not free    */
    struct CacheEntry *hnext;   /* hash-bucket chain                         */
} CacheEntry;

/* ── In-flight / queued load ────────────────────────────────────────────── */
typedef struct Job {
    JceArchiveLoader *loader;
    uint64_t          hash;
    JceArchiveEntry   entry;    /* copy of the index record                  */
    void             *buf;      /* produced bytes (owned or mapped)          */
    uint32_t          size;
    int               borrowed;
    int               success;
} Job;

/* ── Pending async request (id → hash) ──────────────────────────────────── */
typedef struct Request {
    JceArchiveRequestId id;
    uint64_t            hash;
    struct Request     *next;
} Request;

struct JceArchiveLoader {
    JceArchive   *archive;      /* borrowed                                  */

    /* config */
    size_t        budget;       /* 0 = unbounded                             */
    double        frame_ms;
    int           verify;
    uint32_t      worker_count;

    /* hash table */
    CacheEntry  **buckets;
    size_t        bucket_count; /* power of two                              */
    uint32_t      entry_count;

    uint64_t      lru_clock;
    size_t        bytes;        /* owned (decompressed) bytes resident       */

    /* requests */
    Request      *requests;
    JceArchiveRequestId next_id;

    /* async plumbing */
    JceAsyncExecutor *executor; /* private structured executor               */
    JceMutex     *lock;         /* guards table/queues/counters              */
    JceMutex     *io_lock;      /* serialises archive reads (shared DCtx)    */

    uint32_t      pending_count;               /* queued + in flight         */
};

/* ── Hash table ─────────────────────────────────────────────────────────── */

static size_t bucket_index(const JceArchiveLoader *l, uint64_t hash)
{
    /* Mix a little: the low bits of XXH3 are already well-distributed, but the
     * table is small so fold the high word in. */
    uint64_t h = hash ^ (hash >> 32);
    return (size_t)h & (l->bucket_count - 1);
}

static CacheEntry *ht_find(JceArchiveLoader *l, uint64_t hash)
{
    CacheEntry *e = l->buckets[bucket_index(l, hash)];
    while (e) {
        if (e->pub.hash == hash) return e;
        e = e->hnext;
    }
    return NULL;
}

static void ht_rehash(JceArchiveLoader *l, size_t new_count)
{
    CacheEntry **nb = JCE_NEW_ARRAY(CacheEntry *, new_count);
    if (!nb) return; /* keep old table; just skip growth */
    for (size_t i = 0; i < l->bucket_count; ++i) {
        CacheEntry *e = l->buckets[i];
        while (e) {
            CacheEntry *next = e->hnext;
            uint64_t h = e->pub.hash ^ (e->pub.hash >> 32);
            size_t bi = (size_t)h & (new_count - 1);
            e->hnext = nb[bi];
            nb[bi] = e;
            e = next;
        }
    }
    JCE_FREE(l->buckets);
    l->buckets = nb;
    l->bucket_count = new_count;
}

static CacheEntry *ht_insert(JceArchiveLoader *l, uint64_t hash)
{
    if (l->entry_count + 1 > (l->bucket_count * 3) / 4)
        ht_rehash(l, l->bucket_count * 2);

    CacheEntry *e = JCE_NEW(CacheEntry);
    if (!e) return NULL;
    e->pub.hash = hash;
    e->state = LS_LOADING;
    size_t bi = bucket_index(l, hash);
    e->hnext = l->buckets[bi];
    l->buckets[bi] = e;
    l->entry_count++;
    return e;
}

static void ht_remove(JceArchiveLoader *l, CacheEntry *target)
{
    CacheEntry **pp = &l->buckets[bucket_index(l, target->pub.hash)];
    while (*pp) {
        if (*pp == target) { *pp = target->hnext; break; }
        pp = &(*pp)->hnext;
    }
    if (!target->borrowed && target->pub.data)
        l->bytes -= target->pub.size;
    if (!target->borrowed && target->pub.data)
        JCE_FREE((void *)target->pub.data);
    JCE_FREE(target);
    l->entry_count--;
}

/* ── LRU eviction (call with lock held) ─────────────────────────────────── */

static void enforce_budget(JceArchiveLoader *l)
{
    if (l->budget == 0) return;
    while (l->bytes > l->budget) {
        /* find the least-recently-used, unreferenced, ready entry */
        CacheEntry *victim = NULL;
        for (size_t i = 0; i < l->bucket_count; ++i) {
            for (CacheEntry *e = l->buckets[i]; e; e = e->hnext) {
                if (e->state != LS_READY || e->refcount > 0) continue;
                if (e->borrowed) continue; /* mapped: no heap cost to reclaim */
                if (!victim || e->lru < victim->lru) victim = e;
            }
        }
        if (!victim) break; /* nothing evictable; over budget but pinned */
        ht_remove(l, victim);
    }
}

/* ── Load primitive (no lock held; serialise archive read via io_lock) ──── */

static void do_load(JceArchiveLoader *l, Job *j)
{
    const void *p = NULL;
    size_t sz = 0;

    /* §8.2 / §13: zero-copy for uncompressed, unencrypted, mappable entries. */
    if (jce_archive_map_entry(l->archive, &j->entry, &p, &sz)) {
        j->buf = (void *)p;
        j->size = (uint32_t)sz;
        j->borrowed = 1;
        j->success = 1;
        return;
    }

    size_t cap = j->entry.original_size ? j->entry.original_size : 1;
    void *b = JCE_MALLOC(cap);
    if (!b) return;

    jce_mutex_lock(l->io_lock);
    size_t n = jce_archive_read(l->archive, &j->entry, b, cap);
    jce_mutex_unlock(l->io_lock);

    if (n != j->entry.original_size) { JCE_FREE(b); return; }
    if (l->verify && !jce_archive_verify_entry(&j->entry, b, n)) {
        JCE_FREE(b);
        return;
    }
    j->buf = b;
    j->size = (uint32_t)n;
    j->borrowed = 0;
    j->success = 1;
}

/* ── Integrate a completed job into the cache (lock held) ───────────────── */

static void integrate(JceArchiveLoader *l, Job *j)
{
    CacheEntry *e = ht_find(l, j->hash);
    if (!e) {
        /* No placeholder (shouldn't happen): create one. */
        e = ht_insert(l, j->hash);
        if (!e) {
            if (j->buf && !j->borrowed) JCE_FREE(j->buf);
            j->buf = NULL;
            return;
        }
    }

    if (e->state == LS_READY) {
        /* Already satisfied by another path (e.g. a synchronous acquire that
         * raced the worker): discard the duplicate result. */
        if (j->buf && !j->borrowed) JCE_FREE(j->buf);
        j->buf = NULL;
        return;
    }

    if (!j->success) {
        e->state = LS_FAILED;
        return;
    }

    e->pub.data = j->buf;
    e->pub.size = j->size;
    e->borrowed = j->borrowed;
    e->state = LS_READY;
    e->lru = ++l->lru_clock;
    if (!e->borrowed) l->bytes += e->pub.size;
    j->buf = NULL; /* cache owns the decoded bytes now */
    enforce_budget(l);
}

/* ── Structured async callbacks ────────────────────────────────────────── */

static JceAsyncRunResult job_worker(JceAsyncContext *ctx, void *arg)
{
    Job *j = (Job *)arg;
    if (jce_async_context_cancel_requested(ctx))
        return JCE_ASYNC_RUN_CANCELLED;

    do_load(j->loader, j);
    if (jce_async_context_cancel_requested(ctx))
        return JCE_ASYNC_RUN_CANCELLED;
    if (!j->success) {
        jce_async_context_fail(ctx, -1, "archive resource load failed");
        return JCE_ASYNC_RUN_FAILED;
    }
    return JCE_ASYNC_RUN_SUCCESS;
}

static void job_complete(JceAsyncTask *task, void *arg)
{
    Job *j = (Job *)arg;
    JceArchiveLoader *l = j->loader;
    if (jce_async_task_state(task) != JCE_ASYNC_STATE_SUCCEEDED)
        j->success = 0;

    jce_mutex_lock(l->lock);
    integrate(l, j);
    if (l->pending_count > 0) l->pending_count--;
    jce_mutex_unlock(l->lock);
}

static void job_cleanup(void *arg)
{
    Job *j = (Job *)arg;
    if (j->buf && !j->borrowed) JCE_FREE(j->buf);
    JCE_FREE(j);
}

/* Submission is non-inline. On rejection, restore the cache to its state
 * before begin_load() so callers can retry on a later frame. Lock held. */
static int dispatch_job(JceArchiveLoader *l, Job *j)
{
    JceAsyncTaskDesc desc;
    jce_async_task_desc_init(&desc);
    desc.work       = job_worker;
    desc.complete   = job_complete;
    desc.cleanup    = job_cleanup;
    desc.user_data  = j;
    desc.debug_name = "archive.resource.load";
    desc.priority   = JCE_ASYNC_PRIORITY_NORMAL;

    JceAsyncTask *task = jce_async_submit(l->executor, &desc);
    if (task) {
        jce_async_task_release(task);
        return 1;
    }

    CacheEntry *e = ht_find(l, j->hash);
    if (e && e->state == LS_LOADING) ht_remove(l, e);
    if (l->pending_count > 0) l->pending_count--;
    job_cleanup(j);
    return 0;
}

/* ── Helpers ────────────────────────────────────────────────────────────── */

/* Look up the index entry and create a LOADING placeholder + Job.  Returns the
 * job (caller dispatches/queues it) or NULL if absent / already resident /
 * allocation failure.  Lock held. */
static Job *begin_load(JceArchiveLoader *l, uint64_t hash)
{
    CacheEntry *e = ht_find(l, hash);
    if (e) return NULL; /* already ready or already loading */

    /* find the on-disk entry by hash via the resident index */
    const JceArchiveEntry *ae = NULL;
    uint32_t n = jce_archive_count(l->archive);
    for (uint32_t i = 0; i < n; ++i) {
        const JceArchiveEntry *cur = jce_archive_get(l->archive, i);
        if (cur && cur->path_hash == hash) { ae = cur; break; }
    }
    if (!ae) return NULL;

    e = ht_insert(l, hash);
    if (!e) return NULL;

    Job *j = JCE_NEW(Job);
    if (!j) { ht_remove(l, e); return NULL; }
    j->loader = l;
    j->hash = hash;
    j->entry = *ae;
    l->pending_count++;
    return j;
}

static void add_request(JceArchiveLoader *l, uint64_t hash, JceArchiveRequestId id)
{
    Request *r = JCE_NEW(Request);
    if (!r) return;
    r->id = id;
    r->hash = hash;
    r->next = l->requests;
    l->requests = r;
}

/* ── Public: create / destroy ───────────────────────────────────────────── */

JceArchiveLoader *jce_archive_loader_create(JceArchive *archive,
                                            const JceArchiveLoaderConfig *cfg)
{
    if (!archive) return NULL;

    JceArchiveLoader *l = JCE_NEW(JceArchiveLoader);
    if (!l) return NULL;

    l->archive = archive;
    l->budget = cfg ? cfg->cache_budget_bytes : 0;
    l->frame_ms = (cfg && cfg->frame_budget_ms > 0.0) ? cfg->frame_budget_ms : 2.0;
    l->verify = cfg ? cfg->verify_crc : 0;
    l->worker_count = cfg ? cfg->worker_count : 0;
    if (l->worker_count > ARCHIVE_LOADER_MAX_WORKERS)
        l->worker_count = ARCHIVE_LOADER_MAX_WORKERS;
    l->next_id = 1;

    l->bucket_count = 64;
    l->buckets = JCE_NEW_ARRAY(CacheEntry *, l->bucket_count);
    l->lock = jce_mutex_create();
    l->io_lock = jce_mutex_create();
    if (!l->buckets || !l->lock || !l->io_lock) {
        jce_archive_loader_destroy(l);
        return NULL;
    }

    JceAsyncExecutorConfig async_cfg;
    jce_async_executor_config_init(&async_cfg);
    async_cfg.mode = l->worker_count > 0
                   ? JCE_ASYNC_EXECUTION_THREADED
                   : JCE_ASYNC_EXECUTION_COOPERATIVE;
    async_cfg.worker_count = l->worker_count;
    async_cfg.max_tasks = ARCHIVE_LOADER_MAX_TASKS;
    async_cfg.cooperative_tasks_per_pump = 1;
    async_cfg.cooperative_completions_per_pump = 64;
    async_cfg.cooperative_time_budget_us =
        (uint64_t)(l->frame_ms * 1000.0);
    async_cfg.debug_name = "archive-loader";
    l->executor = jce_async_executor_create(&async_cfg);
    if (!l->executor) {
        jce_archive_loader_destroy(l);
        return NULL;
    }
    return l;
}

void jce_archive_loader_destroy(JceArchiveLoader *loader)
{
    if (!loader) return;

    if (loader->executor) {
        (void)jce_async_executor_shutdown(loader->executor,
                                          JCE_ASYNC_SHUTDOWN_CANCEL_ALL,
                                          JCE_ASYNC_WAIT_INFINITE);
        jce_async_executor_destroy(loader->executor);
        loader->executor = NULL;
    }

    /* Free requests. */
    Request *r = loader->requests;
    while (r) { Request *n = r->next; JCE_FREE(r); r = n; }

    /* Free cache entries (and owned buffers). */
    if (loader->buckets) {
        for (size_t i = 0; i < loader->bucket_count; ++i) {
            CacheEntry *e = loader->buckets[i];
            while (e) {
                CacheEntry *n = e->hnext;
                if (!e->borrowed && e->pub.data) JCE_FREE((void *)e->pub.data);
                JCE_FREE(e);
                e = n;
            }
        }
        JCE_FREE(loader->buckets);
    }

    if (loader->lock) jce_mutex_destroy(loader->lock);
    if (loader->io_lock) jce_mutex_destroy(loader->io_lock);
    JCE_FREE(loader);
}

/* ── Public: synchronous acquire ────────────────────────────────────────── */

const JceArchiveResource *jce_archive_loader_acquire_hash(JceArchiveLoader *loader,
                                                          uint64_t hash)
{
    if (!loader) return NULL;

    jce_mutex_lock(loader->lock);
    CacheEntry *e = ht_find(loader, hash);
    if (e && e->state == LS_READY) {
        e->refcount++;
        e->lru = ++loader->lru_clock;
        jce_mutex_unlock(loader->lock);
        return &e->pub;
    }
    jce_mutex_unlock(loader->lock);

    /* Locate the on-disk entry (read-only; safe without the lock). */
    const JceArchiveEntry *ae = NULL;
    uint32_t n = jce_archive_count(loader->archive);
    for (uint32_t i = 0; i < n; ++i) {
        const JceArchiveEntry *cur = jce_archive_get(loader->archive, i);
        if (cur && cur->path_hash == hash) { ae = cur; break; }
    }
    if (!ae) return NULL;

    Job job;
    memset(&job, 0, sizeof(job));
    job.loader = loader;
    job.hash = hash;
    job.entry = *ae;
    do_load(loader, &job);
    if (!job.success) return NULL;

    jce_mutex_lock(loader->lock);
    e = ht_find(loader, hash);
    if (e && e->state == LS_READY) {
        /* Raced another loader: keep the existing copy, drop ours. */
        if (!job.borrowed) { jce_mutex_unlock(loader->lock); JCE_FREE(job.buf); jce_mutex_lock(loader->lock); }
        e->refcount++;
        e->lru = ++loader->lru_clock;
        jce_mutex_unlock(loader->lock);
        return &e->pub;
    }
    if (!e) {
        e = ht_insert(loader, hash);
        if (!e) {
            jce_mutex_unlock(loader->lock);
            if (!job.borrowed) JCE_FREE(job.buf);
            return NULL;
        }
    }
    e->pub.data = job.buf;
    e->pub.size = job.size;
    e->borrowed = job.borrowed;
    e->state = LS_READY;
    e->refcount++;
    e->lru = ++loader->lru_clock;
    if (!e->borrowed) loader->bytes += e->pub.size;
    enforce_budget(loader);
    jce_mutex_unlock(loader->lock);
    return &e->pub;
}

const JceArchiveResource *jce_archive_loader_acquire(JceArchiveLoader *loader,
                                                     const char *path)
{
    if (!loader || !path) return NULL;
    uint64_t hash = jce_archive_path_id(loader->archive, path);
    if (hash == 0) return NULL;
    return jce_archive_loader_acquire_hash(loader, hash);
}

void jce_archive_loader_release(JceArchiveLoader *loader, const JceArchiveResource *res)
{
    if (!loader || !res) return;
    /* pub is the first member of CacheEntry. */
    CacheEntry *e = (CacheEntry *)res;
    jce_mutex_lock(loader->lock);
    if (e->refcount > 0) e->refcount--;
    jce_mutex_unlock(loader->lock);
}

/* ── Public: asynchronous loading ───────────────────────────────────────── */

JceArchiveRequestId jce_archive_loader_request(JceArchiveLoader *loader, const char *path)
{
    if (!loader || !path) return 0;
    uint64_t hash = jce_archive_path_id(loader->archive, path);
    if (hash == 0) return 0;

    jce_mutex_lock(loader->lock);

    CacheEntry *e = ht_find(loader, hash);
    if (!e) {
        Job *j = begin_load(loader, hash);
        if (!j) { jce_mutex_unlock(loader->lock); return 0; } /* absent */
        if (!dispatch_job(loader, j)) {
            jce_mutex_unlock(loader->lock);
            return 0;
        }
    }

    JceArchiveRequestId id = loader->next_id++;
    add_request(loader, hash, id);
    jce_mutex_unlock(loader->lock);
    return id;
}

int jce_archive_loader_poll(JceArchiveLoader *loader, JceArchiveRequestId id,
                            const JceArchiveResource **out)
{
    if (!loader || id == 0) return -1;

    jce_mutex_lock(loader->lock);

    /* find the request */
    Request **pp = &loader->requests;
    while (*pp && (*pp)->id != id) pp = &(*pp)->next;
    if (!*pp) { jce_mutex_unlock(loader->lock); return -1; }

    Request *r = *pp;
    CacheEntry *e = ht_find(loader, r->hash);
    int result;
    if (!e) {
        result = -1; /* evicted before poll, or never created */
    } else if (e->state == LS_READY) {
        e->refcount++;
        e->lru = ++loader->lru_clock;
        if (out) *out = &e->pub;
        result = 1;
    } else if (e->state == LS_FAILED) {
        result = -1;
    } else {
        result = 0; /* still loading */
    }

    if (result != 0) {        /* consume the request on a terminal result */
        *pp = r->next;
        JCE_FREE(r);
    }
    jce_mutex_unlock(loader->lock);
    return result;
}

void jce_archive_loader_tick(JceArchiveLoader *loader)
{
    if (!loader || !loader->executor) return;

    JceAsyncPumpBudget budget;
    jce_async_pump_budget_init(&budget);
    budget.max_work_items =
        jce_async_executor_mode(loader->executor) ==
        JCE_ASYNC_EXECUTION_COOPERATIVE ? 1u : 0u;
    budget.max_completions = 64;
    budget.max_time_us = (uint64_t)(loader->frame_ms * 1000.0);
    (void)jce_async_executor_pump(loader->executor, &budget);
}

void jce_archive_loader_preload(JceArchiveLoader *loader, const char *path)
{
    if (!loader || !path) return;
    uint64_t hash = jce_archive_path_id(loader->archive, path);
    if (hash == 0) return;

    jce_mutex_lock(loader->lock);
    if (!ht_find(loader, hash)) {
        Job *j = begin_load(loader, hash);
        if (j) (void)dispatch_job(loader, j);
    }
    jce_mutex_unlock(loader->lock);
}

/* ── Public: introspection ──────────────────────────────────────────────── */

size_t jce_archive_loader_cache_bytes(const JceArchiveLoader *loader)
{
    return loader ? loader->bytes : 0;
}

uint32_t jce_archive_loader_cache_count(const JceArchiveLoader *loader)
{
    return loader ? loader->entry_count : 0;
}

uint32_t jce_archive_loader_pending_count(const JceArchiveLoader *loader)
{
    return loader ? loader->pending_count : 0;
}
