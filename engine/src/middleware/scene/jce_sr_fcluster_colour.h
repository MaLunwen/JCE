/* Private whole-cluster colour emission owner. */
#ifndef JCE_SR_FCLUSTER_COLOUR_H
#define JCE_SR_FCLUSTER_COLOUR_H
#include "jce_sr_internal.h"
#include "jce_sr_fcluster_submit_internal.h"
JceGrassColourReceipt sr_fcluster_submit_colour(JceSceneRenderer *sr,
    JceScene *scene, EntityList *list, const JceFoliageClusterComponent *field,
    const JceCamera *camera, uint16_t view, const float *instances, uint32_t count);
JceGrassColourReceipt sr_fcluster_submit_colour_mesh(JceSceneRenderer *sr,
    JceScene *scene, EntityList *list, const JceFoliageClusterComponent *field,
    const JceCamera *camera, uint16_t view, const JceMesh *mesh, uint32_t count);
#endif
