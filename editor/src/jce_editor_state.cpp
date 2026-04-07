/*
 * jce_editor_state.cpp  Central editor state implementation.
 *
 * Manages entity selection, edit modes, play state, and demo scene data.
 * When ECS integration is added (Phase 3), the entity storage will be
 * replaced by queries into the actual ECS world.
 */

#include "jce_editor_state.h"
#include "jce_editor_scene_render.h"

#include <cjson/cJSON.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <vector>
#include <string>

extern "C" {
#include <jce/core/jce_log.h>
}

#define LOG_TAG "editor_state"

/* ── Internal State ────────────────────────────────────────────────── */

static struct {
    /* Selection. */
    uint32_t    selected[JCE_MAX_SELECTED];
    int         selected_count;
    uint32_t    focused;       /* primary selection */

    /* Modes. */
    JceEditMode      edit_mode;
    JceGizmoMode     gizmo_mode;
    JceGizmoSpace    gizmo_space;
    JceSceneViewMode view_mode;
    JcePlayState     play_state;
    bool             show_grid;
    bool             is_2d_mode;
    bool             live_preview;

    /* Entity storage (demo data, replaced by ECS later). */
    JceEntityInfo    entities[JCE_MAX_ENTITIES];
    JceComponentInfo components[JCE_MAX_ENTITIES][JCE_MAX_COMPONENTS];
    int              entity_count;
    uint32_t         next_id;
    char             current_scene_path[512];

    bool initialized;
} s;

/* ── Helper: find entity index by id ───────────────────────────────── */

static int find_entity(uint32_t id)
{
    for (int i = 0; i < s.entity_count; i++)
        if (s.entities[i].id == id) return i;
    return -1;
}

static void set_current_scene_path_internal(const char *scene_path)
{
    if (scene_path && scene_path[0] != '\0')
        snprintf(s.current_scene_path, sizeof(s.current_scene_path), "%s", scene_path);
    else
        s.current_scene_path[0] = '\0';
}

static void update_scene_dir_from_path(const char *scene_path)
{
    if (!scene_path || scene_path[0] == '\0')
        return;

    char scene_dir[512];
    snprintf(scene_dir, sizeof(scene_dir), "%s", scene_path);

    /* Find last path separator. */
    char *sep = strrchr(scene_dir, '/');
    char *bsep = strrchr(scene_dir, '\\');
    if (bsep && (!sep || bsep > sep)) sep = bsep;
    if (sep) {
        *sep = '\0';
        /* Go up one more level if we're in a "Scenes" subdirectory. */
        char *last_comp = strrchr(scene_dir, '/');
        char *last_bcomp = strrchr(scene_dir, '\\');
        if (last_bcomp && (!last_comp || last_bcomp > last_comp)) last_comp = last_bcomp;
        const char *dir_name = last_comp ? last_comp + 1 : scene_dir;
        if (_stricmp(dir_name, "Scenes") == 0 || _stricmp(dir_name, "scenes") == 0) {
            if (last_comp) *last_comp = '\0';
        }
    }

    jce_editor_scene_set_scene_dir(scene_dir);
}

static void clear_scene_entities(void)
{
    s.entity_count = 0;
    s.next_id = 1;
    jce_state_clear_selection();
}

static void build_demo_scene(void)
{
    uint32_t root = jce_state_create_entity("Scene Root", 0);

    uint32_t cam = jce_state_create_entity("Main Camera", root);
    jce_state_add_component(cam, JCE_COMP_TRANSFORM);
    /* Set camera position. */
    {
        int idx = find_entity(cam);
        if (idx >= 0) {
            s.components[idx][0].data.transform.pos[1] = 5.0f;
            s.components[idx][0].data.transform.pos[2] = 10.0f;
        }
    }
    jce_state_add_component(cam, JCE_COMP_CAMERA);

    uint32_t lights = jce_state_create_entity("Lights", root);
    uint32_t dir_light = jce_state_create_entity("Directional Light", lights);
    jce_state_add_component(dir_light, JCE_COMP_TRANSFORM);
    jce_state_add_component(dir_light, JCE_COMP_LIGHT);

    uint32_t pt_light = jce_state_create_entity("Point Light", lights);
    jce_state_add_component(pt_light, JCE_COMP_TRANSFORM);
    jce_state_add_component(pt_light, JCE_COMP_LIGHT);
    {
        int idx = find_entity(pt_light);
        if (idx >= 0) {
            /* Point light type = 1. */
            for (int i = 0; i < s.entities[idx].component_count; i++) {
                if (s.components[idx][i].type == JCE_COMP_LIGHT)
                    s.components[idx][i].data.light.type = 1;
            }
        }
    }

    uint32_t objs = jce_state_create_entity("Objects", root);
    uint32_t cube = jce_state_create_entity("Cube", objs);
    jce_state_add_component(cube, JCE_COMP_TRANSFORM);
    jce_state_add_component(cube, JCE_COMP_MESH_RENDERER);

    uint32_t sphere = jce_state_create_entity("Sphere", objs);
    jce_state_add_component(sphere, JCE_COMP_TRANSFORM);
    jce_state_add_component(sphere, JCE_COMP_MESH_RENDERER);

    uint32_t plane = jce_state_create_entity("Plane", objs);
    jce_state_add_component(plane, JCE_COMP_TRANSFORM);
    jce_state_add_component(plane, JCE_COMP_MESH_RENDERER);

    uint32_t ui = jce_state_create_entity("UI", root);
    jce_state_create_entity("Canvas", ui);
}

static bool equals_ignore_case(const char *a, const char *b)
{
    if (!a || !b) return false;

    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static const cJSON *json_get_any(const cJSON *obj, const char *const *keys, int key_count)
{
    if (!cJSON_IsObject(obj)) return NULL;

    for (int i = 0; i < key_count; i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, keys[i]);
        if (item) return item;
    }
    return NULL;
}

static const char *json_get_string_any(const cJSON *obj,
                                       const char *const *keys,
                                       int key_count)
{
    const cJSON *item = json_get_any(obj, keys, key_count);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool json_get_bool_any(const cJSON *obj,
                              const char *const *keys,
                              int key_count,
                              bool *out)
{
    const cJSON *item = json_get_any(obj, keys, key_count);
    if (!item || !out) return false;

    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
        return true;
    }
    if (cJSON_IsNumber(item)) {
        *out = (item->valuedouble != 0.0);
        return true;
    }
    return false;
}

static JceTagColor parse_tag_color(const cJSON *obj)
{
    static const char *const color_keys[] = {
        "tag_color", "tagColor", "tag_color_id", "tagColorId"
    };
    const cJSON *item = json_get_any(obj, color_keys,
                                     (int)(sizeof(color_keys) / sizeof(color_keys[0])));
    if (!item) return JCE_TAG_NONE;

    if (cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v >= JCE_TAG_NONE && v < JCE_TAG_COLOR_COUNT)
            return (JceTagColor)v;
        return JCE_TAG_NONE;
    }

    if (cJSON_IsString(item) && item->valuestring) {
        if (equals_ignore_case(item->valuestring, "red")) return JCE_TAG_RED;
        if (equals_ignore_case(item->valuestring, "orange")) return JCE_TAG_ORANGE;
        if (equals_ignore_case(item->valuestring, "yellow")) return JCE_TAG_YELLOW;
        if (equals_ignore_case(item->valuestring, "green")) return JCE_TAG_GREEN;
        if (equals_ignore_case(item->valuestring, "blue")) return JCE_TAG_BLUE;
        if (equals_ignore_case(item->valuestring, "purple")) return JCE_TAG_PURPLE;
        if (equals_ignore_case(item->valuestring, "gray")
            || equals_ignore_case(item->valuestring, "grey"))
            return JCE_TAG_GRAY;
    }

    return JCE_TAG_NONE;
}

static std::string key_from_number(int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    return std::string(buf);
}

static std::string resolve_source_key(const cJSON *obj, int fallback)
{
    if (!cJSON_IsObject(obj))
        return key_from_number(fallback);

    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(obj, "id");
    if (cJSON_IsString(id_item) && id_item->valuestring && id_item->valuestring[0] != '\0')
        return std::string(id_item->valuestring);
    if (cJSON_IsNumber(id_item))
        return key_from_number(id_item->valueint);

    return key_from_number(fallback);
}

static bool resolve_parent_key(const cJSON *obj, std::string *out_parent_key)
{
    if (!cJSON_IsObject(obj) || !out_parent_key) return false;

    static const char *const parent_keys[] = { "parent_id", "parentId", "parent" };
    const cJSON *item = json_get_any(obj, parent_keys,
        (int)(sizeof(parent_keys) / sizeof(parent_keys[0])));
    if (!item) return false;

    if (cJSON_IsNumber(item)) {
        if (item->valueint == 0) return false;
        *out_parent_key = key_from_number(item->valueint);
        return true;
    }

    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        if (strcmp(item->valuestring, "0") == 0) return false;
        *out_parent_key = item->valuestring;
        return true;
    }

    return false;
}

struct FlatEntityRef {
    std::string source_key;
    std::string source_alt_key;
    std::string parent_key;
    bool has_parent;
    uint32_t new_id;
};

/* ── Component Type from Name ──────────────────────────────────────── */

static JceComponentType component_type_from_name(const char *name)
{
    if (!name) return JCE_COMP_TYPE_COUNT;

    static const struct { const char *name; JceComponentType type; } map[] = {
        { "Transform",            JCE_COMP_TRANSFORM },
        { "transform",            JCE_COMP_TRANSFORM },
        { "MeshRenderer",         JCE_COMP_MESH_RENDERER },
        { "Mesh Renderer",        JCE_COMP_MESH_RENDERER },
        { "meshRenderer",         JCE_COMP_MESH_RENDERER },
        { "mesh_renderer",        JCE_COMP_MESH_RENDERER },
        { "SpriteRenderer",       JCE_COMP_SPRITE_RENDERER },
        { "Sprite Renderer",      JCE_COMP_SPRITE_RENDERER },
        { "spriteRenderer",       JCE_COMP_SPRITE_RENDERER },
        { "sprite_renderer",      JCE_COMP_SPRITE_RENDERER },
        { "Camera",               JCE_COMP_CAMERA },
        { "camera",               JCE_COMP_CAMERA },
        { "Light",                JCE_COMP_LIGHT },
        { "light",                JCE_COMP_LIGHT },
        { "Animator",             JCE_COMP_ANIMATOR },
        { "animator",             JCE_COMP_ANIMATOR },
        { "SkeletalAnimator",     JCE_COMP_SKELETAL_ANIMATOR },
        { "Skeletal Animator",    JCE_COMP_SKELETAL_ANIMATOR },
        { "Rigidbody",            JCE_COMP_RIGIDBODY },
        { "rigidbody",            JCE_COMP_RIGIDBODY },
        { "BoxCollider",          JCE_COMP_BOX_COLLIDER },
        { "Box Collider",         JCE_COMP_BOX_COLLIDER },
        { "SphereCollider",       JCE_COMP_SPHERE_COLLIDER },
        { "Sphere Collider",      JCE_COMP_SPHERE_COLLIDER },
        { "CharacterController",  JCE_COMP_CHARACTER_CONTROLLER },
        { "Character Controller", JCE_COMP_CHARACTER_CONTROLLER },
        { "AudioSource",          JCE_COMP_AUDIO_SOURCE },
        { "Audio Source",         JCE_COMP_AUDIO_SOURCE },
        { "Script",               JCE_COMP_SCRIPT },
        { "script",               JCE_COMP_SCRIPT },
    };

    for (int i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++) {
        if (strcmp(name, map[i].name) == 0)
            return map[i].type;
    }
    return JCE_COMP_TYPE_COUNT;
}

/* ── JSON Number Helper ────────────────────────────────────────────── */

static float json_get_float(const cJSON *obj, const char *key, float def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? (float)item->valuedouble : def;
}

static float json_get_float_any2(const cJSON *obj, const char *k1, const char *k2, float def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, k1);
    if (!cJSON_IsNumber(item))
        item = cJSON_GetObjectItemCaseSensitive(obj, k2);
    return cJSON_IsNumber(item) ? (float)item->valuedouble : def;
}

/* ── Parse Component from JSON ─────────────────────────────────────── */

static bool parse_component_json(const cJSON *comp_json, JceComponentInfo *out)
{
    if (!cJSON_IsObject(comp_json) || !out) return false;

    /* Determine component type. */
    static const char *const type_keys[] = { "type", "componentType", "class" };
    const char *type_str = json_get_string_any(comp_json, type_keys,
        (int)(sizeof(type_keys) / sizeof(type_keys[0])));
    if (!type_str) return false;

    JceComponentType type = component_type_from_name(type_str);
    if (type >= JCE_COMP_TYPE_COUNT) return false;

    memset(out, 0, sizeof(*out));
    out->type = type;
    out->expanded = true;

    /*
     * Properties can be either:
     *   - Directly on the component object (SceneSpec / flat format)
     *   - Wrapped in a "properties" sub-object (SceneData / editor format)
     */
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(comp_json, "properties");
    if (!cJSON_IsObject(props))
        props = comp_json; /* flat format — properties are siblings of "type" */

    switch (type) {
    case JCE_COMP_TRANSFORM:
        out->data.transform.pos[0]   = json_get_float_any2(props, "posX", "pos_x", 0.0f);
        out->data.transform.pos[1]   = json_get_float_any2(props, "posY", "pos_y", 0.0f);
        out->data.transform.pos[2]   = json_get_float_any2(props, "posZ", "pos_z", 0.0f);
        /* Rotation: check for quaternion (rotW present) and convert to Euler degrees. */
        {
            float rx = json_get_float_any2(props, "rotX", "rot_x", 0.0f);
            float ry = json_get_float_any2(props, "rotY", "rot_y", 0.0f);
            float rz = json_get_float_any2(props, "rotZ", "rot_z", 0.0f);
            const cJSON *rw_item = cJSON_GetObjectItemCaseSensitive(props, "rotW");
            if (!rw_item) rw_item = cJSON_GetObjectItemCaseSensitive(props, "rot_w");
            if (cJSON_IsNumber(rw_item)) {
                /* quaternion -> Euler degrees (Y-X-Z convention). */
                float qx = rx, qy = ry, qz = rz;
                float qw = (float)rw_item->valuedouble;
                /* Normalize quaternion. */
                float len = sqrtf(qx*qx + qy*qy + qz*qz + qw*qw);
                if (len > 1e-8f) { qx /= len; qy /= len; qz /= len; qw /= len; }
                /* Convert quaternion to Euler (YXZ order in radians, then to degrees). */
                float sinr_cosp = 2.0f * (qw * qx + qy * qz);
                float cosr_cosp = 1.0f - 2.0f * (qx * qx + qy * qy);
                float pitch = atan2f(sinr_cosp, cosr_cosp);
                float sinp = 2.0f * (qw * qy - qz * qx);
                float yaw;
                if (fabsf(sinp) >= 1.0f)
                    yaw = copysignf(3.14159265f * 0.5f, sinp);
                else
                    yaw = asinf(sinp);
                float siny_cosp = 2.0f * (qw * qz + qx * qy);
                float cosy_cosp = 1.0f - 2.0f * (qy * qy + qz * qz);
                float roll = atan2f(siny_cosp, cosy_cosp);
                rx = pitch * (180.0f / 3.14159265f);
                ry = yaw   * (180.0f / 3.14159265f);
                rz = roll  * (180.0f / 3.14159265f);
            }
            out->data.transform.rot[0] = rx;
            out->data.transform.rot[1] = ry;
            out->data.transform.rot[2] = rz;
        }
        out->data.transform.scale[0] = json_get_float_any2(props, "scaleX", "scale_x", 1.0f);
        out->data.transform.scale[1] = json_get_float_any2(props, "scaleY", "scale_y", 1.0f);
        out->data.transform.scale[2] = json_get_float_any2(props, "scaleZ", "scale_z", 1.0f);
        break;

    case JCE_COMP_CAMERA:
        out->data.camera.fov       = json_get_float(props, "fov", 60.0f);
        out->data.camera.near_clip = json_get_float_any2(props, "nearClip", "near_clip", 0.1f);
        out->data.camera.far_clip  = json_get_float_any2(props, "farClip", "far_clip", 1000.0f);
        {
            bool ortho = false;
            static const char *const ortho_keys[] = { "orthographic", "ortho", "isOrtho" };
            json_get_bool_any(props, ortho_keys,
                (int)(sizeof(ortho_keys) / sizeof(ortho_keys[0])), &ortho);
            out->data.camera.ortho = ortho;
        }
        break;

    case JCE_COMP_LIGHT:
        out->data.light.color[0]   = json_get_float_any2(props, "colorR", "color_r", 1.0f);
        out->data.light.color[1]   = json_get_float_any2(props, "colorG", "color_g", 1.0f);
        out->data.light.color[2]   = json_get_float_any2(props, "colorB", "color_b", 1.0f);
        out->data.light.color[3]   = json_get_float_any2(props, "colorA", "color_a", 1.0f);
        out->data.light.intensity  = json_get_float(props, "intensity", 1.0f);
        {
            /* Light type: 0 = directional, 1 = point, 2 = spot */
            const cJSON *lt = cJSON_GetObjectItemCaseSensitive(props, "lightType");
            if (!lt) lt = cJSON_GetObjectItemCaseSensitive(props, "type");
            if (cJSON_IsNumber(lt))
                out->data.light.type = lt->valueint;
            else if (cJSON_IsString(lt) && lt->valuestring) {
                if (equals_ignore_case(lt->valuestring, "point")) out->data.light.type = 1;
                else if (equals_ignore_case(lt->valuestring, "spot")) out->data.light.type = 2;
                /* else directional = 0 (default) */
            }
        }
        break;

    case JCE_COMP_MESH_RENDERER: {
        static const char *const mesh_keys[] = { "meshPath", "mesh_path", "mesh" };
        static const char *const mat_keys[] = { "materialPath", "material_path", "material" };
        const char *mesh = json_get_string_any(props, mesh_keys,
            (int)(sizeof(mesh_keys) / sizeof(mesh_keys[0])));
        const char *mat = json_get_string_any(props, mat_keys,
            (int)(sizeof(mat_keys) / sizeof(mat_keys[0])));
        if (mesh)
            snprintf(out->data.mesh_renderer.mesh_path,
                     sizeof(out->data.mesh_renderer.mesh_path), "%s", mesh);
        if (mat)
            snprintf(out->data.mesh_renderer.material_path,
                     sizeof(out->data.mesh_renderer.material_path), "%s", mat);
        break;
    }

    default:
        /* Other component types — store type only, no specific data yet. */
        break;
    }

    return true;
}

/* ── Parse Components Array for an Entity ──────────────────────────── */

static void parse_entity_components(int entity_idx, const cJSON *obj)
{
    if (entity_idx < 0 || entity_idx >= s.entity_count) return;
    if (!cJSON_IsObject(obj)) return;

    static const char *const comp_keys[] = { "components", "component" };
    const cJSON *comps = json_get_any(obj, comp_keys,
        (int)(sizeof(comp_keys) / sizeof(comp_keys[0])));
    if (!cJSON_IsArray(comps)) return;

    int count = 0;
    for (cJSON *c = comps->child; c && count < JCE_MAX_COMPONENTS; c = c->next) {
        JceComponentInfo info;
        if (parse_component_json(c, &info))
            s.components[entity_idx][count++] = info;
    }
    s.entities[entity_idx].component_count = count;
}

/* ── Ensure Entity has a Transform Component ───────────────────────── */

static void ensure_transform_component(int entity_idx)
{
    if (entity_idx < 0 || entity_idx >= s.entity_count) return;

    JceEntityInfo *e = &s.entities[entity_idx];
    JceComponentInfo *comps = s.components[entity_idx];

    /* Check if already has Transform. */
    for (int i = 0; i < e->component_count; i++) {
        if (comps[i].type == JCE_COMP_TRANSFORM)
            return;
    }

    /* Add default Transform at the front. */
    if (e->component_count >= JCE_MAX_COMPONENTS) return;

    /* Shift existing components right. */
    for (int i = e->component_count; i > 0; i--)
        comps[i] = comps[i - 1];

    memset(&comps[0], 0, sizeof(comps[0]));
    comps[0].type = JCE_COMP_TRANSFORM;
    comps[0].expanded = true;
    comps[0].data.transform.scale[0] = 1.0f;
    comps[0].data.transform.scale[1] = 1.0f;
    comps[0].data.transform.scale[2] = 1.0f;
    e->component_count++;
}

static uint32_t find_new_id_for_key(const std::vector<FlatEntityRef> &refs,
                                    const std::string &key)
{
    for (size_t i = 0; i < refs.size(); i++) {
        if (refs[i].source_key == key)
            return refs[i].new_id;
        if (!refs[i].source_alt_key.empty() && refs[i].source_alt_key == key)
            return refs[i].new_id;
    }
    return 0;
}

static void apply_entity_fields(JceEntityInfo *e, const cJSON *obj)
{
    if (!e || !cJSON_IsObject(obj)) return;

    static const char *const name_keys[] = { "name", "entityName", "label" };
    static const char *const tag_keys[] = { "tag", "entityTag" };
    static const char *const enabled_keys[] = { "enabled", "isEnabled", "active" };

    const char *name = json_get_string_any(obj, name_keys,
        (int)(sizeof(name_keys) / sizeof(name_keys[0])));
    if (name && name[0] != '\0')
        snprintf(e->name, sizeof(e->name), "%s", name);

    const char *tag = json_get_string_any(obj, tag_keys,
        (int)(sizeof(tag_keys) / sizeof(tag_keys[0])));
    if (tag)
        snprintf(e->tag, sizeof(e->tag), "%s", tag);

    bool enabled = true;
    if (json_get_bool_any(obj, enabled_keys,
        (int)(sizeof(enabled_keys) / sizeof(enabled_keys[0])),
        &enabled))
        e->enabled = enabled;

    e->tag_color = parse_tag_color(obj);

    /* Parse components from JSON. */
    int idx = find_entity(e->id);
    if (idx >= 0) {
        parse_entity_components(idx, obj);
        ensure_transform_component(idx);
    }
}

static uint32_t load_entity_tree_node(const cJSON *node, uint32_t parent_id)
{
    if (!cJSON_IsObject(node)) return 0;

    static const char *const name_keys[] = { "name", "entityName", "label" };
    static const char *const children_keys[] = {
        "children", "nodes", "entities", "objects"
    };

    const char *name = json_get_string_any(node, name_keys,
        (int)(sizeof(name_keys) / sizeof(name_keys[0])));
    if (!name || name[0] == '\0')
        name = "Entity";

    uint32_t id = jce_state_create_entity(name, parent_id);
    JceEntityInfo *e = jce_state_get_entity(id);
    apply_entity_fields(e, node);

    const cJSON *children = json_get_any(node, children_keys,
        (int)(sizeof(children_keys) / sizeof(children_keys[0])));
    if (cJSON_IsArray(children)) {
        for (cJSON *child = children->child; child; child = child->next)
            load_entity_tree_node(child, id);
    }

    return id;
}

static bool array_has_parent_refs(const cJSON *arr)
{
    if (!cJSON_IsArray(arr)) return false;

    for (cJSON *item = arr->child; item; item = item->next) {
        if (!cJSON_IsObject(item)) continue;
        const cJSON *p1 = cJSON_GetObjectItemCaseSensitive(item, "parent_id");
        const cJSON *p2 = cJSON_GetObjectItemCaseSensitive(item, "parentId");
        const cJSON *p3 = cJSON_GetObjectItemCaseSensitive(item, "parent");
        if (cJSON_IsNumber(p1) || cJSON_IsNumber(p2))
            return true;
        if (cJSON_IsString(p3) && p3->valuestring && p3->valuestring[0] != '\0'
            && strcmp(p3->valuestring, "0") != 0)
            return true;
    }
    return false;
}

static void load_entities_from_array(const cJSON *arr)
{
    if (!cJSON_IsArray(arr)) return;

    if (!array_has_parent_refs(arr)) {
        for (cJSON *item = arr->child; item; item = item->next)
            load_entity_tree_node(item, 0);
        return;
    }

    std::vector<FlatEntityRef> refs;
    refs.reserve((size_t)cJSON_GetArraySize(arr));

    int fallback_id = 1;
    for (cJSON *item = arr->child; item; item = item->next) {
        if (!cJSON_IsObject(item)) continue;

        static const char *const name_keys[] = { "name", "entityName", "label" };
        const char *name = json_get_string_any(item, name_keys,
            (int)(sizeof(name_keys) / sizeof(name_keys[0])));
        if (!name || name[0] == '\0')
            name = "Entity";

        uint32_t id = jce_state_create_entity(name, 0);
        JceEntityInfo *e = jce_state_get_entity(id);
        apply_entity_fields(e, item);

        FlatEntityRef ref;
        ref.source_key = resolve_source_key(item, fallback_id++);
        ref.source_alt_key.clear();
        ref.has_parent = resolve_parent_key(item, &ref.parent_key);
        ref.new_id = id;
        refs.push_back(ref);
    }

    for (size_t i = 0; i < refs.size(); i++) {
        if (!refs[i].has_parent) continue;

        uint32_t parent_new_id = find_new_id_for_key(refs, refs[i].parent_key);

        if (parent_new_id != 0 && parent_new_id != refs[i].new_id)
            jce_state_reparent_entity(refs[i].new_id, parent_new_id);
    }
}

static bool object_map_has_parent_refs(const cJSON *obj)
{
    if (!cJSON_IsObject(obj)) return false;

    for (const cJSON *item = obj->child; item; item = item->next) {
        if (!cJSON_IsObject(item)) continue;
        std::string parent_key;
        if (resolve_parent_key(item, &parent_key))
            return true;
    }
    return false;
}

static void load_entities_from_object_map(const cJSON *obj)
{
    if (!cJSON_IsObject(obj)) return;

    if (!object_map_has_parent_refs(obj)) {
        for (const cJSON *item = obj->child; item; item = item->next) {
            if (cJSON_IsObject(item))
                load_entity_tree_node(item, 0);
        }
        return;
    }

    std::vector<FlatEntityRef> refs;
    int fallback_id = 1;

    for (const cJSON *item = obj->child; item; item = item->next) {
        if (!cJSON_IsObject(item)) continue;

        static const char *const name_keys[] = { "name", "entityName", "label" };
        const char *name = json_get_string_any(item, name_keys,
            (int)(sizeof(name_keys) / sizeof(name_keys[0])));
        if (!name || name[0] == '\0')
            name = "Entity";

        uint32_t id = jce_state_create_entity(name, 0);
        JceEntityInfo *e = jce_state_get_entity(id);
        apply_entity_fields(e, item);

        FlatEntityRef ref;
        std::string resolved = resolve_source_key(item, fallback_id++);
        if (item->string && item->string[0] != '\0') {
            ref.source_key = item->string;
            ref.source_alt_key = (ref.source_key == resolved) ? std::string() : resolved;
        } else {
            ref.source_key = resolved;
            ref.source_alt_key.clear();
        }
        ref.has_parent = resolve_parent_key(item, &ref.parent_key);
        ref.new_id = id;
        refs.push_back(ref);
    }

    for (size_t i = 0; i < refs.size(); i++) {
        if (!refs[i].has_parent) continue;

        uint32_t parent_new_id = find_new_id_for_key(refs, refs[i].parent_key);
        if (parent_new_id != 0 && parent_new_id != refs[i].new_id)
            jce_state_reparent_entity(refs[i].new_id, parent_new_id);
    }
}

static bool looks_like_entity_object(const cJSON *obj)
{
    if (!cJSON_IsObject(obj)) return false;

    static const char *const entity_keys[] = {
        "name", "entityName", "label", "children",
        "nodes", "entities", "objects", "parent",
        "parent_id", "parentId"
    };

    return json_get_any(obj, entity_keys,
        (int)(sizeof(entity_keys) / sizeof(entity_keys[0]))) != NULL;
}

/* ── Init / Shutdown ───────────────────────────────────────────────── */

void jce_editor_state_init(void)
{
    memset(&s, 0, sizeof(s));
    s.edit_mode   = JCE_EDIT_MODE_SELECT;
    s.gizmo_mode  = JCE_GIZMO_TRANSLATE;
    s.gizmo_space = JCE_GIZMO_LOCAL;
    s.view_mode   = JCE_VIEW_SHADED;
    s.play_state  = JCE_PLAY_STOPPED;
    s.show_grid   = true;
    s.current_scene_path[0] = '\0';
    clear_scene_entities();
    build_demo_scene();

    s.initialized = true;
    LOG_INFO(LOG_TAG, "editor state initialized (%d demo entities)", s.entity_count);
}

void jce_editor_state_shutdown(void)
{
    memset(&s, 0, sizeof(s));
    LOG_INFO(LOG_TAG, "editor state shutdown");
}

/* ── Selection ─────────────────────────────────────────────────────── */

void jce_state_select_entity(uint32_t id, bool add_to_selection)
{
    if (!add_to_selection)
        jce_state_clear_selection();

    /* Don't add duplicates. */
    if (jce_state_is_selected(id)) {
        s.focused = id;
        return;
    }
    if (s.selected_count >= JCE_MAX_SELECTED) return;

    s.selected[s.selected_count++] = id;
    s.focused = id;
}

void jce_state_deselect_entity(uint32_t id)
{
    for (int i = 0; i < s.selected_count; i++) {
        if (s.selected[i] == id) {
            s.selected[i] = s.selected[--s.selected_count];
            if (s.focused == id)
                s.focused = s.selected_count > 0 ? s.selected[0] : 0;
            return;
        }
    }
}

void jce_state_clear_selection(void)
{
    s.selected_count = 0;
    s.focused = 0;
}

bool jce_state_is_selected(uint32_t id)
{
    for (int i = 0; i < s.selected_count; i++)
        if (s.selected[i] == id) return true;
    return false;
}

uint32_t jce_state_get_focused(void) { return s.focused; }

const uint32_t *jce_state_get_selection(int *out_count)
{
    if (out_count) *out_count = s.selected_count;
    return s.selected;
}

/* ── Entity Management ─────────────────────────────────────────────── */

int jce_state_get_entity_count(void) { return s.entity_count; }

JceEntityInfo *jce_state_get_entity(uint32_t id)
{
    int idx = find_entity(id);
    return idx >= 0 ? &s.entities[idx] : NULL;
}

JceEntityInfo *jce_state_get_entity_by_index(int index)
{
    if (index < 0 || index >= s.entity_count)
        return NULL;
    return &s.entities[index];
}

JceEntityInfo *jce_state_get_root_entities(int *out_count)
{
    /* Build flat list of root entities (parent_id == 0).
     * For simplicity, return pointer into entities array;
     * caller should iterate with jce_state_get_entity_count(). */
    static uint32_t roots[JCE_MAX_ENTITIES];
    static JceEntityInfo *root_ptrs[JCE_MAX_ENTITIES];
    int n = 0;
    for (int i = 0; i < s.entity_count; i++) {
        if (s.entities[i].parent_id == 0) {
            root_ptrs[n] = &s.entities[i];
            roots[n++] = s.entities[i].id;
        }
    }
    if (out_count) *out_count = n;
    /* Return first root — caller iterates via get_entity on children. */
    return n > 0 ? &s.entities[find_entity(roots[0])] : NULL;
}

uint32_t jce_state_create_entity(const char *name, uint32_t parent_id)
{
    if (s.entity_count >= JCE_MAX_ENTITIES) return 0;

    JceEntityInfo *e = &s.entities[s.entity_count++];
    memset(e, 0, sizeof(*e));
    e->id = s.next_id++;
    snprintf(e->name, sizeof(e->name), "%s", name ? name : "Entity");
    e->enabled   = true;
    e->parent_id = parent_id;
    e->tag_color = JCE_TAG_NONE;

    /* Add to parent's children list. */
    if (parent_id != 0) {
        JceEntityInfo *parent = jce_state_get_entity(parent_id);
        if (parent && parent->child_count < JCE_MAX_CHILDREN)
            parent->children[parent->child_count++] = e->id;
    }

    return e->id;
}

void jce_state_delete_entity(uint32_t id)
{
    int idx = find_entity(id);
    if (idx < 0) return;

    /* Remove from parent's children list. */
    JceEntityInfo *e = &s.entities[idx];
    if (e->parent_id != 0) {
        JceEntityInfo *parent = jce_state_get_entity(e->parent_id);
        if (parent) {
            for (int i = 0; i < parent->child_count; i++) {
                if (parent->children[i] == id) {
                    parent->children[i] = parent->children[--parent->child_count];
                    break;
                }
            }
        }
    }

    /* Recursively delete children. */
    for (int i = e->child_count - 1; i >= 0; i--)
        jce_state_delete_entity(e->children[i]);

    /* Remove from selection. */
    jce_state_deselect_entity(id);

    /* Swap-remove from array. */
    idx = find_entity(id);  /* re-find after recursive deletes */
    if (idx >= 0) {
        int last = s.entity_count - 1;
        if (idx != last) {
            s.entities[idx] = s.entities[last];
            memcpy(s.components[idx], s.components[last],
                   sizeof(JceComponentInfo) * JCE_MAX_COMPONENTS);
        }
        s.entity_count--;
    }
}

void jce_state_rename_entity(uint32_t id, const char *name)
{
    JceEntityInfo *e = jce_state_get_entity(id);
    if (e) snprintf(e->name, sizeof(e->name), "%s", name);
}

void jce_state_set_entity_enabled(uint32_t id, bool enabled)
{
    JceEntityInfo *e = jce_state_get_entity(id);
    if (!e) return;

    e->enabled = enabled;
    for (int i = 0; i < e->child_count; i++)
        jce_state_set_entity_enabled(e->children[i], enabled);
}

void jce_state_set_entity_tag(uint32_t id, const char *tag)
{
    JceEntityInfo *e = jce_state_get_entity(id);
    if (e) snprintf(e->tag, sizeof(e->tag), "%s", tag ? tag : "");
}

void jce_state_set_entity_tag_color(uint32_t id, JceTagColor color)
{
    JceEntityInfo *e = jce_state_get_entity(id);
    if (e) e->tag_color = color;
}

void jce_state_reparent_entity(uint32_t id, uint32_t new_parent)
{
    JceEntityInfo *e = jce_state_get_entity(id);
    if (!e || e->id == new_parent) return;

    /* Cycle detection: ensure new_parent is not a descendant of id. */
    uint32_t check = new_parent;
    while (check != 0) {
        if (check == id) return;  /* would create cycle */
        JceEntityInfo *p = jce_state_get_entity(check);
        check = p ? p->parent_id : 0;
    }

    /* Remove from old parent. */
    if (e->parent_id != 0) {
        JceEntityInfo *old_p = jce_state_get_entity(e->parent_id);
        if (old_p) {
            for (int i = 0; i < old_p->child_count; i++) {
                if (old_p->children[i] == id) {
                    old_p->children[i] = old_p->children[--old_p->child_count];
                    break;
                }
            }
        }
    }

    /* Add to new parent. */
    e->parent_id = new_parent;
    if (new_parent != 0) {
        JceEntityInfo *new_p = jce_state_get_entity(new_parent);
        if (new_p && new_p->child_count < JCE_MAX_CHILDREN)
            new_p->children[new_p->child_count++] = id;
    }
}

uint32_t jce_state_duplicate_entity(uint32_t id)
{
    JceEntityInfo *src = jce_state_get_entity(id);
    if (!src) return 0;

    int src_idx = find_entity(id);
    char dup_name[JCE_MAX_ENTITY_NAME];
    snprintf(dup_name, sizeof(dup_name), "%s (Copy)", src->name);
    uint32_t dup = jce_state_create_entity(dup_name, src->parent_id);

    JceEntityInfo *d = jce_state_get_entity(dup);
    if (d) {
        d->tag_color = src->tag_color;
        d->enabled   = src->enabled;
        snprintf(d->tag, sizeof(d->tag), "%s", src->tag);
    }

    /* Copy components. */
    int dup_idx = find_entity(dup);
    if (src_idx >= 0 && dup_idx >= 0) {
        int count = s.entities[src_idx].component_count;
        if (count > JCE_MAX_COMPONENTS) count = JCE_MAX_COMPONENTS;
        memcpy(s.components[dup_idx], s.components[src_idx],
               sizeof(JceComponentInfo) * count);
        s.entities[dup_idx].component_count = count;
    }

    return dup;
}

/* ── Components (stub for demo) ────────────────────────────────────── */

static const char *s_comp_names[] = {
    "Transform",
    "Mesh Renderer",
    "Sprite Renderer",
    "Camera",
    "Light",
    "Animator",
    "Skeletal Animator",
    "Rigidbody",
    "Box Collider",
    "Sphere Collider",
    "Character Controller",
    "Audio Source",
    "Script",
};

const char *jce_component_type_name(JceComponentType type)
{
    if (type >= 0 && type < JCE_COMP_TYPE_COUNT)
        return s_comp_names[type];
    return "Unknown";
}

int jce_state_get_components(uint32_t entity_id, JceComponentInfo *out, int max)
{
    if (max < 1 || !out) return 0;

    int idx = find_entity(entity_id);
    if (idx < 0) return 0;

    JceEntityInfo *e = &s.entities[idx];
    int n = (e->component_count < max) ? e->component_count : max;
    for (int i = 0; i < n; i++)
        out[i] = s.components[idx][i];
    return n;
}

JceComponentInfo *jce_state_get_entity_components(uint32_t entity_id, int *out_count)
{
    int idx = find_entity(entity_id);
    if (idx < 0) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    if (out_count) *out_count = s.entities[idx].component_count;
    return s.components[idx];
}

void jce_state_set_component(uint32_t entity_id, const JceComponentInfo *comp)
{
    if (!comp) return;
    int idx = find_entity(entity_id);
    if (idx < 0) return;

    JceEntityInfo *e = &s.entities[idx];
    /* Update existing component of same type. */
    for (int i = 0; i < e->component_count; i++) {
        if (s.components[idx][i].type == comp->type) {
            s.components[idx][i] = *comp;
            return;
        }
    }
    /* Not found — add it. */
    if (e->component_count < JCE_MAX_COMPONENTS) {
        s.components[idx][e->component_count++] = *comp;
    }
}

void jce_state_add_component(uint32_t entity_id, JceComponentType type)
{
    int idx = find_entity(entity_id);
    if (idx < 0) return;

    JceEntityInfo *e = &s.entities[idx];
    /* Check for duplicate. */
    for (int i = 0; i < e->component_count; i++) {
        if (s.components[idx][i].type == type)
            return;
    }
    if (e->component_count >= JCE_MAX_COMPONENTS) return;

    JceComponentInfo *c = &s.components[idx][e->component_count++];
    memset(c, 0, sizeof(*c));
    c->type = type;
    c->expanded = true;

    /* Set sensible defaults. */
    switch (type) {
    case JCE_COMP_TRANSFORM:
        c->data.transform.scale[0] = 1.0f;
        c->data.transform.scale[1] = 1.0f;
        c->data.transform.scale[2] = 1.0f;
        break;
    case JCE_COMP_CAMERA:
        c->data.camera.fov = 60.0f;
        c->data.camera.near_clip = 0.1f;
        c->data.camera.far_clip = 1000.0f;
        break;
    case JCE_COMP_LIGHT:
        c->data.light.color[0] = 1.0f;
        c->data.light.color[1] = 1.0f;
        c->data.light.color[2] = 1.0f;
        c->data.light.color[3] = 1.0f;
        c->data.light.intensity = 1.0f;
        break;
    default:
        break;
    }

    LOG_INFO(LOG_TAG, "add component %s to entity %u",
             jce_component_type_name(type), entity_id);
}

void jce_state_remove_component(uint32_t entity_id, JceComponentType type)
{
    int idx = find_entity(entity_id);
    if (idx < 0) return;

    JceEntityInfo *e = &s.entities[idx];
    for (int i = 0; i < e->component_count; i++) {
        if (s.components[idx][i].type == type) {
            /* Swap-remove. */
            s.components[idx][i] = s.components[idx][--e->component_count];
            LOG_INFO(LOG_TAG, "remove component %s from entity %u",
                     jce_component_type_name(type), entity_id);
            return;
        }
    }
}

JceComponentType jce_component_type_from_name(const char *name)
{
    return component_type_from_name(name);
}

/* ── Mode Accessors ────────────────────────────────────────────────── */

void          jce_state_set_edit_mode(JceEditMode m)       { s.edit_mode = m; }
JceEditMode   jce_state_get_edit_mode(void)                { return s.edit_mode; }

void          jce_state_set_gizmo_mode(JceGizmoMode m)     { s.gizmo_mode = m; }
JceGizmoMode  jce_state_get_gizmo_mode(void)               { return s.gizmo_mode; }

void          jce_state_set_gizmo_space(JceGizmoSpace sp)  { s.gizmo_space = sp; }
JceGizmoSpace jce_state_get_gizmo_space(void)              { return s.gizmo_space; }

void              jce_state_set_view_mode(JceSceneViewMode m)  { s.view_mode = m; }
JceSceneViewMode  jce_state_get_view_mode(void)                { return s.view_mode; }

bool  jce_state_get_show_grid(void)          { return s.show_grid; }
void  jce_state_set_show_grid(bool show)     { s.show_grid = show; }

bool  jce_state_get_2d_mode(void)            { return s.is_2d_mode; }
void  jce_state_set_2d_mode(bool is_2d)      { s.is_2d_mode = is_2d; }

bool  jce_state_get_live_preview(void)       { return s.live_preview; }
void  jce_state_set_live_preview(bool on)    { s.live_preview = on; }

bool jce_state_load_scene_file(const char *scene_path)
{
    if (!scene_path || scene_path[0] == '\0')
        return false;

    /* Set the scene base directory for mesh/texture resolution. */
    update_scene_dir_from_path(scene_path);

    FILE *fp = fopen(scene_path, "rb");
    if (!fp) {
        LOG_WARN(LOG_TAG, "scene load failed, cannot open file: %s", scene_path);
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0) {
        fclose(fp);
        LOG_WARN(LOG_TAG, "scene load failed, invalid size: %s", scene_path);
        return false;
    }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        LOG_WARN(LOG_TAG, "scene load failed, out of memory: %s", scene_path);
        return false;
    }

    size_t n = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        LOG_WARN(LOG_TAG, "scene JSON parse failed: %s", scene_path);
        return false;
    }

    const cJSON *container = root;
    if (cJSON_IsObject(root)) {
        const cJSON *scene = cJSON_GetObjectItemCaseSensitive(root, "scene");
        if (cJSON_IsObject(scene))
            container = scene;
    }

    clear_scene_entities();
    bool loaded_any = false;

    if (cJSON_IsArray(container)) {
        int before = s.entity_count;
        load_entities_from_array(container);
        loaded_any = (s.entity_count > before);
    } else if (cJSON_IsObject(container)) {
        static const char *const array_keys[] = {
            "entities", "objects", "nodes", "children"
        };
        const cJSON *entities_data = json_get_any(container, array_keys,
            (int)(sizeof(array_keys) / sizeof(array_keys[0])));
        if (cJSON_IsArray(entities_data)) {
            int before = s.entity_count;
            load_entities_from_array(entities_data);
            loaded_any = loaded_any || (s.entity_count > before);
        } else if (cJSON_IsObject(entities_data)) {
            int before = s.entity_count;
            load_entities_from_object_map(entities_data);
            loaded_any = loaded_any || (s.entity_count > before);
        }

        static const char *const root_node_keys[] = {
            "root", "sceneRoot", "hierarchyRoot"
        };
        const cJSON *root_node = json_get_any(container, root_node_keys,
            (int)(sizeof(root_node_keys) / sizeof(root_node_keys[0])));
        if (cJSON_IsObject(root_node)) {
            int before = s.entity_count;
            load_entity_tree_node(root_node, 0);
            loaded_any = loaded_any || (s.entity_count > before);
        }

        if (!loaded_any && looks_like_entity_object(container)) {
            int before = s.entity_count;
            load_entity_tree_node(container, 0);
            loaded_any = loaded_any || (s.entity_count > before);
        }
    }

    if (!loaded_any)
        LOG_WARN(LOG_TAG, "scene load: no supported entity data in %s", scene_path);

    jce_state_clear_selection();
    set_current_scene_path_internal(scene_path);
    LOG_INFO(LOG_TAG, "scene loaded from %s (%d entities)", scene_path, s.entity_count);

    cJSON_Delete(root);
    return true;
}

static const char *component_type_save_name(JceComponentType type)
{
    switch (type) {
    case JCE_COMP_TRANSFORM:          return "Transform";
    case JCE_COMP_MESH_RENDERER:      return "MeshRenderer";
    case JCE_COMP_SPRITE_RENDERER:    return "SpriteRenderer";
    case JCE_COMP_CAMERA:             return "Camera";
    case JCE_COMP_LIGHT:              return "Light";
    case JCE_COMP_ANIMATOR:           return "Animator";
    case JCE_COMP_SKELETAL_ANIMATOR:  return "SkeletalAnimator";
    case JCE_COMP_RIGIDBODY:          return "Rigidbody";
    case JCE_COMP_BOX_COLLIDER:       return "BoxCollider";
    case JCE_COMP_SPHERE_COLLIDER:    return "SphereCollider";
    case JCE_COMP_CHARACTER_CONTROLLER:return "CharacterController";
    case JCE_COMP_AUDIO_SOURCE:       return "AudioSource";
    case JCE_COMP_SCRIPT:             return "Script";
    default:                          return "Unknown";
    }
}

static cJSON *serialize_component_json(const JceComponentInfo *comp)
{
    if (!comp) return NULL;

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "type", component_type_save_name(comp->type));

    switch (comp->type) {
    case JCE_COMP_TRANSFORM:
        cJSON_AddNumberToObject(obj, "posX", comp->data.transform.pos[0]);
        cJSON_AddNumberToObject(obj, "posY", comp->data.transform.pos[1]);
        cJSON_AddNumberToObject(obj, "posZ", comp->data.transform.pos[2]);
        cJSON_AddNumberToObject(obj, "rotX", comp->data.transform.rot[0]);
        cJSON_AddNumberToObject(obj, "rotY", comp->data.transform.rot[1]);
        cJSON_AddNumberToObject(obj, "rotZ", comp->data.transform.rot[2]);
        cJSON_AddNumberToObject(obj, "scaleX", comp->data.transform.scale[0]);
        cJSON_AddNumberToObject(obj, "scaleY", comp->data.transform.scale[1]);
        cJSON_AddNumberToObject(obj, "scaleZ", comp->data.transform.scale[2]);
        break;
    case JCE_COMP_MESH_RENDERER:
        cJSON_AddStringToObject(obj, "meshPath", comp->data.mesh_renderer.mesh_path);
        cJSON_AddStringToObject(obj, "materialPath", comp->data.mesh_renderer.material_path);
        break;
    case JCE_COMP_CAMERA:
        cJSON_AddNumberToObject(obj, "fov", comp->data.camera.fov);
        cJSON_AddNumberToObject(obj, "nearClip", comp->data.camera.near_clip);
        cJSON_AddNumberToObject(obj, "farClip", comp->data.camera.far_clip);
        cJSON_AddBoolToObject(obj, "orthographic", comp->data.camera.ortho);
        break;
    case JCE_COMP_LIGHT:
        cJSON_AddNumberToObject(obj, "colorR", comp->data.light.color[0]);
        cJSON_AddNumberToObject(obj, "colorG", comp->data.light.color[1]);
        cJSON_AddNumberToObject(obj, "colorB", comp->data.light.color[2]);
        cJSON_AddNumberToObject(obj, "colorA", comp->data.light.color[3]);
        cJSON_AddNumberToObject(obj, "intensity", comp->data.light.intensity);
        cJSON_AddNumberToObject(obj, "lightType", comp->data.light.type);
        break;
    default:
        break;
    }

    return obj;
}

bool jce_state_save_scene_file(const char *scene_path)
{
    if (!scene_path || scene_path[0] == '\0')
        return false;

    cJSON *root = cJSON_CreateObject();
    cJSON *scene = cJSON_CreateObject();
    cJSON *entities = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "scene", scene);
    cJSON_AddItemToObject(scene, "entities", entities);

    for (int i = 0; i < s.entity_count; i++) {
        JceEntityInfo *e = &s.entities[i];
        cJSON *eobj = cJSON_CreateObject();

        cJSON_AddNumberToObject(eobj, "id", (double)e->id);
        cJSON_AddStringToObject(eobj, "name", e->name);
        cJSON_AddNumberToObject(eobj, "parentId", (double)e->parent_id);
        cJSON_AddBoolToObject(eobj, "enabled", e->enabled);
        if (e->tag[0] != '\0')
            cJSON_AddStringToObject(eobj, "tag", e->tag);
        cJSON_AddNumberToObject(eobj, "tagColor", (double)e->tag_color);

        cJSON *comps = cJSON_CreateArray();
        for (int ci = 0; ci < e->component_count; ci++) {
            cJSON *cobj = serialize_component_json(&s.components[i][ci]);
            if (cobj)
                cJSON_AddItemToArray(comps, cobj);
        }
        cJSON_AddItemToObject(eobj, "components", comps);
        cJSON_AddItemToArray(entities, eobj);
    }

    char *json_text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json_text) {
        LOG_WARN(LOG_TAG, "scene save failed, JSON serialization error: %s", scene_path);
        return false;
    }

    FILE *fp = fopen(scene_path, "wb");
    if (!fp) {
        LOG_WARN(LOG_TAG, "scene save failed, cannot open file: %s", scene_path);
        free(json_text);
        return false;
    }

    size_t len = strlen(json_text);
    size_t wr = fwrite(json_text, 1, len, fp);
    fclose(fp);
    free(json_text);

    if (wr != len) {
        LOG_WARN(LOG_TAG, "scene save failed, short write: %s", scene_path);
        return false;
    }

    update_scene_dir_from_path(scene_path);
    set_current_scene_path_internal(scene_path);
    LOG_INFO(LOG_TAG, "scene saved to %s (%d entities)", scene_path, s.entity_count);
    return true;
}

const char *jce_state_get_current_scene_path(void)
{
    return s.current_scene_path;
}

/* ── Play Mode ─────────────────────────────────────────────────────── */

void jce_state_play(void)
{
    if (s.play_state == JCE_PLAY_STOPPED) {
        s.play_state = JCE_PLAY_PLAYING;
        LOG_INFO(LOG_TAG, "play mode started");
    }
}

void jce_state_pause(void)
{
    if (s.play_state == JCE_PLAY_PLAYING) {
        s.play_state = JCE_PLAY_PAUSED;
        LOG_INFO(LOG_TAG, "play mode paused");
    } else if (s.play_state == JCE_PLAY_PAUSED) {
        s.play_state = JCE_PLAY_PLAYING;
        LOG_INFO(LOG_TAG, "play mode resumed");
    }
}

void jce_state_stop(void)
{
    if (s.play_state != JCE_PLAY_STOPPED) {
        s.play_state = JCE_PLAY_STOPPED;
        LOG_INFO(LOG_TAG, "play mode stopped");
    }
}

JcePlayState jce_state_get_play_state(void) { return s.play_state; }

/* ── Entity Clipboard ──────────────────────────────────────────────── */

static struct {
    uint32_t    id;
    char        name[JCE_MAX_ENTITY_NAME];
    JceTagColor tag_color;
    char        tag[JCE_MAX_TAG_STRING];
} s_clipboard;

void jce_state_copy_entity(uint32_t id)
{
    JceEntityInfo *e = jce_state_get_entity(id);
    if (!e) return;
    s_clipboard.id = id;
    snprintf(s_clipboard.name, sizeof(s_clipboard.name), "%s", e->name);
    s_clipboard.tag_color = e->tag_color;
    snprintf(s_clipboard.tag, sizeof(s_clipboard.tag), "%s", e->tag);
    LOG_INFO(LOG_TAG, "copied entity %u (%s)", id, e->name);
}

uint32_t jce_state_paste_entity(uint32_t parent_id)
{
    if (s_clipboard.id == 0) return 0;

    char paste_name[JCE_MAX_ENTITY_NAME];
    snprintf(paste_name, sizeof(paste_name), "%s (Paste)", s_clipboard.name);
    uint32_t new_id = jce_state_create_entity(paste_name, parent_id);

    JceEntityInfo *e = jce_state_get_entity(new_id);
    if (e) {
        e->tag_color = s_clipboard.tag_color;
        snprintf(e->tag, sizeof(e->tag), "%s", s_clipboard.tag);
    }

    LOG_INFO(LOG_TAG, "pasted entity as %u (%s)", new_id, paste_name);
    return new_id;
}

bool jce_state_has_copied(void)
{
    return s_clipboard.id != 0;
}

/* ── Undo / Redo (stubs) ──────────────────────────────────────────── */

void  jce_state_undo(void)      { LOG_INFO(LOG_TAG, "undo (stub)"); }
void  jce_state_redo(void)      { LOG_INFO(LOG_TAG, "redo (stub)"); }
bool  jce_state_can_undo(void)  { return false; }
bool  jce_state_can_redo(void)  { return false; }
