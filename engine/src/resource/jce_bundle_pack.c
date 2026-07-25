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
#include <jce/os/core/jce_thread.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_archive_cook.h>

#include "jce_cook_policy.h"
#include "jce_bundle_contract_internal.h"
#include "os/core/jce_memory.h"

#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jce/resource/jce_bundle_format.h>
#include <jce/resource/jce_bundle_deps.h>

/* P0-build-bundles-cook: in-process asset cooking (textures/audio) and
 * mesh→GLB conversion. These TUs are added to the jce_resource layer in
 * engine/CMakeLists.txt so the packer can cook without a jce_cook subprocess. */
#include "jce_asset_cooker.h"

#include <cjson/cJSON.h>
#include <xxhash.h>
/* cgltf is the engine's single glTF authority — used here ONLY to probe an
 * already-authored .gltf/.glb before handing it to the assimp-backed mesh
 * converter (see the COOK_CLASS_MODEL branch in cook_asset). */
#include <cgltf.h>

/* Mesh→GLB converter (jce_bundle_mesh_convert.cpp, same layer). */
JCE_API int jce_bundle_convert_to_glb(const uint8_t *src, size_t src_sz,
                                      const char *ext_hint,
                                      uint8_t **out_buf, size_t *out_size);

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
 * one thread, editor on another) cannot stomp each other.  Backed by
 * jce_tls_* (SDL3) instead of a per-toolchain TLS keyword, so there is
 * no silent data-race fallback on compilers without native TLS. */
static JceTLS *g_pack_tls = NULL;

static PackCtx *pack_ctx_get(void)
{
    return g_pack_tls ? (PackCtx *)jce_tls_get(g_pack_tls) : NULL;
}

static void pack_ctx_set(PackCtx *c)
{
    if (!g_pack_tls) g_pack_tls = jce_tls_create(NULL);
    jce_tls_set(g_pack_tls, c);
}

/* Non-NULL only for the duration of the phase-2 parallel cook.  The active VFS
 * (PhysFS) is not reentrant, and cook_asset re-enters read_asset()->
 * jce_fs_read_all on worker threads for sibling .import.json sidecars, so this
 * serialises VFS reads across the cook workers + the driver's cooperative drain
 * (audit Round-3 P1).  It is a process-global shared by all packer instances,
 * which is safe: only one parallel cook runs at a time per process. */
static JceMutex *s_vfs_read_mutex = NULL;

static void pack_emit(JceBundlePackLogLevel level, const char *fmt, va_list ap)
{
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    /* Trim a single trailing newline — callbacks add their own. */
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
    PackCtx *ctx = pack_ctx_get();
    if (ctx && ctx->log_fn) {
        if (ctx->quiet && level == JCE_BUNDLE_PACK_LOG_INFO) return;
        ctx->log_fn(level, buf, ctx->log_user);
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
    PackCtx *ctx = pack_ctx_get();
    if (ctx) {
        ctx->error_code = -1;
        longjmp(ctx->jmp, 1);
    }
    /* Should never reach here; abort just in case. */
    abort();
}

static char *pack_strdup(const char *s)
{
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *out = (char *)JCE_MALLOC(n);
    if (!out) die("oom");
    memcpy(out, s, n);
    return out;
}

static char *pack_cjson_print_owned(const cJSON *item, size_t *out_len,
                                    bool unformatted)
{
    char *tmp = unformatted ? cJSON_PrintUnformatted(item) : cJSON_Print(item);
    if (!tmp) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    size_t n = strlen(tmp);
    char *out = (char *)JCE_MALLOC(n + 1);
    if (!out) {
        cJSON_free(tmp);
        die("oom");
    }
    memcpy(out, tmp, n + 1);
    cJSON_free(tmp);
    if (out_len) *out_len = n;
    return out;
}

/* ================================================================== */
/* Small dynamic vectors                                                */
/* ================================================================== */

static void sv_push(StrVec *v, const char *s) {
    if (v->n == v->c) {
        v->c = v->c ? v->c * 2 : 16;
        v->items = (char **)JCE_REALLOC(v->items, v->c * sizeof(char *));
        if (!v->items) die("oom");
    }
    v->items[v->n++] = pack_strdup(s);
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
/* External-asset map                                                   */
/* ================================================================== */

#define JCE_BUNDLE_EXTERNAL_PREFIX "_external/"

typedef struct {
    char *abs;
    char *vpath;
} ExternalEntry;

typedef struct {
    ExternalEntry *items;
    size_t         n, c;
} ExternalMap;

static int pack_is_abs_path(const char *path)
{
    if (!path || !path[0])
        return 0;
    if ((path[0] >= 'A' && path[0] <= 'Z') ||
        (path[0] >= 'a' && path[0] <= 'z')) {
        if (path[1] == ':' && (path[2] == '/' || path[2] == '\\'))
            return 1;
    }
    return path[0] == '/' || (path[0] == '\\' && path[1] == '\\');
}

static int ends_with_ci(const char *s, const char *suffix)
{
    if (!s || !suffix)
        return 0;
    size_t n = strlen(s);
    size_t m = strlen(suffix);
    if (n < m)
        return 0;
    s += n - m;
    for (size_t i = 0; i < m; ++i) {
        char a = s[i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

static void ext_free(ExternalMap *m)
{
    if (!m)
        return;
    for (size_t i = 0; i < m->n; ++i) {
        JCE_FREE(m->items[i].abs);
        JCE_FREE(m->items[i].vpath);
    }
    JCE_FREE(m->items);
    m->items = NULL;
    m->n = m->c = 0;
}

static const char *ext_lookup_vpath(const ExternalMap *m, const char *abs)
{
    if (!m || !abs)
        return NULL;
    for (size_t i = 0; i < m->n; ++i) {
        if (strcmp(m->items[i].abs, abs) == 0)
            return m->items[i].vpath;
    }
    return NULL;
}

static const char *ext_lookup_abs(const ExternalMap *m, const char *vpath)
{
    if (!m || !vpath)
        return NULL;
    for (size_t i = 0; i < m->n; ++i) {
        if (strcmp(m->items[i].vpath, vpath) == 0)
            return m->items[i].abs;
    }
    return NULL;
}

static char *ext_make_vpath(const char *abs)
{
    const char *base = abs;
    for (const char *p = abs; p && *p; ++p) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    if (!base || !*base)
        base = "file";

    uint64_t h = XXH3_64bits(abs, strlen(abs));
    size_t need = sizeof(JCE_BUNDLE_EXTERNAL_PREFIX) + 8 + 1 +
                  strlen(base) + 1;
    char *out = (char *)JCE_MALLOC(need);
    if (!out)
        die("oom");
    snprintf(out, need, JCE_BUNDLE_EXTERNAL_PREFIX "%08x_%s",
             (unsigned)(h ^ (h >> 32)), base);
    for (char *p = out; *p; ++p) {
        if (*p == '\\' || *p == ':')
            *p = '_';
    }
    return out;
}

static const char *ext_register(ExternalMap *m, const char *abs)
{
    const char *vpath = ext_lookup_vpath(m, abs);
    if (vpath)
        return vpath;
    if (m->n == m->c) {
        m->c = m->c ? m->c * 2 : 16;
        m->items = (ExternalEntry *)JCE_REALLOC(m->items,
                                                m->c * sizeof(ExternalEntry));
        if (!m->items)
            die("oom");
    }
    ExternalEntry *e = &m->items[m->n++];
    e->abs = pack_strdup(abs);
    /* Convention sidecars ("<model>.jcol", "<skeleton>.anim.json") must
     * keep their "<base><suffix>" relationship after virtualisation, or
     * the runtime's suffix-derived lookups would miss: when the base
     * asset is already registered, reuse its vpath + suffix instead of
     * minting a fresh (differently-hashed) name.  Bases are always
     * registered before their sidecars (scene scan stages models first;
     * the virtualisation walk preserves insertion order). */
    e->vpath = NULL;
    {
        static const char *const kSidecars[] = { ".jcol", ".anim.json", NULL };
        size_t alen = strlen(abs);
        for (size_t si = 0; kSidecars[si] && !e->vpath; ++si) {
            size_t slen = strlen(kSidecars[si]);
            if (alen <= slen || !ends_with_ci(abs, kSidecars[si]))
                continue;
            char base[1280];
            if (alen - slen >= sizeof(base))
                continue;
            memcpy(base, abs, alen - slen);
            base[alen - slen] = '\0';
            const char *bvp = ext_lookup_vpath(m, base);
            if (!bvp)
                continue;
            size_t need = strlen(bvp) + slen + 1;
            char *out = (char *)JCE_MALLOC(need);
            if (!out)
                die("oom");
            snprintf(out, need, "%s%s", bvp, kSidecars[si]);
            e->vpath = out;
        }
    }
    if (!e->vpath)
        e->vpath = ext_make_vpath(abs);
    return e->vpath;
}

static uint8_t *ext_rewrite_text(const uint8_t *src, size_t src_sz,
                                 const ExternalMap *m, size_t *out_size)
{
    if (!src) {
        if (out_size) *out_size = 0;
        return NULL;
    }
    if (!m || m->n == 0) {
        uint8_t *copy = (uint8_t *)JCE_MALLOC(src_sz ? src_sz : 1);
        if (!copy)
            die("oom");
        if (src_sz)
            memcpy(copy, src, src_sz);
        if (out_size) *out_size = src_sz;
        return copy;
    }

    size_t cap = src_sz + 1;
    for (size_t i = 0; i < m->n; ++i)
        cap += strlen(m->items[i].vpath) + 16;

    uint8_t *out = (uint8_t *)JCE_MALLOC(cap ? cap : 1);
    if (!out)
        die("oom");

    size_t ip = 0;
    size_t op = 0;
    while (ip < src_sz) {
        int matched = 0;
        for (size_t i = 0; i < m->n; ++i) {
            const char *abs = m->items[i].abs;
            size_t alen = strlen(abs);
            if (alen == 0 || ip + alen > src_sz)
                continue;
            if (memcmp(src + ip, abs, alen) != 0)
                continue;

            const char *vp = m->items[i].vpath;
            size_t vlen = strlen(vp);
            if (op + vlen > cap) {
                cap = (op + vlen + 1) * 2;
                uint8_t *grow = (uint8_t *)JCE_REALLOC(out, cap);
                if (!grow) {
                    JCE_FREE(out);
                    die("oom");
                }
                out = grow;
            }
            memcpy(out + op, vp, vlen);
            op += vlen;
            ip += alen;
            matched = 1;
            break;
        }
        if (!matched)
            out[op++] = src[ip++];
    }

    if (out_size) *out_size = op;
    return out;
}

static void ext_rewrite_cjson(cJSON *root, const ExternalMap *m)
{
    if (!root || !m || m->n == 0)
        return;

    cJSON **stack = NULL;
    size_t n = 0;
    size_t c = 0;
#define PUSH_NODE(NODE) do {                                                   \
        cJSON *_node = (NODE);                                                 \
        if (!_node) break;                                                     \
        if (n == c) {                                                          \
            c = c ? c * 2 : 64;                                                \
            cJSON **grow = (cJSON **)JCE_REALLOC(stack, c * sizeof(*stack));   \
            if (!grow) { JCE_FREE(stack); return; }                            \
            stack = grow;                                                      \
        }                                                                      \
        stack[n++] = _node;                                                    \
    } while (0)

    PUSH_NODE(root);
    while (n > 0) {
        cJSON *node = stack[--n];
        if (cJSON_IsString(node) && node->valuestring) {
            const char *vp = ext_lookup_vpath(m, node->valuestring);
            if (vp)
                cJSON_SetValuestring(node, vp);
        }
        for (cJSON *child = node->child; child; child = child->next)
            PUSH_NODE(child);
    }

#undef PUSH_NODE
    JCE_FREE(stack);
}

static uint8_t *ext_rewrite_json(const uint8_t *src, size_t src_sz,
                                 const ExternalMap *m, size_t *out_size)
{
    if (!src || !m || m->n == 0)
        return ext_rewrite_text(src, src_sz, m, out_size);

    cJSON *root = cJSON_ParseWithLength((const char *)src, src_sz);
    if (!root)
        return ext_rewrite_text(src, src_sz, m, out_size);

    ext_rewrite_cjson(root, m);
    size_t len = 0;
    char *txt = pack_cjson_print_owned(root, &len, true);
    cJSON_Delete(root);
    if (!txt)
        return ext_rewrite_text(src, src_sz, m, out_size);

    if (out_size) *out_size = len;
    return (uint8_t *)txt;
}

static uint8_t *ext_rewrite_asset(const char *vpath, uint8_t *raw,
                                  size_t raw_size, const ExternalMap *m,
                                  size_t *out_size)
{
    if (!raw) {
        if (out_size) *out_size = 0;
        return NULL;
    }
    if (!m || m->n == 0) {
        if (out_size) *out_size = raw_size;
        return raw;
    }

    if (!ends_with_ci(vpath, ".json") &&
        !ends_with_ci(vpath, ".gltf") &&
        !ends_with_ci(vpath, ".scene") &&
        !ends_with_ci(vpath, ".obj") &&
        !ends_with_ci(vpath, ".mtl")) {
        if (out_size) *out_size = raw_size;
        return raw;
    }

    size_t new_size = 0;
    uint8_t *rewritten =
        (ends_with_ci(vpath, ".json") || ends_with_ci(vpath, ".gltf") ||
         ends_with_ci(vpath, ".scene"))
            ? ext_rewrite_json(raw, raw_size, m, &new_size)
            : ext_rewrite_text(raw, raw_size, m, &new_size);
    if (!rewritten) {
        if (out_size) *out_size = raw_size;
        return raw;
    }

    JCE_FREE(raw);
    if (out_size) *out_size = new_size;
    return rewritten;
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

    if (emap) {
        const char *abs = ext_lookup_abs(emap, vpath);
        if (abs) return read_file(abs, out_size);
    }

    if (pack_is_abs_path(vpath))
        return read_file(vpath, out_size);

    JceFileSystem *fs = jce_fs_get_active();
    if (fs) {
        uint64_t vsz = 0;
        /* Serialise the non-reentrant VFS read when a parallel cook is in
         * flight (s_vfs_read_mutex non-NULL); a no-op on the single-threaded
         * driver path (audit Round-3 P1). */
        if (s_vfs_read_mutex) jce_mutex_lock(s_vfs_read_mutex);
        void *vbuf = jce_fs_read_all(fs, vpath, &vsz);
        if (s_vfs_read_mutex) jce_mutex_unlock(s_vfs_read_mutex);
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

static int can_read_asset(const char *vpath, const char *resource_root,
                          PackResolveFn resolve_fn, void *resolve_user,
                          const ExternalMap *emap)
{
    size_t sz = 0;
    uint8_t *raw = read_asset(vpath, resource_root, resolve_fn, resolve_user,
                              emap, &sz);
    if (!raw)
        return 0;
    JCE_FREE(raw);
    return 1;
}

/* ================================================================== */
/* P0-build-bundles-cook: in-process asset cooking                     */
/* ================================================================== */

/* Map an import.json target_format string to a JCEASSET_TEXFMT_* value.
 * Returns -1 for "auto"/unknown so the caller falls back to platform auto. */
static int cook_parse_texfmt(const char *s)
{
    if (!s || !s[0]) return -1;
    if (strcmp(s, "rgba8") == 0) return JCEASSET_TEXFMT_RGBA8;
    if (strcmp(s, "bc1")   == 0) return JCEASSET_TEXFMT_BC1;
    if (strcmp(s, "bc3")   == 0) return JCEASSET_TEXFMT_BC3;
    if (strcmp(s, "bc5")   == 0) return JCEASSET_TEXFMT_BC5;
    if (strcmp(s, "bc7")   == 0) return JCEASSET_TEXFMT_BC7;
    if (strcmp(s, "astc")  == 0 || strcmp(s, "astc_4x4") == 0)
        return JCEASSET_TEXFMT_ASTC_4x4;
    return -1;
}

/* Texture-format policy (normal-map heuristic + per-platform auto format)
 * lives in jce_cook_policy.h — shared with jce_asset_cooker.c so the two
 * cook paths can never drift apart. */

/* Read & parse a sibling "<vpath>.import.json" preset, if present. Returns a
 * cJSON root the caller must cJSON_Delete, or NULL when absent/unparseable. */
static cJSON *cook_read_import_json(const char *vpath,
                                    const char *resource_root,
                                    PackResolveFn resolve_fn,
                                    void *resolve_user,
                                    const ExternalMap *emap)
{
    char sp[1408];
    snprintf(sp, sizeof(sp), "%s.import.json", vpath);
    size_t sz = 0;
    uint8_t *buf = read_asset(sp, resource_root, resolve_fn, resolve_user,
                              emap, &sz);
    if (!buf) return NULL;
    cJSON *root = cJSON_ParseWithLength((const char *)buf, sz);
    JCE_FREE(buf);
    return root;
}

/* Per-job cook outcome, written on the worker and replayed by the driver —
 * workers must never touch the thread-local log sink (audit Round-3 P2). */
typedef struct {
    bool failed;       /* a cook was attempted but failed → shipped raw      */
    char err[96];      /* short failure detail for the driver-side warning   */
} CookStatus;

/* Would round-tripping this glTF/GLB blob through the assimp mesh converter
 * destroy data cgltf would otherwise have loaded at runtime?
 *
 * True for anything the converter's two writers cannot express: both the
 * indexed-glb writer and assimp's glb2 exporter fallback emit plain
 * POSITION/NORMAL/UV0 primitives, so skins, animation clips and morph targets
 * do not survive the trip.  Detection runs on cgltf so the answer comes from
 * the same parser that owns the format at runtime; cgltf_parse reads only the
 * JSON header (plus the GLB chunk table) — no buffer load, no image decode —
 * which keeps this cheap enough for the parallel cook.  Reentrant: no globals,
 * allocations go through cgltf's default malloc/free.
 *
 * A parse failure returns false so a malformed/unusual input keeps the exact
 * pre-existing behaviour (hand it to assimp and let that path report). */
static bool gltf_assimp_roundtrip_is_lossy(const uint8_t *raw, size_t raw_size)
{
    cgltf_options options;
    cgltf_data   *data = NULL;
    memset(&options, 0, sizeof options);
    if (cgltf_parse(&options, raw, (cgltf_size)raw_size, &data)
            != cgltf_result_success || !data)
        return false;

    bool lossy = (data->skins_count > 0) || (data->animations_count > 0);
    for (cgltf_size m = 0; !lossy && m < data->meshes_count; ++m) {
        const cgltf_mesh *mesh = &data->meshes[m];
        if (mesh->weights_count > 0) { lossy = true; break; }
        for (cgltf_size p = 0; p < mesh->primitives_count; ++p) {
            if (mesh->primitives[p].targets_count > 0) { lossy = true; break; }
        }
    }

    cgltf_free(data);
    return lossy;
}

/* Cook one gathered asset.  On success frees `raw` and returns a freshly
 * JCE_MALLOC'd cooked buffer (out_size set), keeping the original vpath
 * (runtime loaders content-sniff).  On any non-cook / failure case returns
 * `raw` unchanged so the asset still ships uncooked. */
static uint8_t *cook_asset(const char *vpath, uint8_t *raw, size_t raw_size,
                           const char *resource_root,
                           PackResolveFn resolve_fn, void *resolve_user,
                           const ExternalMap *emap, int target_platform,
                           size_t *out_size, CookStatus *st)
{
    if (out_size) *out_size = raw_size;
    if (st) { st->failed = false; st->err[0] = '\0'; }
    if (!raw || raw_size == 0) return raw;

    CookClass cls = jce_bundle_classify_cook(vpath);
    if (cls == COOK_CLASS_NONE) return raw;

    /* LUT strip PNGs must ship as raw PNG bytes — jce_texture_load_lut_3d
     * calls jce_texture_decode_cpu which decodes the PNG directly; a
     * .jceasset wrapper (even RGBA8) is opaque to that loader.  Any block
     * compression (BC3/ASTC) is also lossy-corrupted for a precision LUT.
     * Return the raw bytes untouched so the PAK contains a plain PNG. */
    if (cls == COOK_CLASS_TEXTURE && jce_cook_path_is_lut(vpath)) {
        return raw;   /* passthrough — keep exact PNG bytes */
    }

    cJSON *imp = cook_read_import_json(vpath, resource_root, resolve_fn,
                                       resolve_user, emap);

    if (cls == COOK_CLASS_TEXTURE) {
        JceCookOptions opt = JCE_COOK_DEFAULT;
        opt.platform         = (JceCookPlatform)target_platform;
        opt.generate_mipmaps = true;
        opt.texture_format   = jce_cook_auto_texture_format(vpath,
                                                            target_platform);
        opt.max_texture_size = 0;
        /* Build-bundles iteration favours speed: range-fit block encode is
         * ~5-7x faster than the cluster-fit default at a modest quality cost.
         * An .import.json "quality" overrides per texture for hero/UI art. */
        opt.encode_quality   = JCE_COOK_ENCODE_FAST;
        if (imp) {
            const cJSON *tf = cJSON_GetObjectItemCaseSensitive(imp, "target_format");
            const cJSON *gm = cJSON_GetObjectItemCaseSensitive(imp, "gen_mips");
            const cJSON *ms = cJSON_GetObjectItemCaseSensitive(imp, "max_size");
            const cJSON *q  = cJSON_GetObjectItemCaseSensitive(imp, "quality");
            if (cJSON_IsString(tf)) {
                int f = cook_parse_texfmt(tf->valuestring);
                if (f >= 0) opt.texture_format = f;   /* -1 => keep auto */
            }
            if (cJSON_IsBool(gm)) opt.generate_mipmaps = cJSON_IsTrue(gm);
            if (cJSON_IsNumber(ms) && ms->valuedouble > 0)
                opt.max_texture_size = (int)ms->valuedouble;
            if (cJSON_IsString(q)) {
                if (jce_strcasecmp(q->valuestring, "fast") == 0)
                    opt.encode_quality = JCE_COOK_ENCODE_FAST;
                else if (jce_strcasecmp(q->valuestring, "high") == 0 ||
                         jce_strcasecmp(q->valuestring, "default") == 0)
                    opt.encode_quality = JCE_COOK_ENCODE_DEFAULT;
                else if (jce_strcasecmp(q->valuestring, "highest") == 0)
                    opt.encode_quality = JCE_COOK_ENCODE_HIGHEST;
            }
        }
        JceCookResult r = jce_cook_texture(raw, raw_size, &opt);
        if (imp) cJSON_Delete(imp);
        if (!r.success || !r.data) {
            if (st) { st->failed = true;
                snprintf(st->err, sizeof(st->err), "%s",
                         r.error[0] ? r.error : "unknown"); }
            jce_cook_result_free(&r);
            return raw;
        }
        JCE_FREE(raw);
        if (out_size) *out_size = r.size;
        return (uint8_t *)r.data;   /* JCE_MALLOC'd by the cooker */
    }

    if (cls == COOK_CLASS_AUDIO) {
        JceCookOptions opt = JCE_COOK_DEFAULT;
        opt.platform = (JceCookPlatform)target_platform;
        JceCookResult r = jce_cook_audio(raw, raw_size, &opt);
        if (imp) cJSON_Delete(imp);
        if (!r.success || !r.data) {
            if (st) { st->failed = true;
                snprintf(st->err, sizeof(st->err), "%s",
                         r.error[0] ? r.error : "unknown"); }
            jce_cook_result_free(&r);
            return raw;
        }
        JCE_FREE(raw);
        if (out_size) *out_size = r.size;
        return (uint8_t *)r.data;
    }

    /* COOK_CLASS_MODEL — convert any Assimp-readable mesh to GLB (+meshopt).
     * Already-GLB inputs are re-run through the converter so the meshopt
     * dedup/vertex-cache pass still applies — EXCEPT the rigged/morphed ones
     * the round-trip would mangle (see the cgltf guard below); if conversion
     * fails we ship the source bytes verbatim (a .gltf/.glb still loads at
     * runtime). */
    if (imp) cJSON_Delete(imp);   /* model import.json (scale/normals) is
                                     honoured by the editor importer, not the
                                     bundle-time GLB converter; consult here
                                     only to detect presence. */
    {
        const char *dot = strrchr(vpath, '.');
        const char *ext_hint = dot ? dot + 1 : "";

        /* glTF dual-authority guard: an already-authored .gltf/.glb is cgltf's
         * asset, and the converter above is an ASSIMP path.  Re-importing it
         * only to re-export it is a lossy round-trip — both the indexed-glb
         * writer and assimp's glb2 exporter emit POSITION/NORMAL/UV0 geometry
         * only, so a rigged character silently comes out of the bundle with no
         * skeleton, no animations and no morph targets.  Probe the source with
         * cgltf (the engine's single glTF authority; header parse only — no
         * buffer load, no image decode) and ship those inputs verbatim.  Static
         * glTF still goes through the converter, so the meshopt dedup +
         * auto-LOD + auto-meshlet cook is unchanged for the common case. */
        if ((ends_with_ci(vpath, ".glb") || ends_with_ci(vpath, ".gltf")) &&
            gltf_assimp_roundtrip_is_lossy(raw, raw_size))
            return raw;

        uint8_t *glb = NULL;
        size_t   glb_sz = 0;
        if (jce_bundle_convert_to_glb(raw, raw_size, ext_hint, &glb, &glb_sz)
            && glb && glb_sz > 0) {
            JCE_FREE(raw);
            if (out_size) *out_size = glb_sz;
            return glb;
        }
        if (st) { st->failed = true;
            snprintf(st->err, sizeof(st->err), "convert-to-GLB failed"); }
        return raw;
    }
}

/* ── Parallel asset cook ──────────────────────────────────────────────
 *
 * cook_asset is the slow part of a build (BC block-encode = seconds per 2K
 * texture; Assimp+meshopt for large models), and each asset cooks fully
 * independently, so the per-bundle asset loop fans out onto the shared job
 * pool.  Safety (see the per-field reasoning in the loop below):
 *   - emap and resolve_fn/user are READ-ONLY during cooking — shared, no lock.
 *   - each job writes only its own CookJob slot — no shared counter.
 *   - workers NEVER call die()/longjmp or pack_log: g_ctx is thread-local and
 *     belongs to the driver thread, and the editor's log sink is not
 *     thread-safe.  read_asset / ext_rewrite_asset (longjmp on OOM) and
 *     pack_strdup run on the driver BEFORE dispatch; cook_asset itself only
 *     returns NULL/raw on failure (never longjmps), and its internal LOG/warn
 *     lines are intentionally dropped on workers and reconstructed by the
 *     driver from job metadata after the group completes (deterministic order).
 * The lone non-reentrant encoder path (NVTT BC7/BC6H process-global flags) is
 * serialised inside jce_tex_encode; the default BC3/BC5/ASTC policy never hits
 * it, so the common case stays fully parallel. */
typedef struct {
    size_t      slot;       /* entries[] index this asset writes              */
    const char *vpath;      /* borrowed (b->assets.items[i])                  */
    uint8_t    *buf;        /* in: rewritten raw; out: cooked (worker writes)  */
    size_t      in_size;    /* original size (for the reconstructed log line)  */
    size_t      out_size;   /* cooked size (worker writes)                     */
    CookStatus  status;     /* worker-written outcome, driver-replayed        */
} CookJob;

typedef struct {
    CookJob          *jobs;
    const char       *resource_root;
    PackResolveFn     resolve_fn;
    void             *resolve_user;
    const ExternalMap *emap;
    int               target_platform;
} CookCtx;

static void cook_jobs_range(uint32_t begin, uint32_t end, void *user)
{
    CookCtx *c = (CookCtx *)user;
    for (uint32_t k = begin; k < end; ++k) {
        CookJob *j = &c->jobs[k];
        j->buf = cook_asset(j->vpath, j->buf, j->in_size, c->resource_root,
                            c->resolve_fn, c->resolve_user, c->emap,
                            c->target_platform, &j->out_size, &j->status);
    }
}

static int write_file(const char *path, const void *data, size_t size) {
    return jce_fs_host_write_all_atomic(path, data, (uint64_t)size) ? 1 : 0;
}

static void mkdir_p(const char *path) {
    if (path && path[0])
        jce_fs_host_create_directory(path);
}

static void normalise_sep(char *s) { for (; *s; ++s) if (*s == '\\') *s = '/'; }

static int ends_with_lit(const char *s, const char *suffix)
{
    if (!s || !suffix)
        return 0;
    size_t n = strlen(s);
    size_t m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static size_t scene_suffix_len(const char *path)
{
    if (ends_with_lit(path, ".scene.json"))
        return 11;
    if (ends_with_lit(path, ".scene"))
        return 6;
    return 0;
}

static int is_scene_file_path(const char *path)
{
    return scene_suffix_len(path) != 0;
}

static void strip_scene_suffix(char *path)
{
    size_t ext = scene_suffix_len(path);
    if (ext)
        path[strlen(path) - ext] = '\0';
}

/* Recursive enumeration of .scene / .scene.json under a directory. */
static bool walk_scenes_cb(const char *path, bool is_dir, void *user)
{
    if (is_dir || !path || !user)
        return true;

    const char *leaf = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') leaf = p + 1;
    if (leaf[0] == '.')
        return true;

    if (is_scene_file_path(leaf))
        sv_push((StrVec *)user, path);
    return true;
}

static void walk_scenes(const char *dir, StrVec *out)
{
    if (dir && out)
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
    n->asset = pack_strdup(asset);
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
    b->id = pack_strdup(id);
    b->kind = pack_strdup(kind);
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
/* Per-scene dependency staging                                        */
/* ================================================================== */

/* Shared state for pushing a discovered asset into BOTH the per-scene
 * dep list (which doubles as the recursion's visited set) and the
 * global asset→bundle association map. */
typedef struct DepStage {
    JceBundleDepList *deps;
    AssetMap         *am;
    JceBundleDependencyEdges *edges;
    const char       *scene_id;
    const char       *parent;
    const char       *origin;
} DepStage;

/* Push `vpath` if not already present.  Returns true when newly
 * inserted (used as the fixed-point "grew" signal by the descriptor
 * recursion). */
static bool stage_push_dep(DepStage *st, const char *vpath,
                           const char *bundle_tag)
{
    if (!vpath || !vpath[0]) return false;
    if (!jce_bundle_edges_add(st->edges, st->parent, vpath, st->origin))
        die("oom");
    JceBundleDepList *deps = st->deps;
    for (uint32_t e = 0; e < deps->count; ++e) {
        if (deps->items[e].path &&
            strcmp(deps->items[e].path, vpath) == 0)
            return false;
    }
    if (deps->count + 1 > deps->capacity) {
        uint32_t nc = deps->capacity ? deps->capacity * 2 : 16;
        JceBundleDep *g = (JceBundleDep *)JCE_REALLOC(
            deps->items, nc * sizeof(*g));
        if (!g) return false;
        deps->items    = g;
        deps->capacity = nc;
    }
    JceBundleDep *nd = &deps->items[deps->count++];
    nd->path   = pack_strdup(vpath);
    nd->bundle = bundle_tag ? pack_strdup(bundle_tag) : NULL;

    AssetRef *r = am_get_or_create(st->am, vpath);
    if (!sv_contains(&r->refs, st->scene_id))
        sv_push(&r->refs, st->scene_id);
    if (bundle_tag && !r->override)
        r->override = pack_strdup(bundle_tag);
    return true;
}

static void stage_dependency_document(
    DepStage *stage, const JceBundleDependencyDocument *document,
    const char *source, const char *resource_root,
    PackResolveFn resolve_fn, void *resolve_user)
{
    for (uint32_t i = 0; i < document->assets.count; ++i) {
        const JceBundleDep *dep = &document->assets.items[i];
        (void)stage_push_dep(stage, dep->path, dep->bundle);
    }

    for (uint32_t i = 0; i < document->optional_assets.count; ++i) {
        const JceBundleDep *dep = &document->optional_assets.items[i];
        if (can_read_asset(dep->path, resource_root, resolve_fn,
                           resolve_user, NULL)) {
            (void)stage_push_dep(stage, dep->path, dep->bundle);
        } else {
            pack_log_warn("optional dependency missing: %s (declared by %s)",
                          dep->path, source);
        }
    }
}

/* Game-content localization: jce_loc_set_source_pak() expects locale
 * tables addressed as "i18n/<locale>.json" inside the mounted pak, so
 * any "i18n/" directory under the source-assets root ships wholesale.
 * Collected once per build, then staged into every scene's dep list —
 * the shared-bundle threshold automatically promotes the tables into
 * the shared bundle when more than one scene ships. */
typedef struct I18nWalkCtx {
    StrVec *out;
    size_t  dir_len; /* strlen of the host i18n dir prefix */
} I18nWalkCtx;

static bool walk_i18n_cb(const char *path, bool is_dir, void *user)
{
    I18nWalkCtx *c = (I18nWalkCtx *)user;
    if (is_dir || !path || !user)
        return true;
    if (!ends_with_ci(path, ".json"))
        return true;
    const char *rel = path + c->dir_len;
    while (*rel == '/' || *rel == '\\') ++rel;
    if (!*rel)
        return true;
    char vpath[1280];
    snprintf(vpath, sizeof(vpath), "i18n/%s", rel);
    normalise_sep(vpath);
    if (!sv_contains(c->out, vpath))
        sv_push(c->out, vpath);
    return true;
}

/* ================================================================== */
/* Scene-id derivation                                                 */
/* ================================================================== */

/* Derive a stable bundle id for a scene file.
 *
 * Common case (project / selected-scenes-under-scenes_dir): the path
 * starts with `scenes_dir`, so we strip that prefix and the trailing
 * scene suffix (`.scene` or `.scene.json`), then replace path
 * separators with `_`.
 *
 * Standalone case: when the scene lives OUTSIDE `scenes_dir` (selected
 * scenes / single-scene mode with a floating file), we fall back to
 * the basename minus its scene suffix and append an 8-hex collision tag
 * derived from the full path so two files with the same basename in
 * different directories cannot clobber each other in the catalog. */
static char *make_scene_id(const char *full_path, const char *scenes_dir) {
    size_t blen = scenes_dir ? strlen(scenes_dir) : 0;
    int    inside = (blen > 0 && strncmp(full_path, scenes_dir, blen) == 0);

    if (inside) {
        const char *rel = full_path + blen;
        while (*rel == '/' || *rel == '\\') ++rel;
        char *out = pack_strdup(rel);
        if (!out) die("oom");
        strip_scene_suffix(out);
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
    size_t ext = scene_suffix_len(base);
    size_t stem = ext ? bn - ext : bn;
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
        char *out = pack_strdup(rel);
        if (!out) die("oom");
        normalise_sep(out);
        return out;
    }
    const char *base = full_path;
    for (const char *p = full_path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    char *out = pack_strdup(base);
    if (!out) die("oom");
    normalise_sep(out);
    return out;
}

/* ================================================================== */
/* JPAK v1 builder                                                      */
/* ================================================================== */

#define JCE_BUNDLE_CANONICAL_PATH_CAP 2048u

typedef struct {
    char    *path;
    uint64_t id;
} CanonicalAddress;

static size_t canonical_entry_path(const char *path, char *out, size_t out_cap)
{
    if (!path || !path[0])
        return 0;
    return jce_archive_normalize_path(path, out, out_cap);
}

static bool bundle_owns_address(const Bundle *bundle, const char *address)
{
    char canonical[JCE_BUNDLE_CANONICAL_PATH_CAP];
    if (bundle->scene_path &&
        canonical_entry_path(bundle->scene_path, canonical,
                             sizeof(canonical)) > 0 &&
        strcmp(canonical, address) == 0)
        return true;
    for (size_t i = 0; i < bundle->assets.n; ++i) {
        if (canonical_entry_path(bundle->assets.items[i], canonical,
                                 sizeof(canonical)) > 0 &&
            strcmp(canonical, address) == 0)
            return true;
    }
    return false;
}

static bool validate_entry_addresses(const PakEntry *entries, size_t count)
{
    CanonicalAddress *addresses = (CanonicalAddress *)JCE_CALLOC(
        count ? count : 1, sizeof(*addresses));
    if (!addresses)
        die("oom");

    bool valid = true;
    for (size_t i = 0; i < count; ++i) {
        char canonical[JCE_BUNDLE_CANONICAL_PATH_CAP];
        size_t len = canonical_entry_path(entries[i].vpath, canonical,
                                          sizeof(canonical));
        if (len == 0) {
            ERR("invalid virtual asset address: '%s'",
                entries[i].vpath ? entries[i].vpath : "(null)");
            valid = false;
            break;
        }
        if (strcmp(canonical, JCE_BUNDLE_MANIFEST_VPATH) == 0) {
            ERR("asset address is reserved by the bundle contract: '%s'",
                entries[i].vpath);
            valid = false;
            break;
        }

        addresses[i].path = pack_strdup(canonical);
        addresses[i].id = jce_archive_hash_normalized(canonical, len);
        if (!addresses[i].path || addresses[i].id == 0)
            die("oom");

        for (size_t j = 0; j < i; ++j) {
            if (strcmp(addresses[j].path, canonical) == 0) {
                ERR("canonical address collision: '%s' and '%s' -> '%s'",
                    entries[j].vpath, entries[i].vpath, canonical);
                valid = false;
                break;
            }
            if (addresses[j].id == addresses[i].id) {
                ERR("asset id hash collision: '%s' and '%s'",
                    addresses[j].path, canonical);
                valid = false;
                break;
            }
        }
        if (!valid)
            break;
    }

    for (size_t i = 0; i < count; ++i)
        JCE_FREE(addresses[i].path);
    JCE_FREE(addresses);
    return valid;
}

static uint8_t *build_jbundle(PakEntry *entries, size_t count, int zstd_level,
                              bool encrypt, const uint8_t *encryption_key,
                              const char *encrypt_label, size_t *out_size)
{
    JceCookInput *inputs = (JceCookInput *)JCE_MALLOC(
        sizeof(JceCookInput) * (count ? count : 1));
    if (!inputs)
        die("oom");
    for (size_t i = 0; i < count; ++i) {
        PakEntry *e = &entries[i];
        e->content_hash = XXH3_64bits(e->raw, e->raw_size);
        inputs[i].vpath = e->vpath;
        inputs[i].data  = e->raw;
        inputs[i].size  = e->raw_size;
    }

    JceCookConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.zstd_level       = zstd_level;
    /* Shipping encryption also keys the index.  A recoverable debug-path
     * table would defeat that protection and is rejected by the archive
     * writer, so retain paths only for plain development bundles. */
    cfg.emit_debug_paths = !encrypt;
    cfg.compress_index   = true;
    cfg.use_dict         = true;
    cfg.dedup_content    = true;
    /* Encrypt EVERYTHING in the bundle (incl. __bundle__/manifest.json);
     * the bundle id seeds the per-archive nonce salt. */
    cfg.encrypt          = encrypt && encryption_key != NULL;
    cfg.encryption_key   = encryption_key;
    cfg.encrypt_label    = encrypt_label;

    void *blob = NULL;
    size_t blob_size = 0;
    uint16_t dict_count = 0;
    if (!jce_archive_cook(inputs, count, &cfg, &blob, &blob_size,
                          &dict_count)) {
        JCE_FREE(inputs);
        die("bundle archive cook failed");
    }

    (void)dict_count;
    JCE_FREE(inputs);
    *out_size = blob_size;
    return (uint8_t *)blob;
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
    cJSON *contract = cJSON_CreateObject();
    cJSON_AddStringToObject(contract, JCE_BUNDLE_KEY_CONTRACT_NAME,
                            JCE_BUNDLE_DIFF_CONTRACT_NAME);
    cJSON_AddNumberToObject(contract, JCE_BUNDLE_KEY_CONTRACT_MAJOR,
                            JCE_BUNDLE_DIFF_CONTRACT_MAJOR);
    cJSON_AddNumberToObject(contract, JCE_BUNDLE_KEY_CONTRACT_MINOR,
                            JCE_BUNDLE_DIFF_CONTRACT_MINOR);
    cJSON_AddItemToObject(diff, JCE_BUNDLE_KEY_CONTRACT, contract);
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
            const char *old_hash = jce_bundle_previous_content_hash(oj, id);
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

    size_t out_len = 0;
    char *out = pack_cjson_print_owned(diff, &out_len, false);
    int ok = out ? write_file(out_path, out, out_len) : 0;
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
        if (!resource_root[0]) {
            /* Prefer the project's ASSET root over the bare project root: JCE
             * keeps assets under <project>/resources/assets, and a bundle_roots
             * dep like fonts/<x>.ttf is resolved as <resource_root>/fonts/... .
             * Using the bare project root here made those probe at
             * <project>/fonts/... and silently fail to pack (the walk-up below
             * only runs for explicit-scene builds, and is pre-empted anyway once
             * resource_root is set).  Fall through resources/assets → resources
             * → assets → the project root itself. */
            static const char *const kAssetSub[] = {
                "resources/assets", "resources", "assets" };
            for (size_t _si = 0;
                 _si < sizeof(kAssetSub) / sizeof(kAssetSub[0]); ++_si) {
                char _probe[1024];
                snprintf(_probe, sizeof(_probe), "%s/%s",
                         opts->project_root, kAssetSub[_si]);
                if (jce_fs_host_exists_dir(_probe)) {
                    snprintf(resource_root, sizeof(resource_root), "%s", _probe);
                    break;
                }
            }
            if (!resource_root[0])
                snprintf(resource_root, sizeof(resource_root),
                         "%s", opts->project_root);
        }
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

    /* Diagnostic: the resolved asset root that read_asset probes as
     * <resource_root>/<vpath>.  A bundle_roots asset that fails to pack almost
     * always means this points at the project root instead of resources/assets. */
    pack_log_info("resource_root='%s' (scenes_dir='%s', single_file=%d)",
                  resource_root, scenes_dir, (int)single_file);

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
    {
        char stale_graph[1280];
        snprintf(stale_graph, sizeof(stale_graph), "%s/%s",
                 out_dir, JCE_BUNDLE_GRAPH_NAME);
        (void)jce_fs_host_remove_file(stale_graph);
    }

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
            if (!is_scene_file_path(p)) {
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

    /* Source-asset localization tables (packed by convention). */
    StrVec i18n_files = {0};
    {
        char i18n_dir[1100];
        snprintf(i18n_dir, sizeof(i18n_dir), "%s/i18n", resource_root);
        if (jce_fs_host_exists_dir(i18n_dir)) {
            I18nWalkCtx ic = { &i18n_files, strlen(i18n_dir) };
            (void)jce_fs_host_walk(i18n_dir, walk_i18n_cb, &ic);
            if (i18n_files.n)
                LOG("localization: packing %zu i18n table(s) from %s",
                    i18n_files.n, i18n_dir);
        }
    }

    StrVec   scene_ids = {0};
    AssetMap am        = {0};
    JceBundleDependencyEdges dependency_edges = {0};
    int      dependency_errors = 0;

    /* Project-level runtime roots cover assets that are not reachable from a
     * specific scene (boot descriptors, dynamically selected content, and
     * similar application-owned roots).  The file is optional, but once
     * present its contract is strict. */
    JceBundleDependencyDocument project_roots = {0};
    bool have_project_roots = false;
    {
        size_t roots_size = 0;
        uint8_t *roots_json = read_asset("bundle_roots.json", resource_root,
                                         opts->resolve_fn, opts->resolve_user,
                                         NULL, &roots_size);
        if (roots_json) {
            have_project_roots = jce_bundle_deps_parse_document(
                (const char *)roots_json, roots_size, &project_roots);
            if (!have_project_roots) {
                ERR("invalid dependency contract: bundle_roots.json");
                dependency_errors = 1;
            } else {
                LOG("dependency roots: %u required, %u optional",
                    project_roots.assets.count,
                    project_roots.optional_assets.count);
            }
            JCE_FREE(roots_json);
        }
    }

    for (size_t i = 0; i < scene_paths.n; ++i) {
        char *sid = NULL;
        /* Single-file mode: caller-supplied bundle id takes precedence. */
        if (single_file && opts->single_bundle_id &&
            opts->single_bundle_id[0]) {
            sid = pack_strdup(opts->single_bundle_id);
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
        char *scene_address = make_scene_vpath(scene_paths.items[i],
                                               resource_root);
        for (uint32_t k = 0; k < deps.count; ++k) {
            AssetRef *r = am_get_or_create(&am, deps.items[k].path);
            if (!sv_contains(&r->refs, id_now)) sv_push(&r->refs, id_now);
            if (deps.items[k].bundle && !r->override)
                r->override = pack_strdup(deps.items[k].bundle);
            if (!jce_bundle_edges_add(&dependency_edges, scene_address,
                                      deps.items[k].path, "scene"))
                die("oom");
        }

        /* Depth-1..N recursion: a scene's first-level deps are usually
         * descriptors (materials / prefabs / OBJ meshes / world-streaming
         * scene fragments) that themselves reference further assets.
         * Without this expansion the bundle would ship the .mat.json but
         * not the .png it names, the .obj but not the .mtl/.jpg the OBJ
         * wires up via `mtllib` and `map_*`, or a streaming chunk's
         * .scene.json fragment but none of the meshes/textures the
         * fragment spawns.  We expand iteratively up to a small depth cap
         * so cycles can't run away; the per-scene dep list doubles as the
         * visited set (stage_push_dep dedups, so a fragment cycle simply
         * stops growing).
         *
         * Three descriptor formats handled:
         *   - *.json   → cJSON walk via jce_bundle_deps_scan (covers
         *                .scene.json streaming fragments, incl. their own
         *                nested "streaming" chunk tables)
         *   - *.obj    → parse `mtllib …` lines, push referenced MTLs
         *   - *.mtl    → parse `map_*` / `bump` / `disp` / `decal` / `refl`
         *                lines, push the texture paths
         *
         * Paths inside OBJ/MTL are typically relative to the file that
         * names them — we resolve them against that file's directory so
         * the bundle stores vpaths the runtime VFS can actually open. */
        {
            size_t    start_idx = 0;
            DepStage st = {
                .deps = &deps,
                .am = &am,
                .edges = &dependency_edges,
                .scene_id = id_now,
                .parent = scene_address,
                .origin = "scene"
            };

            if (have_project_roots) {
                st.parent = scene_address;
                st.origin = "project_root";
                stage_dependency_document(&st, &project_roots,
                    "bundle_roots.json", resource_root,
                    opts->resolve_fn, opts->resolve_user);
            }

            /* The dep list is also the visited set, so this computes the full
             * fixed-point closure without an arbitrary depth limit. */
            while (start_idx < deps.count) {
                uint32_t expand_until = deps.count;
                bool grew = false;
                for (uint32_t k = (uint32_t)start_idx; k < expand_until; ++k) {
                    const char *dp = deps.items[k].path;
                    if (!dp) continue;
                    st.parent = dp;
                    st.origin = "dependency_document";

                    /* Every asset may declare runtime-only references in a
                     * sibling `<asset>.deps.json`.  This descriptor is a cook
                     * input and is deliberately not staged into the output. */
                    {
                        char deps_path[1536];
                        int written = snprintf(deps_path, sizeof(deps_path),
                                               "%s.deps.json", dp);
                        if (written > 0 &&
                            (size_t)written < sizeof(deps_path)) {
                            size_t deps_size = 0;
                            uint8_t *deps_json = read_asset(
                                deps_path, resource_root, opts->resolve_fn,
                                opts->resolve_user, NULL, &deps_size);
                            if (deps_json) {
                                JceBundleDependencyDocument document = {0};
                                if (!jce_bundle_deps_parse_document(
                                        (const char *)deps_json, deps_size,
                                        &document)) {
                                    ERR("invalid dependency contract: %s",
                                        deps_path);
                                    dependency_errors = 1;
                                } else {
                                    uint32_t before = deps.count;
                                    stage_dependency_document(
                                        &st, &document, deps_path,
                                        resource_root, opts->resolve_fn,
                                        opts->resolve_user);
                                    grew = deps.count > before || grew;
                                }
                                jce_bundle_deps_document_free(&document);
                                JCE_FREE(deps_json);
                            }
                        }
                    }

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
                        st.parent = dp;
                        st.origin = "descriptor";
                        JceBundleDepList sub = {0};
                        bool parsed = jce_bundle_deps_scan((const char *)child_buf,
                                                           child_sz, &sub);
                        if (parsed) {
                            for (uint32_t s = 0; s < sub.count; ++s) {
                                const char *sp = sub.items[s].path;
                                if (!sp || !sp[0]) continue;
                                /* Loaders resolve a descriptor's relative
                                 * refs against the descriptor's own dir
                                 * (.mat.json textures, .tilemap.json
                                 * tilesets …), while authors equally write
                                 * resource-root-relative paths.  Stage
                                 * whichever candidates actually resolve; if
                                 * neither does, stage the verbatim ref so
                                 * the missing-asset error names what the
                                 * descriptor asked for.  The archive's
                                 * canonical normalisation (jce_archive_
                                 * path.c) hashes "<dir>/../x" and "x"
                                 * identically, so runtime lookups succeed
                                 * for either spelling. */
                                bool staged = false;
                                if (base_dir[0] && !pack_is_abs_path(sp)) {
                                    char joined[1536];
                                    snprintf(joined, sizeof(joined),
                                             "%s/%s", base_dir, sp);
                                    if (strcmp(joined, sp) != 0 &&
                                        can_read_asset(joined, resource_root,
                                                       opts->resolve_fn,
                                                       opts->resolve_user,
                                                       NULL)) {
                                        grew = stage_push_dep(&st, joined,
                                                  sub.items[s].bundle) || grew;
                                        staged = true;
                                    }
                                }
                                if (can_read_asset(sp, resource_root,
                                                   opts->resolve_fn,
                                                   opts->resolve_user, NULL)) {
                                    grew = stage_push_dep(&st, sp,
                                              sub.items[s].bundle) || grew;
                                    staged = true;
                                }
                                if (!staged)
                                    grew = stage_push_dep(&st, sp,
                                              sub.items[s].bundle) || grew;
                            }
                        }
                        jce_bundle_deps_free(&sub);
                    } else {
                        st.parent = dp;
                        st.origin = "descriptor";
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
                                     if (can_read_asset(joined, resource_root,
                                                        opts->resolve_fn,
                                                        opts->resolve_user,
                                                        NULL)) {
                                         grew = stage_push_dep(&st, joined,
                                                               NULL) || grew;
                                     } else {
                                         pack_log_warn(
                                             "optional OBJ material library missing: %s (referenced by %s)",
                                             joined, dp);
                                     }
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
                                grew = stage_push_dep(&st, joined, NULL) || grew;
                            }
                        }
                    }
                    JCE_FREE(child_buf);
                }
                start_idx = expand_until;
                (void)grew;
            }

            /* ── Convention sidecars ────────────────────────────────
             * Files referenced by NAMING CONVENTION rather than by a
             * JSON key.  All existence-gated so a missing optional
             * sidecar can never fail the bundle:
             *   <model>.anim.json   frame-event tables consumed by the
             *                       scene renderer (sr_anim_events_load)
             *   <model>.jcol        cooked collider blob consumed by the
             *                       runtime (rt_spawn_cooked_body)
             *   <scene>.navmesh.bin editor-baked navmesh, same basename
             *                       as the scene file.
             * (.terrain.json → .terrain.bin is handled inside the deps
             * scanner itself since that pair is never optional; a
             * texture's .import.json is cook-time-only input and is
             * baked into the cooked bytes, so it never ships.) */
            {
                uint32_t snap = deps.count;
                char side[1408];
                for (uint32_t k = 0; k < snap; ++k) {
                    const char *dp = deps.items[k].path;
                    if (!dp || !dp[0]) continue;
                    if (jce_bundle_classify_cook(dp) != COOK_CLASS_MODEL)
                        continue;
                    st.parent = dp;
                    st.origin = "model_sidecar";
                    snprintf(side, sizeof(side), "%s.anim.json", dp);
                    if (can_read_asset(side, resource_root, opts->resolve_fn,
                                       opts->resolve_user, NULL))
                        (void)stage_push_dep(&st, side, NULL);
                    snprintf(side, sizeof(side), "%s.jcol", dp);
                    if (can_read_asset(side, resource_root, opts->resolve_fn,
                                       opts->resolve_user, NULL))
                        (void)stage_push_dep(&st, side, NULL);

                    /* GLB/glTF/FBX embedded-texture sidecars the model
                     * importer extracts next to the model as
                     * <model_stem>_tex<N>.png/.tga (write_embedded_texture).
                     * They are collected indirectly when a path lands in a
                     * MeshRenderer albedoTex, but a textured model referenced
                     * only by meshPath/modelPath (LOD level, collider mesh,
                     * prefab, bulk-imported scene) leaves them orphaned and
                     * the model ships without its textures.  Harvest them by
                     * the same naming convention, existence-gated, indices
                     * contiguous from 0 (stop at the first wholly-missing
                     * index). */
                    {
                        char stem[1280];
                        snprintf(stem, sizeof(stem), "%s", dp);
                        char *sdot   = strrchr(stem, '.');
                        char *sslash = strrchr(stem, '/');
                        if (sdot && (!sslash || sdot > sslash)) *sdot = '\0';
                        for (int ti = 0; ti < 32; ++ti) {
                            int found = 0;
                            snprintf(side, sizeof(side), "%s_tex%d.png", stem, ti);
                            if (can_read_asset(side, resource_root, opts->resolve_fn,
                                               opts->resolve_user, NULL)) {
                                (void)stage_push_dep(&st, side, NULL); found = 1;
                            }
                            snprintf(side, sizeof(side), "%s_tex%d.tga", stem, ti);
                            if (can_read_asset(side, resource_root, opts->resolve_fn,
                                               opts->resolve_user, NULL)) {
                                (void)stage_push_dep(&st, side, NULL); found = 1;
                            }
                            if (!found) break;
                        }
                    }
                }

                char *svp = make_scene_vpath(scene_paths.items[i],
                                             resource_root);
                if (svp) {
                    st.parent = scene_address;
                    st.origin = "scene_sidecar";
                    size_t ext = scene_suffix_len(svp);
                    size_t sl  = strlen(svp);
                    if (ext && sl > ext &&
                        sl - ext + sizeof(".navmesh.bin") < sizeof(side)) {
                        memcpy(side, svp, sl - ext);
                        memcpy(side + (sl - ext), ".navmesh.bin",
                               sizeof(".navmesh.bin"));
                        if (can_read_asset(side, resource_root,
                                           opts->resolve_fn,
                                           opts->resolve_user, NULL))
                            (void)stage_push_dep(&st, side, NULL);
                    }
                    JCE_FREE(svp);
                }
            }

            /* ── Localization tables (i18n/<locale>.json) ──────────── */
            st.parent = scene_address;
            st.origin = "localization";
            for (size_t li = 0; li < i18n_files.n; ++li)
                (void)stage_push_dep(&st, i18n_files.items[li], NULL);
        }

        jce_bundle_deps_free(&deps);
        JCE_FREE(scene_address);
    }

    jce_bundle_deps_document_free(&project_roots);
    if (dependency_errors) {
        sv_free(&scene_paths);
        sv_free(&scene_ids);
        sv_free(&i18n_files);
        am_free(&am);
        jce_bundle_edges_free(&dependency_edges);
        if (prev_catalog_j) cJSON_Delete(prev_catalog_j);
        return 1;
    }

    ExternalMap emap = {0};
    for (size_t i = 0; i < am.n; ++i) {
        AssetRef *r = &am.items[i];
        if (!r->asset || !pack_is_abs_path(r->asset))
            continue;

        char abs[1280];
        snprintf(abs, sizeof(abs), "%s", r->asset);
        normalise_sep(abs);

        const char *vp = ext_register(&emap, abs);
        if (!vp)
            continue;

        LOG("external asset virtualised: %s -> %s", r->asset, vp);
        if (!jce_bundle_edges_replace_path(&dependency_edges, r->asset, vp))
            die("oom");
        JCE_FREE(r->asset);
        r->asset = pack_strdup(vp);
    }

    if (!jce_bundle_edges_normalize_and_sort(&dependency_edges)) {
        sv_free(&scene_paths);
        sv_free(&scene_ids);
        sv_free(&i18n_files);
        am_free(&am);
        jce_bundle_edges_free(&dependency_edges);
        ext_free(&emap);
        if (prev_catalog_j) cJSON_Delete(prev_catalog_j);
        return 1;
    }

    BundleVec bundles = {0};
    for (size_t i = 0; i < scene_paths.n; ++i) {
        Bundle *b = bv_create(&bundles, scene_ids.items[i],
                              JCE_BUNDLE_KIND_SCENE);
        b->scene_path     = make_scene_vpath(scene_paths.items[i],
                                             resource_root);
        b->scene_src_path = pack_strdup(scene_paths.items[i]);
        if (!b->scene_src_path) die("oom");
    }

    for (size_t i = 0; i < am.n; ++i) {
        AssetRef *r = &am.items[i];
        const char *target_id = NULL;

        /* A standalone bundle is a closed deployment unit.  Partitioning
         * tags remain useful metadata in catalog builds, but they must never
         * split assets out of the one requested file. */
        if (single_file) {
            Bundle *only = bv_get(&bundles, scene_ids.items[0]);
            if (only) bundle_add_asset(only, r->asset);
            continue;
        }

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
    int had_errors = 0;

    for (size_t bi = 0; bi < bundles.n; ++bi) {
        Bundle *b = &bundles.items[bi];
        if (b->assets.n == 0 && !b->scene_path) continue;
        int bundle_errors = 0;

        XXH3_state_t *xs = XXH3_createState();
        XXH3_64bits_reset(xs);
        /* P0-build-bundles-cook: fold the cook flag + target platform into
         * the bundle input hash so toggling cooking or retargeting the
         * platform invalidates the incremental-cache reuse. */
        {
            uint8_t cook_flag = opts->cook_assets ? 1u : 0u;
            int     cook_plat = opts->target_platform;
            uint32_t writer_revision = 2u;
            XXH3_64bits_update(xs, &cook_flag, sizeof(cook_flag));
            XXH3_64bits_update(xs, &cook_plat, sizeof(cook_plat));
            XXH3_64bits_update(xs, &writer_revision,
                               sizeof(writer_revision));
        }
        /* Encryption state busts the incremental cache: fold the encrypt
         * flag and a key FINGERPRINT (never the key itself) into the input
         * hash so toggling encryption — or rotating the key — rebuilds
         * every bundle instead of reusing stale (differently-encrypted)
         * .prev artifacts. */
        {
            uint8_t  enc_flag = (opts->encrypt && opts->encryption_key) ? 1u : 0u;
            uint64_t key_fp   = enc_flag
                ? (uint64_t)XXH3_64bits(opts->encryption_key, 32) : 0u;
            XXH3_64bits_update(xs, &enc_flag, sizeof(enc_flag));
            XXH3_64bits_update(xs, &key_fp, sizeof(key_fp));
        }
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
            } else {
                char missing[JCE_BUNDLE_CANONICAL_PATH_CAP];
                char root[JCE_BUNDLE_CANONICAL_PATH_CAP];
                char chain[1536];
                const char *missing_address = b->assets.items[i];
                const char *root_address = b->scene_path;
                if (canonical_entry_path(b->assets.items[i], missing,
                                         sizeof(missing)) > 0)
                    missing_address = missing;
                if (b->scene_path &&
                    canonical_entry_path(b->scene_path, root,
                                         sizeof(root)) > 0)
                    root_address = root;
                jce_bundle_edges_format_chain(&dependency_edges, root_address,
                                              missing_address, chain,
                                              sizeof(chain));
                ERR("missing asset %s (dependency chain: %s; bundle %s)",
                    b->assets.items[i], chain[0] ? chain : missing_address,
                    b->id);
                bundle_errors = 1;
                had_errors = 1;
            }
            /* P0-build-bundles-cook: fold the sibling import.json so editing
             * a preset (target_format/max_size/...) invalidates the cache. */
            if (opts->cook_assets) {
                char sp[1408];
                snprintf(sp, sizeof(sp), "%s.import.json", b->assets.items[i]);
                size_t isz = 0;
                uint8_t *iraw = read_asset(sp, resource_root,
                                           opts->resolve_fn, opts->resolve_user,
                                           &emap, &isz);
                if (iraw) {
                    uint64_t ih = XXH3_64bits(iraw, isz);
                    XXH3_64bits_update(xs, &ih, sizeof(ih));
                    JCE_FREE(iraw);
                }
            }
        }
        for (size_t i = 0; i < dependency_edges.n; ++i) {
            const JceBundleDependencyEdge *edge =
                &dependency_edges.items[i];
            if (!bundle_owns_address(b, edge->from))
                continue;
            XXH3_64bits_update(xs, edge->from, strlen(edge->from));
            XXH3_64bits_update(xs, edge->to, strlen(edge->to));
            XXH3_64bits_update(xs, edge->origin, strlen(edge->origin));
        }
        uint64_t input_hash = XXH3_64bits_digest(xs);
        XXH3_freeState(xs);

        const char *prev_h = jce_bundle_previous_build_hash(prev_catalog_j,
                                                            b->id);
        const char *prev_f = jce_bundle_previous_file(prev_catalog_j, b->id);
        char prev_h_str[17] = {0};
        snprintf(prev_h_str, sizeof(prev_h_str), "%016llx",
                 (unsigned long long)input_hash);

        char *short_h = short_hash(input_hash);
        char fname[256];
        if (single_file) {
            /* Stable, predictable filename so callers can re-deploy
             * without consulting a catalog. */
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

        if (bundle_errors) {
            (void)jce_fs_host_remove_file(out_path);
            (void)jce_fs_host_remove_file(sidecar_path);
            continue;
        }

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
                    }
                    CatalogEntry *ce = jce_bundle_catalog_entry_create(&catalog);
                    if (!ce) die("oom");
                    ce->id   = pack_strdup(b->id);
                    ce->file = pack_strdup(fname);
                    ce->kind = pack_strdup(b->kind);
                    if (b->scene_path) ce->scene_path = pack_strdup(b->scene_path);
                    ce->content_hash = hex16(XXH3_64bits(blob, sz));
                    ce->build_hash = pack_strdup(prev_h_str);
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
                            const cJSON *enc = cJSON_GetObjectItemCaseSensitive(
                                sj, JCE_BUNDLE_KEY_ENCRYPTED);
                            ce->encrypted = cJSON_IsTrue(enc);
                            const cJSON *assets = cJSON_GetObjectItemCaseSensitive(
                                sj, JCE_BUNDLE_KEY_ASSETS);
                            if (cJSON_IsArray(assets)) {
                                const cJSON *ae;
                                cJSON_ArrayForEach(ae, assets) {
                                    const cJSON *ap = cJSON_GetObjectItemCaseSensitive(ae, JCE_BUNDLE_KEY_ASSET_PATH);
                                    const cJSON *az = cJSON_GetObjectItemCaseSensitive(ae, JCE_BUNDLE_KEY_ASSET_SIZE);
                                    const cJSON *ah = cJSON_GetObjectItemCaseSensitive(ae, JCE_BUNDLE_KEY_ASSET_HASH);
                                    if (!(ap && cJSON_IsString(ap))) continue;
                                    ReportEntry *re =
                                        jce_bundle_report_entry_create(
                                            &ce->entries);
                                    if (!re) die("oom");
                                    re->path = pack_strdup(ap->valuestring);
                                    re->size = (az && cJSON_IsNumber(az)) ? (uint64_t)az->valuedouble : 0;
                                    re->hash = (ah && cJSON_IsString(ah)) ? pack_strdup(ah->valuestring) : pack_strdup("");
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
                bundle_errors = 1;
                had_errors = 1;
            } else {
                size_t rsz = 0;
                uint8_t *rraw = ext_rewrite_asset(b->scene_path, sraw, ssz,
                                                  &emap, &rsz);
                entries[actual].vpath    = pack_strdup(b->scene_path);
                entries[actual].raw      = rraw ? rraw : sraw;
                entries[actual].raw_size = rraw ? rsz : ssz;
                actual++;
            }
        }

        /* Phase 1 (driver, serial): read + rewrite + pre-assign output slots.
         * These touch the per-thread longjmp/log machinery (read_asset ERR,
         * ext_rewrite_asset die-on-OOM, pack_strdup die) so they must NOT run
         * on the worker pool.  Cookable assets are queued; the slot's raw ptr
         * doubles as the cook input (cook_asset frees it and returns the cooked
         * buffer, which phase 3 stores back). */
        CookJob *cook_jobs = (CookJob *)JCE_CALLOC(b->assets.n > 0 ? b->assets.n : 1,
                                                   sizeof(CookJob));
        if (!cook_jobs) die("oom");
        size_t njobs = 0;

        for (size_t i = 0; i < b->assets.n; ++i) {
            size_t sz = 0;
            uint8_t *raw = read_asset(b->assets.items[i], resource_root,
                                      opts->resolve_fn, opts->resolve_user,
                                      &emap, &sz);
            if (!raw) {
                ERR("missing asset %s (referenced by bundle %s)",
                    b->assets.items[i], b->id);
                bundle_errors = 1;
                had_errors = 1;
                continue;
            }
            size_t rsz = 0;
            uint8_t *rraw = ext_rewrite_asset(b->assets.items[i], raw, sz,
                                              &emap, &rsz);
            uint8_t *abuf = rraw ? rraw : raw;
            size_t   asz  = rraw ? rsz : sz;

            size_t slot = actual++;
            entries[slot].vpath    = pack_strdup(b->assets.items[i]);
            entries[slot].raw      = abuf;   /* provisional; cook overwrites */
            entries[slot].raw_size = asz;

            if (opts->cook_assets &&
                jce_bundle_classify_cook(b->assets.items[i]) !=
                    COOK_CLASS_NONE) {
                cook_jobs[njobs].slot     = slot;
                cook_jobs[njobs].vpath    = b->assets.items[i];
                cook_jobs[njobs].buf      = abuf;
                cook_jobs[njobs].in_size  = asz;
                cook_jobs[njobs].out_size = asz;
                njobs++;
            }
        }

        if (bundle_errors) {
            for (size_t i = 0; i < actual; ++i) {
                JCE_FREE(entries[i].vpath);
                JCE_FREE(entries[i].raw);
            }
            JCE_FREE(cook_jobs);
            JCE_FREE(entries);
            (void)jce_fs_host_remove_file(out_path);
            (void)jce_fs_host_remove_file(sidecar_path);
            continue;
        }

        /* Phase 2 (workers, parallel): cook each queued asset.  emap +
         * resolve_fn/user are read-only here; each job writes only its own
         * CookJob; cook_asset never longjmps; its logs are reconstructed
         * below. */
        if (njobs > 0) {
            CookCtx cctx;
            cctx.jobs            = cook_jobs;
            cctx.resource_root   = resource_root;
            cctx.resolve_fn      = opts->resolve_fn;
            cctx.resolve_user    = opts->resolve_user;
            cctx.emap            = &emap;
            cctx.target_platform = opts->target_platform;

            /* The active VFS is not thread-safe; cook_asset re-enters
             * read_asset()->jce_fs_read_all on workers, so guard VFS reads for
             * the lifetime of the parallel cook (audit Round-3 P1).  NULL on
             * OOM degrades to the prior (racy) behaviour rather than crashing. */
            s_vfs_read_mutex = jce_mutex_create();

            /* A PRIVATE pool, not jce_thread_pool_shared(), for two reasons.
             *
             * Correctness first: the editor runs this whole function on its own
             * "jce-bundle-pack" thread (jce_panel_bundle_browser.cpp) while the
             * frame loop keeps drawing.  enkiTS routes a submission into the
             * pipe of the SUBMITTING thread's scheduler slot, and a thread that
             * never registered with the scheduler reports slot 0 — the slot the
             * frame loop owns.  Those pipes are single-producer, so submitting
             * the shared pool from here would have raced the main thread's own
             * per-frame submissions.  A scheduler created on this thread makes
             * this thread its slot 0, so the writes stay ours.
             *
             * And policy second: cooking a texture or a mesh blocks for far
             * longer than a frame, and jce_thread_pool_parallel_for() waits by
             * running ANY queued task — so a cook job sitting in the shared
             * pool would get executed by the main thread inside a cull.  See
             * the private-pool note in jce_thread.h.
             *
             * Sized by the house policy rather than enkiTS auto-detect: a build
             * must not take every core out from under an editor that is still
             * drawing.  NULL (OOM) degrades to the inline path. */
            JceThreadPool *cook_pool = NULL;
            if (njobs >= 2)
                cook_pool = jce_thread_pool_create(
                    jce_thread_pool_default_workers() + 1);
            if (cook_pool)
                jce_thread_pool_parallel_for(cook_pool, (uint32_t)njobs, 1,
                                             cook_jobs_range, &cctx);
            else
                cook_jobs_range(0u, (uint32_t)njobs, &cctx);
            if (cook_pool) jce_thread_pool_destroy(cook_pool);

            if (s_vfs_read_mutex) {
                jce_mutex_destroy(s_vfs_read_mutex);
                s_vfs_read_mutex = NULL;
            }

            /* Phase 3 (driver, serial): store cooked buffers + replay logs in
             * asset order so the editor's non-thread-safe log sink is touched
             * only from this thread and output stays deterministic. */
            for (size_t k = 0; k < njobs; ++k) {
                CookJob *j = &cook_jobs[k];
                entries[j->slot].raw      = j->buf;
                entries[j->slot].raw_size = j->out_size;
                CookClass cls = jce_bundle_classify_cook(j->vpath);
                const char *kind = (cls == COOK_CLASS_TEXTURE) ? "texture"
                                 : (cls == COOK_CLASS_AUDIO)   ? "audio"
                                 : "model";
                /* Replay the worker's outcome here on the driver, in asset
                 * order: warnings are otherwise lost on worker threads (g_ctx
                 * is thread-local) and successes would double-log when a job
                 * ran on the driver via the cooperative drain (audit Round-3
                 * P2). */
                if (j->status.failed) {
                    pack_log_warn("cook %s failed (%s): %s — shipping raw",
                                  kind, j->vpath,
                                  j->status.err[0] ? j->status.err : "unknown");
                } else if (j->out_size != j->in_size) {
                    LOG("cooked %s %s (%zu -> %zu B)", kind, j->vpath,
                        j->in_size, j->out_size);
                }
            }
        }
        JCE_FREE(cook_jobs);

        for (size_t i = 1; i < actual; ++i)
            entries[i].content_hash = XXH3_64bits(entries[i].raw,
                                                  entries[i].raw_size);

        if (!validate_entry_addresses(entries + 1, actual - 1)) {
            had_errors = 1;
            for (size_t i = 0; i < actual; ++i) {
                JCE_FREE(entries[i].vpath);
                JCE_FREE(entries[i].raw);
            }
            JCE_FREE(entries);
            (void)jce_fs_host_remove_file(out_path);
            (void)jce_fs_host_remove_file(sidecar_path);
            continue;
        }

        const bool enc_now = opts->encrypt && opts->encryption_key != NULL;

        size_t mlen = 0;
        char *mtext = jce_bundle_build_manifest(
            b, entries + 1, actual - 1, input_hash, catalog_version,
            enc_now, opts->cook_assets, opts->target_platform,
            &dependency_edges, &mlen);
        if (!mtext)
            die("cannot build bundle manifest");
        entries[manifest_slot].vpath    = pack_strdup(JCE_BUNDLE_MANIFEST_VPATH);
        entries[manifest_slot].raw      = (uint8_t *)mtext;
        entries[manifest_slot].raw_size = mlen;

        size_t pak_size = 0;
        uint8_t *pak = build_jbundle(entries, actual, zstd_level,
                                     enc_now, opts->encryption_key, b->id,
                                     &pak_size);
        uint64_t archive_hash = XXH3_64bits(pak, pak_size);
        size_t sidecar_len = 0;
        char *sidecar_text = jce_bundle_build_sidecar(
            mtext, mlen, archive_hash, pak_size, &sidecar_len);
        if (!sidecar_text)
            die("cannot build bundle sidecar");

        if (!write_file(out_path, pak, pak_size)) {
            ERR("cannot write %s", out_path);
            had_errors = 1;
            (void)jce_fs_host_remove_file(out_path);
            (void)jce_fs_host_remove_file(sidecar_path);
            for (size_t i = 0; i < actual; ++i) {
                JCE_FREE(entries[i].vpath);
                JCE_FREE(entries[i].raw);
            }
            JCE_FREE(entries);
            JCE_FREE(pak);
            JCE_FREE(sidecar_text);
            continue;
        } else {
            LOG("built %s (%zu B, %zu asset%s)", fname, pak_size,
                actual - 1, (actual - 1) == 1 ? "" : "s");
        }
        if (!write_file(sidecar_path, sidecar_text, sidecar_len)) {
            ERR("cannot write %s", sidecar_path);
            had_errors = 1;
            (void)jce_fs_host_remove_file(out_path);
            (void)jce_fs_host_remove_file(sidecar_path);
            for (size_t i = 0; i < actual; ++i) {
                JCE_FREE(entries[i].vpath);
                JCE_FREE(entries[i].raw);
            }
            JCE_FREE(entries);
            JCE_FREE(pak);
            JCE_FREE(sidecar_text);
            continue;
        }

        CatalogEntry *ce = jce_bundle_catalog_entry_create(&catalog);
        if (!ce) die("oom");
        ce->id   = pack_strdup(b->id);
        ce->file = pack_strdup(fname);
        ce->kind = pack_strdup(b->kind);
        if (b->scene_path) ce->scene_path = pack_strdup(b->scene_path);
        ce->content_hash = hex16(archive_hash);
        ce->build_hash = pack_strdup(prev_h_str);
        ce->size = pak_size;
        ce->encrypted = enc_now;
        for (size_t d = 0; d < b->deps.n; ++d)
            sv_push(&ce->deps, b->deps.items[d]);
        /* Capture per-asset entries for the build report.  Skip the
         * manifest pseudo-entry — it's an implementation detail of the
         * pak format, not a user-visible asset. */
        for (size_t i = 0; i < actual; ++i) {
            const PakEntry *pe = &entries[i];
            if (strcmp(pe->vpath, JCE_BUNDLE_MANIFEST_VPATH) == 0) continue;
            ReportEntry *re = jce_bundle_report_entry_create(&ce->entries);
            if (!re) die("oom");
            char canonical[JCE_BUNDLE_CANONICAL_PATH_CAP];
            if (canonical_entry_path(pe->vpath, canonical,
                                     sizeof(canonical)) == 0)
                die("invalid report asset address");
            re->path = pack_strdup(canonical);
            re->size = (uint64_t)pe->raw_size;
            re->hash = hex16(pe->content_hash);
        }
        total_bytes += pak_size;

        for (size_t i = 0; i < actual; ++i) {
            JCE_FREE(entries[i].vpath);
            JCE_FREE(entries[i].raw);
        }
        JCE_FREE(entries);
        JCE_FREE(pak);
        JCE_FREE(sidecar_text);
    }

    int graph_ok = 1;
    if (!had_errors && catalog.n > 0) {
        size_t graph_len = 0;
        char *graph_json = jce_bundle_build_graph_json(
            &catalog, opts->cook_assets, opts->target_platform,
            &dependency_edges, &graph_len);
        char graph_path[1280];
        snprintf(graph_path, sizeof(graph_path), "%s/%s",
                 out_dir, JCE_BUNDLE_GRAPH_NAME);
        graph_ok = graph_json && write_file(graph_path, graph_json, graph_len);
        if (!graph_ok)
            ERR("cannot write %s", graph_path);
        else
            LOG("content graph written: %s", graph_path);
        JCE_FREE(graph_json);
        if (!graph_ok) {
            for (size_t i = 0; i < catalog.n; ++i) {
                char bundle_path[1280];
                char sidecar_path[1280];
                snprintf(bundle_path, sizeof(bundle_path), "%s/%s",
                         out_dir, catalog.items[i].file);
                snprintf(sidecar_path, sizeof(sidecar_path), "%s%s",
                         bundle_path, ".json");
                (void)jce_fs_host_remove_file(bundle_path);
                (void)jce_fs_host_remove_file(sidecar_path);
            }
        }
    }

    int cat_ok = 1;
    if (single_file) {
        /* Single-file mode: no catalog.  Report the produced bundle. */
        if (catalog.n == 1) {
            pack_log_ok("packed %s -> %s (%.2f KB)",
                        catalog.items[0].id, catalog.items[0].file,
                        (double)total_bytes / 1024.0);
        } else if (catalog.n == 0) {
            if (!had_errors)
                ERR("single-file mode: no bundle produced");
            cat_ok = 0;
        }
    } else {
        size_t cjson_len = 0;
        char *cat_json = jce_bundle_build_catalog_json(
            &catalog, catalog_version, opts->cook_assets,
            opts->target_platform, &cjson_len);
        if (!cat_json)
            die("cannot build bundle catalog");
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

        /* P3-A.4 — emit build_report.json for the editor's Build Report
         * panel.  Best-effort: failure to write the report is not fatal
         * to the build itself; just log a warning. */
        {
            size_t rlen = 0;
            char *rjson = jce_bundle_build_report_json(&catalog, &rlen);
            if (rjson) {
                char rpath[1280];
                snprintf(rpath, sizeof(rpath), "%s/%s",
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
    sv_free(&i18n_files);
    am_free(&am);
    jce_bundle_edges_free(&dependency_edges);
    bv_free(&bundles);
    jce_bundle_catalog_entries_free(&catalog);
    ext_free(&emap);
    if (prev_catalog_j) cJSON_Delete(prev_catalog_j);
    return (cat_ok && graph_ok && !had_errors) ? 0 : 1;
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
    PackCtx *prev = pack_ctx_get();
    pack_ctx_set(&ctx);
    int rc;
    if (setjmp(ctx.jmp) == 0) {
        rc = run_build_impl(opts);
    } else {
        rc = ctx.error_code ? ctx.error_code : -1;
    }
    pack_ctx_set(prev);
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
    PackCtx *prev = pack_ctx_get();
    pack_ctx_set(&ctx);
    int rc;
    if (setjmp(ctx.jmp) == 0) {
        rc = run_diff_impl(old_catalog_path, new_catalog_path, out_diff_path);
    } else {
        rc = ctx.error_code ? ctx.error_code : -1;
    }
    pack_ctx_set(prev);
    return rc;
}
