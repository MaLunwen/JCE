/*
 * jce_json.c  Thin facade implementation over cJSON.
 *
 * Public/editor code should talk to this facade instead of cJSON directly.
 * Low-level JSON bridge modules may still include cJSON internally.
 */

#include "jce/os/core/jce_json.h"
#include "jce/os/core/jce_filesystem.h"

#include <cjson/cJSON.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Lifecycle ─────────────────────────────────────────────────────── */

JceJson *jce_json_parse(const char *text, size_t len)
{
    if (!text) return NULL;
    if (len == 0)
        return cJSON_Parse(text);
    return cJSON_ParseWithLength(text, len);
}

JceJson *jce_json_parse_file(const char *path)
{
    if (!path) return NULL;

    /* Prefer the unified host_read_all path so an active VFS override
     * (e.g. editor scene preview from a .jbundle) can intercept reads
     * for project-relative JSON files like terrain/material metadata. */
    {
        uint64_t  sz   = 0;
        void     *vbuf = jce_fs_host_read_all(path, &sz);
        if (vbuf) {
            JceJson *j = cJSON_ParseWithLength((const char *)vbuf, (size_t)sz);
            jce_fs_buffer_free(vbuf);
            if (j) return j;
            /* fall through to host attempt if parse failed (defensive) */
        }
    }

    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) return NULL;

    Sint64 sz = SDL_GetIOSize(io);
    if (sz <= 0) { SDL_CloseIO(io); return NULL; }

    char *buf = (char *)SDL_malloc((size_t)sz + 1);
    if (!buf) { SDL_CloseIO(io); return NULL; }

    size_t nread = SDL_ReadIO(io, buf, (size_t)sz);
    SDL_CloseIO(io);
    if (nread != (size_t)sz) { SDL_free(buf); return NULL; }
    buf[nread] = '\0';

    JceJson *j = cJSON_ParseWithLength(buf, nread);
    SDL_free(buf);
    return j;
}

JceJson *jce_json_object(void) { return cJSON_CreateObject(); }
JceJson *jce_json_array (void) { return cJSON_CreateArray();  }
JceJson *jce_json_number(double v) { return cJSON_CreateNumber(v); }
JceJson *jce_json_string(const char *v) { return cJSON_CreateString(v ? v : ""); }
JceJson *jce_json_bool(bool v) { return cJSON_CreateBool(v); }

char *jce_json_print(const JceJson *root, bool pretty)
{
    if (!root) return NULL;
    return pretty ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
}

bool jce_json_write_file(const char *path, JceJson *root,
                         bool pretty, bool take_ownership)
{
    if (!path || !root) {
        if (take_ownership && root) cJSON_Delete(root);
        return false;
    }

    char *txt = pretty ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
    if (take_ownership) cJSON_Delete(root);
    if (!txt) return false;

    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (!io) { cJSON_free(txt); return false; }

    size_t len = strlen(txt);
    size_t wrote = SDL_WriteIO(io, txt, len);
    SDL_CloseIO(io);
    cJSON_free(txt);
    return wrote == len;
}

void jce_json_free(JceJson *root)        { cJSON_Delete(root); }
void jce_json_free_string(char *s)       { if (s) cJSON_free(s); }

/* ── Type tests / navigation ───────────────────────────────────────── */

bool jce_json_is_object(const JceJson *j) { return cJSON_IsObject(j) ? true : false; }
bool jce_json_is_array (const JceJson *j) { return cJSON_IsArray(j)  ? true : false; }
bool jce_json_is_number(const JceJson *j) { return cJSON_IsNumber(j) ? true : false; }
bool jce_json_is_string(const JceJson *j) { return cJSON_IsString(j) ? true : false; }
bool jce_json_is_bool  (const JceJson *j) { return cJSON_IsBool(j)   ? true : false; }

JceJson *jce_json_get(const JceJson *obj, const char *key)
{
    if (!obj || !key) return NULL;
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

bool jce_json_has(const JceJson *obj, const char *key)
{
    return jce_json_get(obj, key) != NULL;
}

void jce_json_remove(JceJson *obj, const char *key)
{
    if (!obj || !key) return;
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
}

int jce_json_array_size(const JceJson *arr)
{
    if (!cJSON_IsArray(arr)) return 0;
    return cJSON_GetArraySize(arr);
}

JceJson *jce_json_array_at(const JceJson *arr, int index)
{
    if (!cJSON_IsArray(arr)) return NULL;
    return cJSON_GetArrayItem(arr, index);
}

JceJson *jce_json_first_child(const JceJson *obj)
{
    return obj ? obj->child : NULL;
}

JceJson *jce_json_next_sibling(const JceJson *node)
{
    return node ? node->next : NULL;
}

const char *jce_json_member_key(const JceJson *node)
{
    return node ? node->string : NULL;
}

const char *jce_json_string_value(const JceJson *node, const char *def)
{
    if (cJSON_IsString(node) && node->valuestring) return node->valuestring;
    return def;
}

double jce_json_number_value(const JceJson *node, double def)
{
    if (cJSON_IsNumber(node)) return node->valuedouble;
    return def;
}

/* ── Typed accessors with defaults ─────────────────────────────────── */

double jce_json_get_number(const JceJson *obj, const char *key, double def)
{
    const cJSON *it = jce_json_get(obj, key);
    if (cJSON_IsNumber(it)) return it->valuedouble;
    return def;
}

int jce_json_get_int(const JceJson *obj, const char *key, int def)
{
    const cJSON *it = jce_json_get(obj, key);
    if (cJSON_IsNumber(it)) return it->valueint;
    return def;
}

bool jce_json_get_bool(const JceJson *obj, const char *key, bool def)
{
    const cJSON *it = jce_json_get(obj, key);
    if (cJSON_IsBool(it))   return cJSON_IsTrue(it) ? true : false;
    if (cJSON_IsNumber(it)) return it->valuedouble != 0.0;
    return def;
}

const char *jce_json_get_string(const JceJson *obj, const char *key,
                                const char *def)
{
    const cJSON *it = jce_json_get(obj, key);
    if (cJSON_IsString(it) && it->valuestring) return it->valuestring;
    return def;
}

double jce_json_get_number_any(const JceJson *obj,
                               const char *const *keys, int n, double def)
{
    if (!keys) return def;
    for (int i = 0; i < n; i++) {
        const cJSON *it = jce_json_get(obj, keys[i]);
        if (cJSON_IsNumber(it)) return it->valuedouble;
    }
    return def;
}

const char *jce_json_get_string_any(const JceJson *obj,
                                    const char *const *keys, int n,
                                    const char *def)
{
    if (!keys) return def;
    for (int i = 0; i < n; i++) {
        const cJSON *it = jce_json_get(obj, keys[i]);
        if (cJSON_IsString(it) && it->valuestring) return it->valuestring;
    }
    return def;
}

void jce_json_get_floats(const JceJson *obj, const char *key,
                         float *out, int n, const float *def)
{
    if (!out || n <= 0) return;
    const cJSON *arr = jce_json_get(obj, key);
    if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) >= n) {
        for (int i = 0; i < n; i++) {
            const cJSON *e = cJSON_GetArrayItem(arr, i);
            out[i] = cJSON_IsNumber(e) ? (float)e->valuedouble
                                       : (def ? def[i] : 0.0f);
        }
    } else {
        for (int i = 0; i < n; i++) out[i] = def ? def[i] : 0.0f;
    }
}

static void s_get_axis(const JceJson *obj, const char *prefix,
                       const char axis, float *out, float def)
{
    char key[64];
    snprintf(key, sizeof key, "%s%c", prefix ? prefix : "", axis);
    *out = (float)jce_json_get_number(obj, key, def);
}

void jce_json_get_xyz(const JceJson *obj, const char *prefix,
                      float out[3], const float def[3])
{
    if (!out) return;
    s_get_axis(obj, prefix, 'X', &out[0], def ? def[0] : 0.0f);
    s_get_axis(obj, prefix, 'Y', &out[1], def ? def[1] : 0.0f);
    s_get_axis(obj, prefix, 'Z', &out[2], def ? def[2] : 0.0f);
}

void jce_json_get_xyzw(const JceJson *obj, const char *prefix,
                       float out[4], const float def[4])
{
    if (!out) return;
    s_get_axis(obj, prefix, 'X', &out[0], def ? def[0] : 0.0f);
    s_get_axis(obj, prefix, 'Y', &out[1], def ? def[1] : 0.0f);
    s_get_axis(obj, prefix, 'Z', &out[2], def ? def[2] : 0.0f);
    s_get_axis(obj, prefix, 'W', &out[3], def ? def[3] : 0.0f);
}

/* ── Builders ──────────────────────────────────────────────────────── */

void jce_json_set_number(JceJson *obj, const char *key, double v)
{
    if (!obj || !key) return;
    cJSON_AddNumberToObject(obj, key, v);
}

void jce_json_set_int(JceJson *obj, const char *key, int v)
{
    if (!obj || !key) return;
    cJSON_AddNumberToObject(obj, key, (double)v);
}

void jce_json_set_bool(JceJson *obj, const char *key, bool v)
{
    if (!obj || !key) return;
    cJSON_AddBoolToObject(obj, key, v);
}

void jce_json_set_string(JceJson *obj, const char *key, const char *v)
{
    if (!obj || !key) return;
    cJSON_AddStringToObject(obj, key, v ? v : "");
}

void jce_json_set_child(JceJson *obj, const char *key, JceJson *child)
{
    if (!obj || !key || !child) return;
    cJSON_AddItemToObject(obj, key, child);
}

void jce_json_array_push(JceJson *arr, JceJson *item)
{
    if (!arr || !item) return;
    cJSON_AddItemToArray(arr, item);
}

void jce_json_array_push_number(JceJson *arr, double v)
{
    if (!arr) return;
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(v));
}

void jce_json_array_push_string(JceJson *arr, const char *v)
{
    if (!arr) return;
    cJSON_AddItemToArray(arr, cJSON_CreateString(v ? v : ""));
}

static void s_set_axis(JceJson *obj, const char *prefix,
                       char axis, float v)
{
    char key[64];
    snprintf(key, sizeof key, "%s%c", prefix ? prefix : "", axis);
    cJSON_AddNumberToObject(obj, key, v);
}

void jce_json_set_xyz(JceJson *obj, const char *prefix, const float v[3])
{
    if (!obj || !v) return;
    s_set_axis(obj, prefix, 'X', v[0]);
    s_set_axis(obj, prefix, 'Y', v[1]);
    s_set_axis(obj, prefix, 'Z', v[2]);
}

void jce_json_set_xyzw(JceJson *obj, const char *prefix, const float v[4])
{
    if (!obj || !v) return;
    s_set_axis(obj, prefix, 'X', v[0]);
    s_set_axis(obj, prefix, 'Y', v[1]);
    s_set_axis(obj, prefix, 'Z', v[2]);
    s_set_axis(obj, prefix, 'W', v[3]);
}

void jce_json_set_float_array(JceJson *obj, const char *key,
                              const float *v, int n)
{
    if (!obj || !key || !v || n < 0) return;
    cJSON *arr = cJSON_CreateFloatArray(v, n);
    if (!arr) return;
    cJSON_AddItemToObject(obj, key, arr);
}
