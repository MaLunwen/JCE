#include "jce_scene_components_internal.h"
#include "jce_scene_sky_celestial_internal.h"
#include "jce_scene_internal.h"

JceSceneSkyCelestialSettings JCE_CALL jce_scene_sky_celestial_default(void)
{
    JceSceneSkyCelestialSettings value = {0};
    value.struct_size = sizeof value;
    value.version = JCE_SCENE_SKY_CELESTIAL_VERSION;
    value.moon_phase = 1.0f;
    return value;
}

static bool in_range(double value, double high)
{
    return value >= 0.0 && value <= high;
}

const JceSceneSkyCelestialSettings *JCE_CALL jce_scene_get_sky_celestial_settings(
    const JceScene *scene)
{
    const JceSceneRenderLook *state = jce_scene_internal_get_render_look(scene);
    return state && state->sky_celestial.version ? &state->sky_celestial : NULL;
}

bool JCE_CALL jce_scene_set_sky_celestial_settings(JceScene *scene,
    const JceSceneSkyCelestialSettings *value)
{
    JceSceneRenderLook *state = jce_scene_internal_render_look(scene);
    if (!state) return false;
    JceSceneSkyCelestialSettings *slot = &state->sky_celestial;
    if (!value) {
        slot->version = 0;
        state->sky_night = (JceSceneSkyNightStyle){0};
        return true;
    }
    if (value->struct_size != sizeof *value ||
        value->version != JCE_SCENE_SKY_CELESTIAL_VERSION ||
        !in_range(value->star_intensity, 4.0) ||
        !in_range(value->moon_intensity, 4.0) ||
        !in_range(value->solar_flare_intensity, 4.0) ||
        !in_range(value->moon_phase, 1.0))
        return false;
    jce_scene_get_rendering_settings_mut(scene);
    *slot = *value;
    return true;
}

JceSceneSkyNightStyle jce_scene_sky_night_style(const JceScene *scene)
{
    const JceSceneRenderLook *state = jce_scene_internal_get_render_look(scene);
    return state ? state->sky_night : (JceSceneSkyNightStyle){0};
}

bool jce_scene_sky_celestial_write_json(cJSON *sky, const JceScene *scene)
{
    const JceSceneSkyCelestialSettings *value = jce_scene_get_sky_celestial_settings(scene);
    if (!value) return true;
    const JceSceneSkyNightStyle style = jce_scene_sky_night_style(scene);
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return false;
    if (cJSON_AddNumberToObject(obj, "version", value->version) &&
        cJSON_AddNumberToObject(obj, "starIntensity", value->star_intensity) &&
        cJSON_AddNumberToObject(obj, "moonIntensity", value->moon_intensity) &&
        cJSON_AddNumberToObject(obj, "solarFlareIntensity", value->solar_flare_intensity) &&
        cJSON_AddNumberToObject(obj, "moonPhase", value->moon_phase) &&
        (style.moon_radius_degrees == 0.0f ||
         cJSON_AddNumberToObject(obj, "moonRadiusDegrees", style.moon_radius_degrees)) &&
        (style.gradient_strength == 0.0f ||
         cJSON_AddNumberToObject(obj, "nightGradientStrength", style.gradient_strength)) &&
        cJSON_AddItemToObject(sky, "celestial", obj))
        return true;
    cJSON_Delete(obj);
    return false;
}

static bool read_number(const cJSON *obj, const char *name, double high, float *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!item) return true;
    if (!cJSON_IsNumber(item) || !in_range(item->valuedouble, high)) return false;
    *out = (float)item->valuedouble;
    return true;
}

bool jce_scene_sky_celestial_read_json(const cJSON *rendering,
    JceSceneSkyCelestialSettings *out, JceSceneSkyNightStyle *style)
{
    const cJSON *env = cJSON_GetObjectItemCaseSensitive(rendering, "environment");
    const cJSON *sky = cJSON_GetObjectItemCaseSensitive(env ? env : rendering, "sky");
    const cJSON *obj = cJSON_GetObjectItemCaseSensitive(sky, "celestial");
    out->version = 0;
    *style = (JceSceneSkyNightStyle){0};
    if (!obj || cJSON_IsNull(obj)) return true;
    if (!cJSON_IsObject(obj)) return false;

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(obj, "version");
    if (version && (!cJSON_IsNumber(version) ||
                    version->valuedouble != JCE_SCENE_SKY_CELESTIAL_VERSION))
        return false;
    JceSceneSkyCelestialSettings value = jce_scene_sky_celestial_default();
    JceSceneSkyNightStyle night = {0};
    if (!read_number(obj, "starIntensity", 4.0, &value.star_intensity) ||
        !read_number(obj, "moonIntensity", 4.0, &value.moon_intensity) ||
        !read_number(obj, "solarFlareIntensity", 4.0, &value.solar_flare_intensity) ||
        !read_number(obj, "moonPhase", 1.0, &value.moon_phase) ||
        !read_number(obj, "moonRadiusDegrees", 3.0, &night.moon_radius_degrees) ||
        !read_number(obj, "nightGradientStrength", 2.0, &night.gradient_strength))
        return false;
    *out = value;
    *style = night;
    return true;
}
