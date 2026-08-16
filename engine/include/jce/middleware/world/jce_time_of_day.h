/*
 * jce_time_of_day.h -- Day/night cycle driver.
 *
 * Pure-CPU module that, given a fractional hour-of-day (0 .. 24),
 * produces a self-consistent snapshot of all lighting parameters the
 * scene renderer needs to portray that moment of the day:
 *
 *     - sun direction (light vector pointing TOWARD the sun)
 *     - sun colour    (warm at dawn/dusk, white at noon, dim at night)
 *     - sky gradient  (top / horizon / ground)
 *     - ambient sky tint
 *     - fog colour
 *     - exposure scalar
 *
 * The module is intentionally renderer-agnostic: callers either feed
 * the snapshot directly into their own shaders or pass it to
 * jce_scene_renderer_set_time_of_day() which overrides the renderer's
 * built-in sky gradient + sun direction for that frame.
 *
 * Layer: Graphics (Layer 3) — public.
 */

#ifndef JCE_TIME_OF_DAY_H
#define JCE_TIME_OF_DAY_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* Snapshot of lighting state for one moment in the day. */
typedef struct {
    jce_vec3 sun_direction;   /* unit vector from world origin TOWARD sun */
    jce_vec3 sun_color;       /* linear RGB; intensity baked in */
    float    sun_intensity;   /* convenience copy (length of color) */

    jce_vec3 sky_top;         /* zenith colour */
    jce_vec3 sky_horizon;
    jce_vec3 sky_ground;

    jce_vec3 ambient_color;
    jce_vec3 fog_color;
    float    fog_density;     /* exponential, in 1/m */

    float    exposure;        /* HDR exposure multiplier */

    bool     is_night;        /* convenience flag (sun below horizon) */
} JceTimeOfDayState;

/* Configuration for the day/night curve.  Defaults match a temperate
 * coastal day; callers can override for stylised looks. */
typedef struct {
    float dawn_hour;          /* sun crosses horizon ascending  (default 6.0)  */
    float dusk_hour;          /* sun crosses horizon descending (default 18.0) */

    /* Latitude affects how high the sun climbs.  0 = equator (zenith
     * at noon), 45 = mid-latitude.  Stored in degrees.  Default 35. */
    float latitude_degrees;

    /* World "north" vector in scene space.  Default (0,0,1). */
    jce_vec3 north_axis;
    /* World "up" vector.  Default (0,1,0). */
    jce_vec3 up_axis;
} JceTimeOfDayConfig;

JCE_API JceTimeOfDayConfig jce_time_of_day_default_config(void);

/* Compute the full lighting snapshot for `hour_of_day` ∈ [0, 24).
 * Hours outside that range are wrapped. */
JCE_API void jce_time_of_day_evaluate(const JceTimeOfDayConfig *cfg,
                                       float                      hour_of_day,
                                       JceTimeOfDayState         *out);

/* ── Publish contract ───────────────────────────────────────────────
 *
 * The snapshot above is only worth computing if consumers actually read it.
 * fog_color, fog_density and exposure were computed for every frame of every
 * day/night cycle and read by nothing, so a scene kept its daytime fog and
 * daytime exposure at midnight.
 *
 * These resolvers are the publish points.  They are pure functions of
 * (authored value, snapshot) so the policy is testable without a renderer,
 * and so there is exactly one place that decides how a day/night cycle
 * overrides authored scene settings.
 *
 * `tod` NULL means the cycle is inactive: the authored value passes through
 * unchanged, so a scene with no day/night cycle is bit-identical. */

/* Fog colour and density for this moment.  A day/night cycle owns the fog
 * TINT and THICKNESS (night is darker and hazier); the scene keeps ownership
 * of whether fog exists at all and of its start/end distances. */
JCE_API void JCE_CALL jce_time_of_day_resolve_fog(
    const JceTimeOfDayState *tod,
    const float              authored_color[3],
    float                    authored_density,
    float                    out_color[3],
    float                   *out_density);

/* Exposure multiplier for this moment.  Returns `authored_exposure` when the
 * cycle is inactive. */
JCE_API float JCE_CALL jce_time_of_day_resolve_exposure(
    const JceTimeOfDayState *tod, float authored_exposure);

JCE_EXTERN_C_END

#endif /* JCE_TIME_OF_DAY_H */
