/*
 * jce_handle.h  Generational handle pool.
 *
 * Provides type-erased object storage with generational handles
 * for safe indirect references.  Handles detect use-after-free
 * via generation counters, support hot-reload, and are trivially
 * serializable as a single uint32_t.
 *
 * ID layout: [generation:12 | index:20]
 *   → Up to ~1 million concurrent objects per pool
 *   → 4096 reuse cycles before generation wraps
 *
 * Layer: Foundation (Layer 1 — no engine dependencies).
 */

#ifndef JCE_HANDLE_H
#define JCE_HANDLE_H


#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Handle type                                                         */
/* ================================================================== */

typedef struct { uint32_t id; } jce_handle_t;

#define JCE_HANDLE_NULL        ((jce_handle_t){0})
#define JCE_HANDLE_INDEX(h)    ((h).id & 0xFFFFFu)
#define JCE_HANDLE_GEN(h)      ((h).id >> 20)
#define JCE_HANDLE_VALID(h)    ((h).id != 0)
#define JCE_HANDLE_MAKE(g, i)  ((jce_handle_t){ ((uint32_t)(g) << 20) | ((i) & 0xFFFFFu) })

/* ================================================================== */
/* Handle pool                                                         */
/* ================================================================== */

typedef struct jce_handle_pool jce_handle_pool_t;

/* Create a pool that stores items of 'item_size' bytes.
   'capacity' is the maximum number of live items (up to 2^20 - 1). */
jce_handle_pool_t *jce_handle_pool_create(jce_allocator_t alloc,
                                          uint32_t item_size,
                                          uint32_t capacity);

/* Destroy the pool and free all storage. */
void jce_handle_pool_destroy(jce_handle_pool_t *pool);

/* Add an item (copies 'item_size' bytes from 'item').
   Returns JCE_HANDLE_NULL if pool is full. */
jce_handle_t jce_handle_pool_add(jce_handle_pool_t *pool, const void *item);

/* Remove an item by handle (increments generation for that slot).
   No-op if handle is stale. */
void jce_handle_pool_remove(jce_handle_pool_t *pool, jce_handle_t h);

/* Get a pointer to the item.  Returns NULL if handle is stale or invalid. */
void *jce_handle_pool_get(const jce_handle_pool_t *pool, jce_handle_t h);

/* Check whether a handle still refers to a live item. */
bool jce_handle_pool_alive(const jce_handle_pool_t *pool, jce_handle_t h);

/* Number of currently live items. */
uint32_t jce_handle_pool_count(const jce_handle_pool_t *pool);

JCE_EXTERN_C_END

#endif /* JCE_HANDLE_H */
