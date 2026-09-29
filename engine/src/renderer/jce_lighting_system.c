/*
 * jce_lighting_system.c  Multi-light environment implementation.
 */

#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_lighting_system.h>
#include <jce/renderer/jce_views.h>
#include <jce/renderer/jce_renderer.h>   /* jce_renderer_get_frame_index */
#include <jce/renderer/jce_texture_types.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "renderer/jce_render_encoder.h"

#define LOG_TAG "jce_lighting_system"

struct JceLightEnv {
    jce_vec3 ambient_color;
    float    ambient_intensity;

    JceDirLightDesc   dir_lights[JCE_MAX_DIR_LIGHTS];
    uint32_t          num_dir;

    JcePointLightDesc point_lights[JCE_MAX_POINT_LIGHTS];
    uint32_t          num_point;

    JceSpotLightDesc  spot_lights[JCE_MAX_SPOT_LIGHTS];
    uint32_t          num_area;
    JceAreaLightDesc  area_lights[JCE_MAX_AREA_LIGHTS];
    uint32_t          num_spot;

    jce_vec3 camera_pos;

    /* Packed-uniform cache. jce_light_env_apply() runs once per MATERIAL
     * CHANGE through the scene renderer's bind callback (and per entity on
     * its inline path) — not once per frame. Re-packing the light arrays,
     * re-normalizing directions and rebuilding both cookie VP matrices on
     * every call recomputed identical results dozens of times a frame: the
     * env's contents only change through the setters (which set pack_dirty)
     * or across frames (the scene renderer rebuilds the env each frame).
     * So pack once per frame into these buffers and replay the cached bytes
     * on every later apply. The UPLOADS are NOT cacheable: bgfx draw items
     * record only the uniform updates issued since the last state-discarding
     * submit and replay them in view-SORTED draw order, so every material
     * run's first draw must carry the full light state in its own range. */
    bool     pack_dirty;
    bool     pack_valid;
    uint32_t pack_frame;
    bool     packed_has_dir_cookie; /* dir-cookie VP depends on camera_pos */
    jce_vec3 packed_cam_pos;        /* camera the dir-cookie VP was built at */
    float    packed_ambient[4];
    float    packed_dir[JCE_MAX_DIR_LIGHTS * 2 * 4];
    float    packed_point[JCE_MAX_POINT_LIGHTS * 2 * 4];
    float    packed_spot[JCE_MAX_SPOT_LIGHTS * 4 * 4];
    float    packed_area[JCE_MAX_AREA_LIGHTS * 4 * 4];
    float    packed_area_params[4];
    float    packed_counts[4];
    float    packed_dir_vp[16];
    float    packed_ies_params[4];
    float    packed_cookie_dir_params[4];
    bgfx_texture_handle_t packed_ies_tex;

    /* Rendering layers -- the pack as one RECEIVER LAYER sees it.
     *
     * ONE slot, not 32: the scene renderer folds the receiver's layer into the
     * material key, so a batch's draws all share a layer and consecutive
     * applies ask for the same one.  The memo therefore misses once per layer
     * change and hits on every draw after it.
     *
     * any_layer_mask is the whole feature's off switch: recomputed at repack,
     * false for every scene that never authored a mask, and when it is false
     * jce_light_env_apply_for_layer IS jce_light_env_apply -- it delegates,
     * so a scene that does not use this pays nothing and uploads the same
     * bytes it always did. */
    bool     any_layer_mask;
    bool     masked_valid;
    uint32_t masked_layer;
    float    masked_dir[JCE_MAX_DIR_LIGHTS * 2 * 4];
    float    masked_point[JCE_MAX_POINT_LIGHTS * 2 * 4];
    float    masked_spot[JCE_MAX_SPOT_LIGHTS * 4 * 4];
    float    masked_area[JCE_MAX_AREA_LIGHTS * 4 * 4];

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
    /* JCE_COOKIE_SLOT_EMPTY = nothing here.  NOT 0: bgfx handle 0 is a VALID
     * handle, and a cookie texture that legitimately got it was assigned a
     * slot and then skipped by the layer sync as "empty" -- that light
     * projected a blank layer and looked exactly like a light with no cookie.
     * Measured on the two-cookie fixture: `spot 1: slot=2 tex=0`. */
    uint16_t cookie_slot_handle[JCE_COOKIE_ATLAS_CAPACITY];
    uint64_t cookie_slot_last_used[JCE_COOKIE_ATLAS_CAPACITY];
    uint64_t cookie_frame_counter;
    /* What each LAYER of the array texture currently holds, which is a
     * different question from what each SLOT is assigned: the assignment is
     * decided when a light registers, the layer is filled by a blit that has
     * to happen on a frame the renderer is running.  Keeping them apart is
     * what makes the blit happen exactly once per (slot, texture) pair
     * instead of every frame. */
    uint16_t cookie_layer_uploaded[JCE_COOKIE_ATLAS_CAPACITY];
};

/* ================================================================== */
/* Static uniforms (lazy-initialized)                                  */
/* ================================================================== */

/* ONE RECORD, not seventeen statics.  Every field below is created by the
 * same ensure_light_uniforms() and is meaningless before it runs -- which the
 * file already implied by keeping an init flag at the end of the list.  The
 * comments that explained each uniform's packing are kept verbatim beside the
 * field they explain. */
static struct {
    bgfx_uniform_handle_t ambient_color;
    bgfx_uniform_handle_t dir_lights;
    bgfx_uniform_handle_t point_lights;
    bgfx_uniform_handle_t spot_lights;
    bgfx_uniform_handle_t light_counts;
    /* The area-light pair: created together, uploaded together, and
     * meaningless apart -- the array is unreadable without the count that
     * says how much of it is live. */
    bgfx_uniform_handle_t area_lights;
    bgfx_uniform_handle_t area_params;
    bgfx_uniform_handle_t camera_pos;
    /* P3-E.5 — IES photometric profile.  Spot COOKIES are not here: every
     * cookie field this vec4 used to carry described the single light that
     * won the sampler bind, and each now lives in that light's own pack.
     *
     *   u_iesParams.x = has_ies_spot   (1.0 / 0.0)
     *   u_iesParams.y = ies_spot_index (which u_spotLights[] slot owns the
     *                                    one LUT sampler 14 can carry)
     *   u_iesParams.zw = 0, unread; a vec4 is the smallest uniform here. */
    bgfx_uniform_handle_t ies_params;
    /* P3-E.5b — Directional cookie projection.
     *   u_cookieDirParams.x = has_cookie_dir   (1.0 / 0.0)
     *   u_cookieDirParams.y = cookie_strength  (0..1)
     *   u_cookieDirParams.z = cookie_dir_index (atlas slot for the active
     *                                            dir cookie; reserved for
     *                                            sampler-array promotion)
     *   u_cookieDirParams.w = 0 */
    bgfx_uniform_handle_t cookie_dir_params;
    bgfx_uniform_handle_t cookie_dir_vp;    /* mat4: clip-from-world, dir cookie */
    bgfx_uniform_handle_t s_cookie;         /* sampler slot 13 (2D ARRAY) */
    /* Always a valid array texture, so sampler 13 never holds the wrong
     * TYPE: the real 16-layer atlas when cookie_array_ok, a 1x1 white
     * one-layer placeholder when not. */
    bgfx_texture_handle_t cookie_atlas_array;
    bool                  cookie_array_ok;
    bgfx_uniform_handle_t s_ies_lut;        /* sampler slot 14 */
    bgfx_texture_handle_t white_1x1;        /* default 1x1 white bind */
    /* P4-E.3b — Cookie atlas promoted to texture2DArray when the backend
     * supports it.  All JCE_COOKIE_ATLAS_CAPACITY layers are pre-initialised
     * to white so slot 0 (the sentinel) is always a valid "no cookie" sample. */
    bool                  init;
} s_lu;

/* Defined beside the slot allocator it mirrors; declared here because the
 * per-submit bind that calls it comes first in this file. */
/* bgfx handle 0 is VALID, so neither of these may be 0.  RESERVED and EMPTY
 * are separate values because they answer different questions -- "never evict
 * this slot" and "nothing is here" -- and one value meaning both is how the
 * layer sync came to skip a real cookie. */
#define JCE_COOKIE_SLOT_EMPTY    ((uint16_t)0xFFFEu)
#define JCE_COOKIE_SLOT_RESERVED ((uint16_t)0xFFFFu)

static void cookie_sync_layers(JceLightEnv *env);

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static void ensure_light_uniforms(void)
{
    if (s_lu.init) return;

    s_lu.ambient_color = bgfx_create_uniform("u_ambientColor", BGFX_UNIFORM_TYPE_VEC4, 1);
    /* dir: 2 vec4s per light * max 2 = 4 vec4s */
    s_lu.dir_lights    = bgfx_create_uniform("u_dirLights",    BGFX_UNIFORM_TYPE_VEC4,
                                             JCE_MAX_DIR_LIGHTS * 2);
    /* point: 2 vec4s per light * max 8 = 16 vec4s */
    s_lu.point_lights  = bgfx_create_uniform("u_pointLights",  BGFX_UNIFORM_TYPE_VEC4,
                                             JCE_MAX_POINT_LIGHTS * 2);
    /* spot: 4 vec4s per light * max 4 = 16 vec4s */
    s_lu.spot_lights   = bgfx_create_uniform("u_spotLights",   BGFX_UNIFORM_TYPE_VEC4,
                                             JCE_MAX_SPOT_LIGHTS * 4);
    s_lu.light_counts  = bgfx_create_uniform("u_lightCounts",  BGFX_UNIFORM_TYPE_VEC4, 1);
    /* Its own count vec4: u_lightCounts has all four components spoken for
     * (dir / point / spot / shadow-dir slot). */
    s_lu.area_lights   = bgfx_create_uniform("u_areaLights",   BGFX_UNIFORM_TYPE_VEC4,
                                             JCE_MAX_AREA_LIGHTS * 4);
    s_lu.area_params   = bgfx_create_uniform("u_areaParams",   BGFX_UNIFORM_TYPE_VEC4, 1);
    s_lu.camera_pos    = bgfx_create_uniform("u_cameraPos",    BGFX_UNIFORM_TYPE_VEC4, 1);

    /* P3-E.5 — cookie + IES uniforms and samplers.
     * P3-E.5b — adds u_cookieDirParams + u_cookieDirVP for directional
     * light cookie projection (sampler 13 shared with the spot cookie
     * under the v1 single-bind constraint). */
    s_lu.ies_params        = bgfx_create_uniform("u_iesParams",    BGFX_UNIFORM_TYPE_VEC4, 1);
    s_lu.cookie_dir_params = bgfx_create_uniform("u_cookieDirParams", BGFX_UNIFORM_TYPE_VEC4, 1);
    s_lu.cookie_dir_vp     = bgfx_create_uniform("u_cookieDirVP",     BGFX_UNIFORM_TYPE_MAT4, 1);
    s_lu.s_cookie            = bgfx_create_uniform("s_cookie",          BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s_lu.s_ies_lut           = bgfx_create_uniform("s_iesLut",          BGFX_UNIFORM_TYPE_SAMPLER, 1);

    /* Default 1x1 white texture for safe bind even when no cookie/IES
     * is active (avoids undefined sampler reads on tight backends). */
    static const uint32_t kWhitePix = 0xFFFFFFFFu;
    const bgfx_memory_t *mem = bgfx_copy(&kWhitePix, sizeof(kWhitePix));
    s_lu.white_1x1 = bgfx_create_texture_2d(1, 1, false, 1,
                                          BGFX_TEXTURE_FORMAT_RGBA8,
                                          BGFX_TEXTURE_NONE
                                          | BGFX_SAMPLER_U_CLAMP
                                          | BGFX_SAMPLER_V_CLAMP, mem, 0);

    s_lu.init = true;

    /* ── The per-light cookie ARRAY ─────────────────────────────────
     *
     * 16 layers of 256x256, cleared white, so layer 0 (the reserved "no
     * cookie" slot) and every un-filled layer sample as 1.0 and multiply
     * radiance by nothing.
     *
     * BGFX_TEXTURE_BLIT_DST is the ONLY flag this needs.  The code that used
     * to live here said the layers could not be filled without
     * BGFX_TEXTURE_BLIT_SRC on every cookie texture, and bgfx asks for no
     * such thing -- its own assert names the destination flag and nothing
     * else.  A feature sat half-built behind a blocker that did not exist.
     *
     * Gated on BGFX_CAPS_TEXTURE_2D_ARRAY, which every backend this engine
     * targets has: the graphics tier floor is GL 3.1 / GLES 3.0 at its
     * lowest (conan/hooks/hook_bgfx_wasm_fix.py), and array textures are core
     * in both.
     *
     * THERE IS NO 2D FALLBACK ANY MORE, and removing it is part of the fix.
     * s_cookie is SAMPLER2DARRAY in every program this tree compiles, so a
     * plain 2D texture bound here is not a substitute that merely loses the
     * per-light slot -- it is a sampler TYPE mismatch, which desktop D3D and
     * GL tolerate silently and WebGL2 rejects for the whole draw (the same
     * trap the dummy cubemap in jce_scene_renderer.c exists to avoid).  When
     * the caps bit or the allocation is missing we therefore still bind an
     * ARRAY: a 1x1 white single-layer placeholder, with every slot forced to
     * 0, so cookies switch off cleanly instead of becoming a
     * backend-dependent draw failure. */
    {
        const bgfx_caps_t *caps = bgfx_get_caps();
        /* JCE_COOKIE_NO_ARRAY=1 takes the atlas away on a backend that has
         * it, which switches every cookie off.  This is the ABLATION the
         * claim needs -- the same scene with and without cookies, no scene
         * edit -- and a knob that exists only in a patch is a measurement
         * nobody can re-run. */
        const char *no_arr = getenv("JCE_COOKIE_NO_ARRAY");
        const bool forced_off = no_arr && no_arr[0] && no_arr[0] != '0';
        s_lu.cookie_array_ok =
            !forced_off &&
            caps && (caps->supported & BGFX_CAPS_TEXTURE_2D_ARRAY) != 0
                 && caps->limits.maxTextureLayers >= JCE_COOKIE_ATLAS_CAPACITY;
        if (s_lu.cookie_array_ok) {
            const uint16_t dim = JCE_COOKIE_ATLAS_DIM;
            const uint32_t bytes = (uint32_t)dim * dim * 4u
                                 * JCE_COOKIE_ATLAS_CAPACITY;
            const bgfx_memory_t *mem = bgfx_alloc(bytes);
            if (mem) {
                memset(mem->data, 0xFF, bytes);
                s_lu.cookie_atlas_array = bgfx_create_texture_2d(
                    dim, dim, false, JCE_COOKIE_ATLAS_CAPACITY,
                    BGFX_TEXTURE_FORMAT_RGBA8,
                    BGFX_TEXTURE_BLIT_DST
                    | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
                    mem, 0);
            }
            s_lu.cookie_array_ok =
                BGFX_HANDLE_IS_VALID(s_lu.cookie_atlas_array);
        }
        if (s_lu.cookie_array_ok) {
            LOG_INFO(LOG_TAG, "cookie atlas array: ready (%d layers of %dx%d)",
                     (int)JCE_COOKIE_ATLAS_CAPACITY,
                     (int)JCE_COOKIE_ATLAS_DIM, (int)JCE_COOKIE_ATLAS_DIM);
        } else {
            uint32_t white = 0xFFFFFFFFu;
            const bgfx_memory_t *wmem = bgfx_copy(&white, 4);
            s_lu.cookie_atlas_array = bgfx_create_texture_2d(
                1, 1, false, 1, BGFX_TEXTURE_FORMAT_RGBA8,
                BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, wmem, 0);
            LOG_INFO(LOG_TAG, "cookie atlas unavailable (%s); light cookies "
                              "are OFF and sampler 13 holds a 1x1 white array",
                     forced_off ? "JCE_COOKIE_NO_ARRAY" : "backend caps");
        }
    }

    /* P4-E.3b, REMOVED 2026-09-17, and the removal is the fix.
     *
     * A 16-layer 256x256 RGBA array texture was created here and bound to
     * sampler 13 in place of the real cookie -- 4 MB of VRAM on an engine
     * whose stated baseline is 512 MB, holding nothing.  Its own TODO said
     * the layers were never populated ("pending per-cookie re-import flag"),
     * and the shader side that would have sampled them is behind
     * JCE_RENDER_COOKIE_2D_ARRAY, a macro defined nowhere in the tree.
     *
     * Half a feature that merely does nothing is cheap.  This half DISABLED
     * the working one: the bind was unconditional on BGFX_CAPS_TEXTURE_2D_
     * ARRAY, i.e. every desktop backend, so the 2D atlas the shader actually
     * samples never reached sampler 13.
     *
     * The atlas SLOT bookkeeping above stays -- it is correct, it is uploaded
     * per light, and it is what a real array path would consume.  What is
     * gone is the texture with no writer.  Reviving the array path means
     * doing all three halves together: populate the layers, define the macro
     * so the SAMPLER2DARRAY declaration compiles, and bind the array.  Any
     * two of the three reproduce this defect.  The pending axis is recorded
     * in tools/lint/shader_axis_exempt.txt with that sentence. */

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
    /* EVERY slot starts EMPTY, explicitly.  A calloc'd table reads as
     * "handle 0 everywhere", and handle 0 is a real bgfx texture -- so the
     * lookup below would match the first cookie that happened to get it
     * against fifteen slots it was never put in. */
    for (int i = 0; i < (int)JCE_COOKIE_ATLAS_CAPACITY; i++) {
        env->cookie_slot_handle[i]     = JCE_COOKIE_SLOT_EMPTY;
        env->cookie_layer_uploaded[i]  = JCE_COOKIE_SLOT_EMPTY;
    }
    /* P3-E.5b — atlas slot 0 is reserved for the white "no cookie" default.
     * RESERVED and EMPTY are different questions, so they get different
     * values: this one says "never evict me", not "nothing here". */
    env->cookie_slot_handle[0]    = JCE_COOKIE_SLOT_RESERVED;
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
    env->pack_dirty        = true;
}

void jce_light_env_get_ambient(const JceLightEnv *env, jce_vec3 *color,
                               float *intensity)
{
    if (color)     *color     = env ? env->ambient_color : jce_v3(1, 1, 1);
    if (intensity) *intensity = env ? env->ambient_intensity : 0.0f;
}

bool jce_light_env_get_dir_light(const JceLightEnv *env, uint32_t index,
                                 JceDirLightDesc *out)
{
    if (!env || !out || index >= env->num_dir) return false;
    *out = env->dir_lights[index];
    return true;
}

int jce_light_env_add_dir_light(JceLightEnv *env, const JceDirLightDesc *light)
{
    if (!env || !light || env->num_dir >= JCE_MAX_DIR_LIGHTS) return -1;
    env->dir_lights[env->num_dir] = *light;
    env->pack_dirty = true;
    return (int)env->num_dir++;
}

int jce_light_env_add_point_light(JceLightEnv *env, const JcePointLightDesc *light)
{
    if (!env || !light || env->num_point >= JCE_MAX_POINT_LIGHTS) return -1;
    env->point_lights[env->num_point] = *light;
    env->pack_dirty = true;
    return (int)env->num_point++;
}

int jce_light_env_add_spot_light(JceLightEnv *env, const JceSpotLightDesc *light)
{
    if (!env || !light || env->num_spot >= JCE_MAX_SPOT_LIGHTS) return -1;
    env->spot_lights[env->num_spot] = *light;
    env->pack_dirty = true;
    return (int)env->num_spot++;
}

int jce_light_env_add_area_light(JceLightEnv *env, const JceAreaLightDesc *light)
{
    if (!env || !light || env->num_area >= JCE_MAX_AREA_LIGHTS) return -1;
    env->area_lights[env->num_area] = *light;
    env->pack_dirty = true;
    return (int)env->num_area++;
}

void jce_light_env_clear(JceLightEnv *env)
{
    if (!env) return;
    env->num_dir   = 0;
    env->num_point = 0;
    env->num_spot  = 0;
    env->num_area  = 0;
    env->pack_dirty = true;
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

uint32_t jce_light_env_area_count(const JceLightEnv *env)
{
    return env ? env->num_area : 0;
}

/* ================================================================== */
/* Apply (upload uniforms)                                             */
/* ================================================================== */

/* Rebuild the env's packed-uniform cache (see the cache comment in struct
 * JceLightEnv). Runs once per frame — or immediately after a setter dirtied
 * the env — instead of on every apply. Cookie LRU note: register_cookie
 * touches each cookie's last_used stamp here, so a once-per-frame repack
 * keeps the LRU exactly as fresh as the old per-apply behavior did. */
static void light_env_repack(JceLightEnv *env)
{
    /* Ambient: xyz = raw color, w = intensity.
       Shader computes: u_ambientColor.xyz * u_ambientColor.w * albedo * ao */
    env->packed_ambient[0] = env->ambient_color.x;
    env->packed_ambient[1] = env->ambient_color.y;
    env->packed_ambient[2] = env->ambient_color.z;
    env->packed_ambient[3] = env->ambient_intensity;

    /* Directional lights: 2 vec4s per light.
     * [i*2+0] = normalize(dir).xyz, intensity
     * [i*2+1] = color.xyz, cookie_atlas_slot  (P3-E.5b)
     *
     * cookie_atlas_slot is the slice index into the cookie atlas; 0 =
     * no cookie (white).  Used by the shader to pick the array layer
     * when sampler 13 is promoted to a 2D texture array; harmless on
     * the v1 single-bind path. */
    memset(env->packed_dir, 0, sizeof(env->packed_dir));
    for (uint32_t i = 0; i < env->num_dir; i++) {
        const JceDirLightDesc *dl = &env->dir_lights[i];
        jce_vec3 d = jce_v3_normalize(dl->direction);
        float *data = env->packed_dir + i * 8;
        data[0] = d.x;
        data[1] = d.y;
        data[2] = d.z;
        data[3] = dl->intensity;
        data[4] = dl->color.x;
        data[5] = dl->color.y;
        data[6] = dl->color.z;
        data[7] = (float)jce_light_env_register_cookie(env, dl->cookie_texture);
        /* The directional pack is 2 vec4 and its second one is
         * (color.xyz, cookie_slot); the strength has no home there, so
         * directional keeps the global u_cookieDirParams.y.  There is at most
         * one cookie-bearing directional light in any scene this engine
         * renders -- the sun -- so the global is the right shape for it, and
         * saying so is better than widening a hot uniform for a case that
         * does not occur. */
    }

    /* Point lights: 2 vec4s per light.
     * [i*2+0] = pos.xyz, radius
     * [i*2+1] = color.xyz, intensity */
    memset(env->packed_point, 0, sizeof(env->packed_point));
    for (uint32_t i = 0; i < env->num_point; i++) {
        const JcePointLightDesc *pl = &env->point_lights[i];
        float *data = env->packed_point + i * 8;
        data[0] = pl->position.x;
        data[1] = pl->position.y;
        data[2] = pl->position.z;
        data[3] = pl->radius;
        data[4] = pl->color.x;
        data[5] = pl->color.y;
        data[6] = pl->color.z;
        data[7] = pl->intensity;
    }

    /* Spot lights: 4 vec4s per light.
     * [i*4+0] = pos.xyz, radius
     * [i*4+1] = dir.xyz, intensity
     * [i*4+2] = color.xyz, innerConeCos
     * [i*4+3] = outerConeCos, cookie_atlas_slot, 0, 0  (P3-E.5b)
     *
     * cookie_atlas_slot is the slice index into the cookie atlas; 0 =
     * no cookie (white).  Same meaning as the directional channel. */
    memset(env->packed_spot, 0, sizeof(env->packed_spot));
    for (uint32_t i = 0; i < env->num_spot; i++) {
        const JceSpotLightDesc *sl = &env->spot_lights[i];
        jce_vec3 d = jce_v3_normalize(sl->direction);
        float *data = env->packed_spot + i * 16;
        data[0]  = sl->position.x;
        data[1]  = sl->position.y;
        data[2]  = sl->position.z;
        data[3]  = sl->radius;
        data[4]  = d.x;
        data[5]  = d.y;
        data[6]  = d.z;
        data[7]  = sl->intensity;
        data[8]  = sl->color.x;
        data[9]  = sl->color.y;
        data[10] = sl->color.z;
        data[11] = sl->inner_cone_cos;
        data[12] = sl->outer_cone_cos;
        data[13] = (float)jce_light_env_register_cookie(env, sl->cookie_texture);
        /* PER-LIGHT cookie strength, in what was a literal 0.  The old single
         * global strength (u_cookieParams.y) could only describe the one light
         * that won the sampler; with every light sampling its own layer, a
         * global strength would be the same bug one field over. */
        data[14] = sl->cookie_strength;
        data[15] = 0.0f;
    }

    float shadow_dir_slot = 0.0f;
    for (uint32_t i = 0; i < env->num_dir; i++) {
        if (env->dir_lights[i].casts_shadow) {
            shadow_dir_slot = (float)i + 1.0f;
            break;
        }
    }

    /* Light counts: x=numDir, y=numPoint, z=numSpot, w=shadow dir index+1. */
    env->packed_counts[0] = (float)env->num_dir;
    env->packed_counts[1] = (float)env->num_point;
    env->packed_counts[2] = (float)env->num_spot;
    env->packed_counts[3] = shadow_dir_slot;

    /* Area lights.  The basis is orthonormalised HERE and not at the call
     * site: a light authored on a transform arrives with whatever the scale
     * and a non-uniform parent left behind, and a skewed basis makes the
     * shader's cross(normal, right) a non-perpendicular `up` -- a rectangle
     * that is quietly a parallelogram. */
    env->packed_area_params[0] = (float)env->num_area;
    /* .y is the RECEIVER's rendering layer, filled in per draw by
     * jce_light_env_apply_for_layer -- the clustered path needs it to test
     * the per-light mask that now rides in each cluster light's L3 texel.
     * Parked here rather than in a new uniform because this vec4 already has
     * three spare lanes and is already uploaded on the per-draw replay range;
     * a new uniform would be a second upload for one float. */
    env->packed_area_params[1] = 0.0f;
    env->packed_area_params[2] = 0.0f;
    env->packed_area_params[3] = 0.0f;
    for (uint32_t i = 0; i < env->num_area; i++) {
        const JceAreaLightDesc *a = &env->area_lights[i];
        float *o = &env->packed_area[i * 16];

        jce_vec3 n = jce_v3_normalize(a->normal);
        jce_vec3 r = a->right;
        /* Gram-Schmidt: drop the component of `right` along the normal, then
         * renormalise.  A `right` that is parallel to the normal (or zero)
         * carries no direction at all, so pick any perpendicular. */
        r = jce_v3_sub(r, jce_v3_scale(n, jce_v3_dot(n, r)));
        if (jce_v3_len(r) < 1e-5f) {
            jce_vec3 alt = (fabsf(n.y) < 0.9f) ? jce_v3(0.0f, 1.0f, 0.0f)
                                               : jce_v3(1.0f, 0.0f, 0.0f);
            r = jce_v3_cross(alt, n);
        }
        r = jce_v3_normalize(r);

        o[0] = a->position.x; o[1] = a->position.y; o[2] = a->position.z;
        o[3] = a->width  * 0.5f;
        o[4] = n.x; o[5] = n.y; o[6] = n.z;
        o[7] = a->height * 0.5f;
        o[8]  = a->color.x * a->intensity;
        o[9]  = a->color.y * a->intensity;
        o[10] = a->color.z * a->intensity;
        o[11] = a->radius;
        o[12] = r.x; o[13] = r.y; o[14] = r.z;
        o[15] = a->two_sided ? 1.0f : 0.0f;
    }

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
        int      ies_spot    = -1;
        int      cookie_dir  = -1;
        float    cookie_strength_dir  = 1.0f;
        bgfx_texture_handle_t ies_tex = s_lu.white_1x1;

        /* No "which spot light owns the cookie sampler" scan any more: all
         * of them do.  The atlas layer each one samples is registered while
         * its pack is built (jce_light_env_register_cookie), so nothing here
         * has to choose a winner. */
        for (uint32_t i = 0; i < env->num_spot; i++) {
            const JceSpotLightDesc *sl = &env->spot_lights[i];
            if (ies_spot < 0 && jce_texture_valid(sl->ies_lut_texture)) {
                ies_spot = (int)i;
                ies_tex.idx = sl->ies_lut_texture.idx;
                break;      /* sampler 14 carries exactly one LUT */
            }
        }
        for (uint32_t i = 0; i < env->num_dir; i++) {
            const JceDirLightDesc *dl = &env->dir_lights[i];
            if (cookie_dir < 0 && jce_texture_valid(dl->cookie_texture)) {
                cookie_dir = (int)i;
                cookie_strength_dir = clamp01(dl->cookie_strength);
            }
        }

        /* THE SPOT COOKIE VP IS GONE, and its absence is the fix.
         *
         * It was ONE mat4, built from whichever light won the single sampler
         * bind and uploaded as u_cookieSpotVP for every spot light to project
         * through.  That is what kept per-light cookies impossible after the
         * atlas, the layers, the per-light slot and the per-light strength
         * were all already correct: the second light projected through the
         * FIRST light's frustum, landed outside [0,1] and was masked to
         * white -- indistinguishable from a light with no cookie at all.
         * fs_pbr_main.sh now derives each light's frustum from its own
         * position, direction and cone, which are already in its pack, so
         * per-light projection costs neither a uniform nor an upload. */

        /* Directional cookie VP (P3-E.5b) --------------------------- */
        /* World-aligned ortho box centred on camera xz, viewed along
         * the chosen directional light's forward.  Half-extent is
         * fixed (50m) for v1 stability — no per-frame jitter, no CSM
         * coupling.  Slide-to-v2: re-use cascade 0's tight frustum
         * fit from jce_csm_compute when the lighting system gains
         * camera-frustum awareness. */
        float *dir_vp = env->packed_dir_vp;
        memset(dir_vp, 0, sizeof(env->packed_dir_vp));
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
            memcpy(dir_vp, JCE_M4_PTR(vp4), sizeof(env->packed_dir_vp));
        }

        /* Param uniforms -------------------------------------------- */
        /* IES ONLY.  Every cookie component of this vec4 described the one
         * light the single bind selected, and each of them now lives in that
         * light's own pack instead: has-cookie and slot in .y, strength in
         * .z of u_spotLights[i*4+3].  What is left is the IES profile, which
         * genuinely is one per frame because sampler 14 carries one LUT.
         *
         * .y is ies_spot, and it used to be
         * `cookie_spot >= 0 ? cookie_spot : ies_spot` -- so a scene with a
         * cookie on one light and an IES profile on another applied the IES
         * curve to the light holding the COOKIE.  One of the two was always
         * wrong, and only a scene using both at once could show it. */
        env->packed_ies_params[0] = (ies_spot >= 0) ? 1.0f : 0.0f;
        env->packed_ies_params[1] = (ies_spot >= 0) ? (float)ies_spot : 0.0f;
        env->packed_ies_params[2] = 0.0f;   /* a vec4 is the smallest uniform */
        env->packed_ies_params[3] = 0.0f;   /* this API has; both are unread */

        env->packed_cookie_dir_params[0] = (cookie_dir >= 0) ? 1.0f : 0.0f;
        env->packed_cookie_dir_params[1] = cookie_strength_dir;
        env->packed_cookie_dir_params[2] = (cookie_dir >= 0) ? (float)cookie_dir
                                                             : 0.0f;
        env->packed_cookie_dir_params[3] = 0.0f;

        env->packed_ies_tex         = ies_tex;
        env->packed_has_dir_cookie  = (cookie_dir >= 0);
        env->packed_cam_pos         = env->camera_pos;
    }

    /* Rendering layers.  Recomputed here rather than maintained by the adders
     * because the env is rebuilt from the scene every frame anyway, and a flag
     * that is only ever set would stay true for the rest of the process after
     * one masked light was removed. */
    {
        uint32_t any = 0u;
        for (uint32_t i = 0; i < env->num_dir;   i++) any |= env->dir_lights[i].layer_mask;
        for (uint32_t i = 0; i < env->num_point; i++) any |= env->point_lights[i].layer_mask;
        for (uint32_t i = 0; i < env->num_spot;  i++) any |= env->spot_lights[i].layer_mask;
        for (uint32_t i = 0; i < env->num_area;  i++) any |= env->area_lights[i].layer_mask;
        env->any_layer_mask = (any != 0u);
    }
    env->masked_valid = false;
}

/* Is this light allowed to reach a receiver on `layer`?
 *
 * 0 = every layer, for the reason spelled out in the header: every desc that
 * predates the field arrives zeroed, and spending 0 on "lights nothing" would
 * black out every scene saved before today. */
static bool light_lights_layer(uint32_t mask, uint32_t layer)
{
    return mask == 0u || (mask & (1u << (layer & 31u))) != 0u;
}

/* Build masked_* from packed_* for one receiver layer.
 *
 * ZEROES THE INTENSITY, does not compact the array.  jce_sr_shadow.c addresses
 * a local light's shadow-atlas slot by its index in these very arrays, so
 * dropping light 1 would hand light 2 light 1's shadow -- a masked lamp would
 * disappear and take an unrelated lamp's shadow with it. */
static void light_env_build_masked(JceLightEnv *env, uint32_t layer)
{
    memcpy(env->masked_dir,   env->packed_dir,   sizeof(env->masked_dir));
    memcpy(env->masked_point, env->packed_point, sizeof(env->masked_point));
    memcpy(env->masked_spot,  env->packed_spot,  sizeof(env->masked_spot));
    memcpy(env->masked_area,  env->packed_area,  sizeof(env->masked_area));

    /* Offsets mirror light_env_repack above; a change there without a change
     * here would zero a colour channel instead of an intensity. */
    for (uint32_t i = 0; i < env->num_dir; i++)
        if (!light_lights_layer(env->dir_lights[i].layer_mask, layer))
            env->masked_dir[i * 8 + 3] = 0.0f;          /* dir.xyz, INTENSITY */
    for (uint32_t i = 0; i < env->num_point; i++)
        if (!light_lights_layer(env->point_lights[i].layer_mask, layer))
            env->masked_point[i * 8 + 7] = 0.0f;        /* color.xyz, INTENSITY */
    for (uint32_t i = 0; i < env->num_spot; i++)
        if (!light_lights_layer(env->spot_lights[i].layer_mask, layer))
            env->masked_spot[i * 16 + 7] = 0.0f;        /* dir.xyz, INTENSITY */
    /* Area lights premultiply intensity into the colour, so the three colour
     * channels ARE the intensity here -- there is no separate lane to zero. */
    for (uint32_t i = 0; i < env->num_area; i++)
        if (!light_lights_layer(env->area_lights[i].layer_mask, layer)) {
            env->masked_area[i * 16 + 8]  = 0.0f;
            env->masked_area[i * 16 + 9]  = 0.0f;
            env->masked_area[i * 16 + 10] = 0.0f;
        }

    env->masked_layer = layer & 31u;
    env->masked_valid = true;
}

/* Bring the packed cache up to date for this frame.  Split out of
 * jce_light_env_apply so the per-layer entry point below runs the SAME
 * freshness rule rather than a second copy of it that could drift. */
static void light_env_ensure_pack(JceLightEnv *mut, const JceRenderer *r)
{
    ensure_light_uniforms();

    uint32_t frame  = r ? jce_renderer_get_frame_index(r) : 0u;
    bool     repack = mut->pack_dirty || !mut->pack_valid ||
                      (r && mut->pack_frame != frame);

    /* The dir-cookie VP is the only packed value derived from the camera;
       set_camera_pos intentionally does not dirty the pack (it changes
       every frame), so catch camera motion here. */
    if (!repack && mut->packed_has_dir_cookie &&
        (mut->packed_cam_pos.x != mut->camera_pos.x ||
         mut->packed_cam_pos.y != mut->camera_pos.y ||
         mut->packed_cam_pos.z != mut->camera_pos.z))
        repack = true;

    if (repack) {
        light_env_repack(mut);
        mut->pack_dirty = false;
        mut->pack_valid = true;
        mut->pack_frame = frame;
    }
}

/* The upload itself, over WHICHEVER pack the caller chose -- the shared one or
 * the per-receiver-layer filtered one.  Everything outside the four light
 * arrays is identical either way, which is exactly why this is one function:
 * a masked draw that missed a cookie param would light differently for a
 * reason that has nothing to do with layers. */
static void light_env_upload(JceLightEnv *mut,
                             const float *dir, const float *pt,
                             const float *sp,  const float *ar)
{
    const JceLightEnv *env = mut;
    /* Upload on EVERY apply — this is not cacheable: bgfx draw items record
       only the uniform updates issued since the last state-discarding submit
       and replay them in view-SORTED draw order, so each material run's
       first draw must carry the full light state in its own update range
       (skipping would leave draws inheriting another material's values).
       The light arrays upload only their used lanes: the shader loops break
       at u_lightCounts, lanes past the count are never read, so stale tail
       data in backend shadow storage is harmless. */
    jce_enc_set_uniform(s_lu.ambient_color, mut->packed_ambient, 1);
    if (env->num_dir > 0)
        jce_enc_set_uniform(s_lu.dir_lights, dir,
                         (uint16_t)(env->num_dir * 2));
    if (env->num_point > 0)
        jce_enc_set_uniform(s_lu.point_lights, pt,
                         (uint16_t)(env->num_point * 2));
    if (env->num_spot > 0)
        jce_enc_set_uniform(s_lu.spot_lights, sp,
                         (uint16_t)(env->num_spot * 4));
    jce_enc_set_uniform(s_lu.light_counts, mut->packed_counts, 1);
    /* u_areaParams uploads UNCONDITIONALLY -- the shader reads .x to decide
     * whether to enter the loop at all, so a scene with no area lights must
     * still be told zero rather than inherit another material run's count. */
    jce_enc_set_uniform(s_lu.area_params, mut->packed_area_params, 1);
    if (env->num_area > 0)
        jce_enc_set_uniform(s_lu.area_lights, ar,
                         (uint16_t)(env->num_area * 4));

    /* Camera position for PBR specular — read live, not from the pack
       (set_camera_pos does not dirty the pack). */
    float cam[4] = { env->camera_pos.x, env->camera_pos.y,
                     env->camera_pos.z, 0.0f };
    jce_enc_set_uniform(s_lu.camera_pos, cam, 1);

    /* The DIRECTIONAL cookie VP is still one matrix, and legitimately so:
     * it is an ortho box along one light's forward, and u_cookieDirParams.x
     * gates the whole block, so skipping the upload when no directional
     * cookie is active is byte-identical.  The SPOT VP that used to sit
     * beside it is gone -- see light_env_repack.  The params vec4s always
     * upload; the shader reads .x to gate. */
    if (mut->packed_cookie_dir_params[0] > 0.5f)
        jce_enc_set_uniform(s_lu.cookie_dir_vp,  mut->packed_dir_vp, 1);
    jce_enc_set_uniform(s_lu.ies_params,        mut->packed_ies_params, 1);
    jce_enc_set_uniform(s_lu.cookie_dir_params, mut->packed_cookie_dir_params, 1);

    /* THE ARRAY, ALWAYS -- there is no other kind of texture this sampler
     * can legally hold.  Each light's layer rides in u_dirLights[i*2+1].w /
     * u_spotLights[i*4+3].y, so one bind serves every cookie-bearing light;
     * a single 2D bind could only ever carry one image, which is what made
     * "every light has its own cookie" untestable before.
     *
     * fs_pbr_decl.sh declares s_cookie to match, unconditionally: bgfx
     * rewrites the shader's #version to 140 on this engine's GL floor (3.1,
     * the lowest of the three graphics tiers) and emits
     * `#define texture2DArray texture`, so profile-120 source compiles to
     * valid GLSL 1.40 on every backend here.  That is why this needs no
     * shader permutation -- checked in bgfx's renderer_gl.cpp, not assumed. */
    cookie_sync_layers(mut);
    jce_enc_set_texture(13, s_lu.s_cookie, s_lu.cookie_atlas_array,
                        UINT32_MAX);
    jce_enc_set_texture(14, s_lu.s_ies_lut, mut->packed_ies_tex, UINT32_MAX);
}

void jce_light_env_apply(const JceLightEnv *env, const JceRenderer *r)
{
    if (!env) return;

    /* The public API stays logically const: the pack cache and the cookie
       LRU are internal mutable state (same precedent as the old in-place
       register_cookie cast). */
    JceLightEnv *mut = (JceLightEnv *)env;

    light_env_ensure_pack(mut, r);
    light_env_upload(mut, mut->packed_dir, mut->packed_point,
                     mut->packed_spot, mut->packed_area);
}

/* Does any light here restrict itself to layers?
 *
 * This used to decide whether Forward+ could run at all.  It no longer does --
 * the clustered path honours masks now -- so its remaining job is to let a
 * TOOL say that layers are live in this scene, which is the first thing a
 * masked key light looks like a bug for. */
bool jce_light_env_has_layer_masks(const JceLightEnv *env)
{
    if (!env) return false;
    /* Read it out of the pack, which means packing first: the flag is derived
       at repack, and a caller that asks before the first apply of the frame
       would otherwise get LAST frame's answer -- and this answer decides
       whether Forward+ runs, so a stale one is a frame lit the wrong way. */
    JceLightEnv *mut = (JceLightEnv *)env;
    light_env_ensure_pack(mut, NULL);
    return mut->any_layer_mask;
}

void jce_light_env_apply_for_layer(const JceLightEnv *env, const JceRenderer *r,
                                   uint32_t layer_index)
{
    if (!env) return;
    JceLightEnv *mut = (JceLightEnv *)env;

    light_env_ensure_pack(mut, r);

    layer_index &= 31u;
    /* BEFORE the fast path returns, not after.  The clustered shader tests
     * the receiver's layer against a mask that travels inside s_cluster, and
     * the cluster carries masks whether or not these CPU-side packs do -- so
     * a frame with no masked light still has to say which layer is receiving,
     * or the clustered path reads whatever was in this lane last. */
    mut->packed_area_params[1] = (float)layer_index;

    /* No mask anywhere in this env: this IS the unfiltered path.  Not an
       optimisation -- it is the guarantee that a project which never touches
       rendering layers uploads exactly the bytes it uploaded before the
       feature existed. */
    if (!mut->any_layer_mask) {
        light_env_upload(mut, mut->packed_dir, mut->packed_point,
                         mut->packed_spot, mut->packed_area);
        return;
    }

    if (!mut->masked_valid || mut->masked_layer != layer_index)
        light_env_build_masked(mut, layer_index);

    light_env_upload(mut, mut->masked_dir, mut->masked_point,
                     mut->masked_spot, mut->masked_area);
}

void jce_light_env_set_camera_pos(JceLightEnv *env, jce_vec3 pos)
{
    if (env) env->camera_pos = pos;
}

/* ================================================================== */
/* P3-E.5b — Cookie atlas (LRU)                                        */
/* ================================================================== */

/* Copy every slot's cookie into its array LAYER, once per (slot, texture).
 *
 * ON A VIEW BELOW EVERY SCENE BASE, because bgfx orders blits by view id just
 * as it orders draws: a blit on a higher id lands AFTER the colour pass that
 * samples the layer, and the first frame with a new cookie would show the
 * previous one -- a stale projector nobody could attribute.  It shares
 * JCE_VIEW_MORPH_DEFORM, and the sharing is argued rather than assumed: both
 * are PRODUCERS that write textures the colour pass reads, neither reads the
 * other's output, and neither uses any view STATE (a dispatch carries its own
 * bindings, a blit carries src/dst), which is the state that makes two
 * subsystems on one view id normally destroy each other.
 *
 * The comparison is against cookie_layer_uploaded, NOT cookie_slot_handle: the
 * slot assignment is decided when a light registers and the layer is filled on
 * a frame the renderer runs, so the two are answers to different questions and
 * conflating them would either blit every frame or never blit again after an
 * eviction reused a slot. */
static void cookie_sync_layers(JceLightEnv *env)
{
    if (!env || !s_lu.cookie_array_ok) return;
    for (int i = 1; i < (int)JCE_COOKIE_ATLAS_CAPACITY; i++) {
        const uint16_t want = env->cookie_slot_handle[i];
        if (want == JCE_COOKIE_SLOT_EMPTY || want == JCE_COOKIE_SLOT_RESERVED)
            continue;
        if (env->cookie_layer_uploaded[i] == want) continue;

        bgfx_texture_handle_t src = { want };
        bgfx_blit(JCE_VIEW_MORPH_DEFORM,
                  s_lu.cookie_atlas_array, 0,
                  0, 0, (uint16_t)i,          /* dstZ = the layer */
                  src, 0, 0, 0, 0,
                  JCE_COOKIE_ATLAS_DIM, JCE_COOKIE_ATLAS_DIM, 1);
        env->cookie_layer_uploaded[i] = want;
        LOG_DEBUG(LOG_TAG, "cookie layer %d <- texture %u", i, (unsigned)want);
    }
}

int jce_light_env_register_cookie(JceLightEnv *env, JceTexture cookie)
{
    /* No atlas means no slot, for EVERY light.  Slot 0 is the reserved
     * white layer, so answering 0 is what switches the shader's per-light
     * gate off; without it the lights would gate on a layer the 1x1
     * placeholder does not have. */
    if (!env || !s_lu.cookie_array_ok || !jce_texture_valid(cookie)) return 0;

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
        if (env->cookie_slot_handle[i] == JCE_COOKIE_SLOT_EMPTY) {
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

/* ── Per-pixel light budget (see jce_lighting_system.h) ──────────────
 * Process-global for the same reason jce_texture_set_quality_mip_bias is: a
 * shipped game applies it in app_init, before any scene renderer exists. */
static int s_pixel_light_count = 0;   /* <= 0 == no budget */

void jce_lighting_set_pixel_light_count(int count)
{
    s_pixel_light_count = count > 0 ? count : 0;
}

int jce_lighting_get_pixel_light_count(void) { return s_pixel_light_count; }
