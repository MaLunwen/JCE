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
#include <string.h>   /* memset, for the desc initialisers below */

JCE_EXTERN_C_BEGIN

typedef struct JceRenderer JceRenderer;
typedef struct JceLightEnv JceLightEnv;

/* ================================================================== */
/* Limits                                                              */
/* ================================================================== */

#define JCE_MAX_DIR_LIGHTS   2
#define JCE_MAX_POINT_LIGHTS 16   /* raised from 8: scenes (e.g. graveyard) have >8 point lights; the extra were silently dropped -> "point light has no light" */
#define JCE_MAX_SPOT_LIGHTS  4
/* Four, matching the spot budget.  Each costs four vec4 of uniform and a
 * solid-angle integral per fragment, which is why this is not sixteen. */
#define JCE_MAX_AREA_LIGHTS  4

/* ── Per-pixel light budget (Unity's QualitySettings.pixelLightCount) ──
 *
 * The caps above are COMPILE-TIME ceilings: 16 point + 4 spot is what the
 * shader's uniform arrays hold.  This is the RUNTIME budget a quality level
 * sets under them, and Project Settings > Quality has carried a
 * `pixel_light_count` field per level for as long as the tab has existed with
 * nothing to hand it to -- no engine call accepted it and the effective-render-
 * settings fold never exported it, so a designer capping lights for a low-end
 * target changed a number in a file and nothing else.
 *
 * <= 0 means "no budget", which is the default and is EXACTLY the old
 * behaviour: the per-kind top-N selection already fills each array to its own
 * ceiling by score, and an unlimited budget reproduces that selection
 * light-for-light.
 *
 * The budget is COMBINED across point and spot, which is what Unity counts,
 * and it is spent highest-score-first -- so dropping the budget to 4 keeps the
 * four lights that matter most to this camera rather than the first four the
 * scene happens to declare.  The directional sun is not counted: it is one
 * light, always on, and is not what a low-end budget is trying to bound.
 *
 * Process-global, matching jce_texture_set_quality_mip_bias -- the other
 * quality knob a shipped game applies before any renderer exists. */
JCE_API void JCE_CALL jce_lighting_set_pixel_light_count(int count);
JCE_API int  JCE_CALL jce_lighting_get_pixel_light_count(void);

/* P3-E.5b — Multi-cookie atlas (texture array) capacity.
 * Slot 0 is reserved for a 1x1 white default ("no cookie"); slots
 * 1..(capacity-1) hold registered cookie images.  LRU eviction kicks
 * in when more unique cookies than the cap are requested in a frame. */
#define JCE_COOKIE_ATLAS_CAPACITY 16

/* Edge length of one cookie atlas LAYER.  Every registered cookie is blitted
 * into a layer of this size; a source that is larger is clipped by the blit
 * rather than scaled, which is the honest behaviour for a projector mask --
 * a silently resampled cookie would soften edges the author sharpened. */
#define JCE_COOKIE_ATLAS_DIM 256

/* ================================================================== */
/* Light descriptors                                                   */
/* ================================================================== */

/* ── Rendering layers: which objects a light is allowed to reach ─────
 *
 * Unity's Rendering Layers, UE's Lighting Channels, Godot's
 * Light3D.light_cull_mask.  Bit N set = this light lights layer N, where the
 * layer of a RECEIVER is JceLayerComponent.layer (0..31) -- the same 32 slots
 * JceCameraComponent.culling_mask already filters on.
 *
 * ZERO IS "EVERY LAYER", not "no layer".  Every struct here is
 * zero-initialised by callers that predate the field and by every scene saved
 * before it existed, so spending 0 on "lights nothing" would turn those scenes
 * black.  Same convention, same reason, as culling_mask.
 *
 * WHAT IT REACHES, exactly: the DIRECT term of this light.  A masked light is
 * uploaded with intensity 0 for that receiver, so its diffuse, its specular
 * and its shadow all vanish together.  It does NOT remove the light from
 * ambient, from a baked probe, or from the volumetric fog -- those are
 * per-frame quantities with no receiver to be masked against, and Unity's
 * layers do not reach its GI either.
 *
 * IT MASKS THE RECEIVER, NOT THE CASTER.  An object on a layer this light
 * does not reach still blocks it, so it still appears in the shadow this light
 * casts onto objects that ARE reached.  That is URP's behaviour with one mask
 * (HDRP adds a second, shadow-only one); it is stated here because it is the
 * first thing a masked key light looks like a bug for.
 *
 * IT WORKS UNDER FORWARD AND FORWARD+ ALIKE, and that took a second
 * mechanism.  The clustered path reads its point and spot parameters from the
 * cluster texture, which is built ONCE PER FRAME with no receiver in sight --
 * so a CPU-side mask cannot reach them, and for a while Forward+ was simply
 * turned off for any frame carrying a mask.  That was correct and expensive:
 * authoring one light layer cost the whole clustered path, which is a
 * performance cliff on exactly the hardware clustered lighting exists for.
 *
 * Now the mask travels WITH each cluster light, packed into the two spare
 * lanes of its L3 texel as a pair of 16-bit halves (a 32-bit mask does not
 * survive a float -- 2^31 needs a 32-bit mantissa and there are 24), and the
 * receiver's layer arrives in u_areaParams.y on the per-draw upload the
 * brute-force masks already use.  The test is mod(floor(m / exp2(b)), 2),
 * which is the only form that survives the GLSL 1.20 floor the OpenGL backend
 * compiles at -- it has no integer bitwise operators at all. */

typedef struct {
    jce_vec3 direction;       /* normalized direction light travels */
    jce_vec3 color;           /* linear RGB */
    float    intensity;       /* multiplier, default 1.0 */
    bool     casts_shadow;    /* enable shadow map for this light */
    /* P3-E.5 — Optional directional light cookie (projector mask). */
    JceTexture cookie_texture; /* JCE_TEXTURE_INVALID = disabled */
    float      cookie_strength;
    /* Rendering layers -- see the block above.  0 = every layer.  APPENDED. */
    uint32_t   layer_mask;
} JceDirLightDesc;

typedef struct {
    jce_vec3 position;
    jce_vec3 color;           /* linear RGB */
    float    intensity;
    float    radius;          /* attenuation cutoff distance */
    bool     casts_shadow;    /* P1 — enable local (atlas) shadow map */
    float    shadow_bias;     /* depth bias; 0 = engine default */
    /* Rendering layers -- see the block above.  0 = every layer.  APPENDED. */
    uint32_t layer_mask;
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
    /* Rendering layers -- see the block above.  0 = every layer.  APPENDED. */
    uint32_t   layer_mask;
} JceSpotLightDesc;

/* ZERO IS NOT "NO TEXTURE", so these are how a desc starts.
 *
 * `memset(&desc, 0, sizeof desc)` leaves cookie_texture.idx == 0, and bgfx
 * handle 0 is a perfectly valid texture -- somebody else's.  A desc built
 * that way therefore claims a cookie, wins an atlas slot, and gets that
 * stranger's image blitted into a layer; measured on the time-of-day sun,
 * which never set the field and never meant to have one.  Leaving the desc
 * uninitialised is the same trap with a garbage handle instead of 0.
 *
 * These set every field a light can have to the value that means "not
 * asked for", so a caller only has to fill in what it actually wants. */
JCE_INLINE JceDirLightDesc jce_dir_light_desc_default(void)
{
    JceDirLightDesc d;
    memset(&d, 0, sizeof d);
    d.intensity       = 1.0f;
    d.cookie_texture.idx  = UINT16_MAX;   /* the macro is a C99 compound
                                           * literal; this header is also
                                           * included from C++ */
    return d;
}

JCE_INLINE JceSpotLightDesc jce_spot_light_desc_default(void)
{
    JceSpotLightDesc d;
    memset(&d, 0, sizeof d);
    d.intensity       = 1.0f;
    d.cookie_texture.idx  = UINT16_MAX;   /* see above */
    d.ies_lut_texture.idx = UINT16_MAX;
    return d;
}

/* A RECTANGULAR AREA LIGHT -- Unity's Rectangle light, UE's Rect Light.
 *
 * The thing a point or a spot cannot express: a highlight shaped like the
 * emitter and a terminator softened by the emitter's SIZE rather than by a
 * cone angle.  A softbox, a window, a strip light, a screen.
 *
 * The basis must be orthonormal: `normal` is the direction it emits and
 * `right` spans the width; the shader derives up = cross(normal, right).  A
 * non-unit or non-perpendicular `right` skews the rectangle, so
 * jce_light_env_add_area_light orthonormalises rather than trusting the
 * caller -- a light authored through a transform arrives with whatever the
 * scale left behind.
 *
 * Shadows are NOT part of this: an area light's shadow is a soft shadow with
 * a penumbra that grows with the emitter, and this engine's local shadow
 * atlas stores one hard depth map per light.  Saying so here is the point --
 * a casts_shadow field that silently produced a point light's hard shadow
 * would be worse than no field. */
typedef struct {
    jce_vec3 position;        /* centre of the rectangle */
    jce_vec3 normal;          /* unit; the direction it emits */
    jce_vec3 right;           /* unit, perpendicular to normal; spans width */
    jce_vec3 color;           /* linear RGB */
    float    intensity;
    float    width;           /* full width along `right`, world units */
    float    height;          /* full height along cross(normal,right) */
    float    radius;          /* attenuation cutoff, as for point/spot */
    bool     two_sided;       /* emit from the back face as well */
    /* Rendering layers -- see the block above.  0 = every layer.  APPENDED. */
    uint32_t layer_mask;
} JceAreaLightDesc;

/* ================================================================== */
/* Light environment API                                               */
/* ================================================================== */

/* Create an empty light environment. */
JCE_API JceLightEnv *jce_light_env_create(void);

/* Destroy the light environment. */
JCE_API void jce_light_env_destroy(JceLightEnv *env);

/* Set ambient light color and intensity. */
JCE_API void jce_light_env_set_ambient(JceLightEnv *env, jce_vec3 color, float intensity);

/* Read back the ambient (GI L2 feeds the probe grid's sky floor from it). */
JCE_API void jce_light_env_get_ambient(const JceLightEnv *env, jce_vec3 *color,
                                       float *intensity);

/* Read back a directional light (GI L3 feeds the probes' sun-bounce term
 * from light 0).  Returns false when `index` has no light. */
JCE_API bool jce_light_env_get_dir_light(const JceLightEnv *env, uint32_t index,
                                         JceDirLightDesc *out);

/* Add lights. Returns the light index, or -1 if at capacity. */
JCE_API int jce_light_env_add_dir_light(JceLightEnv *env, const JceDirLightDesc *light);
JCE_API int jce_light_env_add_point_light(JceLightEnv *env, const JcePointLightDesc *light);
JCE_API int jce_light_env_add_spot_light(JceLightEnv *env, const JceSpotLightDesc *light);
JCE_API int jce_light_env_add_area_light(JceLightEnv *env, const JceAreaLightDesc *light);

/* Clear all lights (ambient retained). */
JCE_API void jce_light_env_clear(JceLightEnv *env);

/* Get current light counts. */
JCE_API uint32_t jce_light_env_dir_count(const JceLightEnv *env);
JCE_API uint32_t jce_light_env_point_count(const JceLightEnv *env);
JCE_API uint32_t jce_light_env_spot_count(const JceLightEnv *env);
JCE_API uint32_t jce_light_env_area_count(const JceLightEnv *env);

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

/* Upload the light uniforms AS SEEN BY A RECEIVER ON LAYER `layer_index`.
 *
 * Identical to jce_light_env_apply in every respect except that a light whose
 * layer_mask excludes this layer uploads with intensity 0.  Zeroed, NOT
 * compacted: the shadow atlas addresses a local light by its index in these
 * same arrays (jce_sr_shadow.c says so twice), so removing a light would slide
 * every later one onto another light's shadow slot.
 *
 * `layer_index` is 0..31 and is masked to that range; anything else would
 * shift a bit off the end of the mask and silently light nothing.
 *
 * When no light in the env carries a mask this is jce_light_env_apply, byte
 * for byte and instruction for instruction -- it delegates. */
JCE_API void jce_light_env_apply_for_layer(const JceLightEnv *env,
                                           const JceRenderer *r,
                                           uint32_t layer_index);

/* Does ANY light in this env restrict itself to a subset of layers?
 *
 * The renderer asks once per frame, for two decisions it cannot make per
 * draw: whether to pay for the per-layer filter at all, and whether Forward+
 * may run this frame (it may not -- see the block above the descriptors). */
JCE_API bool jce_light_env_has_layer_masks(const JceLightEnv *env);

JCE_EXTERN_C_END

#endif /* JCE_LIGHTING_SYSTEM_H */
