/*
 * jce_memory.h  Unified memory allocation macros.
 *
 * All engine allocations go through these macros, which delegate
 * to mimalloc (mi_malloc / mi_calloc / mi_realloc / mi_free) for
 * high-performance, thread-safe allocation across platforms.
 *
 * Layer: Foundation (Layer 1 — no engine dependencies).
 */

#ifndef JCE_MEMORY_H
#define JCE_MEMORY_H

#include <mimalloc.h>
#include <stddef.h>
#include <jce/core/jce_profiler.h>

/* -- Core allocation macros ---------------------------------------- */

static inline void *jce__malloc_tracked(size_t size) {
    void *p = mi_malloc(size);
    if (p) JCE_PROFILE_ALLOC(p, size);
    return p;
}
static inline void *jce__calloc_tracked(size_t count, size_t size) {
    void *p = mi_calloc(count, size);
    if (p) JCE_PROFILE_ALLOC(p, count * size);
    return p;
}
static inline void *jce__realloc_tracked(void *ptr, size_t size) {
    if (ptr) JCE_PROFILE_FREE(ptr);
    void *p = mi_realloc(ptr, size);
    if (p) JCE_PROFILE_ALLOC(p, size);
    return p;
}
static inline void jce__free_tracked(void *ptr) {
    if (ptr) JCE_PROFILE_FREE(ptr);
    mi_free(ptr);
}

#define JCE_MALLOC(size)         jce__malloc_tracked(size)
#define JCE_CALLOC(count, size)  jce__calloc_tracked((count), (size))
#define JCE_REALLOC(ptr, size)   jce__realloc_tracked((ptr), (size))
#define JCE_FREE(ptr)            jce__free_tracked(ptr)

/* Allocate and zero-initialize a single struct of type T. */
#define JCE_NEW(T)  ((T *)JCE_CALLOC(1, sizeof(T)))

/* Allocate an array of N elements of type T (zeroed). */
#define JCE_NEW_ARRAY(T, n)  ((T *)JCE_CALLOC((n), sizeof(T)))

#endif /* JCE_MEMORY_H */
