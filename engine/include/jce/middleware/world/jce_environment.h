/*
 * jce_environment.h -- the authoritative world environment state.
 *
 * WHY THIS EXISTS.  Before it, the engine carried at least six partial,
 * mutually-unaware descriptions of "what the weather is doing":
 *
 *   JceTimeOfDayState   sun direction/colour, sky colours, fog, exposure
 *   JceWeatherState     type, intensity, wetness, wind
 *   JceAtmosphereParams the physical atmosphere
 *   scene render settings  cloud coverage/density, fog, sky mode
 *   the ocean spectrum  its OWN wind direction and speed
 *   vegetation/terrain  its OWN wind
 *
 * Nothing reconciled them.  The scene renderer did not even read the weather
 * system's simulated state -- it built a fresh one from a default each frame
 * (jce_sr_environment.c) -- so the wind that moved the grass, the wind that
 * drove the ocean spectrum, and the wind the weather system was simulating
 * were three unrelated numbers that happened to sit in the same world.
 *
 * THE RULE.  There is exactly one environment state.  Sky, cloud, fog, water
 * and vegetation READ it; they do not each keep their own.  Weather
 * controllers, timelines and scripts WRITE it and nothing else.  Every
 * derived quantity is a pure function of it, so two subsystems that read the
 * same state agree by construction rather than by anyone remembering to keep
 * them in sync.
 *
 * DETERMINISM.  Advancing the state is a pure function of (state, dt).  It
 * holds no clock, no RNG and no GPU resource, so a headless or replaying
 * build steps the same environment the renderer does, bit for bit.
 */
#ifndef JCE_ENVIRONMENT_H
#define JCE_ENVIRONMENT_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Kept numerically identical to JceWeatherType so the two can be converted
 * without a lookup.  Asserted in the tests rather than assumed. */
typedef enum {
    JCE_ENV_WEATHER_CLEAR = 0,
    JCE_ENV_WEATHER_RAIN  = 1,
    JCE_ENV_WEATHER_SNOW  = 2
} JceEnvWeatherType;

typedef struct JceEnvironmentState {
    /* ── Clock ───────────────────────────────────────────────────────
     * world_time_seconds is monotonic and unbounded; day_fraction is its
     * position within the current day in [0,1).  Both are stored rather
     * than one derived on read, because the day length is authored and a
     * reader that recomputed it would need to know that too. */
    double   world_time_seconds;
    float    day_fraction;
    float    seconds_per_day;      /* > 0; 86400 is one real day */

    /* ── Celestial ───────────────────────────────────────────────────
     * Directions point FROM the world TOWARD the body, matching
     * JceTimeOfDayState::sun_direction so the two cannot disagree about
     * sign -- a mistake that renders as light arriving from underground. */
    jce_vec3 sun_direction_ws;
    float    sun_illuminance_lux;
    jce_vec3 moon_direction_ws;
    float    moon_illuminance_lux;

    /* ── Wind ────────────────────────────────────────────────────────
     * ONE wind.  The ocean spectrum, the vegetation shader and the weather
     * overlay all read this; none of them keeps its own any more. */
    jce_vec3 wind_direction_ws;    /* normalised */
    float    wind_speed_mps;       /* SUSTAINED speed, gust excluded */
    /* Gust AMPLITUDE: the peak fraction above the sustained speed, 0..1.
     *
     * Not the instantaneous excess. The instant value is
     * amplitude * envelope(world_time_seconds), applied by
     * jce_environment_wind_speed_now() -- so the state stays constant while
     * the weather does, and the fluctuation is a pure function of the clock
     * that is already in the state rather than a second thing to integrate.
     *
     * This field had NO WRITER for its whole existence: nothing outside the
     * sanitiser and the tests ever assigned it, so it held
     * jce_environment_default()'s 0 in every shipping frame and
     * wind_speed_now() was identically wind_speed_mps -- the (1 + gust)
     * multiply was a guaranteed multiply by one.
     * jce_environment_apply_weather_wind is the writer now, alongside the
     * speed it already set. */
    float    wind_gust;

    /* ── Cloud ───────────────────────────────────────────────────────  */
    float    cloud_coverage;       /* 0..1 -- fraction of sky with cloud */
    float    cloud_density;        /* > 0 */
    float    cloud_precipitation;  /* 0..1 -- storm/precipitating fraction */

    /* ── Atmosphere / precipitation ──────────────────────────────────  */
    float    humidity;             /* 0..1 */
    float    fog_density;          /* 1/m */
    float    precipitation_rate;   /* 0..1 */
    float    temperature_c;

    /* ── Accumulated surface response ────────────────────────────────
     * These LAG the weather rather than tracking it: ground stays wet after
     * rain stops and snow melts slowly.  jce_environment_advance integrates
     * them, which is why advancing must be the only way time passes. */
    float    global_wetness;       /* 0..1 */
    float    snow_amount;          /* 0..1 */

    uint32_t weather_type;         /* JceEnvWeatherType */
    uint32_t weather_seed;

    /* Authored hour this clock was last seeded from, in [0,24).
     *
     * The seed and the live value have to be two numbers or the distinction
     * cannot be made: with one, a running clock looks like an authoring edit on
     * its second frame and re-seeds itself forever, which is exactly why the
     * renderer's private copy needed a tod_authored_hour beside its
     * tod_clock_hour.  It lives HERE, per scene, and not as a file static in
     * the advance function, because the editor and its Play session are two
     * live scenes at once and a process-wide seed would let one re-seed the
     * other's clock.
     *
     * Negative means "never seeded", which is not a reachable hour and so needs
     * no separate valid flag.  APPENDED. */
    float    day_seed_hour;
} JceEnvironmentState;

/* A clear, windless noon on Earth.  Every field is finite and in range, so a
 * caller can take this and change only what it cares about. */
JCE_API JceEnvironmentState JCE_CALL jce_environment_default(void);

/* Force every field into its valid range.  Call after any external write
 * (script, scene load, UI) -- the derived accessors below assume sanity and a
 * NaN loose in the wind direction reaches the ocean spectrum, the vegetation
 * shader and the weather overlay in the same frame. */
JCE_API void JCE_CALL jce_environment_sanitize(JceEnvironmentState *s);

/* Advance by dt seconds: moves the clock, wraps day_fraction, and integrates
 * wetness and snow toward what the current precipitation implies.
 *
 * Pure in (state, dt): no clock is read, no RNG is drawn.  dt <= 0 or
 * non-finite is a no-op rather than a rewind. */
JCE_API void JCE_CALL jce_environment_advance(JceEnvironmentState *s, float dt_seconds);

/* Set the wind implied by the current weather, for a world that authors none.
 *
 * A WRITER, deliberately separate from advance(): the state does not invent
 * its own wind, because the moment a scene, a weather preset or a script wants
 * to author it, an advance() that overwrote wind every frame would be
 * unfixable without changing what advance means.
 *
 * The rule is the one the weather overlay used to apply privately (rain
 * 3 m/s per unit intensity, snow 1.5) -- reproduced here rather than dropped,
 * because moving the overlay onto the environment WITHOUT it is precisely how
 * the wind silently became zero and precipitation started falling vertically.
 * A crude rule with one home beats a crude rule copied into three subsystems,
 * and it is replaceable by a real preset without touching any consumer. */
/* Wind speed in calm weather, in m/s.
 *
 * Not a tuning value: it is the unit that makes a per-field wind number a
 * MULTIPLIER of this one without changing what that number already meant. A
 * grass field authored at 1.6 keeps moving at 1.6 in calm weather and at 4.8
 * in rain, instead of stopping dead the day the fields stop carrying their own
 * wind. */
#define JCE_ENV_WIND_CALM_MPS 1.0f

/* Also sets wind_gust, for the same reason and with the same standing: the
 * gustiness implied by the weather, for a world that authors none. Direction
 * remains untouched -- see the implementation for why the three fields are
 * not treated alike. */
JCE_API void JCE_CALL jce_environment_apply_weather_wind(JceEnvironmentState *s);

/* Set the humidity implied by the current weather.
 *
 * A sibling of the wind writer above and separate for the same reason: the
 * state must not invent humidity inside advance(), or a scene that wants to
 * author a dry thunderstorm or a fogbank with no rain could never do it.
 *
 * Instantaneous rather than integrated, unlike wetness. Air IS humid while it
 * is raining -- the lag people actually see is the ground drying afterwards,
 * and advance() already integrates that. Adding a second lag here would make
 * fog trail the rain by a time constant nobody authored. */
JCE_API void JCE_CALL jce_environment_apply_weather_humidity(JceEnvironmentState *s);

/* ── Derived views ───────────────────────────────────────────────────
 * Pure functions of the state.  These exist so subsystems stop deriving
 * their own: two callers of the same accessor cannot disagree. */

/* Instantaneous wind speed: the sustained speed plus the gust of this moment.
 *
 * Pure in the state -- it varies with time only because world_time_seconds IS
 * part of the state -- so two subsystems reading it in the same frame still
 * get the same number, which is the property the one-environment design
 * exists for.
 *
 * DO NOT key a cache or a rebuild on this. It changes every frame by design,
 * and anything that compares it against a stored copy will rebuild every
 * frame. The ocean spectrum learned this the expensive way; it takes the
 * sustained speed below, which is also the physically right input. */
JCE_API float JCE_CALL jce_environment_wind_speed_now(const JceEnvironmentState *s);

/* Sustained wind speed, gust deliberately EXCLUDED.
 *
 * For readers that model something with a memory longer than a gust. A wave
 * spectrum is the case that forced this to exist: Phillips/JONSWAP is
 * parameterised by the wind that has blown over the fetch for hours, and a
 * seven-second gust does not restructure a developed sea. Feeding it the
 * instantaneous speed is wrong twice over -- it is not the quantity the
 * spectrum takes, and it re-solves the entire FFT spectrum every frame
 * because the spectrum cache keys on exactly this number. */
JCE_API float JCE_CALL jce_environment_wind_speed_sustained(const JceEnvironmentState *s);

/* Atmospheric extinction coefficient, in 1/m, implied by the weather.
 *
 * Feeds volumetric fog. Returned as EXTINCTION rather than as a "fog density
 * slider" so the number means something a person can check: extinction and
 * meteorological visibility are related by Koschmieder's law, V = 3.912 / s,
 * so every value below is quoted as the visibility it produces and can be
 * argued with on those terms instead of tuned until a screenshot looks right.
 *
 *   clear air        V = 30 km   ->  1.3e-4
 *   saturated air    V = 500 m   ->  7.8e-3
 *   heavy precip     V = 1 km    ->  3.9e-3   (added on top)
 *
 * Humidity below JCE_ENV_FOG_HUMIDITY_ONSET contributes nothing: air at 40%
 * relative humidity is not hazy, and a mapping that starts at zero would make
 * every clear scene faintly milky and get "fixed" by someone turning fog off
 * entirely.
 *
 * Temperature is deliberately NOT an input. Real fog formation is a dew-point
 * problem, and approximating it from temperature alone would add a knob whose
 * effect nobody could predict; humidity here already stands for "how close the
 * air is to condensing".
 *
 * Returns 0 for a NULL state -- absent, not "clear", so a caller that forgot
 * to pass the state gets no fog rather than a plausible amount of it. */
JCE_API float JCE_CALL jce_environment_fog_extinction(const JceEnvironmentState *s);

/* True when the sun is above the horizon, i.e. the sun is the key light.
 * The moon takes over below it; callers that pick a key light must agree on
 * where the switch happens or shadows swap direction mid-frame. */
JCE_API bool JCE_CALL jce_environment_is_daytime(const JceEnvironmentState *s);

/* Illuminance of whichever body is currently the key light, in lux. */
JCE_API float JCE_CALL jce_environment_key_illuminance(const JceEnvironmentState *s);

/* Direction toward the current key light. */
JCE_API jce_vec3 JCE_CALL jce_environment_key_direction(const JceEnvironmentState *s);

/* ── Scene-driven environment ────────────────────────────────────────
 *
 * jce_scene_environment() has said "on the SCENE and not on the renderer" for
 * a while, and the STATE moved.  What did not move is the only thing that
 * makes it tick: jce_environment_advance() had exactly one caller in the whole
 * repository, inside the scene RENDERER.  A headless build has no renderer --
 * "no window, no GPU device", which is the dedicated-server mode the engine
 * ships -- so on a server world_time_seconds never advanced, the gust envelope
 * was frozen at its phase-zero value, and global_wetness / snow_amount never
 * integrated at all.  The state was scene-owned and renderer-driven, which is
 * the same defect one level down.
 *
 * Time of day was worse, because it had a SECOND clock: the renderer kept a
 * private tod_clock_hour seeded from the authored hour, and the editor
 * advanced the AUTHORED field in place from ImGui's frame time.  Two clocks,
 * two owners, two rates, and nothing a game could read: `time_of_day` appears
 * zero times in contracts/script-api.json.
 *
 * So: one advance, on the scene, callable with or without a renderer. */

typedef struct JceScene JceScene;

/* Sync the scene's authored rendering settings into its environment and
 * integrate one step of dt seconds.
 *
 * Call once per frame per scene, BEFORE anything reads the environment.
 * jce_scene_update() does it for the runtime (so a dedicated server is now
 * driven); the editor calls it directly while NOT playing, because edit mode
 * runs no simulation and its preview clock would otherwise stop.  No-op on a
 * NULL scene or a non-positive dt, so a paused frame does not integrate. */
JCE_API void JCE_CALL jce_scene_environment_advance(JceScene *scene, float dt);

/* Live hour of day in [0, 24) -- what the sky is actually showing right now,
 * not the authored seed in JceSceneRenderingSettings::tod_hour.
 *
 * This is the accessor gameplay wants ("is it night?") and the one that did
 * not exist: the live value was a private field of JceSceneRenderer, so the
 * only way to reach it needed a renderer. Returns the authored hour when the
 * scene has settings but time-of-day is disabled, and 0 for a NULL scene. */
JCE_API float JCE_CALL jce_scene_environment_hour(JceScene *scene);

/* Set the live hour, wrapping into [0, 24).  For save-game restore and for
 * gameplay that jumps time ("sleep until dawn").  Does not touch the authored
 * seed, so reloading the scene still starts where the designer set it. */
JCE_API void JCE_CALL jce_scene_environment_set_hour(JceScene *scene, float hour);

#ifdef __cplusplus
}
#endif

#endif /* JCE_ENVIRONMENT_H */
