/*
 * jce_weather.h -- screen-space weather overlay (rain/snow) plus
 * shared environmental parameters (wetness, wind) that other systems
 * (PBR, foliage, cloth) can read.
 *
 * The runtime keeps a single CPU-side state struct and renders a
 * camera-relative full-screen overlay each frame when active.  The
 * BRDF wetness coefficient is exposed here for shaders that want to
 * react (e.g. roughness reduction on wet surfaces) — wiring those
 * shaders is outside the scope of this module.
 */
#ifndef JCE_WEATHER_H
#define JCE_WEATHER_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JcePakArchive JcePakArchive;

typedef enum {
    JCE_WEATHER_CLEAR = 0,
    JCE_WEATHER_RAIN  = 1,
    JCE_WEATHER_SNOW  = 2
} JceWeatherType;

typedef struct {
    JceWeatherType type;
    float          intensity;       /* 0..1 */
    float          wetness;         /* 0..1 — surface BRDF darken/sharpen */
    jce_vec3       wind_dir;        /* world-space, normalized */
    float          wind_strength;   /* m/s */
    jce_vec3       tint;            /* rain/snow particle tint */
    float          alpha;           /* overlay alpha multiplier */
} JceWeatherState;

typedef struct {
    const JcePakArchive *pak;
} JceWeatherDesc;

typedef struct JceWeatherSystem JceWeatherSystem;

JCE_API JceWeatherSystem *jce_weather_create(const JceWeatherDesc *desc);
JCE_API void              jce_weather_destroy(JceWeatherSystem *w);

JCE_API void              jce_weather_set_state(JceWeatherSystem *w, const JceWeatherState *state);
JCE_API JceWeatherState   jce_weather_get_state(const JceWeatherSystem *w);

/* Advance internal time by dt seconds for animation. */
JCE_API void              jce_weather_update(JceWeatherSystem *w, float dt);

/* Render the screen-space overlay for the current state into view_id. */
JCE_API void              jce_weather_render(JceWeatherSystem *w, uint16_t view_id);

/* Helper: produce a sensible default state for the given type. */
JCE_API JceWeatherState   jce_weather_default(JceWeatherType type, float intensity);

#ifdef __cplusplus
}
#endif

#endif /* JCE_WEATHER_H */
