/*
 * jce_time_of_day.c -- analytical day/night curve.
 *
 * A single sun trajectory parameterised by hour-of-day, plus the lighting
 * snapshot the renderer needs for that moment.  No GPU work.
 *
 * The sun colour and the daytime sky gradient are DERIVED, not authored:
 * sun colour is E_top_of_atmosphere * atmospheric transmittance
 * (jce_atmosphere.h), and the three sky stops are sampled from the same
 * analytic Preetham sky the renderer draws (jce_sky.h).  Both used to be
 * hand-tuned palettes lerped on the sun altitude, which meant the cheap
 * gradient tier and the analytic sky could disagree about the same moment
 * of the same day, and every atmosphere change needed a re-tune.
 *
 * Night stops remain authored: Preetham is a daylight model and is not
 * valid with the sun below the horizon.
 */

#include <jce/middleware/world/jce_time_of_day.h>
#include <jce/middleware/world/jce_atmosphere.h>
#include <jce/middleware/world/jce_sky.h>
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

    /* Sun colour is DERIVED, not authored.
     *
     * It used to be a four-stop palette (night tint -> warm dawn -> white noon
     * -> deeper dusk) lerped on the sun's altitude.  That has to be re-tuned
     * for every atmosphere and it can never agree with a sky drawn by a
     * different model.  Now it is E_top_of_atmosphere * transmittance, so the
     * reddening near the horizon falls out of Rayleigh extinction instead of
     * being painted in: blue is scattered away over the long grazing path
     * while red survives.
     *
     * The result is divided back down into the renderer's existing arbitrary
     * intensity range.  Publishing true lux would overflow mediump on the
     * WebGL2 / integrated-GPU tier, and every shipped scene is authored
     * against the old range, so a units migration is deliberately NOT done
     * here -- see the environment-lighting design.  `sun_reference_lux` is the
     * single constant that maps physical to authored; changing it rescales
     * every light in lockstep rather than drifting one curve stop at a time. */
    JceAtmosphereParams atmo = jce_atmosphere_default_params();
    jce_vec3 lux = jce_atmosphere_sun_illuminance(&atmo, 0.0f, sun, up);

    /* Peak clear-noon illuminance maps to the previous palette's peak so
     * existing content keeps its exposure. */
    const float sun_reference_lux = 105000.0f;
    const float sun_peak_intensity = 2.80f;
    const float k = sun_peak_intensity / sun_reference_lux;
    jce_vec3 sun_color = jce_v3(lux.x * k, lux.y * k, lux.z * k);

    /* Night floor: a moonlight tint so a fully-set sun still reads as a
     * direction rather than as pure black. */
    const jce_vec3 col_night = jce_v3(0.05f, 0.07f, 0.15f);
    if (sun_color.x < col_night.x) sun_color.x = col_night.x;
    if (sun_color.y < col_night.y) sun_color.y = col_night.y;
    if (sun_color.z < col_night.z) sun_color.z = col_night.z;

    /* Sky gradient -- DERIVED from the same analytic sky the renderer draws.
     *
     * These three stops used to be nine hand-tuned palette entries (day /
     * night / twilight zenith, day / night / dawn / dusk horizon, day / night
     * ground) lerped on the sun altitude.  Nothing tied them to the Preetham
     * sky, so the cheap gradient tier and the analytic tier could disagree
     * about the same moment of the same day.
     *
     * Sampling the model at three directions makes the gradient a faithful
     * three-point reduction of the sky.  The warm dawn/dusk horizon is no
     * longer painted in: it comes out of the model automatically, because a
     * low sun means a long grazing path and a reddened horizon.  That is why
     * the separate dawn and dusk horizon palettes are gone.
     *
     * Preetham is a DAYLIGHT model and is not valid with the sun below the
     * horizon, so the night stops remain authored and are blended in by
     * day_t.  Deriving those needs a night-sky model, which is out of scope
     * here. */
    jce_vec3 sky_top_night   = jce_v3(0.02f, 0.03f, 0.07f);
    jce_vec3 sky_top_twilight= jce_v3(0.18f, 0.10f, 0.30f);
    jce_vec3 sky_horiz_night = jce_v3(0.04f, 0.05f, 0.10f);
    jce_vec3 sky_ground_night= jce_v3(0.02f, 0.02f, 0.04f);

    jce_vec3 sky_top_day, sky_horiz_day, sky_ground_day;
    {
        JceSkyConfig scfg = jce_sky_config_default();
        /* normalize divides luminance by the zenith value, so the output is
         * turbidity-independent and lands in a stable O(1) range.  The scale
         * below then maps it onto the range existing content is authored
         * against -- one constant, rather than nine drifting palette stops. */
        scfg.normalize = 1;
        const float sd[3] = { sun.x, sun.y, sun.z };
        JceSkyState sst = jce_sky_evaluate(&scfg, sd);

        const float SKY_GRADIENT_SCALE = 0.45f;
        /* Fraction of horizon light the ground bounces back.  Ground is not
         * part of a daylight sky model, so it is an explicit albedo. */
        const float GROUND_ALBEDO = 0.30f;

        float rgb[3];
        const float up_d[3] = { up.x, up.y, up.z };
        jce_sky_radiance(&sst, up_d, rgb);
        sky_top_day = jce_v3(rgb[0] * SKY_GRADIENT_SCALE,
                             rgb[1] * SKY_GRADIENT_SCALE,
                             rgb[2] * SKY_GRADIENT_SCALE);

        /* Horizon: average a ring just above the horizon so the stop is the
         * whole skyline rather than one azimuth, and so it stays continuous
         * as the sun swings around. */
        jce_vec3 east  = jce_v3_normalize(jce_v3_cross(cfg.north_axis, up));
        jce_vec3 north = jce_v3_normalize(jce_v3_cross(up, east));
        jce_vec3 acc = jce_v3(0.0f, 0.0f, 0.0f);
        const int RING = 8;
        for (int i = 0; i < RING; i++) {
            const float a = (float)i * (2.0f * (float)M_PI / (float)RING);
            const float ca = cosf(a), sa2 = sinf(a);
            /* ~3 degrees above the horizon: inside the model's valid range
             * but low enough to read as "the horizon". */
            const float el = 0.052f;
            jce_vec3 d = jce_v3(
                (east.x * sa2 + north.x * ca) * cosf(el) + up.x * sinf(el),
                (east.y * sa2 + north.y * ca) * cosf(el) + up.y * sinf(el),
                (east.z * sa2 + north.z * ca) * cosf(el) + up.z * sinf(el));
            d = jce_v3_normalize(d);
            const float dd[3] = { d.x, d.y, d.z };
            jce_sky_radiance(&sst, dd, rgb);
            acc.x += rgb[0]; acc.y += rgb[1]; acc.z += rgb[2];
        }
        const float inv = SKY_GRADIENT_SCALE / (float)RING;
        sky_horiz_day = jce_v3(acc.x * inv, acc.y * inv, acc.z * inv);

        sky_ground_day = jce_v3(sky_horiz_day.x * GROUND_ALBEDO,
                                sky_horiz_day.y * GROUND_ALBEDO,
                                sky_horiz_day.z * GROUND_ALBEDO);
    }

    jce_vec3 sky_top     = td_lerp(sky_top_night, sky_top_day, day_t);
    /* Boost twilight blue-purple. */
    sky_top = td_lerp(sky_top, sky_top_twilight, horizon_band * (1.0f - day_t) * 0.4f);

    jce_vec3 sky_horizon = td_lerp(sky_horiz_night, sky_horiz_day, day_t);

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

/* ── Publish contract ──────────────────────────────────────────────── */

void JCE_CALL jce_time_of_day_resolve_fog(const JceTimeOfDayState *tod,
                                          const float authored_color[3],
                                          float       authored_density,
                                          float       out_color[3],
                                          float      *out_density)
{
    /* Inactive cycle: pass the authored values straight through, so a scene
     * without a day/night cycle renders bit-identically. */
    if (out_color) {
        if (tod) {
            out_color[0] = tod->fog_color.x;
            out_color[1] = tod->fog_color.y;
            out_color[2] = tod->fog_color.z;
        } else if (authored_color) {
            out_color[0] = authored_color[0];
            out_color[1] = authored_color[1];
            out_color[2] = authored_color[2];
        } else {
            out_color[0] = out_color[1] = out_color[2] = 0.0f;
        }
    }

    if (out_density) {
        /* The cycle scales the authored density rather than replacing it, so
         * an artist who authored thick fog keeps thick fog and still gets the
         * night thickening.  tod->fog_density is calibrated around the daytime
         * default, so the ratio is what carries the time-of-day signal. */
        if (tod) {
            const float daytime_reference = 0.0025f;
            float scale = tod->fog_density / daytime_reference;
            if (scale < 0.0f) scale = 0.0f;
            *out_density = authored_density * scale;
        } else {
            *out_density = authored_density;
        }
    }
}

float JCE_CALL jce_time_of_day_resolve_exposure(const JceTimeOfDayState *tod,
                                                float authored_exposure)
{
    if (!tod) return authored_exposure;
    /* Multiplicative: the cycle dims toward night relative to whatever the
     * scene authored, instead of discarding the authored value. */
    return authored_exposure * tod->exposure;
}
