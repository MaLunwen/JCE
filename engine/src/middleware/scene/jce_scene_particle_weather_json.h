#ifndef JCE_SCENE_PARTICLE_WEATHER_JSON_H
#define JCE_SCENE_PARTICLE_WEATHER_JSON_H

#include "jce_scene_internal.h"
#include "jce_scene_components_internal.h"
#include <float.h>
#include <string.h>

static bool scene_weather_write_precipitation(cJSON *weather, const JceScene *scene)
{
    const JceSceneParticleState *state = jce_scene_internal_particle_state(scene);
    if (!state) return true;
    const JceScenePrecipitationSettings *p = &state->precipitation;
    return (!p->use_emitters || cJSON_AddStringToObject(weather, "particleSource", "emitters")) &&
        (!(p->wetting_seconds > 0.0f) ||
            cJSON_AddNumberToObject(weather, "wettingSeconds", p->wetting_seconds)) &&
        (!(p->snowfall_seconds > 0.0f) ||
            cJSON_AddNumberToObject(weather, "snowfallSeconds", p->snowfall_seconds));
}

static bool scene_weather_read_seconds(const cJSON *weather, const char *key, float *out)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(weather, key);
    if (!value || cJSON_IsNull(value)) return true;
    if (!cJSON_IsNumber(value) || !(value->valuedouble >= 1.0 && value->valuedouble <= 600.0))
        return false;
    *out = (float)value->valuedouble;
    return true;
}

static bool scene_weather_read_precipitation(const cJSON *rendering,
                                              JceScenePrecipitationSettings *out)
{
    const cJSON *env = cJSON_GetObjectItemCaseSensitive(rendering, "environment");
    const cJSON *weather = cJSON_GetObjectItemCaseSensitive(env, "weather");
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(weather, "particleSource");
    *out = (JceScenePrecipitationSettings){0};
    if (source && !cJSON_IsNull(source)) {
        if (!cJSON_IsString(source) || !source->valuestring) return false;
        out->use_emitters = strcmp(source->valuestring, "emitters") == 0;
        if (!out->use_emitters && strcmp(source->valuestring, "overlay") != 0) return false;
    }
    return scene_weather_read_seconds(weather, "wettingSeconds", &out->wetting_seconds) &&
           scene_weather_read_seconds(weather, "snowfallSeconds", &out->snowfall_seconds);
}

static void scene_weather_apply_precipitation(JceScene *scene,
                                               const JceScenePrecipitationSettings *value)
{
    JceSceneParticleState *state = jce_scene_internal_particle_state_mut(scene);
    if (state) state->precipitation = value ? *value : (JceScenePrecipitationSettings){0};
}

static bool scene_snow_look_read_number(const cJSON *look, const char *key,
                                         float low, float high, float *out)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(look, key);
    if (!value || cJSON_IsNull(value)) return true;
    if (!cJSON_IsNumber(value) || !(fabs(value->valuedouble) <= FLT_MAX)) return false;
    const float number = (float)value->valuedouble;
    if (number < low || number > high) return false;
    *out = number;
    return true;
}

static bool scene_snow_look_read(const cJSON *rendering, JceSceneTerrainSnowLook *out)
{
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(rendering, "environment");
    const cJSON *weather = cJSON_GetObjectItemCaseSensitive(environment, "weather");
    const cJSON *look = cJSON_GetObjectItemCaseSensitive(weather, "snowLook");
    *out = (JceSceneTerrainSnowLook){0};
    if (!look || cJSON_IsNull(look)) return true;
    return cJSON_IsObject(look) &&
        scene_snow_look_read_number(look, "brightness", -0.4f, 0.08f, &out->brightness) &&
        scene_snow_look_read_number(look, "warmth", -1.0f, 1.0f, &out->warmth) &&
        scene_snow_look_read_number(look, "lowlandTint", 0.0f, 1.0f, &out->lowland_tint) &&
        scene_snow_look_read_number(look, "microNormalStrength", 0.0f, 1.0f, &out->micro_normal_strength);
}

static bool scene_snow_look_write(cJSON *weather, const JceScene *scene)
{
    const JceSceneRenderLook *state = jce_scene_internal_get_render_look(scene);
    if (!state) return true;
    const JceSceneTerrainSnowLook *look = &state->terrain_snow;
    if (look->brightness == 0.0f && look->warmth == 0.0f &&
        look->lowland_tint == 0.0f && look->micro_normal_strength == 0.0f) return true;

    cJSON *object = cJSON_CreateObject();
    if (!object) return false;
    if (cJSON_AddNumberToObject(object, "brightness", look->brightness) &&
        cJSON_AddNumberToObject(object, "warmth", look->warmth) &&
        cJSON_AddNumberToObject(object, "lowlandTint", look->lowland_tint) &&
        cJSON_AddNumberToObject(object, "microNormalStrength", look->micro_normal_strength) &&
        cJSON_AddItemToObject(weather, "snowLook", object)) return true;

    cJSON_Delete(object);
    return false;
}

static void scene_snow_look_set(JceScene *scene, const JceSceneTerrainSnowLook *look)
{
    JceSceneRenderLook *state = jce_scene_internal_render_look(scene);
    if (state) state->terrain_snow = look ? *look : (JceSceneTerrainSnowLook){0};
}

#endif
