/*
 * jce_scene_serial.c  Scene serialization implementation (cJSON).
 *
 * Converts the ECS scene graph to/from a JSON representation.
 * Format:
 * {
 *   "version": 1,
 *   "entities": [
 *     {
 *       "name": "Player",
 *       "transform": { "position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1] },
 *       "mesh_renderer": { "model": 0, "shader": 0, "visible": true },
 *       "camera": { "fov_deg": 60, "near": 0.1, "far": 1000, "primary": true },
 *       "dir_light": { "direction": [0,-1,0], "color": [1,1,1], "intensity": 1 }
 *     }
 *   ]
 * }
 */

#include <jce/resource/jce_scene_serial.h>
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
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "version", 1);

    cJSON *entities = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "entities", entities);

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

    cJSON *entities = cJSON_GetObjectItem(root, "entities");
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
            mr.model.idx  = (uint16_t)cJSON_GetObjectItem(mc, "model")->valuedouble;
            mr.shader.idx = (uint16_t)cJSON_GetObjectItem(mc, "shader")->valuedouble;
            cJSON *vis = cJSON_GetObjectItem(mc, "visible");
            mr.visible = vis ? cJSON_IsTrue(vis) : true;
            jce_scene_set_mesh_renderer(scene, e, &mr);
        }

        /* Camera. */
        cJSON *cc = cJSON_GetObjectItem(ent, "camera");
        if (cc) {
            JceCameraComponent cam;
            cam.fov_deg    = (float)cJSON_GetObjectItem(cc, "fov_deg")->valuedouble;
            cam.near_plane = (float)cJSON_GetObjectItem(cc, "near")->valuedouble;
            cam.far_plane  = (float)cJSON_GetObjectItem(cc, "far")->valuedouble;
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
            dl.intensity = (float)cJSON_GetObjectItem(lc, "intensity")->valuedouble;
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
