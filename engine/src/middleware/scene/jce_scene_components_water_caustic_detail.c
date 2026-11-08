#include "jce_scene_components_internal.h"

static const float caustic_detail_fine_default_depth = 1.25f;

JceWaterCausticDetailComponent JCE_CALL jce_scene_water_caustic_detail_default(void)
{
    JceWaterCausticDetailComponent value = {0};
    value.struct_size = sizeof(value);
    value.version = JCE_WATER_CAUSTIC_DETAIL_COMPONENT_VERSION;
    value.transition_start_m = 0.5f;
    value.transition_end_m = 4.0f;
    value.fine_fade_depth_m = caustic_detail_fine_default_depth;
    return value;
}

static bool caustic_detail_read(const cJSON *object, const char *key, double max, float *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!item) return true;
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 0.0 && item->valuedouble <= max))
        return false;
    *out = (float)item->valuedouble;
    return true;
}

void parse_water_caustic_detail(JceScene *s, JceEntity e, const cJSON *c)
{
    JceWaterCausticDetailComponent value = jce_scene_water_caustic_detail_default();
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(c, "version");
    if (version && (!cJSON_IsNumber(version) || version->valuedouble != value.version))
        return;
    if (!caustic_detail_read(c, "secondaryWeight", 1.0, &value.secondary_weight) ||
        !caustic_detail_read(c, "transitionStartM", 80.0, &value.transition_start_m) ||
        !caustic_detail_read(c, "transitionEndM", 100.0, &value.transition_end_m) ||
        !caustic_detail_read(c, "tileSizeM", 128.0, &value.tile_size_m) ||
        !caustic_detail_read(c, "finePeriodRatio", 0.5, &value.fine_period_ratio) ||
        !caustic_detail_read(c, "fineWeight", 1.0, &value.fine_weight) ||
        !caustic_detail_read(c, "fineFadeDepthM", 100.0, &value.fine_fade_depth_m))
        return;
    jce_scene_set_water_caustic_detail(s, e, &value);
}

static bool caustic_detail_write_fine(cJSON *properties, const JceWaterCausticDetailComponent *value)
{
    /* Preserve the double-scale JSON shape at the fine-layer defaults. */
    if (value->fine_period_ratio == 0.0f && value->fine_weight == 0.0f &&
        value->fine_fade_depth_m == caustic_detail_fine_default_depth)
        return true;
    return cJSON_AddNumberToObject(properties, "finePeriodRatio", value->fine_period_ratio) &&
        cJSON_AddNumberToObject(properties, "fineWeight", value->fine_weight) &&
        cJSON_AddNumberToObject(properties, "fineFadeDepthM", value->fine_fade_depth_m);
}

void serw_water_caustic_detail(JceScene *s, JceEntity e, cJSON *arr)
{
    const JceWaterCausticDetailComponent *value = jce_scene_get_water_caustic_detail(s, e);
    if (!jce_water_caustic_detail_valid(value)) return;
    cJSON *object = cJSON_CreateObject(), *properties = cJSON_CreateObject();
    if (!object || !properties ||
        !cJSON_AddStringToObject(object, "type", "WaterCausticDetail") ||
        !cJSON_AddNumberToObject(properties, "version", value->version) ||
        !cJSON_AddNumberToObject(properties, "secondaryWeight", value->secondary_weight) ||
        !cJSON_AddNumberToObject(properties, "transitionStartM", value->transition_start_m) ||
        !cJSON_AddNumberToObject(properties, "transitionEndM", value->transition_end_m) ||
        !cJSON_AddNumberToObject(properties, "tileSizeM", value->tile_size_m) ||
        !caustic_detail_write_fine(properties, value) ||
        !cJSON_AddItemToObject(object, "properties", properties)) {
        cJSON_Delete(properties);
        cJSON_Delete(object);
        return;
    }
    if (!cJSON_AddItemToArray(arr, object)) cJSON_Delete(object);
}
