#include "jce_bundle_contract_internal.h"

#include <jce/jce_version.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_bundle_format.h>

#include "jce_asset_reader.h"
#include "os/core/jce_memory.h"

#include <cjson/cJSON.h>
#include <xxhash.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#define JCE_BUNDLE_CONTRACT_PATH_CAP 2048u
#define JCE_BUILD_REPORT_SCHEMA "jce.buildreport.v1"

static bool ends_with_ci(const char *value, const char *suffix)
{
    if (!value || !suffix)
        return false;
    size_t value_size = strlen(value);
    size_t suffix_size = strlen(suffix);
    if (value_size < suffix_size)
        return false;
    value += value_size - suffix_size;
    for (size_t i = 0; i < suffix_size; ++i) {
        char a = value[i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

static bool is_scene_path(const char *path)
{
    return ends_with_ci(path, ".scene") ||
           ends_with_ci(path, ".scene.json");
}

CookClass jce_bundle_classify_cook(const char *path)
{
    if (!path)
        return COOK_CLASS_NONE;
    if (ends_with_ci(path, ".png") || ends_with_ci(path, ".jpg") ||
        ends_with_ci(path, ".jpeg") || ends_with_ci(path, ".tga") ||
        ends_with_ci(path, ".bmp"))
        return COOK_CLASS_TEXTURE;
    if (ends_with_ci(path, ".obj") || ends_with_ci(path, ".fbx") ||
        ends_with_ci(path, ".dae") || ends_with_ci(path, ".gltf") ||
        ends_with_ci(path, ".glb"))
        return COOK_CLASS_MODEL;
    if (ends_with_ci(path, ".wav") || ends_with_ci(path, ".ogg") ||
        ends_with_ci(path, ".flac") || ends_with_ci(path, ".opus") ||
        ends_with_ci(path, ".mp3"))
        return COOK_CLASS_AUDIO;
    return COOK_CLASS_NONE;
}

static char *contract_strdup(const char *value)
{
    if (!value)
        value = "";
    size_t size = strlen(value) + 1;
    char *copy = (char *)JCE_MALLOC(size);
    if (copy)
        memcpy(copy, value, size);
    return copy;
}

static char *json_print_owned(const cJSON *root, size_t *out_len)
{
    char *temporary = cJSON_Print(root);
    if (!temporary) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    size_t size = strlen(temporary);
    char *result = (char *)JCE_MALLOC(size + 1);
    if (result)
        memcpy(result, temporary, size + 1);
    cJSON_free(temporary);
    if (out_len) *out_len = result ? size : 0;
    return result;
}

static void hex16(char out[17], uint64_t value)
{
    snprintf(out, 17, "%016llx", (unsigned long long)value);
}

static cJSON *build_contract(const char *name, uint32_t major, uint32_t minor)
{
    cJSON *contract = cJSON_CreateObject();
    cJSON_AddStringToObject(contract, JCE_BUNDLE_KEY_CONTRACT_NAME, name);
    cJSON_AddNumberToObject(contract, JCE_BUNDLE_KEY_CONTRACT_MAJOR, major);
    cJSON_AddNumberToObject(contract, JCE_BUNDLE_KEY_CONTRACT_MINOR, minor);
    return contract;
}

static const char *target_profile(bool cooked, int platform)
{
    static const char *const names[] = {
        "windows", "linux", "macos", "android", "ios", "web", "auto"
    };
    if (!cooked || platform < 0 ||
        platform >= (int)(sizeof(names) / sizeof(names[0])))
        return "auto";
    return names[platform];
}

static int cooked_asset_type(const PakEntry *entry)
{
    JceAssetView view;
    if (!entry || !jce_asset_is_cooked(entry->raw, entry->raw_size) ||
        !jce_asset_open(&view, entry->raw, entry->raw_size))
        return -1;
    return (int)view.header->asset_type;
}

static const char *asset_type_name(const PakEntry *entry)
{
    switch (cooked_asset_type(entry)) {
    case JCEASSET_TYPE_TEXTURE:   return "texture";
    case JCEASSET_TYPE_MESH:      return "mesh";
    case JCEASSET_TYPE_SOUND:     return "sound";
    case JCEASSET_TYPE_FONT:      return "font";
    case JCEASSET_TYPE_SHADER:    return "shader";
    case JCEASSET_TYPE_MATERIAL:  return "material";
    case JCEASSET_TYPE_MODEL:     return "model";
    case JCEASSET_TYPE_ANIMATION: return "animation";
    case JCEASSET_TYPE_SCENE:     return "scene";
    case JCEASSET_TYPE_RAW:       return "binary";
    default: break;
    }

    CookClass type = jce_bundle_classify_cook(entry->vpath);
    if (type == COOK_CLASS_TEXTURE) return "texture";
    if (type == COOK_CLASS_MODEL) return "model";
    if (type == COOK_CLASS_AUDIO) return "sound";
    if (is_scene_path(entry->vpath)) return "scene";
    if (ends_with_ci(entry->vpath, ".lua")) return "script";
    if (ends_with_ci(entry->vpath, ".mat") ||
        ends_with_ci(entry->vpath, ".mat.json")) return "material";
    if (ends_with_ci(entry->vpath, ".json")) return "descriptor";
    if (ends_with_ci(entry->vpath, ".ttf") ||
        ends_with_ci(entry->vpath, ".otf")) return "font";
    return "binary";
}

static const char *asset_representation(const PakEntry *entry)
{
    switch (cooked_asset_type(entry)) {
    case JCEASSET_TYPE_TEXTURE: return "jcea.texture.v1";
    case JCEASSET_TYPE_SOUND: return "jcea.audio-pcm.v1";
    case JCEASSET_TYPE_MESH: return "jcea.mesh.v1";
    case JCEASSET_TYPE_MODEL: return "jcea.model.v1";
    case JCEASSET_TYPE_FONT: return "jcea.font.v1";
    case JCEASSET_TYPE_SHADER: return "jcea.shader.v1";
    case JCEASSET_TYPE_MATERIAL: return "jcea.material.v1";
    case JCEASSET_TYPE_ANIMATION: return "jcea.animation.v1";
    case JCEASSET_TYPE_SCENE: return "jcea.scene.v1";
    case JCEASSET_TYPE_RAW: return "jcea.binary.v1";
    default: break;
    }
    if (entry->raw_size >= 4 && memcmp(entry->raw, "glTF", 4) == 0)
        return "gltf.glb.v2";

    CookClass type = jce_bundle_classify_cook(entry->vpath);
    if (type == COOK_CLASS_TEXTURE) return "image.encoded";
    if (type == COOK_CLASS_AUDIO) return "audio.encoded";
    if (type == COOK_CLASS_MODEL) return "model.source";
    if (is_scene_path(entry->vpath) || ends_with_ci(entry->vpath, ".json"))
        return "json.utf8";
    if (ends_with_ci(entry->vpath, ".lua")) return "lua.source";
    if (ends_with_ci(entry->vpath, ".ttf") ||
        ends_with_ci(entry->vpath, ".otf")) return "font.source";
    return "binary.raw";
}

ReportEntry *jce_bundle_report_entry_create(ReportEntryVec *entries)
{
    if (entries->n == entries->c) {
        size_t cap = entries->c ? entries->c * 2 : 16;
        ReportEntry *items = (ReportEntry *)JCE_REALLOC(
            entries->items, cap * sizeof(*items));
        if (!items)
            return NULL;
        entries->items = items;
        entries->c = cap;
    }
    ReportEntry *entry = &entries->items[entries->n++];
    memset(entry, 0, sizeof(*entry));
    return entry;
}

CatalogEntry *jce_bundle_catalog_entry_create(CatalogVec *catalog)
{
    if (catalog->n == catalog->c) {
        size_t cap = catalog->c ? catalog->c * 2 : 16;
        CatalogEntry *items = (CatalogEntry *)JCE_REALLOC(
            catalog->items, cap * sizeof(*items));
        if (!items)
            return NULL;
        catalog->items = items;
        catalog->c = cap;
    }
    CatalogEntry *entry = &catalog->items[catalog->n++];
    memset(entry, 0, sizeof(*entry));
    return entry;
}

void jce_bundle_catalog_entries_free(CatalogVec *catalog)
{
    for (size_t i = 0; i < catalog->n; ++i) {
        CatalogEntry *entry = &catalog->items[i];
        JCE_FREE(entry->id);
        JCE_FREE(entry->file);
        JCE_FREE(entry->kind);
        JCE_FREE(entry->scene_path);
        JCE_FREE(entry->content_hash);
        JCE_FREE(entry->build_hash);
        for (size_t j = 0; j < entry->deps.n; ++j)
            JCE_FREE(entry->deps.items[j]);
        JCE_FREE(entry->deps.items);
        for (size_t j = 0; j < entry->entries.n; ++j) {
            JCE_FREE(entry->entries.items[j].path);
            JCE_FREE(entry->entries.items[j].hash);
        }
        JCE_FREE(entry->entries.items);
    }
    JCE_FREE(catalog->items);
    catalog->items = NULL;
    catalog->n = catalog->c = 0;
}

char *jce_bundle_build_manifest(const Bundle *bundle,
                                const PakEntry *entries,
                                size_t entry_count,
                                uint64_t build_hash,
                                uint32_t version,
                                bool encrypted,
                                bool cooked,
                                int target_platform,
                                const JceBundleDependencyEdges *edges,
                                size_t *out_len)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, JCE_BUNDLE_KEY_CONTRACT,
        build_contract(JCE_BUNDLE_MANIFEST_CONTRACT_NAME,
                       JCE_BUNDLE_MANIFEST_CONTRACT_MAJOR,
                       JCE_BUNDLE_MANIFEST_CONTRACT_MINOR));
    cJSON_AddStringToObject(root, JCE_BUNDLE_KEY_ID, bundle->id);
    cJSON_AddNumberToObject(root, JCE_BUNDLE_KEY_VERSION, version);
    char hash[17];
    hex16(hash, build_hash);
    cJSON_AddStringToObject(root, JCE_BUNDLE_KEY_BUILD_HASH, hash);
    cJSON_AddStringToObject(root, JCE_BUNDLE_KEY_TARGET_PROFILE,
                            target_profile(cooked, target_platform));
    cJSON_AddStringToObject(root, JCE_BUNDLE_KEY_CONTENT_ABI,
                            JCE_BUNDLE_CONTENT_ABI);
    cJSON_AddNumberToObject(root, JCE_BUNDLE_KEY_COOK_VERSION,
                            cooked ? JCE_BUNDLE_COOK_VERSION : 0u);
    cJSON_AddStringToObject(root, JCE_BUNDLE_KEY_MIN_ENGINE_VERSION,
                            JCE_VERSION_STR);
    cJSON_AddStringToObject(root, JCE_BUNDLE_KIND_KEY, bundle->kind);
    cJSON_AddBoolToObject(root, JCE_BUNDLE_KEY_ENCRYPTED, encrypted);
    if (bundle->scene_path)
        cJSON_AddStringToObject(root, JCE_BUNDLE_KEY_SCENE_PATH,
                                bundle->scene_path);

    cJSON *dependencies = cJSON_AddArrayToObject(
        root, JCE_BUNDLE_KEY_DEPENDS_ON);
    for (size_t i = 0; i < bundle->deps.n; ++i) {
        cJSON_AddItemToArray(dependencies,
                             cJSON_CreateString(bundle->deps.items[i]));
    }

    cJSON *assets = cJSON_AddArrayToObject(root, JCE_BUNDLE_KEY_ASSETS);
    for (size_t i = 0; i < entry_count; ++i) {
        const PakEntry *entry = &entries[i];
        char canonical[JCE_BUNDLE_CONTRACT_PATH_CAP];
        size_t canonical_size = jce_archive_normalize_path(
            entry->vpath, canonical, sizeof(canonical));
        if (canonical_size == 0) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON *asset = cJSON_CreateObject();
        cJSON_AddStringToObject(asset, JCE_BUNDLE_KEY_ASSET_PATH, canonical);
        cJSON_AddNumberToObject(asset, JCE_BUNDLE_KEY_ASSET_SIZE,
                                (double)entry->raw_size);
        char asset_id[17];
        char content_id[17];
        hex16(asset_id, jce_archive_hash_normalized(canonical,
                                                    canonical_size));
        hex16(content_id, entry->content_hash);
        cJSON_AddStringToObject(asset, JCE_BUNDLE_KEY_ASSET_ID, asset_id);
        cJSON_AddStringToObject(asset, JCE_BUNDLE_KEY_CONTENT_ID, content_id);
        cJSON_AddStringToObject(asset, JCE_BUNDLE_KEY_ASSET_HASH, content_id);
        cJSON_AddStringToObject(asset, JCE_BUNDLE_KEY_ASSET_TYPE,
                                asset_type_name(entry));
        cJSON_AddStringToObject(asset, JCE_BUNDLE_KEY_REPRESENTATION,
                                asset_representation(entry));
        jce_bundle_edges_add_json(asset, canonical, edges);
        cJSON_AddItemToArray(assets, asset);
    }
    char *json = json_print_owned(root, out_len);
    cJSON_Delete(root);
    return json;
}

char *jce_bundle_build_sidecar(const char *manifest, size_t manifest_size,
                               uint64_t content_hash,
                               uint64_t archive_size,
                               size_t *out_len)
{
    cJSON *root = cJSON_ParseWithLength(manifest, manifest_size);
    if (!root)
        return NULL;
    char hash[17];
    hex16(hash, content_hash);
    cJSON_AddStringToObject(root, JCE_BUNDLE_KEY_CONTENT_HASH, hash);
    cJSON_AddNumberToObject(root, JCE_BUNDLE_KEY_ARCHIVE_SIZE,
                            (double)archive_size);
    char *json = json_print_owned(root, out_len);
    cJSON_Delete(root);
    return json;
}

char *jce_bundle_build_catalog_json(const CatalogVec *catalog,
                                    uint32_t version,
                                    bool cooked,
                                    int target_platform,
                                    size_t *out_len)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, JCE_BUNDLE_KEY_CONTRACT,
        build_contract(JCE_BUNDLE_CATALOG_CONTRACT_NAME,
                       JCE_BUNDLE_CATALOG_CONTRACT_MAJOR,
                       JCE_BUNDLE_CATALOG_CONTRACT_MINOR));
    cJSON_AddNumberToObject(root, JCE_BUNDLE_CATALOG_KEY_VERSION, version);
    cJSON_AddStringToObject(root, JCE_BUNDLE_KEY_TARGET_PROFILE,
                            target_profile(cooked, target_platform));
    cJSON_AddStringToObject(root, JCE_BUNDLE_KEY_CONTENT_ABI,
                            JCE_BUNDLE_CONTENT_ABI);
    cJSON_AddNumberToObject(root, JCE_BUNDLE_KEY_COOK_VERSION,
                            cooked ? JCE_BUNDLE_COOK_VERSION : 0u);
    cJSON_AddStringToObject(root, JCE_BUNDLE_KEY_MIN_ENGINE_VERSION,
                            JCE_VERSION_STR);
    cJSON *bundles = cJSON_AddObjectToObject(
        root, JCE_BUNDLE_CATALOG_KEY_BUNDLES);
    for (size_t i = 0; i < catalog->n; ++i) {
        const CatalogEntry *entry = &catalog->items[i];
        cJSON *record = cJSON_AddObjectToObject(bundles, entry->id);
        cJSON_AddStringToObject(record, JCE_BUNDLE_CATALOG_KEY_FILE,
                                entry->file);
        cJSON_AddStringToObject(record, JCE_BUNDLE_CATALOG_KEY_KIND,
                                entry->kind);
        if (entry->scene_path) {
            cJSON_AddStringToObject(record, JCE_BUNDLE_CATALOG_KEY_SCENE,
                                    entry->scene_path);
        }
        cJSON_AddStringToObject(record, JCE_BUNDLE_CATALOG_KEY_HASH,
                                entry->content_hash);
        cJSON_AddStringToObject(record, JCE_BUNDLE_CATALOG_KEY_BUILD_HASH,
                                entry->build_hash);
        cJSON_AddNumberToObject(record, JCE_BUNDLE_CATALOG_KEY_SIZE,
                                (double)entry->size);
        cJSON *dependencies = cJSON_AddArrayToObject(
            record, JCE_BUNDLE_CATALOG_KEY_DEPS);
        for (size_t j = 0; j < entry->deps.n; ++j) {
            cJSON_AddItemToArray(dependencies,
                                 cJSON_CreateString(entry->deps.items[j]));
        }
    }
    char *json = json_print_owned(root, out_len);
    cJSON_Delete(root);
    return json;
}

static ReportEntry *find_report_entry(ReportEntryVec *entries,
                                      const char *path)
{
    for (size_t i = 0; i < entries->n; ++i) {
        if (entries->items[i].path &&
            strcmp(entries->items[i].path, path) == 0)
            return &entries->items[i];
    }
    return NULL;
}

static void free_report_entries(ReportEntryVec *entries)
{
    for (size_t i = 0; i < entries->n; ++i) {
        JCE_FREE(entries->items[i].path);
        JCE_FREE(entries->items[i].hash);
    }
    JCE_FREE(entries->items);
    entries->items = NULL;
    entries->n = entries->c = 0;
}

char *jce_bundle_build_graph_json(const CatalogVec *catalog,
                                  bool cooked,
                                  int target_platform,
                                  const JceBundleDependencyEdges *edges,
                                  size_t *out_len)
{
    ReportEntryVec graph = {0};
    for (size_t i = 0; i < catalog->n; ++i) {
        const ReportEntryVec *source_entries = &catalog->items[i].entries;
        for (size_t j = 0; j < source_entries->n; ++j) {
            const ReportEntry *source = &source_entries->items[j];
            if (!source->path || !source->hash)
                continue;
            ReportEntry *existing = find_report_entry(&graph, source->path);
            if (existing) {
                if (strcmp(existing->hash, source->hash) != 0) {
                    free_report_entries(&graph);
                    return NULL;
                }
                continue;
            }
            ReportEntry *entry = jce_bundle_report_entry_create(&graph);
            if (!entry) {
                free_report_entries(&graph);
                return NULL;
            }
            entry->path = contract_strdup(source->path);
            entry->hash = contract_strdup(source->hash);
            entry->size = source->size;
            if (!entry->path || !entry->hash) {
                free_report_entries(&graph);
                return NULL;
            }
        }
    }

    for (size_t i = 1; i < graph.n; ++i) {
        for (size_t j = i; j > 0 &&
             strcmp(graph.items[j - 1].path, graph.items[j].path) > 0; --j) {
            ReportEntry swap = graph.items[j];
            graph.items[j] = graph.items[j - 1];
            graph.items[j - 1] = swap;
        }
    }

    XXH3_state_t *state = XXH3_createState();
    if (!state) {
        free_report_entries(&graph);
        return NULL;
    }
    XXH3_64bits_reset(state);
    for (size_t i = 0; i < graph.n; ++i) {
        XXH3_64bits_update(state, graph.items[i].path,
                           strlen(graph.items[i].path));
        XXH3_64bits_update(state, graph.items[i].hash,
                           strlen(graph.items[i].hash));
        XXH3_64bits_update(state, &graph.items[i].size,
                           sizeof(graph.items[i].size));
    }
    if (edges) {
        for (size_t i = 0; i < edges->n; ++i) {
            XXH3_64bits_update(state, edges->items[i].from,
                               strlen(edges->items[i].from));
            XXH3_64bits_update(state, edges->items[i].to,
                               strlen(edges->items[i].to));
            XXH3_64bits_update(state, edges->items[i].origin,
                               strlen(edges->items[i].origin));
        }
    }
    uint64_t graph_hash = XXH3_64bits_digest(state);
    XXH3_freeState(state);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, JCE_BUNDLE_KEY_CONTRACT,
        build_contract(JCE_BUNDLE_GRAPH_CONTRACT_NAME,
                       JCE_BUNDLE_GRAPH_CONTRACT_MAJOR,
                       JCE_BUNDLE_GRAPH_CONTRACT_MINOR));
    char graph_id[17];
    hex16(graph_id, graph_hash);
    cJSON_AddStringToObject(root, JCE_BUNDLE_KEY_GRAPH_ID, graph_id);
    cJSON_AddStringToObject(root, JCE_BUNDLE_KEY_TARGET_PROFILE,
                            target_profile(cooked, target_platform));
    cJSON_AddStringToObject(root, JCE_BUNDLE_KEY_CONTENT_ABI,
                            JCE_BUNDLE_CONTENT_ABI);
    cJSON_AddNumberToObject(root, JCE_BUNDLE_KEY_COOK_VERSION,
                            cooked ? JCE_BUNDLE_COOK_VERSION : 0u);
    cJSON_AddStringToObject(root, JCE_BUNDLE_KEY_MIN_ENGINE_VERSION,
                            JCE_VERSION_STR);
    cJSON *assets = cJSON_AddArrayToObject(root, JCE_BUNDLE_KEY_ASSETS);
    for (size_t i = 0; i < graph.n; ++i) {
        const ReportEntry *entry = &graph.items[i];
        char canonical[JCE_BUNDLE_CONTRACT_PATH_CAP];
        size_t canonical_size = jce_archive_normalize_path(
            entry->path, canonical, sizeof(canonical));
        if (canonical_size == 0) {
            cJSON_Delete(root);
            free_report_entries(&graph);
            return NULL;
        }
        cJSON *asset = cJSON_CreateObject();
        cJSON_AddStringToObject(asset, JCE_BUNDLE_KEY_ASSET_ADDRESS,
                                canonical);
        char asset_id[17];
        hex16(asset_id, jce_archive_hash_normalized(canonical,
                                                    canonical_size));
        cJSON_AddStringToObject(asset, JCE_BUNDLE_KEY_ASSET_ID, asset_id);
        cJSON_AddStringToObject(asset, JCE_BUNDLE_KEY_CONTENT_ID,
                                entry->hash);
        cJSON_AddNumberToObject(asset, JCE_BUNDLE_KEY_ASSET_SIZE,
                                (double)entry->size);
        jce_bundle_edges_add_json(asset, canonical, edges);
        cJSON_AddItemToArray(assets, asset);
    }
    char *json = json_print_owned(root, out_len);
    cJSON_Delete(root);
    free_report_entries(&graph);
    return json;
}

static const char *previous_string(const cJSON *catalog, const char *id,
                                   const char *key)
{
    if (!catalog)
        return NULL;
    const cJSON *bundles = cJSON_GetObjectItemCaseSensitive(
        catalog, JCE_BUNDLE_CATALOG_KEY_BUNDLES);
    const cJSON *entry = bundles
        ? cJSON_GetObjectItemCaseSensitive(bundles, id) : NULL;
    const cJSON *value = entry
        ? cJSON_GetObjectItemCaseSensitive(entry, key) : NULL;
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

const char *jce_bundle_previous_content_hash(const cJSON *catalog,
                                             const char *id)
{
    return previous_string(catalog, id, JCE_BUNDLE_CATALOG_KEY_HASH);
}

const char *jce_bundle_previous_build_hash(const cJSON *catalog,
                                           const char *id)
{
    return previous_string(catalog, id, JCE_BUNDLE_CATALOG_KEY_BUILD_HASH);
}

const char *jce_bundle_previous_file(const cJSON *catalog, const char *id)
{
    return previous_string(catalog, id, JCE_BUNDLE_CATALOG_KEY_FILE);
}

static const char *report_type(const char *path, char fallback[16])
{
    const char *dot = NULL;
    for (const char *cursor = path; *cursor; ++cursor) {
        if (*cursor == '.')
            dot = cursor;
    }
    if (!dot)
        return "other";
    char extension[16];
    size_t size = 0;
    for (const char *cursor = dot + 1;
         *cursor && size + 1 < sizeof(extension); ++cursor) {
        char value = *cursor;
        if (value >= 'A' && value <= 'Z')
            value = (char)(value - 'A' + 'a');
        extension[size++] = value;
    }
    extension[size] = '\0';
    if (is_scene_path(path)) return "scene";
    if (strcmp(extension, "gltf") == 0 || strcmp(extension, "glb") == 0 ||
        strcmp(extension, "fbx") == 0 || strcmp(extension, "obj") == 0 ||
        strcmp(extension, "mesh") == 0) return "mesh";
    if (strcmp(extension, "png") == 0 || strcmp(extension, "jpg") == 0 ||
        strcmp(extension, "jpeg") == 0 || strcmp(extension, "tga") == 0 ||
        strcmp(extension, "bmp") == 0 || strcmp(extension, "hdr") == 0 ||
        strcmp(extension, "ktx") == 0 || strcmp(extension, "ktx2") == 0 ||
        strcmp(extension, "basis") == 0 || strcmp(extension, "dds") == 0 ||
        strcmp(extension, "webp") == 0) return "texture";
    if (strcmp(extension, "wav") == 0 || strcmp(extension, "ogg") == 0 ||
        strcmp(extension, "mp3") == 0 || strcmp(extension, "opus") == 0 ||
        strcmp(extension, "flac") == 0) return "audio";
    if (strcmp(extension, "mp4") == 0 || strcmp(extension, "webm") == 0 ||
        strcmp(extension, "mkv") == 0 || strcmp(extension, "ivf") == 0)
        return "video";
    if (strcmp(extension, "ttf") == 0 || strcmp(extension, "otf") == 0)
        return "font";
    if (strcmp(extension, "bin") == 0 || strcmp(extension, "sc") == 0)
        return "shader";
    if (strcmp(extension, "json") == 0) return "json";
    if (!extension[0])
        return "other";
    memcpy(fallback, extension, sizeof(extension));
    return fallback;
}

char *jce_bundle_build_report_json(const CatalogVec *catalog,
                                   size_t *out_len)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "$schema", JCE_BUILD_REPORT_SCHEMA);
    char timestamp[32];
    time_t now = time(NULL);
    struct tm utc_value;
    struct tm *utc = gmtime(&now);
    if (utc) utc_value = *utc;
    else memset(&utc_value, 0, sizeof(utc_value));
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc_value);
    cJSON_AddStringToObject(root, "timestamp", timestamp);

    uint64_t total_bytes = 0;
    size_t unique_assets = 0;
    cJSON *hash_map = cJSON_CreateObject();
    cJSON *bundles = cJSON_AddArrayToObject(root, "bundles");
    for (size_t i = 0; i < catalog->n; ++i) {
        const CatalogEntry *entry = &catalog->items[i];
        cJSON *bundle = cJSON_CreateObject();
        cJSON_AddStringToObject(bundle, "name", entry->id);
        cJSON_AddStringToObject(bundle, "file", entry->file);
        cJSON_AddNumberToObject(bundle, "size_bytes", (double)entry->size);
        cJSON_AddNumberToObject(bundle, "entry_count",
                                (double)entry->entries.n);
        cJSON_AddBoolToObject(bundle, "encrypted", entry->encrypted);
        total_bytes += entry->size;

        cJSON *dependencies = cJSON_AddArrayToObject(bundle, "dependencies");
        for (size_t j = 0; j < entry->deps.n; ++j) {
            cJSON_AddItemToArray(dependencies,
                                 cJSON_CreateString(entry->deps.items[j]));
        }
        cJSON *records = cJSON_AddArrayToObject(bundle, "entries");
        for (size_t j = 0; j < entry->entries.n; ++j) {
            const ReportEntry *source = &entry->entries.items[j];
            cJSON *record = cJSON_CreateObject();
            cJSON_AddStringToObject(record, "path",
                                    source->path ? source->path : "");
            cJSON_AddNumberToObject(record, "size_bytes",
                                    (double)source->size);
            cJSON_AddStringToObject(record, "hash",
                                    source->hash ? source->hash : "");
            char type_fallback[16];
            const char *type = report_type(
                source->path ? source->path : "", type_fallback);
            cJSON_AddStringToObject(record, "type", type);
            cJSON_AddBoolToObject(record, "encrypted", entry->encrypted);
            cJSON_AddItemToArray(records, record);

            if (source->hash && source->hash[0]) {
                cJSON *slot = cJSON_GetObjectItemCaseSensitive(
                    hash_map, source->hash);
                if (!slot) {
                    slot = cJSON_CreateObject();
                    cJSON_AddNumberToObject(slot, "size_bytes",
                                            (double)source->size);
                    cJSON_AddArrayToObject(slot, "in_bundles");
                    cJSON_AddItemToObject(hash_map, source->hash, slot);
                    ++unique_assets;
                }
                cJSON *owners = cJSON_GetObjectItemCaseSensitive(
                    slot, "in_bundles");
                cJSON_AddItemToArray(owners, cJSON_CreateString(entry->id));
            }
        }
        cJSON_AddItemToArray(bundles, bundle);
    }

    cJSON *duplicates = cJSON_AddArrayToObject(root, "duplicates");
    cJSON *slot = NULL;
    cJSON_ArrayForEach(slot, hash_map) {
        const cJSON *owners = cJSON_GetObjectItemCaseSensitive(
            slot, "in_bundles");
        if (!cJSON_IsArray(owners) || cJSON_GetArraySize(owners) < 2)
            continue;
        cJSON *duplicate = cJSON_CreateObject();
        cJSON_AddStringToObject(duplicate, "hash", slot->string);
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(
            slot, "size_bytes");
        cJSON_AddNumberToObject(duplicate, "size_bytes",
                                size ? size->valuedouble : 0.0);
        cJSON *duplicate_owners = cJSON_AddArrayToObject(
            duplicate, "in_bundles");
        const cJSON *owner = NULL;
        cJSON_ArrayForEach(owner, owners)
            cJSON_AddItemToArray(duplicate_owners, cJSON_Duplicate(owner, 1));
        cJSON_AddItemToArray(duplicates, duplicate);
    }
    cJSON_Delete(hash_map);

    cJSON *totals = cJSON_AddObjectToObject(root, "totals");
    cJSON_AddNumberToObject(totals, "bundle_count", (double)catalog->n);
    cJSON_AddNumberToObject(totals, "total_size_bytes", (double)total_bytes);
    cJSON_AddNumberToObject(totals, "unique_asset_count",
                            (double)unique_assets);

    char *json = json_print_owned(root, out_len);
    cJSON_Delete(root);
    return json;
}
