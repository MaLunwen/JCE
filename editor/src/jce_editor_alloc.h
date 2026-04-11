/*
 * jce_editor_alloc.h  Editor memory allocation convenience macros.
 *
 * Wraps the engine's public allocator API so editor code uses the
 * same tracked, mimalloc-backed allocator as the engine core.
 *
 * Usage: replace raw malloc/calloc/free with ED_MALLOC/ED_CALLOC/ED_FREE.
 */

#ifndef JCE_EDITOR_ALLOC_H
#define JCE_EDITOR_ALLOC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/core/jce_allocator.h>

#ifdef __cplusplus
}
#endif

#include <string.h>  /* memset */

/* Convenience: get the default engine allocator once. */
static inline jce_allocator_t ed_alloc_default(void)
{
    return jce_allocator_default();
}

#define ED_MALLOC(size)        (ed_alloc_default().alloc((size), ed_alloc_default().ctx))
#define ED_FREE(ptr)           do { jce_allocator_t _a = ed_alloc_default(); _a.free((ptr), _a.ctx); } while (0)
#define ED_REALLOC(ptr, size)  (ed_alloc_default().realloc((ptr), (size), ed_alloc_default().ctx))

static inline void *ed_calloc(size_t count, size_t size)
{
    size_t total = count * size;
    jce_allocator_t a = jce_allocator_default();
    void *p = a.alloc(total, a.ctx);
    if (p) memset(p, 0, total);
    return p;
}

#define ED_CALLOC(count, size)  ed_calloc((count), (size))

#endif /* JCE_EDITOR_ALLOC_H */
