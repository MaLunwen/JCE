/*
 * jce_editor_state.cpp  Central editor state implementation.
 *
 * Manages entity selection, edit modes, play state, and demo scene data.
 * When ECS integration is added (Phase 3), the entity storage will be
 * replaced by queries into the actual ECS world.
 */

#include "jce_editor_state.h"
#include "jce_editor_scene_render.h"
#include "jce_editor_defaults.h"

#include <cjson/cJSON.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <vector>
#include <string>
#include <utility>

extern "C" {
#include <jce/core/jce_log.h>
#include <jce/resource/jce_scene_contract.h>
#include <jce/physics/jce_physics.h>
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

struct EditorHistorySnapshot {
    std::string scene_json;
    std::string scene_path;
};

static std::vector<EditorHistorySnapshot> s_undo_history;
static std::vector<EditorHistorySnapshot> s_redo_history;
static int s_history_suspend_depth = 0;
static int s_history_edit_nesting = 0;
static bool s_history_outer_edit_pushed_snapshot = false;
static int s_history_manual_batch_depth = 0;

static struct {
    bool active;
    char label[64];
    EditorHistorySnapshot before;
} s_transaction;

static bool history_begin_edit(void);
static void history_end_edit(bool active);
static bool history_capture_snapshot(EditorHistorySnapshot *out);
static bool history_push_undo_snapshot(void);
static bool history_restore_snapshot(const EditorHistorySnapshot &snapshot,
                                    const char *reason);
static cJSON *serialize_component_json(const JceComponentInfo *comp);
static bool parse_scene_contract_version(const cJSON *root, int *out_major, int *out_minor);
static cJSON *serialize_entity_tree_json(uint32_t entity_id);
static cJSON *build_prefab_json_root(uint32_t entity_id);
static const cJSON *find_prefab_root_node(const cJSON *root);
static void mark_prefab_instance_recursive(uint32_t entity_id, const char *prefab_path);

struct HistorySuspendScope {
    HistorySuspendScope() { ++s_history_suspend_depth; }
    ~HistorySuspendScope() {
        if (s_history_suspend_depth > 0)
            --s_history_suspend_depth;
    }
};

struct HistoryEditScope {
    bool active;
    HistoryEditScope() : active(history_begin_edit()) {}
    ~HistoryEditScope() { history_end_edit(active); }
};

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
    /* Position cube at (0, 0.5, 0) so it sits on the Y=0 ground. */
    {
        int idx = find_entity(cube);
        if (idx >= 0) {
            for (int i = 0; i < s.entities[idx].component_count; i++) {
                if (s.components[idx][i].type == JCE_COMP_TRANSFORM)
                    s.components[idx][i].data.transform.pos[1] = 0.5f;
            }
        }
    }

    uint32_t sphere = jce_state_create_entity("Sphere", objs);
    jce_state_add_component(sphere, JCE_COMP_TRANSFORM);
    jce_state_add_component(sphere, JCE_COMP_MESH_RENDERER);
    /* Sphere at (2.5, 0.5, 0) — to the right of the cube. */
    {
        int idx = find_entity(sphere);
        if (idx >= 0) {
            for (int i = 0; i < s.entities[idx].component_count; i++) {
                if (s.components[idx][i].type == JCE_COMP_TRANSFORM) {
                    s.components[idx][i].data.transform.pos[0] = 2.5f;
                    s.components[idx][i].data.transform.pos[1] = 0.5f;
                }
                if (s.components[idx][i].type == JCE_COMP_MESH_RENDERER)
                    s.components[idx][i].data.mesh_renderer.mesh_shape = JCE_MESH_SHAPE_SPHERE;
            }
        }
    }

    uint32_t plane = jce_state_create_entity("Plane", objs);
    jce_state_add_component(plane, JCE_COMP_TRANSFORM);
    jce_state_add_component(plane, JCE_COMP_MESH_RENDERER);
    /* Plane at (-2.5, 0, 0) — to the left, acts as a flat ground patch. */
    {
        int idx = find_entity(plane);
        if (idx >= 0) {
            for (int i = 0; i < s.entities[idx].component_count; i++) {
                if (s.components[idx][i].type == JCE_COMP_TRANSFORM) {
                    s.components[idx][i].data.transform.pos[0] = -2.5f;
                    s.components[idx][i].data.transform.scale[0] = 3.0f;
                    s.components[idx][i].data.transform.scale[1] = 3.0f;
                    s.components[idx][i].data.transform.scale[2] = 3.0f;
                }
                if (s.components[idx][i].type == JCE_COMP_MESH_RENDERER)
                    s.components[idx][i].data.mesh_renderer.mesh_shape = JCE_MESH_SHAPE_PLANE;
            }
        }
    }

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

/* ── JSON String Helper ───────────────────────────────────────────── */

static const char *json_get_string(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : NULL;
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
        /* Mesh shape enum. */
        out->data.mesh_renderer.mesh_shape = (int)json_get_float(props, "meshShape", 0.0f);
        /* PBR parameters (inline). */
        out->data.mesh_renderer.base_color[0] = json_get_float(props, "baseColorR", 1.0f);
        out->data.mesh_renderer.base_color[1] = json_get_float(props, "baseColorG", 1.0f);
        out->data.mesh_renderer.base_color[2] = json_get_float(props, "baseColorB", 1.0f);
        out->data.mesh_renderer.base_color[3] = json_get_float(props, "baseColorA", 1.0f);
        out->data.mesh_renderer.metallic      = json_get_float(props, "metallic", 0.0f);
        out->data.mesh_renderer.roughness     = json_get_float(props, "roughness", 1.0f);
        out->data.mesh_renderer.emissive[0]   = json_get_float(props, "emissiveR", 0.0f);
        out->data.mesh_renderer.emissive[1]   = json_get_float(props, "emissiveG", 0.0f);
        out->data.mesh_renderer.emissive[2]   = json_get_float(props, "emissiveB", 0.0f);
        out->data.mesh_renderer.normal_scale  = json_get_float(props, "normalScale", 1.0f);
        out->data.mesh_renderer.ao_strength   = json_get_float(props, "aoStrength", 1.0f);
        out->data.mesh_renderer.alpha_mode    = (int)json_get_float(props, "alphaMode", 0.0f);
        out->data.mesh_renderer.alpha_cutoff  = json_get_float(props, "alphaCutoff", 0.5f);
        {
            const cJSON *ds = cJSON_GetObjectItemCaseSensitive(props, "doubleSided");
            out->data.mesh_renderer.double_sided = cJSON_IsTrue(ds);
        }
        /* Texture paths. */
        { const char *v = json_get_string(props, "albedoTex");
          if (v) snprintf(out->data.mesh_renderer.albedo_tex, 128, "%s", v); }
        { const char *v = json_get_string(props, "mrTex");
          if (v) snprintf(out->data.mesh_renderer.mr_tex, 128, "%s", v); }
        { const char *v = json_get_string(props, "normalTex");
          if (v) snprintf(out->data.mesh_renderer.normal_tex, 128, "%s", v); }
        { const char *v = json_get_string(props, "aoTex");
          if (v) snprintf(out->data.mesh_renderer.ao_tex, 128, "%s", v); }
        { const char *v = json_get_string(props, "emissiveTex");
          if (v) snprintf(out->data.mesh_renderer.emissive_tex, 128, "%s", v); }
        break;
    }

    case JCE_COMP_SPRITE_RENDERER:
        { const char *v = json_get_string(props, "spritePath");
          if (v) snprintf(out->data.sprite_renderer.sprite_path, 128, "%s", v); }
        out->data.sprite_renderer.color[0] = json_get_float(props, "colorR", 1.0f);
        out->data.sprite_renderer.color[1] = json_get_float(props, "colorG", 1.0f);
        out->data.sprite_renderer.color[2] = json_get_float(props, "colorB", 1.0f);
        out->data.sprite_renderer.color[3] = json_get_float(props, "colorA", 1.0f);
        out->data.sprite_renderer.flip_x = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "flipX"));
        out->data.sprite_renderer.flip_y = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "flipY"));
        out->data.sprite_renderer.sorting_order = (int)json_get_float(props, "sortingOrder", 0.0f);
        break;

    case JCE_COMP_ANIMATOR:
        { const char *v = json_get_string(props, "clipName");
          if (v) snprintf(out->data.animator.clip_name, 64, "%s", v); }
        out->data.animator.speed   = json_get_float(props, "speed", 1.0f);
        out->data.animator.loop    = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "loop"));
        out->data.animator.playing = false;
        break;

    case JCE_COMP_SKELETAL_ANIMATOR:
        { const char *v = json_get_string(props, "skeletonPath");
          if (v) snprintf(out->data.skeletal_animator.skeleton_path, 128, "%s", v); }
        out->data.skeletal_animator.speed       = json_get_float(props, "speed", 1.0f);
        out->data.skeletal_animator.loop        = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "loop"));
        out->data.skeletal_animator.playing     = false;
        out->data.skeletal_animator.active_clip = (int)json_get_float(props, "activeClip", 0.0f);
        {
            const cJSON *clips = cJSON_GetObjectItemCaseSensitive(props, "clipNames");
            out->data.skeletal_animator.clip_count = 0;
            if (cJSON_IsArray(clips)) {
                int n = cJSON_GetArraySize(clips);
                if (n > 8) n = 8;
                for (int ci = 0; ci < n; ci++) {
                    const cJSON *ce = cJSON_GetArrayItem(clips, ci);
                    if (cJSON_IsString(ce) && ce->valuestring)
                        snprintf(out->data.skeletal_animator.clip_names[ci], 64, "%s", ce->valuestring);
                }
                out->data.skeletal_animator.clip_count = n;
            }
        }
        break;

    case JCE_COMP_RIGIDBODY:
        out->data.rigidbody.mass         = json_get_float(props, "mass", 1.0f);
        out->data.rigidbody.drag         = json_get_float(props, "drag", 0.0f);
        out->data.rigidbody.angular_drag = json_get_float(props, "angularDrag", 0.05f);
        out->data.rigidbody.use_gravity  = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "useGravity"));
        out->data.rigidbody.is_kinematic = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "isKinematic"));
        /* Default: gravity on if field absent. */
        if (!cJSON_GetObjectItemCaseSensitive(props, "useGravity"))
            out->data.rigidbody.use_gravity = true;
        break;

    case JCE_COMP_BOX_COLLIDER:
        out->data.box_collider.center[0] = json_get_float(props, "centerX", 0.0f);
        out->data.box_collider.center[1] = json_get_float(props, "centerY", 0.0f);
        out->data.box_collider.center[2] = json_get_float(props, "centerZ", 0.0f);
        out->data.box_collider.size[0]   = json_get_float(props, "sizeX", 1.0f);
        out->data.box_collider.size[1]   = json_get_float(props, "sizeY", 1.0f);
        out->data.box_collider.size[2]   = json_get_float(props, "sizeZ", 1.0f);
        out->data.box_collider.is_trigger = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "isTrigger"));
        break;

    case JCE_COMP_SPHERE_COLLIDER:
        out->data.sphere_collider.center[0] = json_get_float(props, "centerX", 0.0f);
        out->data.sphere_collider.center[1] = json_get_float(props, "centerY", 0.0f);
        out->data.sphere_collider.center[2] = json_get_float(props, "centerZ", 0.0f);
        out->data.sphere_collider.radius     = json_get_float(props, "radius", 0.5f);
        out->data.sphere_collider.is_trigger = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "isTrigger"));
        break;

    case JCE_COMP_CHARACTER_CONTROLLER:
        out->data.character_controller.height      = json_get_float(props, "height", 2.0f);
        out->data.character_controller.radius      = json_get_float(props, "radius", 0.5f);
        out->data.character_controller.step_offset  = json_get_float(props, "stepOffset", 0.3f);
        out->data.character_controller.slope_limit  = json_get_float(props, "slopeLimit", 45.0f);
        break;

    case JCE_COMP_AUDIO_SOURCE:
        { const char *v = json_get_string(props, "clipPath");
          if (v) snprintf(out->data.audio_source.clip_path, 128, "%s", v); }
        out->data.audio_source.volume        = json_get_float(props, "volume", 1.0f);
        out->data.audio_source.pitch         = json_get_float(props, "pitch", 1.0f);
        out->data.audio_source.spatial_blend  = json_get_float(props, "spatialBlend", 0.0f);
        out->data.audio_source.loop          = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "loop"));
        out->data.audio_source.play_on_awake = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "playOnAwake"));
        if (!cJSON_GetObjectItemCaseSensitive(props, "playOnAwake"))
            out->data.audio_source.play_on_awake = true;
        break;

    case JCE_COMP_SCRIPT:
        { const char *v = json_get_string(props, "scriptPath");
          if (v) snprintf(out->data.script.script_path, 128, "%s", v); }
        break;

    default:
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
    static const char *const prefab_path_keys[] = { "prefabPath", "prefab_path" };
    static const char *const prefab_instance_keys[] = {
        "prefabInstance", "prefab_instance", "isPrefabInstance"
    };

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

    bool prefab_instance = e->prefab_instance;
    if (json_get_bool_any(obj, prefab_instance_keys,
        (int)(sizeof(prefab_instance_keys) / sizeof(prefab_instance_keys[0])),
        &prefab_instance)) {
        e->prefab_instance = prefab_instance;
        if (!prefab_instance)
            e->prefab_path[0] = '\0';
    }

    const char *prefab_path = json_get_string_any(obj, prefab_path_keys,
        (int)(sizeof(prefab_path_keys) / sizeof(prefab_path_keys[0])));
    if (prefab_path && prefab_path[0] != '\0') {
        snprintf(e->prefab_path, sizeof(e->prefab_path), "%s", prefab_path);
        e->prefab_instance = true;
    }

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

static bool parse_scene_contract_version(const cJSON *root, int *out_major, int *out_minor)
{
    if (out_major) *out_major = (int)JCE_SCENE_CONTRACT_MAJOR;
    if (out_minor) *out_minor = (int)JCE_SCENE_CONTRACT_MINOR;
    if (!cJSON_IsObject(root)) return false;

    const cJSON *contract = cJSON_GetObjectItemCaseSensitive(root,
        JCE_SCENE_CONTRACT_KEY);
    if (cJSON_IsObject(contract)) {
        const cJSON *major_item = cJSON_GetObjectItemCaseSensitive(contract,
            JCE_SCENE_CONTRACT_MAJOR_KEY);
        const cJSON *minor_item = cJSON_GetObjectItemCaseSensitive(contract,
            JCE_SCENE_CONTRACT_MINOR_KEY);

        if (cJSON_IsNumber(major_item) && out_major)
            *out_major = major_item->valueint;
        if (cJSON_IsNumber(minor_item) && out_minor)
            *out_minor = minor_item->valueint;
        return true;
    }

    const cJSON *legacy_version = cJSON_GetObjectItemCaseSensitive(root,
        JCE_SCENE_VERSION_KEY);
    if (cJSON_IsNumber(legacy_version)) {
        if (out_major) *out_major = legacy_version->valueint;
        if (out_minor) *out_minor = 0;
        return true;
    }

    return false;
}

/* ── Undo/Redo helpers ─────────────────────────────────────────────── */

static bool history_begin_edit(void)
{
    if (s_history_suspend_depth > 0)
        return false;

    if (s_history_edit_nesting == 0)
        s_history_outer_edit_pushed_snapshot = history_push_undo_snapshot();

    ++s_history_edit_nesting;
    return true;
}

static void history_end_edit(bool active)
{
    if (!active)
        return;
    if (s_history_edit_nesting <= 0)
        return;

    --s_history_edit_nesting;
    if (s_history_edit_nesting != 0)
        return;

    bool changed = true;
    EditorHistorySnapshot current;
    if (history_capture_snapshot(&current) && !s_undo_history.empty()) {
        const EditorHistorySnapshot &before = s_undo_history.back();
        changed = !(before.scene_json == current.scene_json
                    && before.scene_path == current.scene_path);
    }

    if (changed) {
        s_redo_history.clear();
    } else if (s_history_outer_edit_pushed_snapshot && !s_undo_history.empty()) {
        s_undo_history.pop_back();
    }

    s_history_outer_edit_pushed_snapshot = false;
}

static cJSON *build_scene_json_root(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *contract = cJSON_CreateObject();
    cJSON *scene = cJSON_CreateObject();
    cJSON *entities = cJSON_CreateArray();
    if (!root || !contract || !scene || !entities) {
        cJSON_Delete(root);
        cJSON_Delete(contract);
        cJSON_Delete(scene);
        cJSON_Delete(entities);
        return NULL;
    }

    cJSON_AddItemToObject(root, JCE_SCENE_CONTRACT_KEY, contract);
    cJSON_AddStringToObject(contract, JCE_SCENE_CONTRACT_NAME_KEY,
                            JCE_SCENE_CONTRACT_NAME);
    cJSON_AddNumberToObject(contract, JCE_SCENE_CONTRACT_MAJOR_KEY,
                            JCE_SCENE_CONTRACT_MAJOR);
    cJSON_AddNumberToObject(contract, JCE_SCENE_CONTRACT_MINOR_KEY,
                            JCE_SCENE_CONTRACT_MINOR);

    cJSON_AddItemToObject(root, JCE_SCENE_ROOT_KEY, scene);
    cJSON_AddNumberToObject(scene, JCE_SCENE_VERSION_KEY,
                            JCE_SCENE_CONTRACT_MAJOR);
    cJSON_AddItemToObject(scene, JCE_SCENE_ENTITIES_KEY, entities);

    for (int i = 0; i < s.entity_count; i++) {
        JceEntityInfo *e = &s.entities[i];
        cJSON *eobj = cJSON_CreateObject();
        if (!eobj)
            continue;

        cJSON_AddNumberToObject(eobj, "id", (double)e->id);
        cJSON_AddStringToObject(eobj, "name", e->name);
        cJSON_AddNumberToObject(eobj, "parentId", (double)e->parent_id);
        cJSON_AddBoolToObject(eobj, "enabled", e->enabled);
        if (e->tag[0] != '\0')
            cJSON_AddStringToObject(eobj, "tag", e->tag);
        cJSON_AddNumberToObject(eobj, "tagColor", (double)e->tag_color);
        if (e->prefab_instance) {
            cJSON_AddBoolToObject(eobj, "prefabInstance", true);
            if (e->prefab_path[0] != '\0')
                cJSON_AddStringToObject(eobj, "prefabPath", e->prefab_path);
        }

        cJSON *comps = cJSON_CreateArray();
        for (int ci = 0; ci < e->component_count; ci++) {
            cJSON *cobj = serialize_component_json(&s.components[i][ci]);
            if (cobj)
                cJSON_AddItemToArray(comps, cobj);
        }
        cJSON_AddItemToObject(eobj, "components", comps);
        cJSON_AddItemToArray(entities, eobj);
    }

    return root;
}

static bool load_scene_from_parsed_root(const cJSON *root,
                                        const char *scene_label,
                                        const char *scene_path)
{
    if (!root)
        return false;

    int contract_major = (int)JCE_SCENE_CONTRACT_MAJOR;
    int contract_minor = (int)JCE_SCENE_CONTRACT_MINOR;
    parse_scene_contract_version(root, &contract_major, &contract_minor);
    if (!jce_scene_contract_major_compatible((uint32_t)contract_major)) {
        LOG_WARN(LOG_TAG,
                 "scene load failed: unsupported contract major %d (expected %u)",
                 contract_major,
                 (unsigned)JCE_SCENE_CONTRACT_MAJOR);
        return false;
    }
    (void)contract_minor;

    const cJSON *container = root;
    if (cJSON_IsObject(root)) {
        const cJSON *scene = cJSON_GetObjectItemCaseSensitive(root,
            JCE_SCENE_ROOT_KEY);
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
            JCE_SCENE_ENTITIES_KEY, "objects", "nodes", "children"
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

    const char *label = (scene_label && scene_label[0] != '\0')
        ? scene_label
        : "<memory>";
    if (!loaded_any)
        LOG_WARN(LOG_TAG, "scene load: no supported entity data in %s", label);

    jce_state_clear_selection();
    if (scene_path && scene_path[0] != '\0') {
        update_scene_dir_from_path(scene_path);
        set_current_scene_path_internal(scene_path);
    } else {
        set_current_scene_path_internal(NULL);
    }

    LOG_INFO(LOG_TAG, "scene loaded from %s (%d entities)", label, s.entity_count);
    return true;
}

static bool history_capture_snapshot(EditorHistorySnapshot *out)
{
    if (!out)
        return false;

    cJSON *root = build_scene_json_root();
    if (!root)
        return false;

    char *json_text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_text)
        return false;

    out->scene_json = json_text;
    out->scene_path = s.current_scene_path;
    free(json_text);
    return true;
}

static bool history_push_undo_snapshot(void)
{
    if (s_history_suspend_depth > 0)
        return false;

    EditorHistorySnapshot snap;
    if (!history_capture_snapshot(&snap))
        return false;

    if (!s_undo_history.empty()) {
        const EditorHistorySnapshot &last = s_undo_history.back();
        if (last.scene_json == snap.scene_json && last.scene_path == snap.scene_path)
            return false;
    }

    if ((int)s_undo_history.size() >= JCE_UNDO_HISTORY_LIMIT)
        s_undo_history.erase(s_undo_history.begin());

    s_undo_history.push_back(std::move(snap));
    return true;
}

static bool history_restore_snapshot(const EditorHistorySnapshot &snapshot,
                                    const char *reason)
{
    if (snapshot.scene_json.empty())
        return false;

    cJSON *root = cJSON_Parse(snapshot.scene_json.c_str());
    if (!root) {
        LOG_WARN(LOG_TAG, "history restore parse failed (%s)",
                 reason ? reason : "unknown");
        return false;
    }

    HistorySuspendScope suspend;
    const char *label = snapshot.scene_path.empty()
        ? (reason ? reason : "history")
        : snapshot.scene_path.c_str();
    bool ok = load_scene_from_parsed_root(root, label, snapshot.scene_path.c_str());
    cJSON_Delete(root);
    return ok;
}

/* ── Init / Shutdown ───────────────────────────────────────────────── */

void jce_editor_state_init(void)
{
    memset(&s, 0, sizeof(s));
    s_undo_history.clear();
    s_redo_history.clear();
    s_history_suspend_depth = 0;
    s_history_edit_nesting = 0;
    s_history_outer_edit_pushed_snapshot = false;
    s_history_manual_batch_depth = 0;
    s_transaction.active = false;
    s_transaction.label[0] = '\0';
    s_transaction.before.scene_json.clear();
    s_transaction.before.scene_path.clear();

    s.edit_mode   = JCE_EDIT_MODE_SELECT;
    s.gizmo_mode  = JCE_GIZMO_TRANSLATE;
    s.gizmo_space = JCE_GIZMO_LOCAL;
    s.view_mode   = JCE_VIEW_SHADED;
    s.play_state  = JCE_PLAY_STOPPED;
    s.show_grid   = true;
    s.current_scene_path[0] = '\0';

    {
        HistorySuspendScope suspend;
        clear_scene_entities();
        build_demo_scene();
    }

    s.initialized = true;
    LOG_INFO(LOG_TAG, "editor state initialized (%d demo entities)", s.entity_count);
}

void jce_editor_state_shutdown(void)
{
    s_undo_history.clear();
    s_redo_history.clear();
    s_history_suspend_depth = 0;
    s_history_edit_nesting = 0;
    s_history_outer_edit_pushed_snapshot = false;
    s_history_manual_batch_depth = 0;
    s_transaction.active = false;
    s_transaction.label[0] = '\0';
    s_transaction.before.scene_json.clear();
    s_transaction.before.scene_path.clear();
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
    HistoryEditScope edit_scope;

    if (s.entity_count >= JCE_MAX_ENTITIES) return 0;

    JceEntityInfo *e = &s.entities[s.entity_count++];
    memset(e, 0, sizeof(*e));
    e->id = s.next_id++;
    snprintf(e->name, sizeof(e->name), "%s", name ? name : "Entity");
    e->enabled   = true;
    e->parent_id = parent_id;
    e->tag_color = JCE_TAG_NONE;
    e->prefab_instance = false;
    e->prefab_path[0] = '\0';

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
    HistoryEditScope edit_scope;

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
    HistoryEditScope edit_scope;

    JceEntityInfo *e = jce_state_get_entity(id);
    if (e) snprintf(e->name, sizeof(e->name), "%s", name);
}

void jce_state_set_entity_enabled(uint32_t id, bool enabled)
{
    HistoryEditScope edit_scope;

    JceEntityInfo *e = jce_state_get_entity(id);
    if (!e) return;

    e->enabled = enabled;
    for (int i = 0; i < e->child_count; i++)
        jce_state_set_entity_enabled(e->children[i], enabled);
}

void jce_state_set_entity_tag(uint32_t id, const char *tag)
{
    HistoryEditScope edit_scope;

    JceEntityInfo *e = jce_state_get_entity(id);
    if (e) snprintf(e->tag, sizeof(e->tag), "%s", tag ? tag : "");
}

void jce_state_set_entity_tag_color(uint32_t id, JceTagColor color)
{
    HistoryEditScope edit_scope;

    JceEntityInfo *e = jce_state_get_entity(id);
    if (e) e->tag_color = color;
}

void jce_state_reparent_entity(uint32_t id, uint32_t new_parent)
{
    HistoryEditScope edit_scope;

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

void jce_state_reorder_sibling(uint32_t entity_id, uint32_t ref_id,
                               bool insert_after)
{
    HistoryEditScope edit_scope;

    JceEntityInfo *e   = jce_state_get_entity(entity_id);
    JceEntityInfo *ref = jce_state_get_entity(ref_id);
    if (!e || !ref) return;
    if (entity_id == ref_id) return;

    /* Both must share the same parent. */
    if (e->parent_id != ref->parent_id) return;

    JceEntityInfo *parent = (e->parent_id != 0)
        ? jce_state_get_entity(e->parent_id) : NULL;

    /* For root-level entities, we don't have a parent children array.
       Rearranging root display order is handled by the flat entity list. */
    if (!parent) return;

    /* Remove entity_id from parent's children array. */
    int from = -1;
    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == entity_id) { from = i; break; }
    }
    if (from < 0) return;

    /* Shift remaining children down to fill gap. */
    for (int i = from; i < parent->child_count - 1; i++)
        parent->children[i] = parent->children[i + 1];
    parent->child_count--;

    /* Find the ref_id position in the (now shorter) array. */
    int ref_pos = -1;
    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == ref_id) { ref_pos = i; break; }
    }
    if (ref_pos < 0) {
        /* ref disappeared; just append back */
        parent->children[parent->child_count++] = entity_id;
        return;
    }

    int insert_pos = insert_after ? ref_pos + 1 : ref_pos;

    /* Shift children up to make room. */
    for (int i = parent->child_count; i > insert_pos; i--)
        parent->children[i] = parent->children[i - 1];
    parent->children[insert_pos] = entity_id;
    parent->child_count++;
}

uint32_t jce_state_duplicate_entity(uint32_t id)
{
    HistoryEditScope edit_scope;

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
        d->prefab_instance = src->prefab_instance;
        snprintf(d->prefab_path, sizeof(d->prefab_path), "%s", src->prefab_path);
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
    HistoryEditScope edit_scope;

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
    HistoryEditScope edit_scope;

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
    case JCE_COMP_MESH_RENDERER:
        c->data.mesh_renderer.base_color[0] = 1.0f;
        c->data.mesh_renderer.base_color[1] = 1.0f;
        c->data.mesh_renderer.base_color[2] = 1.0f;
        c->data.mesh_renderer.base_color[3] = 1.0f;
        c->data.mesh_renderer.roughness     = 0.5f;
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
    HistoryEditScope edit_scope;

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

    bool ok = false;
    {
        HistorySuspendScope suspend;
        ok = load_scene_from_parsed_root(root, scene_path, scene_path);
    }
    if (ok) {
        s_undo_history.clear();
        s_redo_history.clear();
        s_history_edit_nesting = 0;
        s_history_outer_edit_pushed_snapshot = false;
        s_history_manual_batch_depth = 0;
        s_transaction.active = false;
        s_transaction.label[0] = '\0';
        s_transaction.before.scene_json.clear();
        s_transaction.before.scene_path.clear();
    }
    cJSON_Delete(root);
    return ok;
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
        cJSON_AddNumberToObject(obj, "meshShape", comp->data.mesh_renderer.mesh_shape);
        /* PBR parameters. */
        cJSON_AddNumberToObject(obj, "baseColorR", comp->data.mesh_renderer.base_color[0]);
        cJSON_AddNumberToObject(obj, "baseColorG", comp->data.mesh_renderer.base_color[1]);
        cJSON_AddNumberToObject(obj, "baseColorB", comp->data.mesh_renderer.base_color[2]);
        cJSON_AddNumberToObject(obj, "baseColorA", comp->data.mesh_renderer.base_color[3]);
        cJSON_AddNumberToObject(obj, "metallic",   comp->data.mesh_renderer.metallic);
        cJSON_AddNumberToObject(obj, "roughness",  comp->data.mesh_renderer.roughness);
        cJSON_AddNumberToObject(obj, "emissiveR",  comp->data.mesh_renderer.emissive[0]);
        cJSON_AddNumberToObject(obj, "emissiveG",  comp->data.mesh_renderer.emissive[1]);
        cJSON_AddNumberToObject(obj, "emissiveB",  comp->data.mesh_renderer.emissive[2]);
        cJSON_AddNumberToObject(obj, "normalScale", comp->data.mesh_renderer.normal_scale);
        cJSON_AddNumberToObject(obj, "aoStrength",  comp->data.mesh_renderer.ao_strength);
        cJSON_AddNumberToObject(obj, "alphaMode",   comp->data.mesh_renderer.alpha_mode);
        cJSON_AddNumberToObject(obj, "alphaCutoff", comp->data.mesh_renderer.alpha_cutoff);
        cJSON_AddBoolToObject(obj, "doubleSided", comp->data.mesh_renderer.double_sided);
        if (comp->data.mesh_renderer.albedo_tex[0])
            cJSON_AddStringToObject(obj, "albedoTex", comp->data.mesh_renderer.albedo_tex);
        if (comp->data.mesh_renderer.mr_tex[0])
            cJSON_AddStringToObject(obj, "mrTex", comp->data.mesh_renderer.mr_tex);
        if (comp->data.mesh_renderer.normal_tex[0])
            cJSON_AddStringToObject(obj, "normalTex", comp->data.mesh_renderer.normal_tex);
        if (comp->data.mesh_renderer.ao_tex[0])
            cJSON_AddStringToObject(obj, "aoTex", comp->data.mesh_renderer.ao_tex);
        if (comp->data.mesh_renderer.emissive_tex[0])
            cJSON_AddStringToObject(obj, "emissiveTex", comp->data.mesh_renderer.emissive_tex);
        break;
    case JCE_COMP_SPRITE_RENDERER:
        cJSON_AddStringToObject(obj, "spritePath", comp->data.sprite_renderer.sprite_path);
        cJSON_AddNumberToObject(obj, "colorR", comp->data.sprite_renderer.color[0]);
        cJSON_AddNumberToObject(obj, "colorG", comp->data.sprite_renderer.color[1]);
        cJSON_AddNumberToObject(obj, "colorB", comp->data.sprite_renderer.color[2]);
        cJSON_AddNumberToObject(obj, "colorA", comp->data.sprite_renderer.color[3]);
        cJSON_AddBoolToObject(obj, "flipX", comp->data.sprite_renderer.flip_x);
        cJSON_AddBoolToObject(obj, "flipY", comp->data.sprite_renderer.flip_y);
        cJSON_AddNumberToObject(obj, "sortingOrder", comp->data.sprite_renderer.sorting_order);
        break;
    case JCE_COMP_ANIMATOR:
        cJSON_AddStringToObject(obj, "clipName", comp->data.animator.clip_name);
        cJSON_AddNumberToObject(obj, "speed", comp->data.animator.speed);
        cJSON_AddBoolToObject(obj, "loop", comp->data.animator.loop);
        break;
    case JCE_COMP_SKELETAL_ANIMATOR:
        cJSON_AddStringToObject(obj, "skeletonPath", comp->data.skeletal_animator.skeleton_path);
        cJSON_AddNumberToObject(obj, "speed", comp->data.skeletal_animator.speed);
        cJSON_AddBoolToObject(obj, "loop", comp->data.skeletal_animator.loop);
        cJSON_AddNumberToObject(obj, "activeClip", comp->data.skeletal_animator.active_clip);
        if (comp->data.skeletal_animator.clip_count > 0) {
            cJSON *clips = cJSON_CreateArray();
            for (int ci = 0; ci < comp->data.skeletal_animator.clip_count; ci++)
                cJSON_AddItemToArray(clips, cJSON_CreateString(comp->data.skeletal_animator.clip_names[ci]));
            cJSON_AddItemToObject(obj, "clipNames", clips);
        }
        break;
    case JCE_COMP_RIGIDBODY:
        cJSON_AddNumberToObject(obj, "mass", comp->data.rigidbody.mass);
        cJSON_AddNumberToObject(obj, "drag", comp->data.rigidbody.drag);
        cJSON_AddNumberToObject(obj, "angularDrag", comp->data.rigidbody.angular_drag);
        cJSON_AddBoolToObject(obj, "useGravity", comp->data.rigidbody.use_gravity);
        cJSON_AddBoolToObject(obj, "isKinematic", comp->data.rigidbody.is_kinematic);
        break;
    case JCE_COMP_BOX_COLLIDER:
        cJSON_AddNumberToObject(obj, "centerX", comp->data.box_collider.center[0]);
        cJSON_AddNumberToObject(obj, "centerY", comp->data.box_collider.center[1]);
        cJSON_AddNumberToObject(obj, "centerZ", comp->data.box_collider.center[2]);
        cJSON_AddNumberToObject(obj, "sizeX", comp->data.box_collider.size[0]);
        cJSON_AddNumberToObject(obj, "sizeY", comp->data.box_collider.size[1]);
        cJSON_AddNumberToObject(obj, "sizeZ", comp->data.box_collider.size[2]);
        cJSON_AddBoolToObject(obj, "isTrigger", comp->data.box_collider.is_trigger);
        break;
    case JCE_COMP_SPHERE_COLLIDER:
        cJSON_AddNumberToObject(obj, "centerX", comp->data.sphere_collider.center[0]);
        cJSON_AddNumberToObject(obj, "centerY", comp->data.sphere_collider.center[1]);
        cJSON_AddNumberToObject(obj, "centerZ", comp->data.sphere_collider.center[2]);
        cJSON_AddNumberToObject(obj, "radius", comp->data.sphere_collider.radius);
        cJSON_AddBoolToObject(obj, "isTrigger", comp->data.sphere_collider.is_trigger);
        break;
    case JCE_COMP_CHARACTER_CONTROLLER:
        cJSON_AddNumberToObject(obj, "height", comp->data.character_controller.height);
        cJSON_AddNumberToObject(obj, "radius", comp->data.character_controller.radius);
        cJSON_AddNumberToObject(obj, "stepOffset", comp->data.character_controller.step_offset);
        cJSON_AddNumberToObject(obj, "slopeLimit", comp->data.character_controller.slope_limit);
        break;
    case JCE_COMP_AUDIO_SOURCE:
        cJSON_AddStringToObject(obj, "clipPath", comp->data.audio_source.clip_path);
        cJSON_AddNumberToObject(obj, "volume", comp->data.audio_source.volume);
        cJSON_AddNumberToObject(obj, "pitch", comp->data.audio_source.pitch);
        cJSON_AddNumberToObject(obj, "spatialBlend", comp->data.audio_source.spatial_blend);
        cJSON_AddBoolToObject(obj, "loop", comp->data.audio_source.loop);
        cJSON_AddBoolToObject(obj, "playOnAwake", comp->data.audio_source.play_on_awake);
        break;
    case JCE_COMP_SCRIPT:
        cJSON_AddStringToObject(obj, "scriptPath", comp->data.script.script_path);
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

static cJSON *serialize_entity_tree_json(uint32_t entity_id)
{
    int idx = find_entity(entity_id);
    if (idx < 0)
        return NULL;

    JceEntityInfo *e = &s.entities[idx];
    cJSON *node = cJSON_CreateObject();
    if (!node)
        return NULL;

    cJSON_AddStringToObject(node, "name", e->name);
    cJSON_AddBoolToObject(node, "enabled", e->enabled);
    cJSON_AddNumberToObject(node, "tagColor", (double)e->tag_color);
    if (e->tag[0] != '\0')
        cJSON_AddStringToObject(node, "tag", e->tag);
    if (e->prefab_instance) {
        cJSON_AddBoolToObject(node, "prefabInstance", true);
        if (e->prefab_path[0] != '\0')
            cJSON_AddStringToObject(node, "prefabPath", e->prefab_path);
    }

    cJSON *components = cJSON_CreateArray();
    if (!components) {
        cJSON_Delete(node);
        return NULL;
    }
    cJSON_AddItemToObject(node, "components", components);
    for (int i = 0; i < e->component_count; i++) {
        cJSON *comp = serialize_component_json(&s.components[idx][i]);
        if (comp)
            cJSON_AddItemToArray(components, comp);
    }

    cJSON *children = cJSON_CreateArray();
    if (!children) {
        cJSON_Delete(node);
        return NULL;
    }
    cJSON_AddItemToObject(node, "children", children);
    for (int i = 0; i < e->child_count; i++) {
        cJSON *child = serialize_entity_tree_json(e->children[i]);
        if (child)
            cJSON_AddItemToArray(children, child);
    }

    return node;
}

static cJSON *build_prefab_json_root(uint32_t entity_id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *contract = cJSON_CreateObject();
    cJSON *prefab = cJSON_CreateObject();
    cJSON *root_node = serialize_entity_tree_json(entity_id);
    if (!root || !contract || !prefab || !root_node) {
        cJSON_Delete(root);
        cJSON_Delete(contract);
        cJSON_Delete(prefab);
        cJSON_Delete(root_node);
        return NULL;
    }

    cJSON_AddItemToObject(root, JCE_SCENE_CONTRACT_KEY, contract);
    cJSON_AddStringToObject(contract, JCE_SCENE_CONTRACT_NAME_KEY,
                            JCE_SCENE_CONTRACT_NAME);
    cJSON_AddNumberToObject(contract, JCE_SCENE_CONTRACT_MAJOR_KEY,
                            JCE_SCENE_CONTRACT_MAJOR);
    cJSON_AddNumberToObject(contract, JCE_SCENE_CONTRACT_MINOR_KEY,
                            JCE_SCENE_CONTRACT_MINOR);

    cJSON_AddItemToObject(root, "prefab", prefab);
    cJSON_AddNumberToObject(prefab, JCE_SCENE_VERSION_KEY,
                            JCE_SCENE_CONTRACT_MAJOR);
    cJSON_AddItemToObject(prefab, "root", root_node);
    return root;
}

static const cJSON *find_prefab_root_node(const cJSON *root)
{
    if (!root) return NULL;

    if (cJSON_IsObject(root)) {
        const cJSON *prefab = cJSON_GetObjectItemCaseSensitive(root, "prefab");
        if (cJSON_IsObject(prefab)) {
            const cJSON *node = cJSON_GetObjectItemCaseSensitive(prefab, "root");
            if (cJSON_IsObject(node))
                return node;
        }
        if (looks_like_entity_object(root))
            return root;
    }

    return NULL;
}

static void mark_prefab_instance_recursive(uint32_t entity_id, const char *prefab_path)
{
    JceEntityInfo *e = jce_state_get_entity(entity_id);
    if (!e) return;

    if (prefab_path && prefab_path[0] != '\0') {
        e->prefab_instance = true;
        snprintf(e->prefab_path, sizeof(e->prefab_path), "%s", prefab_path);
    } else {
        e->prefab_instance = false;
        e->prefab_path[0] = '\0';
    }

    uint32_t child_ids[JCE_MAX_CHILDREN];
    int child_count = e->child_count;
    if (child_count > JCE_MAX_CHILDREN)
        child_count = JCE_MAX_CHILDREN;
    for (int i = 0; i < child_count; i++)
        child_ids[i] = e->children[i];

    for (int i = 0; i < child_count; i++)
        mark_prefab_instance_recursive(child_ids[i], prefab_path);
}

bool jce_state_save_scene_file(const char *scene_path)
{
    if (!scene_path || scene_path[0] == '\0')
        return false;

    cJSON *root = build_scene_json_root();
    if (!root) {
        LOG_WARN(LOG_TAG, "scene save failed, JSON root creation error: %s", scene_path);
        return false;
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

/* Scene snapshot taken when play starts; restored when stopped. */
static EditorHistorySnapshot s_play_snapshot;
static bool s_play_snapshot_valid = false;

/* Physics world created on play, destroyed on stop. */
static JcePhysicsWorld *s_play_physics = NULL;

/* Maps entity index → physics body handle for rigidbody entities. */
#define PLAY_MAX_BODIES 256
static struct {
    int          entity_index;
    JceBodyHandle body;
} s_play_bodies[PLAY_MAX_BODIES];
static int s_play_body_count = 0;

static void play_create_physics_world(void)
{
    JcePhysicsWorldDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.gravity.x = 0.0f;
    desc.gravity.y = -9.81f;
    desc.gravity.z = 0.0f;
    desc.fixed_timestep = 1.0f / 60.0f;
    s_play_physics = jce_physics_create(&desc);
    s_play_body_count = 0;

    if (!s_play_physics) {
        LOG_ERROR(LOG_TAG, "failed to create physics world for play mode");
        return;
    }

    /* Create physics bodies for entities with rigidbody + transform. */
    for (int i = 0; i < s.entity_count && s_play_body_count < PLAY_MAX_BODIES; i++) {
        JceEntityInfo *e = &s.entities[i];
        if (e->id == 0 || !e->enabled) continue;

        const JceComponentInfo *rb_comp = NULL;
        const JceComponentInfo *tf_comp = NULL;
        int comp_count = 0;
        JceComponentInfo *comps = jce_state_get_entity_components(e->id, &comp_count);
        for (int c = 0; c < comp_count; c++) {
            if (comps[c].type == JCE_COMP_RIGIDBODY) rb_comp = &comps[c];
            if (comps[c].type == JCE_COMP_TRANSFORM) tf_comp = &comps[c];
        }
        if (!rb_comp || !tf_comp) continue;

        JceBodyDesc bd;
        memset(&bd, 0, sizeof(bd));
        bd.position.x = tf_comp->data.transform.pos[0];
        bd.position.y = tf_comp->data.transform.pos[1];
        bd.position.z = tf_comp->data.transform.pos[2];
        bd.rotation   = jce_q_identity();
        bd.mass       = rb_comp->data.rigidbody.mass;
        bd.linear_damping  = rb_comp->data.rigidbody.drag;
        bd.angular_damping = rb_comp->data.rigidbody.angular_drag;
        bd.friction    = 0.5f;
        bd.restitution = 0.0f;
        bd.shape       = JCE_SHAPE_SPHERE;
        bd.half_extents.x = 0.5f;
        bd.type        = rb_comp->data.rigidbody.is_kinematic
                         ? JCE_BODY_KINEMATIC : JCE_BODY_DYNAMIC;

        JceBodyHandle body = jce_physics_body_create(s_play_physics, &bd);
        if (jce_body_valid(body)) {
            s_play_bodies[s_play_body_count].entity_index = i;
            s_play_bodies[s_play_body_count].body = body;
            s_play_body_count++;
        }
    }

    LOG_INFO(LOG_TAG, "play physics: %d bodies created", s_play_body_count);
}

static void play_destroy_physics_world(void)
{
    if (s_play_physics) {
        jce_physics_destroy(s_play_physics);
        s_play_physics = NULL;
    }
    s_play_body_count = 0;
}

static void play_sync_physics_to_entities(void)
{
    if (!s_play_physics) return;

    for (int i = 0; i < s_play_body_count; i++) {
        int eidx = s_play_bodies[i].entity_index;
        if (eidx < 0 || eidx >= s.entity_count) continue;

        jce_vec3 pos;
        jce_quat rot;
        jce_physics_body_get_transform(s_play_physics,
                                       s_play_bodies[i].body, &pos, &rot);

        /* Update the transform component. */
        JceComponentInfo *comps = s.components[eidx];
        for (int c = 0; c < JCE_MAX_COMPONENTS; c++) {
            if (comps[c].type == JCE_COMP_TRANSFORM) {
                comps[c].data.transform.pos[0] = pos.x;
                comps[c].data.transform.pos[1] = pos.y;
                comps[c].data.transform.pos[2] = pos.z;
                break;
            }
        }
    }
}

void jce_state_play(void)
{
    if (s.play_state == JCE_PLAY_STOPPED) {
        /* Capture scene snapshot before entering play mode. */
        s_play_snapshot_valid = history_capture_snapshot(&s_play_snapshot);
        if (!s_play_snapshot_valid)
            LOG_WARN(LOG_TAG, "failed to capture play-mode snapshot");

        play_create_physics_world();
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
        play_destroy_physics_world();

        /* Restore scene to pre-play state. */
        if (s_play_snapshot_valid) {
            history_restore_snapshot(s_play_snapshot, "play-stop-restore");
            s_play_snapshot = EditorHistorySnapshot();
            s_play_snapshot_valid = false;
        }

        s.play_state = JCE_PLAY_STOPPED;
        LOG_INFO(LOG_TAG, "play mode stopped");
    }
}

JcePlayState jce_state_get_play_state(void) { return s.play_state; }

void jce_state_play_mode_tick(float dt)
{
    if (s.play_state != JCE_PLAY_PLAYING) return;

    /* Step physics simulation. */
    if (s_play_physics) {
        jce_physics_step(s_play_physics, dt);
        play_sync_physics_to_entities();
    }
}

/* ── Entity Clipboard ──────────────────────────────────────────────── */

static struct {
    uint32_t    id;
    char        name[JCE_MAX_ENTITY_NAME];
    JceTagColor tag_color;
    char        tag[JCE_MAX_TAG_STRING];
    bool        prefab_instance;
    char        prefab_path[JCE_MAX_PREFAB_PATH];
} s_clipboard;

void jce_state_copy_entity(uint32_t id)
{
    JceEntityInfo *e = jce_state_get_entity(id);
    if (!e) return;
    s_clipboard.id = id;
    snprintf(s_clipboard.name, sizeof(s_clipboard.name), "%s", e->name);
    s_clipboard.tag_color = e->tag_color;
    snprintf(s_clipboard.tag, sizeof(s_clipboard.tag), "%s", e->tag);
    s_clipboard.prefab_instance = e->prefab_instance;
    snprintf(s_clipboard.prefab_path, sizeof(s_clipboard.prefab_path), "%s", e->prefab_path);
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
        e->prefab_instance = s_clipboard.prefab_instance;
        snprintf(e->prefab_path, sizeof(e->prefab_path), "%s", s_clipboard.prefab_path);
    }

    LOG_INFO(LOG_TAG, "pasted entity as %u (%s)", new_id, paste_name);
    return new_id;
}

bool jce_state_has_copied(void)
{
    return s_clipboard.id != 0;
}

/* ── Undo / Redo ──────────────────────────────────────────────────── */

void jce_state_undo(void)
{
    if (s_undo_history.empty()) {
        LOG_INFO(LOG_TAG, "undo: history empty");
        return;
    }

    EditorHistorySnapshot current;
    if (!history_capture_snapshot(&current)) {
        LOG_WARN(LOG_TAG, "undo: failed to capture current state");
        return;
    }

    EditorHistorySnapshot target = s_undo_history.back();
    s_undo_history.pop_back();

    if (!history_restore_snapshot(target, "undo")) {
        s_undo_history.push_back(std::move(target));
        LOG_WARN(LOG_TAG, "undo: restore failed");
        return;
    }

    if ((int)s_redo_history.size() >= JCE_UNDO_HISTORY_LIMIT)
        s_redo_history.erase(s_redo_history.begin());
    s_redo_history.push_back(std::move(current));

    LOG_INFO(LOG_TAG, "undo: applied");
}

void jce_state_redo(void)
{
    if (s_redo_history.empty()) {
        LOG_INFO(LOG_TAG, "redo: history empty");
        return;
    }

    EditorHistorySnapshot current;
    if (!history_capture_snapshot(&current)) {
        LOG_WARN(LOG_TAG, "redo: failed to capture current state");
        return;
    }

    EditorHistorySnapshot target = s_redo_history.back();
    s_redo_history.pop_back();

    if (!history_restore_snapshot(target, "redo")) {
        s_redo_history.push_back(std::move(target));
        LOG_WARN(LOG_TAG, "redo: restore failed");
        return;
    }

    if ((int)s_undo_history.size() >= JCE_UNDO_HISTORY_LIMIT)
        s_undo_history.erase(s_undo_history.begin());
    s_undo_history.push_back(std::move(current));

    LOG_INFO(LOG_TAG, "redo: applied");
}

bool jce_state_can_undo(void)
{
    return !s_undo_history.empty();
}

bool jce_state_can_redo(void)
{
    return !s_redo_history.empty();
}

void jce_state_begin_batch_edit(void)
{
    if (history_begin_edit())
        ++s_history_manual_batch_depth;
}

void jce_state_end_batch_edit(void)
{
    if (s_history_manual_batch_depth <= 0)
        return;

    --s_history_manual_batch_depth;
    history_end_edit(true);
}

bool jce_state_begin_transaction(const char *label)
{
    if (s_transaction.active)
        return false;

    if (!history_capture_snapshot(&s_transaction.before))
        return false;

    if (label && label[0] != '\0')
        snprintf(s_transaction.label, sizeof(s_transaction.label), "%s", label);
    else
        snprintf(s_transaction.label, sizeof(s_transaction.label), "transaction");

    int depth_before = s_history_manual_batch_depth;
    jce_state_begin_batch_edit();
    if (s_history_manual_batch_depth == depth_before) {
        s_transaction.before.scene_json.clear();
        s_transaction.before.scene_path.clear();
        s_transaction.label[0] = '\0';
        return false;
    }

    s_transaction.active = true;
    return true;
}

void jce_state_commit_transaction(void)
{
    if (!s_transaction.active)
        return;

    jce_state_end_batch_edit();
    s_transaction.active = false;
    s_transaction.before.scene_json.clear();
    s_transaction.before.scene_path.clear();
    s_transaction.label[0] = '\0';
}

void jce_state_cancel_transaction(void)
{
    if (!s_transaction.active)
        return;

    history_restore_snapshot(s_transaction.before,
                             s_transaction.label[0] ? s_transaction.label : "transaction");
    jce_state_end_batch_edit();

    s_transaction.active = false;
    s_transaction.before.scene_json.clear();
    s_transaction.before.scene_path.clear();
    s_transaction.label[0] = '\0';
}

bool jce_state_transaction_active(void)
{
    return s_transaction.active;
}

bool jce_state_save_prefab(uint32_t entity_id, const char *prefab_path)
{
    if (!prefab_path || prefab_path[0] == '\0')
        return false;
    if (find_entity(entity_id) < 0)
        return false;

    cJSON *root = build_prefab_json_root(entity_id);
    if (!root)
        return false;

    char *json_text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json_text)
        return false;

    FILE *fp = fopen(prefab_path, "wb");
    if (!fp) {
        free(json_text);
        LOG_WARN(LOG_TAG, "prefab save failed, cannot open file: %s", prefab_path);
        return false;
    }

    size_t len = strlen(json_text);
    size_t wr = fwrite(json_text, 1, len, fp);
    fclose(fp);
    free(json_text);
    if (wr != len) {
        LOG_WARN(LOG_TAG, "prefab save failed, short write: %s", prefab_path);
        return false;
    }

    HistoryEditScope edit_scope;
    mark_prefab_instance_recursive(entity_id, prefab_path);
    LOG_INFO(LOG_TAG, "prefab saved: %s", prefab_path);
    return true;
}

uint32_t jce_state_instantiate_prefab(const char *prefab_path, uint32_t parent_id)
{
    if (!prefab_path || prefab_path[0] == '\0')
        return 0;

    FILE *fp = fopen(prefab_path, "rb");
    if (!fp) {
        LOG_WARN(LOG_TAG, "prefab instantiate failed, cannot open file: %s", prefab_path);
        return 0;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        fclose(fp);
        return 0;
    }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return 0;
    }

    size_t n = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    if (n != (size_t)size) {
        free(buf);
        LOG_WARN(LOG_TAG, "prefab instantiate failed, short read: %s", prefab_path);
        return 0;
    }
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        LOG_WARN(LOG_TAG, "prefab instantiate failed, JSON parse error: %s", prefab_path);
        return 0;
    }

    int contract_major = (int)JCE_SCENE_CONTRACT_MAJOR;
    int contract_minor = (int)JCE_SCENE_CONTRACT_MINOR;
    parse_scene_contract_version(root, &contract_major, &contract_minor);
    (void)contract_minor;
    if (!jce_scene_contract_major_compatible((uint32_t)contract_major)) {
        LOG_WARN(LOG_TAG,
                 "prefab instantiate failed, unsupported contract major %d: %s",
                 contract_major, prefab_path);
        cJSON_Delete(root);
        return 0;
    }

    const cJSON *node = find_prefab_root_node(root);
    if (!node) {
        cJSON_Delete(root);
        LOG_WARN(LOG_TAG, "prefab instantiate failed, missing root node: %s", prefab_path);
        return 0;
    }

    HistoryEditScope edit_scope;
    uint32_t id = load_entity_tree_node(node, parent_id);
    cJSON_Delete(root);

    if (id != 0)
        mark_prefab_instance_recursive(id, prefab_path);

    return id;
}

bool jce_state_revert_prefab(uint32_t entity_id)
{
    JceEntityInfo *e = jce_state_get_entity(entity_id);
    if (!e || !e->prefab_instance || e->prefab_path[0] == '\0')
        return false;

    uint32_t parent_id = e->parent_id;
    bool was_selected = jce_state_is_selected(entity_id);
    char prefab_path[JCE_MAX_PREFAB_PATH];
    snprintf(prefab_path, sizeof(prefab_path), "%s", e->prefab_path);

    HistoryEditScope edit_scope;
    uint32_t new_id = jce_state_instantiate_prefab(prefab_path, parent_id);
    if (new_id == 0)
        return false;

    jce_state_delete_entity(entity_id);

    if (was_selected)
        jce_state_select_entity(new_id, false);
    return true;
}

bool jce_state_is_prefab_instance(uint32_t entity_id)
{
    JceEntityInfo *e = jce_state_get_entity(entity_id);
    return e ? e->prefab_instance : false;
}

const char *jce_state_get_prefab_path(uint32_t entity_id)
{
    JceEntityInfo *e = jce_state_get_entity(entity_id);
    if (!e || !e->prefab_instance || e->prefab_path[0] == '\0')
        return NULL;
    return e->prefab_path;
}
