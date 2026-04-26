/*
 * jce_lighting_system.c  Multi-light environment implementation.
 */

#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_lighting_system.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <string.h>

#define LOG_TAG "jce_lighting_system"

struct JceLightEnv {
    jce_vec3 ambient_color;
    float    ambient_intensity;

    JceDirLightDesc   dir_lights[JCE_MAX_DIR_LIGHTS];
    uint32_t          num_dir;

    JcePointLightDesc point_lights[JCE_MAX_POINT_LIGHTS];
    uint32_t          num_point;

    JceSpotLightDesc  spot_lights[JCE_MAX_SPOT_LIGHTS];
    uint32_t          num_spot;

    jce_vec3 camera_pos;
};

/* ================================================================== */
/* Static uniforms (lazy-initialized)                                  */
/* ================================================================== */

static bgfx_uniform_handle_t s_u_ambient_color;
static bgfx_uniform_handle_t s_u_dir_lights;
static bgfx_uniform_handle_t s_u_point_lights;
static bgfx_uniform_handle_t s_u_spot_lights;
static bgfx_uniform_handle_t s_u_light_counts;
static bgfx_uniform_handle_t s_u_camera_pos;
static bool s_light_uniforms_init = false;

static void ensure_light_uniforms(void)
{
    if (s_light_uniforms_init) return;

    s_u_ambient_color = bgfx_create_uniform("u_ambientColor", BGFX_UNIFORM_TYPE_VEC4, 1);
    /* dir: 2 vec4s per light * max 2 = 4 vec4s */
    s_u_dir_lights    = bgfx_create_uniform("u_dirLights",    BGFX_UNIFORM_TYPE_VEC4,
                                             JCE_MAX_DIR_LIGHTS * 2);
    /* point: 2 vec4s per light * max 8 = 16 vec4s */
    s_u_point_lights  = bgfx_create_uniform("u_pointLights",  BGFX_UNIFORM_TYPE_VEC4,
                                             JCE_MAX_POINT_LIGHTS * 2);
    /* spot: 4 vec4s per light * max 4 = 16 vec4s */
    s_u_spot_lights   = bgfx_create_uniform("u_spotLights",   BGFX_UNIFORM_TYPE_VEC4,
                                             JCE_MAX_SPOT_LIGHTS * 4);
    s_u_light_counts  = bgfx_create_uniform("u_lightCounts",  BGFX_UNIFORM_TYPE_VEC4, 1);
    s_u_camera_pos    = bgfx_create_uniform("u_cameraPos",    BGFX_UNIFORM_TYPE_VEC4, 1);

    s_light_uniforms_init = true;
    LOG_DEBUG(LOG_TAG, "light uniforms initialized");
}

/* ================================================================== */
/* Create / Destroy                                                    */
/* ================================================================== */

JceLightEnv *jce_light_env_create(void)
{
    JceLightEnv *env = (JceLightEnv *)JCE_CALLOC(1, sizeof(*env));
    if (!env) return NULL;

    env->ambient_color     = jce_v3(1.0f, 1.0f, 1.0f);
    env->ambient_intensity = 0.1f;
    return env;
}

void jce_light_env_destroy(JceLightEnv *env)
{
    JCE_FREE(env);
}

/* ================================================================== */
/* Configuration                                                       */
/* ================================================================== */

void jce_light_env_set_ambient(JceLightEnv *env, jce_vec3 color, float intensity)
{
    if (!env) return;
    env->ambient_color     = color;
    env->ambient_intensity = intensity;
}

int jce_light_env_add_dir_light(JceLightEnv *env, const JceDirLightDesc *light)
{
    if (!env || !light || env->num_dir >= JCE_MAX_DIR_LIGHTS) return -1;
    env->dir_lights[env->num_dir] = *light;
    return (int)env->num_dir++;
}

int jce_light_env_add_point_light(JceLightEnv *env, const JcePointLightDesc *light)
{
    if (!env || !light || env->num_point >= JCE_MAX_POINT_LIGHTS) return -1;
    env->point_lights[env->num_point] = *light;
    return (int)env->num_point++;
}

int jce_light_env_add_spot_light(JceLightEnv *env, const JceSpotLightDesc *light)
{
    if (!env || !light || env->num_spot >= JCE_MAX_SPOT_LIGHTS) return -1;
    env->spot_lights[env->num_spot] = *light;
    return (int)env->num_spot++;
}

void jce_light_env_clear(JceLightEnv *env)
{
    if (!env) return;
    env->num_dir   = 0;
    env->num_point = 0;
    env->num_spot  = 0;
}

/* ================================================================== */
/* Queries                                                             */
/* ================================================================== */

uint32_t jce_light_env_dir_count(const JceLightEnv *env)
{
    return env ? env->num_dir : 0;
}

uint32_t jce_light_env_point_count(const JceLightEnv *env)
{
    return env ? env->num_point : 0;
}

uint32_t jce_light_env_spot_count(const JceLightEnv *env)
{
    return env ? env->num_spot : 0;
}

/* ================================================================== */
/* Apply (upload uniforms)                                             */
/* ================================================================== */

void jce_light_env_apply(const JceLightEnv *env, const JceRenderer *r)
{
    if (!env) return;
    (void)r;

    ensure_light_uniforms();

    /* Ambient: xyz = raw color, w = intensity.
       Shader computes: u_ambientColor.xyz * u_ambientColor.w * albedo * ao */
    float ambient[4] = {
        env->ambient_color.x,
        env->ambient_color.y,
        env->ambient_color.z,
        env->ambient_intensity
    };
    bgfx_set_uniform(s_u_ambient_color, ambient, 1);

    /* Directional lights: 2 vec4s per light.
     * [i*2+0] = normalize(dir).xyz, intensity
     * [i*2+1] = color.xyz, 0 */
    {
        float data[JCE_MAX_DIR_LIGHTS * 2 * 4];
        memset(data, 0, sizeof(data));
        for (uint32_t i = 0; i < env->num_dir; i++) {
            const JceDirLightDesc *dl = &env->dir_lights[i];
            jce_vec3 d = jce_v3_normalize(dl->direction);
            uint32_t base = i * 8;
            data[base + 0] = d.x;
            data[base + 1] = d.y;
            data[base + 2] = d.z;
            data[base + 3] = dl->intensity;
            data[base + 4] = dl->color.x;
            data[base + 5] = dl->color.y;
            data[base + 6] = dl->color.z;
            data[base + 7] = 0.0f;
        }
        bgfx_set_uniform(s_u_dir_lights, data, JCE_MAX_DIR_LIGHTS * 2);
    }

    /* Point lights: 2 vec4s per light.
     * [i*2+0] = pos.xyz, radius
     * [i*2+1] = color.xyz, intensity */
    {
        float data[JCE_MAX_POINT_LIGHTS * 2 * 4];
        memset(data, 0, sizeof(data));
        for (uint32_t i = 0; i < env->num_point; i++) {
            const JcePointLightDesc *pl = &env->point_lights[i];
            uint32_t base = i * 8;
            data[base + 0] = pl->position.x;
            data[base + 1] = pl->position.y;
            data[base + 2] = pl->position.z;
            data[base + 3] = pl->radius;
            data[base + 4] = pl->color.x;
            data[base + 5] = pl->color.y;
            data[base + 6] = pl->color.z;
            data[base + 7] = pl->intensity;
        }
        bgfx_set_uniform(s_u_point_lights, data, JCE_MAX_POINT_LIGHTS * 2);
    }

    /* Spot lights: 4 vec4s per light.
     * [i*4+0] = pos.xyz, radius
     * [i*4+1] = dir.xyz, intensity
     * [i*4+2] = color.xyz, innerConeCos
     * [i*4+3] = outerConeCos, 0, 0, 0 */
    {
        float data[JCE_MAX_SPOT_LIGHTS * 4 * 4];
        memset(data, 0, sizeof(data));
        for (uint32_t i = 0; i < env->num_spot; i++) {
            const JceSpotLightDesc *sl = &env->spot_lights[i];
            jce_vec3 d = jce_v3_normalize(sl->direction);
            uint32_t base = i * 16;
            data[base + 0]  = sl->position.x;
            data[base + 1]  = sl->position.y;
            data[base + 2]  = sl->position.z;
            data[base + 3]  = sl->radius;
            data[base + 4]  = d.x;
            data[base + 5]  = d.y;
            data[base + 6]  = d.z;
            data[base + 7]  = sl->intensity;
            data[base + 8]  = sl->color.x;
            data[base + 9]  = sl->color.y;
            data[base + 10] = sl->color.z;
            data[base + 11] = sl->inner_cone_cos;
            data[base + 12] = sl->outer_cone_cos;
            data[base + 13] = 0.0f;
            data[base + 14] = 0.0f;
            data[base + 15] = 0.0f;
        }
        bgfx_set_uniform(s_u_spot_lights, data, JCE_MAX_SPOT_LIGHTS * 4);
    }

    /* Light counts: x=numDir, y=numPoint, z=numSpot, w=0. */
    float counts[4] = {
        (float)env->num_dir,
        (float)env->num_point,
        (float)env->num_spot,
        0.0f
    };
    bgfx_set_uniform(s_u_light_counts, counts, 1);

    /* Camera position for PBR specular. */
    float cam[4] = { env->camera_pos.x, env->camera_pos.y, env->camera_pos.z, 0.0f };
    bgfx_set_uniform(s_u_camera_pos, cam, 1);
}

void jce_light_env_set_camera_pos(JceLightEnv *env, jce_vec3 pos)
{
    if (env) env->camera_pos = pos;
}
