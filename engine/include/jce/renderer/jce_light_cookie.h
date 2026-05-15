/*
 * jce_light_cookie.h  Light cookie / projected mask textures.
 *
 * Unity's "Cookie" feature: every light can carry a 2D (spot/directional)
 * or cubemap (point) mask texture that's projected through the light's
 * frustum / sphere.  Without engine support the most a user can do is
 * fake cookies via decals — this header binds cookie info to a light
 * id so the renderer's forward pass can sample.
 *
 * Data layer only — actual sampling is wired in B17.7 alongside the
 * lighting system uniforms.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_LIGHT_COOKIE_H
#define JCE_LIGHT_COOKIE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_LIGHT_COOKIE_MAX 64
#define JCE_LIGHT_COOKIE_PATH_LEN 128

typedef enum {
    JCE_LIGHT_COOKIE_NONE       = 0,
    JCE_LIGHT_COOKIE_2D         = 1,   /* spot / directional */
    JCE_LIGHT_COOKIE_CUBE       = 2,   /* point light */
} JceLightCookieKind;

typedef struct {
    /* Owning light entity id — externally meaningful key.  0 means
     * the slot is unused. */
    uint32_t           light_id;
    JceLightCookieKind kind;
    char               texture_path[JCE_LIGHT_COOKIE_PATH_LEN];
    /* Loaded texture handle (set by the renderer once the asset is
     * resolved).  UINT32_MAX = not yet loaded. */
    uint32_t           texture_handle;
    /* UV transform for spot / directional cookies — Unity exposes
     * `Light.cookieSize`; we expose a full 2x2 + offset for flexibility. */
    float              uv_scale[2];
    float              uv_offset[2];
    /* Tint colour applied multiplicatively to the cookie sample. */
    float              tint_rgba[4];
    bool               active;
} JceLightCookieEntry;

/* Register or update a cookie binding for `light_id`.  Returns true
 * on success / false on full registry. */
JCE_API bool jce_light_cookie_set(uint32_t light_id,
                                   JceLightCookieKind kind,
                                   const char *texture_path,
                                   const float uv_scale[2],
                                   const float uv_offset[2],
                                   const float tint_rgba[4]);

/* Strip cookie info for `light_id` (returns true if a binding was
 * removed). */
JCE_API bool jce_light_cookie_clear(uint32_t light_id);

JCE_API const JceLightCookieEntry *jce_light_cookie_get(uint32_t light_id);

/* Iterate every active cookie (renderer use). */
JCE_API uint32_t jce_light_cookie_count(void);
JCE_API const JceLightCookieEntry *jce_light_cookie_at(uint32_t index);

JCE_EXTERN_C_END

#endif /* JCE_LIGHT_COOKIE_H */
