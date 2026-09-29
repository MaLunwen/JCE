/*
 * jce_sr_portal.h  Occlusion portals as conservative CPU occluder volumes.
 *
 * JceOcclusionPortalComponent.open was authored, serialized, and read by
 * nothing: the component's own header called it RESERVED for "a future
 * portal/cell occlusion culler".  A portal/cell culler IS a subsystem, but the
 * thing a CLOSED portal does needs none of it -- a closed portal is a solid
 * box, and a solid box occludes what is behind it.  That is the same primitive
 * UE spells "occluder volume" and Godot spells OccluderInstance3D, and it is
 * what Unity's closed portal means in its baked PVS.
 *
 * This runs on the CPU, so unlike the GPU-query culler it also works where
 * hardware occlusion queries do not exist (GL ES 2.0, WebGL 1.0).
 *
 * Layer: Middleware/scene.  Internal to the scene renderer.
 */

#ifndef JCE_SR_PORTAL_H
#define JCE_SR_PORTAL_H

#include "jce_sr_internal.h"

/* The portal set is DATA, not renderer state: it is a camera and a handful of
 * boxes.  Keeping the two apart is what lets the occlusion proof be tested
 * without a GPU -- jce_scene_renderer_create needs a live JceRenderer, and a
 * geometry predicate that can only run inside a renderer cannot be checked. */
struct SrPortalSet *sr_portal_set_create(void);
void                sr_portal_set_destroy(struct SrPortalSet *set);

/* Rebuild the set from the scene's CLOSED portals as seen from `camera`. */
void sr_portal_set_build(struct SrPortalSet *set, JceScene *scene,
                         const JceCamera *camera, float aspect,
                         bool homogeneous_depth);

/* True when the world AABB is provably hidden behind a closed portal.
 * CONSERVATIVE in the only direction that matters: it returns false whenever
 * it cannot prove occlusion, so it can drop draws but never geometry. */
bool sr_portal_set_occludes(const struct SrPortalSet *set,
                            jce_vec3 wmin, jce_vec3 wmax);

/* Per-frame / teardown wrappers over the renderer's own set. */
void sr_portal_begin(JceSceneRenderer *sr, JceScene *scene,
                     const JceCamera *camera, float aspect);
bool sr_portal_occludes(const JceSceneRenderer *sr,
                        jce_vec3 wmin, jce_vec3 wmax);
void sr_portal_free(JceSceneRenderer *sr);

#endif /* JCE_SR_PORTAL_H */
