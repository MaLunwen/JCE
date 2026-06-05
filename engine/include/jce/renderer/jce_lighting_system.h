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
#include <jce/renderer/jce_texture_types.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceRenderer JceRenderer;
typedef struct JceLightEnv JceLightEnv;

/* ================================================================== */
/* Limits                                                              */
/* ================================================================== */

#define JCE_MAX_DIR_LIGHTS   2
#define JCE_MAX_POINT_LIGHTS 8
#define JCE_MAX_SPOT_LIGHTS  4

/* P3-E.5b — Multi-cookie atlas (texture array) capacity.
 * Slot 0 is reserved for a 1x1 white default ("no cookie"); slots
 * 1..(capacity-1) hold registered cookie images.  LRU eviction kicks
 * in when more unique cookies than the cap are requested in a frame. */
#define JCE_COOKIE_ATLAS_CAPACITY 16

/* ================================================================== */
/* Light descriptors                                                   */
/* ================================================================== */

typedef struct {
    jce_vec3 direction;       /* normalized direction light travels */
    jce_vec3 color;           /* linear RGB */
    float    intensity;       /* multiplier, default 1.0 */
    bool     casts_shadow;    /* enable shadow map for this light */
    /* P3-E.5 — Optional directional light cookie (projector mask). */
    JceTexture cookie_texture; /* JCE_TEXTURE_INVALID = disabled */
    float      cookie_strength;
} JceDirLightDesc;

typedef struct {
    jce_vec3 position;
    jce_vec3 color;           /* linear RGB */
    float    intensity;
    float    radius;          /* attenuation cutoff distance */
    bool     casts_shadow;    /* P1 — enable local (atlas) shadow map */
    float    shadow_bias;     /* depth bias; 0 = engine default */
} JcePointLightDesc;

typedef struct {
    jce_vec3 position;
    jce_vec3 direction;       /* normalized direction the spot points */
    jce_vec3 color;           /* linear RGB */
    float    intensity;
    float    inner_cone_cos;  /* cos(inner half-angle), full intensity inside */
    float    outer_cone_cos;  /* cos(outer half-angle), zero intensity outside */
    float    radius;          /* attenuation cutoff */
    bool     casts_shadow;    /* P1 — enable local (atlas) shadow map */
    float    shadow_bias;     /* depth bias; 0 = engine default */
    /* P3-E.5 — Optional cookie + IES profile. */
    JceTexture cookie_texture; /* JCE_TEXTURE_INVALID = no cookie */
    JceTexture ies_lut_texture;/* JCE_TEXTURE_INVALID = no IES */
    float      cookie_strength;
} JceSpotLightDesc;

/* ================================================================== */
/* Light environment API                                               */
/* ================================================================== */

/* Create an empty light environment. */
JCE_API JceLightEnv *jce_light_env_create(void);

/* Destroy the light environment. */
JCE_API void jce_light_env_destroy(JceLightEnv *env);

/* Set ambient light color and intensity. */
JCE_API void jce_light_env_set_ambient(JceLightEnv *env, jce_vec3 color, float intensity);

/* Add lights. Returns the light index, or -1 if at capacity. */
JCE_API int jce_light_env_add_dir_light(JceLightEnv *env, const JceDirLightDesc *light);
JCE_API int jce_light_env_add_point_light(JceLightEnv *env, const JcePointLightDesc *light);
JCE_API int jce_light_env_add_spot_light(JceLightEnv *env, const JceSpotLightDesc *light);

/* Clear all lights (ambient retained). */
JCE_API void jce_light_env_clear(JceLightEnv *env);

/* Get current light counts. */
JCE_API uint32_t jce_light_env_dir_count(const JceLightEnv *env);
JCE_API uint32_t jce_light_env_point_count(const JceLightEnv *env);
JCE_API uint32_t jce_light_env_spot_count(const JceLightEnv *env);

/* Set the camera world position (needed for PBR specular and for
 * directional-cookie projection centring). */
JCE_API void jce_light_env_set_camera_pos(JceLightEnv *env, jce_vec3 pos);

/* P3-E.5b — Register a cookie texture in the per-frame atlas and
 * return its slice index.  Returns 0 (white default) when the texture
 * handle is invalid or the atlas backend is not available.  Repeated
 * registrations of the same handle return the cached slot and touch
 * the LRU.  When the atlas is full a least-recently-used slot is
 * evicted.  Cap = JCE_COOKIE_ATLAS_CAPACITY (slot 0 reserved). */
JCE_API int jce_light_env_register_cookie(JceLightEnv *env, JceTexture cookie);

/* Upload all light uniforms for the current draw state.
 * Call once per frame before submitting PBR draw calls. */
JCE_API void jce_light_env_apply(const JceLightEnv *env, const JceRenderer *r);

JCE_EXTERN_C_END

#endif /* JCE_LIGHTING_SYSTEM_H */
