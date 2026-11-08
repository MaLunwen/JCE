/* Private per-device cookie texture owner. Include after jce_render_encoder.h.
 * bgfx owns a Memory as soon as create_texture_2d is called, even on failure. */
#ifndef JCE_LIGHT_COOKIE_INTERNAL_H
#define JCE_LIGHT_COOKIE_INTERNAL_H

#include <bgfx/c99/bgfx.h>
#include <jce/renderer/jce_lighting_system.h>
#include <string.h>

typedef enum JceLightCookieReason {
    JCE_LIGHT_COOKIE_READY,
    JCE_LIGHT_COOKIE_COMPATIBILITY,
    JCE_LIGHT_COOKIE_ARRAY_UNAVAILABLE,
    JCE_LIGHT_COOKIE_FORCED_OFF,
    JCE_LIGHT_COOKIE_ATLAS_UNAVAILABLE,
    JCE_LIGHT_COOKIE_ATLAS_ALLOC_FAILED,
    JCE_LIGHT_COOKIE_ATLAS_CREATE_FAILED,
    JCE_LIGHT_COOKIE_WHITE_ALLOC_FAILED,
    JCE_LIGHT_COOKIE_WHITE_CREATE_FAILED
} JceLightCookieReason;

typedef struct JceLightCookieTexture {
    bgfx_texture_handle_t texture;
    bool atlas_enabled;
    JceLightCookieReason reason;
} JceLightCookieTexture;

static inline JceLightCookieTexture jce_light_cookie_texture_create(
    const bgfx_caps_t *caps, bool compatibility, bool forced_off)
{
    JceLightCookieTexture out = {{UINT16_MAX}, false,
        JCE_LIGHT_COOKIE_ARRAY_UNAVAILABLE};
    if (compatibility) {
        out.reason = JCE_LIGHT_COOKIE_COMPATIBILITY;
        return out;
    }
    if (!caps || !(caps->supported & BGFX_CAPS_TEXTURE_2D_ARRAY) ||
        caps->limits.maxTextureLayers < 2)
        return out;

    out.reason = forced_off ? JCE_LIGHT_COOKIE_FORCED_OFF
                            : JCE_LIGHT_COOKIE_ATLAS_UNAVAILABLE;
    if (!forced_off && caps->limits.maxTextureLayers >= JCE_COOKIE_ATLAS_CAPACITY &&
        (caps->supported & BGFX_CAPS_TEXTURE_BLIT)) {
        const uint32_t bytes = (uint32_t)JCE_COOKIE_ATLAS_DIM *
            JCE_COOKIE_ATLAS_DIM * 4u * JCE_COOKIE_ATLAS_CAPACITY;
        const bgfx_memory_t *mem = bgfx_alloc(bytes);
        out.reason = JCE_LIGHT_COOKIE_ATLAS_ALLOC_FAILED;
        if (mem) {
            memset(mem->data, 0xff, bytes);
            out.texture = bgfx_create_texture_2d(JCE_COOKIE_ATLAS_DIM,
                JCE_COOKIE_ATLAS_DIM, false, JCE_COOKIE_ATLAS_CAPACITY,
                BGFX_TEXTURE_FORMAT_RGBA8, BGFX_TEXTURE_BLIT_DST |
                BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, mem, 0);
            out.reason = JCE_LIGHT_COOKIE_ATLAS_CREATE_FAILED;
            if (BGFX_HANDLE_IS_VALID(out.texture)) {
                out.atlas_enabled = true;
                out.reason = JCE_LIGHT_COOKIE_READY;
                return out;
            }
        }
    }

    /* One layer is a 2D texture in bgfx, so the legal array placeholder
     * contains two initialized white layers. No atlas slot can address it. */
    const uint32_t white[2] = {UINT32_MAX, UINT32_MAX};
    const bgfx_memory_t *mem = bgfx_copy(white, sizeof(white));
    if (!mem) {
        out.reason = JCE_LIGHT_COOKIE_WHITE_ALLOC_FAILED;
        return out;
    }
    out.texture = bgfx_create_texture_2d(1, 1, false, 2,
        BGFX_TEXTURE_FORMAT_RGBA8, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        mem, 0);
    if (!BGFX_HANDLE_IS_VALID(out.texture))
        out.reason = JCE_LIGHT_COOKIE_WHITE_CREATE_FAILED;
    return out;
}

static inline void jce_light_cookie_texture_bind(
    const JceLightCookieTexture *owner, bgfx_uniform_handle_t sampler)
{
    if (owner && BGFX_HANDLE_IS_VALID(owner->texture) &&
        BGFX_HANDLE_IS_VALID(sampler))
        jce_enc_set_texture(13, sampler, owner->texture, UINT32_MAX);
}

static inline void jce_light_cookie_texture_destroy(JceLightCookieTexture *owner)
{
    if (!owner) return;
    const bgfx_texture_handle_t texture = owner->texture;
    owner->texture.idx = UINT16_MAX;
    owner->atlas_enabled = false;
    if (BGFX_HANDLE_IS_VALID(texture)) bgfx_destroy_texture(texture);
}

static inline const char *jce_light_cookie_texture_reason(JceLightCookieReason reason)
{
    switch (reason) {
    case JCE_LIGHT_COOKIE_READY: return "atlas ready";
    case JCE_LIGHT_COOKIE_COMPATIBILITY: return "compatibility GL/ES shader profile";
    case JCE_LIGHT_COOKIE_ARRAY_UNAVAILABLE: return "array textures unavailable";
    case JCE_LIGHT_COOKIE_FORCED_OFF: return "JCE_COOKIE_NO_ARRAY";
    case JCE_LIGHT_COOKIE_ATLAS_UNAVAILABLE: return "atlas layer/blit capability unavailable";
    case JCE_LIGHT_COOKIE_ATLAS_ALLOC_FAILED: return "atlas allocation failed";
    case JCE_LIGHT_COOKIE_ATLAS_CREATE_FAILED: return "atlas creation failed";
    case JCE_LIGHT_COOKIE_WHITE_ALLOC_FAILED: return "white array allocation failed";
    case JCE_LIGHT_COOKIE_WHITE_CREATE_FAILED: return "white array creation failed";
    default: return "unknown cookie texture failure";
    }
}

#endif
