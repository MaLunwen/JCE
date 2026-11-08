#ifndef JCE_SR_RAIN_SURFACE_H
#define JCE_SR_RAIN_SURFACE_H

#include <jce/renderer/jce_scene_renderer.h>
#include "renderer/jce_gpu_particles_internal.h"

/* Borrow the current atlas of the nearshore region that best overlaps the
 * emitter. No new texture, upload or scene state is owned by the caller. */
bool sr_water_shore_gpu_rain_surface(JceSceneRenderer *sr, JceScene *scene,
    jce_vec3 origin, jce_vec3 spawn_box, JceParticleRainSurface *out);

#endif
