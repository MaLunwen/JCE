/*
 * jce_json.c  Thin facade implementation over cJSON.
 *
 * Public/editor code should talk to this facade instead of cJSON directly.
 * Low-level JSON bridge modules may still include cJSON internally.
 *
 * ── Why cJSON_InitHooks is NOT called here (audit JSON-05) ───────────
 * cJSON allocates through the CRT, so JSON trees — which is most of what a
 * scene/prefab/material/settings load allocates — sit outside mimalloc and
 * outside the engine's memory accounting.  Routing them through JCE_MALLOC
 * looks like a one-line fix and is a trap: cJSON_InitHooks swaps GLOBAL
 * function pointers, so any tree allocated before the swap is later freed by
 * cJSON_Delete through the NEW free — a cross-allocator free, i.e. exactly
 * the P0 class this audit already had to fix twice elsewhere.  Several TUs
 * still call cJSON directly (scene, resource, ai_dispatch), so a lazy
 * "install on first facade call" cannot guarantee it wins the race against
 * the first allocation.
 *
 * The safe fix is to install the hooks exactly once BEFORE any cJSON
 * allocation in the process — a single early init that every binary
 * (engine, editor, and each host tool) is guaranteed to run — and only then
 * is it worth doing.  Left deliberately undone rather than half-done.
 */

#include "jce/os/core/jce_json.h"
#include "jce/os/core/jce_filesystem.h"
#include "jce/os/core/jce_log.h"
#include "jce/os/core/jce_path.h"

#include <cjson/cJSON.h>
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "jce_json"

/* ── Parse-failure diagnostics (audit JSON-08) ─────────────────────── */

/* Thread-local rather than a plain static because asset loads parse JSON on
 * worker threads: a global record would be a data race AND would routinely
 * hand the main thread some other load's failure.  This is also why cJSON's
 * cJSON_GetErrorPtr() is not used — that one IS a plain global; the *Opts
 * parse entry points hand the same pointer back per call, so we take it from
 * there instead. */
#ifdef _MSC_VER
static __declspec(thread) char   tl_err[192]   = {0};
static __declspec(thread) size_t tl_err_offset = (size_t)-1;
#else
static __thread char   tl_err[192]   = {0};
static __thread size_t tl_err_offset = (size_t)-1;
#endif

static void s_err_clear(void)
{
    tl_err[0]     = '\0';
    tl_err_offset = (size_t)-1;
}

/* Record a non-parse (input / I/O) failure.  Leaves the offset at the "not a
 * parse failure" sentinel, and refuses to overwrite a parse failure already
 * recorded: when a VFS mount served bytes that do not parse, THAT is the
 * actionable diagnosis — the development-tree fallback also being absent is
 * noise on top of it. */
static void s_err_set_io(const char *fmt, ...)
{
    if (tl_err_offset != (size_t)-1) return;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tl_err, sizeof tl_err, fmt, ap);
    va_end(ap);
}

/* Record a cJSON failure.  `at` is where the parser stopped inside
 * [text, text+len).  The offset is resolved to line/column plus a short
 * excerpt because a bare byte offset is unusable to someone staring at a
 * 40k-line scene file. */
static void s_err_set_parse(const char *text, size_t len, const char *at)
{
    if (!text) len = 0;

    size_t off = 0;
    if (text && at && at > text) {
        off = (size_t)(at - text);
        if (off > len) off = len;
    }

    size_t line = 1, col = 1;
    for (size_t i = 0; i < off; i++) {
        if (text[i] == '\n') { line++; col = 1; }
        else                   col++;
    }

    char   snippet[25];
    size_t n = 0;
    while (n + 1 < sizeof snippet && off + n < len) {
        /* Printable ASCII only: a raw slice can cut a UTF-8 sequence in half
         * and poison the log line it lands in. */
        unsigned char c = (unsigned char)text[off + n];
        if (c == '\n' || c == '\r') break;
        snippet[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    snippet[n] = '\0';

    snprintf(tl_err, sizeof tl_err,
             "malformed JSON: line %zu, column %zu (byte %zu), near %s",
             line, col, off, snippet);
    tl_err_offset = off;
}

/* Report content corruption, never a missing file: many jce_json_parse_file
 * callers probe for an optional file (per-user config, sidecar metadata) and
 * treat NULL as "absent", so logging every miss would bury the one case a
 * user must act on — a file that IS there and is broken.  A non-sentinel
 * offset is exactly "we got far enough to parse it". */
static void s_log_failure(const char *path)
{
    if (tl_err_offset != (size_t)-1)
        LOG_ERROR(LOG_TAG, "'%s': %s", path, tl_err);
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

JceJson *jce_json_parse(const char *text, size_t len)
{
    s_err_clear();
    if (!text) {
        s_err_set_io("no input (NULL text)");
        return NULL;
    }

    const char *end = NULL;
    JceJson    *j   = (len == 0)
                    ? cJSON_ParseWithOpts(text, &end, 0)
                    : cJSON_ParseWithLengthOpts(text, len, &end, 0);
    if (!j) s_err_set_parse(text, len ? len : strlen(text), end);
    return j;
}

JceJson *jce_json_parse_strict(const char *text, size_t len)
{
    const char *end = NULL;
    const char *limit;
    JceJson *j;

    s_err_clear();
    if (!text) {
        s_err_set_io("no input (NULL text)");
        return NULL;
    }
    if (len == 0)
        len = strlen(text);
    limit = text + len;
    j = cJSON_ParseWithLengthOpts(text, len, &end, 0);
    if (!j) {
        s_err_set_parse(text, len, end);
        return NULL;
    }
    while (end && end < limit &&
           (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        ++end;
    if (!end || end != limit) {
        cJSON_Delete(j);
        s_err_set_parse(text, len, end ? end : text);
        return NULL;
    }
    return j;
}

JceJson *jce_json_parse_file(const char *path)
{
    s_err_clear();
    if (!path) {
        s_err_set_io("no input (NULL path)");
        return NULL;
    }

    const bool isolated_relative =
        jce_fs_get_active() != NULL &&
        jce_fs_get_active_policy() == JCE_FS_ACTIVE_ISOLATED &&
        !jce_path_is_absolute(path);

    /* Prefer the unified host_read_all path so an active VFS override
     * (e.g. editor scene preview from a .jbundle) can intercept reads
     * for project-relative JSON files like terrain/material metadata. */
    {
        uint64_t  sz   = 0;
        void     *vbuf = jce_fs_host_read_all(path, &sz);
        if (vbuf) {
            const char *end = NULL;
            JceJson    *j   = cJSON_ParseWithLengthOpts((const char *)vbuf,
                                                        (size_t)sz, &end, 0);
            /* Must read the buffer before it is released. */
            if (!j) s_err_set_parse((const char *)vbuf, (size_t)sz, end);
            jce_fs_buffer_free(vbuf);
            if (j) return j;
            /* An isolated mount is authoritative, including malformed data. */
        }
    }

    /* Bundle Preview must expose missing dependencies instead of silently
     * borrowing a same-named development file from the process directory. */
    if (isolated_relative) {
        s_err_set_io("not present in the active isolated mount");
        s_log_failure(path);
        return NULL;
    }

    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) {
        s_err_set_io("cannot open file: %s", SDL_GetError());
        return NULL;
    }

    Sint64 sz = SDL_GetIOSize(io);
    if (sz <= 0) {
        SDL_CloseIO(io);
        s_err_set_io("file is empty or has unknown size");
        return NULL;
    }

    char *buf = (char *)SDL_malloc((size_t)sz + 1);
    if (!buf) {
        SDL_CloseIO(io);
        s_err_set_io("out of memory reading %lld bytes", (long long)sz);
        return NULL;
    }

    size_t nread = SDL_ReadIO(io, buf, (size_t)sz);
    SDL_CloseIO(io);
    if (nread != (size_t)sz) {
        SDL_free(buf);
        s_err_set_io("truncated read (%zu of %lld bytes): %s",
                     nread, (long long)sz, SDL_GetError());
        return NULL;
    }
    buf[nread] = '\0';

    const char *end = NULL;
    JceJson    *j   = cJSON_ParseWithLengthOpts(buf, nread, &end, 0);
    if (j) s_err_clear();  /* a VFS miss on the way here is not a failure */
    else   s_err_set_parse(buf, nread, end);
    SDL_free(buf);
    if (!j) s_log_failure(path);
    return j;
}

const char *jce_json_last_error(void)
{
    return tl_err;
}

size_t jce_json_last_error_offset(void)
{
    return tl_err_offset;
}

JceJson *jce_json_object(void) { return cJSON_CreateObject(); }
JceJson *jce_json_array (void) { return cJSON_CreateArray();  }
JceJson *jce_json_number(double v) { return cJSON_CreateNumber(v); }
JceJson *jce_json_string(const char *v) { return cJSON_CreateString(v ? v : ""); }
JceJson *jce_json_bool(bool v) { return cJSON_CreateBool(v); }

char *jce_json_print(const JceJson *root, bool pretty)
{
    if (!root) return NULL;
    return pretty ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
}

bool jce_json_write_file(const char *path, JceJson *root,
                         bool pretty, bool take_ownership)
{
    if (!path || !root) {
        if (take_ownership && root) cJSON_Delete(root);
        return false;
    }

    char *txt = pretty ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
    if (take_ownership) cJSON_Delete(root);
    if (!txt) return false;

    /* Atomic (temp + rename): every jce_json_write_file consumer is user
     * data (editor config, project settings, audio mixer, input actions,
     * package lists) — a crash or full disk mid-write must never leave a
     * truncated file, because loaders treat unparseable JSON as absent and
     * silently fall back to defaults (= total silent settings loss). */
    size_t len = strlen(txt);
    bool ok = jce_fs_host_write_all_atomic(path, txt, (uint64_t)len);
    cJSON_free(txt);
    return ok;
}

void jce_json_free(JceJson *root)        { cJSON_Delete(root); }
void jce_json_free_string(char *s)       { if (s) cJSON_free(s); }

/* ── Type tests / navigation ───────────────────────────────────────── */

bool jce_json_is_object(const JceJson *j) { return cJSON_IsObject(j) ? true : false; }
bool jce_json_is_array (const JceJson *j) { return cJSON_IsArray(j)  ? true : false; }
bool jce_json_is_number(const JceJson *j) { return cJSON_IsNumber(j) ? true : false; }
bool jce_json_is_string(const JceJson *j) { return cJSON_IsString(j) ? true : false; }
bool jce_json_is_bool  (const JceJson *j) { return cJSON_IsBool(j)   ? true : false; }
bool jce_json_is_null  (const JceJson *j) { return cJSON_IsNull(j)   ? true : false; }

JceJson *jce_json_get(const JceJson *obj, const char *key)
{
    if (!obj || !key) return NULL;
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

bool jce_json_has(const JceJson *obj, const char *key)
{
    return jce_json_get(obj, key) != NULL;
}

void jce_json_remove(JceJson *obj, const char *key)
{
    if (!obj || !key) return;
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
}

void jce_json_detach(JceJson *parent, JceJson *child)
{
    if (!parent || !child) return;
    /* cJSON_DetachItemViaPointer unlinks and RETURNS the item without
     * freeing it; ownership moves to the caller.  It already tolerates a
     * child that does not belong to parent (returns NULL, changes nothing),
     * so no extra guard is needed here. */
    (void)cJSON_DetachItemViaPointer(parent, child);
}

int jce_json_array_size(const JceJson *arr)
{
    if (!cJSON_IsArray(arr)) return 0;
    return cJSON_GetArraySize(arr);
}

JceJson *jce_json_array_at(const JceJson *arr, int index)
{
    if (!cJSON_IsArray(arr)) return NULL;
    return cJSON_GetArrayItem(arr, index);
}

JceJson *jce_json_first_child(const JceJson *obj)
{
    return obj ? obj->child : NULL;
}

JceJson *jce_json_next_sibling(const JceJson *node)
{
    return node ? node->next : NULL;
}

const char *jce_json_member_key(const JceJson *node)
{
    return node ? node->string : NULL;
}

const char *jce_json_string_value(const JceJson *node, const char *def)
{
    if (cJSON_IsString(node) && node->valuestring) return node->valuestring;
    return def;
}

double jce_json_number_value(const JceJson *node, double def)
{
    if (cJSON_IsNumber(node)) return node->valuedouble;
    return def;
}

bool jce_json_bool_value(const JceJson *node, bool def)
{
    if (!cJSON_IsBool(node)) return def;
    return cJSON_IsTrue(node) ? true : false;
}

/* ── Typed accessors with defaults ─────────────────────────────────── */

double jce_json_get_number(const JceJson *obj, const char *key, double def)
{
    const cJSON *it = jce_json_get(obj, key);
    if (cJSON_IsNumber(it)) return it->valuedouble;
    return def;
}

int jce_json_get_int(const JceJson *obj, const char *key, int def)
{
    const cJSON *it = jce_json_get(obj, key);
    if (cJSON_IsNumber(it)) return it->valueint;
    return def;
}

bool jce_json_get_bool(const JceJson *obj, const char *key, bool def)
{
    const cJSON *it = jce_json_get(obj, key);
    if (cJSON_IsBool(it))   return cJSON_IsTrue(it) ? true : false;
    if (cJSON_IsNumber(it)) return it->valuedouble != 0.0;
    return def;
}

const char *jce_json_get_string(const JceJson *obj, const char *key,
                                const char *def)
{
    const cJSON *it = jce_json_get(obj, key);
    if (cJSON_IsString(it) && it->valuestring) return it->valuestring;
    return def;
}

double jce_json_get_number_any(const JceJson *obj,
                               const char *const *keys, int n, double def)
{
    if (!keys) return def;
    for (int i = 0; i < n; i++) {
        const cJSON *it = jce_json_get(obj, keys[i]);
        if (cJSON_IsNumber(it)) return it->valuedouble;
    }
    return def;
}

const char *jce_json_get_string_any(const JceJson *obj,
                                    const char *const *keys, int n,
                                    const char *def)
{
    if (!keys) return def;
    for (int i = 0; i < n; i++) {
        const cJSON *it = jce_json_get(obj, keys[i]);
        if (cJSON_IsString(it) && it->valuestring) return it->valuestring;
    }
    return def;
}

void jce_json_get_floats(const JceJson *obj, const char *key,
                         float *out, int n, const float *def)
{
    if (!out || n <= 0) return;
    const cJSON *arr = jce_json_get(obj, key);
    if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) >= n) {
        for (int i = 0; i < n; i++) {
            const cJSON *e = cJSON_GetArrayItem(arr, i);
            out[i] = cJSON_IsNumber(e) ? (float)e->valuedouble
                                       : (def ? def[i] : 0.0f);
        }
    } else {
        for (int i = 0; i < n; i++) out[i] = def ? def[i] : 0.0f;
    }
}

static void s_get_axis(const JceJson *obj, const char *prefix,
                       const char axis, float *out, float def)
{
    char key[64];
    snprintf(key, sizeof key, "%s%c", prefix ? prefix : "", axis);
    *out = (float)jce_json_get_number(obj, key, def);
}

void jce_json_get_xyz(const JceJson *obj, const char *prefix,
                      float out[3], const float def[3])
{
    if (!out) return;
    s_get_axis(obj, prefix, 'X', &out[0], def ? def[0] : 0.0f);
    s_get_axis(obj, prefix, 'Y', &out[1], def ? def[1] : 0.0f);
    s_get_axis(obj, prefix, 'Z', &out[2], def ? def[2] : 0.0f);
}

void jce_json_get_xyzw(const JceJson *obj, const char *prefix,
                       float out[4], const float def[4])
{
    if (!out) return;
    s_get_axis(obj, prefix, 'X', &out[0], def ? def[0] : 0.0f);
    s_get_axis(obj, prefix, 'Y', &out[1], def ? def[1] : 0.0f);
    s_get_axis(obj, prefix, 'Z', &out[2], def ? def[2] : 0.0f);
    s_get_axis(obj, prefix, 'W', &out[3], def ? def[3] : 0.0f);
}

/* ── Builders ──────────────────────────────────────────────────────── */

void jce_json_set_number(JceJson *obj, const char *key, double v)
{
    if (!obj || !key) return;
    cJSON_AddNumberToObject(obj, key, v);
}

void jce_json_set_int(JceJson *obj, const char *key, int v)
{
    if (!obj || !key) return;
    cJSON_AddNumberToObject(obj, key, (double)v);
}

void jce_json_set_bool(JceJson *obj, const char *key, bool v)
{
    if (!obj || !key) return;
    cJSON_AddBoolToObject(obj, key, v);
}

void jce_json_set_string(JceJson *obj, const char *key, const char *v)
{
    if (!obj || !key) return;
    cJSON_AddStringToObject(obj, key, v ? v : "");
}

void jce_json_set_child(JceJson *obj, const char *key, JceJson *child)
{
    if (!obj || !key || !child) return;
    cJSON_AddItemToObject(obj, key, child);
}

void jce_json_array_push(JceJson *arr, JceJson *item)
{
    if (!arr || !item) return;
    cJSON_AddItemToArray(arr, item);
}

void jce_json_array_push_number(JceJson *arr, double v)
{
    if (!arr) return;
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(v));
}

void jce_json_array_push_string(JceJson *arr, const char *v)
{
    if (!arr) return;
    cJSON_AddItemToArray(arr, cJSON_CreateString(v ? v : ""));
}

static void s_set_axis(JceJson *obj, const char *prefix,
                       char axis, float v)
{
    char key[64];
    snprintf(key, sizeof key, "%s%c", prefix ? prefix : "", axis);
    cJSON_AddNumberToObject(obj, key, v);
}

void jce_json_set_xyz(JceJson *obj, const char *prefix, const float v[3])
{
    if (!obj || !v) return;
    s_set_axis(obj, prefix, 'X', v[0]);
    s_set_axis(obj, prefix, 'Y', v[1]);
    s_set_axis(obj, prefix, 'Z', v[2]);
}

void jce_json_set_xyzw(JceJson *obj, const char *prefix, const float v[4])
{
    if (!obj || !v) return;
    s_set_axis(obj, prefix, 'X', v[0]);
    s_set_axis(obj, prefix, 'Y', v[1]);
    s_set_axis(obj, prefix, 'Z', v[2]);
    s_set_axis(obj, prefix, 'W', v[3]);
}

void jce_json_set_float_array(JceJson *obj, const char *key,
                              const float *v, int n)
{
    if (!obj || !key || !v || n < 0) return;
    cJSON *arr = cJSON_CreateFloatArray(v, n);
    if (!arr) return;
    cJSON_AddItemToObject(obj, key, arr);
}
