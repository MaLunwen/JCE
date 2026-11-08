/*
 * jce_memory.c  mimalloc-backed implementation of JCE_MALLOC / friends,
 * and the engine's single allocation-accounting point.
 *
 * One of only TWO files in the engine that may #include <mimalloc.h>
 * (the other is jce_allocator.c). Keeping the dependency funnelled
 * through these two files means swapping the allocator never ripples
 * through the rest of the codebase.
 *
 * Both mimalloc front-ends report into the counters below: the
 * JCE_MALLOC macros implemented here, and the jce_allocator_t default
 * vtable in jce_allocator.c (via jce__alloc_account_*). Each used to
 * keep half the picture — the vtable owned live/peak/per-frame bytes,
 * the macros owned only the Tracy heap view — so no reader saw the
 * engine's true total, and a block crossing between them unbalanced
 * whichever counter had seen only one end of its life.
 *
 * This owner is also compiled directly by host tools. Its counter adapter
 * needs no SDL/thread service, heap, registration or runtime initialization.
 */

#include "jce_memory.h"
#include "os/platform/jce_native_atomic_word_internal.h"

#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_profiler.h>

#include <mimalloc.h>
#include <string.h>

/* ================================================================== */
/* Debug allocation tracking (engine-side leak hunting)                */
/* ================================================================== */

#if JCE_ALLOC_TRACKING
/* Every shared word is atomic. A multi-word observation is not one globally
 * consistent instant; native-word wrap and the tracked-heap scope remain. */
static JceNativeAtomicWord s_live_bytes, s_live_count, s_peak_bytes;
static JceNativeAtomicWord s_total_allocs, s_total_frees;
static JceNativeAtomicWord s_bkt_bytes[JCE_ALLOC_TRACK_BUCKETS];
static JceNativeAtomicWord s_bkt_count[JCE_ALLOC_TRACK_BUCKETS];

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
    size_t live = jce_native_atomic_word_add(&s_live_bytes, sz) + sz;
    (void)jce_native_atomic_word_add(&s_live_count, 1);
    (void)jce_native_atomic_word_add(&s_total_allocs, 1);
    (void)jce_native_atomic_word_add(&s_bkt_bytes[b], sz);
    (void)jce_native_atomic_word_add(&s_bkt_count[b], 1);
    size_t peak = jce_native_atomic_word_load(&s_peak_bytes);
    while (peak < live) {
        if (jce_native_atomic_word_compare_exchange(&s_peak_bytes, peak, live)) break;
        peak = jce_native_atomic_word_load(&s_peak_bytes);
    }
}
static void track_on_free_size(size_t sz)
{
    int b = track_bucket(sz);
    (void)jce_native_atomic_word_add(&s_live_bytes, (size_t)0 - sz);
    (void)jce_native_atomic_word_add(&s_live_count, (size_t)0 - 1);
    (void)jce_native_atomic_word_add(&s_total_frees, 1);
    (void)jce_native_atomic_word_add(&s_bkt_bytes[b], (size_t)0 - sz);
    (void)jce_native_atomic_word_add(&s_bkt_count[b], (size_t)0 - 1);
}
static void track_on_free(void *p)   /* call BEFORE mi_free, while p is valid */
{
    if (p) track_on_free_size(mi_usable_size(p));
}
#else
#  define track_on_free_size(sz) ((void)(sz))
#  define track_on_alloc(p) ((void)(p))
#  define track_on_free(p)  ((void)(p))
#endif

/* Always-on per-frame allocation counters (rank-9): two native-word increments
 * per allocation, compiled into RELEASE too (unlike the NDEBUG-only tracker
 * above), so JCE_PERF_LOG can surface allocs/frame + KB/frame in a profiling
 * build — making per-frame heap churn measurable (verify-before-fix) instead of
 * reasoned-from-code. Atomic words count concurrent producers without a
 * process-wide lock; separate reads do not form a globally consistent pair. */
static JceNativeAtomicWord s_pf_allocs, s_pf_alloc_bytes; /* native-word wrap */
static size_t s_pf_last_allocs, s_pf_last_bytes; /* single consumer thread */

void jce_alloc_frame_delta(uint64_t *out_allocs, uint64_t *out_bytes)
{
    size_t a = jce_native_atomic_word_load(&s_pf_allocs);
    size_t b = jce_native_atomic_word_load(&s_pf_alloc_bytes);
    if (out_allocs) *out_allocs = (uint64_t)(a - s_pf_last_allocs);
    if (out_bytes)  *out_bytes  = (uint64_t)(b - s_pf_last_bytes);
    s_pf_last_allocs = a;
    s_pf_last_bytes  = b;
}

bool jce_alloc_track_snapshot(JceAllocTrack *out)
{
    if (!out) return false;
#if JCE_ALLOC_TRACKING
    out->live_bytes   = (uint64_t)jce_native_atomic_word_load(&s_live_bytes);
    out->live_count   = (uint64_t)jce_native_atomic_word_load(&s_live_count);
    out->peak_bytes   = (uint64_t)jce_native_atomic_word_load(&s_peak_bytes);
    out->total_allocs = (uint64_t)jce_native_atomic_word_load(&s_total_allocs);
    out->total_frees  = (uint64_t)jce_native_atomic_word_load(&s_total_frees);
    for (int i = 0; i < JCE_ALLOC_TRACK_BUCKETS; ++i) {
        out->bucket_bytes[i] = (uint64_t)jce_native_atomic_word_load(&s_bkt_bytes[i]);
        out->bucket_count[i] = (uint64_t)jce_native_atomic_word_load(&s_bkt_count[i]);
    }
    return true;
#else
    memset(out, 0, sizeof(*out));
    return false;
#endif
}

/* ================================================================== */
/* Shared accounting point (both mimalloc front-ends report here)      */
/* ================================================================== */

void jce__alloc_account_alloc(void *ptr, size_t requested)
{
    if (!ptr) return;
    track_on_alloc(ptr);
    (void)jce_native_atomic_word_add(&s_pf_allocs, 1);
    (void)jce_native_atomic_word_add(&s_pf_alloc_bytes, requested);
    JCE_PROFILE_ALLOC(ptr, requested);
}

void jce__alloc_account_free(void *ptr)   /* call BEFORE the block is released */
{
    if (!ptr) return;
    JCE_PROFILE_FREE(ptr);
    track_on_free(ptr);                 /* needs the block still valid */
}

/* ================================================================== */
/* JCE_MALLOC front-end                                                */
/* ================================================================== */

void *jce__malloc_tracked(size_t size)
{
    void *p = mi_malloc(size);
    jce__alloc_account_alloc(p, size);
    return p;
}

void *jce__calloc_tracked(size_t count, size_t size)
{
    void *p = mi_calloc(count, size);
    jce__alloc_account_alloc(p, count * size);
    return p;
}

void *jce__realloc_tracked(void *ptr, size_t size)
{
    if (size == 0) {
        jce__free_tracked(ptr);
        return NULL;
    }
    const uintptr_t old_address = (uintptr_t)ptr;
#if JCE_ALLOC_TRACKING
    const size_t old_size = ptr ? mi_usable_size(ptr) : 0;
#else
    const size_t old_size = 0;
#endif
    void *p = mi_realloc(ptr, size);
    if (!p) return NULL; /* The old block and its accounting remain live. */
    if (old_address) {
        /* The old allocation may have moved: never query/dereference it now. */
        JCE_PROFILE_FREE((void *)old_address);
        track_on_free_size(old_size);
    }
    jce__alloc_account_alloc(p, size);
    return p;
}

void jce__free_tracked(void *ptr)
{
    jce__alloc_account_free(ptr);        /* account before the block is freed  */
    mi_free(ptr);
}
