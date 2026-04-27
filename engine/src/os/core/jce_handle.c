/*
 * jce_handle.c  Generational handle pool implementation.
 *
 * Uses a flat array of items + metadata, with a free-list stack
 * for O(1) allocation and deallocation.
 *
 * ID layout: [generation:12 | index:20]
 */

#include <jce/os/core/jce_handle.h>

#include <string.h>

/* Maximum index value (2^20 - 1, minus 1 for null sentinel). */
#define MAX_INDEX  0xFFFFFu
#define GEN_MASK   0xFFFu
#define GEN_BITS   12

typedef struct {
    uint16_t generation;   /* 12-bit generation, stored in 16 for alignment */
    bool     alive;
} slot_meta_t;

struct jce_handle_pool {
    jce_allocator_t alloc;
    uint8_t        *items;       /* flat item array: items[i * item_size] */
    slot_meta_t    *meta;        /* generation + alive flag per slot */
    uint32_t       *free_stack;  /* indices of free slots */
    uint32_t        item_size;
    uint32_t        capacity;
    uint32_t        count;       /* live items */
    uint32_t        free_top;    /* stack pointer into free_stack */
};

jce_handle_pool_t *jce_handle_pool_create(jce_allocator_t alloc,
                                          uint32_t item_size,
                                          uint32_t capacity)
{
    if (item_size == 0 || capacity == 0) return NULL;
    if (capacity > MAX_INDEX) capacity = MAX_INDEX;

    jce_handle_pool_t *p = (jce_handle_pool_t *)alloc.alloc(
        sizeof(jce_handle_pool_t), alloc.ctx);
    if (!p) return NULL;

    p->alloc     = alloc;
    p->item_size = item_size;
    p->capacity  = capacity;
    p->count     = 0;

    p->items = (uint8_t *)alloc.alloc((size_t)item_size * capacity, alloc.ctx);
    p->meta  = (slot_meta_t *)alloc.alloc(sizeof(slot_meta_t) * capacity, alloc.ctx);
    p->free_stack = (uint32_t *)alloc.alloc(sizeof(uint32_t) * capacity, alloc.ctx);

    if (!p->items || !p->meta || !p->free_stack) {
        if (p->items)      alloc.free(p->items, alloc.ctx);
        if (p->meta)       alloc.free(p->meta, alloc.ctx);
        if (p->free_stack) alloc.free(p->free_stack, alloc.ctx);
        alloc.free(p, alloc.ctx);
        return NULL;
    }

    /* Initialize metadata: all dead, generation 1 (so first handle is 1|index,
       never 0 which is JCE_HANDLE_NULL). */
    for (uint32_t i = 0; i < capacity; i++) {
        p->meta[i].generation = 1;
        p->meta[i].alive = false;
    }

    /* Build free stack (LIFO: top = index 0 for sequential first use). */
    for (uint32_t i = 0; i < capacity; i++)
        p->free_stack[i] = capacity - 1 - i;
    p->free_top = capacity;

    return p;
}

void jce_handle_pool_destroy(jce_handle_pool_t *pool)
{
    if (!pool) return;
    jce_allocator_t a = pool->alloc;
    a.free(pool->items, a.ctx);
    a.free(pool->meta, a.ctx);
    a.free(pool->free_stack, a.ctx);
    a.free(pool, a.ctx);
}

jce_handle_t jce_handle_pool_add(jce_handle_pool_t *pool, const void *item)
{
    if (!pool || pool->free_top == 0) return JCE_HANDLE_NULL;

    uint32_t idx = pool->free_stack[--pool->free_top];
    uint16_t gen = pool->meta[idx].generation;

    /* Copy item data. */
    memcpy(pool->items + (size_t)idx * pool->item_size,
           item, pool->item_size);

    pool->meta[idx].alive = true;
    pool->count++;

    return JCE_HANDLE_MAKE(gen, idx);
}

void jce_handle_pool_remove(jce_handle_pool_t *pool, jce_handle_t h)
{
    if (!pool || !JCE_HANDLE_VALID(h)) return;

    uint32_t idx = JCE_HANDLE_INDEX(h);
    uint32_t gen = JCE_HANDLE_GEN(h);

    if (idx >= pool->capacity) return;
    if (!pool->meta[idx].alive) return;
    if (pool->meta[idx].generation != (uint16_t)gen) return;

    pool->meta[idx].alive = false;
    pool->meta[idx].generation = (uint16_t)((gen + 1) & GEN_MASK);
    /* Ensure generation is never 0 (which would make handle == index,
       conflicting with JCE_HANDLE_NULL for index 0). */
    if (pool->meta[idx].generation == 0)
        pool->meta[idx].generation = 1;

    pool->free_stack[pool->free_top++] = idx;
    pool->count--;
}

void *jce_handle_pool_get(const jce_handle_pool_t *pool, jce_handle_t h)
{
    if (!pool || !JCE_HANDLE_VALID(h)) return NULL;

    uint32_t idx = JCE_HANDLE_INDEX(h);
    uint32_t gen = JCE_HANDLE_GEN(h);

    if (idx >= pool->capacity) return NULL;
    if (!pool->meta[idx].alive) return NULL;
    if (pool->meta[idx].generation != (uint16_t)gen) return NULL;

    return pool->items + (size_t)idx * pool->item_size;
}

bool jce_handle_pool_alive(const jce_handle_pool_t *pool, jce_handle_t h)
{
    return jce_handle_pool_get(pool, h) != NULL;
}

uint32_t jce_handle_pool_count(const jce_handle_pool_t *pool)
{
    return pool ? pool->count : 0;
}
