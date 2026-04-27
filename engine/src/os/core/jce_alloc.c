/*
 * jce_alloc.c  Public allocator wrappers around the internal tracker.
 */

#include <jce/os/core/jce_alloc.h>

#include "jce_memory.h"

void *jce_malloc(size_t size)
{
    return JCE_MALLOC(size);
}

void *jce_realloc(void *ptr, size_t new_size)
{
    return JCE_REALLOC(ptr, new_size);
}

void jce_free(void *ptr)
{
    if (ptr) JCE_FREE(ptr);
}
