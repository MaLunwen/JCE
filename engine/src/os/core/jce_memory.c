/*
 * jce_memory.c  mimalloc-backed implementation of JCE_MALLOC / friends.
 *
 * One of only TWO files in the engine that may #include <mimalloc.h>
 * (the other is jce_allocator.c). Keeping the dependency funnelled
 * through these two files means swapping the allocator never ripples
 * through the rest of the codebase.
 *
 * Each call also notifies the profiler so Tracy can track heap usage.
 */

#include "jce_memory.h"

#include <jce/os/core/jce_profiler.h>

#include <mimalloc.h>

void *jce__malloc_tracked(size_t size)
{
    void *p = mi_malloc(size);
    if (p) JCE_PROFILE_ALLOC(p, size);
    return p;
}

void *jce__calloc_tracked(size_t count, size_t size)
{
    void *p = mi_calloc(count, size);
    if (p) JCE_PROFILE_ALLOC(p, count * size);
    return p;
}

void *jce__realloc_tracked(void *ptr, size_t size)
{
    if (ptr) JCE_PROFILE_FREE(ptr);
    void *p = mi_realloc(ptr, size);
    if (p) JCE_PROFILE_ALLOC(p, size);
    return p;
}

void jce__free_tracked(void *ptr)
{
    if (ptr) JCE_PROFILE_FREE(ptr);
    mi_free(ptr);
}
