#ifndef JCE_LIGHTING_SYSTEM_INTERNAL_H
#define JCE_LIGHTING_SYSTEM_INTERNAL_H

#include <jce/renderer/jce_lighting_system.h>

/* Directional-only shading shares the ordinary packed/layer-filtered values.
 * The caller owns its camera, geometry and other pass uniforms. */
void jce_light_env_apply_directional_for_layer(const JceLightEnv *env,
    const JceRenderer *renderer, uint32_t layer_index);

/* Shared camera handle, with the actual current draw's position and w=0. */
void jce_lighting_system_bind_camera(jce_vec3 position);

/* Device-owned shared light bindings; environments only borrow them. */
void jce_lighting_system_shutdown(void);

#endif
