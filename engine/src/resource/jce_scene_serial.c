/*
 * jce_scene_serial.c  Scene serialization implementation (cJSON).
 *
 * Converts the ECS scene graph to/from a JSON representation.
 * Format (contract envelope):
 * {
 *   "contract": { "name": "jce.scene", "major": 1, "minor": 0 },
 *   "scene": {
 *     "version": 1,
 *     "entities": [
 *       {
 *         "name": "Player",
 *         "transform": { "position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1] },
 *         "mesh_renderer": { "model": 0, "shader": 0, "visible": true },
 *         "camera": { "fov_deg": 60, "near": 0.1, "far": 1000, "primary": true },
 *         "dir_light": { "direction": [0,-1,0], "color": [1,1,1], "intensity": 1 }
 *       }
 *     ]
 *   }
 * }
 *
 * Legacy root-level {"version":..., "entities":...} is still accepted on load.
 */

#include <jce/resource/jce_scene_serial.h>
#include <jce/resource/jce_scene_contract.h>
#include "scene/jce_scene.h"
#include <jce/core/jce_log.h>

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "scene_serial"

/* ── JSON helpers ──────────────────────────────────────────────────── */

static cJSON *vec3_to_json(jce_vec3 v)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)v.x));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)v.y));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)v.z));
    return arr;
}

static jce_vec3 json_to_vec3(const cJSON *arr)
{
    jce_vec3 v = {{0, 0, 0}};
    if (!arr || !cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < 3) return v;
    v.x = (float)cJSON_GetArrayItem(arr, 0)->valuedouble;
    v.y = (float)cJSON_GetArrayItem(arr, 1)->valuedouble;
    v.z = (float)cJSON_GetArrayItem(arr, 2)->valuedouble;
    return v;
}

static cJSON *quat_to_json(jce_quat q)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)q.x));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)q.y));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)q.z));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)q.w));
    return arr;
}

static jce_quat json_to_quat(const cJSON *arr)
{
    jce_quat q = GLMS_QUAT_IDENTITY_INIT;
    if (!arr || !cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < 4) return q;
    q.x = (float)cJSON_GetArrayItem(arr, 0)->valuedouble;
    q.y = (float)cJSON_GetArrayItem(arr, 1)->valuedouble;
    q.z = (float)cJSON_GetArrayItem(arr, 2)->valuedouble;
    q.w = (float)cJSON_GetArrayItem(arr, 3)->valuedouble;
    return q;
}

static double json_get_number(const cJSON *parent, const char *key, double fallback)
{
    const cJSON *item = cJSON_GetObjectItem(parent, key);
    if (!item || !cJSON_IsNumber(item)) return fallback;
    return item->valuedouble;
}

static bool read_contract_version(const cJSON *root,
                                  uint32_t *out_major,
                                  uint32_t *out_minor)
{
    if (out_major) *out_major = JCE_SCENE_CONTRACT_MAJOR;
    if (out_minor) *out_minor = JCE_SCENE_CONTRACT_MINOR;
    if (!root || !cJSON_IsObject(root)) return false;

    const cJSON *contract = cJSON_GetObjectItemCaseSensitive(root,
        JCE_SCENE_CONTRACT_KEY);
    if (cJSON_IsObject(contract)) {
        const cJSON *major = cJSON_GetObjectItemCaseSensitive(contract,
            JCE_SCENE_CONTRACT_MAJOR_KEY);
        const cJSON *minor = cJSON_GetObjectItemCaseSensitive(contract,
            JCE_SCENE_CONTRACT_MINOR_KEY);
        if (cJSON_IsNumber(major) && out_major)
            *out_major = (uint32_t)major->valueint;
        if (cJSON_IsNumber(minor) && out_minor)
            *out_minor = (uint32_t)minor->valueint;
        return true;
    }

    const cJSON *legacy_version = cJSON_GetObjectItemCaseSensitive(root,
        JCE_SCENE_VERSION_KEY);
    if (cJSON_IsNumber(legacy_version)) {
        if (out_major)
            *out_major = (uint32_t)legacy_version->valueint;
        if (out_minor)
            *out_minor = 0;
        return true;
    }

    return false;
}

static cJSON *resolve_scene_container(cJSON *root)
{
    if (!root || !cJSON_IsObject(root))
        return root;

    cJSON *scene = cJSON_GetObjectItemCaseSensitive(root, JCE_SCENE_ROOT_KEY);
    return cJSON_IsObject(scene) ? scene : root;
}

/* ── Serialize callback (per entity) ───────────────────────────────── */

typedef struct {
    JceScene *scene;
    cJSON    *entities_array;
} SaveCtx;

static void save_entity_cb(JceScene *s, JceEntity e, void *user_data)
{
    SaveCtx *ctx = (SaveCtx *)user_data;

    cJSON *ent = cJSON_CreateObject();
    if (!ent) return;

    /* Name. */
    const char *name = jce_scene_entity_name(s, e);
    cJSON_AddStringToObject(ent, "name", name ? name : "");

    /* Transform. */
    JceTransform *t = jce_scene_get_transform(s, e);
    if (t) {
        cJSON *tc = cJSON_CreateObject();
        cJSON_AddItemToObject(tc, "position", vec3_to_json(t->position));
        cJSON_AddItemToObject(tc, "rotation", quat_to_json(t->rotation));
        cJSON_AddItemToObject(tc, "scale",    vec3_to_json(t->scale));
        cJSON_AddItemToObject(ent, "transform", tc);
    }

    /* Mesh renderer. */
    JceMeshRenderer *mr = jce_scene_get_mesh_renderer(s, e);
    if (mr) {
        cJSON *mc = cJSON_CreateObject();
        cJSON_AddNumberToObject(mc, "model",   (double)mr->model.idx);
        cJSON_AddNumberToObject(mc, "shader",  (double)mr->shader.idx);
        cJSON_AddBoolToObject(mc, "visible",   mr->visible);
        cJSON_AddItemToObject(ent, "mesh_renderer", mc);
    }

    /* Camera. */
    JceCameraComponent *cam = jce_scene_get_camera(s, e);
    if (cam) {
        cJSON *cc = cJSON_CreateObject();
        cJSON_AddNumberToObject(cc, "fov_deg", (double)cam->fov_deg);
        cJSON_AddNumberToObject(cc, "near",    (double)cam->near_plane);
        cJSON_AddNumberToObject(cc, "far",     (double)cam->far_plane);
        cJSON_AddBoolToObject(cc, "primary",   cam->is_primary);
        cJSON_AddItemToObject(ent, "camera", cc);
    }

    /* Directional light. */
    JceDirectionalLight *dl = jce_scene_get_dir_light(s, e);
    if (dl) {
        cJSON *lc = cJSON_CreateObject();
        cJSON_AddItemToObject(lc, "direction", vec3_to_json(dl->direction));
        cJSON_AddItemToObject(lc, "color",     vec3_to_json(dl->color));
        cJSON_AddNumberToObject(lc, "intensity", (double)dl->intensity);
        cJSON_AddItemToObject(ent, "dir_light", lc);
    }

    cJSON_AddItemToArray(ctx->entities_array, ent);
}

/* ── Save ──────────────────────────────────────────────────────────── */

char *jce_scene_serial_save(const JceScene *scene, size_t *out_len)
{
    if (!scene) return NULL;

    cJSON *root = cJSON_CreateObject();
    cJSON *contract = cJSON_CreateObject();
    cJSON *scene_obj = cJSON_CreateObject();
    cJSON *entities = cJSON_CreateArray();
    if (!root || !contract || !scene_obj || !entities) {
        cJSON_Delete(root);
        cJSON_Delete(contract);
        cJSON_Delete(scene_obj);
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

    cJSON_AddItemToObject(root, JCE_SCENE_ROOT_KEY, scene_obj);
    cJSON_AddNumberToObject(scene_obj, JCE_SCENE_VERSION_KEY,
                            JCE_SCENE_CONTRACT_MAJOR);
    cJSON_AddItemToObject(scene_obj, JCE_SCENE_ENTITIES_KEY, entities);

    SaveCtx ctx = { (JceScene *)scene, entities };
    jce_scene_each_entity((JceScene *)scene, save_entity_cb, &ctx);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (out_len && json)
        *out_len = strlen(json);

    return json;
}

bool jce_scene_serial_save_file(const JceScene *scene, const char *path)
{
    if (!scene || !path) return false;

    size_t len = 0;
    char *json = jce_scene_serial_save(scene, &len);
    if (!json) return false;

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        free(json);
        LOG_ERROR(LOG_TAG, "cannot open '%s' for writing", path);
        return false;
    }

    size_t written = fwrite(json, 1, len, fp);
    fclose(fp);
    free(json);

    if (written != len) {
        LOG_ERROR(LOG_TAG, "write error '%s'", path);
        return false;
    }

    LOG_SUCCESS(LOG_TAG, "scene saved to '%s' (%zu bytes)", path, len);
    return true;
}

/* ── Load ──────────────────────────────────────────────────────────── */

bool jce_scene_serial_load(JceScene *scene, const char *json, size_t len)
{
    if (!scene || !json || len == 0) return false;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        LOG_ERROR(LOG_TAG, "JSON parse error");
        return false;
    }

    uint32_t contract_major = JCE_SCENE_CONTRACT_MAJOR;
    uint32_t contract_minor = JCE_SCENE_CONTRACT_MINOR;
    read_contract_version(root, &contract_major, &contract_minor);
    if (!jce_scene_contract_major_compatible(contract_major)) {
        LOG_ERROR(LOG_TAG,
                  "unsupported scene contract major version: %u (expected %u)",
                  (unsigned)contract_major,
                  (unsigned)JCE_SCENE_CONTRACT_MAJOR);
        cJSON_Delete(root);
        return false;
    }
    (void)contract_minor;

    cJSON *container = resolve_scene_container(root);
    cJSON *entities = cJSON_GetObjectItemCaseSensitive(container,
        JCE_SCENE_ENTITIES_KEY);
    if (!entities || !cJSON_IsArray(entities)) {
        LOG_ERROR(LOG_TAG, "missing 'entities' array");
        cJSON_Delete(root);
        return false;
    }

    int count = cJSON_GetArraySize(entities);
    for (int i = 0; i < count; i++) {
        cJSON *ent = cJSON_GetArrayItem(entities, i);
        if (!ent) continue;

        const cJSON *name_j = cJSON_GetObjectItem(ent, "name");
        const char *name = (name_j && cJSON_IsString(name_j))
                         ? name_j->valuestring : NULL;

        JceEntity e = jce_scene_create_entity(scene, name);
        if (e == JCE_ENTITY_INVALID) continue;

        /* Transform. */
        cJSON *tc = cJSON_GetObjectItem(ent, "transform");
        if (tc) {
            JceTransform t;
            t.position = json_to_vec3(cJSON_GetObjectItem(tc, "position"));
            t.rotation = json_to_quat(cJSON_GetObjectItem(tc, "rotation"));
            t.scale    = json_to_vec3(cJSON_GetObjectItem(tc, "scale"));
            jce_scene_set_transform(scene, e, &t);
        }

        /* Mesh renderer. */
        cJSON *mc = cJSON_GetObjectItem(ent, "mesh_renderer");
        if (mc) {
            JceMeshRenderer mr;
            mr.model.idx  = (uint16_t)json_get_number(mc, "model", UINT16_MAX);
            mr.shader.idx = (uint16_t)json_get_number(mc, "shader", UINT16_MAX);
            cJSON *vis = cJSON_GetObjectItem(mc, "visible");
            mr.visible = vis ? cJSON_IsTrue(vis) : true;
            jce_scene_set_mesh_renderer(scene, e, &mr);
        }

        /* Camera. */
        cJSON *cc = cJSON_GetObjectItem(ent, "camera");
        if (cc) {
            JceCameraComponent cam;
            cam.fov_deg    = (float)json_get_number(cc, "fov_deg", 60.0);
            cam.near_plane = (float)json_get_number(cc, "near", 0.1);
            cam.far_plane  = (float)json_get_number(cc, "far", 1000.0);
            cJSON *pri = cJSON_GetObjectItem(cc, "primary");
            cam.is_primary = pri ? cJSON_IsTrue(pri) : false;
            jce_scene_set_camera(scene, e, &cam);
        }

        /* Directional light. */
        cJSON *lc = cJSON_GetObjectItem(ent, "dir_light");
        if (lc) {
            JceDirectionalLight dl;
            dl.direction = json_to_vec3(cJSON_GetObjectItem(lc, "direction"));
            dl.color     = json_to_vec3(cJSON_GetObjectItem(lc, "color"));
            dl.intensity = (float)json_get_number(lc, "intensity", 1.0);
            jce_scene_set_dir_light(scene, e, &dl);
        }
    }

    cJSON_Delete(root);
    LOG_SUCCESS(LOG_TAG, "scene loaded: %d entities", count);
    return true;
}

bool jce_scene_serial_load_file(JceScene *scene, const char *path)
{
    if (!scene || !path) return false;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LOG_ERROR(LOG_TAG, "cannot open '%s' for reading", path);
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz <= 0) {
        fclose(fp);
        return false;
    }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return false; }

    size_t read_bytes = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[read_bytes] = '\0';

    bool ok = jce_scene_serial_load(scene, buf, read_bytes);
    free(buf);
    return ok;
}

/* ── Memory ────────────────────────────────────────────────────────── */

void jce_scene_serial_free(char *json)
{
    /* cJSON_PrintUnformatted allocates with malloc / cJSON_malloc. */
    if (json) free(json);
}
