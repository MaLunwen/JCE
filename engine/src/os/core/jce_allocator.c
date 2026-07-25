/*
 * jce_allocator.c  Default allocator + arena implementation.
 *
 * Default allocator delegates to mi_malloc / mi_realloc / mi_free.
 * Arena is a simple linear bump allocator with 16-byte alignment.
 *
 * Allocation accounting is NOT kept here: this file and jce_memory.c are
 * two front-ends over one mimalloc heap, so both report through the
 * shared jce__alloc_account_* hooks (owned by jce_memory.c, which the
 * host tools link without this SDL-dependent TU). One set of counters
 * therefore covers the whole engine heap instead of one front-end each.
 */

#include <jce/os/core/jce_allocator.h>

#include "jce_memory.h"        /* jce__alloc_account_*, JCE_ALLOC_TRACKING */

#include <mimalloc.h>
#include <SDL3/SDL_stdinc.h>   /* SDL_SetMemoryFunctions (allocator bridge) */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ================================================================== */
/* Default allocator (mimalloc)                                        */
/* ================================================================== */

static void *default_alloc(size_t size, void *ctx)
{
    (void)ctx;
    void *p = mi_malloc(size);
    jce__alloc_account_alloc(p, size);
    return p;
}

static void *default_realloc(void *ptr, size_t new_size, void *ctx)
{
    (void)ctx;
    jce__alloc_account_free(ptr);            /* drop old accounting (NULL ⇒ no-op) */
    void *p = mi_realloc(ptr, new_size);
    jce__alloc_account_alloc(p, new_size);   /* add new accounting (NULL ⇒ no-op)  */
    return p;
}

static void default_free(void *ptr, void *ctx)
{
    (void)ctx;
    jce__alloc_account_free(ptr);       /* account before the block is freed  */
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
    jce__alloc_account_alloc(p, size);
    return p;
}

void jce_aligned_free(void *ptr)
{
    jce__alloc_account_free(ptr);
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

void jce_alloc_low_mem_mode(bool on)
{
    /* Immediate purge: freed segments decommit right away instead of on the
     * default 10ms(+10x arena multiplier) lazy schedule.  Measured on the
     * editor's startup transient (asset decode/cook burst): the default
     * retained ~1.3GB committed at idle; purge_delay=0 returned it
     * (2431 -> 1103MB private).  Costs decommit syscalls on free-heavy
     * paths, so it is the 512MB-charter mode, not the default. */
    mi_option_set(mi_option_purge_delay, on ? 0 : 10);
}

void jce_alloc_trim(bool aggressive)
{
    /* mimalloc's purge is opportunistic (piggybacks on allocation activity):
     * an IDLE process — the editor at rest renders with 0 allocs/frame —
     * never returns its startup-peak commit.  mi_collect walks the heaps and
     * purges retained segments on demand; `aggressive` additionally frees
     * every candidate page (low-memory machines / explicit trim points). */
    mi_collect(aggressive);
}

/* ================================================================== */
/* Third-party allocator bridge                                        */
/* ================================================================== */

/* Deliberately NOT routed through jce__alloc_account_*: these serve foreign
 * heaps (bgfx, SDL) whose churn would drown the engine-side leak trend the
 * counters exist to expose.  Alloc and free are both unaccounted, so the
 * bridge is self-consistent — it neither inflates nor unbalances the totals.
 * Foreign memory is still visible through jce_mem_stats (process-level). */

void *jce_realloc_aligned(void *ptr, size_t size, size_t align)
{
    if (size == 0) {           /* full malloc contract: size 0 == free */
        mi_free(ptr);
        return NULL;
    }
    if (align <= sizeof(void *))
        return ptr ? mi_realloc(ptr, size) : mi_malloc(size);
    return ptr ? mi_realloc_aligned(ptr, size, align)
               : mi_malloc_aligned(size, align);
}

void jce_free_raw(void *ptr)
{
    mi_free(ptr);              /* mi_free handles plain AND aligned blocks */
}

bool jce_alloc_hook_sdl(void)
{
    /* SDL's malloc/calloc/realloc/free signatures match mimalloc's exactly,
     * so the functions install directly.  The outstanding-allocation guard is
     * load-bearing: anything SDL allocated through the PREVIOUS functions
     * would be freed through OURS after the swap — refuse instead (the
     * caller logs; the process simply keeps SDL on its own heap). */
    if (SDL_GetNumAllocations() != 0) return false;
    return SDL_SetMemoryFunctions(mi_malloc, mi_calloc, mi_realloc, mi_free);
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
