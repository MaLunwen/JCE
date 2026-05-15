/*
 * jce_vcam_blend.h  Virtual-camera blend curves.
 *
 * Existing jce_vcam_system handles priority-based camera selection.
 * This module owns the blend curve between two vcam outputs when
 * the active vcam changes — so the camera doesn't snap, it eases
 * over a configurable duration.
 *
 * Mirrors Unity Cinemachine's CM_BlendList curve types.
 *
 * Layer: scene (Layer 4) — public.
 */

#ifndef JCE_VCAM_BLEND_H
#define JCE_VCAM_BLEND_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_VCAM_BLEND_CUT       = 0,
    JCE_VCAM_BLEND_LINEAR    = 1,
    JCE_VCAM_BLEND_EASE_IN   = 2,
    JCE_VCAM_BLEND_EASE_OUT  = 3,
    JCE_VCAM_BLEND_EASE_INOUT = 4,
} JceVCamBlendCurve;

typedef struct {
    JceVCamBlendCurve curve;
    float             duration_seconds;
} JceVCamBlendSpec;

typedef struct {
    JceVCamBlendSpec spec;
    float            elapsed;
    uint64_t         from_vcam;
    uint64_t         to_vcam;
    bool             active;
} JceVCamBlendState;

JCE_API void jce_vcam_blend_init  (JceVCamBlendState *b);
JCE_API void jce_vcam_blend_start (JceVCamBlendState *b,
                                     uint64_t from_vcam,
                                     uint64_t to_vcam,
                                     JceVCamBlendCurve curve,
                                     float duration_seconds);
JCE_API void jce_vcam_blend_tick  (JceVCamBlendState *b, float dt);

/* Evaluate the blend factor t in [0, 1]:
 *   0.0 = fully on from_vcam
 *   1.0 = fully on to_vcam */
JCE_API float jce_vcam_blend_evaluate(const JceVCamBlendState *b);

JCE_EXTERN_C_END

#endif /* JCE_VCAM_BLEND_H */
