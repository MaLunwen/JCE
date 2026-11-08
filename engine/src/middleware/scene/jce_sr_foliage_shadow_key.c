/* Foliage identity is assembled from current preparation, never stale slots. */
#include "jce_sr_foliage_shadow_key.h"
#include <jce/os/core/jce_hash.h>

bool sr_foliage_shadow_key_walk(uint32_t count,
    const JceSrFoliageShadowProvider *provider, uint64_t *key)
{
    uint64_t hash = JCE_FNV1A64_INIT;
    bool complete = true;

    if (!provider || !provider->read || !provider->prepare || !key)
        return false;

    for (uint32_t i = 0u; i < count; ++i) {
        JceSrFoliageShadowItem item = {0};
        if (!provider->read(i, &item, provider->user) || !item.visible ||
            !item.cast_shadow)
            continue;

        item.ready = provider->prepare(&item, provider->user);
        if (!item.ready)
            complete = false;
        hash = jce_hash_det64_u64(hash, item.entity);
        hash = jce_hash_det64_u32(hash, item.placement_hash);
        hash = jce_hash_det64_u32(hash, item.count);
        hash = jce_hash_det64_u32(hash, item.geometry_hash);
        hash = jce_hash_det64_u32(hash, item.ready ? 1u : 0u);
    }

    *key = hash;
    return complete;
}
