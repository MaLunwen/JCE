/* Private, pure identity of authored foliage placement and geometry. */
#ifndef JCE_SR_FOLIAGE_IDENTITY_H
#define JCE_SR_FOLIAGE_IDENTITY_H
#include <jce/middleware/scene/jce_scene.h>
uint32_t sr_foliage_param_hash(const JceVegetationScatterComponent *scatter,
    const jce_vec3 *origin, uint32_t surface_hash);
uint32_t sr_foliage_geometry_hash(const JceVegetationScatterComponent *scatter);
#endif
