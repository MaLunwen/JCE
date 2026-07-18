/* jce_bundle_loader.c
 *
 * Implementation of the runtime scene-asset-bundle loader.  See
 * jce_bundle_loader.h for the public contract.
 */

#include <jce/resource/jce_bundle_loader.h>
#include <jce/jce_version.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_bundle_format.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/platform/jce_mmap.h>

#include "os/core/jce_memory.h"

#include <cjson/cJSON.h>
#include <xxhash.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* Internal data model                                                  */
/* ================================================================== */

typedef struct BundleEntry {
    char           *id;
    char           *file;          /* basename, e.g. "scene_forest.a1b2c3d4.jbundle" */
    char           *kind;          /* "scene" | "shared" | "patch"                  */
    char           *scene_path;    /* nullable                                       */
    char           *content_hash;  /* nullable                                       */
    char           *build_hash;    /* source-graph identity                           */
    uint64_t        size;
    char          **deps;          /* malloc'd array of malloc'd ids                 */
    uint32_t        dep_count;

    JcePakArchive  *pak;           /* non-null while mounted                         */
    uint32_t        refcount;
} BundleEntry;

struct JceBundleCatalog {
    JceFileSystem *fs;
    char          *base_dir;       /* directory containing the catalog & bundles     */
    uint32_t       version;
    uint32_t       contract_minor;
    bool           verify_content;
    BundleEntry   *entries;
    uint32_t       entry_count;
};

static bool catalog_bundle_manifest_valid(const JcePakArchive *pak,
                                          const BundleEntry *entry,
                                          const char *source_label);

/* ================================================================== */
/* Helpers                                                              */
/* ================================================================== */

static char *dup_str(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *r = (char *)JCE_MALLOC(n);
    if (!r) return NULL;
    memcpy(r, s, n);
    return r;
}

static BundleEntry *find_entry(const JceBundleCatalog *cat, const char *id) {
    if (!cat || !id) return NULL;
    for (uint32_t i = 0; i < cat->entry_count; ++i)
        if (strcmp(cat->entries[i].id, id) == 0) return &cat->entries[i];
    return NULL;
}

static char *read_text(const char *path, size_t *out_size) {
    uint64_t sz = 0;
    void *vbuf = jce_fs_host_read_all(path, &sz);
    if (!vbuf) return NULL;
    char *buf = (char *)JCE_MALLOC((size_t)sz + 1);
    if (!buf) { jce_fs_buffer_free(vbuf); return NULL; }
    if (sz) memcpy(buf, vbuf, (size_t)sz);
    jce_fs_buffer_free(vbuf);
    buf[sz] = '\0';
    if (out_size) *out_size = (size_t)sz;
    return buf;
}

static char *dirname_dup(const char *path) {
    size_t len = strlen(path);
    while (len > 0 && path[len - 1] != '/' && path[len - 1] != '\\') --len;
    if (len == 0) return dup_str(".");
    char *r = (char *)JCE_MALLOC(len + 1);
    if (!r) return NULL;
    memcpy(r, path, len);
    /* strip trailing separator */
    while (len > 1 && (r[len - 1] == '/' || r[len - 1] == '\\')) --len;
    r[len] = '\0';
    return r;
}

static bool is_hex64(const char *text)
{
    if (!text || strlen(text) != 16) return false;
    for (size_t i = 0; i < 16; ++i) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

static bool parse_hex64(const char *text, uint64_t *out)
{
    if (!is_hex64(text) || !out) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < 16; ++i) {
        char c = text[i];
        uint64_t digit = (c >= '0' && c <= '9') ? (uint64_t)(c - '0')
            : (c >= 'a' && c <= 'f') ? (uint64_t)(c - 'a' + 10)
            : (uint64_t)(c - 'A' + 10);
        value = (value << 4) | digit;
    }
    *out = value;
    return true;
}

static bool valid_id(const char *id)
{
    if (!id || !id[0]) return false;
    for (const char *p = id; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (!(c == '_' || c == '-' || c == '.' ||
              (c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z')))
            return false;
    }
    return true;
}

static bool valid_bundle_file(const char *path)
{
    if (!path || !path[0] || path[0] == '/' || path[0] == '\\')
        return false;
    if (((path[0] >= 'A' && path[0] <= 'Z') ||
         (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':')
        return false;

    const char *segment = path;
    for (const char *p = path;; ++p) {
        if (*p == '\\' || *p == ':' ||
            (*p != '\0' && (unsigned char)*p < 0x20))
            return false;
        if (*p == '/' || *p == '\0') {
            size_t len = (size_t)(p - segment);
            if (len == 0 || (len == 1 && segment[0] == '.') ||
                (len == 2 && segment[0] == '.' && segment[1] == '.'))
                return false;
            if (*p == '\0') break;
            segment = p + 1;
        }
    }
    return true;
}

static bool valid_kind(const char *kind)
{
    return kind &&
        (strcmp(kind, JCE_BUNDLE_KIND_SCENE) == 0 ||
         strcmp(kind, JCE_BUNDLE_KIND_SHARED) == 0 ||
         strcmp(kind, JCE_BUNDLE_KIND_PATCH) == 0);
}

typedef struct BundleSemver {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} BundleSemver;

static bool parse_semver_triplet(const char *text, BundleSemver *out)
{
    if (!text || !out) return false;
    uint32_t values[3] = {0, 0, 0};
    const char *p = text;
    for (uint32_t part = 0; part < 3; ++part) {
        if (*p < '0' || *p > '9') return false;
        uint64_t value = 0;
        do {
            value = value * 10u + (uint64_t)(*p - '0');
            if (value > UINT32_MAX) return false;
            ++p;
        } while (*p >= '0' && *p <= '9');
        values[part] = (uint32_t)value;
        if (part < 2) {
            if (*p != '.') return false;
            ++p;
        }
    }
    if (*p != '\0') return false;
    out->major = values[0];
    out->minor = values[1];
    out->patch = values[2];
    return true;
}

static bool minimum_engine_compatible(const char *minimum)
{
    BundleSemver required;
    if (!parse_semver_triplet(minimum, &required)) return false;
    if (JCE_VERSION_MAJOR != required.major)
        return JCE_VERSION_MAJOR > required.major;
    if (JCE_VERSION_MINOR != required.minor)
        return JCE_VERSION_MINOR > required.minor;
    return JCE_VERSION_PATCH >= required.patch;
}

static const char *runtime_target_profile(void)
{
#if JCE_PLATFORM_WEB
    return "web";
#elif JCE_PLATFORM_ANDROID
    return "android";
#elif JCE_PLATFORM_IOS
    return "ios";
#elif JCE_PLATFORM_MACOS
    return "macos";
#elif JCE_PLATFORM_LINUX
    return "linux";
#elif JCE_PLATFORM_WINDOWS
    return "windows";
#else
    return "unknown";
#endif
}

static bool content_contract_compatible(const cJSON *root,
                                        uint32_t contract_minor,
                                        const char *source_label)
{
    if (contract_minor < 1u) return true;

    const cJSON *target = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_TARGET_PROFILE);
    const cJSON *abi = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_CONTENT_ABI);
    const cJSON *cook = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_COOK_VERSION);
    const cJSON *minimum = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_MIN_ENGINE_VERSION);
    if (!cJSON_IsString(target) || !target->valuestring ||
        !cJSON_IsString(abi) || !abi->valuestring ||
        !cJSON_IsNumber(cook) || cook->valuedouble < 0.0 ||
        cook->valuedouble != (double)cook->valueint ||
        !cJSON_IsString(minimum) || !minimum->valuestring) {
        LOG_ERROR("jce_bundle", "content contract metadata missing in %s",
                  source_label ? source_label : "(memory)");
        return false;
    }
    if (strcmp(abi->valuestring, JCE_BUNDLE_CONTENT_ABI) != 0 ||
        (uint32_t)cook->valueint > JCE_BUNDLE_COOK_VERSION) {
        LOG_ERROR("jce_bundle", "incompatible content ABI/cook version in %s",
                  source_label ? source_label : "(memory)");
        return false;
    }
    const char *runtime_target = runtime_target_profile();
    if (strcmp(target->valuestring, "auto") != 0 &&
        strcmp(target->valuestring, runtime_target) != 0) {
        LOG_ERROR("jce_bundle", "target '%s' is incompatible with runtime '%s' in %s",
                  target->valuestring, runtime_target,
                  source_label ? source_label : "(memory)");
        return false;
    }
    if (!minimum_engine_compatible(minimum->valuestring)) {
        LOG_ERROR("jce_bundle", "bundle requires engine %s (runtime %s): %s",
                  minimum->valuestring, JCE_VERSION_STR,
                  source_label ? source_label : "(memory)");
        return false;
    }
    return true;
}

static bool validate_dependency_graph(const JceBundleCatalog *cat)
{
    uint32_t n = cat->entry_count;
    uint32_t *remaining = (uint32_t *)JCE_CALLOC(n ? n : 1,
                                                 sizeof(*remaining));
    uint8_t *removed = (uint8_t *)JCE_CALLOC(n ? n : 1, 1);
    if (!remaining || !removed) {
        JCE_FREE(remaining);
        JCE_FREE(removed);
        return false;
    }

    for (uint32_t i = 0; i < n; ++i) {
        const BundleEntry *entry = &cat->entries[i];
        remaining[i] = entry->dep_count;
        for (uint32_t d = 0; d < entry->dep_count; ++d) {
            if (!find_entry(cat, entry->deps[d])) {
                LOG_ERROR("jce_bundle", "bundle '%s' references unknown dep '%s'",
                          entry->id, entry->deps[d]);
                JCE_FREE(remaining);
                JCE_FREE(removed);
                return false;
            }
            for (uint32_t prior = 0; prior < d; ++prior) {
                if (strcmp(entry->deps[prior], entry->deps[d]) == 0) {
                    LOG_ERROR("jce_bundle", "bundle '%s' repeats dep '%s'",
                              entry->id, entry->deps[d]);
                    JCE_FREE(remaining);
                    JCE_FREE(removed);
                    return false;
                }
            }
        }
    }

    uint32_t processed = 0;
    while (processed < n) {
        uint32_t leaf = n;
        for (uint32_t i = 0; i < n; ++i) {
            if (!removed[i] && remaining[i] == 0) {
                leaf = i;
                break;
            }
        }
        if (leaf == n) {
            LOG_ERROR("jce_bundle", "%s", "catalog dependency cycle detected");
            JCE_FREE(remaining);
            JCE_FREE(removed);
            return false;
        }
        removed[leaf] = 1;
        ++processed;
        for (uint32_t i = 0; i < n; ++i) {
            if (removed[i]) continue;
            for (uint32_t d = 0; d < cat->entries[i].dep_count; ++d) {
                if (strcmp(cat->entries[i].deps[d],
                           cat->entries[leaf].id) == 0) {
                    --remaining[i];
                    break;
                }
            }
        }
    }

    JCE_FREE(remaining);
    JCE_FREE(removed);
    return true;
}

/* ================================================================== */
/* Catalog open / close                                                 */
/* ================================================================== */

JCE_API JceBundleCatalog *jce_bundle_catalog_open(JceFileSystem *fs,
                                                  const char *catalog_path) {
    if (!catalog_path) return NULL;

    size_t txt_len = 0;
    char *txt = read_text(catalog_path, &txt_len);
    if (!txt) {
        LOG_ERROR("jce_bundle", "cannot read catalog '%s'", catalog_path);
        return NULL;
    }

    cJSON *root = cJSON_ParseWithLength(txt, txt_len);
    JCE_FREE(txt);
    if (!root || !cJSON_IsObject(root)) {
        LOG_ERROR("jce_bundle", "catalog JSON parse failed: %s",
                      catalog_path);
        if (root) cJSON_Delete(root);
        return NULL;
    }

    /* Contract metadata is mandatory.  A missing wrapper used to turn random
     * JSON into an empty, apparently valid catalog. */
    const cJSON *contract = cJSON_GetObjectItemCaseSensitive(root,
                                JCE_BUNDLE_KEY_CONTRACT);
    const cJSON *name = contract ? cJSON_GetObjectItemCaseSensitive(
        contract, JCE_BUNDLE_KEY_CONTRACT_NAME) : NULL;
    const cJSON *major = contract ? cJSON_GetObjectItemCaseSensitive(
        contract, JCE_BUNDLE_KEY_CONTRACT_MAJOR) : NULL;
    const cJSON *minor = contract ? cJSON_GetObjectItemCaseSensitive(
        contract, JCE_BUNDLE_KEY_CONTRACT_MINOR) : NULL;
    if (!cJSON_IsObject(contract) || !cJSON_IsString(name) ||
        !name->valuestring ||
        strcmp(name->valuestring, JCE_BUNDLE_CATALOG_CONTRACT_NAME) != 0 ||
        !cJSON_IsNumber(major) ||
        major->valuedouble != (double)JCE_BUNDLE_CATALOG_CONTRACT_MAJOR ||
        !cJSON_IsNumber(minor) || minor->valuedouble < 0.0 ||
        minor->valuedouble != (double)minor->valueint) {
        LOG_ERROR("jce_bundle", "invalid catalog contract: %s", catalog_path);
        cJSON_Delete(root);
        return NULL;
    }

    const cJSON *vj = cJSON_GetObjectItemCaseSensitive(root,
                            JCE_BUNDLE_CATALOG_KEY_VERSION);
    const cJSON *bundles = cJSON_GetObjectItemCaseSensitive(root,
                                JCE_BUNDLE_CATALOG_KEY_BUNDLES);
    if (!cJSON_IsNumber(vj) || vj->valuedouble < 1.0 ||
        vj->valuedouble != (double)vj->valueint ||
        !cJSON_IsObject(bundles)) {
        LOG_ERROR("jce_bundle", "catalog is missing version/bundles: %s",
                  catalog_path);
        cJSON_Delete(root);
        return NULL;
    }

    JceBundleCatalog *cat = (JceBundleCatalog *)JCE_CALLOC(1, sizeof(*cat));
    if (!cat) { cJSON_Delete(root); return NULL; }
    cat->fs       = fs;
    cat->base_dir = dirname_dup(catalog_path);
    cat->version = (uint32_t)vj->valueint;
    cat->contract_minor = (uint32_t)minor->valueint;
    cat->verify_content = cat->contract_minor >= 1u;
    if (!cat->base_dir) goto invalid_catalog;
    if (!content_contract_compatible(root, cat->contract_minor,
                                     catalog_path))
        goto invalid_catalog;

    uint32_t n = (uint32_t)cJSON_GetArraySize(bundles);
    cat->entries = (BundleEntry *)JCE_CALLOC(n ? n : 1,
                                             sizeof(BundleEntry));
    if (!cat->entries) goto invalid_catalog;

    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, bundles) {
        if (!cJSON_IsObject(e) || !valid_id(e->string) ||
            find_entry(cat, e->string)) {
            LOG_ERROR("jce_bundle", "invalid/duplicate bundle id in %s",
                      catalog_path);
            goto invalid_catalog;
        }

        BundleEntry *be = &cat->entries[cat->entry_count++];
        be->id = dup_str(e->string);
        const cJSON *jf = cJSON_GetObjectItemCaseSensitive(e,
                                JCE_BUNDLE_CATALOG_KEY_FILE);
        const cJSON *jk = cJSON_GetObjectItemCaseSensitive(e,
                                JCE_BUNDLE_CATALOG_KEY_KIND);
        const cJSON *js = cJSON_GetObjectItemCaseSensitive(e,
                                JCE_BUNDLE_CATALOG_KEY_SCENE);
        const cJSON *jh = cJSON_GetObjectItemCaseSensitive(e,
                                JCE_BUNDLE_CATALOG_KEY_HASH);
        const cJSON *jb = cJSON_GetObjectItemCaseSensitive(e,
                                JCE_BUNDLE_CATALOG_KEY_BUILD_HASH);
        const cJSON *jz = cJSON_GetObjectItemCaseSensitive(e,
                                JCE_BUNDLE_CATALOG_KEY_SIZE);
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(e,
                                JCE_BUNDLE_CATALOG_KEY_DEPS);

        if (!be->id || !cJSON_IsString(jf) ||
            !valid_bundle_file(jf->valuestring) ||
            !cJSON_IsString(jk) || !valid_kind(jk->valuestring) ||
            !cJSON_IsString(jh) || !is_hex64(jh->valuestring) ||
            !cJSON_IsNumber(jz) || jz->valuedouble < 1.0 ||
            jz->valuedouble != (double)(uint64_t)jz->valuedouble ||
            !cJSON_IsArray(jd) ||
            (cat->verify_content &&
             (!cJSON_IsString(jb) || !is_hex64(jb->valuestring))) ||
            (strcmp(jk->valuestring, JCE_BUNDLE_KIND_SCENE) == 0 &&
             (!cJSON_IsString(js) || !js->valuestring[0]))) {
            LOG_ERROR("jce_bundle", "invalid catalog entry '%s'", e->string);
            goto invalid_catalog;
        }

        be->file = dup_str(jf->valuestring);
        be->kind = dup_str(jk->valuestring);
        be->content_hash = dup_str(jh->valuestring);
        if (cJSON_IsString(jb)) be->build_hash = dup_str(jb->valuestring);
        if (cJSON_IsString(js)) be->scene_path = dup_str(js->valuestring);
        be->size = (uint64_t)jz->valuedouble;
        if (!be->file || !be->kind || !be->content_hash ||
            (cJSON_IsString(jb) && !be->build_hash) ||
            (cJSON_IsString(js) && !be->scene_path))
            goto invalid_catalog;

        uint32_t dn = (uint32_t)cJSON_GetArraySize(jd);
        if (dn > 0) {
            be->deps = (char **)JCE_CALLOC(dn, sizeof(char *));
            if (!be->deps) goto invalid_catalog;
            for (uint32_t i = 0; i < dn; ++i) {
                const cJSON *jx = cJSON_GetArrayItem(jd, (int)i);
                if (!cJSON_IsString(jx) || !valid_id(jx->valuestring)) {
                    LOG_ERROR("jce_bundle", "invalid dependency in '%s'",
                              be->id);
                    goto invalid_catalog;
                }
                be->deps[i] = dup_str(jx->valuestring);
                if (!be->deps[i]) goto invalid_catalog;
                ++be->dep_count;
            }
        }
    }

    if (!validate_dependency_graph(cat)) goto invalid_catalog;
    cJSON_Delete(root);
    LOG_INFO("jce_bundle", "opened catalog v%u with %u bundle(s) at %s",
                 cat->version, cat->entry_count, cat->base_dir ? cat->base_dir : "?");
    return cat;

invalid_catalog:
    cJSON_Delete(root);
    jce_bundle_catalog_close(cat);
    return NULL;
}

static const void *JCE_CALL bundle_pak_find(const JcePakArchive *pak,
                                            const char *path)
{
    return jce_pak_find(pak, path);
}

static uint64_t JCE_CALL bundle_pak_asset_size(const void *asset)
{
    return ((const JcePakAsset *)asset)->original_size;
}

static size_t JCE_CALL bundle_pak_decompress(const void *asset, void *buffer,
                                             size_t size)
{
    return jce_pak_decompress((const JcePakAsset *)asset, buffer, size);
}

static const JceFsPakProvider BUNDLE_PAK_PROVIDER = {
    bundle_pak_find, bundle_pak_asset_size, bundle_pak_decompress
};

static void ensure_pak_provider(void)
{
    if (!jce_fs_get_pak_provider())
        jce_fs_set_pak_provider(&BUNDLE_PAK_PROVIDER);
}

JCE_API void jce_bundle_catalog_close(JceBundleCatalog *cat) {
    if (!cat) return;
    /* Force-unmount everything. */
    for (uint32_t i = 0; i < cat->entry_count; ++i) {
        BundleEntry *be = &cat->entries[i];
        if (be->pak) {
            if (cat->fs) jce_fs_unmount_pak_named(cat->fs, be->id);
            jce_pak_close(be->pak);
            be->pak = NULL;
        }
        JCE_FREE(be->id);
        JCE_FREE(be->file);
        JCE_FREE(be->kind);
        if (be->scene_path)   JCE_FREE(be->scene_path);
        if (be->content_hash) JCE_FREE(be->content_hash);
        if (be->build_hash)   JCE_FREE(be->build_hash);
        for (uint32_t d = 0; d < be->dep_count; ++d) JCE_FREE(be->deps[d]);
        if (be->deps) JCE_FREE(be->deps);
    }
    if (cat->entries)  JCE_FREE(cat->entries);
    if (cat->base_dir) JCE_FREE(cat->base_dir);
    JCE_FREE(cat);
}

/* ================================================================== */
/* Catalog inspection                                                   */
/* ================================================================== */

JCE_API uint32_t jce_bundle_catalog_version(const JceBundleCatalog *cat) {
    return cat ? cat->version : 0;
}
JCE_API uint32_t jce_bundle_catalog_count(const JceBundleCatalog *cat) {
    return cat ? cat->entry_count : 0;
}
JCE_API const char *jce_bundle_catalog_id_at(const JceBundleCatalog *cat,
                                             uint32_t idx) {
    if (!cat || idx >= cat->entry_count) return NULL;
    return cat->entries[idx].id;
}
JCE_API const char *jce_bundle_catalog_kind(const JceBundleCatalog *cat,
                                            const char *bundle_id) {
    const BundleEntry *e = find_entry(cat, bundle_id);
    return e ? e->kind : NULL;
}
JCE_API const char *jce_bundle_catalog_file(const JceBundleCatalog *cat,
                                            const char *bundle_id) {
    const BundleEntry *e = find_entry(cat, bundle_id);
    return e ? e->file : NULL;
}
JCE_API const char *jce_bundle_catalog_scene_path(const JceBundleCatalog *cat,
                                                  const char *bundle_id) {
    const BundleEntry *e = find_entry(cat, bundle_id);
    return e ? e->scene_path : NULL;
}
JCE_API uint32_t jce_bundle_catalog_dep_count(const JceBundleCatalog *cat,
                                              const char *bundle_id) {
    const BundleEntry *e = find_entry(cat, bundle_id);
    return e ? e->dep_count : 0;
}
JCE_API const char *jce_bundle_catalog_dep_at(const JceBundleCatalog *cat,
                                              const char *bundle_id,
                                              uint32_t idx) {
    const BundleEntry *e = find_entry(cat, bundle_id);
    if (!e || idx >= e->dep_count) return NULL;
    return e->deps[idx];
}
JCE_API uint64_t jce_bundle_catalog_size_bytes(const JceBundleCatalog *cat,
                                               const char *bundle_id) {
    const BundleEntry *e = find_entry(cat, bundle_id);
    return e ? e->size : 0;
}
JCE_API const char *jce_bundle_catalog_content_hash(const JceBundleCatalog *cat,
                                                    const char *bundle_id) {
    const BundleEntry *e = find_entry(cat, bundle_id);
    return e ? e->content_hash : NULL;
}

/* ================================================================== */
/* Mount / unmount                                                      */
/* ================================================================== */

static uint32_t entry_index(const JceBundleCatalog *cat,
                            const BundleEntry *entry)
{
    return (uint32_t)(entry - cat->entries);
}

/* Build a unique dependency-first closure without recursive stack growth. */
static BundleEntry **collect_closure(JceBundleCatalog *cat, BundleEntry *root,
                                     uint32_t *out_count)
{
    uint32_t n = cat->entry_count;
    uint8_t *reachable = (uint8_t *)JCE_CALLOC(n ? n : 1, 1);
    uint8_t *added = (uint8_t *)JCE_CALLOC(n ? n : 1, 1);
    uint32_t *remaining = (uint32_t *)JCE_CALLOC(n ? n : 1,
                                                 sizeof(*remaining));
    BundleEntry **order = (BundleEntry **)JCE_CALLOC(n ? n : 1,
                                                     sizeof(*order));
    if (!reachable || !added || !remaining || !order) {
        JCE_FREE(reachable); JCE_FREE(added); JCE_FREE(remaining);
        JCE_FREE(order);
        return NULL;
    }

    reachable[entry_index(cat, root)] = 1;
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t i = 0; i < n; ++i) {
            if (!reachable[i]) continue;
            for (uint32_t d = 0; d < cat->entries[i].dep_count; ++d) {
                BundleEntry *dep = find_entry(cat, cat->entries[i].deps[d]);
                if (!dep) goto invalid;
                uint32_t di = entry_index(cat, dep);
                if (!reachable[di]) {
                    reachable[di] = 1;
                    changed = true;
                }
            }
        }
    }

    uint32_t wanted = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (!reachable[i]) continue;
        ++wanted;
        remaining[i] = cat->entries[i].dep_count;
    }

    uint32_t count = 0;
    while (count < wanted) {
        uint32_t leaf = n;
        for (uint32_t i = 0; i < n; ++i) {
            if (reachable[i] && !added[i] && remaining[i] == 0) {
                leaf = i;
                break;
            }
        }
        if (leaf == n) goto invalid;
        added[leaf] = 1;
        order[count++] = &cat->entries[leaf];
        for (uint32_t i = 0; i < n; ++i) {
            if (!reachable[i] || added[i]) continue;
            for (uint32_t d = 0; d < cat->entries[i].dep_count; ++d) {
                if (strcmp(cat->entries[i].deps[d],
                           cat->entries[leaf].id) == 0) {
                    --remaining[i];
                    break;
                }
            }
        }
    }

    JCE_FREE(reachable); JCE_FREE(added); JCE_FREE(remaining);
    *out_count = count;
    return order;

invalid:
    JCE_FREE(reachable); JCE_FREE(added); JCE_FREE(remaining);
    JCE_FREE(order);
    return NULL;
}

static JcePakArchive *load_verified_bundle(const JceBundleCatalog *cat,
                                           const BundleEntry *entry)
{
    char path[1024];
    int written = snprintf(path, sizeof(path), "%s/%s",
                           cat->base_dir ? cat->base_dir : ".", entry->file);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        LOG_ERROR("jce_bundle", "bundle path too long for '%s'", entry->id);
        return NULL;
    }

    JceMmap *mapping = jce_mmap_open(path);
    if (!mapping) {
        LOG_ERROR("jce_bundle", "failed to map '%s'", path);
        return NULL;
    }
    size_t size = jce_mmap_size(mapping);
    if (size != entry->size) {
        LOG_ERROR("jce_bundle", "size mismatch for '%s': expected %llu, got %llu",
                  entry->id, (unsigned long long)entry->size,
                  (unsigned long long)size);
        jce_mmap_close(mapping);
        return NULL;
    }

    if (cat->verify_content) {
        uint64_t expected = 0;
        if (!parse_hex64(entry->content_hash, &expected) ||
            XXH3_64bits(jce_mmap_data(mapping), size) != expected) {
            LOG_ERROR("jce_bundle", "content hash mismatch for '%s'",
                      entry->id);
            jce_mmap_close(mapping);
            return NULL;
        }
    }
    jce_mmap_close(mapping);

    JcePakArchive *pak = jce_pak_open_file(path);
    if (!pak) {
        LOG_ERROR("jce_bundle", "invalid archive '%s'", path);
    } else if (!catalog_bundle_manifest_valid(pak, entry, path)) {
        jce_pak_close(pak);
        pak = NULL;
    }
    return pak;
}

JCE_API bool jce_bundle_mount(JceBundleCatalog *cat, const char *bundle_id) {
    BundleEntry *be = find_entry(cat, bundle_id);
    if (!be) {
        LOG_WARN("jce_bundle", "mount: unknown bundle '%s'",
                     bundle_id ? bundle_id : "(null)");
        return false;
    }
    ensure_pak_provider();
    uint32_t count = 0;
    BundleEntry **closure = collect_closure(cat, be, &count);
    if (!closure) return false;

    JcePakArchive **prepared = (JcePakArchive **)JCE_CALLOC(
        count ? count : 1, sizeof(*prepared));
    uint8_t *mounted_now = (uint8_t *)JCE_CALLOC(count ? count : 1, 1);
    if (!prepared || !mounted_now) {
        JCE_FREE(prepared); JCE_FREE(mounted_now); JCE_FREE(closure);
        return false;
    }

    /* Preflight every file and hash before changing the filesystem. */
    for (uint32_t i = 0; i < count; ++i) {
        if (closure[i]->pak) continue;
        prepared[i] = load_verified_bundle(cat, closure[i]);
        if (!prepared[i]) goto rollback;
    }

    /* Commit dependency-first.  Refcounts change only after all mounts work. */
    for (uint32_t i = 0; i < count; ++i) {
        BundleEntry *entry = closure[i];
        if (entry->pak) continue;
        if (cat->fs &&
            !jce_fs_mount_pak_named(cat->fs, entry->id, prepared[i])) {
            LOG_ERROR("jce_bundle", "failed to mount '%s' on filesystem",
                      entry->id);
            goto rollback;
        }
        entry->pak = prepared[i];
        prepared[i] = NULL;
        mounted_now[i] = 1;
    }

    for (uint32_t i = 0; i < count; ++i) {
        ++closure[i]->refcount;
        if (mounted_now[i])
            LOG_INFO("jce_bundle", "mounted '%s' (%s)",
                     closure[i]->id, closure[i]->file);
    }
    JCE_FREE(prepared); JCE_FREE(mounted_now); JCE_FREE(closure);
    return true;

rollback:
    for (uint32_t i = count; i > 0; --i) {
        uint32_t at = i - 1;
        if (mounted_now[at]) {
            BundleEntry *entry = closure[at];
            if (cat->fs) jce_fs_unmount_pak_named(cat->fs, entry->id);
            jce_pak_close(entry->pak);
            entry->pak = NULL;
        }
    }
    for (uint32_t i = 0; i < count; ++i)
        if (prepared[i]) jce_pak_close(prepared[i]);
    JCE_FREE(prepared); JCE_FREE(mounted_now); JCE_FREE(closure);
    return false;
}

JCE_API bool jce_bundle_unmount(JceBundleCatalog *cat, const char *bundle_id) {
    BundleEntry *be = find_entry(cat, bundle_id);
    if (!be || !be->pak || be->refcount == 0) return false;
    uint32_t count = 0;
    BundleEntry **closure = collect_closure(cat, be, &count);
    if (!closure) return false;

    for (uint32_t i = 0; i < count; ++i) {
        if (closure[i]->pak && closure[i]->refcount > 0)
            --closure[i]->refcount;
    }
    for (uint32_t i = count; i > 0; --i) {
        BundleEntry *entry = closure[i - 1];
        if (!entry->pak || entry->refcount > 0) continue;
        if (cat->fs) jce_fs_unmount_pak_named(cat->fs, entry->id);
        jce_pak_close(entry->pak);
        entry->pak = NULL;
        LOG_INFO("jce_bundle", "unmounted '%s'", entry->id);
    }
    JCE_FREE(closure);
    return true;
}

JCE_API bool jce_bundle_is_mounted(const JceBundleCatalog *cat,
                                   const char *bundle_id) {
    const BundleEntry *e = find_entry(cat, bundle_id);
    return e && e->pak != NULL;
}

JCE_API uint32_t jce_bundle_refcount(const JceBundleCatalog *cat,
                                     const char *bundle_id) {
    const BundleEntry *e = find_entry(cat, bundle_id);
    return e ? e->refcount : 0;
}

JCE_API const char *jce_bundle_mount_for_scene(JceBundleCatalog *cat,
                                               const char *scene_path) {
    if (!cat || !scene_path) return NULL;
    for (uint32_t i = 0; i < cat->entry_count; ++i) {
        BundleEntry *e = &cat->entries[i];
        if (e->scene_path && strcmp(e->scene_path, scene_path) == 0) {
            if (jce_bundle_mount(cat, e->id)) return e->id;
            return NULL;
        }
    }
    return NULL;
}

/* ================================================================== */
/* Standalone bundle (no catalog) — minimal one-file loader            */
/* ================================================================== */

struct JceBundleFile {
    JceFileSystem *fs;
    JcePakArchive *pak;
    char          *mount_name;   /* what we registered with fs           */
    char          *id;            /* from __bundle__/manifest.json       */
    char          *scene_path;    /* from manifest (nullable)            */
    char          *kind;          /* from manifest (nullable)            */
};

static bool verify_standalone_sidecar(const char *bundle_path)
{
    char sidecar_path[1152];
    int written = snprintf(sidecar_path, sizeof(sidecar_path), "%s.json",
                           bundle_path);
    if (written < 0 || (size_t)written >= sizeof(sidecar_path)) return false;
    if (!jce_fs_host_exists_file(sidecar_path)) return true;

    size_t json_size = 0;
    char *json = read_text(sidecar_path, &json_size);
    if (!json) return false;
    cJSON *root = cJSON_ParseWithLength(json, json_size);
    JCE_FREE(json);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        LOG_ERROR("jce_bundle", "invalid sidecar '%s'", sidecar_path);
        return false;
    }

    const cJSON *contract = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_CONTRACT);
    const cJSON *name = contract ? cJSON_GetObjectItemCaseSensitive(
        contract, JCE_BUNDLE_KEY_CONTRACT_NAME) : NULL;
    const cJSON *major = contract ? cJSON_GetObjectItemCaseSensitive(
        contract, JCE_BUNDLE_KEY_CONTRACT_MAJOR) : NULL;
    const cJSON *minor = contract ? cJSON_GetObjectItemCaseSensitive(
        contract, JCE_BUNDLE_KEY_CONTRACT_MINOR) : NULL;
    bool valid = cJSON_IsObject(contract) && cJSON_IsString(name) &&
        name->valuestring &&
        strcmp(name->valuestring, JCE_BUNDLE_MANIFEST_CONTRACT_NAME) == 0 &&
        cJSON_IsNumber(major) &&
        major->valuedouble == (double)JCE_BUNDLE_MANIFEST_CONTRACT_MAJOR &&
        cJSON_IsNumber(minor) && minor->valuedouble >= 0.0 &&
        minor->valuedouble == (double)minor->valueint;
    if (!valid) {
        cJSON_Delete(root);
        LOG_ERROR("jce_bundle", "invalid sidecar contract '%s'", sidecar_path);
        return false;
    }

    /* v1.0 sidecars carried the source-graph hash under content_hash.  They
     * remain readable, but only v1.1+ can authenticate final archive bytes. */
    if (minor->valueint < 1) {
        cJSON_Delete(root);
        return true;
    }

    const cJSON *hash = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_CONTENT_HASH);
    const cJSON *build_hash = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_BUILD_HASH);
    const cJSON *archive_size = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_ARCHIVE_SIZE);
    uint64_t expected = 0;
    valid = cJSON_IsString(hash) && parse_hex64(hash->valuestring, &expected) &&
        cJSON_IsString(build_hash) && is_hex64(build_hash->valuestring) &&
        cJSON_IsNumber(archive_size) && archive_size->valuedouble >= 1.0 &&
        archive_size->valuedouble ==
            (double)(uint64_t)archive_size->valuedouble;
    uint64_t expected_size = valid ? (uint64_t)archive_size->valuedouble : 0;
    cJSON_Delete(root);
    if (!valid) {
        LOG_ERROR("jce_bundle", "incomplete sidecar '%s'", sidecar_path);
        return false;
    }

    JceMmap *mapping = jce_mmap_open(bundle_path);
    if (!mapping) return false;
    size_t actual_size = jce_mmap_size(mapping);
    uint64_t actual = XXH3_64bits(jce_mmap_data(mapping), actual_size);
    jce_mmap_close(mapping);
    if ((uint64_t)actual_size != expected_size || actual != expected) {
        LOG_ERROR("jce_bundle", "sidecar verification failed for '%s'",
                  bundle_path);
        return false;
    }
    return true;
}

static bool json_read_u64(const cJSON *value, uint64_t *out)
{
    if (!cJSON_IsNumber(value) || value->valuedouble < 0.0 ||
        value->valuedouble > (double)UINT64_MAX)
        return false;
    uint64_t parsed = (uint64_t)value->valuedouble;
    if ((double)parsed != value->valuedouble)
        return false;
    if (out)
        *out = parsed;
    return true;
}

static bool canonical_address(const char *address, char *canonical,
                              size_t canonical_cap)
{
    if (!address || !address[0])
        return false;
    size_t length = jce_archive_normalize_path(address, canonical,
                                               canonical_cap);
    return length > 0 && strcmp(address, canonical) == 0;
}

static bool manifest_dependencies_valid(const JcePakArchive *pak,
                                        const cJSON *dependencies,
                                        bool require_closed,
                                        const char *source_label)
{
    if (!dependencies)
        return true;
    if (!cJSON_IsArray(dependencies))
        return false;

    const cJSON *dependency = NULL;
    cJSON_ArrayForEach(dependency, dependencies) {
        const cJSON *address_j = cJSON_GetObjectItemCaseSensitive(
            dependency, JCE_BUNDLE_KEY_ASSET_ADDRESS);
        const cJSON *asset_id_j = cJSON_GetObjectItemCaseSensitive(
            dependency, JCE_BUNDLE_KEY_ASSET_ID);
        const cJSON *origin_j = cJSON_GetObjectItemCaseSensitive(
            dependency, JCE_BUNDLE_KEY_ORIGIN);
        char canonical[2048];
        if (!cJSON_IsObject(dependency) || !cJSON_IsString(address_j) ||
            !canonical_address(address_j->valuestring, canonical,
                               sizeof(canonical)) ||
            !cJSON_IsString(asset_id_j) ||
            !is_hex64(asset_id_j->valuestring) ||
            !cJSON_IsString(origin_j) || !origin_j->valuestring[0]) {
            LOG_ERROR("jce_bundle", "invalid dependency edge in %s",
                      source_label ? source_label : "(memory)");
            return false;
        }
        uint64_t declared_id = 0;
        if (!parse_hex64(asset_id_j->valuestring, &declared_id) ||
            declared_id != jce_archive_hash_normalized(
                canonical, strlen(canonical))) {
            LOG_ERROR("jce_bundle", "dependency asset id mismatch for '%s' in %s",
                      canonical, source_label ? source_label : "(memory)");
            return false;
        }
        if (require_closed && !jce_pak_find(pak, canonical)) {
            LOG_ERROR("jce_bundle", "standalone dependency '%s' is absent in %s",
                      canonical, source_label ? source_label : "(memory)");
            return false;
        }
    }
    return true;
}

static bool manifest_assets_valid(const JcePakArchive *pak,
                                  const cJSON *assets,
                                  uint32_t contract_minor,
                                  bool require_closed,
                                  const char *source_label)
{
    if (!cJSON_IsArray(assets))
        return false;
    if (contract_minor < 1)
        return true;
    if ((uint32_t)cJSON_GetArraySize(assets) + 1u != jce_pak_count(pak)) {
        LOG_ERROR("jce_bundle", "manifest/archive asset count mismatch in %s",
                  source_label ? source_label : "(memory)");
        return false;
    }

    const cJSON *asset = NULL;
    cJSON_ArrayForEach(asset, assets) {
        const cJSON *path_j = cJSON_GetObjectItemCaseSensitive(
            asset, JCE_BUNDLE_KEY_ASSET_PATH);
        const cJSON *size_j = cJSON_GetObjectItemCaseSensitive(
            asset, JCE_BUNDLE_KEY_ASSET_SIZE);
        const cJSON *asset_id_j = cJSON_GetObjectItemCaseSensitive(
            asset, JCE_BUNDLE_KEY_ASSET_ID);
        const cJSON *content_id_j = cJSON_GetObjectItemCaseSensitive(
            asset, JCE_BUNDLE_KEY_CONTENT_ID);
        const cJSON *hash_j = cJSON_GetObjectItemCaseSensitive(
            asset, JCE_BUNDLE_KEY_ASSET_HASH);
        const cJSON *type_j = cJSON_GetObjectItemCaseSensitive(
            asset, JCE_BUNDLE_KEY_ASSET_TYPE);
        const cJSON *representation_j = cJSON_GetObjectItemCaseSensitive(
            asset, JCE_BUNDLE_KEY_REPRESENTATION);
        const cJSON *dependencies_j = cJSON_GetObjectItemCaseSensitive(
            asset, JCE_BUNDLE_KEY_DEPENDENCIES);
        char canonical[2048];
        uint64_t declared_size = 0;
        uint64_t declared_asset_id = 0;
        uint64_t declared_content_id = 0;

        if (!cJSON_IsObject(asset) || !cJSON_IsString(path_j) ||
            !canonical_address(path_j->valuestring, canonical,
                               sizeof(canonical)) ||
            strcmp(canonical, JCE_BUNDLE_MANIFEST_VPATH) == 0 ||
            !json_read_u64(size_j, &declared_size) ||
            !cJSON_IsString(asset_id_j) ||
            !parse_hex64(asset_id_j->valuestring, &declared_asset_id) ||
            !cJSON_IsString(content_id_j) ||
            !parse_hex64(content_id_j->valuestring, &declared_content_id) ||
            !cJSON_IsString(hash_j) ||
            strcmp(hash_j->valuestring, content_id_j->valuestring) != 0 ||
            !cJSON_IsString(type_j) || !type_j->valuestring[0] ||
            !cJSON_IsString(representation_j) ||
            !representation_j->valuestring[0]) {
            LOG_ERROR("jce_bundle", "invalid asset record in %s",
                      source_label ? source_label : "(memory)");
            return false;
        }
        if (declared_asset_id != jce_archive_hash_normalized(
                canonical, strlen(canonical))) {
            LOG_ERROR("jce_bundle", "asset id mismatch for '%s' in %s",
                      canonical, source_label ? source_label : "(memory)");
            return false;
        }

        const cJSON *previous = assets->child;
        while (previous && previous != asset) {
            const cJSON *previous_path = cJSON_GetObjectItemCaseSensitive(
                previous, JCE_BUNDLE_KEY_ASSET_PATH);
            if (cJSON_IsString(previous_path) &&
                strcmp(previous_path->valuestring, canonical) == 0) {
                LOG_ERROR("jce_bundle", "duplicate asset '%s' in %s",
                          canonical, source_label ? source_label : "(memory)");
                return false;
            }
            previous = previous->next;
        }

        const JcePakAsset *pak_asset = jce_pak_find(pak, canonical);
        if (!pak_asset || pak_asset->original_size != declared_size ||
            declared_size > SIZE_MAX) {
            LOG_ERROR("jce_bundle", "asset size/address mismatch for '%s' in %s",
                      canonical, source_label ? source_label : "(memory)");
            return false;
        }
        size_t payload_size = (size_t)declared_size;
        uint8_t *payload = (uint8_t *)JCE_MALLOC(payload_size ? payload_size : 1);
        if (!payload)
            return false;
        bool read_ok = payload_size == 0 ||
            jce_pak_decompress_ex(pak, pak_asset, payload,
                                  payload_size) == payload_size;
        uint64_t actual_content_id = read_ok
            ? jce_archive_content_hash(payload, payload_size) : 0;
        JCE_FREE(payload);
        if (!read_ok || actual_content_id != declared_content_id) {
            LOG_ERROR("jce_bundle", "content id mismatch for '%s' in %s",
                      canonical, source_label ? source_label : "(memory)");
            return false;
        }
        if (!manifest_dependencies_valid(pak, dependencies_j,
                                         require_closed, source_label))
            return false;
    }
    return true;
}

static bool catalog_bundle_manifest_valid(const JcePakArchive *pak,
                                          const BundleEntry *entry,
                                          const char *source_label)
{
    const JcePakAsset *manifest_asset = jce_pak_find(
        pak, JCE_BUNDLE_MANIFEST_VPATH);
    if (!manifest_asset || manifest_asset->original_size == 0 ||
        manifest_asset->original_size > SIZE_MAX - 1) {
        LOG_ERROR("jce_bundle", "bundle manifest missing from %s",
                  source_label);
        return false;
    }

    size_t manifest_size = (size_t)manifest_asset->original_size;
    char *text = (char *)JCE_MALLOC(manifest_size + 1);
    if (!text || jce_pak_decompress_ex(pak, manifest_asset, text,
                                       manifest_size) != manifest_size) {
        JCE_FREE(text);
        LOG_ERROR("jce_bundle", "cannot read bundle manifest from %s",
                  source_label);
        return false;
    }
    text[manifest_size] = '\0';
    cJSON *root = cJSON_ParseWithLength(text, manifest_size);
    JCE_FREE(text);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        LOG_ERROR("jce_bundle", "invalid bundle manifest JSON in %s",
                  source_label);
        return false;
    }

    const cJSON *contract = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_CONTRACT);
    const cJSON *name = contract ? cJSON_GetObjectItemCaseSensitive(
        contract, JCE_BUNDLE_KEY_CONTRACT_NAME) : NULL;
    const cJSON *major = contract ? cJSON_GetObjectItemCaseSensitive(
        contract, JCE_BUNDLE_KEY_CONTRACT_MAJOR) : NULL;
    const cJSON *minor = contract ? cJSON_GetObjectItemCaseSensitive(
        contract, JCE_BUNDLE_KEY_CONTRACT_MINOR) : NULL;
    const cJSON *id_j = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_ID);
    const cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KIND_KEY);
    const cJSON *scene_j = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_SCENE_PATH);
    const cJSON *build_j = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_BUILD_HASH);
    const cJSON *depends_j = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_DEPENDS_ON);
    const cJSON *assets_j = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_ASSETS);

    bool valid = cJSON_IsObject(contract) && cJSON_IsString(name) &&
        name->valuestring &&
        strcmp(name->valuestring, JCE_BUNDLE_MANIFEST_CONTRACT_NAME) == 0 &&
        cJSON_IsNumber(major) &&
        major->valuedouble == (double)JCE_BUNDLE_MANIFEST_CONTRACT_MAJOR &&
        cJSON_IsNumber(minor) && minor->valuedouble >= 0.0 &&
        minor->valuedouble == (double)minor->valueint &&
        cJSON_IsString(id_j) &&
        strcmp(id_j->valuestring, entry->id) == 0 &&
        cJSON_IsString(kind_j) &&
        strcmp(kind_j->valuestring, entry->kind) == 0 &&
        cJSON_IsArray(depends_j) && cJSON_IsArray(assets_j) &&
        ((entry->scene_path == NULL && !cJSON_IsString(scene_j)) ||
         (entry->scene_path != NULL && cJSON_IsString(scene_j) &&
          strcmp(scene_j->valuestring, entry->scene_path) == 0));
    if (valid && minor->valueint >= 1) {
        valid = cJSON_IsString(build_j) && entry->build_hash &&
            strcmp(build_j->valuestring, entry->build_hash) == 0 &&
            content_contract_compatible(root, (uint32_t)minor->valueint,
                                        source_label);
    }
    if (valid && cJSON_GetArraySize(depends_j) != (int)entry->dep_count)
        valid = false;
    for (uint32_t i = 0; valid && i < entry->dep_count; ++i) {
        bool found = false;
        const cJSON *dependency = NULL;
        cJSON_ArrayForEach(dependency, depends_j) {
            if (cJSON_IsString(dependency) && dependency->valuestring &&
                strcmp(dependency->valuestring, entry->deps[i]) == 0) {
                found = true;
                break;
            }
        }
        valid = found;
    }
    if (valid) {
        valid = manifest_assets_valid(pak, assets_j,
                                      (uint32_t)minor->valueint, false,
                                      source_label);
    }

    cJSON_Delete(root);
    if (!valid) {
        LOG_ERROR("jce_bundle",
                  "catalog/manifest contract mismatch for '%s' in %s",
                  entry->id, source_label);
    }
    return valid;
}

static JceBundleFile *bundle_file_open_pak(JceFileSystem *fs,
                                           JcePakArchive *pak,
                                           const char *fallback_name,
                                           const char *mount_name_or_null,
                                           const char *source_label)
{
    if (!pak) return NULL;
    ensure_pak_provider();

    /* Pull and validate the embedded manifest before exposing any archive
     * entries.  A plain JPAK is not a Bundle. */
    const JcePakAsset *mf = jce_pak_find(pak, JCE_BUNDLE_MANIFEST_VPATH);
    char  *id_str        = NULL;
    char  *scene_str     = NULL;
    char  *kind_str      = NULL;
    if (!mf || mf->original_size == 0 || mf->original_size > SIZE_MAX - 1) {
        LOG_ERROR("jce_bundle", "bundle manifest missing from %s",
                  source_label ? source_label : "(memory)");
        jce_pak_close(pak);
        return NULL;
    }

    size_t need = (size_t)mf->original_size;
    char *buf = (char *)JCE_MALLOC(need + 1);
    if (!buf || jce_pak_decompress_ex(pak, mf, buf, need) != need) {
        LOG_ERROR("jce_bundle", "cannot read bundle manifest from %s",
                  source_label ? source_label : "(memory)");
        JCE_FREE(buf);
        jce_pak_close(pak);
        return NULL;
    }
    buf[need] = '\0';
    cJSON *root = cJSON_ParseWithLength(buf, need);
    JCE_FREE(buf);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        jce_pak_close(pak);
        return NULL;
    }

    const cJSON *contract = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_CONTRACT);
    const cJSON *name = contract ? cJSON_GetObjectItemCaseSensitive(
        contract, JCE_BUNDLE_KEY_CONTRACT_NAME) : NULL;
    const cJSON *major = contract ? cJSON_GetObjectItemCaseSensitive(
        contract, JCE_BUNDLE_KEY_CONTRACT_MAJOR) : NULL;
    const cJSON *minor = contract ? cJSON_GetObjectItemCaseSensitive(
        contract, JCE_BUNDLE_KEY_CONTRACT_MINOR) : NULL;
    const cJSON *id_j = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_ID);
    const cJSON *scene_j = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_SCENE_PATH);
    const cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KIND_KEY);
    const cJSON *assets_j = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_ASSETS);
    const cJSON *build_j = cJSON_GetObjectItemCaseSensitive(
        root, JCE_BUNDLE_KEY_BUILD_HASH);

    bool manifest_ok = cJSON_IsObject(contract) && cJSON_IsString(name) &&
        name->valuestring &&
        strcmp(name->valuestring, JCE_BUNDLE_MANIFEST_CONTRACT_NAME) == 0 &&
        cJSON_IsNumber(major) &&
        major->valuedouble == (double)JCE_BUNDLE_MANIFEST_CONTRACT_MAJOR &&
        cJSON_IsNumber(minor) && minor->valuedouble >= 0.0 &&
        minor->valuedouble == (double)minor->valueint &&
        cJSON_IsString(id_j) && valid_id(id_j->valuestring) &&
        cJSON_IsString(kind_j) && valid_kind(kind_j->valuestring) &&
        cJSON_IsArray(assets_j) &&
        (minor->valueint < 1 ||
         (cJSON_IsString(build_j) && is_hex64(build_j->valuestring))) &&
        (strcmp(kind_j->valuestring, JCE_BUNDLE_KIND_SCENE) != 0 ||
         (cJSON_IsString(scene_j) && scene_j->valuestring[0]));
    if (manifest_ok)
        manifest_ok = content_contract_compatible(
            root, (uint32_t)minor->valueint, source_label);
    if (manifest_ok)
        manifest_ok = manifest_assets_valid(
            pak, assets_j, (uint32_t)minor->valueint, true, source_label);
    if (!manifest_ok) {
        LOG_ERROR("jce_bundle", "invalid bundle manifest in %s",
                  source_label ? source_label : "(memory)");
        cJSON_Delete(root);
        jce_pak_close(pak);
        return NULL;
    }

    bool had_scene = cJSON_IsString(scene_j);
    id_str = dup_str(id_j->valuestring);
    kind_str = dup_str(kind_j->valuestring);
    if (had_scene) scene_str = dup_str(scene_j->valuestring);
    cJSON_Delete(root);
    if (!id_str || !kind_str ||
        (had_scene && !scene_str)) {
        JCE_FREE(id_str); JCE_FREE(kind_str); JCE_FREE(scene_str);
        jce_pak_close(pak);
        return NULL;
    }

    /* Mount name precedence: caller override > manifest id > basename. */
    char *mname = NULL;
    if (mount_name_or_null && mount_name_or_null[0]) {
        mname = dup_str(mount_name_or_null);
    } else if (id_str) {
        mname = dup_str(id_str);
    } else {
        mname = dup_str((fallback_name && fallback_name[0])
                            ? fallback_name
                            : "bundle");
    }
    if (!mname) {
        jce_pak_close(pak);
        JCE_FREE(id_str); JCE_FREE(scene_str); JCE_FREE(kind_str);
        return NULL;
    }

    if (fs && !jce_fs_mount_pak_named(fs, mname, pak)) {
        LOG_ERROR("jce_bundle", "failed to mount standalone bundle '%s'",
                  mname);
        jce_pak_close(pak);
        JCE_FREE(mname); JCE_FREE(id_str);
        JCE_FREE(scene_str); JCE_FREE(kind_str);
        return NULL;
    }

    JceBundleFile *bf = (JceBundleFile *)JCE_MALLOC(sizeof(*bf));
    if (!bf) {
        if (fs) jce_fs_unmount_pak_named(fs, mname);
        jce_pak_close(pak);
        JCE_FREE(mname); JCE_FREE(id_str);
        JCE_FREE(scene_str); JCE_FREE(kind_str);
        return NULL;
    }
    bf->fs         = fs;
    bf->pak        = pak;
    bf->mount_name = mname;
    bf->id         = id_str    ? id_str    : dup_str(mname);
    bf->scene_path = scene_str;
    bf->kind       = kind_str;
    LOG_INFO("jce_bundle", "mounted standalone '%s' from %s",
             bf->mount_name,
             source_label ? source_label : "(memory)");
    return bf;
}

JCE_API JceBundleFile *jce_bundle_file_open(JceFileSystem *fs,
                                            const char *jbundle_path,
                                            const char *mount_name_or_null)
{
    if (!jbundle_path) return NULL;

    if (!verify_standalone_sidecar(jbundle_path)) return NULL;

    JcePakArchive *pak = jce_pak_open_file(jbundle_path);
    if (!pak) {
        LOG_ERROR("jce_bundle", "failed to open '%s'", jbundle_path);
        return NULL;
    }

    const char *base = jbundle_path;
    for (const char *p = jbundle_path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;

    return bundle_file_open_pak(fs, pak, base,
                                mount_name_or_null, jbundle_path);
}

JCE_API JceBundleFile *jce_bundle_file_open_memory(
                                            JceFileSystem *fs,
                                            const void *data,
                                            size_t size,
                                            const char *mount_name_or_null)
{
    if (!data || size == 0) return NULL;

    JcePakArchive *pak = jce_pak_open(data, size);
    if (!pak) {
        LOG_ERROR("jce_bundle", "%s",
                  "failed to open embedded standalone bundle");
        return NULL;
    }

    return bundle_file_open_pak(fs, pak, mount_name_or_null,
                                mount_name_or_null, "(memory)");
}

JCE_API void jce_bundle_file_close(JceBundleFile *bf)
{
    if (!bf) return;
    if (bf->fs && bf->mount_name)
        jce_fs_unmount_pak_named(bf->fs, bf->mount_name);
    if (bf->pak) jce_pak_close(bf->pak);
    JCE_FREE(bf->mount_name);
    JCE_FREE(bf->id);
    JCE_FREE(bf->scene_path);
    JCE_FREE(bf->kind);
    JCE_FREE(bf);
}

JCE_API const char *jce_bundle_file_id(const JceBundleFile *bf) {
    return bf ? bf->id : NULL;
}
JCE_API const char *jce_bundle_file_scene_path(const JceBundleFile *bf) {
    return bf ? bf->scene_path : NULL;
}
JCE_API const char *jce_bundle_file_kind(const JceBundleFile *bf) {
    return bf ? bf->kind : NULL;
}
JCE_API JcePakArchive *jce_bundle_file_pak(const JceBundleFile *bf) {
    return bf ? bf->pak : NULL;
}
