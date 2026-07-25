/*
 * jce_memory.h  Engine-internal allocation macros (mimalloc-backed).
 *
 * IMPORTANT: This header MUST NOT include <mimalloc.h>. The actual
 * mimalloc dependency lives entirely inside jce_memory.c so that the
 * ~65 translation units which use JCE_MALLOC / JCE_NEW do NOT pull
 * <mimalloc.h> into their compilation. This keeps mimalloc a strictly
 * internal dependency; swapping it out only requires touching:
 *   - engine/src/os/core/jce_memory.c
 *   - engine/src/os/core/jce_allocator.c
 * No other engine source touches mi_* symbols.
 *
 * Layer: Foundation (Layer 1 — no engine dependencies beyond profiler).
 */

#ifndef JCE_MEMORY_H
#define JCE_MEMORY_H

#include <stddef.h>

/* -- Allocation-tracking gate -------------------------------------- */

/* Shared by the counter owner (jce_memory.c) and jce_allocator.c, which
 * only reports into it. Kept here rather than duplicated per file so the
 * two can never disagree about whether the tracker exists — a silent
 * disagreement would be an accounting hole, not a compile error. */
#ifndef NDEBUG
#  define JCE_ALLOC_TRACKING 1
#else
#  define JCE_ALLOC_TRACKING 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -- Internal extern entry points (defined in jce_memory.c) -------- */

void *jce__malloc_tracked(size_t size);
void *jce__calloc_tracked(size_t count, size_t size);
void *jce__realloc_tracked(void *ptr, size_t size);
void  jce__free_tracked(void *ptr);

/* -- Shared allocation accounting (defined in jce_memory.c) -------- */

/*
 * THE single accounting point for engine heap traffic. Both mimalloc
 * front-ends — the JCE_MALLOC macros below and the jce_allocator_t
 * default vtable (jce_allocator.c) — report every allocation and free
 * here, so jce_alloc_track_snapshot(), jce_alloc_frame_delta() and the
 * Tracy heap view describe the WHOLE engine heap instead of one
 * front-end each. Both account identically, so a block obtained from
 * one front-end and released through the other still balances.
 *
 * `requested` is the size asked for; the live-byte tracker uses the
 * allocator's usable size instead. NULL is a no-op in both hooks. Call
 * the free hook BEFORE releasing the block, while the pointer is valid.
 */
void jce__alloc_account_alloc(void *ptr, size_t requested);
void jce__alloc_account_free(void *ptr);

#ifdef __cplusplus
}
#endif

/* -- Core allocation macros (source-compatible with prior inline) -- */

#define JCE_MALLOC(size)         jce__malloc_tracked(size)
#define JCE_CALLOC(count, size)  jce__calloc_tracked((count), (size))
#define JCE_REALLOC(ptr, size)   jce__realloc_tracked((ptr), (size))
#define JCE_FREE(ptr)            jce__free_tracked(ptr)

/* Allocate and zero-initialize a single struct of type T. */
#define JCE_NEW(T)               ((T *)JCE_CALLOC(1, sizeof(T)))

/* Allocate an array of N elements of type T (zeroed). */
#define JCE_NEW_ARRAY(T, n)      ((T *)JCE_CALLOC((n), sizeof(T)))

#endif /* JCE_MEMORY_H */
