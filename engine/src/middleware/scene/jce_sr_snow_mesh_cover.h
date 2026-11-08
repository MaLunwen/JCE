#ifndef JCE_SR_SNOW_MESH_COVER_H
#define JCE_SR_SNOW_MESH_COVER_H

#include <jce/renderer/jce_scene_renderer.h>
#include <jce/middleware/scene/jce_scene.h>

/* Three vec4s per triangle, using the terrain's existing exposure budget.
 * Returns zero for unsupported or incomplete geometry. */
uint16_t sr_snow_mesh_cover(JceSceneRenderer *sr, JceScene *scene, JceEntity entity,
    const jce_mat4 *world, float out[][3][4], uint16_t capacity);

#endif
