/*
 * jce_lighting.h  Basic lighting (directional light).
 *
 * Sets uniform values consumed by the mesh fragment shader.
 * Supports one directional light. Point lights planned for Phase 4.
 */

#ifndef JCE_LIGHTING_H
#define JCE_LIGHTING_H

#include <jce/core/jce_math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceRenderer JceRenderer;

/* Directional light descriptor. */
typedef struct {
    jce_vec3 direction;    /* direction *toward* the light source */
    jce_vec3 color;        /* RGB, typically (1,1,1) */
    float    ambient;      /* ambient intensity, 0..1 */
} JceDirLight;

/* Create a default directional light (white, from upper-right-front). */
JceDirLight jce_dir_light_default(void);

/* Apply the directional light uniforms for the next mesh draw calls. */
void jce_lighting_apply(const JceRenderer *r, const JceDirLight *light);

#ifdef __cplusplus
}
#endif

#endif /* JCE_LIGHTING_H */
