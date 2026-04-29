/*
 * jce_reverb_zones.h -- generic 3D reverb-zone blender.
 *
 * Pure-CPU.  Game registers reverb "zones" (AABB / sphere / OBB) each
 * with a target reverb preset and a falloff radius beyond the shape
 * within which the preset blends in.  Each frame, given a listener
 * position, the module returns a single blended JceReverbPreset that
 * the caller can hand to its reverb DSP (e.g. a future
 * jce_audio_set_global_reverb call).
 *
 * Generic — knows nothing about audio.  The output struct is plain
 * floats; caller maps them to whatever reverb implementation they
 * actually own (Freeverb / Schroeder / FMOD / WWise / Steam Audio).
 *
 * Thread-safety: a JceReverbZones instance is single-threaded.
 * jce_reverb_zones_sample() reads the slot array; no add/remove/update
 * calls may overlap with sample().  For multi-threaded queries, either
 * lock externally or copy the snapshot before sampling.
 *
 * Example:
 *   JceReverbZones *rz = jce_reverb_zones_create(32);
 *   JceReverbZoneDesc d = {0};
 *   d.shape          = JCE_REVERB_SHAPE_AABB;
 *   d.center         = (jce_vec3){10, 1, 5};
 *   d.extents        = (jce_vec3){4, 2, 4};
 *   d.falloff_radius = 1.5f;
 *   d.priority       = 1;
 *   d.preset         = jce_reverb_preset_hall();
 *   JceReverbZoneId id = jce_reverb_zones_add(rz, &d);
 *   ...
 *   JceReverbPreset blended;
 *   jce_reverb_zones_sample(rz, listener_pos, &blended);
 *   apply_reverb_to_audio_engine(&blended);
 *
 * Layer: middleware (Layer 4) — public.
 */
#ifndef JCE_REVERB_ZONES_H
#define JCE_REVERB_ZONES_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceReverbZones JceReverbZones;

typedef enum {
    JCE_REVERB_SHAPE_AABB   = 0,
    JCE_REVERB_SHAPE_SPHERE = 1,
    JCE_REVERB_SHAPE_OBB    = 2
} JceReverbShape;

/* Generic reverb parameters — engine-agnostic floats in [0,1] except
 * `decay_seconds` (room tail) and `room_size` (m).  Outside library
 * just blends them numerically. */
typedef struct {
    float wet_mix;        /* 0..1 — overall reverb send level         */
    float dry_mix;        /* 0..1 — direct path level                 */
    float decay_seconds;  /* RT60-ish reverb tail length              */
    float room_size;      /* perceived size hint (m) — 1..1000        */
    float damping;        /* 0..1 — high-frequency absorption         */
    float diffusion;      /* 0..1 — early-reflection density          */
    float density;        /* 0..1 — late-reflection density           */
    float pre_delay_ms;   /* delay before first reflection (ms)       */
    float lowpass_hz;     /* output LP cutoff; 22050 = bypass         */
} JceReverbPreset;

typedef uint32_t JceReverbZoneId;
#define JCE_REVERB_ZONE_INVALID ((JceReverbZoneId)0)

typedef struct {
    JceReverbShape  shape;
    /* Sphere: center, radius (radius = extents.x; extents.y/z unused).
     * AABB:   center, half-extents.
     * OBB:    center, half-extents in local axes, axes via rotation.   */
    jce_vec3        center;
    jce_vec3        extents;
    jce_mat4        rotation;        /* OBB only; identity for AABB/sphere */
    float           falloff_radius;  /* world-units beyond shape; 0 = hard edge */
    int             priority;        /* higher overrides on tie         */
    JceReverbPreset preset;
} JceReverbZoneDesc;

JCE_API JceReverbZones *jce_reverb_zones_create(uint32_t initial_capacity);
JCE_API void            jce_reverb_zones_destroy(JceReverbZones *r);

JCE_API JceReverbZoneId jce_reverb_zones_add(JceReverbZones *r, const JceReverbZoneDesc *desc);
JCE_API bool            jce_reverb_zones_update(JceReverbZones *r, JceReverbZoneId id, const JceReverbZoneDesc *desc);
JCE_API bool            jce_reverb_zones_remove(JceReverbZones *r, JceReverbZoneId id);
JCE_API uint32_t        jce_reverb_zones_count(const JceReverbZones *r);

/* "Outside" / default preset returned when listener is in no zones.    */
JCE_API void            jce_reverb_zones_set_default(JceReverbZones *r, const JceReverbPreset *p);

/* Sample blended preset at a listener position.
 * Algorithm: every zone whose listener-distance to its surface is
 * <= falloff_radius contributes a weight w = 1 if inside, otherwise
 * smoothstep((falloff - dist) / falloff).  Final preset is the
 * weighted mean of all contributing zone presets, plus the default
 * preset weighted by max(0, 1 - sum_w).  Higher `priority` zones
 * scale their weight by 4^(priority - max_priority + n) so they
 * dominate when overlapping equal-distance lower-priority zones. */
JCE_API void            jce_reverb_zones_sample(const JceReverbZones *r,
                                                jce_vec3 listener_position,
                                                JceReverbPreset *out_preset);

/* Convenience presets — generic, not game-specific. */
JCE_API JceReverbPreset jce_reverb_preset_outdoor(void);
JCE_API JceReverbPreset jce_reverb_preset_room(void);
JCE_API JceReverbPreset jce_reverb_preset_hall(void);
JCE_API JceReverbPreset jce_reverb_preset_cave(void);
JCE_API JceReverbPreset jce_reverb_preset_underwater(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_REVERB_ZONES_H */
