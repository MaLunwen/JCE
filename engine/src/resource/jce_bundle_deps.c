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
    /* MeshRenderer per-entity texture overrides (ser_mesh_renderer). */
    "albedoTex",       "mrTex",            "normalTex",
    "aoTex",           "emissiveTex",
    /* Light cookies / IES profiles (ser_light_unified). */
    "cookiePath",      "iesPath",
    /* Physics: Rigidbody / CompoundCollider material + cooked model. */
    "physMaterial",    "modelPath",
    /* ParticleEmitter authored asset (parse accepts both spellings). */
    "assetPath",       "particlePath",
    /* AI / animation / cinematics descriptors. */
    "treePath",        "stateMachine",     "seqPath",
    "avatarPath",      "maskPath",         "overrideController",
    /* 2D tilemaps (ser Tilemap component). */
    "tilemapPath",     "spritesPath",
    /* ReflectionProbe baked cubemap (hdrPath already covers custom). */
    "bakedCubemapPath",
    /* Octahedral impostor terminal LOD (P2 #10): the .impostor.json sidecar.
     * The packer's descriptor recursion re-scans it for its "atlasPath" (a
     * recognised key above) so the baked atlas .png is pulled in too. */
    "impostorMetaPath",
    /* Look-profile colour-grading LUT (rendering.look.lutPath): a strip PNG
     * loaded from the PAK by jce_texture_load_lut_3d in the shipped runtime. */
    "lutPath",         "lut_path",
    /* Terrain/vegetation/water component textures & masks whose spellings are
     * component-specific and not covered by the generic texture keys above:
     *   FoliageCluster.alphaTex, Water.dataTex, VegetationScatter.densityMaskPath */
    "alphaTex",        "dataTex",          "densityMaskPath",
    /* SkeletalAnimator retarget SOURCE rig — a second model/skeleton loaded by
     * path just like skeletonPath, so its GLB + .anim.json clips recurse in. */
    "retargetSource",
    /* Nested-descriptor keys: .mat.json texture maps (primary keys +
     * loader-accepted aliases — see jce_pbr_material_load_json and the
     * editor's try_resolve_texture_from_material_json).  These appear
     * inside material files which the packer re-scans recursively. */
    "albedoMap",       "baseColorMap",     "diffuseMap",
    "mainTexture",     "metallicRoughnessMap", "metallicMap",
    "normalMap",       "aoMap",            "occlusionMap",
    "emissiveMap",     "emissionMap",
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

static int list_push(JceBundleDepList *list, const char *path,
                     const char *bundle_tag)
{
    if (!path || path[0] == '\0') return 1;
    if (dep_has_path(list, path)) return 1;
    if (list->count + 1 > list->capacity && !list_grow(list)) return 0;

    char *path_copy = dup_str(path);
    char *bundle_copy = dup_str(bundle_tag);
    if (!path_copy || (bundle_tag && !bundle_copy)) {
        if (path_copy) JCE_FREE(path_copy);
        if (bundle_copy) JCE_FREE(bundle_copy);
        return 0;
    }
    normalise_slashes(path_copy);

    JceBundleDep *d = &list->items[list->count++];
    d->path   = path_copy;
    d->bundle = bundle_copy;

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
                if (!list_push(list, sidecar, bundle_tag)) return 0;
            }
        }
    }
    return 1;
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

/* World-streaming block: the scene-level
 *   "streaming": { "enabled", …, "chunks": [ {"id","center","radius","path"} ] }
 * object references scene-fragment files (.scene.json) through the
 * generic key "path", which is far too common to add to kAssetKeys.
 * Harvest it contextually instead: whenever a "streaming" object with a
 * "chunks" array is met, every chunks[i].path is a dependency.  The
 * fragments themselves end in .json, so the packer's bounded descriptor
 * recursion (jce_bundle_pack.c) re-scans them for their own assets —
 * including nested streaming blocks.  Harvested regardless of "enabled"
 * so a scene that toggles streaming on at runtime still ships its
 * fragments. */
static void harvest_streaming_chunks(const cJSON *streaming_obj,
                                     JceBundleDepList *out)
{
    const cJSON *chunks =
        cJSON_GetObjectItemCaseSensitive(streaming_obj, "chunks");
    if (!chunks || !cJSON_IsArray(chunks)) return;

    const cJSON *co = NULL;
    cJSON_ArrayForEach(co, chunks) {
        if (!cJSON_IsObject(co)) continue;
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(co, "path");
        if (p && cJSON_IsString(p) && p->valuestring && p->valuestring[0])
            list_push(out, p->valuestring, sibling_bundle_tag(co));
    }
}

/* Contextual nested-descriptor keys.  Some asset descriptors use JSON
 * keys far too generic to harvest globally ("texture", "sprites",
 * "source").  Recognise them structurally — only when the surrounding
 * object also carries the descriptor's signature fields — so a stray
 * "source" string elsewhere in a scene can never be mistaken for an
 * asset path.  Kept in sync with the runtime loaders:
 *   - .particles.json  → jce_particles.c     ("texture" + emitter tuning)
 *   - .tilemap.json    → jce_tilemap.c       ("sprites" + "w"/"h"/"cells")
 *   - .sprites.json    → jce_tilemap.c       ("source"  + "rects")          */
static int obj_has(const cJSON *o, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(o, key) != NULL;
}

static void harvest_descriptor_keys(const cJSON *obj, JceBundleDepList *out)
{
    const cJSON *it;

    /* Particle-emitter descriptor: "texture". */
    it = cJSON_GetObjectItemCaseSensitive(obj, "texture");
    if (it && cJSON_IsString(it) && it->valuestring && it->valuestring[0] &&
        (obj_has(obj, "emitRate") || obj_has(obj, "lifetimeMin") ||
         obj_has(obj, "maxParticles") || obj_has(obj, "sizeStart")))
    {
        list_push(out, it->valuestring, sibling_bundle_tag(obj));
    }

    /* Tilemap descriptor: "sprites" → .sprites.json tileset. */
    it = cJSON_GetObjectItemCaseSensitive(obj, "sprites");
    if (it && cJSON_IsString(it) && it->valuestring && it->valuestring[0] &&
        (obj_has(obj, "cells") || (obj_has(obj, "w") && obj_has(obj, "h"))))
    {
        list_push(out, it->valuestring, sibling_bundle_tag(obj));
    }

    /* Tileset descriptor: "source" → atlas image. */
    it = cJSON_GetObjectItemCaseSensitive(obj, "source");
    if (it && cJSON_IsString(it) && it->valuestring && it->valuestring[0] &&
        obj_has(obj, "rects"))
    {
        list_push(out, it->valuestring, sibling_bundle_tag(obj));
    }
}

static void walk(const cJSON *node, const cJSON *parent_obj,
                 JceBundleDepList *out)
{
    if (!node) return;

    if (cJSON_IsObject(node)) {
        harvest_descriptor_keys(node, out);
        const cJSON *child = NULL;
        cJSON_ArrayForEach(child, node) {
            const char *key = child->string;
            if (key && cJSON_IsString(child) && child->valuestring &&
                child->valuestring[0] &&
                jce_bundle_deps_is_asset_key(key))
            {
                list_push(out, child->valuestring, sibling_bundle_tag(node));
            }
            if (key && cJSON_IsObject(child) &&
                strcmp(key, "streaming") == 0)
            {
                harvest_streaming_chunks(child, out);
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

static bool parse_dep_array(const cJSON *array, JceBundleDepList *out)
{
    if (!array) return true;
    if (!cJSON_IsArray(array)) return false;

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        const char *path = NULL;
        const char *bundle = NULL;
        if (cJSON_IsString(item)) {
            path = item->valuestring;
        } else if (cJSON_IsObject(item)) {
            const cJSON *path_json =
                cJSON_GetObjectItemCaseSensitive(item, "path");
            const cJSON *bundle_json =
                cJSON_GetObjectItemCaseSensitive(item, "bundle");
            if (!path_json || !cJSON_IsString(path_json)) return false;
            path = path_json->valuestring;
            if (bundle_json) {
                if (!cJSON_IsString(bundle_json) ||
                    !bundle_json->valuestring ||
                    !bundle_json->valuestring[0])
                    return false;
                bundle = bundle_json->valuestring;
            }
        } else {
            return false;
        }
        if (!path || !path[0] || !list_push(out, path, bundle)) return false;
    }
    return true;
}

static bool parse_labels(const cJSON *array,
                         JceBundleDependencyDocument *out)
{
    if (!array) return true;
    if (!cJSON_IsArray(array)) return false;

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsString(item) || !item->valuestring ||
            !item->valuestring[0])
            return false;

        bool duplicate = false;
        for (uint32_t i = 0; i < out->label_count; ++i) {
            if (strcmp(out->labels[i], item->valuestring) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        char **grown = (char **)JCE_REALLOC(
            out->labels, (out->label_count + 1u) * sizeof(*grown));
        if (!grown) return false;
        out->labels = grown;
        out->labels[out->label_count] = dup_str(item->valuestring);
        if (!out->labels[out->label_count]) return false;
        ++out->label_count;
    }
    return true;
}

void jce_bundle_deps_document_free(JceBundleDependencyDocument *document)
{
    if (!document) return;
    jce_bundle_deps_free(&document->assets);
    jce_bundle_deps_free(&document->optional_assets);
    for (uint32_t i = 0; i < document->label_count; ++i)
        JCE_FREE(document->labels[i]);
    JCE_FREE(document->labels);
    document->labels = NULL;
    document->label_count = 0;
}

bool jce_bundle_deps_parse_document(
    const char *json, size_t json_len,
    JceBundleDependencyDocument *out_document)
{
    if (!json || !out_document) return false;
    memset(out_document, 0, sizeof(*out_document));
    if (json_len == 0) json_len = strlen(json);

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return false;
    }

    const cJSON *contract =
        cJSON_GetObjectItemCaseSensitive(root, JCE_BUNDLE_KEY_CONTRACT);
    const cJSON *name = contract ? cJSON_GetObjectItemCaseSensitive(
        contract, JCE_BUNDLE_KEY_CONTRACT_NAME) : NULL;
    const cJSON *major = contract ? cJSON_GetObjectItemCaseSensitive(
        contract, JCE_BUNDLE_KEY_CONTRACT_MAJOR) : NULL;
    const cJSON *minor = contract ? cJSON_GetObjectItemCaseSensitive(
        contract, JCE_BUNDLE_KEY_CONTRACT_MINOR) : NULL;

    bool ok = cJSON_IsObject(contract) && cJSON_IsString(name) &&
              name->valuestring &&
              strcmp(name->valuestring, JCE_BUNDLE_DEPS_CONTRACT_NAME) == 0 &&
              cJSON_IsNumber(major) &&
              major->valuedouble == (double)JCE_BUNDLE_DEPS_CONTRACT_MAJOR &&
              cJSON_IsNumber(minor) && minor->valuedouble >= 0.0 &&
              minor->valuedouble == (double)minor->valueint;
    if (ok) {
        ok = parse_dep_array(cJSON_GetObjectItemCaseSensitive(root, "assets"),
                             &out_document->assets) &&
             parse_dep_array(cJSON_GetObjectItemCaseSensitive(
                                 root, "optional_assets"),
                             &out_document->optional_assets) &&
             parse_labels(cJSON_GetObjectItemCaseSensitive(root, "labels"),
                          out_document);
    }

    cJSON_Delete(root);
    if (!ok) jce_bundle_deps_document_free(out_document);
    return ok;
}
