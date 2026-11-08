/* Private pre-shadow foliage preparation and current-content identity. */
#ifndef JCE_SR_FOLIAGE_SHADOW_KEY_H
#define JCE_SR_FOLIAGE_SHADOW_KEY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t entity;
    uint32_t placement_hash;
    uint32_t count;
    uint32_t geometry_hash;
    bool visible;
    bool cast_shadow;
    bool ready;
} JceSrFoliageShadowItem;

typedef struct {
    /* False means the source entity/component is absent. */
    bool (*read)(uint32_t index, JceSrFoliageShadowItem *item, void *user);
    /* Updates current placement/count. True means CPU preparation/upload
     * acceptance for this request, not completion of GPU execution. */
    bool (*prepare)(JceSrFoliageShadowItem *item, void *user);
    void *user;
} JceSrFoliageShadowProvider;

/* Visits the complete authored inventory in provider order. Only visible
 * shadow casters are prepared and folded. Publishes key after all preparation,
 * including failed items' readiness, and returns whether every caster is ready.
 * Provider/callbacks and key are required, including for an empty inventory. */
bool sr_foliage_shadow_key_walk(uint32_t count,
    const JceSrFoliageShadowProvider *provider, uint64_t *key);

#endif
