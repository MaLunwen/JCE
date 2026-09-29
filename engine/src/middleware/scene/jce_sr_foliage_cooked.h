#ifndef JCE_SR_FOLIAGE_COOKED_H
#define JCE_SR_FOLIAGE_COOKED_H

#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_scene_renderer.h>

uint32_t sr_foliage_param_hash(const JceVegetationScatterComponent *scatter,
                               const jce_vec3 *origin,
                               uint32_t surface_hash);
bool sr_foliage_try_load_cooked(JceSceneRenderer *sr,
                                const JceVegetationScatterComponent *scatter,
                                int slot);

#endif /* JCE_SR_FOLIAGE_COOKED_H */
