#include "jce_scene_components_internal.h"

JceWaterNormalDetailComponent JCE_CALL jce_scene_water_normal_detail_default(void)
{
    JceWaterNormalDetailComponent value = {0};
    value.struct_size = sizeof(value);
    value.version = JCE_WATER_NORMAL_DETAIL_COMPONENT_VERSION;
    value.frequency_low = 6.0f;
    value.frequency_high = 16.0f;
    value.strength = 0.14f;
    return value;
}

static bool normal_detail_read(const cJSON *object, const char *key,
    double minimum, double maximum, float *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!item) return true;
    if (!cJSON_IsNumber(item) ||
        !(item->valuedouble >= minimum && item->valuedouble <= maximum))
        return false;
    *out = (float)item->valuedouble;
    return true;
}

void parse_water_normal_detail(JceScene *s, JceEntity e, const cJSON *c)
{
    JceWaterNormalDetailComponent value = jce_scene_water_normal_detail_default();
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(c, "version");
    if (version && (!cJSON_IsNumber(version) || version->valuedouble != value.version))
        return;
    if (!normal_detail_read(c, "frequencyLow", 0.1, 64.0, &value.frequency_low) ||
        !normal_detail_read(c, "frequencyHigh", 0.1, 64.0, &value.frequency_high) ||
        !normal_detail_read(c, "strength", 0.0, 1.0, &value.strength))
        return;
    jce_scene_set_water_normal_detail(s, e, &value);
}

void serw_water_normal_detail(JceScene *s, JceEntity e, cJSON *arr)
{
    const JceWaterNormalDetailComponent *value = jce_scene_get_water_normal_detail(s, e);
    if (!jce_water_normal_detail_valid(value)) return;
    cJSON *object = cJSON_CreateObject(), *properties = cJSON_CreateObject();
    if (!object || !properties ||
        !cJSON_AddStringToObject(object, "type", "WaterNormalDetail") ||
        !cJSON_AddNumberToObject(properties, "version", value->version) ||
        !cJSON_AddNumberToObject(properties, "frequencyLow", value->frequency_low) ||
        !cJSON_AddNumberToObject(properties, "frequencyHigh", value->frequency_high) ||
        !cJSON_AddNumberToObject(properties, "strength", value->strength) ||
        !cJSON_AddItemToObject(object, "properties", properties)) {
        cJSON_Delete(properties);
        cJSON_Delete(object);
        return;
    }
    if (!cJSON_AddItemToArray(arr, object)) cJSON_Delete(object);
}
