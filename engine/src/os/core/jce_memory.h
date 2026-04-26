/*
 * jce_memory.h  Engine-internal allocation macros (mimalloc-backed).
 *
 * IMPORTANT: This header MUST NOT include <mimalloc.h>. The actual
 * mimalloc dependency lives entirely inside jce_memory.c so that the
 * ~65 translation units which use JCE_MALLOC / JCE_NEW do NOT pull
 * <mimalloc.h> into their compilation. This keeps mimalloc a strictly
 * internal dependency; swapping it out only requires touching:
 *   - engine/src/core/jce_memory.c
 *   - engine/src/core/jce_allocator.c
 * No other engine source touches mi_* symbols.
 *
 * Layer: Foundation (Layer 1 — no engine dependencies beyond profiler).
 */

#ifndef JCE_MEMORY_H
#define JCE_MEMORY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Internal extern entry points (defined in jce_memory.c) -------- */

void *jce__malloc_tracked(size_t size);
void *jce__calloc_tracked(size_t count, size_t size);
void *jce__realloc_tracked(void *ptr, size_t size);
void  jce__free_tracked(void *ptr);

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
