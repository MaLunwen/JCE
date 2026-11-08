#include "jce_scene_components_internal.h"

JceWaterOpticsComponent JCE_CALL jce_scene_water_optics_default(void)
{
    JceWaterOpticsComponent value = {0};
    value.struct_size = sizeof(value);
    value.version = JCE_WATER_OPTICS_COMPONENT_VERSION;
    return value;
}

static bool water_optics_read_strength(const cJSON *object, const char *key, float *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!item) return true;
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 0.0 && item->valuedouble <= 1.0))
        return false;
    *out = (float)item->valuedouble;
    return true;
}

void parse_water_optics(JceScene *s, JceEntity e, const cJSON *c)
{
    JceWaterOpticsComponent value = jce_scene_water_optics_default();
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(c, "version");
    if (version && (!cJSON_IsNumber(version) || version->valuedouble != value.version))
        return;
    if (!water_optics_read_strength(c, "ssrStrength", &value.ssr_strength) ||
        !water_optics_read_strength(c, "refractionStrength", &value.refraction_strength))
        return;
    jce_scene_set_water_optics(s, e, &value);
}

void serw_water_optics(JceScene *s, JceEntity e, cJSON *arr)
{
    const JceWaterOpticsComponent *value = jce_scene_get_water_optics(s, e);
    if (!jce_water_optics_valid(value)) return;
    cJSON *object = cJSON_CreateObject(), *properties = cJSON_CreateObject();
    if (!object || !properties ||
        !cJSON_AddStringToObject(object, "type", "WaterOptics") ||
        !cJSON_AddNumberToObject(properties, "version", value->version) ||
        !cJSON_AddNumberToObject(properties, "ssrStrength", value->ssr_strength) ||
        !cJSON_AddNumberToObject(properties, "refractionStrength", value->refraction_strength) ||
        !cJSON_AddItemToObject(object, "properties", properties)) {
        cJSON_Delete(properties);
        cJSON_Delete(object);
        return;
    }
    if (!cJSON_AddItemToArray(arr, object)) cJSON_Delete(object);
}
