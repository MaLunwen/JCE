#ifndef JCE_SR_FCLUSTER_SUBMIT_INTERNAL_H
#define JCE_SR_FCLUSTER_SUBMIT_INTERNAL_H
#include "jce_sr_grass_submit_internal.h"
#include <stddef.h>
#include <string.h>

enum {
    JCE_FCLUSTER_COMPLETE = 0,
    JCE_FCLUSTER_CAPACITY = 1,
    JCE_FCLUSTER_ALLOCATION_NULL = 2,
    JCE_FCLUSTER_ALLOCATION_SHORT = 3,
    JCE_FCLUSTER_INVALID_INPUT = 4,
    JCE_FCLUSTER_GEOMETRY_UNAVAILABLE = 5
};
typedef struct JceFclusterAllocation {
    void *data;
    uint32_t count;
} JceFclusterAllocation;
typedef struct JceFclusterProvider {
    uint32_t (*available)(uint32_t, uint16_t, void *);
    JceFclusterAllocation (*allocate)(uint32_t, uint16_t, void *);
    void *user;
} JceFclusterProvider;

/* Reuses the grass receipt shape; this owner retains cluster's original
 * all-or-nothing transient admission. accepted here means CPU bytes prepared,
 * not a submitted draw. The caller publishes accepted only after its submit. */
static inline JceGrassColourReceipt sr_fcluster_prepare_instances(uint32_t count,
    uint16_t stride, const void *source, const JceFclusterProvider *provider)
{
    JceGrassColourReceipt result = {count, 0, count, JCE_FCLUSTER_COMPLETE};
    if (!count) return result;
    if (!stride || !source || !provider || !provider->available ||
        !provider->allocate || (size_t)count > SIZE_MAX / stride) {
        result.reason = JCE_FCLUSTER_INVALID_INPUT; return result;
    }
    if (provider->available(count, stride, provider->user) < count) {
        result.reason = JCE_FCLUSTER_CAPACITY; return result;
    }
    JceFclusterAllocation allocation = provider->allocate(count, stride, provider->user);
    if (!allocation.data) {
        result.reason = JCE_FCLUSTER_ALLOCATION_NULL; return result;
    }
    if (allocation.count < count) {
        result.reason = JCE_FCLUSTER_ALLOCATION_SHORT; return result;
    }
    memcpy(allocation.data, source, (size_t)count * stride);
    result.accepted = count;
    result.rejected = 0;
    return result;
}
#endif
