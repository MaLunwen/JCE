/*
 * jce_filesystem_host.c  Host (real OS) filesystem helpers.
 *
 * SDL3-only: SDL_GetPathInfo, SDL_CreateDirectory, SDL_RemovePath,
 * SDL_EnumerateDirectory.  No <windows.h> / <dirent.h> / <sys/stat.h>.
 */

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_log.h>

#include "jce_memory.h"
#include "jce_trace_scope_internal.h"
#include "jce_trace_capture_internal.h"
#include "jce_filesystem_host_internal.h"
#include "jce_filesystem_read_internal.h"
#include "os/platform/jce_filesystem_presence_native_internal.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_error.h>
#include <limits.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

JceFsReadFn jce__fs_active_reader(void);

/* One host-read service: active routing/probe tuple and an injected borrow of
 * the Engine-owned diagnostic context. VFS setters change only their tuple;
 * they never reset/detach the independent Engine-session borrow. Quiescent
 * begin/end own its publication; synchronous reads cannot outlive that owner. */
static struct {
    JceFileSystem *fs;
    JceFsReadFn reader;
    JceFsActivePolicy policy;
    JceFsPresenceFnInternal probe;
    JceFsHostOpenFailureContext *open_detail;
} s_host_read_service;

/* Engine configures this only at admitted boot and after diagnostic joins.
 * Log payloads are bounded prefixes, not a lossless path/error transport. */
#ifndef JCE_LOG_NONE
enum { JCE_FS_OPEN_DETAIL_PREFIX = 128, JCE_FS_OPEN_DETAIL_GROUPS = 64 };
typedef struct JceFsOpenFailureText {
    size_t length;
    unsigned captured;
    unsigned char prefix[JCE_FS_OPEN_DETAIL_PREFIX];
} JceFsOpenFailureText;

static void s_open_detail_copy(JceFsOpenFailureText *out, const char *text)
{
    out->length = strlen(text);
    out->captured = out->length < JCE_FS_OPEN_DETAIL_PREFIX
        ? (unsigned)out->length : JCE_FS_OPEN_DETAIL_PREFIX;
    memcpy(out->prefix, text, out->captured);
}
static bool s_open_detail_reserve(JceFsHostOpenFailureContext *context)
{
    int admitted = SDL_GetAtomicInt(&context->admitted);
    while (admitted < JCE_FS_OPEN_DETAIL_GROUPS) {
        if (SDL_CompareAndSwapAtomicInt(&context->admitted,
                                        admitted, admitted + 1)) return true;
        admitted = SDL_GetAtomicInt(&context->admitted);
    }
    int refused = SDL_GetAtomicInt(&context->refused);
    while (refused < INT_MAX) {
        if (SDL_CompareAndSwapAtomicInt(&context->refused,
                                        refused, refused + 1)) break;
        refused = SDL_GetAtomicInt(&context->refused);
    }
    return false;
}
static void s_open_detail_emit(const JceTraceScope *scope, uint64_t hash,
        uint32_t generation, const char *op, const char *field,
        const JceFsOpenFailureText *text)
{
    static const char digits[] = "0123456789abcdef";
    char hex[2 * JCE_FS_OPEN_DETAIL_PREFIX + 1];
    for (unsigned i = 0; i < text->captured; ++i) {
        unsigned char byte = text->prefix[i];
        hex[2*i] = digits[byte >> 4];
        hex[2*i+1] = digits[byte & 15];
    }
    hex[2 * text->captured] = '\0';
    LOG_WARN("FS", "fs.open_failure.v1 task=%016llx parent=%016llx generation=%u "
        "path_hash=%016llx op=%s field=%s bytes=%llu captured=%u truncated=%u "
        "optional=unknown hex=%s", (unsigned long long)scope->id,
        (unsigned long long)scope->parent, (unsigned)generation,
        (unsigned long long)hash, op, field, (unsigned long long)text->length,
        text->captured, (unsigned)(text->length != text->captured), hex);
}
#endif

bool jce__fs_host_open_failure_detail_begin(JceFsHostOpenFailureContext *context,
                                           bool enabled)
{
#ifndef JCE_LOG_NONE
    if (!context || s_host_read_service.open_detail) return false;
    if (!enabled) return true;
    SDL_SetAtomicInt(&context->admitted, 0);
    SDL_SetAtomicInt(&context->refused, 0);
    s_host_read_service.open_detail = context;
    return true;
#else
    (void)context; (void)enabled;
    return false;
#endif
}
bool jce__fs_host_open_failure_detail_end(JceFsHostOpenFailureContext *context)
{
#ifndef JCE_LOG_NONE
    if (!context || s_host_read_service.open_detail != context) return false;
    s_host_read_service.open_detail = NULL;
    int admitted = SDL_GetAtomicInt(&context->admitted);
    int refused = SDL_GetAtomicInt(&context->refused);
    if (jce_trace_suppress_begin()) {
        LOG_WARN("FS", "fs.open_failure.summary attempted=%llu refused=%u "
            "refused_saturated=%u delivery=unavailable",
            (unsigned long long)((uint64_t)(unsigned)admitted + (unsigned)refused),
            (unsigned)refused, (unsigned)(refused == INT_MAX));
        (void)jce_trace_suppress_end();
    }
    return true;
#else
    (void)context;
    return false;
#endif
}
static JceFsHostOpenFailureContext *s_open_detail_borrow(
        const JceFsHostReadTrace *detail, const JceTraceScope *scope)
{
#ifndef JCE_LOG_NONE
    return detail && scope->id ? s_host_read_service.open_detail : NULL;
#else
    (void)detail; (void)scope;
    return NULL;
#endif
}
static void s_open_detail_failure(JceFsHostOpenFailureContext *context,
        const char *path, const char *op,
        const JceTraceScope *scope, uint64_t hash)
{
#ifndef JCE_LOG_NONE
    JceFsOpenFailureText error, request;
    /* First dependency after failed OPEN: copy this thread's provider error
     * before any SDL/trace/log/allocator dependency can replace its storage. */
    s_open_detail_copy(&error, SDL_GetError());
    s_open_detail_copy(&request, path);
    const uint32_t generation = jce_trace_capture_generation();
    if (!s_open_detail_reserve(context) || !jce_trace_suppress_begin()) return;
    s_open_detail_emit(scope, hash, generation, op, "path", &request);
    s_open_detail_emit(scope, hash, generation, op, "error", &error);
    (void)jce_trace_suppress_end();
#else
    (void)context; (void)path; (void)op; (void)scope; (void)hash;
#endif
}

static bool s_empty(const char *p) { return !p || p[0] == '\0'; }

bool jce_fs_host_exists_file(const char *path)
{
    if (s_empty(path)) return false;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return false;
    return info.type == SDL_PATHTYPE_FILE;
}

bool jce_fs_host_exists_dir(const char *path)
{
    if (s_empty(path)) return false;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return false;
    return info.type == SDL_PATHTYPE_DIRECTORY;
}

static bool jce_fs_host_create_directory_impl(const char *path)
{
    if (s_empty(path)) return false;
    /* SDL_CreateDirectory has mkdir -p semantics already. */
    if (SDL_CreateDirectory(path)) return true;
    /* Treat already-exists as success. */
    return jce_fs_host_exists_dir(path);
}

static bool jce_fs_host_remove_file_impl(const char *path)
{
    if (s_empty(path)) return false;
    if (!jce_fs_host_exists_file(path)) {
        /* Already gone — treat as success. */
        SDL_PathInfo info;
        return !SDL_GetPathInfo(path, &info);
    }
    return SDL_RemovePath(path);
}

/* ── Recursive walk ───────────────────────────────────────────────── */

typedef struct WalkCtx {
    JceFsHostWalkFn cb;
    void           *user;
    bool            stop;
} WalkCtx;

static bool s_join(char *out, size_t cap,
                   const char *base, const char *name)
{
    size_t blen = strlen(base);
    size_t nlen = strlen(name);
    bool need_sep = blen > 0 && base[blen - 1] != '/' && base[blen - 1] != '\\';
    size_t need = blen + (need_sep ? 1u : 0u) + nlen + 1;
    if (need > cap) return false;
    memcpy(out, base, blen);
    if (need_sep) out[blen++] = '/';
    memcpy(out + blen, name, nlen + 1);
    return true;
}

static SDL_EnumerationResult s_enum_cb(void *userdata,
                                       const char *dirname,
                                       const char *fname)
{
    WalkCtx *ctx = (WalkCtx *)userdata;
    if (!ctx || ctx->stop) return SDL_ENUM_SUCCESS;
    if (!fname || fname[0] == '\0') return SDL_ENUM_CONTINUE;

    char full[1024];
    if (!s_join(full, sizeof(full), dirname, fname))
        return SDL_ENUM_CONTINUE;

    SDL_PathInfo info;
    bool         is_dir = false;
    if (SDL_GetPathInfo(full, &info))
        is_dir = (info.type == SDL_PATHTYPE_DIRECTORY);

    if (ctx->cb && !ctx->cb(full, is_dir, ctx->user)) {
        ctx->stop = true;
        return SDL_ENUM_SUCCESS;
    }

    if (is_dir) {
        SDL_EnumerateDirectory(full, s_enum_cb, ctx);
        if (ctx->stop) return SDL_ENUM_SUCCESS;
    }
    return SDL_ENUM_CONTINUE;
}

bool jce_fs_host_walk(const char *root, JceFsHostWalkFn cb, void *user)
{
    if (s_empty(root) || !cb) return false;
    if (!jce_fs_host_exists_dir(root)) return false;
    WalkCtx ctx;
    ctx.cb   = cb;
    ctx.user = user;
    ctx.stop = false;
    return SDL_EnumerateDirectory(root, s_enum_cb, &ctx);
}

typedef struct {
    JceFsHostListFn cb;
    void           *user;
    bool            stop;
} ListCtx;

static SDL_EnumerationResult s_list_cb(void *userdata,
                                       const char *dirname,
                                       const char *fname)
{
    ListCtx *ctx = (ListCtx *)userdata;
    if (!ctx || ctx->stop) return SDL_ENUM_SUCCESS;
    if (!fname || fname[0] == '\0') return SDL_ENUM_CONTINUE;

    char full[1024];
    bool is_dir = false;
    if (s_join(full, sizeof(full), dirname, fname)) {
        SDL_PathInfo info;
        if (SDL_GetPathInfo(full, &info))
            is_dir = (info.type == SDL_PATHTYPE_DIRECTORY);
    }
    if (ctx->cb && !ctx->cb(fname, is_dir, ctx->user)) {
        ctx->stop = true;
    }
    return SDL_ENUM_CONTINUE;
}

bool jce_fs_host_list_dir(const char *dir, JceFsHostListFn cb, void *user)
{
    if (s_empty(dir) || !cb) return false;
    if (!jce_fs_host_exists_dir(dir)) return false;
    ListCtx ctx;
    ctx.cb   = cb;
    ctx.user = user;
    ctx.stop = false;
    return SDL_EnumerateDirectory(dir, s_list_cb, &ctx);
}

bool jce_fs_host_get_mtime(const char *path, int64_t *out_epoch_sec)
{
    if (s_empty(path)) return false;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return false;
    /* SDL_PathInfo.modify_time is in nanoseconds since Unix epoch. */
    if (out_epoch_sec)
        *out_epoch_sec = (int64_t)(info.modify_time / 1000000000LL);
    return true;
}

bool jce_fs_host_get_size(const char *path, uint64_t *out_size)
{
    if (s_empty(path)) return false;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return false;
    if (info.type != SDL_PATHTYPE_FILE) return false;
    if (out_size) *out_size = (uint64_t)info.size;
    return true;
}

/* ── Copy / move / unique-name ────────────────────────────────────── */

static bool jce_fs_host_copy_file_impl(const char *src, const char *dst)
{
    if (s_empty(src) || s_empty(dst)) return false;
    return SDL_CopyFile(src, dst);
}

static bool s_ensure_parent_dir(const char *path)
{
    if (s_empty(path)) return false;
    const char *last = NULL;
    for (const char *p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') last = p;
    }
    if (!last || last == path) return true;
    size_t len = (size_t)(last - path);
    if (len >= 1024) return false;
    char parent[1024];
    memcpy(parent, path, len);
    parent[len] = '\0';
    return jce_fs_host_create_directory(parent);
}

typedef struct CopyCtx {
    const char *src_root;
    const char *dst_root;
    bool        ok;
} CopyCtx;

static bool s_copy_walk_cb(const char *path, bool is_dir, void *user)
{
    CopyCtx *ctx = (CopyCtx *)user;
    size_t  rlen = strlen(ctx->src_root);
    if (strncmp(path, ctx->src_root, rlen) != 0) return true;
    const char *rel = path + rlen;
    while (*rel == '/' || *rel == '\\') ++rel;

    char dst[1024];
    if (!s_join(dst, sizeof(dst), ctx->dst_root, rel)) {
        ctx->ok = false;
        return false;
    }
    if (is_dir) {
        if (!jce_fs_host_create_directory(dst)) {
            ctx->ok = false;
            return false;
        }
    } else {
        if (!s_ensure_parent_dir(dst) || !SDL_CopyFile(path, dst)) {
            ctx->ok = false;
            return false;
        }
    }
    return true;
}

static bool jce_fs_host_copy_recursive_impl(const char *src, const char *dst)
{
    if (s_empty(src) || s_empty(dst)) return false;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(src, &info)) return false;

    if (info.type == SDL_PATHTYPE_FILE) {
        if (!s_ensure_parent_dir(dst)) return false;
        return SDL_CopyFile(src, dst);
    }
    if (info.type != SDL_PATHTYPE_DIRECTORY) return false;

    if (!jce_fs_host_create_directory(dst)) return false;
    CopyCtx ctx;
    ctx.src_root = src;
    ctx.dst_root = dst;
    ctx.ok       = true;
    if (!jce_fs_host_walk(src, s_copy_walk_cb, &ctx)) return false;
    return ctx.ok;
}

static bool jce_fs_host_rename_impl(const char *src, const char *dst)
{
    if (s_empty(src) || s_empty(dst)) return false;
    if (!s_ensure_parent_dir(dst)) return false;
    return SDL_RenamePath(src, dst);
}

typedef struct RmCtx {
    bool ok;
} RmCtx;

static bool s_remove_path_if_present(const char *path)
{
    SDL_PathInfo info;

    if (SDL_RemovePath(path))
        return true;
    return !SDL_GetPathInfo(path, &info);
}

static SDL_EnumerationResult s_rm_postorder_cb(void *userdata,
                                               const char *dirname,
                                               const char *fname)
{
    RmCtx *ctx = (RmCtx *)userdata;
    char full[1024];
    SDL_PathInfo info;

    if (!ctx || !ctx->ok)
        return SDL_ENUM_FAILURE;
    if (!fname || fname[0] == '\0')
        return SDL_ENUM_CONTINUE;
    if (!s_join(full, sizeof(full), dirname, fname) ||
        !SDL_GetPathInfo(full, &info)) {
        ctx->ok = false;
        return SDL_ENUM_FAILURE;
    }

    if (info.type == SDL_PATHTYPE_DIRECTORY &&
        !SDL_EnumerateDirectory(full, s_rm_postorder_cb, ctx)) {
        ctx->ok = false;
        return SDL_ENUM_FAILURE;
    }
    if (!ctx->ok || !s_remove_path_if_present(full)) {
        ctx->ok = false;
        return SDL_ENUM_FAILURE;
    }
    return SDL_ENUM_CONTINUE;
}

static bool jce_fs_host_remove_recursive_impl(const char *path)
{
    if (s_empty(path)) return false;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) {
        /* Already gone — treat as success. */
        return true;
    }
    if (info.type != SDL_PATHTYPE_DIRECTORY)
        return s_remove_path_if_present(path);

    RmCtx ctx;
    ctx.ok = true;
    if (!SDL_EnumerateDirectory(path, s_rm_postorder_cb, &ctx) || !ctx.ok)
        return false;
    return s_remove_path_if_present(path);
}

static void s_split_stem_ext(const char *name,
                             char *stem, size_t stem_cap,
                             char *ext,  size_t ext_cap)
{
    const char *dot = NULL;
    for (const char *p = name; *p; ++p) {
        if (*p == '.') dot = p;
    }
    if (dot && dot != name) {
        size_t slen = (size_t)(dot - name);
        if (slen >= stem_cap) slen = stem_cap - 1;
        memcpy(stem, name, slen);
        stem[slen] = '\0';
        (void)snprintf(ext, ext_cap, "%s", dot);
    } else {
        (void)snprintf(stem, stem_cap, "%s", name);
        ext[0] = '\0';
    }
}

bool jce_fs_host_make_unique_path(const char *desired,
                                  char *out, uint32_t out_size)
{
    if (s_empty(desired) || !out || out_size == 0) return false;

    SDL_PathInfo info;
    if (!SDL_GetPathInfo(desired, &info)) {
        size_t need = strlen(desired) + 1;
        if (need > out_size) return false;
        memcpy(out, desired, need);
        return true;
    }

    /* Split desired into <parent>/<stem><ext>. */
    const char *last_sep = NULL;
    for (const char *p = desired; *p; ++p) {
        if (*p == '/' || *p == '\\') last_sep = p;
    }
    const char *name = last_sep ? last_sep + 1 : desired;
    size_t      plen = last_sep ? (size_t)(last_sep - desired) : 0;

    char parent[1024];
    if (plen >= sizeof(parent)) return false;
    if (plen > 0) {
        memcpy(parent, desired, plen);
        parent[plen] = '\0';
    } else {
        parent[0] = '\0';
    }

    char stem[256], ext[64];
    s_split_stem_ext(name, stem, sizeof(stem), ext, sizeof(ext));

    for (int i = 2; i < 10000; ++i) {
        char candidate[1200];
        int  n;
        if (parent[0]) {
            n = snprintf(candidate, sizeof(candidate),
                         "%s/%s (%d)%s", parent, stem, i, ext);
        } else {
            n = snprintf(candidate, sizeof(candidate),
                         "%s (%d)%s", stem, i, ext);
        }
        if (n < 0 || (size_t)n >= sizeof(candidate)) return false;
        SDL_PathInfo cinfo;
        if (!SDL_GetPathInfo(candidate, &cinfo)) {
            size_t need = (size_t)n + 1;
            if (need > out_size) return false;
            memcpy(out, candidate, need);
            return true;
        }
    }
    return false;
}

/* ----------------------------------------------------------------- */
/* One-shot read/write helpers (host-path equivalents of the VFS     */
/*jce_fs_read_all / jce_fs_write_all).                              */
/* ----------------------------------------------------------------- */

/* Only an admitted scope performs hashing or initializes detail metadata.
 * No optional profiler plot, allocation, path text or error text is emitted. */
static JceFsHostReadTrace *s_read_trace_begin(JceFsHostReadTrace *detail,
        JceTraceScope scope, const char *path, bool capped, uint64_t cap)
{
    if (!scope.id) return NULL;
    *detail = (JceFsHostReadTrace){0};
    detail->path_hash = path ? jce_fnv1a64_str(path) : 0;
    if (capped) {
        detail->cap = cap;
        detail->known |= JCE_FS_HOST_READ_KNOWN_CAP;
    }
    return detail;
}
static void s_read_trace_fail(JceFsHostReadTrace *detail, JceFsHostReadFailure stage)
{
    if (detail && detail->failure_stage == JCE_FS_HOST_READ_OK)
        detail->failure_stage = (uint32_t)stage;
}
static void s_read_trace_close(SDL_IOStream *io, JceFsHostReadTrace *detail)
{
    bool closed = SDL_CloseIO(io);
    if (detail) detail->close = closed ? JCE_FS_HOST_READ_CLOSE_OK
                                     : JCE_FS_HOST_READ_CLOSE_FAILED;
}
static void s_read_trace_pair(JceTraceScope scope, const char *hi,
                              const char *lo, uint64_t value)
{
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_CORE, hi,
                                     (int64_t)(uint32_t)(value >> 32), scope.id, scope.parent);
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_CORE, lo,
                                     (int64_t)(uint32_t)value, scope.id, scope.parent);
}
static void s_read_trace_emit(const JceFsHostReadTrace *detail, JceTraceScope scope)
{
    if (!detail) return;
    s_read_trace_pair(scope, "fs.read.path_hash_hi", "fs.read.path_hash_lo", detail->path_hash);
    s_read_trace_pair(scope, "fs.read.total_hi", "fs.read.total_lo", detail->total);
    s_read_trace_pair(scope, "fs.read.requested_hi", "fs.read.requested_lo", detail->requested);
    s_read_trace_pair(scope, "fs.read.cap_hi", "fs.read.cap_lo", detail->cap);
    s_read_trace_pair(scope, "fs.read.actual_hi", "fs.read.actual_lo", detail->actual);
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_CORE, "fs.read.dispatch",
                                     detail->dispatch, scope.id, scope.parent);
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_CORE, "fs.read.known",
                                     detail->known, scope.id, scope.parent);
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_CORE, "fs.read.failure_stage",
                                     detail->failure_stage, scope.id, scope.parent);
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_CORE, "fs.read.close",
                                     detail->close, scope.id, scope.parent);
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_CORE, "fs.read.short",
                                     detail->short_read, scope.id, scope.parent);
}

static void *jce_fs_host_read_all_impl(const char *path, uint64_t *out_size,
        JceFsHostReadTrace *detail, const JceTraceScope *scope)
{
    if (out_size) *out_size = 0;
    if (s_empty(path)) {
        s_read_trace_fail(detail, JCE_FS_HOST_READ_INVALID);
        return NULL;
    }

    /* Active VFS override: try the mounted bundle / pak first.  Allows
     * scene preview from a .jbundle to satisfy asset cache loaders
     * (which call this function with project-relative paths) without
     * touching every loader's read path. */
    {
        JceFileSystem *afs = jce_fs_get_active();
        JceFsReadFn    fn  = jce__fs_active_reader();
        /* Open-coded rather than calling jce_path_is_absolute (audit DUP-044):
         * this TU is compiled into the standalone jce_cook host tool, whose
         * explicit source list in the root CMakeLists.txt does NOT include
         * os/core/jce_path.c, so delegating here would be an unresolved
         * external there.  Keep in step with jce_path_is_absolute. */
        bool absolute = path[0] == '/' || path[0] == '\\' ||
            (((path[0] >= 'A' && path[0] <= 'Z') ||
              (path[0] >= 'a' && path[0] <= 'z')) &&
             path[1] == ':' && (path[2] == '/' || path[2] == '\\'));
        if (!absolute && afs && fn) {
            if (detail) detail->dispatch = JCE_FS_HOST_READ_VFS;
            uint64_t vsz = 0;
            void    *vbuf = fn(afs, path, &vsz);
            if (vbuf) {
                if (detail) {
                    detail->total = detail->requested = detail->actual = vsz;
                    detail->known |= JCE_FS_HOST_READ_KNOWN_TOTAL |
                        JCE_FS_HOST_READ_KNOWN_REQUESTED | JCE_FS_HOST_READ_KNOWN_ACTUAL;
                }
                if (out_size) *out_size = vsz;
                return vbuf;
            }
            if (jce_fs_get_active_policy() == JCE_FS_ACTIVE_ISOLATED) {
                s_read_trace_fail(detail, JCE_FS_HOST_READ_ISOLATED_VFS);
                return NULL;
            }
        }
    }

    if (detail) {
        detail->dispatch = detail->dispatch == JCE_FS_HOST_READ_VFS
            ? JCE_FS_HOST_READ_VFS_THEN_HOST : JCE_FS_HOST_READ_HOST;
        detail->known |= JCE_FS_HOST_READ_KNOWN_ACTUAL;
    }
    JceFsHostOpenFailureContext *const open_detail = s_open_detail_borrow(detail, scope);
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) {
        if (open_detail) s_open_detail_failure(open_detail, path, "read_all", scope, detail->path_hash);
        s_read_trace_fail(detail, JCE_FS_HOST_READ_OPEN);
        return NULL;
    }

    Sint64 sz = SDL_GetIOSize(io);
    if (sz < 0) {
        s_read_trace_fail(detail, JCE_FS_HOST_READ_SIZE);
        s_read_trace_close(io, detail);
        return NULL;
    }
    if (detail) {
        detail->total = detail->requested = (uint64_t)sz;
        detail->known |= JCE_FS_HOST_READ_KNOWN_TOTAL | JCE_FS_HOST_READ_KNOWN_REQUESTED;
    }
    if ((uint64_t)sz > (uint64_t)SIZE_MAX - 1u) {
        s_read_trace_fail(detail, JCE_FS_HOST_READ_LIMIT);
        s_read_trace_close(io, detail);
        return NULL;
    }

    void *buf = JCE_MALLOC((size_t)sz + 1);   /* +1: text-safe sentinel */
    if (!buf) {
        s_read_trace_fail(detail, JCE_FS_HOST_READ_ALLOC);
        s_read_trace_close(io, detail);
        return NULL;
    }

    size_t nread = 0;
    if (sz > 0) {
        nread = SDL_ReadIO(io, buf, (size_t)sz);
        if (detail) {
            detail->actual = (uint64_t)nread;
            detail->short_read = nread == (size_t)sz ? JCE_FS_HOST_READ_FULL
                                                    : JCE_FS_HOST_READ_SHORT_OBSERVED;
        }
        if (nread != (size_t)sz) {
            s_read_trace_fail(detail, JCE_FS_HOST_READ_SHORT);
            JCE_FREE(buf);
            s_read_trace_close(io, detail);
            return NULL;
        }
    }
    ((char *)buf)[nread] = '\0';
    s_read_trace_close(io, detail); /* Observation only; retain the existing return policy. */
    if (out_size) *out_size = nread;
    return buf;
}

SDL_IOStream *jce__fs_host_stream_new(const char *path, bool *created)
{
    if (created) *created = false;
    if (!created || s_empty(path) || !s_ensure_parent_dir(path)) return NULL;
    /* Only this exclusive open admits ownership. Failed stat is not absence. */
    SDL_IOStream *io = SDL_IOFromFile(path, "wbx");
    if (io) *created = true;
    return io;
}

bool jce__fs_host_stream_write(SDL_IOStream *io, const void *data, uint64_t size)
{
    if (!io || (size && !data) || size > SIZE_MAX) return false;
    return !size || SDL_WriteIO(io, data, (size_t)size) == (size_t)size;
}

bool jce__fs_host_stream_close(SDL_IOStream **io)
{
    if (!io || !*io) return false;
    SDL_IOStream *owned = *io;
    *io = NULL; /* SDL closes/frees even when its flush reports failure. */
    return SDL_CloseIO(owned);
}

bool jce__fs_host_reserve_new(const char *path, const void *data,
                             uint64_t size, bool *created)
{
    if (created) *created = false;
    if (!created || s_empty(path) || (size && !data) || size > SIZE_MAX)
        return false;
    SDL_IOStream *io = jce__fs_host_stream_new(path, created);
    if (!io) return false;
    bool ok = jce__fs_host_stream_write(io, data, size);
    if (!jce__fs_host_stream_close(&io)) ok = false;
    return ok;
}

/* Copy details share the existing synchronous service scope and ring. Byte
 * counts are accepted SDL transfers, not durable storage observations. */
typedef struct JceFsHostCopyTrace {
    uint64_t source_hash, destination_hash, total, read, written;
    uint32_t known, created, failure_stage, source_close, destination_close;
} JceFsHostCopyTrace;
enum {
    JCE_FS_HOST_COPY_INVALID = 1, JCE_FS_HOST_COPY_OPEN,
    JCE_FS_HOST_COPY_SIZE, JCE_FS_HOST_COPY_ALLOC, JCE_FS_HOST_COPY_CREATE,
    JCE_FS_HOST_COPY_READ, JCE_FS_HOST_COPY_WRITE, JCE_FS_HOST_COPY_EOF,
    JCE_FS_HOST_COPY_SOURCE_CLOSE, JCE_FS_HOST_COPY_DESTINATION_CLOSE
};
static void s_copy_trace_fail(JceFsHostCopyTrace *detail, uint32_t stage)
{
    if (detail && detail->failure_stage == 0) detail->failure_stage = stage;
}
static void s_copy_trace_emit(const JceFsHostCopyTrace *detail, JceTraceScope scope)
{
    if (!detail) return;
    s_read_trace_pair(scope, "fs.copy.source_hash_hi", "fs.copy.source_hash_lo", detail->source_hash);
    s_read_trace_pair(scope, "fs.copy.destination_hash_hi", "fs.copy.destination_hash_lo", detail->destination_hash);
    s_read_trace_pair(scope, "fs.copy.total_hi", "fs.copy.total_lo", detail->total);
    s_read_trace_pair(scope, "fs.copy.read_hi", "fs.copy.read_lo", detail->read);
    s_read_trace_pair(scope, "fs.copy.written_hi", "fs.copy.written_lo", detail->written);
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_CORE, "fs.copy.known",
                                     detail->known, scope.id, scope.parent);
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_CORE, "fs.copy.created",
                                     detail->created, scope.id, scope.parent);
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_CORE, "fs.copy.failure_stage",
                                     detail->failure_stage, scope.id, scope.parent);
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_CORE, "fs.copy.source_close",
                                     detail->source_close, scope.id, scope.parent);
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_CORE, "fs.copy.destination_close",
                                     detail->destination_close, scope.id, scope.parent);
}
static bool jce_fs_host_copy_file_new_impl(const char *src, const char *dst,
                                           bool *out_created, JceFsHostCopyTrace *detail)
{
    if (out_created) *out_created = false;
    if (!out_created || s_empty(src) || s_empty(dst)) {
        s_copy_trace_fail(detail, JCE_FS_HOST_COPY_INVALID);
        return false;
    }
    SDL_IOStream *source = SDL_IOFromFile(src, "rb");
    if (!source) {
        s_copy_trace_fail(detail, JCE_FS_HOST_COPY_OPEN);
        return false;
    }
    if (detail) detail->known |= 2u; /* Source transfer count observed, even zero. */
    bool ok = false;
    unsigned char *buffer = NULL;
    SDL_IOStream *target = NULL;
    Sint64 source_size = SDL_GetIOSize(source);
    if (source_size < 0) {
        s_copy_trace_fail(detail, JCE_FS_HOST_COPY_SIZE);
        goto cleanup;
    }
    uint64_t remaining = (uint64_t)source_size;
    if (detail) {
        detail->total = remaining;
        detail->known |= 1u;
    }
    if (remaining) {
        buffer = JCE_MALLOC(65536u);
        if (!buffer) {
            s_copy_trace_fail(detail, JCE_FS_HOST_COPY_ALLOC);
            goto cleanup;
        }
    }
    /* The shared exclusive creator is the only destination admission owner. */
    target = jce__fs_host_stream_new(dst, out_created);
    if (detail) detail->created = *out_created;
    if (!target) {
        s_copy_trace_fail(detail, JCE_FS_HOST_COPY_CREATE);
        goto cleanup;
    }
    if (detail) detail->known |= 4u;
    while (remaining) {
        size_t chunk = remaining > 65536u ? 65536u : (size_t)remaining;
        size_t received = SDL_ReadIO(source, buffer, chunk);
        if (detail) detail->read += (uint64_t)received;
        if (received != chunk) {
            s_copy_trace_fail(detail, JCE_FS_HOST_COPY_READ);
            goto cleanup;
        }
        size_t written = SDL_WriteIO(target, buffer, chunk);
        if (detail) detail->written += (uint64_t)written;
        if (written != chunk) {
            s_copy_trace_fail(detail, JCE_FS_HOST_COPY_WRITE);
            goto cleanup;
        }
        remaining -= (uint64_t)chunk;
    }
    /* Growth and terminal IO errors are failures. This is not a snapshot:
     * same-length concurrent source mutation can still be copied. */
    unsigned char trailing;
    size_t extra = SDL_ReadIO(source, &trailing, 1u);
    if (detail) detail->read += (uint64_t)extra;
    if (extra || SDL_GetIOStatus(source) != SDL_IO_STATUS_EOF) {
        s_copy_trace_fail(detail, JCE_FS_HOST_COPY_EOF);
        goto cleanup;
    }
    ok = true;
cleanup:
    if (target) {
        bool destination_closed = jce__fs_host_stream_close(&target);
        if (detail) detail->destination_close = destination_closed ? 1u : 2u;
        if (!destination_closed) {
            s_copy_trace_fail(detail, JCE_FS_HOST_COPY_DESTINATION_CLOSE);
            ok = false;
        }
    }
    bool source_closed = jce__fs_host_stream_close(&source);
    if (detail) detail->source_close = source_closed ? 1u : 2u;
    if (!source_closed) {
        s_copy_trace_fail(detail, JCE_FS_HOST_COPY_SOURCE_CLOSE);
        ok = false;
    }
    JCE_FREE(buffer);
    return ok;
}

static bool jce_fs_host_write_all_impl(const char *path, const void *data, uint64_t size)
{
    if (s_empty(path) || (size && !data)) return false;
    if (!s_ensure_parent_dir(path)) return false;

    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (!io) return false;

    bool ok = true;
    if (size > 0) {
        size_t nw = SDL_WriteIO(io, data, size);
        ok = (nw == size);
    }
    /* Closing flushes buffered writes; failure must not publish a staging file. */
    if (!SDL_CloseIO(io)) ok = false;
    return ok;
}

static bool jce_fs_host_write_all_atomic_impl(const char *path, const void *data, uint64_t size)
{
    if (s_empty(path) || (size && !data)) return false;

    /* Sibling temp in the same directory so the rename stays on one volume
     * (a cross-volume rename is not atomic and may fail). */
    size_t plen = strlen(path);
    char *tmp = (char *)JCE_MALLOC(plen + 5);   /* ".tmp" + NUL */
    if (!tmp) return false;
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".tmp", 5);

    bool ok = jce_fs_host_write_all(tmp, data, size);
    if (ok) {
        /* SDL_RenamePath already replaces existing destinations: POSIX rename
         * or Windows MoveFileExW(MOVEFILE_REPLACE_EXISTING). Never unlink the
         * old file on failure; sharing/permission errors are not permission to
         * destroy the last successful save. */
        ok = jce_fs_host_rename(tmp, path);
    }
    if (!ok) (void)SDL_RemovePath(tmp);   /* one cleanup attempt for our staging file */
    JCE_FREE(tmp);
    return ok;
}

static bool jce_fs_host_append_impl(const char *path, const void *data, uint64_t size)
{
    if (s_empty(path) || (size && !data)) return false;
    if (!s_ensure_parent_dir(path)) return false;

    /* SDL3's IOFromFile supports "ab" for binary append. */
    SDL_IOStream *io = SDL_IOFromFile(path, "ab");
    if (!io) return false;

    bool ok = true;
    if (size > 0) {
        size_t nw = SDL_WriteIO(io, data, size);
        ok = (nw == size);
    }
    /* Closing may flush buffered output. A successful write alone does not
     * prove the append completed; continuous trace must reject close failure. */
    if (!SDL_CloseIO(io)) ok = false;
    return ok;
}

static void *jce_fs_host_read_capped_impl(const char *path, uint64_t max_bytes,
        uint64_t *out_read, uint64_t *out_total, JceFsHostReadTrace *detail,
        const JceTraceScope *scope)
{
    if (out_read) *out_read = 0;
    if (out_total) *out_total = 0;
    if (s_empty(path)) {
        s_read_trace_fail(detail, JCE_FS_HOST_READ_INVALID);
        return NULL;
    }
    if (detail) {
        detail->dispatch = JCE_FS_HOST_READ_HOST;
        detail->known |= JCE_FS_HOST_READ_KNOWN_ACTUAL;
    }
    JceFsHostOpenFailureContext *const open_detail = s_open_detail_borrow(detail, scope);
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) {
        if (open_detail) s_open_detail_failure(open_detail, path, "read_capped", scope, detail->path_hash);
        s_read_trace_fail(detail, JCE_FS_HOST_READ_OPEN);
        return NULL;
    }

    Sint64 sz = SDL_GetIOSize(io);
    if (sz < 0) {
        s_read_trace_fail(detail, JCE_FS_HOST_READ_SIZE);
        s_read_trace_close(io, detail);
        return NULL;
    }
    if (out_total) *out_total = (uint64_t)sz;

    /* Clamp before narrowing, leaving room for the text-safe NUL sentinel. */
    uint64_t read_bytes = (uint64_t)sz;
    if (read_bytes > max_bytes) read_bytes = max_bytes;
    if (detail) {
        detail->total = (uint64_t)sz;
        detail->requested = read_bytes;
        detail->known |= JCE_FS_HOST_READ_KNOWN_TOTAL | JCE_FS_HOST_READ_KNOWN_REQUESTED;
    }
    if (read_bytes > (uint64_t)(SIZE_MAX - 1u)) {
        s_read_trace_fail(detail, JCE_FS_HOST_READ_LIMIT);
        s_read_trace_close(io, detail);
        return NULL;
    }
    size_t read_size = (size_t)read_bytes;

    void *buf = JCE_MALLOC(read_size + 1);
    if (!buf) {
        s_read_trace_fail(detail, JCE_FS_HOST_READ_ALLOC);
        s_read_trace_close(io, detail);
        return NULL;
    }

    size_t nread = 0;
    if (read_size > 0) {
        nread = SDL_ReadIO(io, buf, read_size);
        if (detail) {
            detail->actual = (uint64_t)nread;
            detail->short_read = nread == read_size ? JCE_FS_HOST_READ_FULL
                                                   : JCE_FS_HOST_READ_SHORT_OBSERVED;
        }
    }
    ((char *)buf)[nread] = '\0';
    s_read_trace_close(io, detail); /* Short reads and close failures retain legacy success. */

    if (out_read) *out_read = nread;
    return buf;
}

bool jce_fs_host_get_base_path(char *out, uint32_t out_size)
{
    if (!out || out_size == 0) return false;
    const char *p = SDL_GetBasePath();
    if (!p) { out[0] = '\0'; return false; }
    size_t n = strlen(p);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

void jce_fs_buffer_free(void *buf)
{
    if (buf) JCE_FREE(buf);
}

bool jce_fs_host_get_current_dir(char *out, uint32_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    /* SDL_GetCurrentDirectory returns a freshly allocated string the
       caller must release with SDL_free.  It already includes a
       trailing path separator on Windows; we strip it for consistency
       with std::filesystem::current_path(). */
    char *cwd = SDL_GetCurrentDirectory();
    if (!cwd) return false;
    size_t n = strlen(cwd);
    while (n > 0 && (cwd[n - 1] == '/' || cwd[n - 1] == '\\')) --n;
    if (n + 1 > out_size) {
        SDL_free(cwd);
        return false;
    }
    memcpy(out, cwd, n);
    out[n] = '\0';
    SDL_free(cwd);
    /* Canonicalise to forward slashes for cross-platform consistency. */
    for (size_t i = 0; i < n; ++i) if (out[i] == '\\') out[i] = '/';
    return true;
}

/* ================================================================== */
/* Active VFS override storage (function-pointer hook).                 */
/*                                                                      */
/* The high-level jce_fs_set_active() in jce_filesystem.c wires both    */
/* slots; cooker tools that only link this TU get inert defaults.       */
/* ================================================================== */


JceFileSystem *jce_fs_get_active(void) { return s_host_read_service.fs; }
JceFsReadFn jce__fs_active_reader(void) { return s_host_read_service.reader; }
void jce__fs_store_active(JceFileSystem *fs, JceFsPresenceFnInternal probe)
{
    s_host_read_service.fs = fs;
    s_host_read_service.probe = probe;
}
void jce__fs_store_active_policy(JceFsActivePolicy policy) { s_host_read_service.policy = policy; }
JceFsActivePolicy jce_fs_get_active_policy(void) { return s_host_read_service.policy; }
void jce_fs_set_active_reader(JceFsReadFn fn)
{
    s_host_read_service.reader = fn;
    s_host_read_service.probe = NULL;
}

JceFsPresence jce_fs_probe_read(const char *path)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.probe_read");
    JceFsPresence presence = JCE_FS_PRESENCE_UNKNOWN;
    if (!s_empty(path)) {
        /* Same absolute-address rule as the existing host-read owner. */
        const bool absolute = path[0] == '/' || path[0] == '\\' ||
            (((path[0] >= 'A' && path[0] <= 'Z') ||
              (path[0] >= 'a' && path[0] <= 'z')) &&
             path[1] == ':' && (path[2] == '/' || path[2] == '\\'));
        if (!absolute && s_host_read_service.fs && s_host_read_service.reader) {
            if (!s_host_read_service.probe) goto done;
            presence = s_host_read_service.probe(s_host_read_service.fs, path);
            if (presence != JCE_FS_PRESENCE_ABSENT ||
                s_host_read_service.policy == JCE_FS_ACTIVE_ISOLATED) goto done;
        }
        presence = jce_fs_native_probe_read_internal(path);
    }
done:
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_CORE, "fs.probe_read.presence",
                                     presence, scope.id, scope.parent);
    /* Successful CPU query with an explicit tri-state result; not IO success. */
    jce_trace_scope_end(scope, s_empty(path) ? JCE_TRACE_TASK_FAILED : JCE_TRACE_TASK_SUCCEEDED);
    return presence;
}

/* Trace adapters keep IO behavior in the existing implementation owner.
 * Read/query APIs with ambiguous zero/EOF results are not labelled successful
 * here. Capture-owned host IO is suppressed only on its consumer thread. */
bool jce_fs_host_create_directory(const char *path)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.host.create_directory");
    bool result = jce_fs_host_create_directory_impl(path);
    jce_trace_scope_end(scope, (result) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

bool jce_fs_host_remove_file(const char *path)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.host.remove_file");
    bool result = jce_fs_host_remove_file_impl(path);
    jce_trace_scope_end(scope, (result) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

bool jce_fs_host_copy_file(const char *src, const char *dst)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.host.copy_file");
    bool result = jce_fs_host_copy_file_impl(src, dst);
    jce_trace_scope_end(scope, (result) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

bool jce_fs_host_copy_file_new(const char *src, const char *dst, bool *out_created)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.host.copy_file_new");
    JceFsHostCopyTrace storage;
    JceFsHostCopyTrace *detail = scope.id ? &storage : NULL;
    if (detail) {
        *detail = (JceFsHostCopyTrace){0};
        detail->source_hash = src ? jce_fnv1a64_str(src) : 0;
        detail->destination_hash = dst ? jce_fnv1a64_str(dst) : 0;
    }
    bool result = jce_fs_host_copy_file_new_impl(src, dst, out_created, detail);
    s_copy_trace_emit(detail, scope);
    jce_trace_scope_end(scope, result ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    return result;
}

bool jce_fs_host_copy_recursive(const char *src, const char *dst)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.host.copy_recursive");
    bool result = jce_fs_host_copy_recursive_impl(src, dst);
    jce_trace_scope_end(scope, (result) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

bool jce_fs_host_rename(const char *src, const char *dst)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.host.rename");
    bool result = jce_fs_host_rename_impl(src, dst);
    jce_trace_scope_end(scope, (result) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

bool jce_fs_host_remove_recursive(const char *path)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.host.remove_recursive");
    bool result = jce_fs_host_remove_recursive_impl(path);
    jce_trace_scope_end(scope, (result) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

void *jce_fs_host_read_all(const char *path, uint64_t *out_size)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.host.read_all");
    JceFsHostReadTrace storage;
    JceFsHostReadTrace *detail = s_read_trace_begin(&storage, scope, path, false, 0);
    void *result = jce_fs_host_read_all_impl(path, out_size, detail, &scope);
    s_read_trace_emit(detail, scope);
    jce_trace_scope_end(scope, (result != NULL) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

bool jce_fs_host_write_all(const char *path, const void *data, uint64_t size)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.host.write_all");
    bool result = jce_fs_host_write_all_impl(path, data, size);
    jce_trace_scope_end(scope, (result) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

bool jce_fs_host_write_new(const char *path, const void *data, uint64_t size)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.host.write_new");
    bool created = false;
    bool result = jce__fs_host_reserve_new(path, data, size, &created);
    jce_trace_scope_end(scope, result ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    return result;
}

bool jce_fs_host_write_all_atomic(const char *path, const void *data, uint64_t size)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.host.write_all_atomic");
    bool result = jce_fs_host_write_all_atomic_impl(path, data, size);
    jce_trace_scope_end(scope, (result) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

bool jce_fs_host_append(const char *path, const void *data, uint64_t size)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.host.append");
    bool result = jce_fs_host_append_impl(path, data, size);
    jce_trace_scope_end(scope, (result) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

void *jce_fs_host_read_capped(const char *path, uint64_t max_bytes,
                              uint64_t *out_read, uint64_t *out_total)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.host.read_capped");
    JceFsHostReadTrace storage;
    JceFsHostReadTrace *detail = s_read_trace_begin(&storage, scope, path, true, max_bytes);
    void *result = jce_fs_host_read_capped_impl(path, max_bytes, out_read, out_total, detail, &scope);
    s_read_trace_emit(detail, scope);
    jce_trace_scope_end(scope, (result != NULL) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}
