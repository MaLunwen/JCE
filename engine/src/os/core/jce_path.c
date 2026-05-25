/*
 * jce_path.c  Implementation of the cross-platform path-string utilities
 * declared in jce/os/core/jce_path.h.
 *
 * All functions are pure string ops — no syscalls, no allocations.
 * Canonical separator is '/'; '\\' is accepted on read.
 */

#include "jce/os/core/jce_path.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static bool s_empty(const char *s) { return !s || !*s; }

static bool s_is_sep(char c) { return c == '/' || c == '\\'; }

/* Find offset of last separator in `path`; returns SIZE_MAX if none. */
static size_t s_last_sep(const char *path)
{
    size_t last = (size_t)-1;
    for (size_t i = 0; path[i]; ++i) {
        if (s_is_sep(path[i])) last = i;
    }
    return last;
}

/* Find offset of last '.' that comes after the last separator.
   Returns SIZE_MAX if no extension. */
static size_t s_ext_dot(const char *path)
{
    size_t last_sep = s_last_sep(path);
    size_t last_dot = (size_t)-1;
    for (size_t i = (last_sep == (size_t)-1) ? 0 : last_sep + 1; path[i]; ++i) {
        if (path[i] == '.') last_dot = i;
    }
    /* "." and ".." are not extensions. */
    if (last_dot == (size_t)-1) return (size_t)-1;
    size_t base_start = (last_sep == (size_t)-1) ? 0 : last_sep + 1;
    if (last_dot == base_start) return (size_t)-1; /* leading dot */
    return last_dot;
}

static bool s_copy_n(char *out, size_t out_size, const char *src, size_t n)
{
    if (!out || out_size == 0) return false;
    if (n + 1 > out_size) { out[0] = '\0'; return false; }
    if (n > 0) memmove(out, src, n);
    out[n] = '\0';
    return true;
}

/* ------------------------------------------------------------------ */
/* Canonical form                                                     */
/* ------------------------------------------------------------------ */

bool jce_path_to_canonical(char *out, size_t out_size, const char *path)
{
    if (!out || out_size == 0) return false;
    if (!path) { out[0] = '\0'; return true; }
    size_t n = strlen(path);
    if (n + 1 > out_size) { out[0] = '\0'; return false; }
    for (size_t i = 0; i < n; ++i) {
        char c = path[i];
        out[i] = (c == '\\') ? '/' : c;
    }
    out[n] = '\0';
    return true;
}

bool jce_path_is_canonical(const char *path)
{
    if (!path) return true;
    for (size_t i = 0; path[i]; ++i) {
        if (path[i] == '\\') return false;
    }
    return true;
}

bool jce_path_is_absolute(const char *path)
{
    if (s_empty(path)) return false;
    if (s_is_sep(path[0])) return true;
    /* Windows-style "C:" or "C:/..." */
    if (((path[0] >= 'A' && path[0] <= 'Z') ||
         (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':')
        return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* Decomposition                                                      */
/* ------------------------------------------------------------------ */

bool jce_path_parent(char *out, size_t out_size, const char *path)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (s_empty(path)) return false;
    size_t last = s_last_sep(path);
    if (last == (size_t)-1) return false;
    /* Preserve root: "/" or "C:/" */
    if (last == 0) return s_copy_n(out, out_size, path, 1);
    return s_copy_n(out, out_size, path, last);
}

bool jce_path_basename(char *out, size_t out_size, const char *path)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (s_empty(path)) return true;
    size_t last = s_last_sep(path);
    const char *base = (last == (size_t)-1) ? path : path + last + 1;
    return s_copy_n(out, out_size, base, strlen(base));
}

bool jce_path_stem(char *out, size_t out_size, const char *path)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (s_empty(path)) return true;
    size_t last_sep = s_last_sep(path);
    size_t base_start = (last_sep == (size_t)-1) ? 0 : last_sep + 1;
    size_t dot = s_ext_dot(path);
    size_t end = (dot == (size_t)-1) ? strlen(path) : dot;
    if (end < base_start) end = base_start;
    return s_copy_n(out, out_size, path + base_start, end - base_start);
}

bool jce_path_extension(char *out, size_t out_size, const char *path)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (s_empty(path)) return false;
    size_t dot = s_ext_dot(path);
    if (dot == (size_t)-1) return false;
    return s_copy_n(out, out_size, path + dot, strlen(path + dot));
}

/* ------------------------------------------------------------------ */
/* Composition                                                        */
/* ------------------------------------------------------------------ */

bool jce_path_join(char *out, size_t out_size, const char *a, const char *b)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    bool ea = s_empty(a), eb = s_empty(b);
    if (ea && eb) return true;
    if (ea) return jce_path_to_canonical(out, out_size, b);
    if (eb) return jce_path_to_canonical(out, out_size, a);

    /* If b is absolute, it replaces a entirely. */
    if (jce_path_is_absolute(b))
        return jce_path_to_canonical(out, out_size, b);

    size_t na = strlen(a);
    size_t nb = strlen(b);

    /* Drop trailing sep from a; drop leading sep from b. */
    while (na > 0 && s_is_sep(a[na - 1])) --na;
    size_t b_off = 0;
    while (b[b_off] && s_is_sep(b[b_off])) ++b_off;
    nb -= b_off;

    if (na + 1 + nb + 1 > out_size) return false;
    for (size_t i = 0; i < na; ++i) out[i] = (a[i] == '\\') ? '/' : a[i];
    out[na] = '/';
    for (size_t i = 0; i < nb; ++i) {
        char c = b[b_off + i];
        out[na + 1 + i] = (c == '\\') ? '/' : c;
    }
    out[na + 1 + nb] = '\0';
    return true;
}

bool jce_path_replace_extension(char *out, size_t out_size,
                                const char *path, const char *ext)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (s_empty(path)) return false;

    size_t dot = s_ext_dot(path);
    size_t base_len = (dot == (size_t)-1) ? strlen(path) : dot;

    bool need_dot = false;
    size_t ext_len = 0;
    if (!s_empty(ext)) {
        if (ext[0] != '.') need_dot = true;
        ext_len = strlen(ext);
    }

    size_t total = base_len + (need_dot ? 1 : 0) + ext_len;
    if (total + 1 > out_size) return false;

    /* Use memmove since out and path may alias. */
    memmove(out, path, base_len);
    /* Canonicalize separators in the kept prefix. */
    for (size_t i = 0; i < base_len; ++i) if (out[i] == '\\') out[i] = '/';

    size_t off = base_len;
    if (need_dot) out[off++] = '.';
    if (ext_len) memmove(out + off, ext, ext_len);
    out[total] = '\0';
    return true;
}

bool jce_path_normalize(char *out, size_t out_size, const char *path)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (s_empty(path)) return true;

    /* Detect absolute prefix (drive letter or leading /). */
    size_t prefix = 0;
    bool absolute = false;
    if (((path[0] >= 'A' && path[0] <= 'Z') ||
         (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':') {
        prefix = 2;
        absolute = true;
    }
    if (s_is_sep(path[prefix])) {
        ++prefix;
        absolute = true;
    }

    /* Walk tokens, maintain a stack of segment offsets in `out`. */
    size_t out_len = 0;
    if (prefix + 1 > out_size) return false;
    for (size_t i = 0; i < prefix; ++i)
        out[out_len++] = (path[i] == '\\') ? '/' : path[i];

    size_t segs[256];
    size_t nseg = 0;
    size_t seg_start = out_len;

    size_t i = prefix;
    while (path[i]) {
        /* Skip separator runs. */
        while (path[i] && s_is_sep(path[i])) ++i;
        if (!path[i]) break;
        /* Read one segment. */
        size_t s = i;
        while (path[i] && !s_is_sep(path[i])) ++i;
        size_t seg_len = i - s;

        if (seg_len == 1 && path[s] == '.') continue; /* drop "." */
        if (seg_len == 2 && path[s] == '.' && path[s + 1] == '.') {
            if (nseg > 0) {
                /* Pop one segment. */
                --nseg;
                out_len = (nseg > 0) ? segs[nseg - 1] + 0 : seg_start;
                /* Recompute end of previous segment. */
                if (nseg > 0) {
                    size_t prev = segs[nseg - 1];
                    out_len = prev;
                    while (out_len < out_size && out[out_len] && !s_is_sep(out[out_len])) ++out_len;
                } else {
                    out_len = seg_start;
                }
                continue;
            }
            if (absolute) return false; /* underflow */
            /* Relative path: keep ".." literally. */
        }

        /* Append separator if needed. */
        if (out_len > seg_start) {
            if (out_len + 1 >= out_size) return false;
            out[out_len++] = '/';
        }
        if (out_len + seg_len + 1 > out_size) return false;
        if (nseg < (sizeof(segs) / sizeof(segs[0]))) segs[nseg++] = out_len;
        for (size_t k = 0; k < seg_len; ++k)
            out[out_len + k] = (path[s + k] == '\\') ? '/' : path[s + k];
        out_len += seg_len;
    }

    if (out_len == 0) {
        if (absolute) {
            if (1 + 1 > out_size) return false;
            out[out_len++] = '/';
        } else {
            if (1 + 1 > out_size) return false;
            out[out_len++] = '.';
        }
    }
    out[out_len] = '\0';
    return true;
}

bool jce_path_relative(char *out, size_t out_size,
                       const char *path, const char *base)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (s_empty(path) || s_empty(base)) return false;

    /* Normalize both inputs into temporary buffers. */
    char np[1024], nb[1024];
    if (!jce_path_normalize(np, sizeof(np), path)) return false;
    if (!jce_path_normalize(nb, sizeof(nb), base)) return false;

    /* Find the longest matching directory prefix. */
    size_t common = 0;
    size_t i = 0;
    while (np[i] && nb[i] && np[i] == nb[i]) {
        if (np[i] == '/') common = i + 1;
        ++i;
    }
    /* Whole base matches up to a '/' or end. */
    if (!nb[i] && (np[i] == '/' || !np[i])) common = i + (np[i] == '/' ? 1 : 0);

    /* Count '/' segments remaining in base after `common` -> that many "../".
     * `common` is an index into np that may also legitimately equal
     * strlen(nb)+1 when nb was fully consumed (post-loop +1 to skip the
     * separator in np).  Indexing nb with that value reads past its
     * null terminator (UB → phantom "../" prefixes), so cap to nb's
     * length here. */
    size_t nblen = strlen(nb);
    size_t cstart = (common <= nblen) ? common : nblen;
    size_t up = 0;
    for (size_t j = cstart; j < nblen; ++j) if (nb[j] == '/') ++up;
    /* If there's still un-matched text in nb past `cstart`, that's an
     * extra segment to climb out of (preserves the original semantics
     * of "++up for the leftover segment").  The cstart<nblen guard
     * makes the safety against reading past nb's terminator explicit,
     * which the old code lacked when common was set to strlen(nb)+1
     * after fully consuming nb. */
    if (cstart < nblen) ++up;

    size_t need = up * 3;                   /* "../" per up step */
    size_t tail_off = (common < strlen(np) && np[common] == '/') ? common + 1 : common;
    if (tail_off > strlen(np)) tail_off = strlen(np);
    size_t tail_len = strlen(np + tail_off);
    if (need + tail_len + 1 > out_size) return false;
    if (need == 0 && tail_len == 0) {
        out[0] = '.'; out[1] = '\0'; return true;
    }
    size_t off = 0;
    for (size_t k = 0; k < up; ++k) {
        out[off++] = '.'; out[off++] = '.'; out[off++] = '/';
    }
    if (tail_len) {
        memmove(out + off, np + tail_off, tail_len);
        off += tail_len;
    } else if (off > 0) {
        --off; /* trim trailing '/' */
    }
    out[off] = '\0';
    return true;
}
