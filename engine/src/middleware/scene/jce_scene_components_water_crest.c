#include "jce_scene_components_internal.h"

void parse_water_crest(JceScene *s, JceEntity e, const cJSON *c)
{
    JceWaterCrestComponent value = jce_scene_water_crest_default();
    if (j_num(c, "version", 1.0) != JCE_WATER_CREST_COMPONENT_VERSION) return;
    value.start_height = (float)j_num(c, "startHeight", value.start_height);
    value.height_range = (float)j_num(c, "heightRange", value.height_range);
    value.fade_distance = (float)j_num(c, "fadeDistance", value.fade_distance);
    value.strength = (float)j_num(c, "strength", value.strength);
    value.tint[0] = (float)j_num(c, "tintR", value.tint[0]);
    value.tint[1] = (float)j_num(c, "tintG", value.tint[1]);
    value.tint[2] = (float)j_num(c, "tintB", value.tint[2]);
    jce_scene_set_water_crest(s, e, &value);
}

void serw_water_crest(JceScene *s, JceEntity e, cJSON *arr)
{
    const JceWaterCrestComponent *value = jce_scene_get_water_crest(s, e);
    if (!value) return;
    cJSON *o = cJSON_CreateObject(), *p = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "WaterCrest");
    cJSON_AddNumberToObject(p, "version", value->version);
    cJSON_AddNumberToObject(p, "startHeight", value->start_height);
    cJSON_AddNumberToObject(p, "heightRange", value->height_range);
    cJSON_AddNumberToObject(p, "fadeDistance", value->fade_distance);
    cJSON_AddNumberToObject(p, "strength", value->strength);
    cJSON_AddNumberToObject(p, "tintR", value->tint[0]);
    cJSON_AddNumberToObject(p, "tintG", value->tint[1]);
    cJSON_AddNumberToObject(p, "tintB", value->tint[2]);
    cJSON_AddItemToObject(o, "properties", p);
    cJSON_AddItemToArray(arr, o);
}
