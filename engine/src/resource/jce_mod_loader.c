/* jce_mod_loader.c
 *
 * Game-side modding / external-PAK loader (FEATURE 9.5).  See
 * jce_mod_loader.h for the public contract and the sandbox / threat model.
 *
 * This module is pure plumbing over two existing primitives:
 *   - the JCE Archive reader (<jce/resource/jce_archive.h>) opens each mod
 *     file as a borrowed JceArchive, and
 *   - the layered-archive mount stack (jce_archive_mount_*) composes the
 *     base + enabled mods so a higher-priority mod's asset transparently
 *     overrides a lower-priority one's and, last, the base content.
 *
 * It deliberately executes NO code from a mod: a mod is a bag of asset bytes
 * read through the normalized virtual-path namespace.  Discovery is explicit
 * (jce_mod_loader_scan) so nothing auto-runs from engine boot.
 */

#include <jce/resource/jce_mod_loader.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* Internal data model                                                  */
/* ================================================================== */

typedef struct ModRecord {
    char       *id;          /* owned; never NULL                          */
    char       *name;        /* owned; never NULL                          */
    char       *version;     /* owned; never NULL ("" when unspecified)    */
    char       *file;        /* owned; absolute path to the archive        */
    int32_t     load_order;  /* lower = lower priority                      */
    bool        enabled;
    bool        mounted;     /* in the live mount stack after mount()       */
    JceArchive *archive;     /* owned by the loader (opened in scan)        */
} ModRecord;

struct JceModLoader {
    ModRecord       *mods;
    size_t           count;
    size_t           cap;
    JceArchive      *base;       /* borrowed (caller-owned), lowest layer   */
    JceArchiveMount *mount;      /* live override stack (owned)             */
};

/* ================================================================== */
/* Small helpers                                                        */
/* ================================================================== */

static char *dup_str(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *r = (char *)jce_malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

/* Case-insensitive suffix test (extension match). */
static bool ends_with_ci(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) return false;
    const char *t = s + (ls - lf);
    for (size_t i = 0; i < lf; ++i) {
        char a = t[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static bool name_is_mod_file(const char *name) {
    return ends_with_ci(name, ".jpak") || ends_with_ci(name, ".jmod");
}

/* Strip the directory + last extension from a file path to get a stem id. */
static char *stem_from_path(const char *path) {
    if (!path) return dup_str("");
    const char *base = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    const char *dot = NULL;
    for (const char *p = base; *p; ++p)
        if (*p == '.') dot = p;
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    char *r = (char *)jce_malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, base, n);
    r[n] = '\0';
    return r;
}

static ModRecord *find_by_id(JceModLoader *ml, const char *id) {
    if (!ml || !id) return NULL;
    for (size_t i = 0; i < ml->count; ++i)
        if (ml->mods[i].id && strcmp(ml->mods[i].id, id) == 0)
            return &ml->mods[i];
    return NULL;
}

static ModRecord *find_by_file(JceModLoader *ml, const char *file) {
    if (!ml || !file) return NULL;
    for (size_t i = 0; i < ml->count; ++i)
        if (ml->mods[i].file && strcmp(ml->mods[i].file, file) == 0)
            return &ml->mods[i];
    return NULL;
}

static void mod_record_free(ModRecord *m) {
    if (!m) return;
    if (m->archive) jce_archive_close(m->archive);
    jce_free(m->id);
    jce_free(m->name);
    jce_free(m->version);
    jce_free(m->file);
    memset(m, 0, sizeof(*m));
}

/* ================================================================== */
/* Manifest parsing                                                     */
/* ================================================================== */
/*
 * A manifest is OPTIONAL.  Two sources are consulted, sidecar first:
 *   1. a sibling "<file-stem>.mod.json" next to the archive, then
 *   2. an in-archive entry "mod.json".
 * The sidecar wins so users can re-skin a shipped mod's metadata without
 * repacking it.  Recognized fields (all optional):
 *   id, name, version : strings
 *   load_order        : integer (lower = lower priority)
 *   enabled           : bool (default true)
 * Unknown fields are ignored; crucially NO field can request code execution.
 */
static void apply_manifest_json(ModRecord *rec, const char *json, size_t len) {
    if (!rec || !json || len == 0) return;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return;

    const cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsString(jid) && jid->valuestring && jid->valuestring[0]) {
        char *nv = dup_str(jid->valuestring);
        if (nv) { jce_free(rec->id); rec->id = nv; }
    }
    const cJSON *jname = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(jname) && jname->valuestring) {
        char *nv = dup_str(jname->valuestring);
        if (nv) { jce_free(rec->name); rec->name = nv; }
    }
    const cJSON *jver = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (cJSON_IsString(jver) && jver->valuestring) {
        char *nv = dup_str(jver->valuestring);
        if (nv) { jce_free(rec->version); rec->version = nv; }
    }
    const cJSON *jord = cJSON_GetObjectItemCaseSensitive(root, "load_order");
    if (cJSON_IsNumber(jord))
        rec->load_order = (int32_t)jord->valuedouble;
    const cJSON *jen = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (cJSON_IsBool(jen))
        rec->enabled = cJSON_IsTrue(jen) ? true : false;

    cJSON_Delete(root);
}

/* Read the sibling "<stem>.mod.json" sidecar, if present, into rec. */
static void load_sidecar_manifest(ModRecord *rec) {
    if (!rec || !rec->file) return;
    /* Replace the archive extension with ".mod.json". */
    const char *dot = NULL;
    for (const char *p = rec->file; *p; ++p)
        if (*p == '.') dot = p;
    size_t base_len = dot ? (size_t)(dot - rec->file) : strlen(rec->file);
    static const char k_suffix[] = ".mod.json";
    size_t total = base_len + sizeof(k_suffix); /* incl NUL */
    char *side = (char *)jce_malloc(total);
    if (!side) return;
    memcpy(side, rec->file, base_len);
    memcpy(side + base_len, k_suffix, sizeof(k_suffix));

    if (jce_fs_host_exists_file(side)) {
        uint64_t sz = 0;
        void *buf = jce_fs_host_read_all(side, &sz);
        if (buf) {
            apply_manifest_json(rec, (const char *)buf, (size_t)sz);
            jce_fs_buffer_free(buf);
        }
    }
    jce_free(side);
}

/* Read the in-archive "mod.json" entry, if present, into rec. */
static void load_inpak_manifest(ModRecord *rec) {
    if (!rec || !rec->archive) return;
    const JceArchiveEntry *e = jce_archive_find(rec->archive, "mod.json");
    if (!e || e->original_size == 0) return;
    char *buf = (char *)jce_malloc((size_t)e->original_size);
    if (!buf) return;
    size_t n = jce_archive_read(rec->archive, e, buf, (size_t)e->original_size);
    if (n) apply_manifest_json(rec, buf, n);
    jce_free(buf);
}

/* ================================================================== */
/* Lifecycle                                                            */
/* ================================================================== */

JceModLoader *JCE_CALL jce_mod_loader_create(void) {
    JceModLoader *ml = (JceModLoader *)jce_malloc(sizeof(*ml));
    if (!ml) return NULL;
    ml->mods  = NULL;
    ml->count = 0;
    ml->cap   = 0;
    ml->base  = NULL;
    ml->mount = NULL;
    return ml;
}

void JCE_CALL jce_mod_loader_destroy(JceModLoader *ml) {
    if (!ml) return;
    if (ml->mount) jce_archive_mount_destroy(ml->mount);
    for (size_t i = 0; i < ml->count; ++i)
        mod_record_free(&ml->mods[i]);
    jce_free(ml->mods);
    jce_free(ml);
}

/* ================================================================== */
/* Discovery                                                            */
/* ================================================================== */

typedef struct ScanCtx {
    JceModLoader *ml;
    const char   *dir;     /* host directory being scanned (no trailing sep) */
    int           found;   /* count of NEW mods added                        */
} ScanCtx;

/* Join dir + leaf into out (forward-slash separated). */
static void join_path(char *out, size_t cap, const char *dir, const char *leaf) {
    size_t dl = dir ? strlen(dir) : 0;
    bool need_sep = dl > 0 && dir[dl - 1] != '/' && dir[dl - 1] != '\\';
    snprintf(out, cap, "%s%s%s", dir ? dir : "", need_sep ? "/" : "", leaf);
}

static bool scan_cb(const char *name, bool is_dir, void *user) {
    ScanCtx *ctx = (ScanCtx *)user;
    if (is_dir || !name || !name_is_mod_file(name)) return true;

    char full[1024];
    join_path(full, sizeof(full), ctx->dir, name);

    /* Idempotent: re-scanning must not clobber prior enable/order edits. */
    if (find_by_file(ctx->ml, full)) return true;

    JceArchive *ar = jce_archive_open_file(full);
    if (!ar) {
        LOG_WARN("jce_mod", "skipping unreadable mod archive: %s", full);
        return true;
    }

    /* Grow the record array. */
    JceModLoader *ml = ctx->ml;
    if (ml->count == ml->cap) {
        size_t ncap = ml->cap ? ml->cap * 2 : 4;
        ModRecord *nm = (ModRecord *)jce_realloc(ml->mods, ncap * sizeof(*nm));
        if (!nm) { jce_archive_close(ar); return true; }
        ml->mods = nm;
        ml->cap  = ncap;
    }

    ModRecord *rec = &ml->mods[ml->count];
    memset(rec, 0, sizeof(*rec));
    rec->archive    = ar;
    rec->file       = dup_str(full);
    rec->id         = stem_from_path(full);   /* default id = file stem      */
    rec->name       = dup_str(rec->id ? rec->id : "");
    rec->version    = dup_str("");
    rec->load_order = 0;
    rec->enabled    = true;
    rec->mounted    = false;

    /* Layer manifests: in-archive first, then sidecar overrides it. */
    load_inpak_manifest(rec);
    load_sidecar_manifest(rec);

    /* Manifest may have replaced id but not name → keep name in sync only if
     * the manifest never supplied an explicit one (name still equals stem). */
    if (rec->name && rec->id && rec->name[0] == '\0') {
        jce_free(rec->name);
        rec->name = dup_str(rec->id);
    }

    if (!rec->id || !rec->name || !rec->version || !rec->file) {
        mod_record_free(rec);
        return true;
    }

    /* Reject a duplicate id (different file, same id): keep the first. */
    bool dup = false;
    for (size_t i = 0; i < ml->count; ++i)
        if (ml->mods[i].id && strcmp(ml->mods[i].id, rec->id) == 0) { dup = true; break; }
    if (dup) {
        LOG_WARN("jce_mod", "duplicate mod id '%s' (%s) — ignoring", rec->id, full);
        mod_record_free(rec);
        return true;
    }

    ml->count++;
    ctx->found++;
    LOG_INFO("jce_mod", "discovered mod '%s' v%s (order=%d, %s)",
             rec->id, rec->version, rec->load_order,
             rec->enabled ? "enabled" : "disabled");
    return true;
}

int JCE_CALL jce_mod_loader_scan(JceModLoader *ml, const char *dir) {
    if (!ml) return -1;
    if (!dir || !dir[0] || !jce_fs_host_exists_dir(dir)) return 0;

    /* Normalize away any trailing separator for clean join. */
    char root[1024];
    snprintf(root, sizeof(root), "%s", dir);
    size_t rl = strlen(root);
    while (rl > 1 && (root[rl - 1] == '/' || root[rl - 1] == '\\'))
        root[--rl] = '\0';

    ScanCtx ctx = { ml, root, 0 };
    jce_fs_host_list_dir(root, scan_cb, &ctx);
    return ctx.found;
}

/* ================================================================== */
/* Ordering + queries                                                   */
/* ================================================================== */

/* Stable mount/display order: ascending load_order, ties broken by id, then
 * by file path (so equal ids — which scan rejects — still order). */
static int order_cmp(const void *pa, const void *pb) {
    const ModRecord *a = (const ModRecord *)pa;
    const ModRecord *b = (const ModRecord *)pb;
    if (a->load_order != b->load_order)
        return a->load_order < b->load_order ? -1 : 1;
    int c = strcmp(a->id ? a->id : "", b->id ? b->id : "");
    if (c) return c;
    return strcmp(a->file ? a->file : "", b->file ? b->file : "");
}

static void fill_info(const ModRecord *rec, JceModInfo *out) {
    out->id         = rec->id ? rec->id : "";
    out->name       = rec->name ? rec->name : "";
    out->version    = rec->version ? rec->version : "";
    out->file       = rec->file ? rec->file : "";
    out->load_order = rec->load_order;
    out->enabled    = rec->enabled;
    out->mounted    = rec->mounted;
}

size_t JCE_CALL jce_mod_loader_count(const JceModLoader *ml) {
    return ml ? ml->count : 0;
}

bool JCE_CALL jce_mod_loader_get(const JceModLoader *ml, size_t index,
                                 JceModInfo *out) {
    if (!ml || !out || index >= ml->count) return false;
    /* Present in effective (sorted) order without disturbing storage. */
    ModRecord *tmp = (ModRecord *)jce_malloc(ml->count * sizeof(*tmp));
    if (!tmp) { fill_info(&ml->mods[index], out); return true; }
    memcpy(tmp, ml->mods, ml->count * sizeof(*tmp));
    qsort(tmp, ml->count, sizeof(*tmp), order_cmp);
    fill_info(&tmp[index], out);
    jce_free(tmp);
    return true;
}

int JCE_CALL jce_mod_loader_find(const JceModLoader *ml, const char *id) {
    if (!ml || !id) return -1;
    for (size_t i = 0; i < ml->count; ++i)
        if (ml->mods[i].id && strcmp(ml->mods[i].id, id) == 0)
            return (int)i;
    return -1;
}

/* ================================================================== */
/* Enable / disable & load order                                        */
/* ================================================================== */

bool JCE_CALL jce_mod_loader_set_enabled(JceModLoader *ml, const char *id,
                                         bool enabled) {
    ModRecord *m = find_by_id(ml, id);
    if (!m) return false;
    m->enabled = enabled;
    return true;
}

bool JCE_CALL jce_mod_loader_is_enabled(const JceModLoader *ml, const char *id) {
    ModRecord *m = find_by_id((JceModLoader *)ml, id);
    return m ? m->enabled : false;
}

bool JCE_CALL jce_mod_loader_set_load_order(JceModLoader *ml, const char *id,
                                            int32_t order) {
    ModRecord *m = find_by_id(ml, id);
    if (!m) return false;
    m->load_order = order;
    return true;
}

int32_t JCE_CALL jce_mod_loader_get_load_order(const JceModLoader *ml,
                                               const char *id) {
    ModRecord *m = find_by_id((JceModLoader *)ml, id);
    return m ? m->load_order : 0;
}

/* ================================================================== */
/* Base content + mounting                                              */
/* ================================================================== */

void JCE_CALL jce_mod_loader_set_base(JceModLoader *ml, JceArchive *base) {
    if (!ml) return;
    ml->base = base;
}

int JCE_CALL jce_mod_loader_mount(JceModLoader *ml) {
    if (!ml) return -1;

    /* Rebuild from scratch so toggles / reorders take effect cleanly. */
    if (ml->mount) { jce_archive_mount_destroy(ml->mount); ml->mount = NULL; }
    for (size_t i = 0; i < ml->count; ++i) ml->mods[i].mounted = false;

    JceArchiveMount *m = jce_archive_mount_create();
    if (!m) return -1;

    int layers = 0;

    /* Base is the lowest-priority layer (added first). */
    if (ml->base) {
        if (jce_archive_mount_add(m, ml->base)) layers++;
    }

    /* Enabled mods in ascending load order: each subsequent add becomes a
     * higher-priority layer (jce_archive_mount_add appends to the top), so a
     * higher-order enabled mod overrides a lower-order one and the base. */
    if (ml->count > 0) {
        size_t *idx = (size_t *)jce_malloc(ml->count * sizeof(*idx));
        if (!idx) { jce_archive_mount_destroy(m); return -1; }
        for (size_t i = 0; i < ml->count; ++i) idx[i] = i;

        /* Insertion sort the index array by the same effective order_cmp. */
        for (size_t i = 1; i < ml->count; ++i) {
            size_t key = idx[i];
            size_t j = i;
            while (j > 0 && order_cmp(&ml->mods[idx[j - 1]],
                                      &ml->mods[key]) > 0) {
                idx[j] = idx[j - 1];
                --j;
            }
            idx[j] = key;
        }

        for (size_t k = 0; k < ml->count; ++k) {
            ModRecord *rec = &ml->mods[idx[k]];
            if (!rec->enabled || !rec->archive) continue;
            if (jce_archive_mount_add(m, rec->archive)) {
                rec->mounted = true;
                layers++;
            }
        }
        jce_free(idx);
    }

    ml->mount = m;
    return layers;
}

const JceArchiveMount *JCE_CALL jce_mod_loader_mount_handle(const JceModLoader *ml) {
    return ml ? ml->mount : NULL;
}

size_t JCE_CALL jce_mod_loader_mounted_count(const JceModLoader *ml) {
    if (!ml) return 0;
    size_t n = 0;
    for (size_t i = 0; i < ml->count; ++i)
        if (ml->mods[i].mounted) n++;
    return n;
}
