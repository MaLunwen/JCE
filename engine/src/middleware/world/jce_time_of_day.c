/*
 * jce_time_of_day.c -- analytical day/night curve.
 *
 * The model is intentionally cheap: a single sun trajectory parameterised
 * by hour-of-day, plus a small number of curated colour gradients keyed
 * on the sun's altitude angle.  This is far simpler than a Hosek-Wilkie
 * fit, but produces convincing dawn / midday / dusk / night transitions
 * without any GPU work.
 *
 * Colour palettes were hand-tuned to read well under a tone-mapped HDR
 * pipeline (Reinhard at exposure ~1.0).
 */

#include <jce/middleware/world/jce_time_of_day.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_profiler.h>

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */

JceTimeOfDayConfig jce_time_of_day_default_config(void)
{
    JceTimeOfDayConfig c;
    c.dawn_hour        = 6.0f;
    c.dusk_hour        = 18.0f;
    c.latitude_degrees = 35.0f;
    c.north_axis       = jce_v3(0.0f, 0.0f, 1.0f);
    c.up_axis          = jce_v3(0.0f, 1.0f, 0.0f);
    return c;
}

/* ------------------------------------------------------------------ */

static float td_wrap_hour(float h)
{
    h = fmodf(h, 24.0f);
    if (h < 0.0f) h += 24.0f;
    return h;
}

static jce_vec3 td_lerp(jce_vec3 a, jce_vec3 b, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return jce_v3(a.x + (b.x - a.x) * t,
                  a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t);
}

static float td_smooth(float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* ------------------------------------------------------------------ */

/* Sun direction: parameterise by an "azimuth phase" that runs the sun
 * from east-rise (dawn) over the southern sky to west-set (dusk), with
 * altitude tracing a half-sine across the daytime arc.  At night the
 * sun is below the horizon; we still emit a downward-pointing vector so
 * downstream code never sees a zero light dir. */
static jce_vec3 td_sun_direction(const JceTimeOfDayConfig *cfg, float hour)
{
    float dawn = cfg->dawn_hour;
    float dusk = cfg->dusk_hour;
    float day_len = dusk - dawn;
    if (day_len < 0.5f) day_len = 0.5f;

    /* phase: 0 at dawn, 1 at dusk (during the day);
     * during the night we extend smoothly so the sun arcs underneath. */
    float phase;
    bool  is_day;
    if (hour >= dawn && hour <= dusk) {
        phase  = (hour - dawn) / day_len;
        is_day = true;
    } else {
        float night_hour = (hour > dusk) ? (hour - dusk)
                                          : (24.0f - dusk + hour);
        float night_len  = 24.0f - day_len;
        if (night_len < 0.5f) night_len = 0.5f;
        phase  = night_hour / night_len; /* [0,1] across the night */
        is_day = false;
    }

    /* Azimuth runs east → south → west → north → east */
    float azimuth_t = is_day ? (phase * 0.5f)         /* 0   .. 0.5 */
                              : (0.5f + phase * 0.5f); /* 0.5 .. 1.0 */
    float az = azimuth_t * 2.0f * (float)M_PI;

    /* Altitude: half-sine peaking at solar noon, biased by latitude. */
    float lat_rad = cfg->latitude_degrees * (float)(M_PI / 180.0);
    float max_alt = 0.5f * (float)M_PI - fabsf(lat_rad) * 0.6f;
    if (max_alt < 0.05f) max_alt = 0.05f;

    float alt;
    if (is_day) {
        alt = sinf(phase * (float)M_PI) * max_alt;
    } else {
        /* Symmetric arc below horizon. */
        alt = -sinf(phase * (float)M_PI) * max_alt;
    }

    /* Build basis from up + north. */
    jce_vec3 up    = jce_v3_normalize(cfg->up_axis);
    jce_vec3 north = jce_v3_normalize(cfg->north_axis);
    jce_vec3 east  = jce_v3_normalize(jce_v3_cross(north, up));
    /* Recompute true north so basis is orthonormal. */
    north = jce_v3_normalize(jce_v3_cross(up, east));

    float ca = cosf(alt);
    float sa = sinf(alt);
    float cz = cosf(az);
    float sz = sinf(az);

    jce_vec3 dir;
    dir.x = ca * (east.x * sz + north.x * cz) + up.x * sa;
    dir.y = ca * (east.y * sz + north.y * cz) + up.y * sa;
    dir.z = ca * (east.z * sz + north.z * cz) + up.z * sa;
    return jce_v3_normalize(dir);
}

/* ------------------------------------------------------------------ */

void jce_time_of_day_evaluate(const JceTimeOfDayConfig *cfg_in,
                               float                      hour_of_day,
                               JceTimeOfDayState         *out)
{
    if (!out) return;

    JCE_PROFILE_ZONE_N("TimeOfDay::Evaluate");

    JceTimeOfDayConfig cfg = cfg_in ? *cfg_in : jce_time_of_day_default_config();
    float hour = td_wrap_hour(hour_of_day);

    jce_vec3 sun = td_sun_direction(&cfg, hour);

    /* Altitude relative to the up axis -> -1 (below) .. +1 (zenith). */
    jce_vec3 up = jce_v3_normalize(cfg.up_axis);
    float alt_dot = sun.x * up.x + sun.y * up.y + sun.z * up.z;

    /* Day-night blend factor: 0 below horizon, 1 well above. */
    float day_t = td_smooth(alt_dot * 4.0f + 0.5f);

    /* Twilight band peaks when sun is just at horizon. */
    float horizon_band = 1.0f - fabsf(alt_dot);
    horizon_band = horizon_band * horizon_band; /* sharper */

    /* ---- Palettes (linear RGB, tuned for HDR + Reinhard) -------- */

    /* Sun colour: deep night → warm dawn → white noon → warm dusk → night. */
    jce_vec3 col_night  = jce_v3(0.05f, 0.07f, 0.15f); /* moonlight tint */
    jce_vec3 col_dawn   = jce_v3(2.20f, 1.20f, 0.55f); /* warm orange */
    jce_vec3 col_noon   = jce_v3(2.80f, 2.70f, 2.55f); /* near-white, hot */
    jce_vec3 col_dusk   = jce_v3(2.40f, 1.05f, 0.40f); /* deeper red */

    /* Choose dawn vs dusk based on hour. */
    bool morning = (hour < (cfg.dawn_hour + cfg.dusk_hour) * 0.5f);
    jce_vec3 sun_color;
    if (alt_dot < 0.0f) {
        sun_color = col_night;
    } else if (alt_dot < 0.25f) {
        float t = alt_dot / 0.25f;
        jce_vec3 warm = morning ? col_dawn : col_dusk;
        sun_color = td_lerp(warm, col_noon, td_smooth(t));
    } else {
        sun_color = col_noon;
    }

    /* Sky gradient. */
    jce_vec3 sky_top_day     = jce_v3(0.18f, 0.45f, 0.85f);
    jce_vec3 sky_top_night   = jce_v3(0.02f, 0.03f, 0.07f);
    jce_vec3 sky_top_twilight= jce_v3(0.18f, 0.10f, 0.30f);

    jce_vec3 sky_horiz_day      = jce_v3(0.65f, 0.78f, 0.92f);
    jce_vec3 sky_horiz_night    = jce_v3(0.04f, 0.05f, 0.10f);
    jce_vec3 sky_horiz_dawn     = jce_v3(1.10f, 0.55f, 0.30f);
    jce_vec3 sky_horiz_dusk     = jce_v3(1.15f, 0.40f, 0.25f);

    jce_vec3 sky_ground_day   = jce_v3(0.22f, 0.22f, 0.28f);
    jce_vec3 sky_ground_night = jce_v3(0.02f, 0.02f, 0.04f);

    jce_vec3 sky_top     = td_lerp(sky_top_night, sky_top_day, day_t);
    /* Boost twilight blue-purple. */
    sky_top = td_lerp(sky_top, sky_top_twilight, horizon_band * (1.0f - day_t) * 0.4f);

    jce_vec3 sky_horizon = td_lerp(sky_horiz_night, sky_horiz_day, day_t);
    jce_vec3 horiz_warm  = morning ? sky_horiz_dawn : sky_horiz_dusk;
    sky_horizon = td_lerp(sky_horizon, horiz_warm,
                          horizon_band * (alt_dot > -0.15f ? 1.0f : 0.0f));

    jce_vec3 sky_ground = td_lerp(sky_ground_night, sky_ground_day, day_t);

    /* Ambient sky-derived (averaged horizon + top). */
    jce_vec3 ambient = jce_v3(
        (sky_top.x + sky_horizon.x) * 0.25f,
        (sky_top.y + sky_horizon.y) * 0.25f,
        (sky_top.z + sky_horizon.z) * 0.25f);

    /* Fog uses the horizon colour (atmospheric perspective). */
    jce_vec3 fog = sky_horizon;
    float fog_density = 0.0025f + 0.0050f * (1.0f - day_t); /* hazier at night */

    float exposure = 1.0f - 0.35f * (1.0f - day_t); /* darker at night */

    out->sun_direction = sun;
    out->sun_color     = sun_color;
    out->sun_intensity = sqrtf(sun_color.x*sun_color.x +
                                sun_color.y*sun_color.y +
                                sun_color.z*sun_color.z);
    out->sky_top       = sky_top;
    out->sky_horizon   = sky_horizon;
    out->sky_ground    = sky_ground;
    out->ambient_color = ambient;
    out->fog_color     = fog;
    out->fog_density   = fog_density;
    out->exposure      = exposure;
    out->is_night      = (alt_dot < 0.0f);
    JCE_PROFILE_ZONE_END;
}
