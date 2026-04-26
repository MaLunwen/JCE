/*
 * jce_lighting_system.h  Multi-light environment for PBR rendering.
 *
 * Supports multiple directional, point, and spot lights.
 * Packs light data into uniform arrays consumed by fs_pbr.sc.
 *
 * The legacy single-light API (jce_lighting.h) remains for the
 * existing fs_mesh.sc Lambert shader.
 *
 * Layer: Graphics (Layer 3).
 */

#ifndef JCE_LIGHTING_SYSTEM_H
#define JCE_LIGHTING_SYSTEM_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <stdint.h>
#include <stdbool.h>

JCE_EXTERN_C_BEGIN

typedef struct JceRenderer JceRenderer;
typedef struct JceLightEnv JceLightEnv;

/* ================================================================== */
/* Limits                                                              */
/* ================================================================== */

#define JCE_MAX_DIR_LIGHTS   2
#define JCE_MAX_POINT_LIGHTS 8
#define JCE_MAX_SPOT_LIGHTS  4

/* ================================================================== */
/* Light descriptors                                                   */
/* ================================================================== */

typedef struct {
    jce_vec3 direction;       /* normalized, toward the light source */
    jce_vec3 color;           /* linear RGB */
    float    intensity;       /* multiplier, default 1.0 */
    bool     casts_shadow;    /* enable shadow map for this light */
} JceDirLightDesc;

typedef struct {
    jce_vec3 position;
    jce_vec3 color;           /* linear RGB */
    float    intensity;
    float    radius;          /* attenuation cutoff distance */
} JcePointLightDesc;

typedef struct {
    jce_vec3 position;
    jce_vec3 direction;       /* normalized, direction the spot points */
    jce_vec3 color;           /* linear RGB */
    float    intensity;
    float    inner_cone_cos;  /* cos(inner half-angle), full intensity inside */
    float    outer_cone_cos;  /* cos(outer half-angle), zero intensity outside */
    float    radius;          /* attenuation cutoff */
} JceSpotLightDesc;

/* ================================================================== */
/* Light environment API                                               */
/* ================================================================== */

/* Create an empty light environment. */
JceLightEnv *jce_light_env_create(void);

/* Destroy the light environment. */
void jce_light_env_destroy(JceLightEnv *env);

/* Set ambient light color and intensity. */
void jce_light_env_set_ambient(JceLightEnv *env, jce_vec3 color, float intensity);

/* Add lights. Returns the light index, or -1 if at capacity. */
int jce_light_env_add_dir_light(JceLightEnv *env, const JceDirLightDesc *light);
int jce_light_env_add_point_light(JceLightEnv *env, const JcePointLightDesc *light);
int jce_light_env_add_spot_light(JceLightEnv *env, const JceSpotLightDesc *light);

/* Clear all lights (ambient retained). */
void jce_light_env_clear(JceLightEnv *env);

/* Get current light counts. */
uint32_t jce_light_env_dir_count(const JceLightEnv *env);
uint32_t jce_light_env_point_count(const JceLightEnv *env);
uint32_t jce_light_env_spot_count(const JceLightEnv *env);

/* Set the camera world position (needed for PBR specular). */
void jce_light_env_set_camera_pos(JceLightEnv *env, jce_vec3 pos);

/* Upload all light uniforms for the current draw state.
 * Call once per frame before submitting PBR draw calls. */
void jce_light_env_apply(const JceLightEnv *env, const JceRenderer *r);

JCE_EXTERN_C_END

#endif /* JCE_LIGHTING_SYSTEM_H */
