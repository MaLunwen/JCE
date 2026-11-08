#include "jce_scene_components_internal.h"
#include "jce_water_foam.h"
void parse_water_foam(JceScene *s, JceEntity e, const cJSON *c)
{
    JceWaterFoamComponent value = jce_scene_water_foam_default();
    if (j_num(c, "version", 1.0) != JCE_WATER_FOAM_COMPONENT_VERSION) return;
    value.generation_rate = (float)j_num(c, "generationRate", value.generation_rate);
    value.decay_rate = (float)j_num(c, "decayRate", value.decay_rate);
    value.fold_threshold = (float)j_num(c, "foldThreshold", value.fold_threshold);
    value.opacity = (float)j_num(c, "opacity", value.opacity);
    value.texture_scale = (float)j_num(c, "textureScale", value.texture_scale);
    if (jce_water_foam_valid(&value)) jce_scene_set_water_foam(s, e, &value);
}
void serw_water_foam(JceScene *s, JceEntity e, cJSON *arr)
{
    const JceWaterFoamComponent *value = jce_scene_get_water_foam(s, e);
    if (!value) return;
    cJSON *o = cJSON_CreateObject(), *p = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "WaterFoam");
    cJSON_AddNumberToObject(p, "version", value->version);
    cJSON_AddNumberToObject(p, "generationRate", value->generation_rate);
    cJSON_AddNumberToObject(p, "decayRate", value->decay_rate);
    cJSON_AddNumberToObject(p, "foldThreshold", value->fold_threshold);
    cJSON_AddNumberToObject(p, "opacity", value->opacity);
    cJSON_AddNumberToObject(p, "textureScale", value->texture_scale);
    cJSON_AddItemToObject(o, "properties", p);
    cJSON_AddItemToArray(arr, o);
}
