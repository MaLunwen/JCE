/* jce_bundle_deps.c — see header for design notes. */

#include <jce/resource/jce_bundle_deps.h>
#include <jce/resource/jce_bundle_format.h>
#include <jce/os/core/jce_filesystem.h>

#include "os/core/jce_memory.h"

#include <cjson/cJSON.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* Recognised asset-path JSON keys.                                    */
/* ================================================================== */
/* Kept in sync with jce_scene_components_json.c.                      */

static const char *const kAssetKeys[] = {
    "meshPath",        "mesh_path",        "mesh",
    "materialPath",    "material_path",    "material",
    "texturePath",     "texture_path",
    "audioPath",       "audio_path",       "clipPath",
    "fontPath",        "font_path",
    "spritePath",      "sprite_path",
    "scriptPath",      "script_path",
    "skeletonPath",    "skeleton_path",
    "atlasPath",       "atlas_path",
    "sheetPath",       "sheet_path",
    "terrainPath",     "terrain_path",
    "prefabPath",      "prefab_path",
    "hdrPath",         "hdr_path",
    "animationPath",   "animation_path",
    "layerAlbedoPath0","layerAlbedoPath1",
    "layerAlbedoPath2","layerAlbedoPath3",
    NULL
};

/* "meshPath0", "meshPath1" … pattern recogniser. */
static int is_indexed_mesh_key(const char *k)
{
    if (!k) return 0;
    if (strncmp(k, "meshPath", 8) != 0) return 0;
    const char *p = k + 8;
    if (*p == '\0') return 0;
    while (*p) {
        if (*p < '0' || *p > '9') return 0;
        ++p;
    }
    return 1;
}

int jce_bundle_deps_is_asset_key(const char *key)
{
    if (!key) return 0;
    for (size_t i = 0; kAssetKeys[i]; ++i)
        if (strcmp(key, kAssetKeys[i]) == 0) return 1;
    return is_indexed_mesh_key(key);
}

/* ================================================================== */
/* List management.                                                    */
/* ================================================================== */

static char *dup_str(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *r = (char *)JCE_MALLOC(n);
    if (r) memcpy(r, s, n);
    return r;
}

static void normalise_slashes(char *s)
{
    for (; *s; ++s) if (*s == '\\') *s = '/';
}

static int dep_has_path(const JceBundleDepList *list, const char *path)
{
    for (uint32_t i = 0; i < list->count; ++i)
        if (list->items[i].path && strcmp(list->items[i].path, path) == 0)
            return 1;
    return 0;
}

static int list_grow(JceBundleDepList *list)
{
    uint32_t nc = list->capacity ? list->capacity * 2 : 16;
    JceBundleDep *ni = (JceBundleDep *)JCE_REALLOC(list->items,
                                                    nc * sizeof(JceBundleDep));
    if (!ni) return 0;
    list->items    = ni;
    list->capacity = nc;
    return 1;
}

static void list_push(JceBundleDepList *list, const char *path,
                      const char *bundle_tag)
{
    if (!path || path[0] == '\0') return;
    if (dep_has_path(list, path)) return;
    if (list->count + 1 > list->capacity && !list_grow(list)) return;

    JceBundleDep *d = &list->items[list->count++];
    d->path   = dup_str(path);
    d->bundle = dup_str(bundle_tag); /* may be NULL */
    if (d->path) normalise_slashes(d->path);

    /* Sidecar: a `.terrain.json` always pairs with a `.terrain.bin` that
     * the terrain loader reads via jce_fs_host_read_all.  Recurse once
     * (the recursive call is a leaf — `.bin` is not itself an asset key
     * source) so the bin lands in the bundle alongside its JSON. */
    if (d->path) {
        size_t n = strlen(d->path);
        const char suffix[] = ".terrain.json";
        size_t sn = sizeof(suffix) - 1;
        if (n > sn && strcmp(d->path + n - sn, suffix) == 0) {
            char sidecar[1024];
            if (n - sn + sizeof(".terrain.bin") < sizeof(sidecar)) {
                memcpy(sidecar, d->path, n - sn);
                memcpy(sidecar + (n - sn), ".terrain.bin",
                       sizeof(".terrain.bin"));
                list_push(list, sidecar, bundle_tag);
            }
        }
    }
}

void jce_bundle_deps_free(JceBundleDepList *list)
{
    if (!list) return;
    for (uint32_t i = 0; i < list->count; ++i) {
        if (list->items[i].path)   JCE_FREE(list->items[i].path);
        if (list->items[i].bundle) JCE_FREE(list->items[i].bundle);
    }
    if (list->items) JCE_FREE(list->items);
    list->items    = NULL;
    list->count    = 0;
    list->capacity = 0;
}

/* ================================================================== */
/* Recursive walk.                                                     */
/* ================================================================== */

/* Returns the value of a sibling "bundle" string under `parent_obj`,
 * or NULL.  Used to attach an override tag to discovered assets in the
 * same component object. */
static const char *sibling_bundle_tag(const cJSON *parent_obj)
{
    if (!parent_obj || !cJSON_IsObject(parent_obj)) return NULL;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(parent_obj,
                                                      JCE_BUNDLE_TAG_KEY);
    if (t && cJSON_IsString(t) && t->valuestring && t->valuestring[0])
        return t->valuestring;
    return NULL;
}

static void walk(const cJSON *node, const cJSON *parent_obj,
                 JceBundleDepList *out)
{
    if (!node) return;

    if (cJSON_IsObject(node)) {
        const cJSON *child = NULL;
        cJSON_ArrayForEach(child, node) {
            const char *key = child->string;
            if (key && cJSON_IsString(child) && child->valuestring &&
                child->valuestring[0] &&
                jce_bundle_deps_is_asset_key(key))
            {
                list_push(out, child->valuestring, sibling_bundle_tag(node));
            }
            walk(child, node, out);
        }
    } else if (cJSON_IsArray(node)) {
        const cJSON *child = NULL;
        cJSON_ArrayForEach(child, node) {
            walk(child, parent_obj, out);
        }
    }
}

/* ================================================================== */
/* Public API.                                                         */
/* ================================================================== */

bool jce_bundle_deps_scan(const char *json, size_t json_len,
                          JceBundleDepList *out_list)
{
    if (!json || !out_list) return false;
    if (json_len == 0) json_len = strlen(json);

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root) return false;

    out_list->items    = NULL;
    out_list->count    = 0;
    out_list->capacity = 0;

    walk(root, NULL, out_list);

    cJSON_Delete(root);
    return true;
}

bool jce_bundle_deps_scan_file(const char *scene_path,
                               JceBundleDepList *out_list)
{
    if (!scene_path || !out_list) return false;

    uint64_t sz = 0;
    void *vbuf = jce_fs_host_read_all(scene_path, &sz);
    if (!vbuf || sz == 0) {
        if (vbuf) jce_fs_buffer_free(vbuf);
        return false;
    }

    bool ok = jce_bundle_deps_scan((const char *)vbuf, (size_t)sz, out_list);
    jce_fs_buffer_free(vbuf);
    return ok;
}
