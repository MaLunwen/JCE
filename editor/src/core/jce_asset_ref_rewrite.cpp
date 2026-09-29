/*
 * jce_asset_ref_rewrite.cpp — see the header for why this exists.
 *
 * One pass over the buffer.  At each position, try the longest candidate
 * first: a rel path is longer and more specific than a basename, and a buffer
 * that carries "Textures/wood.png" must be rewritten as a path, not as a path
 * whose tail was separately rewritten as a basename.
 */

#include "jce_asset_ref_rewrite.h"

#include <jce/os/core/jce_alloc.h>

#include <string.h>

namespace {

/* Does `hay` carry `needle` at `i`, treating '/' and '\\' as the same byte?
 *
 * A scene written on Windows can carry either separator for the same asset,
 * and jce_assetdb's own matcher already finds both (it searches for the rel
 * path as stored).  Rewriting only one form would repair half the projects. */
bool sep_insensitive_match(const char *hay, size_t hay_len, size_t i,
                           const char *needle, size_t needle_len)
{
    if (needle_len == 0 || i + needle_len > hay_len) return false;
    for (size_t k = 0; k < needle_len; ++k) {
        char a = hay[i + k], b = needle[k];
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
        if (a != b) return false;
    }
    return true;
}

/* A basename is only a reference when it sits where a path sits.
 *
 * Preceded by a separator or an opening quote, and followed by a closing
 * quote.  "wood.png" inside "darkwood.png" fails the left test; wood.png in a
 * sentence fails the right one.  Without this the rewrite corrupts prose and
 * neighbouring filenames, which is worse than the broken reference it set out
 * to fix. */
bool basename_is_delimited(const char *hay, size_t hay_len, size_t i,
                           size_t needle_len)
{
    if (i > 0) {
        const char p = hay[i - 1];
        if (p != '/' && p != '\\' && p != '"' && p != '\'') return false;
    }
    const size_t end = i + needle_len;
    if (end >= hay_len) return false;          /* must be closed, not truncated */
    const char n = hay[end];
    return n == '"' || n == '\'';
}

struct Out {
    char  *buf = nullptr;
    size_t len = 0;
    size_t cap = 0;

    bool reserve(size_t need)
    {
        if (need <= cap) return true;
        size_t c = cap ? cap : 256;
        while (c < need) c *= 2;
        char *n = (char *)jce_realloc(buf, c);
        if (!n) return false;
        buf = n;
        cap = c;
        return true;
    }
    bool put(const char *s, size_t n)
    {
        if (!reserve(len + n + 1)) return false;
        memcpy(buf + len, s, n);
        len += n;
        return true;
    }
};

} /* namespace */

extern "C" int jce_asset_ref_rewrite(const char *text, size_t len,
                                     const char *old_rel,  const char *new_rel,
                                     const char *old_base, const char *new_base,
                                     char **out, size_t *out_len)
{
    if (!text || !out) return 0;

    const size_t rel_n  = (old_rel  && new_rel)  ? strlen(old_rel)  : 0;
    const size_t base_n = (old_base && new_base) ? strlen(old_base) : 0;
    if (rel_n == 0 && base_n == 0) return 0;

    Out o;
    int hits = 0;
    size_t i = 0;
    while (i < len) {
        /* Longest first: a rel path ENDS with the basename, so testing the
         * basename first would rewrite the tail of a path and leave a
         * half-renamed reference behind -- silently, and only for the files
         * that spelled the reference the more careful way. */
        if (rel_n && sep_insensitive_match(text, len, i, old_rel, rel_n)) {
            if (!o.put(new_rel, strlen(new_rel))) { jce_free(o.buf); return 0; }
            i += rel_n;
            hits++;
            continue;
        }
        if (base_n && sep_insensitive_match(text, len, i, old_base, base_n) &&
            basename_is_delimited(text, len, i, base_n)) {
            if (!o.put(new_base, strlen(new_base))) { jce_free(o.buf); return 0; }
            i += base_n;
            hits++;
            continue;
        }
        if (!o.put(text + i, 1)) { jce_free(o.buf); return 0; }
        i++;
    }

    if (hits == 0) { jce_free(o.buf); return 0; }
    if (!o.reserve(o.len + 1)) { jce_free(o.buf); return 0; }
    o.buf[o.len] = '\0';
    *out = o.buf;
    if (out_len) *out_len = o.len;
    return hits;
}
