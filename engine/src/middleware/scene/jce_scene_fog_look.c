#include "jce_scene_fog_look_internal.h"

#include "jce_scene_components_internal.h"
#include <math.h>

static bool fog_look_read_number(const cJSON *detail, const char *name,
    double low, double high, float *out)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(detail, name);
    if (!value) return true;
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble)) return false;
    *out = (float)fmin(fmax(value->valuedouble, low), high);
    return true;
}

bool jce_scene_fog_look_read_json(const cJSON *rendering, JceSceneFogLookSettings *out)
{
    const cJSON *fog = cJSON_GetObjectItemCaseSensitive(rendering, "fog");
    const cJSON *detail = cJSON_GetObjectItemCaseSensitive(fog, "volumeDetail");
    *out = (JceSceneFogLookSettings){0};
    if (!detail || cJSON_IsNull(detail)) return true;
    if (!cJSON_IsObject(detail)) return false;

    JceVolumetricFogLook look = {
        .noise_cell_m = 24.0f,
        .forward_strength = 1.0f,
    };
    if (!fog_look_read_number(detail, "noiseStrength", 0.0, 1.0, &look.noise_strength) ||
        !fog_look_read_number(detail, "noiseCellM", 1.0, 256.0, &look.noise_cell_m) ||
        !fog_look_read_number(detail, "forwardStrength", 0.0, 4.0, &look.forward_strength) ||
        !fog_look_read_number(detail, "backwardStrength", 0.0, 4.0, &look.backward_strength) ||
        !fog_look_read_number(detail, "screenShaftStrength", 0.0, 4.0, &look.screen_shaft_strength) ||
        !fog_look_read_number(detail, "volumeRadiusM", 0.0, 100000.0, &look.volume_radius_m) ||
        !fog_look_read_number(detail, "volumeCenterX", -100000.0, 100000.0, &look.volume_center[0]) ||
        !fog_look_read_number(detail, "volumeCenterY", -100000.0, 100000.0, &look.volume_center[1]) ||
        !fog_look_read_number(detail, "volumeCenterZ", -100000.0, 100000.0, &look.volume_center[2]))
        return false;

    *out = (JceSceneFogLookSettings){true, look};
    return true;
}

bool jce_scene_fog_look_write_json(cJSON *fog, const JceScene *scene)
{
    const JceSceneRenderLook *state = jce_scene_internal_get_render_look(scene);
    if (!state || !state->fog.authored) return true;

    const JceVolumetricFogLook *look = &state->fog.look;
    cJSON *detail = cJSON_CreateObject();
    if (!detail) return false;
    if (cJSON_AddNumberToObject(detail, "noiseStrength", look->noise_strength) &&
        cJSON_AddNumberToObject(detail, "noiseCellM", look->noise_cell_m) &&
        cJSON_AddNumberToObject(detail, "forwardStrength", look->forward_strength) &&
        cJSON_AddNumberToObject(detail, "backwardStrength", look->backward_strength) &&
        (look->screen_shaft_strength == 0.0f ||
         cJSON_AddNumberToObject(detail, "screenShaftStrength", look->screen_shaft_strength)) &&
        (look->volume_radius_m == 0.0f ||
         cJSON_AddNumberToObject(detail, "volumeRadiusM", look->volume_radius_m)) &&
        (look->volume_center[0] == 0.0f ||
         cJSON_AddNumberToObject(detail, "volumeCenterX", look->volume_center[0])) &&
        (look->volume_center[1] == 0.0f ||
         cJSON_AddNumberToObject(detail, "volumeCenterY", look->volume_center[1])) &&
        (look->volume_center[2] == 0.0f ||
         cJSON_AddNumberToObject(detail, "volumeCenterZ", look->volume_center[2])) &&
        cJSON_AddItemToObject(fog, "volumeDetail", detail))
        return true;

    cJSON_Delete(detail);
    return false;
}

void jce_scene_fog_look_set(JceScene *scene, const JceSceneFogLookSettings *settings)
{
    JceSceneRenderLook *state = jce_scene_internal_render_look(scene);
    if (state) state->fog = settings ? *settings : (JceSceneFogLookSettings){0};
}

void jce_scene_fog_look_advance(JceScene *scene, const JceEnvironmentState *env, float dt)
{
    JceSceneRenderLook *state = jce_scene_internal_render_look(scene);
    if (!state || !state->fog.authored || !(state->fog.look.noise_strength > 0.0f) ||
        !env || !(dt > 0.0f) || !isfinite(dt))
        return;

    const double distance = (double)jce_environment_wind_speed_now(env) * dt;
    state->fog_wind_xz_m[0] += env->wind_direction_ws.x * distance;
    state->fog_wind_xz_m[1] += env->wind_direction_ws.z * distance;
}

void jce_scene_fog_look_bind(const JceScene *scene, JceVolumetricFog *fog)
{
    const JceSceneRenderLook *state = jce_scene_internal_get_render_look(scene);
    if (state && state->fog.authored)
        jce_volumetric_fog_set_look(fog, &state->fog.look, state->fog_wind_xz_m);
    else
        jce_volumetric_fog_set_look(fog, NULL, NULL);
}

float jce_scene_fog_look_screen_shafts(const JceScene *scene)
{
    const JceSceneRenderLook *state = jce_scene_internal_get_render_look(scene);
    return state && state->fog.authored ? state->fog.look.screen_shaft_strength : 0.0f;
}
