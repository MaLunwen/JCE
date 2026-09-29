/*
 * jce_sr_veg_material.h  VegetationScatter albedo / tint override.
 *
 * Its own header rather than jce_sr_internal.h: two callers, and that header
 * is size-frozen -- a shared header is the wrong place to spend that budget
 * on a two-caller prototype.
 */
#ifndef JCE_SR_VEG_MATERIAL_H
#define JCE_SR_VEG_MATERIAL_H

#include "jce_sr_internal.h"

/* apply() writes the override into an EXISTING material -- what the
 * enqueueing primitive path needs, since an armed override would be cleared
 * before the queue flushes.  arm() builds one and installs it as the
 * renderer-wide override, for the paths that submit immediately. */
bool sr_veg_apply_material(JceSceneRenderer *sr,
                           const JceVegetationScatterComponent *vs,
                           JcePbrMaterial *m);
bool sr_veg_arm_material(JceSceneRenderer *sr,
                         const JceVegetationScatterComponent *vs,
                         JcePbrMaterial *out);

#endif /* JCE_SR_VEG_MATERIAL_H */
