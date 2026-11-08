#include "jce_scene_components_internal.h"

JceWaterMotionComponent JCE_CALL jce_scene_water_motion_default(void)
{
    JceWaterMotionComponent value = {0};
    value.struct_size = sizeof(value);
    value.version = JCE_WATER_MOTION_COMPONENT_VERSION;
    value.phase_rate = 1.0f;
    value.horizontal_scale = 1.0f;
    return value;
}

void parse_water_motion(JceScene *s, JceEntity e, const cJSON *c)
{
    JceWaterMotionComponent value = jce_scene_water_motion_default();
    if (j_num(c, "version", 1.0) != JCE_WATER_MOTION_COMPONENT_VERSION) return;
    const double fraction = j_num(c, "cascadeFraction", value.cascade_fraction);
    const double rate = j_num(c, "phaseRate", value.phase_rate);
    const double horizontal = j_num(c, "horizontalScale", value.horizontal_scale);
    const double limit = j_num(c, "maxHorizontalDisplacement", value.max_horizontal_displacement);
    if (!(fraction >= 0.0 && fraction <= 0.95 &&
          rate >= 0.0 && rate <= 4.0 &&
          horizontal >= 0.0 && horizontal <= 2.0 &&
          limit >= 0.0 && limit <= 64.0)) return;
    value.cascade_fraction = (float)fraction;
    value.phase_rate = (float)rate;
    value.horizontal_scale = (float)horizontal;
    value.max_horizontal_displacement = (float)limit;
    jce_scene_set_water_motion(s, e, &value);
}

void serw_water_motion(JceScene *s, JceEntity e, cJSON *arr)
{
    const JceWaterMotionComponent *value = jce_scene_get_water_motion(s, e);
    if (!jce_water_motion_valid(value)) return;
    cJSON *o = cJSON_CreateObject(), *p = cJSON_CreateObject();
    if (!o || !p ||
        !cJSON_AddStringToObject(o, "type", "WaterMotion") ||
        !cJSON_AddNumberToObject(p, "version", value->version) ||
        !cJSON_AddNumberToObject(p, "cascadeFraction", value->cascade_fraction) ||
        !cJSON_AddNumberToObject(p, "phaseRate", value->phase_rate) ||
        !cJSON_AddNumberToObject(p, "horizontalScale", value->horizontal_scale) ||
        !cJSON_AddNumberToObject(p, "maxHorizontalDisplacement",
                               value->max_horizontal_displacement) ||
        !cJSON_AddItemToObject(o, "properties", p)) {
        cJSON_Delete(p);
        cJSON_Delete(o);
        return;
    }
    if (!cJSON_AddItemToArray(arr, o)) cJSON_Delete(o);
}
