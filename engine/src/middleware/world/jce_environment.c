/*
 * jce_environment.c -- the authoritative world environment state.
 *
 * See jce_environment.h for why there is exactly one of these.
 *
 * Everything here is pure arithmetic on the struct: no clock, no RNG, no GPU
 * resource, no global. That is deliberate and load-bearing -- a headless
 * server, a replaying client and the editor must step the same environment to
 * the same bits, and the cheapest way to guarantee that is to leave nothing
 * else for them to disagree about.
 */

#include <jce/middleware/world/jce_environment.h>

#include <math.h>
#include <string.h>

/* ── small helpers ───────────────────────────────────────────────────
 * Written as positive tests so a NaN, which fails every comparison, falls
 * through to the safe branch instead of propagating. */

static float env_clampf(float v, float lo, float hi, float fallback)
{
    if (!(v == v)) return fallback;          /* NaN */
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float env_clamp01(float v) { return env_clampf(v, 0.0f, 1.0f, 0.0f); }

/* Exponential approach toward `target` with a time constant of `tau` seconds.
 *
 * Framerate-independent by construction: the same elapsed time produces the
 * same result whether it arrived as one step or sixty. A naive
 * `v += (target - v) * rate * dt` does NOT have that property -- it converges
 * faster at high framerates, so wet ground would dry at a speed that depended
 * on the machine, and a replay at a different framerate would diverge. */
static float env_approach(float v, float target, float tau, float dt)
{
    if (!(tau > 0.0f)) return target;
    const float k = 1.0f - expf(-dt / tau);
    return v + (target - v) * k;
}

/* default() repairs itself through sanitize(), which is defined below. */
JCE_API void JCE_CALL jce_environment_sanitize(JceEnvironmentState *s);

/* ── defaults ────────────────────────────────────────────────────────  */

JceEnvironmentState JCE_CALL jce_environment_default(void)
{
    JceEnvironmentState s;
    memset(&s, 0, sizeof s);

    s.world_time_seconds = 0.0;
    s.day_fraction       = 0.5f;         /* noon */
    s.seconds_per_day    = 86400.0f;
    s.day_seed_hour      = -1.0f;        /* never seeded from an authored hour */

    /* Sun overhead and slightly south; moon opposite it so the pair is never
     * both above the horizon at the default, which would make "which is the
     * key light" ambiguous on frame zero. */
    s.sun_direction_ws     = jce_v3(0.0f,  0.9f, 0.436f);
    s.sun_illuminance_lux  = 100000.0f;  /* clear midday sun */
    s.moon_direction_ws    = jce_v3(0.0f, -0.9f, -0.436f);
    s.moon_illuminance_lux = 0.25f;      /* full moon at zenith */

    /* A unit direction even though the speed is zero: readers normalise
     * nothing, and a zero-length direction becomes NaN on first division. */
    s.wind_direction_ws = jce_v3(1.0f, 0.0f, 0.0f);
    s.wind_speed_mps    = 0.0f;
    s.wind_gust         = 0.0f;

    s.cloud_coverage      = 0.0f;
    s.cloud_density       = 1.0f;
    s.cloud_precipitation = 0.0f;

    s.humidity           = 0.4f;
    s.fog_density        = 0.0f;
    s.precipitation_rate = 0.0f;
    s.temperature_c      = 15.0f;

    s.global_wetness = 0.0f;
    s.snow_amount    = 0.0f;

    s.weather_type = (uint32_t)JCE_ENV_WEATHER_CLEAR;
    s.weather_seed = 0u;

    /* Run it through the same repair every external write goes through, so
     * the default is sanitize-STABLE by construction rather than because
     * somebody typed a unit vector correctly.  (0, 0.9, 0.436) is off by
     * 4.8e-5, which is invisible in a picture and is exactly the kind of
     * near-miss that makes "sanitize changed nothing" fail mysteriously.) */
    jce_environment_sanitize(&s);
    return s;
}

/* ── sanitize ────────────────────────────────────────────────────────  */

static jce_vec3 env_safe_direction(jce_vec3 v, jce_vec3 fallback)
{
    const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
    /* Positive test: catches NaN, and catches a direction so short that
     * normalising it would amplify float error into a random heading. */
    if (!(len2 > 1e-12f) || !(len2 < 1e12f)) return fallback;
    const float inv = 1.0f / sqrtf(len2);
    return jce_v3(v.x * inv, v.y * inv, v.z * inv);
}

void JCE_CALL jce_environment_sanitize(JceEnvironmentState *s)
{
    if (!s) return;

    if (!(s->world_time_seconds == s->world_time_seconds))   /* NaN */
        s->world_time_seconds = 0.0;

    /* Day length before day fraction: the fraction is wrapped against it. */
    if (!(s->seconds_per_day > 0.0f) || !(s->seconds_per_day < 1e9f))
        s->seconds_per_day = 86400.0f;

    s->day_fraction = env_clampf(s->day_fraction, -1e9f, 1e9f, 0.5f);
    s->day_fraction -= floorf(s->day_fraction);
    /* floorf of a value just below zero rounds the fraction to exactly 1.0,
     * which is the same instant as 0 by periodicity -- fold it so the
     * documented [0,1) really holds. */
    if (!(s->day_fraction >= 0.0f && s->day_fraction < 1.0f)) s->day_fraction = 0.0f;

    s->sun_direction_ws  = env_safe_direction(s->sun_direction_ws,
                                              jce_v3(0.0f, 0.9f, 0.436f));
    s->moon_direction_ws = env_safe_direction(s->moon_direction_ws,
                                              jce_v3(0.0f, -0.9f, -0.436f));
    s->sun_illuminance_lux  = env_clampf(s->sun_illuminance_lux,  0.0f, 2e6f, 100000.0f);
    s->moon_illuminance_lux = env_clampf(s->moon_illuminance_lux, 0.0f, 1e3f, 0.25f);

    s->wind_direction_ws = env_safe_direction(s->wind_direction_ws,
                                              jce_v3(1.0f, 0.0f, 0.0f));
    s->wind_speed_mps = env_clampf(s->wind_speed_mps, 0.0f, 120.0f, 0.0f);
    s->wind_gust      = env_clamp01(s->wind_gust);

    s->cloud_coverage      = env_clamp01(s->cloud_coverage);
    /* Density has no meaningful zero -- a zero-density cloud layer is just an
     * expensive way to draw nothing, and coverage already expresses "none". */
    s->cloud_density       = env_clampf(s->cloud_density, 1e-3f, 100.0f, 1.0f);
    s->cloud_precipitation = env_clamp01(s->cloud_precipitation);

    s->humidity           = env_clamp01(s->humidity);
    s->fog_density        = env_clampf(s->fog_density, 0.0f, 1.0f, 0.0f);
    s->precipitation_rate = env_clamp01(s->precipitation_rate);
    s->temperature_c      = env_clampf(s->temperature_c, -100.0f, 100.0f, 15.0f);

    s->global_wetness = env_clamp01(s->global_wetness);
    s->snow_amount    = env_clamp01(s->snow_amount);

    if (s->weather_type > (uint32_t)JCE_ENV_WEATHER_SNOW)
        s->weather_type = (uint32_t)JCE_ENV_WEATHER_CLEAR;
}

/* ── advance ─────────────────────────────────────────────────────────  */

/* Time constants, in seconds.  Wetting is far faster than drying because that
 * is what it looks like: a downpour darkens pavement in under a minute and it
 * stays damp for a good few minutes.  Chosen so the effect is legible within
 * a play session: a 15-minute drying constant is more faithful to wet tarmac
 * and reads, in a game, as wetness that never goes away. */
#define ENV_TAU_WETTING   30.0f
#define ENV_TAU_DRYING   240.0f
#define ENV_TAU_SNOWFALL 120.0f
#define ENV_TAU_MELT     300.0f

/* Snow settles below this and melts above it.  Not 0 C: wet snow lands and
 * melts on contact for the first couple of degrees, which is why a 1 C
 * snowfall leaves nothing lying. */
#define ENV_SNOW_SETTLE_C 1.0f

void JCE_CALL jce_environment_advance(JceEnvironmentState *s, float dt_seconds)
{
    if (!s) return;
    /* Positive test: rejects NaN and negative dt alike.  A no-op, never a
     * rewind -- a paused frame or a broken timer must not move the world
     * backwards, because wetness and snow are integrators and would unwind. */
    if (!(dt_seconds > 0.0f)) return;
    if (!(dt_seconds < 1e6f)) return;

    jce_environment_sanitize(s);

    s->world_time_seconds += (double)dt_seconds;

    /* Wrap against the authored day length.  Derived from the increment
     * rather than recomputed from world_time_seconds, so a long-running
     * server does not lose fractional precision as the double grows. */
    s->day_fraction += dt_seconds / s->seconds_per_day;
    s->day_fraction -= floorf(s->day_fraction);
    if (!(s->day_fraction >= 0.0f && s->day_fraction < 1.0f)) s->day_fraction = 0.0f;

    /* ── Surface response ────────────────────────────────────────────
     * Rain wets; snow does not (it covers).  Both fall back toward dry. */
    const bool raining = (s->weather_type == (uint32_t)JCE_ENV_WEATHER_RAIN)
                      && (s->precipitation_rate > 0.0f);
    const bool snowing = (s->weather_type == (uint32_t)JCE_ENV_WEATHER_SNOW)
                      && (s->precipitation_rate > 0.0f);

    if (raining) {
        s->global_wetness = env_approach(s->global_wetness, s->precipitation_rate,
                                         ENV_TAU_WETTING, dt_seconds);
    } else {
        s->global_wetness = env_approach(s->global_wetness, 0.0f,
                                         ENV_TAU_DRYING, dt_seconds);
    }

    /* Snow accumulates only where it is cold enough to survive landing, and
     * melts whenever it is not -- including with no precipitation at all,
     * which is how a snowfield disappears over a warm afternoon. */
    if (snowing && s->temperature_c <= ENV_SNOW_SETTLE_C) {
        s->snow_amount = env_approach(s->snow_amount, s->precipitation_rate,
                                      ENV_TAU_SNOWFALL, dt_seconds);
    } else if (s->temperature_c > ENV_SNOW_SETTLE_C) {
        s->snow_amount = env_approach(s->snow_amount, 0.0f,
                                      ENV_TAU_MELT, dt_seconds);
    }
    /* Cold and not snowing: lying snow simply stays. */

    s->global_wetness = env_clamp01(s->global_wetness);
    s->snow_amount    = env_clamp01(s->snow_amount);
}

/* ── weather-implied wind ────────────────────────────────────────────  */

void JCE_CALL jce_environment_apply_weather_wind(JceEnvironmentState *s)
{
    if (!s) return;
    const float intensity = env_clamp01(s->precipitation_rate);
    float speed = 0.0f;
    switch ((JceEnvWeatherType)s->weather_type) {
    case JCE_ENV_WEATHER_RAIN: speed = 3.0f * intensity; break;
    case JCE_ENV_WEATHER_SNOW: speed = 1.5f * intensity; break;
    default:                   speed = 0.0f;             break;
    }
    /* Calm weather is a BREEZE, not a vacuum.
     *
     * The floor exists so the per-field wind values a scene already authored
     * keep their meaning once they become multipliers of this (U4). With a
     * calm-weather base of 1 m/s, a grass field authored at 1.6 still moves at
     * 1.6 and an ocean at 7.5 still at 7.5 -- every existing look survives the
     * migration numerically unchanged -- while rain multiplies them instead of
     * being ignored. A base of zero would have stopped every blade of grass in
     * every scene the moment the migration landed, which is precisely the
     * regression the capability matrix warns this task carries.
     *
     * It is also true: still air is rare, and a world where nothing moves
     * unless it is raining reads as paused rather than calm. */
    if (speed < JCE_ENV_WIND_CALM_MPS) speed = JCE_ENV_WIND_CALM_MPS;
    s->wind_speed_mps = speed;
    /* Gustiness, in the units meteorology already uses: the gust factor
     * G = peak / mean. Over open terrain in neutral stratification G runs
     * about 1.2-1.5; convective, showery air is gustier still, 1.6-2.0. The
     * amplitude stored here is G - 1, so these numbers can be argued with as
     * gust factors instead of tuned until the grass looks busy:
     *
     *   clear        G 1.20            steady breeze, open ground
     *   snow         G 1.25 .. 1.50    frontal, less convective than rain
     *   rain         G 1.30 .. 1.70    showery air at full intensity
     *
     * Set here rather than in advance() for the reason the speed is: advance()
     * must not invent wind, or a scene that authors a steady gale could never
     * hold one. Unlike DIRECTION, which is left alone above, gustiness is not
     * a thing scenes author -- it is a property of the weather -- so it
     * belongs with the speed. */
    switch ((JceEnvWeatherType)s->weather_type) {
    case JCE_ENV_WEATHER_RAIN: s->wind_gust = 0.30f + 0.40f * intensity; break;
    case JCE_ENV_WEATHER_SNOW: s->wind_gust = 0.25f + 0.25f * intensity; break;
    default:                   s->wind_gust = 0.20f;                     break;
    }
    /* Direction is left alone: it is the field a scene or preset is most
     * likely to author, and clobbering it here would make authored wind
     * un-authorable the moment it started raining. */
}

/* ── derived views ───────────────────────────────────────────────────  */

/* Fog extinction.  See jce_environment.h for why the constants are quoted as
 * visibilities rather than as tuning values.
 *
 * Koschmieder: a coefficient s (1/m) gives meteorological visibility
 * V = 3.912 / s.  Writing the anchors this way is the whole point -- 7.8e-3
 * is unarguable on its own, "half a kilometre of visibility in saturated air"
 * is something a reader can agree or disagree with. */
#define JCE_ENV_FOG_CLEAR_EXT      1.30e-4f   /* V = 30 km  */
#define JCE_ENV_FOG_SATURATED_EXT  7.80e-3f   /* V = 500 m  */
#define JCE_ENV_FOG_PRECIP_EXT     3.90e-3f   /* V = 1 km, additive */
#define JCE_ENV_FOG_HUMIDITY_ONSET 0.60f      /* below this, air is not hazy */

/* Humidity implied by the weather.  See the header for why it is a separate
 * writer and why it does not lag.
 *
 * The targets are relative humidities, not tuning constants: precipitating air
 * is at or near saturation (that is what precipitation IS), and a clear day in
 * this engine's default world is the module default of 0.40.  Snow reaches a
 * slightly lower ceiling than rain because it falls from colder air, which
 * holds less water even when saturated. */
void JCE_CALL jce_environment_apply_weather_humidity(JceEnvironmentState *s)
{
    if (!s) return;

    float p = s->precipitation_rate;
    if (!(p == p)) p = 0.0f;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;

    float target;
    switch ((JceEnvWeatherType)s->weather_type) {
    case JCE_ENV_WEATHER_RAIN: target = 0.60f + 0.35f * p; break;   /* -> 0.95 */
    case JCE_ENV_WEATHER_SNOW: target = 0.60f + 0.28f * p; break;   /* -> 0.88 */
    case JCE_ENV_WEATHER_CLEAR:
    default:                   target = 0.40f;             break;   /* the default */
    }
    s->humidity = target;
}

float JCE_CALL jce_environment_fog_extinction(const JceEnvironmentState *s)
{
    /* NULL is absent, not clear: a caller that forgot the state gets no fog
     * rather than a believable amount of it. */
    if (!s) return 0.0f;

    const float onset = JCE_ENV_FOG_HUMIDITY_ONSET;
    float h = s->humidity;
    if (!(h == h)) h = 0.0f;                       /* NaN */
    if (h < 0.0f) h = 0.0f;
    if (h > 1.0f) h = 1.0f;

    /* Humidity -> haze, smooth from the onset so there is no visible step as
     * weather crosses it.  t*t is enough curvature to keep the low end clear;
     * a linear ramp made 70% humidity read as light fog, which it is not. */
    float haze = 0.0f;
    if (h > onset) {
        const float t = (h - onset) / (1.0f - onset);
        haze = (JCE_ENV_FOG_SATURATED_EXT - JCE_ENV_FOG_CLEAR_EXT) * t * t;
    }

    /* Precipitation adds on top rather than replacing: rain in already-humid
     * air is foggier than rain in dry air, which is the observed behaviour and
     * also what keeps the two inputs independently authorable. */
    float p = s->precipitation_rate;
    if (!(p == p)) p = 0.0f;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;

    return JCE_ENV_FOG_CLEAR_EXT + haze + JCE_ENV_FOG_PRECIP_EXT * p;
}

/* A smooth periodic wave in [0,1], exactly reproducible everywhere.
 *
 * sinf is deliberately NOT used. It is not bit-identical across libm
 * implementations, and this value reaches the grass shader, the cloud drift
 * accumulator and the rain overlay -- a replay on another machine would
 * diverge in the PICTURE while every stored state still matched, which is the
 * worst shape a determinism bug can take. A triangle folded through
 * smoothstep is C1 (smoothstep's derivative is zero exactly at the fold,
 * which is where the triangle's corner is) and uses only +, - and *, so it is
 * the same number on every IEEE-754 machine.
 *
 * floor() on the double is exact, so the phase does not drift as
 * world_time_seconds grows. */
static float env_wave01(double t_seconds, double period_s, double phase01)
{
    double u = t_seconds / period_s + phase01;
    u -= floor(u);
    const float tri = 1.0f - fabsf((float)(2.0 * u) - 1.0f);
    return tri * tri * (3.0f - 2.0f * tri);
}

/* The gust envelope, 0..1, as a pure function of the state's own clock.
 *
 * Three incommensurate periods rather than one: real gustiness is broadband,
 * and a single period reads as a machine breathing. The periods are chosen
 * against the scales that exist -- a gust is defined meteorologically as a
 * 3-second peak, while the energy-containing eddies near the ground turn over
 * in tens of seconds -- and weighted toward the slow one because that is
 * where the energy is. Their ratios do not produce a short common period, so
 * the sum does not visibly repeat within a session.
 *
 * The phases come from weather_seed, so two worlds under the same weather
 * gust differently, and the same world replays identically. */
static float env_gust_envelope(const JceEnvironmentState *s)
{
    const double   t = s->world_time_seconds;
    const uint32_t k = s->weather_seed;
    const double p0 = (double)((k * 2654435761u)      >> 8) / 16777216.0;
    const double p1 = (double)((k * 40503u + 12345u)  >> 8) / 16777216.0;
    const double p2 = (double)((k * 2246822519u)      >> 8) / 16777216.0;
    return 0.50f * env_wave01(t, 41.3, p0)
         + 0.30f * env_wave01(t, 17.9, p1)
         + 0.20f * env_wave01(t,  7.3, p2);
}

float JCE_CALL jce_environment_wind_speed_sustained(const JceEnvironmentState *s)
{
    if (!s) return 0.0f;
    return env_clampf(s->wind_speed_mps, 0.0f, 120.0f, 0.0f);
}

float JCE_CALL jce_environment_wind_speed_now(const JceEnvironmentState *s)
{
    if (!s) return 0.0f;
    /* Zero gust returns the sustained speed EXACTLY, not approximately.
     * Readers compare these values against each other, and a last-bit
     * difference would let one subsystem move while another stood still. The
     * envelope is multiplied by the amplitude, so an amplitude of zero
     * removes the term entirely rather than leaving a rounding of it. */
    const float base = env_clampf(s->wind_speed_mps, 0.0f, 120.0f, 0.0f);
    const float amp  = env_clamp01(s->wind_gust);
    if (amp <= 0.0f) return base;
    return base * (1.0f + amp * env_gust_envelope(s));
}

bool JCE_CALL jce_environment_is_daytime(const JceEnvironmentState *s)
{
    if (!s) return false;
    /* Strictly above the horizon.  Everything that picks a key light must use
     * this one predicate: two callers with their own thresholds would swap
     * the key light on different frames and the shadows would jump. */
    return s->sun_direction_ws.y > 0.0f;
}

float JCE_CALL jce_environment_key_illuminance(const JceEnvironmentState *s)
{
    if (!s) return 0.0f;
    return jce_environment_is_daytime(s) ? s->sun_illuminance_lux
                                         : s->moon_illuminance_lux;
}

jce_vec3 JCE_CALL jce_environment_key_direction(const JceEnvironmentState *s)
{
    if (!s) return jce_v3(0.0f, 1.0f, 0.0f);
    return jce_environment_is_daytime(s) ? s->sun_direction_ws
                                         : s->moon_direction_ws;
}
