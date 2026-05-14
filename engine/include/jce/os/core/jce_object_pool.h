/*
 * jce_object_pool.h  Generic object pool (Unity ObjectPool equivalent).
 *
 * Pre-allocates a fixed-size pool of slots of a user-defined size,
 * exposes acquire/release semantics, and reuses slots without
 * malloc/free during steady-state.  Typical use: bullet projectiles,
 * particle GameObjects, audio voices, AI agents.
 *
 * The pool stores raw bytes; caller is responsible for any
 * construction / destruction.  Acquire returns a pointer that's
 * stable for the slot's lifetime; release marks the slot free for
 * the next acquire to reuse.
 *
 * Mirrors Unity's UnityEngine.Pool.ObjectPool<T> at the data layer:
 *   capacity   = max simultaneously-active items
 *   actionOnGet/Release = caller-supplied callbacks
 *
 * Layer: os/core (Layer 1) — public.
 */

#ifndef JCE_OBJECT_POOL_H
#define JCE_OBJECT_POOL_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

typedef struct JceObjectPool JceObjectPool;

/* Optional callbacks fired on get / release.  Pass NULL for no-op.
 * `slot` is the same pointer Acquire returns. */
typedef void (*JceObjectPoolFn)(void *slot, void *user_data);

typedef struct {
    size_t           item_size;       /* bytes per slot */
    uint32_t         capacity;        /* max active items */
    JceObjectPoolFn  on_acquire;      /* called when a slot leaves the pool */
    JceObjectPoolFn  on_release;      /* called when a slot returns */
    void            *user_data;
    /* If true, pool zero-initialises each slot at create time AND on
     * release (matches Unity's collectionCheck off / on_release zeroing). */
    bool             zero_on_release;
} JceObjectPoolDesc;

JCE_API JceObjectPool *jce_object_pool_create(const JceObjectPoolDesc *desc);
JCE_API void           jce_object_pool_destroy(JceObjectPool *pool);

/* Acquire a slot.  Returns NULL when the pool is at capacity. */
JCE_API void *jce_object_pool_acquire(JceObjectPool *pool);

/* Return a slot to the pool.  Caller-supplied destructor work
 * happens through on_release. */
JCE_API bool  jce_object_pool_release(JceObjectPool *pool, void *slot);

JCE_API uint32_t jce_object_pool_active_count(const JceObjectPool *pool);
JCE_API uint32_t jce_object_pool_capacity    (const JceObjectPool *pool);

/* Forcibly release every active slot (calls on_release for each).
 * Useful at level transition. */
JCE_API void jce_object_pool_release_all(JceObjectPool *pool);

JCE_EXTERN_C_END

#endif /* JCE_OBJECT_POOL_H */
