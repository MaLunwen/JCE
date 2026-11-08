#include "jce_scene_components_internal.h"
#include <float.h>

JceWaterNearshoreComponent JCE_CALL jce_scene_water_nearshore_default(void)
{
    JceWaterNearshoreComponent value = {0};
    value.struct_size = sizeof(value);
    value.version = JCE_WATER_NEARSHORE_COMPONENT_VERSION;
    value.extent = 16.0f;
    value.drag = 0.25f;
    value.boundary_absorption = 8.0f;
    value.foam_opacity = 0.8f;
    return value;
}

void parse_water_nearshore(JceScene *s, JceEntity e, const cJSON *c)
{
    JceWaterNearshoreComponent value = jce_scene_water_nearshore_default();
    if (j_num(c, "version", 1.0) != JCE_WATER_NEARSHORE_COMPONENT_VERSION) return;
    const double terrain = j_num(c, "terrain", 0.0);
    const double offset_x = j_num(c, "offsetX", value.offset_x);
    const double offset_z = j_num(c, "offsetZ", value.offset_z);
    const double extent = j_num(c, "extent", value.extent);
    const double drag = j_num(c, "drag", value.drag);
    const double absorption = j_num(c, "boundaryAbsorption", value.boundary_absorption);
    const double opacity = j_num(c, "foamOpacity", value.foam_opacity);
    /* Scene JSON IDs use doubles. Restrict the cast to exactly representable
     * integer IDs, and reject out-of-range fields before narrowing. */
    if (!(terrain >= 0.0 && terrain <= 9007199254740991.0 &&
          terrain == (double)(JceEntity)terrain &&
          offset_x >= -FLT_MAX && offset_x <= FLT_MAX &&
          offset_z >= -FLT_MAX && offset_z <= FLT_MAX &&
          extent >= 8.0 && extent <= 128.0 &&
          drag >= 0.0 && drag <= 20.0 &&
          absorption >= 0.0 && absorption <= 50.0 &&
          opacity >= 0.0 && opacity <= 1.0)) return;
    value.terrain = (JceEntity)terrain;
    value.offset_x = (float)offset_x;
    value.offset_z = (float)offset_z;
    value.extent = (float)extent;
    value.drag = (float)drag;
    value.boundary_absorption = (float)absorption;
    value.foam_opacity = (float)opacity;
    jce_scene_set_water_nearshore(s, e, &value);
}

void serw_water_nearshore(JceScene *s, JceEntity e, cJSON *arr)
{
    const JceWaterNearshoreComponent *value = jce_scene_get_water_nearshore(s, e);
    if (!value) return;
    cJSON *o = cJSON_CreateObject(), *p = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "WaterNearshore");
    cJSON_AddNumberToObject(p, "version", value->version);
    cJSON_AddNumberToObject(p, "terrain", (double)value->terrain);
    cJSON_AddNumberToObject(p, "offsetX", value->offset_x);
    cJSON_AddNumberToObject(p, "offsetZ", value->offset_z);
    cJSON_AddNumberToObject(p, "extent", value->extent);
    cJSON_AddNumberToObject(p, "drag", value->drag);
    cJSON_AddNumberToObject(p, "boundaryAbsorption", value->boundary_absorption);
    cJSON_AddNumberToObject(p, "foamOpacity", value->foam_opacity);
    cJSON_AddItemToObject(o, "properties", p);
    cJSON_AddItemToArray(arr, o);
}
