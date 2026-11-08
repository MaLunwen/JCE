/* Lazy, transactional ownership of scene Forward+ CPU storage. */
#include "jce_sr_forwardplus_storage.h"
#include "os/core/jce_memory.h"

static JceForwardPlus *sr_fp_create(uint32_t x, uint32_t y, uint32_t z,
    uint32_t per_cell, uint32_t lights, void *user)
{
    (void)user;
    return jce_forwardplus_create(x, y, z, per_cell, lights);
}

static void sr_fp_destroy(JceForwardPlus *module, void *user)
{
    (void)user;
    jce_forwardplus_destroy(module);
}

static void sr_fp_set_enabled(JceForwardPlus *module, bool enabled, void *user)
{
    (void)user;
    jce_forwardplus_set_enabled(module, enabled);
}

static void *sr_fp_allocate(size_t count, size_t size, void *user)
{
    (void)user;
    return JCE_CALLOC(count, size);
}

static void sr_fp_release(void *allocation, void *user)
{
    (void)user;
    JCE_FREE(allocation);
}

static const JceSrForwardPlusProvider sr_fp_provider = {
    sr_fp_create, sr_fp_destroy, sr_fp_set_enabled,
    sr_fp_allocate, sr_fp_release, NULL
};

void sr_forwardplus_storage_destroy(JceSrForwardPlusStorage *storage,
    const JceSrForwardPlusProvider *provider)
{
    if (!storage) return;
    if (!provider) provider = &sr_fp_provider;
    if (storage->module) provider->destroy(storage->module, provider->user);
    if (storage->proxies) provider->release(storage->proxies, provider->user);
    if (storage->params) provider->release(storage->params, provider->user);
    *storage = (JceSrForwardPlusStorage){0};
}

bool sr_forwardplus_storage_prepare(JceSrForwardPlusStorage *storage,
    bool requested, const JceSrForwardPlusProvider *provider)
{
    if (!storage) return false;
    if (!provider) provider = &sr_fp_provider;
    if (!requested) {
        if (storage->module)
            provider->set_enabled(storage->module, false, provider->user);
        return false;
    }

    if (!storage->module) {
        /* Allocate the small pair first so failure never leaves one array
         * available to collection while the parallel shading array is NULL. */
        JceSrForwardPlusStorage candidate = {0};
        candidate.capacity = 256u;
        candidate.proxies = provider->allocate(candidate.capacity,
            sizeof(*candidate.proxies), provider->user);
        if (!candidate.proxies) return false;
        candidate.params = provider->allocate(candidate.capacity,
            sizeof(*candidate.params), provider->user);
        if (candidate.params)
            candidate.module = provider->create(24u, 16u, 24u, 64u,
                candidate.capacity, provider->user);
        if (!candidate.params || !candidate.module) {
            sr_forwardplus_storage_destroy(&candidate, provider);
            return false;
        }
        *storage = candidate;
    }

    provider->set_enabled(storage->module, true, provider->user);
    return true;
}
