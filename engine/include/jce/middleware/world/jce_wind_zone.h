/*
 * jce_wind_zone.h  Directional / spherical wind authoring.
 *
 * Unity WindZone equivalent at the data layer.  Each wind zone is a
 * POD struct describing either:
 *   - Directional: infinite-extent vector field with main + turbulence
 *   - Spherical:   point-centred falloff field
 *
 * Consumers (cloth, foliage, particles) call jce_wind_sample(pos)
 * to fetch a combined wind force vector at a world position.  The
 * sample fn aggregates every registered zone via squared-distance
 * weighting (spherical) or constant contribution (directional).
 *
 * Layer: world (Layer 4) — public.
 */

#ifndef JCE_WIND_ZONE_H
#define JCE_WIND_ZONE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_WIND_ZONE_MAX 16

typedef enum {
    JCE_WIND_ZONE_DIRECTIONAL = 0,
    JCE_WIND_ZONE_SPHERICAL   = 1,
} JceWindZoneMode;

typedef struct {
    JceWindZoneMode mode;
    /* Directional: world-space direction; Spherical: ignored. */
    float           direction[3];
    /* Spherical: world-space centre + radius.  Directional: ignored. */
    float           center[3];
    float           radius;
    /* Steady wind force magnitude. */
    float           main_strength;
    /* Turbulence noise amplitude added on top of main. */
    float           turbulence_strength;
    /* Pulse frequency of the noise term, Hz. */
    float           pulse_frequency;
    /* Pulse magnitude variation (0..1). */
    float           pulse_magnitude;
    bool            active;
} JceWindZone;

/* ── Registry (process-global) ───────────────────────────────── */

JCE_API void     jce_wind_clear(void);
JCE_API uint16_t jce_wind_register(const JceWindZone *zone);
JCE_API bool     jce_wind_remove(uint16_t id);
JCE_API JceWindZone *jce_wind_get(uint16_t id);

/* Sample combined wind at world position `pos_xyz`.  `time_seconds`
 * advances the turbulence noise.  Result is written to `out_force`
 * (xyz force vector).  Safe to call on an empty registry — returns
 * zero force. */
JCE_API void jce_wind_sample(const float *pos_xyz,
                              float        time_seconds,
                              float       *out_force);

/* Count of currently active zones. */
JCE_API uint16_t jce_wind_active_count(void);

JCE_EXTERN_C_END

#endif /* JCE_WIND_ZONE_H */
