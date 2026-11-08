/* Complete authored grass traversal, independent of the resident slot count. */
#ifndef JCE_SR_GRASS_SHADOW_CONTENT_H
#define JCE_SR_GRASS_SHADOW_CONTENT_H
#include "jce_sr_foliage_shadow_key.h"
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_math.h>

/* These are the production identities used by both passes. Mask bytes are
 * refreshed by supported entity-cache reset / active VFS context change. */
bool sr_grass_inputs_finite(const JceGrassFieldComponent *field, const jce_mat4 *world);
uint32_t sr_grass_placement_input_key(const JceGrassFieldComponent *field,
    const jce_mat4 *world, uint32_t surface_hash, float density_scale);
uint32_t sr_grass_complete_placement_key(uint32_t input_key, uint32_t mask_content_hash);
uint32_t sr_grass_shadow_geometry_key(const JceGrassFieldComponent *field,
    float blade_height, float blade_width, int cards, jce_vec3 camera,
    float environment_wind, float time);

typedef JceSrFoliageShadowItem JceSrGrassShadowItem;
typedef enum {
    JCE_GRASS_SHADOW_ABSENT = 0,
    JCE_GRASS_SHADOW_CASTER,
    JCE_GRASS_SHADOW_READ_FAILED
} JceSrGrassShadowRead;
typedef struct {
    JceSrGrassShadowRead (*read)(uint32_t index, JceSrGrassShadowItem *item, void *user);
    bool (*prepare)(JceSrGrassShadowItem *item, void *user);
    /* NULL performs preparation/key admission only. A non-NULL callback is
     * invoked immediately after each successful preparation, before eviction. */
    bool (*submit)(const JceSrGrassShadowItem *item, void *user);
    void *user;
} JceSrGrassShadowProvider;

/* Empty inventory succeeds. Read/prepare/submit failure keeps traversing the
 * remaining records but returns false and folds a failure marker into key.
 * Preparation/submit acceptance is CPU ownership evidence, not GPU completion. */
bool sr_grass_shadow_content_walk(uint32_t count,
    const JceSrGrassShadowProvider *provider, uint64_t *key);
#endif
