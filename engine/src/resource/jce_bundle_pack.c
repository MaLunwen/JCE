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

#include <jce/resource/jce_bundle_pack.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_str.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_timer.h>

#include "os/core/jce_memory.h"

#include <limits.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "resource/jce_pak_format.h"
#include <jce/resource/jce_bundle_format.h>
#include <jce/resource/jce_bundle_deps.h>
#include <jce/resource/jce_archive_cook.h>

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

typedef struct { char **items; size_t n, c; } StrVec;
static void sv_push(StrVec *v, const char *s) {
    if (v->n == v->c) {
        v->c = v->c ? v->c * 2 : 16;
        v->items = (char **)JCE_REALLOC(v->items, v->c * sizeof(char *));
        if (!v->items) die("oom");
    }
    v->items[v->n++] = jce_strdup(s);
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
/* External-asset map — virtualises absolute-path deps into a portable */
/*  `_external/<8hex>_<basename>` vpath so a packed bundle is truly    */
/*  self-contained (no drive letters / project-root leakage).          */
/* ================================================================== */

#define JCE_BUNDLE_EXTERNAL_PREFIX "_external/"

typedef struct {
    char *abs;    /* original on-disk path the user authored (absolute) */
    char *vpath;  /* portable in-bundle vpath ("_external/<hash>_<base>") */
} ExternalEntry;

typedef struct {
    ExternalEntry *items;
    size_t         n, c;
} ExternalMap;

static void ext_free(ExternalMap *m) {
    if (!m) return;
    for (size_t i = 0; i < m->n; ++i) {
        JCE_FREE(m->items[i].abs);
        JCE_FREE(m->items[i].vpath);
    }
    JCE_FREE(m->items);
    m->items = NULL; m->n = m->c = 0;
}

/* Follow the substitution chain: Phase 4 maps abs→vpath1; Phase 5 maps
 * vpath1→vpath2.  A single call must resolve the full chain so that
 * D:/foo.obj → _external/foo.obj → _external/foo.glb collapses to
 * _external/foo.glb in one shot.  Cap at 8 hops to prevent cycles. */
static const char *ext_lookup_vpath(const ExternalMap *m, const char *abs) {
    if (!m || !abs) return NULL;
    const char *result = NULL;
    const char *key = abs;
    for (int hops = 0; hops < 8; ++hops) {
        const char *next = NULL;
        for (size_t i = 0; i < m->n; ++i) {
            if (strcmp(m->items[i].abs, key) == 0) {
                next = m->items[i].vpath;
                break;
            }
        }
        if (!next || next == result) break;
        result = next;
        key    = next; /* follow the chain */
    }
    return result;
}

/* Reverse chain: given a final vpath (e.g. _external/foo.glb), walk
 * backwards through substitution entries until we reach a real on-disk
 * absolute path that read_file() can open.  Cap at 8 hops. */
static const char *ext_lookup_abs(const ExternalMap *m, const char *vpath) {
    if (!m || !vpath) return NULL;
    const char *key = vpath;
    for (int hops = 0; hops < 8; ++hops) {
        const char *found = NULL;
        for (size_t i = 0; i < m->n; ++i) {
            if (strcmp(m->items[i].vpath, key) == 0) {
                found = m->items[i].abs;
                break;
            }
        }
        if (!found) return NULL;
        /* If `found` is itself a vpath key in the map, follow the chain. */
        bool is_key = false;
        for (size_t i = 0; i < m->n; ++i) {
            if (strcmp(m->items[i].vpath, found) == 0) { is_key = true; break; }
        }
        if (!is_key) return found; /* terminal abs path */
        key = found;
    }
    return NULL; /* chain too long or circular */
}

/* Compose the portable vpath for an absolute path.  We hash the FULL
 * abspath (so cross-drive `tex.png` collisions become distinct), then
 * append the basename so a human eyeballing the PAK can still recognise
 * what it was.  Characters that JSON / VFS dislike are sanitised. */
static char *ext_make_vpath(const char *abs) {
    if (!abs || !*abs) return NULL;
    const char *base = abs;
    for (const char *p = abs; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    if (!*base) base = "file";

    uint64_t h = XXH3_64bits(abs, strlen(abs));
    /* sizeof "_external/" + 8 hex + '_' + basename + NUL */
    size_t blen = strlen(base);
    size_t need = sizeof(JCE_BUNDLE_EXTERNAL_PREFIX) + 8 + 1 + blen + 1;
    char *out = (char *)JCE_MALLOC(need);
    if (!out) die("oom");
    snprintf(out, need, JCE_BUNDLE_EXTERNAL_PREFIX "%08x_%s",
             (unsigned)(h ^ (h >> 32)), base);
    /* Sanitise: vpath uses '/' as separator only — strip any others. */
    for (char *p = out; *p; ++p) {
        if (*p == '\\' || *p == ':') *p = '_';
    }
    return out;
}

/* Insert (abs -> vpath) into `m` if not already present.  Returns the
 * (possibly cached) vpath; caller must NOT free it. */
static const char *ext_register(ExternalMap *m, const char *abs) {
    const char *v = ext_lookup_vpath(m, abs);
    if (v) return v;
    if (m->n == m->c) {
        m->c = m->c ? m->c * 2 : 16;
        m->items = (ExternalEntry *)JCE_REALLOC(m->items,
                                                m->c * sizeof(ExternalEntry));
        if (!m->items) die("oom");
    }
    ExternalEntry *e = &m->items[m->n++];
    e->abs   = jce_strdup(abs);
    e->vpath = ext_make_vpath(abs);
    if (!e->abs || !e->vpath) die("oom");
    return e->vpath;
}

/* Rewrite every JSON string in the tree rooted at `root` if its value
 * matches an external-mapped source path.  Iterative (heap-allocated
 * worklist) so deeply-nested scene JSONs cannot overflow the worker
 * thread's stack. */
static void ext_rewrite_cjson(cJSON *root, const ExternalMap *m) {
    if (!root || !m || m->n == 0) return;
    cJSON **stack = NULL;
    size_t  n = 0, c = 0;
    #define PUSH(p) do { \
        if (!(p)) break; \
        if (n == c) { \
            c = c ? c * 2 : 64; \
            cJSON **g = (cJSON **)JCE_REALLOC(stack, c * sizeof(*g)); \
            if (!g) { JCE_FREE(stack); return; } \
            stack = g; \
        } \
        stack[n++] = (p); \
    } while (0)
    PUSH(root);
    while (n > 0) {
        cJSON *node = stack[--n];
        if (cJSON_IsString(node) && node->valuestring) {
            const char *vp = ext_lookup_vpath(m, node->valuestring);
            if (vp) cJSON_SetValuestring(node, vp);
        }
        for (cJSON *child = node->child; child; child = child->next)
            PUSH(child);
    }
    #undef PUSH
    JCE_FREE(stack);
}

/* Replace every occurrence of any mapped absolute path in `src` with
 * its portable vpath.  Allocates a fresh JCE_MALLOC buffer; *out_size
 * receives the new byte count (no NUL).  Returns NULL on OOM.
 *
 * Used for OBJ / MTL text bodies — strncmp-based, so we match on the
 * exact char-for-char abspath the user authored.  Vpaths registered in
 * the map are always shorter than (or close to) the originals because
 * we strip drive letters, so the output buffer is sized as input * 2
 * for safety. */
static uint8_t *ext_rewrite_text(const uint8_t *src, size_t src_sz,
                                 const ExternalMap *m, size_t *out_size) {
    if (!src) { if (out_size) *out_size = 0; return NULL; }
    if (!m || m->n == 0) {
        uint8_t *copy = (uint8_t *)JCE_MALLOC(src_sz ? src_sz : 1);
        if (!copy) return NULL;
        if (src_sz) memcpy(copy, src, src_sz);
        if (out_size) *out_size = src_sz;
        return copy;
    }
    /* Worst-case: every byte expands into the longest vpath we have. */
    size_t max_vp = 0;
    for (size_t i = 0; i < m->n; ++i) {
        size_t vl = strlen(m->items[i].vpath);
        if (vl > max_vp) max_vp = vl;
    }
    size_t cap = src_sz + 1 + (max_vp + 16) * m->n;
    uint8_t *out = (uint8_t *)JCE_MALLOC(cap);
    if (!out) return NULL;
    size_t op = 0;
    size_t ip = 0;
    while (ip < src_sz) {
        bool matched = false;
        for (size_t i = 0; i < m->n; ++i) {
            const char *abs = m->items[i].abs;
            size_t alen     = strlen(abs);
            if (alen == 0 || ip + alen > src_sz) continue;
            if (memcmp(src + ip, abs, alen) != 0) continue;
            const char *vp = m->items[i].vpath;
            size_t vlen    = strlen(vp);
            if (op + vlen >= cap) {
                cap = (op + vlen + 1) * 2;
                uint8_t *grow = (uint8_t *)JCE_REALLOC(out, cap);
                if (!grow) { JCE_FREE(out); return NULL; }
                out = grow;
            }
            memcpy(out + op, vp, vlen);
            op += vlen;
            ip += alen;
            matched = true;
            break;
        }
        if (!matched) out[op++] = src[ip++];
    }
    if (out_size) *out_size = op;
    return out;
}

/* Convenience: rewrite a JSON text buffer by parsing → walking →
 * re-serialising.  Falls back to the textual replacer on parse failure
 * so we never silently ship un-rewritten content. */
static uint8_t *ext_rewrite_json(const uint8_t *src, size_t src_sz,
                                 const ExternalMap *m, size_t *out_size) {
    if (!src || !m || m->n == 0)
        return ext_rewrite_text(src, src_sz, m, out_size);
    cJSON *j = cJSON_ParseWithLength((const char *)src, src_sz);
    if (!j) return ext_rewrite_text(src, src_sz, m, out_size);
    ext_rewrite_cjson(j, m);
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) return ext_rewrite_text(src, src_sz, m, out_size);
    size_t tlen = strlen(txt);
    uint8_t *out = (uint8_t *)JCE_MALLOC(tlen ? tlen : 1);
    if (!out) { cJSON_free(txt); return NULL; }
    memcpy(out, txt, tlen);
    cJSON_free(txt);
    if (out_size) *out_size = tlen;
    return out;
}

/* Dispatch by vpath extension: JSON-like (.json / .scene.json / .gltf /
 * .mat.json / etc.) goes through cJSON; OBJ / MTL go through the
 * line-safe textual replacer; anything else is copied verbatim. */
static uint8_t *ext_rewrite_asset(const char *vpath,
                                  uint8_t *raw, size_t raw_sz,
                                  const ExternalMap *m,
                                  size_t *out_size)
{
    if (!raw) { if (out_size) *out_size = 0; return NULL; }
    if (!m || m->n == 0) { if (out_size) *out_size = raw_sz; return raw; }
    size_t vn = vpath ? strlen(vpath) : 0;
    bool is_json = (vn >= 5 && jce_strcasecmp(vpath + vn - 5, ".json") == 0) ||
                   (vn >= 5 && jce_strcasecmp(vpath + vn - 5, ".gltf") == 0) ||
                   (vn >= 6 && jce_strcasecmp(vpath + vn - 6, ".scene") == 0);
    bool is_obj  = (vn >= 4 && jce_strcasecmp(vpath + vn - 4, ".obj")  == 0);
    bool is_mtl  = (vn >= 4 && jce_strcasecmp(vpath + vn - 4, ".mtl")  == 0);
    if (!is_json && !is_obj && !is_mtl) {
        if (out_size) *out_size = raw_sz;
        return raw;
    }
    size_t nsz = 0;
    uint8_t *rew = is_json ? ext_rewrite_json(raw, raw_sz, m, &nsz)
                           : ext_rewrite_text(raw, raw_sz, m, &nsz);
    if (!rew) { if (out_size) *out_size = raw_sz; return raw; }
    JCE_FREE(raw);
    if (out_size) *out_size = nsz;
    return rew;
}

/* ================================================================== */
/* Phase 5 — Bundle-time mesh-to-glb conversion                        */
/*                                                                     */
/* Non-glTF source meshes (.obj, .fbx, .dae, .3ds, .ply, .stl, ...)    */
/* are coerced into binary glTF (.glb) at pack time so the runtime     */
/* only needs cgltf.  Conversion runs from inside this packer TU via   */
/* `jce_bundle_convert_to_glb` (an Assimp-backed C ABI exposed by      */
/* `jce_bundle_mesh_convert.cpp`).                                     */
/* ================================================================== */

extern int jce_bundle_convert_to_glb(const uint8_t *src, size_t src_sz,
                                     const char *ext_hint,
                                     uint8_t **out_buf, size_t *out_size);

typedef struct {
    char *final_vpath; /* what the bundle ships (".glb") */
    char *src_vpath;   /* what to read_asset() for source bytes */
    char *src_ext;     /* format hint for Assimp ("obj", "fbx", ...) */
} MeshEntry;

typedef struct {
    MeshEntry *items;
    size_t     n, c;
} MeshMap;

static void mesh_free(MeshMap *m) {
    if (!m) return;
    for (size_t i = 0; i < m->n; ++i) {
        JCE_FREE(m->items[i].final_vpath);
        JCE_FREE(m->items[i].src_vpath);
        JCE_FREE(m->items[i].src_ext);
    }
    JCE_FREE(m->items);
    m->items = NULL; m->n = m->c = 0;
}

static const MeshEntry *mesh_lookup(const MeshMap *m, const char *final_vp) {
    if (!m || !final_vp) return NULL;
    for (size_t i = 0; i < m->n; ++i)
        if (strcmp(m->items[i].final_vpath, final_vp) == 0)
            return &m->items[i];
    return NULL;
}

static void mesh_register(MeshMap *m, const char *final_vp,
                          const char *src_vp, const char *src_ext) {
    if (!m || !final_vp || !src_vp) return;
    if (mesh_lookup(m, final_vp)) return;
    if (m->n == m->c) {
        m->c = m->c ? m->c * 2 : 16;
        m->items = (MeshEntry *)JCE_REALLOC(m->items,
                                            m->c * sizeof(MeshEntry));
        if (!m->items) die("oom");
    }
    MeshEntry *e = &m->items[m->n++];
    e->final_vpath = jce_strdup(final_vp);
    e->src_vpath   = jce_strdup(src_vp);
    e->src_ext     = src_ext ? jce_strdup(src_ext) : NULL;
    if (!e->final_vpath || !e->src_vpath) die("oom");
}

/* Returns the Assimp format hint string ("obj", "fbx", ...) if this
 * vpath has a supported source-mesh extension, else NULL.  glTF and
 * GLB pass through (NULL) because no conversion is needed. */
static const char *mesh_convertible_ext(const char *vpath) {
    if (!vpath) return NULL;
    const char *dot = strrchr(vpath, '.');
    if (!dot) return NULL;
    if (jce_strcasecmp(dot, ".obj")   == 0) return "obj";
    if (jce_strcasecmp(dot, ".fbx")   == 0) return "fbx";
    if (jce_strcasecmp(dot, ".dae")   == 0) return "dae";
    if (jce_strcasecmp(dot, ".3ds")   == 0) return "3ds";
    if (jce_strcasecmp(dot, ".ply")   == 0) return "ply";
    if (jce_strcasecmp(dot, ".stl")   == 0) return "stl";
    if (jce_strcasecmp(dot, ".blend") == 0) return "blend";
    if (jce_strcasecmp(dot, ".x")     == 0) return "x";
    return NULL;
}

/* Returns a fresh JCE_MALLOC'd copy of `vp` with its extension
 * replaced by ".glb".  Caller frees with JCE_FREE. */
static char *mesh_dst_vpath(const char *vp) {
    if (!vp) return NULL;
    const char *dot = strrchr(vp, '.');
    size_t stem_len = dot ? (size_t)(dot - vp) : strlen(vp);
    size_t need = stem_len + 5; /* ".glb" + NUL */
    char *out = (char *)JCE_MALLOC(need);
    if (!out) die("oom");
    memcpy(out, vp, stem_len);
    memcpy(out + stem_len, ".glb", 5);
    return out;
}

/* Generic substitution registration: teach the rewrite machinery that
 * any string-literal reference to `old_str` (in scene JSON / MTL /
 * glTF / etc.) should be replaced by `new_str`.  Re-uses the
 * ExternalMap storage because the rewrite helpers already key off
 * (items[i].abs -> items[i].vpath). */
static void emap_register_substitution(ExternalMap *m,
                                       const char *old_str,
                                       const char *new_str) {
    if (!m || !old_str || !new_str) return;
    if (ext_lookup_vpath(m, old_str)) return;
    if (m->n == m->c) {
        m->c = m->c ? m->c * 2 : 16;
        m->items = (ExternalEntry *)JCE_REALLOC(m->items,
                                                m->c * sizeof(ExternalEntry));
        if (!m->items) die("oom");
    }
    ExternalEntry *e = &m->items[m->n++];
    e->abs   = jce_strdup(old_str);
    e->vpath = jce_strdup(new_str);
    if (!e->abs || !e->vpath) die("oom");
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
                           const ExternalMap *emap,
                           size_t *out_size) {
    *out_size = 0;
    if (!vpath || !vpath[0]) return NULL;

    /* External-map fast path: portable vpaths (`_external/<hash>_<base>`)
     * are not really in any VFS — they exist only inside the bundle we
     * are building.  Resolve to the on-disk source path so the binary
     * content lands in the PAK under the portable vpath. */
    if (emap) {
        const char *abs = ext_lookup_abs(emap, vpath);
        if (abs) return read_file(abs, out_size);
    }

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
        jce_path_join(path, sizeof(path), resource_root, vpath);
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
    jce_fs_host_create_directory(path);
}

static void normalise_sep(char *s) { for (; *s; ++s) if (*s == '\\') *s = '/'; }

/* Recursive enumeration of *.scene.json under a directory.
   Implemented on top of jce_fs_host_walk so no platform-specific
   directory API leaks into resource layer. */
static bool walk_scenes_cb(const char *path, bool is_dir, void *user) {
    if (is_dir) return true;
    size_t n = strlen(path);
    if (n > 11 && strcmp(path + n - 11, ".scene.json") == 0)
        sv_push((StrVec *)user, path);
    return true;
}

static void walk_scenes(const char *dir, StrVec *out) {
    jce_fs_host_walk(dir, walk_scenes_cb, out);
}

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
    n->asset = jce_strdup(asset);
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
    b->id = jce_strdup(id);
    b->kind = jce_strdup(kind);
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
        char *out = jce_strdup(rel);
        if (!out) die("oom");
        size_t n = strlen(out);
        if (n > 11 && strcmp(out + n - 11, ".scene.json") == 0) out[n - 11] = '\0';
        else if (n > 6 && strcmp(out + n - 6, ".scene") == 0)   out[n -  6] = '\0';
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
    size_t stem = bn;
    if (bn > 11 && strcmp(base + bn - 11, ".scene.json") == 0) stem = bn - 11;
    else if (bn > 6 && strcmp(base + bn - 6, ".scene") == 0)   stem = bn -  6;
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
        char *out = jce_strdup(rel);
        if (!out) die("oom");
        normalise_sep(out);
        return out;
    }
    const char *base = full_path;
    for (const char *p = full_path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    char *out = jce_strdup(base);
    if (!out) die("oom");
    normalise_sep(out);
    return out;
}

/* ================================================================== */
/* JPAK v1 bundle builder                                              */
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

/* Build the on-disk bundle container.  The bundle rides on the JPAK v1
 * archive format (spec) just like the engine paks; the cook routine
 * classifies entries, trains shared dictionaries and applies the
 * keep-if-helps guard.  Bundles are recognized at runtime by the
 * presence of the manifest entry (JCE_BUNDLE_MANIFEST_VPATH), not by a
 * header flag, so no bundle-specific capability bit is needed. */
static uint8_t *build_jbundle(PakEntry *entries, size_t count, int zstd_level,
                              size_t *out_size) {
    JceCookInput *inputs =
        (JceCookInput *)JCE_MALLOC((count ? count : 1) * sizeof(JceCookInput));
    if (!inputs) die("oom");
    for (size_t i = 0; i < count; ++i) {
        entries[i].content_hash = XXH3_64bits(entries[i].raw, entries[i].raw_size);
        inputs[i].vpath = entries[i].vpath;
        inputs[i].data  = entries[i].raw;
        inputs[i].size  = entries[i].raw_size;
    }

    JceCookConfig cfg = {0};
    cfg.zstd_level       = zstd_level;
    cfg.alignment_log2   = 4;
    cfg.mmap_friendly    = false;
    cfg.emit_debug_paths = true;
    cfg.compress_index   = true;
    cfg.use_dict         = true;
    cfg.dedup_content    = true;

    void  *blob      = NULL;
    size_t blob_size = 0;
    if (!jce_archive_cook(inputs, count, &cfg, &blob, &blob_size, NULL))
        die("archive cook failed");
    JCE_FREE(inputs);

    *out_size = blob_size;
    return (uint8_t *)blob;
}

/* ================================================================== */
/* Manifest + catalog builders                                          */
/* ================================================================== */

/* cJSON_Print() allocates via cJSON's own hooks.  Callers in this TU
 * release the result with JCE_FREE (mimalloc), so without this round-
 * trip the post-build cleanup hits mimalloc with a foreign pointer and
 * corrupts the heap.  Copy into a JCE_MALLOC'd buffer and release the
 * original with cJSON_free so heap-of-origin stays consistent. */
static char *cjson_to_jce_string(cJSON *root, size_t *out_len) {
    char *s = cJSON_Print(root);
    cJSON_Delete(root);
    if (!s) { if (out_len) *out_len = 0; return NULL; }
    size_t slen = strlen(s);
    char *out = (char *)JCE_MALLOC(slen + 1);
    if (!out) { cJSON_free(s); if (out_len) *out_len = 0; return NULL; }
    memcpy(out, s, slen + 1);
    cJSON_free(s);
    if (out_len) *out_len = slen;
    return out;
}

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
    return cjson_to_jce_string(root, out_len);
}

/* Per-asset record captured for the editor build report.  Kept on the
 * CatalogEntry so the post-build report writer can walk one structure
 * regardless of whether the bundle was freshly built or reused from
 * the incremental cache (in which case we rehydrate from the sidecar). */
typedef struct {
    char    *path;
    char    *hash;
    uint64_t size;
} ReportEntry;

typedef struct { ReportEntry *items; size_t n, c; } ReportEntryVec;

static ReportEntry *rv_create(ReportEntryVec *v) {
    if (v->n == v->c) {
        v->c = v->c ? v->c * 2 : 16;
        v->items = (ReportEntry *)JCE_REALLOC(v->items, v->c * sizeof(ReportEntry));
        if (!v->items) die("oom");
    }
    ReportEntry *e = &v->items[v->n++];
    memset(e, 0, sizeof(*e));
    return e;
}
static void rv_free(ReportEntryVec *v) {
    for (size_t i = 0; i < v->n; ++i) {
        JCE_FREE(v->items[i].path);
        JCE_FREE(v->items[i].hash);
    }
    JCE_FREE(v->items); v->items = NULL; v->n = v->c = 0;
}

typedef struct {
    char *id;
    char *file;
    char *kind;
    char *scene_path;
    char *content_hash;
    uint64_t size;
    StrVec deps;
    ReportEntryVec entries;
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
        rv_free(&v->items[i].entries);
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
    return cjson_to_jce_string(root, out_len);
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
/* Build report (P3-A.4)                                                */
/* ================================================================== */

/* Filename written next to bundle_catalog.json after a full build.
 * Consumed by editor "Build Report" panel.  Schema version is encoded
 * in the `$schema` field; bump when changing keys. */
#define JCE_BUILD_REPORT_NAME    "build_report.json"
#define JCE_BUILD_REPORT_SCHEMA  "jce.buildreport.v1"

/* Best-effort asset-type classification from extension.  Used purely for
 * the editor's filter/grouping affordances — runtime never reads this. */
static const char *report_guess_type(const char *path) {
    const char *dot = NULL;
    for (const char *p = path; *p; ++p) if (*p == '.') dot = p;
    if (!dot) return "other";
    char ext[16]; size_t n = 0;
    for (const char *p = dot + 1; *p && n + 1 < sizeof(ext); ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        ext[n++] = c;
    }
    ext[n] = '\0';
    size_t pl = strlen(path);
    if (pl >= 11 && strcmp(path + pl - 11, ".scene.json") == 0)
        return "scene";
    if (pl >=  6 && strcmp(path + pl -  6, ".scene")      == 0)
        return "scene";
    if (strcmp(ext, "gltf") == 0 || strcmp(ext, "glb")  == 0 ||
        strcmp(ext, "fbx")  == 0 || strcmp(ext, "obj")  == 0 ||
        strcmp(ext, "mesh") == 0)                          return "mesh";
    if (strcmp(ext, "png")  == 0 || strcmp(ext, "jpg")  == 0 ||
        strcmp(ext, "jpeg") == 0 || strcmp(ext, "tga")  == 0 ||
        strcmp(ext, "bmp")  == 0 || strcmp(ext, "hdr")  == 0 ||
        strcmp(ext, "ktx")  == 0 || strcmp(ext, "ktx2") == 0 ||
        strcmp(ext, "basis")== 0 || strcmp(ext, "dds")  == 0 ||
        strcmp(ext, "webp") == 0)                          return "texture";
    if (strcmp(ext, "wav")  == 0 || strcmp(ext, "ogg")  == 0 ||
        strcmp(ext, "mp3")  == 0 || strcmp(ext, "opus") == 0 ||
        strcmp(ext, "flac") == 0)                          return "audio";
    if (strcmp(ext, "mp4")  == 0 || strcmp(ext, "webm") == 0 ||
        strcmp(ext, "mkv")  == 0 || strcmp(ext, "ivf")  == 0)
        return "video";
    if (strcmp(ext, "ttf")  == 0 || strcmp(ext, "otf")  == 0)
        return "font";
    if (strcmp(ext, "bin")  == 0 || strcmp(ext, "sc")   == 0)
        return "shader";
    if (strcmp(ext, "json") == 0)                          return "json";
    return ext[0] ? ext : "other";
}

static void iso8601_utc_now(char *buf, size_t n) {
    jce_time_format_utc((int64_t)time(NULL), "%Y-%m-%dT%H:%M:%SZ", buf, n);
}

/* Build the build_report.json document.  Returns malloc'd string;
 * caller frees with JCE_FREE().  *out_len receives strlen. */
static char *build_report_json(const CatalogVec *cat, size_t *out_len) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "$schema", JCE_BUILD_REPORT_SCHEMA);
    char ts[32]; iso8601_utc_now(ts, sizeof(ts));
    cJSON_AddStringToObject(root, "timestamp", ts);

    uint64_t total_bytes = 0;
    size_t   unique_assets = 0;

    /* Duplicate detection: hash -> { size, bundle ids[] }.  cJSON
     * map of `hash -> object` is the most compact representation
     * here; flattened to an array at the end. */
    cJSON *hash_map = cJSON_CreateObject();

    cJSON *bundles = cJSON_AddArrayToObject(root, "bundles");
    for (size_t i = 0; i < cat->n; ++i) {
        const CatalogEntry *e = &cat->items[i];
        cJSON *bo = cJSON_CreateObject();
        cJSON_AddStringToObject(bo, "name", e->id);
        cJSON_AddStringToObject(bo, "file", e->file);
        cJSON_AddNumberToObject(bo, "size_bytes", (double)e->size);
        cJSON_AddNumberToObject(bo, "entry_count", (double)e->entries.n);
        total_bytes += e->size;

        cJSON *deps = cJSON_AddArrayToObject(bo, "dependencies");
        for (size_t d = 0; d < e->deps.n; ++d)
            cJSON_AddItemToArray(deps, cJSON_CreateString(e->deps.items[d]));

        cJSON *ents = cJSON_AddArrayToObject(bo, "entries");
        for (size_t k = 0; k < e->entries.n; ++k) {
            const ReportEntry *re = &e->entries.items[k];
            cJSON *eo = cJSON_CreateObject();
            cJSON_AddStringToObject(eo, "path", re->path ? re->path : "");
            cJSON_AddNumberToObject(eo, "size_bytes", (double)re->size);
            cJSON_AddStringToObject(eo, "hash", re->hash ? re->hash : "");
            cJSON_AddStringToObject(eo, "type",
                report_guess_type(re->path ? re->path : ""));
            cJSON_AddItemToArray(ents, eo);

            if (re->hash && re->hash[0]) {
                cJSON *slot = cJSON_GetObjectItemCaseSensitive(hash_map, re->hash);
                if (!slot) {
                    slot = cJSON_CreateObject();
                    cJSON_AddNumberToObject(slot, "size_bytes", (double)re->size);
                    cJSON_AddArrayToObject(slot, "in_bundles");
                    cJSON_AddItemToObject(hash_map, re->hash, slot);
                    ++unique_assets;
                }
                cJSON *arr = cJSON_GetObjectItemCaseSensitive(slot, "in_bundles");
                cJSON_AddItemToArray(arr, cJSON_CreateString(e->id));
            }
        }
        cJSON_AddItemToArray(bundles, bo);
    }

    cJSON *dups = cJSON_AddArrayToObject(root, "duplicates");
    cJSON *slot = NULL;
    cJSON_ArrayForEach(slot, hash_map) {
        const cJSON *arr = cJSON_GetObjectItemCaseSensitive(slot, "in_bundles");
        if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < 2) continue;
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "hash", slot->string);
        const cJSON *sz = cJSON_GetObjectItemCaseSensitive(slot, "size_bytes");
        cJSON_AddNumberToObject(d, "size_bytes",
                                sz ? sz->valuedouble : 0.0);
        cJSON *dub = cJSON_AddArrayToObject(d, "in_bundles");
        const cJSON *bid;
        cJSON_ArrayForEach(bid, arr)
            cJSON_AddItemToArray(dub, cJSON_Duplicate(bid, 1));
        cJSON_AddItemToArray(dups, d);
    }
    cJSON_Delete(hash_map);

    cJSON *totals = cJSON_AddObjectToObject(root, "totals");
    cJSON_AddNumberToObject(totals, "bundle_count", (double)cat->n);
    cJSON_AddNumberToObject(totals, "total_size_bytes", (double)total_bytes);
    cJSON_AddNumberToObject(totals, "unique_asset_count",
                            (double)unique_assets);

    return cjson_to_jce_string(root, out_len);
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
            int ok_json  = (n > 11 && strcmp(p + n - 11, ".scene.json") == 0);
            int ok_scene = (n >  6 && strcmp(p + n -  6, ".scene")      == 0);
            if (!ok_json && !ok_scene) {
                ERR("scene_files[%zu] does not end in .scene or .scene.json: %s",
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
            sid = jce_strdup(opts->single_bundle_id);
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
                r->override = jce_strdup(deps.items[k].bundle);
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
                        _nd->path   = jce_strdup(_vp);                          \
                        _nd->bundle = (BUNDLE_TAG)                           \
                                      ? jce_strdup(BUNDLE_TAG) : NULL;          \
                        AssetRef *_r = am_get_or_create(&am, _vp);           \
                        if (!sv_contains(&_r->refs, id_now))                 \
                            sv_push(&_r->refs, id_now);                      \
                        if ((BUNDLE_TAG) && !_r->override)                   \
                            _r->override = jce_strdup(BUNDLE_TAG);              \
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

                    bool is_json = (dn >= 5 && jce_strcasecmp(dp + dn - 5, ".json") == 0);
                    bool is_obj  = (dn >= 4 && jce_strcasecmp(dp + dn - 4, ".obj")  == 0);
                    bool is_mtl  = (dn >= 4 && jce_strcasecmp(dp + dn - 4, ".mtl")  == 0);
                    if (!is_json && !is_obj && !is_mtl) continue;

                    size_t   child_sz  = 0;
                    uint8_t *child_buf = read_asset(dp, resource_root,
                                                    opts->resolve_fn,
                                                    opts->resolve_user,
                                                    NULL,
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

    /* ── External-asset virtualisation ────────────────────────────────
     *
     * After dep expansion, `am` may contain absolute on-disk paths
     * (e.g. a texture the user dragged from `D:/photos/foo.png` into a
     * material slot, or an OBJ at `E:/meshes/x.obj` whose `mtllib`
     * resolved to `E:/meshes/x.mtl`).  Storing those as PAK vpaths
     * would leak drive letters and absolute project layout into the
     * bundle, breaking portability.
     *
     * We sweep `am` once: each absolute key is registered in the
     * `emap` and renamed in-place to a portable
     * `_external/<8hex>_<basename>` vpath.  All later passes
     * (per-bundle assignment, hash, write, manifest) see only portable
     * vpaths.  `read_asset()` consults `emap` to recover the source
     * file when an `_external/` vpath needs its bytes read.
     *
     * Scene / OBJ / MTL / glTF descriptor bodies are textual and may
     * still embed the original absolute strings — those get rewritten
     * just before being emitted to the PAK (see `rewrite_*_external`
     * below). */
    ExternalMap emap = {0};
    for (size_t i = 0; i < am.n; ++i) {
        AssetRef *r = &am.items[i];
        if (!r->asset || !*r->asset) continue;
        if (!jce_path_is_absolute(r->asset)) continue;
        const char *vp = ext_register(&emap, r->asset);
        if (!vp) continue;
        if (strcmp(vp, r->asset) == 0) continue;
        LOG("external asset virtualised: %s -> %s", r->asset, vp);
        char *new_key = jce_strdup(vp);
        if (!new_key) die("oom");
        JCE_FREE(r->asset);
        r->asset = new_key;
    }

    /* ── Mesh-to-glb conversion sweep (Phase 5) ──────────────────────
     *
     * Any asset whose extension is a supported source-mesh format
     * (.obj/.fbx/.dae/...) is renamed in-place to the same path with
     * a `.glb` extension; the original vpath is recorded in `mcmap`
     * (final → src) so the write loop knows to fetch the source bytes
     * and run them through Assimp.  A substitution is also recorded
     * in `emap` so any scene JSON / MTL / glTF reference to the old
     * name gets transparently rewritten to the new `.glb` name. */
    MeshMap mcmap = {0};
    for (size_t i = 0; i < am.n; ++i) {
        AssetRef *r = &am.items[i];
        if (!r->asset || !*r->asset) continue;
        const char *hint = mesh_convertible_ext(r->asset);
        if (!hint) continue;
        char *dst = mesh_dst_vpath(r->asset);
        if (!dst) continue;
        if (strcmp(dst, r->asset) == 0) { JCE_FREE(dst); continue; }
        mesh_register(&mcmap, dst, r->asset, hint);
        emap_register_substitution(&emap, r->asset, dst);
        LOG("mesh converted (bundle-time): %s -> %s", r->asset, dst);
        JCE_FREE(r->asset);
        r->asset = dst; /* takes ownership */
    }

    BundleVec bundles = {0};
    for (size_t i = 0; i < scene_paths.n; ++i) {
        Bundle *b = bv_create(&bundles, scene_ids.items[i],
                              JCE_BUNDLE_KIND_SCENE);
        b->scene_path     = make_scene_vpath(scene_paths.items[i],
                                             resource_root);
        b->scene_src_path = jce_strdup(scene_paths.items[i]);
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
        /* Packer pipeline version — bump when conversion logic changes so
         * previously-cached bundles are unconditionally rebuilt. */
        static const uint32_t PACKER_VERSION = 3; /* custom GLB writer */
        XXH3_64bits_update(xs, &PACKER_VERSION, sizeof(PACKER_VERSION));
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
                                      opts->resolve_fn, opts->resolve_user,
                                      &emap, &sz);
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
        jce_path_join(out_path, sizeof(out_path), out_dir, fname);
        char sidecar_path[1280];
        snprintf(sidecar_path, sizeof(sidecar_path), "%s/%s.json",
                 out_dir, fname);

        if (prev_h && strcmp(prev_h, prev_h_str) == 0) {
            char src_path[1280];
            jce_path_join(src_path, sizeof(src_path),
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
                    }
                    CatalogEntry *ce = cv_create(&catalog);
                    ce->id   = jce_strdup(b->id);
                    ce->file = jce_strdup(fname);
                    ce->kind = jce_strdup(b->kind);
                    if (b->scene_path) ce->scene_path = jce_strdup(b->scene_path);
                    ce->content_hash = jce_strdup(prev_h_str);
                    ce->size = sz;
                    for (size_t d = 0; d < b->deps.n; ++d)
                        sv_push(&ce->deps, b->deps.items[d]);
                    /* Rehydrate report entries from the reused sidecar so
                     * the build report is consistent regardless of cache
                     * hits.  Best-effort: if parsing fails we still ship
                     * the bundle row with zero entries. */
                    if (sblob) {
                        cJSON *sj = cJSON_ParseWithLength(
                            (const char *)sblob, ssz);
                        if (sj) {
                            const cJSON *assets = cJSON_GetObjectItemCaseSensitive(
                                sj, JCE_BUNDLE_KEY_ASSETS);
                            if (cJSON_IsArray(assets)) {
                                const cJSON *ae;
                                cJSON_ArrayForEach(ae, assets) {
                                    const cJSON *ap = cJSON_GetObjectItemCaseSensitive(ae, JCE_BUNDLE_KEY_ASSET_PATH);
                                    const cJSON *az = cJSON_GetObjectItemCaseSensitive(ae, JCE_BUNDLE_KEY_ASSET_SIZE);
                                    const cJSON *ah = cJSON_GetObjectItemCaseSensitive(ae, JCE_BUNDLE_KEY_ASSET_HASH);
                                    if (!(ap && cJSON_IsString(ap))) continue;
                                    ReportEntry *re = rv_create(&ce->entries);
                                    re->path = jce_strdup(ap->valuestring);
                                    re->size = (az && cJSON_IsNumber(az)) ? (uint64_t)az->valuedouble : 0;
                                    re->hash = (ah && cJSON_IsString(ah)) ? jce_strdup(ah->valuestring) : jce_strdup("");
                                }
                            }
                            cJSON_Delete(sj);
                        }
                        JCE_FREE(sblob);
                    }
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
                size_t   rsz  = 0;
                uint8_t *rraw = ext_rewrite_asset(b->scene_path, sraw, ssz,
                                                  &emap, &rsz);
                entries[actual].vpath    = jce_strdup(b->scene_path);
                entries[actual].raw      = rraw ? rraw : sraw;
                entries[actual].raw_size = rraw ? rsz : ssz;
                actual++;
            }
        }

        for (size_t i = 0; i < b->assets.n; ++i) {
            const char *final_vp = b->assets.items[i];
            const MeshEntry *me = mesh_lookup(&mcmap, final_vp);

            size_t   sz  = 0;
            uint8_t *raw = NULL;
            if (me) {
                /* Read the SOURCE bytes (the .obj/.fbx/...) via the
                 * normal resolver chain; emap will fast-path external
                 * sources back to disk. */
                raw = read_asset(me->src_vpath, resource_root,
                                 opts->resolve_fn, opts->resolve_user,
                                 &emap, &sz);
            } else {
                raw = read_asset(final_vp, resource_root,
                                 opts->resolve_fn, opts->resolve_user,
                                 &emap, &sz);
            }
            if (!raw) {
                ERR("missing asset %s (referenced by bundle %s)",
                    final_vp, b->id);
                continue;
            }

            if (me) {
                /* Cache key — hash the SOURCE bytes (post-VFS, so an
                 * identical .obj used by N projects shares one cache
                 * entry).  Fold in the format hint so two formats with
                 * the same byte fingerprint (theoretical) stay
                 * distinct.  MESH_CONVERTER_VERSION must be bumped
                 * whenever `jce_bundle_convert_to_glb` logic changes
                 * (new writer, meshopt upgrade, etc.) so stale cached
                 * GLBs are automatically discarded on the next pack. */
                static const uint32_t MESH_CONVERTER_VERSION = 3; /* always cache-optimise indices */
                uint64_t key = XXH3_64bits(raw, sz);
                key ^= (uint64_t)MESH_CONVERTER_VERSION << 32;
                if (me->src_ext && *me->src_ext) {
                    key ^= XXH3_64bits(me->src_ext, strlen(me->src_ext));
                }
                char cache_dir [1400];
                char cache_path[1536];
                snprintf(cache_dir,  sizeof(cache_dir),  "%s/.mesh_cache",
                         out_dir);
                snprintf(cache_path, sizeof(cache_path),
                         "%s/%016" PRIx64 ".glb", cache_dir, key);

                uint8_t *glb_buf = NULL;
                size_t   glb_sz  = 0;

                /* Cache lookup. */
                uint8_t *cached = read_file(cache_path, &glb_sz);
                if (cached) {
                    glb_buf = cached;
                    LOG("mesh cache hit:  %s (%zu B) <- %s",
                        final_vp, glb_sz, cache_path);
                } else {
                    if (!jce_bundle_convert_to_glb(raw, sz, me->src_ext,
                                                   &glb_buf, &glb_sz)) {
                        ERR("mesh conversion failed: %s (hint=%s) for bundle %s",
                            me->src_vpath,
                            me->src_ext ? me->src_ext : "auto",
                            b->id);
                        JCE_FREE(raw);
                        continue;
                    }
                    mkdir_p(cache_dir);
                    if (write_file(cache_path, glb_buf, glb_sz)) {
                        LOG("mesh cache store: %s (%zu B) -> %s",
                            final_vp, glb_sz, cache_path);
                    }
                }
                JCE_FREE(raw);
                raw = glb_buf;
                sz  = glb_sz;
                /* .glb is a binary container — DO NOT run the textual /
                 * cJSON rewriter over it.  Any embedded references
                 * Assimp generates were already against the (in-memory)
                 * source and the runtime will resolve them from the
                 * bundle's vpath table. */
                entries[actual].vpath    = jce_strdup(final_vp);
                entries[actual].raw      = raw;
                entries[actual].raw_size = sz;
                actual++;
                continue;
            }

            size_t   rsz  = 0;
            uint8_t *rraw = ext_rewrite_asset(final_vp, raw, sz,
                                              &emap, &rsz);
            entries[actual].vpath    = jce_strdup(final_vp);
            entries[actual].raw      = rraw;
            entries[actual].raw_size = rsz;
            actual++;
        }

        size_t mlen = 0;
        char *mtext = build_manifest(b, entries + 1, actual - 1,
                                     input_hash, catalog_version, &mlen);
        entries[manifest_slot].vpath    = jce_strdup(JCE_BUNDLE_MANIFEST_VPATH);
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
        ce->id   = jce_strdup(b->id);
        ce->file = jce_strdup(fname);
        ce->kind = jce_strdup(b->kind);
        if (b->scene_path) ce->scene_path = jce_strdup(b->scene_path);
        ce->content_hash = jce_strdup(prev_h_str);
        ce->size = pak_size;
        for (size_t d = 0; d < b->deps.n; ++d)
            sv_push(&ce->deps, b->deps.items[d]);
        /* Capture per-asset entries for the build report.  Skip the
         * manifest pseudo-entry — it's an implementation detail of the
         * pak format, not a user-visible asset. */
        for (size_t i = 0; i < actual; ++i) {
            const PakEntry *pe = &entries[i];
            if (strcmp(pe->vpath, JCE_BUNDLE_MANIFEST_VPATH) == 0) continue;
            ReportEntry *re = rv_create(&ce->entries);
            re->path = jce_strdup(pe->vpath);
            re->size = (uint64_t)pe->raw_size;
            re->hash = hex16(pe->content_hash);
        }
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
        jce_path_join(cat_path, sizeof(cat_path),
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

        /* P3-A.4 — emit build_report.json for the editor's Build Report
         * panel.  Best-effort: failure to write the report is not fatal
         * to the build itself; just log a warning. */
        {
            size_t rlen = 0;
            char *rjson = build_report_json(&catalog, &rlen);
            if (rjson) {
                char rpath[1280];
                jce_path_join(rpath, sizeof(rpath),
                         out_dir, JCE_BUILD_REPORT_NAME);
                if (write_file(rpath, rjson, rlen)) {
                    LOG("build report written: %s (%zu bundles)",
                        rpath, catalog.n);
                } else {
                    pack_log_warn("cannot write %s", rpath);
                }
                JCE_FREE(rjson);
            }
        }
    }

    sv_free(&scene_paths);
    sv_free(&scene_ids);
    am_free(&am);
    bv_free(&bundles);
    cv_free(&catalog);
    ext_free(&emap);
    mesh_free(&mcmap);
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
