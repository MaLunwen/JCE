/*
 * jce_postfx_stack.h  Authoring data for post-process effects.
 *
 * Mirrors Unity URP/HDRP's Volume framework at the data layer: a
 * stack of effects with per-effect enable flag + parameters.  The
 * actual GPU passes are bgfx-coupled (Batch 14+); this module owns
 * the data + serialization so artists and gameplay code can author
 * effects today.
 *
 * Effects in this batch (data only):
 *   - Vignette: intensity, smoothness, color (RGB)
 *   - Bloom: threshold, intensity, scatter
 *   - Chromatic Aberration: intensity (0..1)
 *   - Film Grain: intensity, response, type (smooth/rough/fine)
 *   - Color Adjustments: exposure, contrast, saturation, hue shift
 *
 * Each effect is a POD struct.  The whole stack is an ECS component
 * so designers attach it to a Volume entity (or the camera itself).
 *
 * Layer: middleware / scene (Layer 4) — public.
 */

#ifndef JCE_POSTFX_STACK_H
#define JCE_POSTFX_STACK_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_FILM_GRAIN_THIN1   = 0,
    JCE_FILM_GRAIN_THIN2   = 1,
    JCE_FILM_GRAIN_MEDIUM1 = 2,
    JCE_FILM_GRAIN_MEDIUM2 = 3,
    JCE_FILM_GRAIN_LARGE1  = 4,
    JCE_FILM_GRAIN_LARGE2  = 5,
} JceFilmGrainType;

typedef struct {
    bool   enabled;
    float  intensity;      /* 0..1 */
    float  smoothness;     /* 0..1, edge softness */
    float  color[3];       /* RGB */
} JcePostFxVignette;

typedef struct {
    bool   enabled;
    float  threshold;      /* HDR threshold for bright-pass */
    float  intensity;      /* 0..16 multiplier on extracted highlights */
    float  scatter;        /* 0..1 amount of wide blur */
    float  tint[3];        /* RGB tint */
} JcePostFxBloom;

typedef struct {
    bool   enabled;
    float  intensity;      /* 0..1 */
} JcePostFxChromaticAberration;

typedef struct {
    bool   enabled;
    int    type;           /* JceFilmGrainType */
    float  intensity;      /* 0..1 */
    float  response;       /* 0..1 luminance response */
} JcePostFxFilmGrain;

typedef struct {
    bool   enabled;
    float  exposure_ev;    /* -10..+10 stops */
    float  contrast;       /* -100..+100 */
    float  saturation;     /* -100..+100 */
    float  hue_shift_deg;  /* -180..+180 */
    float  color_filter[3];/* RGB tint multiplier */
} JcePostFxColorAdjust;

/* The full stack — attach as a scene component on a Volume entity
 * (or the camera).  Order is fixed (matches HDRP's render order):
 *   exposure → bloom → chromatic aberration → vignette → film grain → color adj */
typedef struct {
    JcePostFxBloom               bloom;
    JcePostFxChromaticAberration ca;
    JcePostFxVignette            vignette;
    JcePostFxFilmGrain           grain;
    JcePostFxColorAdjust         color;
    /* Priority for blending between multiple PostFX volumes — higher
     * wins on overlap (Unity Volume Profile convention). */
    int   priority;
    float blend_radius;
} JcePostFxStackComponent;

/* Return a stack initialised to "all disabled, sensible defaults".
 * Use this to seed new components so artists don't see garbage. */
JCE_API JcePostFxStackComponent jce_postfx_stack_default(void);

/* Blend two stacks by weight `t ∈ [0,1]`.  Used at runtime to fade
 * between Volume profiles; mirrors Unity's smooth profile blending.
 * Numerical fields lerp linearly; boolean `enabled` follows the
 * stronger-weight side (>0.5 = b's flag). */
JCE_API JcePostFxStackComponent jce_postfx_stack_blend(
    JcePostFxStackComponent a,
    JcePostFxStackComponent b,
    float t);

JCE_EXTERN_C_END

#endif /* JCE_POSTFX_STACK_H */
