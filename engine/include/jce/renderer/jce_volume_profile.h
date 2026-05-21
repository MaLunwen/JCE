/*
 * jce_volume_profile.h  Volume profile asset — post-FX per-field overrides.
 *
 * A VolumeProfile stores which JcePostFXParams fields to override and what
 * target values to blend toward.  It is embedded in JceVolumeComponent and
 * is also suitable for standalone asset serialisation (vol.json).
 *
 * Layer: Renderer (L3).
 */

#ifndef JCE_VOLUME_PROFILE_H
#define JCE_VOLUME_PROFILE_H

#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_postfx.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Bitmask that marks which JcePostFXParams fields a profile overrides.
 * Bit positions match the declaration order inside JcePostFXParams. */
typedef enum {
    JCE_VOLUME_OVERRIDE_EXPOSURE            = (1 << 0),
    JCE_VOLUME_OVERRIDE_GAMMA               = (1 << 1),
    JCE_VOLUME_OVERRIDE_BLOOM_THRESHOLD     = (1 << 2),
    JCE_VOLUME_OVERRIDE_BLOOM_INTENSITY     = (1 << 3),
    JCE_VOLUME_OVERRIDE_FXAA_SPAN_MAX       = (1 << 4),
    JCE_VOLUME_OVERRIDE_FXAA_REDUCE_MIN     = (1 << 5),
    JCE_VOLUME_OVERRIDE_FXAA_REDUCE_MUL     = (1 << 6),
    JCE_VOLUME_OVERRIDE_VIGNETTE_INTENSITY  = (1 << 7),
    JCE_VOLUME_OVERRIDE_VIGNETTE_SMOOTHNESS = (1 << 8),
    JCE_VOLUME_OVERRIDE_CHROMATIC_STRENGTH  = (1 << 9),
} JceVolumeOverrideBit;

/* Asset struct.  Zero-initialised = no overrides, falls through to defaults. */
typedef struct {
    uint16_t       enabled_mask; /* OR of JceVolumeOverrideBit */
    JcePostFXParams values;      /* target values for each field */
} JceVolumeProfile;

/* ── Volume system ────────────────────────────────────────────────── */

/* Forward declarations. */
typedef struct JceScene JceScene;
typedef struct jce_vec3 jce_vec3;

/* Blends all Volume components in the scene into inout_params.
 * Call once per frame, before jce_postfx_set_params, inside the
 * scene renderer.  cam_pos is the world-space camera position. */
JCE_API void jce_volume_system_tick(JceScene         *scene,
                                    jce_vec3          cam_pos,
                                    JcePostFXParams  *inout_params);

JCE_EXTERN_C_END

#endif /* JCE_VOLUME_PROFILE_H */
