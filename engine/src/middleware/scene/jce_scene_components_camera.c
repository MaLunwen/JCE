#include "jce_scene_components_internal.h"

void parse_camera(JceScene *s, JceEntity e, const cJSON *c)
{
    JceCameraComponent cc;
    memset(&cc, 0, sizeof(cc));
    cc.fov_deg    = (float)j_num(c, "fov", 60.0);
    cc.near_plane = (float)j_num2(c, "nearClip", "near_clip", 0.1);
    cc.far_plane  = (float)j_num2(c, "farClip", "far_clip", 1000.0);
    cc.is_primary = j_bool(c, "primary", false);
    cc.ortho      = j_bool(c, "orthographic", false);
    if (!cc.ortho) cc.ortho = j_bool(c, "ortho", false);
    if (!cc.ortho) cc.ortho = j_bool(c, "isOrtho", false);
    /* Orthographic height in world units; 0 leaves the camera default. */
    cc.ortho_size = (float)j_num(c, "orthoSize", 0.0);
    cc.stack_index = (uint8_t)(int)j_num(c, "stackIndex", 0);
    cc.clear_mode  = (uint8_t)(int)j_num(c, "clearMode",  0);
    /* Absent (an older scene, or one that never masked) parses to 0, which
     * this field spells as "no filtering" -- see JceCameraComponent. */
    cc.culling_mask = (uint32_t)j_num(c, "cullingMask", 0.0);
    jce_scene_set_camera(s, e, &cc);
}

static void ser_camera(const JceCameraComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Camera");
    cJSON_AddNumberToObject(o, "fov", c->fov_deg);
    cJSON_AddNumberToObject(o, "nearClip", c->near_plane);
    cJSON_AddNumberToObject(o, "farClip",  c->far_plane);
    cJSON_AddBoolToObject(o, "primary", c->is_primary);
    cJSON_AddBoolToObject(o, "orthographic", c->ortho);
    cJSON_AddNumberToObject(o, "orthoSize", c->ortho_size);
    if (c->stack_index != 0)
        cJSON_AddNumberToObject(o, "stackIndex", c->stack_index);
    if (c->clear_mode != 0)
        cJSON_AddNumberToObject(o, "clearMode",  c->clear_mode);
    if (c->culling_mask != 0u)
        cJSON_AddNumberToObject(o, "cullingMask", (double)c->culling_mask);
    cJSON_AddItemToArray(arr, o);
}

void serw_camera(JceScene *s, JceEntity e, cJSON *arr)
{
    JceCameraComponent *c = jce_scene_get_camera(s, e);
    if (c) ser_camera(c, arr);
}
