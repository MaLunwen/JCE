/* jce_bundle_pack.c — In-process scene-bundle packer.
 *
 * The host CLI (tools/jce_bundle_pack.c) is now a thin wrapper around
 * jce_bundle_pack_run() / jce_bundle_pack_diff() so the editor can do
 * the same packaging work in-process — keeping JCE shippable as a
 * single executable with no external toolchain.
 *
 * Failure handling: rather than calling exit() on OOM / IO errors (as
 * a CLI tool would), we longjmp out to the public entry point and
 * return a non-zero error code, leaving any background thread alive.
 */

#ifndef _WIN32
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include <jce/resource/jce_bundle_pack.h>
#include <jce/os/core/jce_filesystem.h>

#include "os/core/jce_memory.h"

#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  define MKDIR(p) mkdir((p), 0755)
#  define _strdup strdup
#endif

#include "resource/jce_pak_format.h"
#include <jce/resource/jce_bundle_format.h>
#include <jce/resource/jce_bundle_deps.h>

#include <cjson/cJSON.h>
#include <xxhash.h>
#include <zstd.h>

/* ================================================================== */
/* Per-call context — lets static helpers reach the log/jmp without    */
/* a second function argument on every call site.                      */
/* ================================================================== */

typedef struct PackCtx {
    JceBundlePackLogFn log_fn;
    void              *log_user;
    bool               quiet;
    jmp_buf            jmp;
    int                error_code; /* set before longjmp                */
} PackCtx;

/* Thread-local so concurrent jce_bundle_pack_run() calls (e.g. CLI on
 * one thread, editor on another) cannot stomp each other.  We fall
 * back to a plain static if the compiler does not support TLS — that's
 * still safe for the only realistic concurrency case (one worker per
 * editor instance + an optional CLI in a separate process). */
#if defined(_MSC_VER)
#  define PACK_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define PACK_TLS __thread
#else
#  define PACK_TLS
#endif

static PACK_TLS PackCtx *g_ctx = NULL;

static void pack_emit(JceBundlePackLogLevel level, const char *fmt, va_list ap)
{
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    /* Trim a single trailing newline — callbacks add their own. */
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
    if (g_ctx && g_ctx->log_fn) {
        if (g_ctx->quiet && level == JCE_BUNDLE_PACK_LOG_INFO) return;
        g_ctx->log_fn(level, buf, g_ctx->log_user);
    }
}

static void pack_log_info(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    pack_emit(JCE_BUNDLE_PACK_LOG_INFO, fmt, ap);
    va_end(ap);
}
static void pack_log_warn(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    pack_emit(JCE_BUNDLE_PACK_LOG_WARNING, fmt, ap);
    va_end(ap);
}
static void pack_log_err(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    pack_emit(JCE_BUNDLE_PACK_LOG_ERROR, fmt, ap);
    va_end(ap);
}
static void pack_log_ok(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    pack_emit(JCE_BUNDLE_PACK_LOG_SUCCESS, fmt, ap);
    va_end(ap);
}

#define LOG(...) pack_log_info(__VA_ARGS__)
#define ERR(...) pack_log_err(__VA_ARGS__)

/* Fatal → bail out of the run with a non-zero return code instead of
 * exit().  Must only be called inside a jce_bundle_pack_run /
 * jce_bundle_pack_diff invocation. */
static void die(const char *m)
{
    pack_log_err("%s", m);
    if (g_ctx) {
        g_ctx->error_code = -1;
        longjmp(g_ctx->jmp, 1);
    }
    /* Should never reach here; abort just in case. */
    abort();
}

/* ================================================================== */
/* Small dynamic vectors                                                */
/* ================================================================== */

typedef struct { uint8_t *d; size_t n, c; } Bytes;
static void bytes_reserve(Bytes *b, size_t need) {
    if (b->c >= need) return;
    size_t nc = b->c ? b->c : 256;
    while (nc < need) nc *= 2;
    b->d = (uint8_t *)JCE_REALLOC(b->d, nc);
    if (!b->d) die("oom");
    b->c = nc;
}
static void bytes_push(Bytes *b, uint8_t v) { bytes_reserve(b, b->n + 1); b->d[b->n++] = v; }
static void bytes_append(Bytes *b, const void *s, size_t n) {
    bytes_reserve(b, b->n + n); memcpy(b->d + b->n, s, n); b->n += n;
}
static void bytes_free(Bytes *b) { JCE_FREE(b->d); b->d = NULL; b->n = b->c = 0; }

static void le32(Bytes *b, uint32_t v) {
    bytes_push(b, (uint8_t)v); bytes_push(b, (uint8_t)(v >> 8));
    bytes_push(b, (uint8_t)(v >> 16)); bytes_push(b, (uint8_t)(v >> 24));
}
static void le64(Bytes *b, uint64_t v) {
    for (int i = 0; i < 8; ++i) bytes_push(b, (uint8_t)(v >> (i * 8)));
}

typedef struct { char **items; size_t n, c; } StrVec;
static void sv_push(StrVec *v, const char *s) {
    if (v->n == v->c) {
        v->c = v->c ? v->c * 2 : 16;
        v->items = (char **)JCE_REALLOC(v->items, v->c * sizeof(char *));
        if (!v->items) die("oom");
    }
    v->items[v->n++] = _strdup(s);
}
static void sv_free(StrVec *v) {
    for (size_t i = 0; i < v->n; ++i) JCE_FREE(v->items[i]);
    JCE_FREE(v->items); v->items = NULL; v->n = v->c = 0;
}
static int sv_contains(const StrVec *v, const char *s) {
    for (size_t i = 0; i < v->n; ++i) if (strcmp(v->items[i], s) == 0) return 1;
    return 0;
}

/* ================================================================== */
/* File I/O helpers                                                    */
/* ================================================================== */

static uint8_t *read_file(const char *path, size_t *out_size) {
    uint64_t sz = 0;
    void *vbuf = jce_fs_host_read_all(path, &sz);
    if (!vbuf) { *out_size = 0; return NULL; }
    uint8_t *buf = (uint8_t *)JCE_MALLOC((size_t)sz ? (size_t)sz : 1);
    if (!buf) { jce_fs_buffer_free(vbuf); *out_size = 0; return NULL; }
    if (sz) memcpy(buf, vbuf, (size_t)sz);
    jce_fs_buffer_free(vbuf);
    *out_size = (size_t)sz;
    return buf;
}

/* Read an asset by its virtual path, preferring the editor/runtime VFS so
 * assets coming from *any* mounted root (engine builtins, project, plus
 * arbitrary developer-added folders like `halloween-test/`) all get
 * gathered into one bundle.  Falls back to a flat <resource_root>/<vpath>
 * read, and finally to a caller-supplied resolver (typically the editor's
 * asset path index) that can locate any file the project has indexed.
 *
 * Buffer ownership is normalised to JCE_MALLOC() so the caller can JCE_FREE()
 * uniformly regardless of which path succeeded.
 */
typedef bool (*PackResolveFn)(const char *vpath, char *out, size_t outsz, void *u);

static uint8_t *read_asset(const char *vpath, const char *resource_root,
                           PackResolveFn resolve_fn, void *resolve_user,
                           size_t *out_size) {
    *out_size = 0;
    if (!vpath || !vpath[0]) return NULL;

    JceFileSystem *fs = jce_fs_get_active();
    if (fs) {
        uint64_t vsz = 0;
        void *vbuf = jce_fs_read_all(fs, vpath, &vsz);
        if (vbuf) {
            uint8_t *copy = (uint8_t *)JCE_MALLOC((size_t)vsz ? (size_t)vsz : 1);
            if (copy) {
                if (vsz) memcpy(copy, vbuf, (size_t)vsz);
                *out_size = (size_t)vsz;
                jce_fs_buffer_free(vbuf);
                return copy;
            }
            jce_fs_buffer_free(vbuf);
        }
    }
    if (resource_root && resource_root[0]) {
        char path[1280];
        snprintf(path, sizeof(path), "%s/%s", resource_root, vpath);
        uint8_t *buf = read_file(path, out_size);
        if (buf) return buf;
    }
    if (resolve_fn) {
        char abs[1280];
        if (resolve_fn(vpath, abs, sizeof(abs), resolve_user) && abs[0])
            return read_file(abs, out_size);
    }
    return NULL;
}

static int write_file(const char *path, const void *data, size_t size) {
    return jce_fs_host_write_all(path, data, (uint64_t)size) ? 1 : 0;
}

static void mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; ++i) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i]; tmp[i] = '\0';
            MKDIR(tmp);
            tmp[i] = c;
        }
    }
    MKDIR(tmp);
}

static void normalise_sep(char *s) { for (; *s; ++s) if (*s == '\\') *s = '/'; }

/* Recursive enumeration of *.scene.json under a directory. */
#ifdef _WIN32
static void walk_scenes(const char *dir, StrVec *out) {
    char pattern[MAX_PATH + 3];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char child[MAX_PATH];
        snprintf(child, sizeof(child), "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            walk_scenes(child, out);
        } else {
            size_t n = strlen(fd.cFileName);
            if (n > 11 && strcmp(fd.cFileName + n - 11, ".scene.json") == 0)
                sv_push(out, child);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#else
static void walk_scenes(const char *dir, StrVec *out) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t need = strlen(dir) + 1 + strlen(ent->d_name) + 1;
        char *child = (char *)JCE_MALLOC(need);
        if (!child) { closedir(d); die("oom"); }
        snprintf(child, need, "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) walk_scenes(child, out);
            else if (S_ISREG(st.st_mode)) {
                size_t n = strlen(ent->d_name);
                if (n > 11 && strcmp(ent->d_name + n - 11, ".scene.json") == 0)
                    sv_push(out, child);
            }
        }
        JCE_FREE(child);
    }
    closedir(d);
}
#endif

/* ================================================================== */
/* Asset-set → bundle association                                      */
/* ================================================================== */

typedef struct {
    char    *asset;
    char    *override;
    StrVec   refs;
} AssetRef;

typedef struct { AssetRef *items; size_t n, c; } AssetMap;

static AssetRef *am_find(AssetMap *m, const char *asset) {
    for (size_t i = 0; i < m->n; ++i)
        if (strcmp(m->items[i].asset, asset) == 0) return &m->items[i];
    return NULL;
}
static AssetRef *am_get_or_create(AssetMap *m, const char *asset) {
    AssetRef *r = am_find(m, asset);
    if (r) return r;
    if (m->n == m->c) {
        m->c = m->c ? m->c * 2 : 64;
        m->items = (AssetRef *)JCE_REALLOC(m->items, m->c * sizeof(AssetRef));
        if (!m->items) die("oom");
    }
    AssetRef *n = &m->items[m->n++];
    memset(n, 0, sizeof(*n));
    n->asset = _strdup(asset);
    return n;
}
static void am_free(AssetMap *m) {
    for (size_t i = 0; i < m->n; ++i) {
        JCE_FREE(m->items[i].asset);
        JCE_FREE(m->items[i].override);
        sv_free(&m->items[i].refs);
    }
    JCE_FREE(m->items); m->items = NULL; m->n = m->c = 0;
}

typedef struct {
    char  *id;
    char  *kind;
    char  *scene_path;     /* vpath inside the bundle (where the engine loads it) */
    char  *scene_src_path; /* absolute host path of the .scene.json (NULL for shared) */
    StrVec assets;
    StrVec deps;
} Bundle;

typedef struct { Bundle *items; size_t n, c; } BundleVec;

static Bundle *bv_get(BundleVec *v, const char *id) {
    for (size_t i = 0; i < v->n; ++i)
        if (strcmp(v->items[i].id, id) == 0) return &v->items[i];
    return NULL;
}
static Bundle *bv_create(BundleVec *v, const char *id, const char *kind) {
    if (v->n == v->c) {
        v->c = v->c ? v->c * 2 : 16;
        v->items = (Bundle *)JCE_REALLOC(v->items, v->c * sizeof(Bundle));
        if (!v->items) die("oom");
    }
    Bundle *b = &v->items[v->n++];
    memset(b, 0, sizeof(*b));
    b->id = _strdup(id);
    b->kind = _strdup(kind);
    return b;
}
static void bv_free(BundleVec *v) {
    for (size_t i = 0; i < v->n; ++i) {
        JCE_FREE(v->items[i].id);
        JCE_FREE(v->items[i].kind);
        JCE_FREE(v->items[i].scene_path);
        JCE_FREE(v->items[i].scene_src_path);
        sv_free(&v->items[i].assets);
        sv_free(&v->items[i].deps);
    }
    JCE_FREE(v->items); v->items = NULL; v->n = v->c = 0;
}

static void bundle_add_asset(Bundle *b, const char *path) {
    if (!sv_contains(&b->assets, path)) sv_push(&b->assets, path);
}
static void bundle_add_dep(Bundle *b, const char *dep) {
    if (strcmp(b->id, dep) == 0) return;
    if (!sv_contains(&b->deps, dep)) sv_push(&b->deps, dep);
}

/* ================================================================== */
/* Scene-id derivation                                                 */
/* ================================================================== */

/* Derive a stable bundle id for a scene file.
 *
 * Common case (project / selected-scenes-under-scenes_dir): the path
 * starts with `scenes_dir`, so we strip that prefix and the trailing
 * `.scene.json`, then replace path separators with `_`.
 *
 * Standalone case: when the scene lives OUTSIDE `scenes_dir` (selected
 * scenes / single-scene mode with a floating file), we fall back to
 * the basename minus `.scene.json` and append an 8-hex collision tag
 * derived from the full path so two files with the same basename in
 * different directories cannot clobber each other in the catalog. */
static char *make_scene_id(const char *full_path, const char *scenes_dir) {
    size_t blen = scenes_dir ? strlen(scenes_dir) : 0;
    int    inside = (blen > 0 && strncmp(full_path, scenes_dir, blen) == 0);

    if (inside) {
        const char *rel = full_path + blen;
        while (*rel == '/' || *rel == '\\') ++rel;
        char *out = _strdup(rel);
        if (!out) die("oom");
        size_t n = strlen(out);
        if (n > 11 && strcmp(out + n - 11, ".scene.json") == 0) out[n - 11] = '\0';
        for (char *p = out; *p; ++p) {
            if (*p == '\\' || *p == '/') *p = '_';
        }
        return out;
    }

    /* Floating scene: basename + collision tag. */
    const char *base = full_path;
    for (const char *p = full_path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    size_t bn = strlen(base);
    size_t stem = (bn > 11 && strcmp(base + bn - 11, ".scene.json") == 0)
                      ? bn - 11 : bn;
    uint64_t h = XXH3_64bits(full_path, strlen(full_path));
    char *out = (char *)JCE_MALLOC(stem + 1 /* '.' */ + 8 + 1);
    if (!out) die("oom");
    memcpy(out, base, stem);
    snprintf(out + stem, 10, ".%08x",
             (unsigned)(h ^ (h >> 32)));
    /* Sanitize characters that JSON keys/filenames dislike. */
    for (char *p = out; *p; ++p) {
        if (*p == '\\' || *p == '/' || *p == ':' || *p == ' ') *p = '_';
    }
    return out;
}

/* Derive the virtual path under which a scene is visible inside its
 * bundle.  When the scene file lives under `resource_root`, we use the
 * relative form; otherwise (floating scene) we use just the basename,
 * which is what `jce_bundle_loader_mount_file()` expects to find. */
static char *make_scene_vpath(const char *full_path, const char *resource_root) {
    size_t blen = resource_root ? strlen(resource_root) : 0;
    if (blen > 0 && strncmp(full_path, resource_root, blen) == 0) {
        const char *rel = full_path + blen;
        while (*rel == '/' || *rel == '\\') ++rel;
        char *out = _strdup(rel);
        if (!out) die("oom");
        normalise_sep(out);
        return out;
    }
    const char *base = full_path;
    for (const char *p = full_path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    char *out = _strdup(base);
    if (!out) die("oom");
    normalise_sep(out);
    return out;
}

/* ================================================================== */
/* JPAK v2 builder                                                      */
/* ================================================================== */

typedef struct {
    char    *vpath;
    uint8_t *raw;
    size_t   raw_size;
    uint8_t *compressed;
    size_t   compressed_size;
    uint64_t path_hash;
    uint64_t content_hash;
    uint32_t flags;
} PakEntry;

static int pak_entry_cmp(const void *a, const void *b) {
    uint64_t ha = ((const PakEntry *)a)->path_hash;
    uint64_t hb = ((const PakEntry *)b)->path_hash;
    return (ha < hb) ? -1 : (ha > hb) ? 1 : 0;
}

static int is_already_compressed(const char *p) {
    static const char *exts[] = {
        ".png", ".jpg", ".jpeg", ".webp", ".ktx", ".ktx2", ".basis", ".dds",
        ".ogg", ".mp3", ".opus", ".flac", ".aac", ".wav", ".m4a", ".mp4",
        ".webm", ".mkv", ".mov", ".avi", ".ivf", ".zip", ".7z", ".gz",
        ".zst", ".xz", ".bz2", ".pak", ".jceasset", ".heic", ".heif", ".avif",
        NULL
    };
    size_t n = strlen(p);
    for (size_t i = 0; exts[i]; ++i) {
        size_t e = strlen(exts[i]);
        if (n < e) continue;
        int eq = 1;
        for (size_t k = 0; k < e; ++k) {
            char c = p[n - e + k];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != exts[i][k]) { eq = 0; break; }
        }
        if (eq) return 1;
    }
    return 0;
}

static uint8_t *build_jbundle(PakEntry *entries, size_t count, int zstd_level,
                              size_t *out_size) {
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    if (!cctx) die("ZSTD_createCCtx");

    for (size_t i = 0; i < count; ++i) {
        PakEntry *e = &entries[i];
        e->path_hash    = XXH3_64bits(e->vpath, strlen(e->vpath));
        e->content_hash = XXH3_64bits(e->raw, e->raw_size);
        int store_raw   = is_already_compressed(e->vpath) || e->raw_size < 256;
        if (store_raw) {
            e->compressed      = (uint8_t *)JCE_MALLOC(e->raw_size ? e->raw_size : 1);
            if (!e->compressed) die("oom");
            if (e->raw_size) memcpy(e->compressed, e->raw, e->raw_size);
            e->compressed_size = e->raw_size;
            e->flags           = JPAK_FLAG_STORED;
        } else {
            size_t bound  = ZSTD_compressBound(e->raw_size);
            e->compressed = (uint8_t *)JCE_MALLOC(bound);
            if (!e->compressed) die("oom");
            size_t cs = ZSTD_compressCCtx(cctx, e->compressed, bound,
                                          e->raw, e->raw_size, zstd_level);
            if (ZSTD_isError(cs)) die("zstd");
            if (cs >= e->raw_size && e->raw_size > 0) {
                memcpy(e->compressed, e->raw, e->raw_size);
                e->compressed_size = e->raw_size;
                e->flags           = JPAK_FLAG_STORED;
            } else {
                e->compressed_size = cs;
                e->flags           = 0;
            }
        }
    }
    ZSTD_freeCCtx(cctx);

    qsort(entries, count, sizeof(PakEntry), pak_entry_cmp);

    uint32_t names_size = 0;
    uint32_t name_off[1024];
    if (count > 1024) die("bundle has more than 1024 entries (raise table)");
    for (size_t i = 0; i < count; ++i) {
        name_off[i] = names_size;
        names_size += (uint32_t)strlen(entries[i].vpath);
    }
    uint64_t data_cursor = 0;
    uint64_t data_off_per[1024];
    for (size_t i = 0; i < count; ++i) {
        data_off_per[i] = data_cursor;
        data_cursor += entries[i].compressed_size;
    }
    const uint64_t toc_offset = JPAK_HEADER_SIZE;
    const uint64_t toc_size   = (uint64_t)count * JPAK_TOC_ENTRY_SIZE;
    const uint64_t names_off  = toc_offset + toc_size;
    const uint64_t data_off   = names_off + names_size;
    const uint64_t total      = data_off + data_cursor;

    Bytes pak; memset(&pak, 0, sizeof(pak));
    bytes_reserve(&pak, (size_t)total);

    bytes_push(&pak, JPAK_MAGIC_0); bytes_push(&pak, JPAK_MAGIC_1);
    bytes_push(&pak, JPAK_MAGIC_2); bytes_push(&pak, JPAK_MAGIC_3);
    le32(&pak, JPAK_VERSION);
    le32(&pak, (uint32_t)count);
    le32(&pak, JPAK_CAP_OPT_BUNDLE);
    le64(&pak, toc_offset);
    le64(&pak, data_off);
    for (size_t i = 0; i < count; ++i) {
        const PakEntry *e = &entries[i];
        le64(&pak, e->path_hash);
        le32(&pak, (uint32_t)(names_off + name_off[i]));
        le32(&pak, (uint32_t)strlen(e->vpath));
        le64(&pak, data_off_per[i]);
        le64(&pak, e->compressed_size);
        le64(&pak, (uint64_t)e->raw_size);
        le32(&pak, e->flags);
        le32(&pak, 0);
        le64(&pak, e->content_hash);
    }
    for (size_t i = 0; i < count; ++i)
        bytes_append(&pak, entries[i].vpath, strlen(entries[i].vpath));
    for (size_t i = 0; i < count; ++i)
        bytes_append(&pak, entries[i].compressed, entries[i].compressed_size);

    *out_size = pak.n;
    return pak.d;
}

/* ================================================================== */
/* Manifest + catalog builders                                          */
/* ================================================================== */

static char *hex16(uint64_t h) {
    char *r = (char *)JCE_MALLOC(17);
    if (!r) die("oom");
    snprintf(r, 17, "%016llx", (unsigned long long)h);
    return r;
}
static char *short_hash(uint64_t h) {
    char *r = (char *)JCE_MALLOC(9);
    if (!r) die("oom");
    snprintf(r, 9, "%08x", (unsigned)(h ^ (h >> 32)));
    return r;
}

static cJSON *build_contract(const char *name, uint32_t major, uint32_t minor) {
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, JCE_BUNDLE_KEY_CONTRACT_NAME, name);
    cJSON_AddNumberToObject(c, JCE_BUNDLE_KEY_CONTRACT_MAJOR, major);
    cJSON_AddNumberToObject(c, JCE_BUNDLE_KEY_CONTRACT_MINOR, minor);
    return c;
}

static char *build_manifest(const Bundle *b, const PakEntry *entries,
                            size_t entry_count, uint64_t content_hash,
                            uint32_t version, size_t *out_len) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, JCE_BUNDLE_KEY_CONTRACT,
        build_contract(JCE_BUNDLE_MANIFEST_CONTRACT_NAME,
                       JCE_BUNDLE_MANIFEST_CONTRACT_MAJOR,
                       JCE_BUNDLE_MANIFEST_CONTRACT_MINOR));
    cJSON_AddStringToObject(root, JCE_BUNDLE_KEY_ID, b->id);
    cJSON_AddNumberToObject(root, JCE_BUNDLE_KEY_VERSION, version);
    char *hex = hex16(content_hash);
    cJSON_AddStringToObject(root, JCE_BUNDLE_KEY_CONTENT_HASH, hex);
    JCE_FREE(hex);
    cJSON_AddStringToObject(root, JCE_BUNDLE_KIND_KEY, b->kind);
    if (b->scene_path)
        cJSON_AddStringToObject(root, JCE_BUNDLE_KEY_SCENE_PATH, b->scene_path);

    cJSON *deps = cJSON_AddArrayToObject(root, JCE_BUNDLE_KEY_DEPENDS_ON);
    for (size_t i = 0; i < b->deps.n; ++i)
        cJSON_AddItemToArray(deps, cJSON_CreateString(b->deps.items[i]));

    cJSON *assets = cJSON_AddArrayToObject(root, JCE_BUNDLE_KEY_ASSETS);
    for (size_t i = 0; i < entry_count; ++i) {
        const PakEntry *e = &entries[i];
        if (strcmp(e->vpath, JCE_BUNDLE_MANIFEST_VPATH) == 0) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, JCE_BUNDLE_KEY_ASSET_PATH, e->vpath);
        cJSON_AddNumberToObject(o, JCE_BUNDLE_KEY_ASSET_SIZE, (double)e->raw_size);
        char *eh = hex16(e->content_hash);
        cJSON_AddStringToObject(o, JCE_BUNDLE_KEY_ASSET_HASH, eh);
        JCE_FREE(eh);
        cJSON_AddItemToArray(assets, o);
    }
    char *s = cJSON_Print(root);
    cJSON_Delete(root);
    *out_len = strlen(s);
    return s;
}

typedef struct {
    char *id;
    char *file;
    char *kind;
    char *scene_path;
    char *content_hash;
    uint64_t size;
    StrVec deps;
} CatalogEntry;

typedef struct { CatalogEntry *items; size_t n, c; } CatalogVec;

static CatalogEntry *cv_create(CatalogVec *v) {
    if (v->n == v->c) {
        v->c = v->c ? v->c * 2 : 16;
        v->items = (CatalogEntry *)JCE_REALLOC(v->items, v->c * sizeof(CatalogEntry));
        if (!v->items) die("oom");
    }
    CatalogEntry *e = &v->items[v->n++];
    memset(e, 0, sizeof(*e));
    return e;
}
static void cv_free(CatalogVec *v) {
    for (size_t i = 0; i < v->n; ++i) {
        JCE_FREE(v->items[i].id);
        JCE_FREE(v->items[i].file);
        JCE_FREE(v->items[i].kind);
        JCE_FREE(v->items[i].scene_path);
        JCE_FREE(v->items[i].content_hash);
        sv_free(&v->items[i].deps);
    }
    JCE_FREE(v->items); v->items = NULL; v->n = v->c = 0;
}

static char *build_catalog_json(const CatalogVec *cat, uint32_t version,
                                size_t *out_len) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, JCE_BUNDLE_KEY_CONTRACT,
        build_contract(JCE_BUNDLE_CATALOG_CONTRACT_NAME,
                       JCE_BUNDLE_CATALOG_CONTRACT_MAJOR,
                       JCE_BUNDLE_CATALOG_CONTRACT_MINOR));
    cJSON_AddNumberToObject(root, JCE_BUNDLE_CATALOG_KEY_VERSION, version);
    cJSON *bundles = cJSON_AddObjectToObject(root, JCE_BUNDLE_CATALOG_KEY_BUNDLES);
    for (size_t i = 0; i < cat->n; ++i) {
        const CatalogEntry *e = &cat->items[i];
        cJSON *o = cJSON_AddObjectToObject(bundles, e->id);
        cJSON_AddStringToObject(o, JCE_BUNDLE_CATALOG_KEY_FILE, e->file);
        cJSON_AddStringToObject(o, JCE_BUNDLE_CATALOG_KEY_KIND, e->kind);
        if (e->scene_path)
            cJSON_AddStringToObject(o, JCE_BUNDLE_CATALOG_KEY_SCENE,
                                    e->scene_path);
        cJSON_AddStringToObject(o, JCE_BUNDLE_CATALOG_KEY_HASH, e->content_hash);
        cJSON_AddNumberToObject(o, JCE_BUNDLE_CATALOG_KEY_SIZE, (double)e->size);
        cJSON *deps = cJSON_AddArrayToObject(o, JCE_BUNDLE_CATALOG_KEY_DEPS);
        for (size_t j = 0; j < e->deps.n; ++j)
            cJSON_AddItemToArray(deps, cJSON_CreateString(e->deps.items[j]));
    }
    char *s = cJSON_Print(root);
    cJSON_Delete(root);
    *out_len = strlen(s);
    return s;
}

static const char *prev_hash_lookup(const cJSON *prev_catalog, const char *id) {
    if (!prev_catalog) return NULL;
    const cJSON *bundles = cJSON_GetObjectItemCaseSensitive(prev_catalog,
                                JCE_BUNDLE_CATALOG_KEY_BUNDLES);
    if (!bundles) return NULL;
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(bundles, id);
    if (!e) return NULL;
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(e,
                                JCE_BUNDLE_CATALOG_KEY_HASH);
    return (h && cJSON_IsString(h)) ? h->valuestring : NULL;
}
static const char *prev_file_lookup(const cJSON *prev_catalog, const char *id) {
    if (!prev_catalog) return NULL;
    const cJSON *bundles = cJSON_GetObjectItemCaseSensitive(prev_catalog,
                                JCE_BUNDLE_CATALOG_KEY_BUNDLES);
    if (!bundles) return NULL;
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(bundles, id);
    if (!e) return NULL;
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(e,
                                JCE_BUNDLE_CATALOG_KEY_FILE);
    return (f && cJSON_IsString(f)) ? f->valuestring : NULL;
}

static char *load_text_file(const char *path, size_t *out_len) {
    size_t sz = 0;
    uint8_t *b = read_file(path, &sz);
    if (!b) return NULL;
    char *s = (char *)JCE_MALLOC(sz + 1);
    if (!s) { JCE_FREE(b); return NULL; }
    memcpy(s, b, sz); s[sz] = '\0';
    JCE_FREE(b);
    if (out_len) *out_len = sz;
    return s;
}

/* ================================================================== */
/* Diff implementation                                                 */
/* ================================================================== */

static int run_diff_impl(const char *old_path, const char *new_path,
                         const char *out_path) {
    size_t osz, nsz;
    uint8_t *ob = read_file(old_path, &osz);
    uint8_t *nb = read_file(new_path, &nsz);
    if (!ob || !nb) {
        ERR("failed to read diff inputs");
        JCE_FREE(ob); JCE_FREE(nb);
        return 1;
    }
    cJSON *oj = cJSON_ParseWithLength((char *)ob, osz);
    cJSON *nj = cJSON_ParseWithLength((char *)nb, nsz);
    JCE_FREE(ob); JCE_FREE(nb);
    if (!oj || !nj) {
        ERR("invalid catalog JSON");
        if (oj) cJSON_Delete(oj);
        if (nj) cJSON_Delete(nj);
        return 1;
    }

    const cJSON *ob_bundles = cJSON_GetObjectItemCaseSensitive(oj,
                                    JCE_BUNDLE_CATALOG_KEY_BUNDLES);
    const cJSON *nb_bundles = cJSON_GetObjectItemCaseSensitive(nj,
                                    JCE_BUNDLE_CATALOG_KEY_BUNDLES);

    cJSON *diff = cJSON_CreateObject();
    cJSON_AddItemToObject(diff, JCE_BUNDLE_KEY_CONTRACT,
        build_contract(JCE_BUNDLE_DIFF_CONTRACT_NAME,
                       JCE_BUNDLE_DIFF_CONTRACT_MAJOR,
                       JCE_BUNDLE_DIFF_CONTRACT_MINOR));
    const cJSON *ov = cJSON_GetObjectItemCaseSensitive(oj,
                                JCE_BUNDLE_CATALOG_KEY_VERSION);
    const cJSON *nv = cJSON_GetObjectItemCaseSensitive(nj,
                                JCE_BUNDLE_CATALOG_KEY_VERSION);
    cJSON_AddNumberToObject(diff, JCE_BUNDLE_DIFF_KEY_FROM_VERSION,
                            ov ? ov->valuedouble : 0);
    cJSON_AddNumberToObject(diff, JCE_BUNDLE_DIFF_KEY_TO_VERSION,
                            nv ? nv->valuedouble : 0);
    cJSON *added   = cJSON_AddArrayToObject(diff, JCE_BUNDLE_DIFF_KEY_ADDED);
    cJSON *updated = cJSON_AddArrayToObject(diff, JCE_BUNDLE_DIFF_KEY_UPDATED);
    cJSON *removed = cJSON_AddArrayToObject(diff, JCE_BUNDLE_DIFF_KEY_REMOVED);

    if (nb_bundles) {
        const cJSON *e = NULL;
        cJSON_ArrayForEach(e, nb_bundles) {
            const char *id = e->string;
            const char *old_hash = prev_hash_lookup(oj, id);
            const cJSON *new_hash = cJSON_GetObjectItemCaseSensitive(e,
                                        JCE_BUNDLE_CATALOG_KEY_HASH);
            if (!old_hash) {
                cJSON_AddItemToArray(added, cJSON_Duplicate(e, 1));
            } else if (!new_hash || !cJSON_IsString(new_hash) ||
                       strcmp(old_hash, new_hash->valuestring) != 0) {
                cJSON_AddItemToArray(updated, cJSON_Duplicate(e, 1));
            }
        }
    }
    if (ob_bundles) {
        const cJSON *e = NULL;
        cJSON_ArrayForEach(e, ob_bundles) {
            const char *id = e->string;
            if (!nb_bundles ||
                !cJSON_GetObjectItemCaseSensitive(nb_bundles, id))
                cJSON_AddItemToArray(removed, cJSON_CreateString(id));
        }
    }

    char *out = cJSON_Print(diff);
    int ok = write_file(out_path, out, strlen(out));
    JCE_FREE(out);
    cJSON_Delete(diff);
    cJSON_Delete(oj);
    cJSON_Delete(nj);
    if (!ok) { ERR("cannot write %s", out_path); return 1; }
    LOG("diff written: %s", out_path);
    return 0;
}

/* ================================================================== */
/* Build implementation — body of the former CLI main()                 */
/* ================================================================== */

static int run_build_impl(const JceBundlePackOptions *opts)
{
    /* Local mutable copies of the path inputs for normalisation. */
    char scenes_dir   [1024];
    char resource_root[1024];
    char out_dir      [1024];
    char prev_catalog [1024];
    char shared_id    [128];

    snprintf(scenes_dir,    sizeof(scenes_dir),    "%s",
             opts->scenes_dir    ? opts->scenes_dir    : "");
    snprintf(resource_root, sizeof(resource_root), "%s",
             opts->resource_root ? opts->resource_root : "");
    snprintf(out_dir,       sizeof(out_dir),       "%s",
             opts->out_dir       ? opts->out_dir       : "");
    snprintf(prev_catalog,  sizeof(prev_catalog),  "%s",
             opts->prev_catalog  ? opts->prev_catalog  : "");
    snprintf(shared_id,     sizeof(shared_id),     "%s",
             (opts->shared_id && opts->shared_id[0])
                 ? opts->shared_id
                 : JCE_BUNDLE_SHARED_DEFAULT_ID);

    /* ── Project-root convenience: derive any unset path from a single
     * project root.  Output convention is `<project>/.bundles/` —
     * a hidden directory in the Library/-style tradition.  Paired with
     * the runtime auto-mount in jce_engine.c this means a freshly
     * built project boots without any extra wiring. */
    if (opts->project_root && opts->project_root[0]) {
        if (!scenes_dir[0])
            snprintf(scenes_dir, sizeof(scenes_dir),
                     "%s", opts->project_root);
        if (!resource_root[0])
            snprintf(resource_root, sizeof(resource_root),
                     "%s", opts->project_root);
        if (!out_dir[0])
            snprintf(out_dir, sizeof(out_dir),
                     "%s/.bundles", opts->project_root);
        if (!prev_catalog[0])
            snprintf(prev_catalog, sizeof(prev_catalog),
                     "%s/.bundles/.prev/%s",
                     opts->project_root, JCE_BUNDLE_CATALOG_NAME);
    }

    /* ── Standalone / selected-scenes mode (v1.1) ─────────────────────
     * When scene_files[] is given, resource_root and out_dir may still
     * be unset.  Auto-derive sensible defaults from the first scene
     * file so that callers (e.g. an editor "Pack Current Scene" command)
     * can supply just the scene path. */
    const bool have_explicit_scenes =
        (opts->scene_files != NULL && opts->scene_file_count > 0);
    const bool single_file =
        opts->single_file_mode && have_explicit_scenes &&
        opts->scene_file_count == 1;

    if (have_explicit_scenes) {
        const char *first = opts->scene_files[0];
        if (!first || !first[0]) {
            ERR("scene_files[0] is empty");
            return 1;
        }
        /* Derive scene dir from first file's parent. */
        if (!scenes_dir[0]) {
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s", first);
            normalise_sep(buf);
            char *slash = strrchr(buf, '/');
            if (slash) {
                *slash = '\0';
                snprintf(scenes_dir, sizeof(scenes_dir), "%s", buf);
            } else {
                snprintf(scenes_dir, sizeof(scenes_dir), ".");
            }
        }
        if (!resource_root[0] && opts->auto_resource_root) {
            /* Walk up from the scene's directory looking for a folder
             * named `resources` or `assets`.  Stop at filesystem root. */
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s", scenes_dir);
            normalise_sep(buf);
            char best[1024] = {0};
            for (;;) {
                /* Check basename of current path. */
                const char *base = buf;
                for (const char *p = buf; *p; ++p)
                    if (*p == '/') base = p + 1;
                if (strcmp(base, "resources") == 0 ||
                    strcmp(base, "assets")    == 0) {
                    snprintf(best, sizeof(best), "%s", buf);
                    break;
                }
                /* Pop one segment. */
                char *slash = strrchr(buf, '/');
                if (!slash || slash == buf) break;
                *slash = '\0';
            }
            if (best[0])
                snprintf(resource_root, sizeof(resource_root), "%s", best);
            else
                snprintf(resource_root, sizeof(resource_root),
                         "%s", scenes_dir);
        } else if (!resource_root[0]) {
            /* No auto-detect: default to scenes_dir. */
            snprintf(resource_root, sizeof(resource_root),
                     "%s", scenes_dir);
        }
        if (!out_dir[0]) {
            snprintf(out_dir, sizeof(out_dir), "%s/.bundles", scenes_dir);
        }
        /* Single-file mode: never read or write the .prev cache, never
         * emit a catalog -> blank prev_catalog so the loader below does
         * not try to find/parse it. */
        if (single_file) {
            prev_catalog[0] = '\0';
        }
    }

    int shared_threshold = opts->shared_threshold > 0 ? opts->shared_threshold : 2;
    if (shared_threshold < 2) shared_threshold = 2;
    /* In single-file mode every asset must live in the one bundle —
     * disable the shared-bundle split entirely. */
    if (single_file) shared_threshold = INT_MAX;

    int zstd_level = opts->zstd_level > 0 ? opts->zstd_level : 3;
    if (zstd_level < 1 || zstd_level > 22) {
        ERR("zstd-level out of range");
        return 1;
    }

    if (!scenes_dir[0] || !resource_root[0] || !out_dir[0]) {
        ERR("scenes_dir / resource_root / out_dir are required "
            "(or pass project_root, or scene_files[])");
        return 1;
    }

    normalise_sep(scenes_dir);
    normalise_sep(resource_root);
    normalise_sep(out_dir);
    for (char *p = scenes_dir   + strlen(scenes_dir);   p > scenes_dir   && p[-1] == '/'; --p) p[-1] = '\0';
    for (char *p = resource_root + strlen(resource_root); p > resource_root && p[-1] == '/'; --p) p[-1] = '\0';
    for (char *p = out_dir       + strlen(out_dir);       p > out_dir       && p[-1] == '/'; --p) p[-1] = '\0';

    mkdir_p(out_dir);

    /* Ensure `.bundles/` is git-ignored.  Self-tracked .gitignore so
     * the file itself is preserved in commits but every artifact below
     * is excluded.  Idempotent: only writes if missing.  Skipped in
     * single-file mode — caller may write the .jbundle anywhere. */
    if (!single_file) {
        char gi[1100];
        snprintf(gi, sizeof(gi), "%s/.gitignore", out_dir);
        if (!jce_fs_host_exists_file(gi)) {
            static const char kGitignoreBody[] =
                "# Auto-generated by jce_bundle_pack — do not edit.\n"
                "# Bundle artifacts are build output; do not commit.\n"
                "*\n"
                "!.gitignore\n";
            (void)jce_fs_host_write_all(gi, kGitignoreBody,
                                        (uint64_t)(sizeof(kGitignoreBody) - 1));
        }
    }

    cJSON *prev_catalog_j = NULL;
    char prev_dir[1024] = {0};
    uint32_t prev_version = 0;
    if (prev_catalog[0]) {
        size_t plen;
        char *txt = load_text_file(prev_catalog, &plen);
        if (txt) {
            prev_catalog_j = cJSON_ParseWithLength(txt, plen);
            JCE_FREE(txt);
            if (prev_catalog_j) {
                const cJSON *v = cJSON_GetObjectItemCaseSensitive(
                    prev_catalog_j, JCE_BUNDLE_CATALOG_KEY_VERSION);
                if (v) prev_version = (uint32_t)v->valuedouble;
            }
            snprintf(prev_dir, sizeof(prev_dir), "%s", prev_catalog);
            for (size_t i = strlen(prev_dir); i > 0; --i) {
                if (prev_dir[i-1] == '/' || prev_dir[i-1] == '\\') {
                    prev_dir[i-1] = '\0'; break;
                }
            }
        }
    }
    uint32_t catalog_version = opts->catalog_version
                                 ? opts->catalog_version
                                 : prev_version + 1;
    if (catalog_version == 0) catalog_version = 1;

    StrVec scene_paths = {0};
    if (have_explicit_scenes) {
        /* Use the explicit list verbatim.  Validate existence + suffix;
         * silently skip duplicates so callers can be sloppy.  Empty
         * after filtering => log and continue (consistent with the
         * walk_scenes path that may also yield zero hits). */
        for (size_t i = 0; i < opts->scene_file_count; ++i) {
            const char *p = opts->scene_files[i];
            if (!p || !p[0]) continue;
            size_t n = strlen(p);
            if (n <= 11 || strcmp(p + n - 11, ".scene.json") != 0) {
                ERR("scene_files[%zu] does not end in .scene.json: %s",
                    i, p);
                continue;
            }
            if (!jce_fs_host_exists_file(p)) {
                ERR("scene_files[%zu] not found: %s", i, p);
                continue;
            }
            /* De-dup (small N expected; linear scan is fine). */
            char norm[1024];
            snprintf(norm, sizeof(norm), "%s", p);
            normalise_sep(norm);
            int dup = 0;
            for (size_t j = 0; j < scene_paths.n; ++j)
                if (strcmp(scene_paths.items[j], norm) == 0) { dup = 1; break; }
            if (!dup) sv_push(&scene_paths, norm);
        }
        LOG("packing %zu explicit scene(s)%s", scene_paths.n,
            single_file ? " (single-file mode)" : "");
    } else {
        walk_scenes(scenes_dir, &scene_paths);
        LOG("discovered %zu scene(s) under %s", scene_paths.n, scenes_dir);
    }

    if (scene_paths.n == 0) {
        LOG("no scenes; emitting empty catalog");
    }

    StrVec   scene_ids = {0};
    AssetMap am        = {0};

    for (size_t i = 0; i < scene_paths.n; ++i) {
        char *sid = NULL;
        /* Single-file mode: caller-supplied bundle id takes precedence. */
        if (single_file && opts->single_bundle_id &&
            opts->single_bundle_id[0]) {
            sid = _strdup(opts->single_bundle_id);
            if (!sid) die("oom");
        } else {
            sid = make_scene_id(scene_paths.items[i], scenes_dir);
        }
        sv_push(&scene_ids, sid);
        JCE_FREE(sid);
        const char *id_now = scene_ids.items[scene_ids.n - 1];

        JceBundleDepList deps = {0};
        if (!jce_bundle_deps_scan_file(scene_paths.items[i], &deps)) {
            ERR("cannot parse scene %s", scene_paths.items[i]);
            continue;
        }
        for (uint32_t k = 0; k < deps.count; ++k) {
            AssetRef *r = am_get_or_create(&am, deps.items[k].path);
            if (!sv_contains(&r->refs, id_now)) sv_push(&r->refs, id_now);
            if (deps.items[k].bundle && !r->override)
                r->override = _strdup(deps.items[k].bundle);
        }

        /* Depth-1..N recursion: a scene's first-level deps are usually
         * descriptors (materials / prefabs / OBJ meshes) that themselves
         * reference further assets.  Without this expansion the bundle
         * would ship the .mat.json but not the .png it names, or the
         * .obj but not the .mtl/.jpg the OBJ wires up via `mtllib` and
         * `map_*`.  We expand iteratively up to a small depth cap so
         * cycles can't run away.
         *
         * Three descriptor formats handled:
         *   - *.json   → cJSON walk via jce_bundle_deps_scan
         *   - *.obj    → parse `mtllib …` lines, push referenced MTLs
         *   - *.mtl    → parse `map_*` / `bump` / `disp` / `decal` / `refl`
         *                lines, push the texture paths
         *
         * Paths inside OBJ/MTL are typically relative to the file that
         * names them — we resolve them against that file's directory so
         * the bundle stores vpaths the runtime VFS can actually open. */
        {
            const int kMaxDepth = 6;
            size_t    start_idx = 0;

            /* Helper: push a vpath into `deps` if not already present.
             * Returns true if it was newly inserted. */
            #define PUSH_DEP_IF_NEW(VPATH, BUNDLE_TAG)                       \
                do {                                                         \
                    const char *_vp = (VPATH);                               \
                    if (!_vp || !_vp[0]) break;                              \
                    bool _dup = false;                                       \
                    for (uint32_t _e = 0; _e < deps.count; ++_e) {           \
                        if (deps.items[_e].path &&                           \
                            strcmp(deps.items[_e].path, _vp) == 0) {         \
                            _dup = true; break;                              \
                        }                                                    \
                    }                                                        \
                    if (!_dup) {                                             \
                        if (deps.count + 1 > deps.capacity) {                \
                            uint32_t _nc = deps.capacity ? deps.capacity*2 : 16; \
                            JceBundleDep *_g = (JceBundleDep *)JCE_REALLOC(      \
                                deps.items, _nc * sizeof(*_g));              \
                            if (!_g) break;                                  \
                            deps.items    = _g;                              \
                            deps.capacity = _nc;                             \
                        }                                                    \
                        JceBundleDep *_nd = &deps.items[deps.count++];       \
                        _nd->path   = _strdup(_vp);                          \
                        _nd->bundle = (BUNDLE_TAG)                           \
                                      ? _strdup(BUNDLE_TAG) : NULL;          \
                        AssetRef *_r = am_get_or_create(&am, _vp);           \
                        if (!sv_contains(&_r->refs, id_now))                 \
                            sv_push(&_r->refs, id_now);                      \
                        if ((BUNDLE_TAG) && !_r->override)                   \
                            _r->override = _strdup(BUNDLE_TAG);              \
                        grew = true;                                         \
                    }                                                        \
                } while (0)

            for (int depth = 0; depth < kMaxDepth; ++depth) {
                uint32_t expand_until = deps.count;
                if (start_idx >= expand_until) break;
                bool grew = false;
                for (uint32_t k = (uint32_t)start_idx; k < expand_until; ++k) {
                    const char *dp = deps.items[k].path;
                    if (!dp) continue;
                    size_t dn = strlen(dp);
                    if (dn < 4) continue;

                    bool is_json = (dn >= 5 && _stricmp(dp + dn - 5, ".json") == 0);
                    bool is_obj  = (dn >= 4 && _stricmp(dp + dn - 4, ".obj")  == 0);
                    bool is_mtl  = (dn >= 4 && _stricmp(dp + dn - 4, ".mtl")  == 0);
                    if (!is_json && !is_obj && !is_mtl) continue;

                    size_t   child_sz  = 0;
                    uint8_t *child_buf = read_asset(dp, resource_root,
                                                    opts->resolve_fn,
                                                    opts->resolve_user,
                                                    &child_sz);
                    if (!child_buf) continue;

                    /* Compute base dir of this descriptor for relative refs. */
                    char base_dir[1280];
                    snprintf(base_dir, sizeof(base_dir), "%s", dp);
                    {
                        char *bslash = strrchr(base_dir, '/');
                        if (!bslash) bslash = strrchr(base_dir, '\\');
                        if (bslash) *bslash = '\0';
                        else base_dir[0] = '\0';
                    }

                    if (is_json) {
                        JceBundleDepList sub = {0};
                        bool parsed = jce_bundle_deps_scan((const char *)child_buf,
                                                           child_sz, &sub);
                        if (parsed) {
                            for (uint32_t s = 0; s < sub.count; ++s) {
                                PUSH_DEP_IF_NEW(sub.items[s].path,
                                                sub.items[s].bundle);
                            }
                        }
                        jce_bundle_deps_free(&sub);
                    } else {
                        /* OBJ / MTL — text scan line by line. */
                        const char *cur = (const char *)child_buf;
                        const char *end = cur + child_sz;
                        char line[1280];
                        while (cur < end) {
                            const char *nl = (const char *)memchr(cur, '\n',
                                                                  (size_t)(end - cur));
                            size_t llen = nl ? (size_t)(nl - cur) : (size_t)(end - cur);
                            if (llen >= sizeof(line)) llen = sizeof(line) - 1;
                            memcpy(line, cur, llen);
                            line[llen] = '\0';
                            cur = nl ? nl + 1 : end;
                            /* Strip CR, leading whitespace, '#' comments. */
                            if (llen && line[llen - 1] == '\r') line[--llen] = '\0';
                            char *p = line;
                            while (*p == ' ' || *p == '\t') ++p;
                            if (*p == '#' || *p == '\0') continue;

                            /* Identify directive. */
                            char *kw_end = p;
                            while (*kw_end && *kw_end != ' ' && *kw_end != '\t')
                                ++kw_end;
                            size_t kw_len = (size_t)(kw_end - p);
                            char  *args   = kw_end;
                            while (*args == ' ' || *args == '\t') ++args;
                            if (!*args) continue;

                            bool emit = false;
                            if (is_obj) {
                                /* `mtllib a.mtl b.mtl …` */
                                emit = (kw_len == 6 && strncmp(p, "mtllib", 6) == 0);
                                /* OBJ can technically reference textures
                                 * directly via non-standard `usemtl` aliases;
                                 * we leave that to MTL scanning. */
                            } else {
                                /* MTL — most texture keywords start with
                                 * `map_`, plus standalone `bump`, `disp`,
                                 * `decal`, `refl`, `norm`. */
                                if (kw_len >= 4 && strncmp(p, "map_", 4) == 0)
                                    emit = true;
                                else if (kw_len == 4 && strncmp(p, "bump", 4) == 0)
                                    emit = true;
                                else if (kw_len == 4 && strncmp(p, "disp", 4) == 0)
                                    emit = true;
                                else if (kw_len == 5 && strncmp(p, "decal", 5) == 0)
                                    emit = true;
                                else if (kw_len == 4 && strncmp(p, "refl", 4) == 0)
                                    emit = true;
                                else if (kw_len == 4 && strncmp(p, "norm", 4) == 0)
                                    emit = true;
                            }
                            if (!emit) continue;

                            /* The path is the LAST whitespace-separated
                             * token: skip leading `-flag value …` pairs. */
                            char *tok      = args;
                            char *last_tok = NULL;
                            while (*tok) {
                                while (*tok == ' ' || *tok == '\t') ++tok;
                                if (!*tok) break;
                                last_tok = tok;
                                while (*tok && *tok != ' ' && *tok != '\t')
                                    ++tok;
                            }
                            if (!last_tok || !*last_tok) continue;

                            /* mtllib may list multiple files; loop tokens. */
                            if (is_obj) {
                                char *t = args;
                                while (*t) {
                                    while (*t == ' ' || *t == '\t') ++t;
                                    if (!*t) break;
                                    char *start_t = t;
                                    while (*t && *t != ' ' && *t != '\t') ++t;
                                    char saved = *t;
                                    *t = '\0';
                                    /* Normalise separators. */
                                    char norm[1280];
                                    snprintf(norm, sizeof(norm), "%s", start_t);
                                    for (char *q = norm; *q; ++q)
                                        if (*q == '\\') *q = '/';
                                    char joined[1536];
                                    if (norm[0] == '/' ||
                                        (norm[1] == ':' && norm[2] == '/')) {
                                        /* absolute */
                                        snprintf(joined, sizeof(joined), "%s", norm);
                                    } else if (base_dir[0]) {
                                        snprintf(joined, sizeof(joined),
                                                 "%s/%s", base_dir, norm);
                                    } else {
                                        snprintf(joined, sizeof(joined), "%s", norm);
                                    }
                                    PUSH_DEP_IF_NEW(joined, NULL);
                                    *t = saved;
                                }
                            } else {
                                char norm[1280];
                                snprintf(norm, sizeof(norm), "%s", last_tok);
                                for (char *q = norm; *q; ++q)
                                    if (*q == '\\') *q = '/';
                                char joined[1536];
                                if (norm[0] == '/' ||
                                    (norm[1] == ':' && norm[2] == '/')) {
                                    snprintf(joined, sizeof(joined), "%s", norm);
                                } else if (base_dir[0]) {
                                    snprintf(joined, sizeof(joined),
                                             "%s/%s", base_dir, norm);
                                } else {
                                    snprintf(joined, sizeof(joined), "%s", norm);
                                }
                                PUSH_DEP_IF_NEW(joined, NULL);
                            }
                        }
                    }
                    JCE_FREE(child_buf);
                }
                start_idx = expand_until;
                if (!grew) break;
            }

            #undef PUSH_DEP_IF_NEW
        }

        jce_bundle_deps_free(&deps);
    }

    BundleVec bundles = {0};
    for (size_t i = 0; i < scene_paths.n; ++i) {
        Bundle *b = bv_create(&bundles, scene_ids.items[i],
                              JCE_BUNDLE_KIND_SCENE);
        b->scene_path     = make_scene_vpath(scene_paths.items[i],
                                             resource_root);
        b->scene_src_path = _strdup(scene_paths.items[i]);
        if (!b->scene_src_path) die("oom");
    }

    for (size_t i = 0; i < am.n; ++i) {
        AssetRef *r = &am.items[i];
        const char *target_id = NULL;

        if (r->override) {
            if (strcmp(r->override, JCE_BUNDLE_TAG_FORCE_LOCAL) == 0) {
                for (size_t s = 0; s < r->refs.n; ++s) {
                    Bundle *b = bv_get(&bundles, r->refs.items[s]);
                    if (b) bundle_add_asset(b, r->asset);
                }
                continue;
            }
            if (strcmp(r->override, JCE_BUNDLE_TAG_FORCE_SHARED) == 0)
                target_id = shared_id;
            else
                target_id = r->override;
        } else if ((int)r->refs.n >= shared_threshold) {
            target_id = shared_id;
        } else if (r->refs.n == 1) {
            target_id = r->refs.items[0];
        } else {
            continue;
        }

        Bundle *b = bv_get(&bundles, target_id);
        if (!b) b = bv_create(&bundles, target_id, JCE_BUNDLE_KIND_SHARED);
        bundle_add_asset(b, r->asset);

        for (size_t s = 0; s < r->refs.n; ++s) {
            Bundle *sb = bv_get(&bundles, r->refs.items[s]);
            if (sb && strcmp(sb->id, target_id) != 0)
                bundle_add_dep(sb, target_id);
        }
    }

    CatalogVec catalog = {0};
    size_t total_bytes = 0;
    size_t reused_count = 0;

    for (size_t bi = 0; bi < bundles.n; ++bi) {
        Bundle *b = &bundles.items[bi];
        if (b->assets.n == 0 && !b->scene_path) continue;

        XXH3_state_t *xs = XXH3_createState();
        XXH3_64bits_reset(xs);
        if (b->scene_path)
            XXH3_64bits_update(xs, b->scene_path, strlen(b->scene_path));
        /* Fold scene file content into the hash so editing only the
         * scene JSON invalidates the cached bundle. */
        if (b->scene_src_path) {
            size_t ssz = 0;
            uint8_t *sraw = read_file(b->scene_src_path, &ssz);
            if (sraw) {
                uint64_t sh = XXH3_64bits(sraw, ssz);
                XXH3_64bits_update(xs, &sh, sizeof(sh));
                JCE_FREE(sraw);
            }
        }
        for (size_t i = 1; i < b->assets.n; ++i) {
            for (size_t j = i; j > 0 && strcmp(b->assets.items[j-1],
                                                b->assets.items[j]) > 0; --j) {
                char *t = b->assets.items[j];
                b->assets.items[j] = b->assets.items[j-1];
                b->assets.items[j-1] = t;
            }
        }
        for (size_t i = 0; i < b->assets.n; ++i) {
            XXH3_64bits_update(xs, b->assets.items[i],
                               strlen(b->assets.items[i]));
            size_t sz = 0;
            uint8_t *raw = read_asset(b->assets.items[i], resource_root,
                                      opts->resolve_fn, opts->resolve_user, &sz);
            if (raw) {
                uint64_t ch = XXH3_64bits(raw, sz);
                XXH3_64bits_update(xs, &ch, sizeof(ch));
                JCE_FREE(raw);
            }
        }
        uint64_t input_hash = XXH3_64bits_digest(xs);
        XXH3_freeState(xs);

        const char *prev_h = prev_hash_lookup(prev_catalog_j, b->id);
        const char *prev_f = prev_file_lookup(prev_catalog_j, b->id);
        char prev_h_str[17] = {0};
        snprintf(prev_h_str, sizeof(prev_h_str), "%016llx",
                 (unsigned long long)input_hash);

        char *short_h = short_hash(input_hash);
        char fname[256];
        if (single_file) {
            /* Stable, predictable filename so callers can re-deploy
             * without consulting a catalog.  Hash is still embedded in
             * the manifest content_hash for verification. */
            snprintf(fname, sizeof(fname), "%s%s",
                     b->id, JCE_BUNDLE_FILE_EXT);
        } else {
            snprintf(fname, sizeof(fname), "%s.%s%s", b->id, short_h,
                     JCE_BUNDLE_FILE_EXT);
        }
        JCE_FREE(short_h);

        char out_path[1280];
        snprintf(out_path, sizeof(out_path), "%s/%s", out_dir, fname);
        char sidecar_path[1280];
        snprintf(sidecar_path, sizeof(sidecar_path), "%s/%s.json",
                 out_dir, fname);

        if (prev_h && strcmp(prev_h, prev_h_str) == 0) {
            char src_path[1280];
            snprintf(src_path, sizeof(src_path), "%s/%s",
                     prev_dir, prev_f ? prev_f : fname);
            size_t sz = 0;
            uint8_t *blob = read_file(src_path, &sz);
            if (blob) {
                if (write_file(out_path, blob, sz)) {
                    char src_side[1280];
                    snprintf(src_side, sizeof(src_side), "%s.json", src_path);
                    size_t ssz = 0;
                    uint8_t *sblob = read_file(src_side, &ssz);
                    if (sblob) {
                        write_file(sidecar_path, sblob, ssz);
                        JCE_FREE(sblob);
                    }
                    CatalogEntry *ce = cv_create(&catalog);
                    ce->id   = _strdup(b->id);
                    ce->file = _strdup(fname);
                    ce->kind = _strdup(b->kind);
                    if (b->scene_path) ce->scene_path = _strdup(b->scene_path);
                    ce->content_hash = _strdup(prev_h_str);
                    ce->size = sz;
                    for (size_t d = 0; d < b->deps.n; ++d)
                        sv_push(&ce->deps, b->deps.items[d]);
                    total_bytes += sz;
                    reused_count++;
                    JCE_FREE(blob);
                    LOG("reused %s (%zu B)", fname, sz);
                    continue;
                }
                JCE_FREE(blob);
            }
        }

        /* Slots: [0]=manifest, [1]=scene file (if any), then deps. */
        size_t entry_count = b->assets.n + 2;
        PakEntry *entries = (PakEntry *)JCE_CALLOC(entry_count, sizeof(PakEntry));
        if (!entries) die("oom");
        size_t actual = 0;

        size_t manifest_slot = actual++;

        /* Embed the scene JSON itself.  Without this, a runtime that
         * mounts only this bundle (no master pak / loose-file fallback)
         * cannot resolve `scene_path` via the VFS.  Mirrors Unity's
         * AssetBundle scene-loading semantics. */
        if (b->scene_src_path && b->scene_path) {
            size_t ssz = 0;
            uint8_t *sraw = read_file(b->scene_src_path, &ssz);
            if (!sraw) {
                ERR("cannot read scene source %s for bundle %s",
                    b->scene_src_path, b->id);
            } else {
                entries[actual].vpath    = _strdup(b->scene_path);
                entries[actual].raw      = sraw;
                entries[actual].raw_size = ssz;
                actual++;
            }
        }

        for (size_t i = 0; i < b->assets.n; ++i) {
            size_t sz = 0;
            uint8_t *raw = read_asset(b->assets.items[i], resource_root,
                                      opts->resolve_fn, opts->resolve_user, &sz);
            if (!raw) {
                ERR("missing asset %s (referenced by bundle %s)",
                    b->assets.items[i], b->id);
                continue;
            }
            entries[actual].vpath    = _strdup(b->assets.items[i]);
            entries[actual].raw      = raw;
            entries[actual].raw_size = sz;
            actual++;
        }

        size_t mlen = 0;
        char *mtext = build_manifest(b, entries + 1, actual - 1,
                                     input_hash, catalog_version, &mlen);
        entries[manifest_slot].vpath    = _strdup(JCE_BUNDLE_MANIFEST_VPATH);
        entries[manifest_slot].raw      = (uint8_t *)mtext;
        entries[manifest_slot].raw_size = mlen;

        size_t pak_size = 0;
        uint8_t *pak = build_jbundle(entries, actual, zstd_level, &pak_size);

        if (!write_file(out_path, pak, pak_size)) {
            ERR("cannot write %s", out_path);
        } else {
            LOG("built %s (%zu B, %zu asset%s)", fname, pak_size,
                actual - 1, (actual - 1) == 1 ? "" : "s");
        }
        write_file(sidecar_path, mtext, mlen);

        CatalogEntry *ce = cv_create(&catalog);
        ce->id   = _strdup(b->id);
        ce->file = _strdup(fname);
        ce->kind = _strdup(b->kind);
        if (b->scene_path) ce->scene_path = _strdup(b->scene_path);
        ce->content_hash = _strdup(prev_h_str);
        ce->size = pak_size;
        for (size_t d = 0; d < b->deps.n; ++d)
            sv_push(&ce->deps, b->deps.items[d]);
        total_bytes += pak_size;

        for (size_t i = 0; i < actual; ++i) {
            JCE_FREE(entries[i].vpath);
            JCE_FREE(entries[i].raw);
            JCE_FREE(entries[i].compressed);
        }
        JCE_FREE(entries);
        JCE_FREE(pak);
    }

    int cat_ok = 1;
    if (single_file) {
        /* Single-file mode: no catalog.  Report the produced bundle. */
        if (catalog.n == 1) {
            pack_log_ok("packed %s -> %s (%.2f KB)",
                        catalog.items[0].id, catalog.items[0].file,
                        (double)total_bytes / 1024.0);
        } else if (catalog.n == 0) {
            ERR("single-file mode: no bundle produced");
            cat_ok = 0;
        }
    } else {
        size_t cjson_len = 0;
        char *cat_json = build_catalog_json(&catalog, catalog_version,
                                            &cjson_len);
        char cat_path[1280];
        snprintf(cat_path, sizeof(cat_path), "%s/%s",
                 out_dir, JCE_BUNDLE_CATALOG_NAME);
        cat_ok = write_file(cat_path, cat_json, cjson_len);
        if (!cat_ok) {
            ERR("cannot write %s", cat_path);
        } else {
            pack_log_ok("catalog v%u written: %s (%zu bundles, %.2f MB)",
                        catalog_version, cat_path, catalog.n,
                        (double)total_bytes / (1024.0 * 1024.0));
        }
        if (reused_count)
            LOG("incremental: reused %zu unchanged bundle(s)", reused_count);
        JCE_FREE(cat_json);
    }

    sv_free(&scene_paths);
    sv_free(&scene_ids);
    am_free(&am);
    bv_free(&bundles);
    cv_free(&catalog);
    if (prev_catalog_j) cJSON_Delete(prev_catalog_j);
    return cat_ok ? 0 : 1;
}

/* ================================================================== */
/* Public API                                                           */
/* ================================================================== */

JCE_API int jce_bundle_pack_run(const JceBundlePackOptions *opts,
                                JceBundlePackLogFn         log_fn,
                                void                      *log_user)
{
    if (!opts) return -1;
    PackCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.log_fn   = log_fn;
    ctx.log_user = log_user;
    ctx.quiet    = opts->quiet;
    PackCtx *prev = g_ctx;
    g_ctx = &ctx;
    int rc;
    if (setjmp(ctx.jmp) == 0) {
        rc = run_build_impl(opts);
    } else {
        rc = ctx.error_code ? ctx.error_code : -1;
    }
    g_ctx = prev;
    return rc;
}

JCE_API int jce_bundle_pack_diff(const char *old_catalog_path,
                                 const char *new_catalog_path,
                                 const char *out_diff_path,
                                 JceBundlePackLogFn log_fn,
                                 void              *log_user)
{
    if (!old_catalog_path || !new_catalog_path || !out_diff_path) return -1;
    PackCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.log_fn   = log_fn;
    ctx.log_user = log_user;
    PackCtx *prev = g_ctx;
    g_ctx = &ctx;
    int rc;
    if (setjmp(ctx.jmp) == 0) {
        rc = run_diff_impl(old_catalog_path, new_catalog_path, out_diff_path);
    } else {
        rc = ctx.error_code ? ctx.error_code : -1;
    }
    g_ctx = prev;
    return rc;
}
