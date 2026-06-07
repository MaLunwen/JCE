/*
 * jce_allocator.c  Default allocator + arena implementation.
 *
 * Default allocator delegates to mi_malloc / mi_realloc / mi_free.
 * Arena is a simple linear bump allocator with 16-byte alignment.
 */

#include <jce/os/core/jce_allocator.h>

#include <mimalloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ================================================================== */
/* Debug allocation tracking (engine-side leak hunting)                */
/* ================================================================== */

#ifndef NDEBUG
#  define JCE_ALLOC_TRACKING 1
#else
#  define JCE_ALLOC_TRACKING 0
#endif

#if JCE_ALLOC_TRACKING
/* Native-word counters (no tearing on aligned access; no locks ⇒ APPROXIMATE
 * under concurrent allocation, which is fine for a leak trend). */
static size_t s_live_bytes, s_live_count, s_peak_bytes;
static size_t s_total_allocs, s_total_frees;
static size_t s_bkt_bytes[JCE_ALLOC_TRACK_BUCKETS];
static size_t s_bkt_count[JCE_ALLOC_TRACK_BUCKETS];

static int track_bucket(size_t sz)
{
    int b = 0;
    size_t lim = 16;
    while (sz >= lim && b < JCE_ALLOC_TRACK_BUCKETS - 1) { lim <<= 1; ++b; }
    return b;
}
static void track_on_alloc(void *p)
{
    if (!p) return;
    size_t sz = mi_usable_size(p);
    int b = track_bucket(sz);
    s_live_bytes += sz; s_live_count += 1; s_total_allocs += 1;
    s_bkt_bytes[b] += sz; s_bkt_count[b] += 1;
    if (s_live_bytes > s_peak_bytes) s_peak_bytes = s_live_bytes;
}
static void track_on_free(void *p)   /* call BEFORE mi_free, while p is valid */
{
    if (!p) return;
    size_t sz = mi_usable_size(p);
    int b = track_bucket(sz);
    s_live_bytes -= sz; s_live_count -= 1; s_total_frees += 1;
    s_bkt_bytes[b] -= sz; s_bkt_count[b] -= 1;
}
#else
#  define track_on_alloc(p) ((void)(p))
#  define track_on_free(p)  ((void)(p))
#endif

/* ================================================================== */
/* Default allocator (mimalloc)                                        */
/* ================================================================== */

static void *default_alloc(size_t size, void *ctx)
{
    (void)ctx;
    void *p = mi_malloc(size);
    track_on_alloc(p);
    return p;
}

static void *default_realloc(void *ptr, size_t new_size, void *ctx)
{
    (void)ctx;
    track_on_free(ptr);                 /* drop old accounting (NULL ⇒ no-op) */
    void *p = mi_realloc(ptr, new_size);
    track_on_alloc(p);                  /* add new accounting (NULL ⇒ no-op)  */
    return p;
}

static void default_free(void *ptr, void *ctx)
{
    (void)ctx;
    track_on_free(ptr);                 /* account before the block is freed  */
    mi_free(ptr);
}

jce_allocator_t jce_allocator_default(void)
{
    jce_allocator_t a;
    a.alloc   = default_alloc;
    a.realloc = default_realloc;
    a.free    = default_free;
    a.ctx     = NULL;
    return a;
}

/* ================================================================== */
/* Arena (linear bump allocator)                                       */
/* ================================================================== */

#define ARENA_ALIGN 16u

struct jce_arena {
    jce_allocator_t backing;
    uint8_t        *base;
    size_t          capacity;
    size_t          used;
};

jce_arena_t *jce_arena_create(jce_allocator_t backing, size_t capacity)
{
    if (capacity == 0) return NULL;

    jce_arena_t *a = (jce_arena_t *)backing.alloc(
        sizeof(jce_arena_t), backing.ctx);
    if (!a) return NULL;

    a->base = (uint8_t *)backing.alloc(capacity, backing.ctx);
    if (!a->base) {
        backing.free(a, backing.ctx);
        return NULL;
    }

    a->backing  = backing;
    a->capacity = capacity;
    a->used     = 0;
    return a;
}

void *jce_arena_push(jce_arena_t *a, size_t size)
{
    if (!a || size == 0) return NULL;

    /* Align up to ARENA_ALIGN. */
    size_t aligned = (a->used + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1);
    if (aligned + size > a->capacity) return NULL;

    void *ptr = a->base + aligned;
    a->used = aligned + size;
    return ptr;
}

void *jce_arena_push_zero(jce_arena_t *a, size_t size)
{
    void *ptr = jce_arena_push(a, size);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}

void jce_arena_reset(jce_arena_t *a)
{
    if (a) a->used = 0;
}

size_t jce_arena_used(const jce_arena_t *a)
{
    return a ? a->used : 0;
}

size_t jce_arena_capacity(const jce_arena_t *a)
{
    return a ? a->capacity : 0;
}

void jce_arena_destroy(jce_arena_t *a)
{
    if (!a) return;
    jce_allocator_t b = a->backing;
    b.free(a->base, b.ctx);
    b.free(a, b.ctx);
}

/* ================================================================== */
/* Aligned allocation                                                  */
/* ================================================================== */

void *jce_aligned_alloc(size_t size, size_t alignment)
{
    void *p = mi_malloc_aligned(size, alignment);
    track_on_alloc(p);
    return p;
}

void jce_aligned_free(void *ptr)
{
    track_on_free(ptr);
    mi_free(ptr);
}

bool jce_mem_stats(JceMemStats *out)
{
    if (!out) return false;

    size_t elapsed = 0, user_ms = 0, sys_ms = 0;
    size_t cur_rss = 0, peak_rss = 0;
    size_t cur_commit = 0, peak_commit = 0;
    size_t page_faults = 0;

    mi_process_info(&elapsed, &user_ms, &sys_ms,
                    &cur_rss, &peak_rss,
                    &cur_commit, &peak_commit,
                    &page_faults);

    out->current_rss    = cur_rss;
    out->peak_rss       = peak_rss;
    out->current_commit = cur_commit;
    out->peak_commit    = peak_commit;
    out->page_faults    = page_faults;
    out->elapsed_ms     = elapsed;
    return true;
}

/* ================================================================== */
/* Debug allocation tracking API                                       */
/* ================================================================== */

size_t jce_alloc_track_bucket_lo(int bucket)
{
    if (bucket <= 0) return 0;
    if (bucket >= JCE_ALLOC_TRACK_BUCKETS) bucket = JCE_ALLOC_TRACK_BUCKETS - 1;
    return (size_t)16 << (bucket - 1);
}

size_t jce_alloc_track_bucket_hi(int bucket)
{
    if (bucket < 0) bucket = 0;
    if (bucket >= JCE_ALLOC_TRACK_BUCKETS - 1) return (size_t)-1; /* open-ended */
    return (size_t)16 << bucket;
}

bool jce_alloc_track_snapshot(JceAllocTrack *out)
{
    if (!out) return false;
#if JCE_ALLOC_TRACKING
    out->live_bytes   = (uint64_t)s_live_bytes;
    out->live_count   = (uint64_t)s_live_count;
    out->peak_bytes   = (uint64_t)s_peak_bytes;
    out->total_allocs = (uint64_t)s_total_allocs;
    out->total_frees  = (uint64_t)s_total_frees;
    for (int i = 0; i < JCE_ALLOC_TRACK_BUCKETS; ++i) {
        out->bucket_bytes[i] = (uint64_t)s_bkt_bytes[i];
        out->bucket_count[i] = (uint64_t)s_bkt_count[i];
    }
    return true;
#else
    memset(out, 0, sizeof(*out));
    return false;
#endif
}

void jce_alloc_track_dump(void)
{
#if JCE_ALLOC_TRACKING
    JceAllocTrack t;
    jce_alloc_track_snapshot(&t);
    fprintf(stderr,
            "[alloc] engine live=%.2f MB count=%llu peak=%.2f MB allocs=%llu frees=%llu\n",
            (double)t.live_bytes / (1024.0 * 1024.0), (unsigned long long)t.live_count,
            (double)t.peak_bytes / (1024.0 * 1024.0),
            (unsigned long long)t.total_allocs, (unsigned long long)t.total_frees);
    /* Print the 5 largest size buckets by live bytes. */
    uint64_t tmp[JCE_ALLOC_TRACK_BUCKETS];
    for (int i = 0; i < JCE_ALLOC_TRACK_BUCKETS; ++i) tmp[i] = t.bucket_bytes[i];
    for (int n = 0; n < 5; ++n) {
        int best = -1;
        uint64_t bv = 0;
        for (int i = 0; i < JCE_ALLOC_TRACK_BUCKETS; ++i)
            if (tmp[i] > bv) { bv = tmp[i]; best = i; }
        if (best < 0 || bv == 0) break;
        fprintf(stderr, "[alloc]   size[%zu..%zu) live=%.2f MB count=%llu\n",
                jce_alloc_track_bucket_lo(best), jce_alloc_track_bucket_hi(best),
                (double)t.bucket_bytes[best] / (1024.0 * 1024.0),
                (unsigned long long)t.bucket_count[best]);
        tmp[best] = 0;
    }
    fflush(stderr);
#endif
}
