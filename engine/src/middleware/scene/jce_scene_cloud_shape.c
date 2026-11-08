#include "jce_scene_components_internal.h"
#include "jce_scene_internal.h"
#include "jce_scene_sky_extras_json.h"
#include "renderer/jce_cloud_noise.h"

JceSceneCloudShapeSettings JCE_CALL jce_scene_cloud_shape_default(void)
{
    JceSceneCloudShapeSettings value = {0};
    value.struct_size = sizeof value;
    value.version = JCE_SCENE_CLOUD_SHAPE_VERSION;
    value.detail_erosion = JCE_CLOUD_DEFAULT_DETAIL_EROSION;
    return value;
}

const JceSceneCloudShapeSettings *JCE_CALL
jce_scene_get_cloud_shape_settings(const JceScene *scene)
{
    const JceSceneRenderLook *state = jce_scene_internal_get_render_look(scene);
    return state && state->cloud_shape.version ? &state->cloud_shape : NULL;
}

bool JCE_CALL jce_scene_set_cloud_shape_settings(JceScene *scene,
    const JceSceneCloudShapeSettings *value)
{
    JceSceneRenderLook *state = jce_scene_internal_render_look(scene);
    if (!state) return false;
    if (!value) {
        state->cloud_shape.version = 0;
        return true;
    }
    if (value->struct_size != sizeof *value ||
        value->version != JCE_SCENE_CLOUD_SHAPE_VERSION ||
        !(value->detail_erosion >= 0.0f && value->detail_erosion <= 0.95f) ||
        !(value->curl_metres >= 0.0f && value->curl_metres <= 256.0f))
        return false;
    jce_scene_get_rendering_settings_mut(scene);
    state->cloud_shape = *value;
    return true;
}

static bool cloud_shape_write(cJSON *sky, const JceSceneCloudShapeSettings *value)
{
    if (!value) return true;
    cJSON *obj = cJSON_CreateObject();
    if (obj && cJSON_AddNumberToObject(obj, "version", value->version) &&
        cJSON_AddNumberToObject(obj, "detailErosion", value->detail_erosion) &&
        cJSON_AddNumberToObject(obj, "curlMetres", value->curl_metres) &&
        cJSON_AddItemToObject(sky, "cloudShape", obj))
        return true;
    cJSON_Delete(obj);
    return false;
}

static bool cloud_shape_number(const cJSON *obj, const char *key, double max, float *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item) return true;
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 0.0 && item->valuedouble <= max))
        return false;
    *out = (float)item->valuedouble;
    return true;
}

static bool cloud_shape_read(const cJSON *rendering, JceSceneCloudShapeSettings *out)
{
    const cJSON *env = cJSON_GetObjectItemCaseSensitive(rendering, "environment");
    const cJSON *sky = cJSON_GetObjectItemCaseSensitive(env ? env : rendering, "sky");
    const cJSON *obj = cJSON_GetObjectItemCaseSensitive(sky, "cloudShape");
    if (!obj || cJSON_IsNull(obj)) return true;
    if (!cJSON_IsObject(obj)) return false;
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(obj, "version");
    if (version && (!cJSON_IsNumber(version) ||
                    version->valuedouble != JCE_SCENE_CLOUD_SHAPE_VERSION))
        return false;
    JceSceneCloudShapeSettings value = jce_scene_cloud_shape_default();
    if (!cloud_shape_number(obj, "detailErosion", 0.95, &value.detail_erosion) ||
        !cloud_shape_number(obj, "curlMetres", 256.0, &value.curl_metres))
        return false;
    *out = value;
    return true;
}

bool jce_scene_sky_extras_write_json(cJSON *sky, const JceScene *scene)
{
    return jce_scene_sky_celestial_write_json(sky, scene) &&
           cloud_shape_write(sky, jce_scene_get_cloud_shape_settings(scene));
}

bool jce_scene_sky_extras_read_json(const cJSON *rendering, JceSceneSkyExtras *out)
{
    *out = (JceSceneSkyExtras){0};
    const bool celestial_ok = jce_scene_sky_celestial_read_json(rendering, &out->celestial,
                                                                 &out->night_style);
    const bool cloud_shape_ok = cloud_shape_read(rendering, &out->cloud_shape);
    return celestial_ok && cloud_shape_ok;
}

void jce_scene_sky_extras_apply(JceScene *scene, const JceSceneSkyExtras *value)
{
    const JceSceneSkyCelestialSettings *celestial = &value->celestial;
    const JceSceneCloudShapeSettings *cloud_shape = &value->cloud_shape;
    jce_scene_set_sky_celestial_settings(scene, celestial->version ? celestial : NULL);
    JceSceneRenderLook *look = jce_scene_internal_render_look(scene);
    if (look) look->sky_night = value->night_style;
    jce_scene_set_cloud_shape_settings(scene, cloud_shape->version ? cloud_shape : NULL);
}
