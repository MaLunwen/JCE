/*
 * jce_editor_alloc.h  Editor memory allocation convenience macros.
 *
 * Wraps the engine's public allocator API so editor code uses the
 * same tracked, mimalloc-backed allocator as the engine core.
 *
 * Usage: replace raw malloc/calloc/free with ED_MALLOC/ED_CALLOC/ED_FREE.
 *
 * Performance note: the underlying allocator vtable (4 function pointers)
 * is cached in a function-local static, so each ED_MALLOC / ED_FREE call
 * resolves the slot once per process — not per call site — and keeps the
 * macro hot-path branch-free.
 */

#ifndef JCE_EDITOR_ALLOC_H
#define JCE_EDITOR_ALLOC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/os/core/jce_allocator.h>

#ifdef __cplusplus
}
#endif

#include <string.h>  /* memset */

/* Cached default allocator. The first invocation copies the vtable into
   the function-local static; subsequent calls return a pointer to it. */
static inline const jce_allocator_t *ed_alloc_default_ptr(void)
{
    static jce_allocator_t s_alloc;
    static int             s_inited = 0;
    if (!s_inited) {
        s_alloc  = jce_allocator_default();
        s_inited = 1;
    }
    return &s_alloc;
}

/* Backward-compatible accessor: returns the cached vtable by value. */
static inline jce_allocator_t ed_alloc_default(void)
{
    return *ed_alloc_default_ptr();
}

#define ED_MALLOC(size)        (ed_alloc_default_ptr()->alloc((size), ed_alloc_default_ptr()->ctx))
#define ED_FREE(ptr)           (ed_alloc_default_ptr()->free((ptr),  ed_alloc_default_ptr()->ctx))
#define ED_REALLOC(ptr, size)  (ed_alloc_default_ptr()->realloc((ptr), (size), ed_alloc_default_ptr()->ctx))

static inline void *ed_calloc(size_t count, size_t size)
{
    size_t total = count * size;
    const jce_allocator_t *a = ed_alloc_default_ptr();
    void *p = a->alloc(total, a->ctx);
    if (p) memset(p, 0, total);
    return p;
}

#define ED_CALLOC(count, size)  ed_calloc((count), (size))

#endif /* JCE_EDITOR_ALLOC_H */
