/*
 * jce_object_pool.c  Generic object pool.
 *
 * Layout: a contiguous byte buffer of `capacity * item_size` plus a
 * parallel `alive[capacity]` flag array.  Acquire scans for the
 * first free slot (linear; capacity is typically small).  Release
 * locates the slot index by pointer arithmetic.
 */

#include <jce/os/core/jce_object_pool.h>

#include "os/core/jce_memory.h"

#include <string.h>

struct JceObjectPool {
    JceObjectPoolDesc desc;
    uint8_t          *data;          /* capacity * item_size bytes */
    bool             *alive;
    uint32_t          active_count;
};

JceObjectPool *jce_object_pool_create(const JceObjectPoolDesc *d)
{
    if (!d || d->item_size == 0 || d->capacity == 0) return NULL;
    JceObjectPool *p = (JceObjectPool *)JCE_CALLOC(1, sizeof(*p));
    if (!p) return NULL;
    p->desc = *d;
    p->data = (uint8_t *)JCE_CALLOC(d->capacity, d->item_size);
    p->alive = (bool *)JCE_CALLOC(d->capacity, sizeof(bool));
    if (!p->data || !p->alive) {
        JCE_FREE(p->data);
        JCE_FREE(p->alive);
        JCE_FREE(p);
        return NULL;
    }
    return p;
}

void jce_object_pool_destroy(JceObjectPool *p)
{
    if (!p) return;
    /* Release any still-active slots so caller gets the on_release
     * callback (resource cleanup). */
    if (p->desc.on_release) {
        for (uint32_t i = 0; i < p->desc.capacity; ++i) {
            if (p->alive[i])
                p->desc.on_release(p->data + (size_t)i * p->desc.item_size,
                                    p->desc.user_data);
        }
    }
    JCE_FREE(p->data);
    JCE_FREE(p->alive);
    JCE_FREE(p);
}

void *jce_object_pool_acquire(JceObjectPool *p)
{
    if (!p) return NULL;
    for (uint32_t i = 0; i < p->desc.capacity; ++i) {
        if (!p->alive[i]) {
            p->alive[i] = true;
            p->active_count++;
            void *slot = p->data + (size_t)i * p->desc.item_size;
            if (p->desc.on_acquire)
                p->desc.on_acquire(slot, p->desc.user_data);
            return slot;
        }
    }
    return NULL;
}

bool jce_object_pool_release(JceObjectPool *p, void *slot)
{
    if (!p || !slot) return false;
    /* Slot index from pointer offset. */
    size_t off = (uint8_t *)slot - p->data;
    if (off >= (size_t)p->desc.capacity * p->desc.item_size) return false;
    if (off % p->desc.item_size != 0) return false;
    uint32_t idx = (uint32_t)(off / p->desc.item_size);
    if (!p->alive[idx]) return false;

    if (p->desc.on_release) p->desc.on_release(slot, p->desc.user_data);
    if (p->desc.zero_on_release) memset(slot, 0, p->desc.item_size);
    p->alive[idx] = false;
    p->active_count--;
    return true;
}

uint32_t jce_object_pool_active_count(const JceObjectPool *p)
{ return p ? p->active_count : 0u; }
uint32_t jce_object_pool_capacity(const JceObjectPool *p)
{ return p ? p->desc.capacity : 0u; }

void jce_object_pool_release_all(JceObjectPool *p)
{
    if (!p) return;
    for (uint32_t i = 0; i < p->desc.capacity; ++i) {
        if (!p->alive[i]) continue;
        void *slot = p->data + (size_t)i * p->desc.item_size;
        if (p->desc.on_release) p->desc.on_release(slot, p->desc.user_data);
        if (p->desc.zero_on_release) memset(slot, 0, p->desc.item_size);
        p->alive[i] = false;
    }
    p->active_count = 0;
}
