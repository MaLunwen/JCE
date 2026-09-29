/*
 * jce_scene_environment.c  The scene drives its own environment.
 *
 * WHAT MOVED AND WHY.  jce_scene_environment() has said "on the SCENE and not
 * on the renderer" since the wind campaign, and the STATE did move.  What
 * stayed behind was the only thing that makes it tick: jce_environment_advance
 * had exactly ONE caller in the repository, inside sr_advance_environment_state
 * in the scene RENDERER.  A headless build creates no renderer -- the engine
 * logs "HEADLESS boot: no window, no GPU device, no audio/UI" and that is the
 * shipped dedicated-server mode -- so on a server:
 *
 *   - world_time_seconds never advanced, so the gust envelope, which is a pure
 *     function of it, stayed pinned to its phase-zero value forever;
 *   - global_wetness and snow_amount are integrators and integrated nothing,
 *     so ground never got wet and snow never settled or melted;
 *   - every derived view built on those returned the same answer forever.
 *
 * Scene-owned state advanced only by the renderer is the same defect the state
 * move was made to fix, one level down.
 *
 * TIME OF DAY HAD TWO CLOCKS.  The renderer kept a private tod_clock_hour,
 * seeded from the authored tod_hour and advanced at tod_speed; the editor
 * advanced the AUTHORED FIELD in place from ImGui's frame delta, and only while
 * its Time-of-Day tab happened to be on screen.  So the hour ran at the UI rate
 * in the editor and the render rate in a game, the editor marked the scene
 * dirty every frame it did so (Ctrl+S then baked whatever hour it had reached
 * into the authored value), and a dedicated server had no clock at all.
 * Neither clock was reachable by gameplay: `time_of_day` occurs zero times in
 * contracts/script-api.json.
 *
 * JceEnvironmentState already carried the clock this needed -- world_time_
 * seconds, day_fraction, seconds_per_day -- so this is a UNIFICATION, not a
 * third clock.  The authored settings stay the SEED; the environment holds the
 * live value; the renderer and the editor both read it.
 *
 * Layer: middleware/scene (L4).  Pure C99, and public API only -- this file
 * cannot see struct JceScene, so it cannot grow a private copy of anything.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/world/jce_environment.h>
#include <jce/middleware/world/jce_time_of_day.h>

#include <math.h>

/* Wrap into [0, 24).  fmodf keeps the sign, hence the second add; authored
 * values reach here straight from a text field, NaN included. */
static float env_wrap_hour(float h)
{
    if (!(h == h)) return 0.0f;                 /* NaN */
    h = fmodf(h, 24.0f);
    if (h < 0.0f) h += 24.0f;
    return h;
}

/* Non-const scene, deliberately: jce_scene_environment() lazily initialises the
 * state on first touch, so a const-qualified reader would have to cast the
 * const away to call it -- and would then be mutating through a pointer it
 * promised not to.  Every other scene accessor in this engine takes a mutable
 * scene for the same reason. */
float jce_scene_environment_hour(JceScene *scene)
{
    if (!scene) return 0.0f;

    const JceSceneRenderingSettings *rs = jce_scene_get_rendering_settings(scene);
    /* Time-of-day off: the sky is lit from the authored moment and never moves,
     * so THAT is the live hour.  Reporting the environment's own day_fraction
     * here would answer with a clock nothing is showing. */
    if (rs && !rs->tod_enabled) return env_wrap_hour(rs->tod_hour);

    const JceEnvironmentState *env = jce_scene_environment(scene);
    if (!env) return rs ? env_wrap_hour(rs->tod_hour) : 0.0f;
    return env_wrap_hour(env->day_fraction * 24.0f);
}

void jce_scene_environment_set_hour(JceScene *scene, float hour)
{
    JceEnvironmentState *env = jce_scene_environment(scene);
    if (!env) return;

    const float h = env_wrap_hour(hour);
    env->day_fraction = h / 24.0f;
    /* Keep the monotonic clock consistent with the fraction instead of letting
     * them disagree: world_time_seconds is what the gust envelope reads, and an
     * hour jump that did not move it shows up as the wind briefly running on
     * the old time. */
    const float spd = env->seconds_per_day > 0.0f ? env->seconds_per_day
                                                  : 86400.0f;
    env->world_time_seconds = (double)env->day_fraction * (double)spd;
}

void jce_scene_environment_advance(JceScene *scene, float dt)
{
    if (!scene || !(dt > 0.0f)) return;      /* a paused frame integrates nothing */

    JceEnvironmentState *env = jce_scene_environment(scene);
    if (!env) return;

    const JceSceneRenderingSettings *rs = jce_scene_get_rendering_settings(scene);
    if (rs) {
        /* ── Authored settings -> environment ─────────────────────────
         * These are the writes sr_advance_environment_state did before it
         * advanced.  They belong on this side of the line because their SOURCE
         * is the scene's serialized settings; the renderer was only where the
         * code happened to sit. */
        env->weather_type       = (uint32_t)rs->weather_type;
        env->precipitation_rate = rs->weather_intensity;
        /* Temperature is written unconditionally: unlike the wind direction
         * there is no value that means "not authored" -- 0 C is a temperature
         * -- and the settings default is the environment's own 15 C, so this is
         * a no-op for every scene that does not care. */
        env->temperature_c      = rs->temperature_c;

        if (rs->wind_direction_x != 0.0f || rs->wind_direction_z != 0.0f) {
            const float wx = rs->wind_direction_x, wz = rs->wind_direction_z;
            const float len = sqrtf(wx * wx + wz * wz);
            if (len > 1e-6f)
                env->wind_direction_ws = jce_v3(wx / len, 0.0f, wz / len);
        }

        /* Wind and humidity BEFORE advance: both are read in the same frame by
         * the weather overlay and the fog extinction, and a writer that ran
         * after would deliver last frame's value. */
        jce_environment_apply_weather_wind(env);
        jce_environment_apply_weather_humidity(env);

        /* ── The day clock ───────────────────────────────────────────
         * tod_speed is authored in HOURS per real second; the environment
         * counts SECONDS PER DAY.  One is 24 / the other, derived here every
         * frame rather than stored twice -- two numbers for one rate is how the
         * two clocks came to disagree in the first place. */
        if (rs->tod_enabled) {
            const float seed = env_wrap_hour(rs->tod_hour);
            if (rs->tod_speed > 0.0f) {
                env->seconds_per_day = 24.0f / rs->tod_speed;
                /* Seed once, then let it run.  Compare against the seed we last
                 * applied, NOT against the live hour: a running clock differs
                 * from the authored hour by design on its second frame, and
                 * comparing to it would re-seed forever and the day would never
                 * advance.  This is the distinction the renderer needed a
                 * private tod_authored_hour for; it is now per-scene state in
                 * the environment, so the editor's scene and its Play session
                 * cannot re-seed each other. */
                if (fabsf(seed - env->day_seed_hour) > 1.0e-4f) {
                    jce_scene_environment_set_hour(scene, seed);
                    env->day_seed_hour = seed;
                }
            } else {
                /* tod_speed == 0 is "frozen at the authored hour" -- a real
                 * authoring choice (static lighting from a chosen moment), and
                 * expressed by pinning rather than by an infinitely long day.
                 * Pinning every frame also means dragging the hour slider is
                 * visible immediately, which is the only time that slider is
                 * enabled in the editor. */
                jce_scene_environment_set_hour(scene, seed);
                env->day_seed_hour = seed;
            }

            /* ── Where the sun is ────────────────────────────────────
             * jce_environment_advance moves day_fraction and nothing else; it
             * does NOT place the sun.  The only writer of sun_direction_ws was
             * the renderer, which derives it from the primary directional light
             * -- itself driven by the time-of-day state the renderer had just
             * pushed.  Headless there is no renderer, so the direction stayed
             * at jce_environment_default()'s, whose sun is above the horizon:
             * jce_environment_is_daytime() -- the predicate gameplay asks "is
             * it night?" with, and the one every key-light chooser is required
             * to agree on -- answered TRUE forever on a dedicated server.
             *
             * The scene knows the hour and the authored sun arc, so it can say.
             * Only when time-of-day is enabled: with it off the authored light
             * IS the sun, and asserting a computed direction over it would
             * overwrite an authored value with a derived one. */
            JceTimeOfDayConfig cfg = jce_time_of_day_default_config();
            cfg.latitude_degrees = rs->tod_latitude;
            cfg.dawn_hour        = rs->tod_dawn_hour;
            cfg.dusk_hour        = rs->tod_dusk_hour;

            JceTimeOfDayState tod;
            jce_time_of_day_evaluate(&cfg, jce_scene_environment_hour(scene),
                                     &tod);
            env->sun_direction_ws  = tod.sun_direction;
            /* Moon exactly opposite, the convention jce_environment_default
             * already encodes.  A real ephemeris is a separate feature; a
             * WRONG moon would be worse than this one, because the shadow it
             * casts would be defensibly placed and still incorrect. */
            env->moon_direction_ws = jce_v3(-tod.sun_direction.x,
                                            -tod.sun_direction.y,
                                            -tod.sun_direction.z);
        }
    }

    jce_environment_advance(env, dt);
}
