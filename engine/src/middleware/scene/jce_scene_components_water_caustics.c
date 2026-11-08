#include "jce_scene_components_internal.h"

void parse_water_caustics(JceScene *s, JceEntity e, const cJSON *c)
{
    JceWaterCausticsComponent value = jce_scene_water_caustics_default();
    if (j_num(c, "version", 1.0) != JCE_WATER_CAUSTICS_COMPONENT_VERSION) return;
    value.offset_x = (float)j_num(c, "offsetX", value.offset_x);
    value.offset_z = (float)j_num(c, "offsetZ", value.offset_z);
    value.fade_depth = (float)j_num(c, "fadeDepth", value.fade_depth);
    jce_scene_set_water_caustics(s, e, &value);
}

void serw_water_caustics(JceScene *s, JceEntity e, cJSON *arr)
{
    const JceWaterCausticsComponent *value = jce_scene_get_water_caustics(s, e);
    if (!value) return;
    cJSON *o = cJSON_CreateObject(), *p = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "WaterCaustics");
    cJSON_AddNumberToObject(p, "version", value->version);
    cJSON_AddNumberToObject(p, "offsetX", value->offset_x);
    cJSON_AddNumberToObject(p, "offsetZ", value->offset_z);
    cJSON_AddNumberToObject(p, "fadeDepth", value->fade_depth);
    cJSON_AddItemToObject(o, "properties", p);
    cJSON_AddItemToArray(arr, o);
}
