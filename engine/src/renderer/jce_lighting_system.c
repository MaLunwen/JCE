/*
 * jce_lighting_system.c  Multi-light environment implementation.
 */

#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_lighting_system.h>
#include <jce/renderer/jce_texture_types.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <math.h>
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

    /* P3-E.5b — Multi-cookie atlas (per-env LRU bookkeeping).
     *
     * Slot 0 is reserved for a 1x1 white default ("no cookie").
     * Slots 1..(capacity-1) hold registered cookie textures and are
     * managed LRU.  Eviction policy: when adding a new cookie and all
     * non-zero slots are occupied, the slot with the smallest
     * last_used counter is evicted.
     *
     * NOTE on the array texture itself: the current ship binds at most
     * one cookie image to sampler slot 13 each draw (the single-bind
     * fallback path from P3-E.5).  The atlas slot indices are still
     * uploaded to the shader per-light so that when sampler 13 is
     * promoted to a 2D-texture-array bind (gated on
     * BGFX_CAPS_TEXTURE_2D_ARRAY + shaderc recompile of fs_pbr.sc with
     * the array sampler form already present in source), no CPU code
     * needs to change.  See engine/src/renderer/AGENTS.md "P3-E.5b". */
    uint16_t cookie_slot_handle[JCE_COOKIE_ATLAS_CAPACITY];   /* 0 = empty (also: slot 0 reserved white) */
    uint64_t cookie_slot_last_used[JCE_COOKIE_ATLAS_CAPACITY];
    uint64_t cookie_frame_counter;
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
/* P3-E.5 — Cookie + IES profile (spot lights v1; dir cookie scaffolded).
 *
 *   u_cookieParams.x = has_cookie_spot   (1.0 / 0.0)
 *   u_cookieParams.y = cookie_strength   (0..1)
 *   u_cookieParams.z = has_ies_spot      (1.0 / 0.0)
 *   u_cookieParams.w = cookie_spot_index (which u_spotLights[] slot owns
 *                                          the cookie/IES sampler binding) */
static bgfx_uniform_handle_t s_u_cookie_params;
static bgfx_uniform_handle_t s_u_cookie_spot_vp; /* mat4: clip-from-world for spot cookie */
/* P3-E.5b — Directional cookie projection.
 *   u_cookieDirParams.x = has_cookie_dir   (1.0 / 0.0)
 *   u_cookieDirParams.y = cookie_strength  (0..1)
 *   u_cookieDirParams.z = cookie_dir_index (atlas slot for the active
 *                                            dir cookie; reserved for
 *                                            sampler-array promotion)
 *   u_cookieDirParams.w = 0 */
static bgfx_uniform_handle_t s_u_cookie_dir_params;
static bgfx_uniform_handle_t s_u_cookie_dir_vp;  /* mat4: clip-from-world for dir cookie */
static bgfx_uniform_handle_t s_s_cookie;         /* sampler slot 13 */
static bgfx_uniform_handle_t s_s_ies_lut;        /* sampler slot 14 */
static bgfx_texture_handle_t s_white_1x1;        /* default 1x1 white bind */
/* P4-E.3b — Cookie atlas promoted to texture2DArray when backend supports it.
 * All JCE_COOKIE_ATLAS_CAPACITY layers are pre-initialised to white so slot 0
 * (the sentinel) is always a valid "no cookie" sample. */
static bgfx_texture_handle_t s_cookie_atlas_array;  /* valid when s_cookie_array_supported */
static bool                  s_cookie_array_supported = false;
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

    /* P3-E.5 — cookie + IES uniforms and samplers.
     * P3-E.5b — adds u_cookieDirParams + u_cookieDirVP for directional
     * light cookie projection (sampler 13 shared with the spot cookie
     * under the v1 single-bind constraint). */
    s_u_cookie_params     = bgfx_create_uniform("u_cookieParams",    BGFX_UNIFORM_TYPE_VEC4, 1);
    s_u_cookie_spot_vp    = bgfx_create_uniform("u_cookieSpotVP",    BGFX_UNIFORM_TYPE_MAT4, 1);
    s_u_cookie_dir_params = bgfx_create_uniform("u_cookieDirParams", BGFX_UNIFORM_TYPE_VEC4, 1);
    s_u_cookie_dir_vp     = bgfx_create_uniform("u_cookieDirVP",     BGFX_UNIFORM_TYPE_MAT4, 1);
    s_s_cookie            = bgfx_create_uniform("s_cookie",          BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s_s_ies_lut           = bgfx_create_uniform("s_iesLut",          BGFX_UNIFORM_TYPE_SAMPLER, 1);

    /* Default 1x1 white texture for safe bind even when no cookie/IES
     * is active (avoids undefined sampler reads on tight backends). */
    static const uint32_t kWhitePix = 0xFFFFFFFFu;
    const bgfx_memory_t *mem = bgfx_copy(&kWhitePix, sizeof(kWhitePix));
    s_white_1x1 = bgfx_create_texture_2d(1, 1, false, 1,
                                          BGFX_TEXTURE_FORMAT_RGBA8,
                                          BGFX_TEXTURE_NONE
                                          | BGFX_SAMPLER_U_CLAMP
                                          | BGFX_SAMPLER_V_CLAMP, mem);

    s_light_uniforms_init = true;

    /* P4-E.3b — Promote cookie atlas to texture2DArray where supported.
     * All layers are filled with 0xFFFFFFFF (white) so the sentinel slot-0
     * and any un-populated layers default to "no tint" (multiply by 1). */
    {
        const bgfx_caps_t *caps = bgfx_get_caps();
        s_cookie_array_supported = (caps->supported & BGFX_CAPS_TEXTURE_2D_ARRAY) != 0;
        if (s_cookie_array_supported) {
            const int k_cookie_dim    = 256;
            const int k_cookie_layers = (int)JCE_COOKIE_ATLAS_CAPACITY;
            const int k_bytes = k_cookie_dim * k_cookie_dim * 4 * k_cookie_layers;
            uint8_t  *buf = (uint8_t *)JCE_MALLOC(k_bytes);
            if (buf) {
                memset(buf, 0xFF, (size_t)k_bytes);
                const bgfx_memory_t *amem = bgfx_copy(buf, (uint32_t)k_bytes);
                JCE_FREE(buf);
                s_cookie_atlas_array = bgfx_create_texture_2d(
                    (uint16_t)k_cookie_dim, (uint16_t)k_cookie_dim,
                    false, (uint16_t)k_cookie_layers,
                    BGFX_TEXTURE_FORMAT_RGBA8,
                    BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, amem);
                LOG_DEBUG(LOG_TAG, "cookie atlas array texture created (%d layers)", k_cookie_layers);
                /* TODO(P4-E.3b): populate array layers from registered cookie textures
                 * using bgfx_blit() (requires source textures to have
                 * BGFX_TEXTURE_BLIT_SRC) — pending per-cookie re-import flag. */
            } else {
                s_cookie_array_supported = false;
                LOG_WARN(LOG_TAG, "cookie array alloc failed — falling back to 2D atlas");
            }
        }
    }

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
    /* P3-E.5b — atlas slot 0 reserved for the white "no cookie"
     * default; mark it occupied with a sentinel handle so eviction
     * never targets it. */
    env->cookie_slot_handle[0]    = UINT16_MAX;
    env->cookie_slot_last_used[0] = 0;
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
     * [i*2+1] = color.xyz, cookie_atlas_slot  (P3-E.5b)
     *
     * cookie_atlas_slot is the slice index into the cookie atlas; 0 =
     * no cookie (white).  Used by the shader to pick the array layer
     * when sampler 13 is promoted to a 2D texture array; harmless on
     * the v1 single-bind path. */
    {
        float data[JCE_MAX_DIR_LIGHTS * 2 * 4];
        memset(data, 0, sizeof(data));
        JceLightEnv *mut_env = (JceLightEnv *)env; /* register_cookie mutates LRU */
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
            data[base + 7] = (float)jce_light_env_register_cookie(mut_env, dl->cookie_texture);
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
     * [i*4+3] = outerConeCos, cookie_atlas_slot, 0, 0  (P3-E.5b)
     *
     * cookie_atlas_slot is the slice index into the cookie atlas; 0 =
     * no cookie (white).  Same meaning as the directional channel. */
    {
        float data[JCE_MAX_SPOT_LIGHTS * 4 * 4];
        memset(data, 0, sizeof(data));
        JceLightEnv *mut_env = (JceLightEnv *)env;
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
            data[base + 13] = (float)jce_light_env_register_cookie(mut_env, sl->cookie_texture);
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

    /* ──────────────────────────────────────────────────────────────
     * P3-E.5  — Cookie + IES profile (spot lights, single-bind v1)
     * P3-E.5b — Directional light cookie projection (shared sampler)
     *
     * GPU model: bgfx samplers are global per draw call, so v1 binds
     * at most ONE cookie texture to sampler 13.  Selection priority:
     *   1. First spot light that has a cookie wins.
     *   2. Otherwise, first directional light that has a cookie wins.
     *   3. Otherwise, white 1x1 default.
     * The 2D-texture-array atlas slot for every registered cookie is
     * still uploaded per-light (see u_dirLights / u_spotLights w / 13
     * channels) so promoting s_cookie to a SAMPLER2DARRAY (gated on
     * BGFX_CAPS_TEXTURE_2D_ARRAY and a shaderc recook of fs_pbr.sc)
     * lifts the single-bind constraint without further CPU changes.
     *
     * Directional VP construction: fixed-size ortho box centred on
     * the camera position, oriented along the light forward.  This is
     * the v1 "stable cookie" choice (documented in
     * engine/src/renderer/AGENTS.md "P3-E.5b").  Tight CSM-cascade
     * alignment is deferred — would couple the lighting system to the
     * shadow pipeline and was not needed for visual parity.
     * ────────────────────────────────────────────────────────────── */
    {
        int      cookie_spot = -1;
        int      ies_spot    = -1;
        int      cookie_dir  = -1;
        float    cookie_strength_spot = 1.0f;
        float    cookie_strength_dir  = 1.0f;
        bgfx_texture_handle_t cookie_tex = s_white_1x1;
        bgfx_texture_handle_t ies_tex    = s_white_1x1;

        for (uint32_t i = 0; i < env->num_spot; i++) {
            const JceSpotLightDesc *sl = &env->spot_lights[i];
            if (cookie_spot < 0 && jce_texture_valid(sl->cookie_texture)) {
                cookie_spot = (int)i;
                cookie_tex.idx = sl->cookie_texture.idx;
                cookie_strength_spot = (sl->cookie_strength > 0.0f) ? sl->cookie_strength : 1.0f;
                if (cookie_strength_spot > 1.0f) cookie_strength_spot = 1.0f;
            }
            if (ies_spot < 0 && jce_texture_valid(sl->ies_lut_texture)) {
                ies_spot = (int)i;
                ies_tex.idx = sl->ies_lut_texture.idx;
            }
        }
        for (uint32_t i = 0; i < env->num_dir; i++) {
            const JceDirLightDesc *dl = &env->dir_lights[i];
            if (cookie_dir < 0 && jce_texture_valid(dl->cookie_texture)) {
                cookie_dir = (int)i;
                cookie_strength_dir = (dl->cookie_strength > 0.0f) ? dl->cookie_strength : 1.0f;
                if (cookie_strength_dir > 1.0f) cookie_strength_dir = 1.0f;
                /* Spot cookie wins the single sampler slot. */
                if (cookie_spot < 0) {
                    cookie_tex.idx = dl->cookie_texture.idx;
                }
            }
        }

        /* Spot cookie VP --------------------------------------------- */
        int driver_spot = (cookie_spot >= 0) ? cookie_spot : ies_spot;
        float vp[16];
        memset(vp, 0, sizeof(vp));
        vp[0] = vp[5] = vp[10] = vp[15] = 1.0f;

        if (driver_spot >= 0 && (uint32_t)driver_spot < env->num_spot) {
            const JceSpotLightDesc *sl = &env->spot_lights[driver_spot];
            float fov = 2.0f * acosf(sl->outer_cone_cos);
            if (fov < 0.01f) fov = 0.01f;
            if (fov > 3.10f) fov = 3.10f;
            float zn = 0.05f * (sl->radius > 0.0f ? sl->radius : 1.0f);
            float zf = (sl->radius > 0.0f) ? sl->radius : 100.0f;

            jce_vec3 eye    = sl->position;
            jce_vec3 d      = jce_v3_normalize(sl->direction);
            jce_vec3 center = jce_v3_add(eye, d);
            jce_vec3 up     = jce_v3(0.0f, 1.0f, 0.0f);
            if (fabsf(d.y) > 0.99f) up = jce_v3(0.0f, 0.0f, 1.0f);

            jce_mat4 view = jce_m4_look_at(eye, center, up);
            jce_mat4 proj = jce_m4_perspective(fov, 1.0f, zn, zf, false);
            jce_mat4 vp4  = jce_m4_multiply(&proj, &view);
            memcpy(vp, JCE_M4_PTR(vp4), sizeof(vp));
        }
        bgfx_set_uniform(s_u_cookie_spot_vp, vp, 1);

        /* Directional cookie VP (P3-E.5b) --------------------------- */
        /* World-aligned ortho box centred on camera xz, viewed along
         * the chosen directional light's forward.  Half-extent is
         * fixed (50m) for v1 stability — no per-frame jitter, no CSM
         * coupling.  Slide-to-v2: re-use cascade 0's tight frustum
         * fit from jce_csm_compute when the lighting system gains
         * camera-frustum awareness. */
        float dir_vp[16];
        memset(dir_vp, 0, sizeof(dir_vp));
        dir_vp[0] = dir_vp[5] = dir_vp[10] = dir_vp[15] = 1.0f;

        if (cookie_dir >= 0 && (uint32_t)cookie_dir < env->num_dir) {
            const JceDirLightDesc *dl = &env->dir_lights[cookie_dir];
            jce_vec3 d  = jce_v3_normalize(dl->direction); /* shine direction */
            jce_vec3 eye = jce_v3_sub(env->camera_pos,
                                      jce_v3(d.x * 50.0f, d.y * 50.0f, d.z * 50.0f));
            jce_vec3 center = env->camera_pos;
            jce_vec3 up = jce_v3(0.0f, 1.0f, 0.0f);
            if (fabsf(d.y) > 0.99f) up = jce_v3(0.0f, 0.0f, 1.0f);

            jce_mat4 view = jce_m4_look_at(eye, center, up);
            const float half = 50.0f; /* world units */
            jce_mat4 proj = jce_m4_ortho(-half, half, -half, half,
                                          0.1f, 200.0f, false);
            jce_mat4 vp4  = jce_m4_multiply(&proj, &view);
            memcpy(dir_vp, JCE_M4_PTR(vp4), sizeof(dir_vp));
        }
        bgfx_set_uniform(s_u_cookie_dir_vp, dir_vp, 1);

        /* Param uniforms -------------------------------------------- */
        float params[4] = {
            (cookie_spot >= 0) ? 1.0f : 0.0f,
            cookie_strength_spot,
            (ies_spot    >= 0) ? 1.0f : 0.0f,
            (cookie_spot >= 0) ? (float)cookie_spot : (float)ies_spot
        };
        bgfx_set_uniform(s_u_cookie_params, params, 1);

        float dir_params[4] = {
            (cookie_dir >= 0) ? 1.0f : 0.0f,
            cookie_strength_dir,
            (cookie_dir >= 0) ? (float)cookie_dir : 0.0f,
            0.0f
        };
        bgfx_set_uniform(s_u_cookie_dir_params, dir_params, 1);

        bgfx_set_texture(13, s_s_cookie,
                         s_cookie_array_supported ? s_cookie_atlas_array : cookie_tex,
                         UINT32_MAX);
        bgfx_set_texture(14, s_s_ies_lut, ies_tex,    UINT32_MAX);
    }
}

void jce_light_env_set_camera_pos(JceLightEnv *env, jce_vec3 pos)
{
    if (env) env->camera_pos = pos;
}

/* ================================================================== */
/* P3-E.5b — Cookie atlas (LRU)                                        */
/* ================================================================== */

int jce_light_env_register_cookie(JceLightEnv *env, JceTexture cookie)
{
    if (!env || !jce_texture_valid(cookie)) return 0;

    env->cookie_frame_counter++;

    /* Lookup: already-registered handle wins (touch LRU). */
    for (int i = 1; i < (int)JCE_COOKIE_ATLAS_CAPACITY; i++) {
        if (env->cookie_slot_handle[i] == cookie.idx) {
            env->cookie_slot_last_used[i] = env->cookie_frame_counter;
            return i;
        }
    }

    /* Allocate: prefer an empty slot. */
    for (int i = 1; i < (int)JCE_COOKIE_ATLAS_CAPACITY; i++) {
        if (env->cookie_slot_handle[i] == 0) {
            env->cookie_slot_handle[i]    = cookie.idx;
            env->cookie_slot_last_used[i] = env->cookie_frame_counter;
            return i;
        }
    }

    /* Evict: smallest last_used wins (slot 0 reserved, skipped). */
    int      victim = 1;
    uint64_t oldest = env->cookie_slot_last_used[1];
    for (int i = 2; i < (int)JCE_COOKIE_ATLAS_CAPACITY; i++) {
        if (env->cookie_slot_last_used[i] < oldest) {
            oldest = env->cookie_slot_last_used[i];
            victim = i;
        }
    }
    LOG_DEBUG(LOG_TAG, "cookie atlas full (cap=%d), evicting slot %d (handle=%u)",
              JCE_COOKIE_ATLAS_CAPACITY, victim,
              (unsigned)env->cookie_slot_handle[victim]);
    env->cookie_slot_handle[victim]    = cookie.idx;
    env->cookie_slot_last_used[victim] = env->cookie_frame_counter;
    return victim;
}
