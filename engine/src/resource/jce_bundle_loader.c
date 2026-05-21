/* jce_bundle_loader.c
 *
 * Implementation of the runtime scene-asset-bundle loader.  See
 * jce_bundle_loader.h for the public contract.
 */

#include <jce/resource/jce_bundle_loader.h>
#include <jce/resource/jce_bundle_format.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <cjson/cJSON.h>

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
    BundleEntry   *entries;
    uint32_t       entry_count;
};

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
    if (!root) {
        LOG_ERROR("jce_bundle", "catalog JSON parse failed: %s",
                      catalog_path);
        return NULL;
    }

    /* Validate contract. */
    const cJSON *contract = cJSON_GetObjectItemCaseSensitive(root,
                                JCE_BUNDLE_KEY_CONTRACT);
    if (contract) {
        const cJSON *name  = cJSON_GetObjectItemCaseSensitive(contract,
                                JCE_BUNDLE_KEY_CONTRACT_NAME);
        const cJSON *major = cJSON_GetObjectItemCaseSensitive(contract,
                                JCE_BUNDLE_KEY_CONTRACT_MAJOR);
        if (name && cJSON_IsString(name) &&
            strcmp(name->valuestring, JCE_BUNDLE_CATALOG_CONTRACT_NAME) != 0) {
            LOG_ERROR("jce_bundle", "catalog has wrong contract name '%s'",
                          name->valuestring);
            cJSON_Delete(root); return NULL;
        }
        if (major && !jce_bundle_catalog_major_compatible((uint32_t)major->valuedouble)) {
            LOG_ERROR("jce_bundle", "catalog contract major %u incompatible",
                          (uint32_t)major->valuedouble);
            cJSON_Delete(root); return NULL;
        }
    }

    JceBundleCatalog *cat = (JceBundleCatalog *)JCE_MALLOC(sizeof(*cat));
    if (!cat) { cJSON_Delete(root); return NULL; }
    memset(cat, 0, sizeof(*cat));
    cat->fs       = fs;
    cat->base_dir = dirname_dup(catalog_path);

    const cJSON *vj = cJSON_GetObjectItemCaseSensitive(root,
                            JCE_BUNDLE_CATALOG_KEY_VERSION);
    cat->version = vj ? (uint32_t)vj->valuedouble : 0;

    const cJSON *bundles = cJSON_GetObjectItemCaseSensitive(root,
                                JCE_BUNDLE_CATALOG_KEY_BUNDLES);
    if (bundles && cJSON_IsObject(bundles)) {
        uint32_t n = (uint32_t)cJSON_GetArraySize(bundles);
        cat->entries = (BundleEntry *)JCE_MALLOC(sizeof(BundleEntry) * (n ? n : 1));
        if (!cat->entries) { cJSON_Delete(root); jce_bundle_catalog_close(cat); return NULL; }
        memset(cat->entries, 0, sizeof(BundleEntry) * (n ? n : 1));

        const cJSON *e = NULL;
        cJSON_ArrayForEach(e, bundles) {
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
            const cJSON *jz = cJSON_GetObjectItemCaseSensitive(e,
                                    JCE_BUNDLE_CATALOG_KEY_SIZE);
            const cJSON *jd = cJSON_GetObjectItemCaseSensitive(e,
                                    JCE_BUNDLE_CATALOG_KEY_DEPS);
            be->file = dup_str(jf && cJSON_IsString(jf) ? jf->valuestring : "");
            be->kind = dup_str(jk && cJSON_IsString(jk) ? jk->valuestring : JCE_BUNDLE_KIND_SCENE);
            if (js && cJSON_IsString(js)) be->scene_path   = dup_str(js->valuestring);
            if (jh && cJSON_IsString(jh)) be->content_hash = dup_str(jh->valuestring);
            be->size = jz ? (uint64_t)jz->valuedouble : 0;
            if (jd && cJSON_IsArray(jd)) {
                uint32_t dn = (uint32_t)cJSON_GetArraySize(jd);
                if (dn > 0) {
                    be->deps = (char **)JCE_MALLOC(sizeof(char *) * dn);
                    if (be->deps) {
                        for (uint32_t i = 0; i < dn; ++i) {
                            const cJSON *jx = cJSON_GetArrayItem(jd, (int)i);
                            be->deps[i] = dup_str(jx && cJSON_IsString(jx)
                                                  ? jx->valuestring : "");
                        }
                        be->dep_count = dn;
                    }
                }
            }
        }
    }

    cJSON_Delete(root);
    LOG_INFO("jce_bundle", "opened catalog v%u with %u bundle(s) at %s",
                 cat->version, cat->entry_count, cat->base_dir ? cat->base_dir : "?");
    return cat;
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

static bool mount_recursive(JceBundleCatalog *cat, BundleEntry *be) {
    /* Mount deps first (they take precedence in lookup if shadowed). */
    for (uint32_t i = 0; i < be->dep_count; ++i) {
        BundleEntry *dep = find_entry(cat, be->deps[i]);
        if (!dep) {
            LOG_WARN("jce_bundle", "bundle '%s' references unknown dep '%s'",
                         be->id, be->deps[i]);
            continue;
        }
        if (!mount_recursive(cat, dep)) return false;
    }

    if (be->pak) {
        be->refcount++;
        return true;
    }

    /* Construct full path. */
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s",
             cat->base_dir ? cat->base_dir : ".", be->file);

    JcePakArchive *pak = jce_pak_open_file(path);
    if (!pak) {
        LOG_ERROR("jce_bundle", "failed to open '%s'", path);
        return false;
    }
    if (cat->fs) {
        if (!jce_fs_mount_pak_named(cat->fs, be->id, pak)) {
            LOG_ERROR("jce_bundle", "failed to mount '%s' on filesystem",
                          be->id);
            jce_pak_close(pak);
            return false;
        }
    }
    be->pak = pak;
    be->refcount = 1;
    LOG_INFO("jce_bundle", "mounted '%s' (%s)", be->id, be->file);
    return true;
}

static void unmount_recursive(JceBundleCatalog *cat, BundleEntry *be) {
    if (!be || !be->pak) return;
    if (be->refcount > 0) be->refcount--;
    if (be->refcount > 0) return;

    if (cat->fs) jce_fs_unmount_pak_named(cat->fs, be->id);
    jce_pak_close(be->pak);
    be->pak = NULL;
    LOG_INFO("jce_bundle", "unmounted '%s'", be->id);

    for (uint32_t i = 0; i < be->dep_count; ++i) {
        BundleEntry *dep = find_entry(cat, be->deps[i]);
        if (dep) unmount_recursive(cat, dep);
    }
}

JCE_API bool jce_bundle_mount(JceBundleCatalog *cat, const char *bundle_id) {
    BundleEntry *be = find_entry(cat, bundle_id);
    if (!be) {
        LOG_WARN("jce_bundle", "mount: unknown bundle '%s'",
                     bundle_id ? bundle_id : "(null)");
        return false;
    }
    return mount_recursive(cat, be);
}

JCE_API bool jce_bundle_unmount(JceBundleCatalog *cat, const char *bundle_id) {
    BundleEntry *be = find_entry(cat, bundle_id);
    if (!be || !be->pak) return false;
    unmount_recursive(cat, be);
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

JCE_API JceBundleFile *jce_bundle_file_open(JceFileSystem *fs,
                                            const char *jbundle_path,
                                            const char *mount_name_or_null)
{
    if (!jbundle_path) return NULL;

    JcePakArchive *pak = jce_pak_open_file(jbundle_path);
    if (!pak) {
        LOG_ERROR("jce_bundle", "failed to open '%s'", jbundle_path);
        return NULL;
    }

    /* Pull manifest to learn id / scene_path. */
    const JcePakAsset *mf = jce_pak_find(pak, JCE_BUNDLE_MANIFEST_VPATH);
    char  *id_str        = NULL;
    char  *scene_str     = NULL;
    char  *kind_str      = NULL;
    if (mf) {
        size_t need = (size_t)mf->original_size;
        char  *buf  = (char *)JCE_MALLOC(need + 1);
        if (buf && jce_pak_decompress_ex(pak, mf, buf, need) == need) {
            buf[need] = '\0';
            cJSON *root = cJSON_ParseWithLength(buf, need);
            if (root) {
                const cJSON *id_j    = cJSON_GetObjectItemCaseSensitive(
                                           root, JCE_BUNDLE_KEY_ID);
                const cJSON *scene_j = cJSON_GetObjectItemCaseSensitive(
                                           root, JCE_BUNDLE_KEY_SCENE_PATH);
                const cJSON *kind_j  = cJSON_GetObjectItemCaseSensitive(
                                           root, JCE_BUNDLE_KIND_KEY);
                if (cJSON_IsString(id_j))    id_str    = dup_str(id_j->valuestring);
                if (cJSON_IsString(scene_j)) scene_str = dup_str(scene_j->valuestring);
                if (cJSON_IsString(kind_j))  kind_str  = dup_str(kind_j->valuestring);
                cJSON_Delete(root);
            }
        }
        JCE_FREE(buf);
    }

    /* Mount name precedence: caller override > manifest id > basename. */
    char *mname = NULL;
    if (mount_name_or_null && mount_name_or_null[0]) {
        mname = dup_str(mount_name_or_null);
    } else if (id_str) {
        mname = dup_str(id_str);
    } else {
        const char *base = jbundle_path;
        for (const char *p = jbundle_path; *p; ++p)
            if (*p == '/' || *p == '\\') base = p + 1;
        mname = dup_str(base);
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
             bf->mount_name, jbundle_path);
    return bf;
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
