/*
 * jce_data_asset.c  Typed JSON asset loader.
 *
 * Storage: process-global type registry + instance pool.  No
 * thread-safety hooks; callers serialise around the editor's main
 * thread.  Reuses jce_json for parsing / writing.
 */

#include <jce/resource/jce_data_asset.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "data_asset"

static JceDataAssetType s_types[JCE_DATA_ASSET_REG_MAX];
static JceDataAsset      s_inst [JCE_DATA_ASSET_INST_MAX];

/* ── Type registry ───────────────────────────────────────────── */

static int find_type_slot(const char *name)
{
    if (!name) return -1;
    int free_slot = -1;
    for (int i = 0; i < JCE_DATA_ASSET_REG_MAX; ++i) {
        if (s_types[i].active &&
            strncmp(s_types[i].type_name, name,
                     JCE_DATA_ASSET_NAME_LEN) == 0) return i;
        if (!s_types[i].active && free_slot < 0) free_slot = i;
    }
    return free_slot;
}

bool jce_data_asset_register_type(const char *name, uint32_t blob_size,
                                    const JceDataAssetField *fields,
                                    uint32_t field_count)
{
    if (!name || blob_size == 0 || field_count > JCE_DATA_ASSET_FIELDS_MAX)
        return false;
    int slot = find_type_slot(name);
    if (slot < 0) return false;
    JceDataAssetType *t = &s_types[slot];
    memset(t, 0, sizeof(*t));
    strncpy(t->type_name, name, JCE_DATA_ASSET_NAME_LEN - 1);
    t->blob_size   = blob_size;
    t->field_count = field_count;
    for (uint32_t i = 0; i < field_count; ++i) {
        t->fields[i] = fields[i];
        if (t->fields[i].offset + t->fields[i].field_size > blob_size) {
            LOG_WARN(LOG_TAG, "%s field %s out of bounds (%u + %u > %u)",
                      name, fields[i].name, fields[i].offset,
                      fields[i].field_size, blob_size);
            return false;
        }
    }
    t->active = true;
    return true;
}

const JceDataAssetType *jce_data_asset_find_type(const char *name)
{
    int s = find_type_slot(name);
    if (s < 0 || !s_types[s].active) return NULL;
    if (strncmp(s_types[s].type_name, name,
                 JCE_DATA_ASSET_NAME_LEN) != 0) return NULL;
    return &s_types[s];
}

/* ── Instance loading ────────────────────────────────────────── */

static int find_free_inst(void)
{
    for (int i = 0; i < JCE_DATA_ASSET_INST_MAX; ++i)
        if (!s_inst[i].active) return i;
    return -1;
}

static int find_inst_by_path(const char *path)
{
    if (!path) return -1;
    for (int i = 0; i < JCE_DATA_ASSET_INST_MAX; ++i)
        if (s_inst[i].active &&
            strncmp(s_inst[i].path, path, JCE_DATA_ASSET_PATH_LEN) == 0)
            return i;
    return -1;
}

static void parse_field(const JceJson *src, const JceDataAssetField *f,
                          void *blob)
{
    uint8_t *base = (uint8_t *)blob + f->offset;
    switch (f->kind) {
    case JCE_DA_FIELD_BOOL:
        *(bool *)base = jce_json_get_bool(src, f->name, false);
        break;
    case JCE_DA_FIELD_INT:
        *(int *)base = jce_json_get_int(src, f->name, 0);
        break;
    case JCE_DA_FIELD_FLOAT:
        *(float *)base = (float)jce_json_get_number(src, f->name, 0.0);
        break;
    case JCE_DA_FIELD_STRING: {
        const char *v = jce_json_get_string(src, f->name, "");
        size_t cap = f->field_size > 0 ? f->field_size : 1;
        strncpy((char *)base, v, cap - 1);
        ((char *)base)[cap - 1] = '\0';
        break;
    }
    case JCE_DA_FIELD_VEC3: {
        static const float def3[3] = { 0.0f, 0.0f, 0.0f };
        jce_json_get_floats(src, f->name, (float *)base, 3, def3);
        break;
    }
    case JCE_DA_FIELD_VEC4: {
        static const float def4[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        jce_json_get_floats(src, f->name, (float *)base, 4, def4);
        break;
    }
    case JCE_DA_FIELD_GUID: {
        const char *hex = jce_json_get_string(src, f->name, "");
        memset(base, 0, 16);
        for (int i = 0; i < 16 && hex[i*2] && hex[i*2+1]; ++i) {
            char buf[3] = { hex[i*2], hex[i*2+1], 0 };
            base[i] = (uint8_t)strtoul(buf, NULL, 16);
        }
        break;
    }
    }
}

JceDataAsset *jce_data_asset_load(const char *path)
{
    if (!path) return NULL;
    int existing = find_inst_by_path(path);
    if (existing >= 0) return &s_inst[existing];
    JceJson *root = jce_json_parse_file(path);
    if (!root) return NULL;
    const char *type_name = jce_json_get_string(root, "type", "");
    const JceDataAssetType *t = jce_data_asset_find_type(type_name);
    if (!t) {
        LOG_WARN(LOG_TAG, "unknown type '%s' in %s", type_name, path);
        jce_json_free(root);
        return NULL;
    }
    int slot = find_free_inst();
    if (slot < 0) { jce_json_free(root); return NULL; }
    JceDataAsset *a = &s_inst[slot];
    memset(a, 0, sizeof(*a));
    strncpy(a->path, path, JCE_DATA_ASSET_PATH_LEN - 1);
    strncpy(a->type_name, type_name, JCE_DATA_ASSET_NAME_LEN - 1);
    a->blob_size = t->blob_size;
    a->blob = calloc(1, t->blob_size);
    if (!a->blob) { jce_json_free(root); return NULL; }

    /* GUID at root.  Stored 32-hex-char form. */
    const char *guid_hex = jce_json_get_string(root, "guid", "");
    for (int i = 0; i < 16 && guid_hex[i*2] && guid_hex[i*2+1]; ++i) {
        char buf[3] = { guid_hex[i*2], guid_hex[i*2+1], 0 };
        a->guid[i] = (uint8_t)strtoul(buf, NULL, 16);
    }

    const JceJson *data = jce_json_get(root, "data");
    if (!data) data = root; /* tolerate flat layout */
    for (uint32_t i = 0; i < t->field_count; ++i)
        parse_field(data, &t->fields[i], a->blob);
    a->active = true;
    jce_json_free(root);
    return a;
}

static void emit_field(JceJson *dst, const JceDataAssetField *f,
                        const void *blob)
{
    const uint8_t *base = (const uint8_t *)blob + f->offset;
    switch (f->kind) {
    case JCE_DA_FIELD_BOOL:
        jce_json_set_number(dst, f->name, *(const bool *)base ? 1.0 : 0.0);
        break;
    case JCE_DA_FIELD_INT:
        jce_json_set_number(dst, f->name, (double)*(const int *)base);
        break;
    case JCE_DA_FIELD_FLOAT:
        jce_json_set_number(dst, f->name, (double)*(const float *)base);
        break;
    case JCE_DA_FIELD_STRING:
        jce_json_set_string(dst, f->name, (const char *)base);
        break;
    case JCE_DA_FIELD_VEC3: {
        const float *v = (const float *)base;
        char key[64];
        for (int i = 0; i < 3; ++i) {
            const char *suf = (i == 0) ? "x" : (i == 1) ? "y" : "z";
            snprintf(key, sizeof(key), "%s_%s", f->name, suf);
            jce_json_set_number(dst, key, (double)v[i]);
        }
        break;
    }
    case JCE_DA_FIELD_VEC4: {
        const float *v = (const float *)base;
        const char *sufs[4] = { "x", "y", "z", "w" };
        char key[64];
        for (int i = 0; i < 4; ++i) {
            snprintf(key, sizeof(key), "%s_%s", f->name, sufs[i]);
            jce_json_set_number(dst, key, (double)v[i]);
        }
        break;
    }
    case JCE_DA_FIELD_GUID: {
        char hex[33];
        for (int i = 0; i < 16; ++i)
            snprintf(&hex[i*2], 3, "%02x", base[i]);
        hex[32] = '\0';
        jce_json_set_string(dst, f->name, hex);
        break;
    }
    }
}

bool jce_data_asset_save(const JceDataAsset *a)
{
    if (!a || !a->active) return false;
    const JceDataAssetType *t = jce_data_asset_find_type(a->type_name);
    if (!t) return false;
    JceJson *root = jce_json_object();
    if (!root) return false;
    jce_json_set_string(root, "type", a->type_name);
    char guid_hex[33];
    for (int i = 0; i < 16; ++i)
        snprintf(&guid_hex[i*2], 3, "%02x", a->guid[i]);
    guid_hex[32] = '\0';
    jce_json_set_string(root, "guid", guid_hex);
    for (uint32_t i = 0; i < t->field_count; ++i)
        emit_field(root, &t->fields[i], a->blob);
    return jce_json_write_file(a->path, root, true, true);
}

void jce_data_asset_release(JceDataAsset *a)
{
    if (!a || !a->active) return;
    free(a->blob);
    memset(a, 0, sizeof(*a));
}

JceDataAsset *jce_data_asset_find_by_path(const char *path)
{
    int s = find_inst_by_path(path);
    return s >= 0 ? &s_inst[s] : NULL;
}

JceDataAsset *jce_data_asset_find_by_guid(const uint8_t guid[16])
{
    if (!guid) return NULL;
    for (int i = 0; i < JCE_DATA_ASSET_INST_MAX; ++i) {
        if (!s_inst[i].active) continue;
        if (memcmp(s_inst[i].guid, guid, 16) == 0) return &s_inst[i];
    }
    return NULL;
}

uint32_t jce_data_asset_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < JCE_DATA_ASSET_INST_MAX; ++i)
        if (s_inst[i].active) n++;
    return n;
}

JceDataAsset *jce_data_asset_at(uint32_t idx)
{
    uint32_t seen = 0;
    for (int i = 0; i < JCE_DATA_ASSET_INST_MAX; ++i) {
        if (!s_inst[i].active) continue;
        if (seen == idx) return &s_inst[i];
        seen++;
    }
    return NULL;
}
