/*
 * jce_allocator.h  Unified allocator interface.
 *
 * All engine allocations should go through a jce_allocator_t.
 * The default implementation delegates to mimalloc; users may
 * provide custom allocators for specific subsystems.
 *
 * Layer: Foundation (Layer 1 — no engine dependencies).
 */

#ifndef JCE_ALLOCATOR_H
#define JCE_ALLOCATOR_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Allocator interface                                                 */
/* ================================================================== */

typedef struct jce_allocator {
    void *(*alloc)(size_t size, void *ctx);
    void *(*realloc)(void *ptr, size_t new_size, void *ctx);
    void  (*free)(void *ptr, void *ctx);
    void  *ctx;   /* opaque context passed to every call */
} jce_allocator_t;

/* Default allocator backed by SDL_malloc (which is mimalloc when
   SDL is configured with it, otherwise the platform allocator). */
JCE_API jce_allocator_t JCE_CALL jce_allocator_default(void);

/* ================================================================== */
/* Convenience macros                                                  */
/* ================================================================== */

#define JCE_ALLOC(a, size)         (a).alloc((size), (a).ctx)
#define JCE_AREALLOC(a, ptr, size) (a).realloc((ptr), (size), (a).ctx)
#define JCE_AFREE(a, ptr)          (a).free((ptr), (a).ctx)
#define JCE_ANEW(a, T)             (T *)JCE_ALLOC(a, sizeof(T))
#define JCE_ANEW_ARRAY(a, T, n)    (T *)JCE_ALLOC(a, sizeof(T) * (n))

/* ================================================================== */
/* Arena (linear / bump allocator)                                     */
/* ================================================================== */

/*
 * Frame-scoped linear allocator.
 *   - Push: O(1) pointer bump, no overhead
 *   - Reset: O(1), rewind to base — zero fragmentation
 *   - Great for per-frame scratch data, string formatting, etc.
 */
typedef struct jce_arena jce_arena_t;

/* Create an arena with the given byte capacity.
   Uses 'backing' for the single large allocation. */
JCE_API jce_arena_t *jce_arena_create(jce_allocator_t backing, size_t capacity);

/* Allocate 'size' bytes from the arena.  Returns NULL if exhausted. */
JCE_API void *jce_arena_push(jce_arena_t *a, size_t size);

/* Allocate and zero-fill 'size' bytes. */
JCE_API void *jce_arena_push_zero(jce_arena_t *a, size_t size);

/* Reset the arena to empty.  All previous pointers are invalidated. */
JCE_API void JCE_CALL jce_arena_reset(jce_arena_t *a);

/* Total bytes currently used. */
JCE_API size_t JCE_CALL jce_arena_used(const jce_arena_t *a);

/* Total capacity in bytes. */
JCE_API size_t JCE_CALL jce_arena_capacity(const jce_arena_t *a);

/* Destroy the arena and free the backing allocation. */
JCE_API void JCE_CALL jce_arena_destroy(jce_arena_t *a);

/* Typed push convenience. */
#define JCE_ARENA_PUSH(arena, T)       (T *)jce_arena_push((arena), sizeof(T))
#define JCE_ARENA_PUSH_ARRAY(arena,T,n) (T *)jce_arena_push((arena), sizeof(T)*(n))

/* ================================================================== */
/* Aligned allocation (Layer 1 — mimalloc-backed, header-clean)        */
/* ================================================================== */

/* Allocate `size` bytes aligned to `alignment` (must be power of two).
   Returned pointer must be released with jce_aligned_free(). Used by
   third-party decoders (e.g. libhevc) that require SIMD-aligned bufs. */
JCE_API void *jce_aligned_alloc(size_t size, size_t alignment);

/* Release a buffer obtained from jce_aligned_alloc(). NULL is OK. */
JCE_API void JCE_CALL jce_aligned_free(void *ptr);

/* ================================================================== */
/* Process-wide allocator stats (mimalloc-backed)                      */
/* ================================================================== */

typedef struct JceMemStats {
    size_t current_rss;     /* current resident set size, bytes              */
    size_t peak_rss;        /* peak resident set size, bytes                 */
    size_t current_commit;  /* currently committed virtual memory, bytes     */
    size_t peak_commit;     /* peak committed virtual memory, bytes          */
    size_t page_faults;     /* hard page faults                              */
    size_t elapsed_ms;      /* process wall time, milliseconds               */
} JceMemStats;

/* Snapshot the current allocator/process memory stats.  Backed by
 * `mi_process_info()`.  Cheap enough to call once per frame for HUDs.
 * Always populates `*out`; returns false only if `out` is NULL. */
JCE_API bool jce_mem_stats(JceMemStats *out);

/* ================================================================== */
/* Debug allocation tracking (engine-side leak hunting)                */
/* ================================================================== */

/*
 * In DEBUG builds (NDEBUG undefined) the default + aligned allocators record
 * live bytes and allocation counts, bucketed by size class.  Engine memory
 * goes through mimalloc (NOT the CRT heap), so a CRT leak dump and VS native
 * heap snapshots miss it — a rising live-byte total here pinpoints an
 * ENGINE-side leak and the size class it lives in.  Portable: plain native-word
 * counters, no platform-specific libraries.  Compiled out in release (snapshot
 * returns false, dump is a no-op), so zero overhead there.
 *
 * NOTE: counters are updated without locks, so values are APPROXIMATE under
 * heavy concurrent allocation (asset worker threads) — fine for a leak TREND.
 */
#define JCE_ALLOC_TRACK_BUCKETS 28

typedef struct JceAllocTrack {
    uint64_t live_bytes;     /* currently-allocated bytes (alloc - free)        */
    uint64_t live_count;     /* currently-live allocations                      */
    uint64_t peak_bytes;     /* high-water mark of live_bytes                   */
    uint64_t total_allocs;   /* cumulative alloc calls                          */
    uint64_t total_frees;    /* cumulative free calls                           */
    uint64_t bucket_bytes[JCE_ALLOC_TRACK_BUCKETS]; /* live bytes  per size class */
    uint64_t bucket_count[JCE_ALLOC_TRACK_BUCKETS]; /* live allocs per size class */
} JceAllocTrack;

/* Bucket b covers sizes [16<<(b-1), 16<<b); bucket 0 covers [0,16). */
JCE_API size_t jce_alloc_track_bucket_lo(int bucket);
JCE_API size_t jce_alloc_track_bucket_hi(int bucket);

/* Copy the current tracking snapshot to *out.  Returns false (and zeroes *out)
 * when tracking is compiled out (release) or out is NULL. */
JCE_API bool jce_alloc_track_snapshot(JceAllocTrack *out);

/* Print the snapshot (totals + the largest size buckets) to stderr.
 * No-op when tracking is compiled out. */
JCE_API void jce_alloc_track_dump(void);

JCE_EXTERN_C_END

#endif /* JCE_ALLOCATOR_H */
