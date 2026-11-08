/* Private scene Forward+ storage ownership. No graphics backend types. */
#ifndef JCE_SR_FORWARDPLUS_STORAGE_H
#define JCE_SR_FORWARDPLUS_STORAGE_H

#include <jce/renderer/jce_forwardplus.h>
#include <stddef.h>

/* One complete owner serves sequential color views of a scene renderer. */
typedef struct {
    JceForwardPlus *module;
    JceLightProxy *proxies;
    JceForwardPlusLightParam *params;
    uint32_t capacity;
} JceSrForwardPlusStorage;

/* Immutable admission provider for actual-owner failure controls. This is
 * private engine maintenance machinery, never an SDK injection interface. */
typedef struct {
    JceForwardPlus *(*create)(uint32_t x, uint32_t y, uint32_t z,
        uint32_t per_cell, uint32_t lights, void *user);
    void (*destroy)(JceForwardPlus *module, void *user);
    void (*set_enabled)(JceForwardPlus *module, bool enabled, void *user);
    void *(*allocate)(size_t count, size_t size, void *user);
    void (*release)(void *allocation, void *user);
    void *user;
} JceSrForwardPlusProvider;

/* NULL provider selects the production JCE allocator/module. False means the
 * fixed-light fallback must be used for this view. OFF keeps a complete owner
 * disabled for reuse; failed first admission publishes nothing and retries on
 * the next enabled request. Call before collecting any point/spot lights.
 * Allocation acceptance is not evidence of GPU upload or physical residency. */
bool sr_forwardplus_storage_prepare(JceSrForwardPlusStorage *storage,
    bool requested, const JceSrForwardPlusProvider *provider);
void sr_forwardplus_storage_destroy(JceSrForwardPlusStorage *storage,
    const JceSrForwardPlusProvider *provider);

#endif
