/* jce_archive_path.c
 *
 * Canonical virtual-path normalization (spec §10.3) and XXH3-64 path
 * hashing (spec §10.4) for the JCE Archive format.  This is the single
 * source of truth the editor, build pipeline, and runtime all share; any
 * divergence would hash the same logical resource differently, so the
 * procedure lives here and nowhere else.
 */

#include <jce/resource/jce_archive.h>

#include <jce/os/core/jce_alloc.h>

#include <string.h>

#include <xxhash.h>

/* Resolve and append/pop segments into `out`, lowercasing, collapsing
 * separators, dropping "." and resolving ".." against earlier segments. */
size_t jce_archive_normalize_path(const char *in, char *out, size_t out_cap) {
    if (!in || !out || out_cap == 0) return 0;

    const char *p = in;

    /* (1) strip a leading drive letter of the form "X:".
     *
     * Open-coded on purpose (audit DUP-044): this function IS the canonical
     * definition of resource identity, and the TU is compiled into the
     * standalone jce_pak host tool, which links no engine library, so it must
     * stay dependency-free. */
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
        p[1] == ':') {
        p += 2;
    }

    /* (2) strip leading separators so the path becomes relative. */
    while (*p == '/' || *p == '\\') p++;

    size_t in_len = strlen(p);
    /* Per-segment content start offsets in `out`, for ".." popping.
     * Stack-first (mirrors jce_archive_hash_path's stackbuf): a segment needs
     * at least 2 input chars ("x/"), so 256 slots cover any path up to 511
     * chars — every lookup in the engine.  Only a pathological longer path
     * pays the heap round-trip; before this EVERY pak lookup paid a
     * malloc+free pair (allocator contention on worker-thread decode paths). */
    size_t  seg_stack[256];
    size_t *seg = seg_stack;
    if (in_len + 1 > sizeof(seg_stack) / sizeof(seg_stack[0])) {
        seg = (size_t *)jce_malloc((in_len + 1) * sizeof(size_t));
        if (!seg) return 0;
    }
    size_t seg_count = 0;
    size_t out_len = 0;

    while (*p) {
        const char *s = p;
        /* (3) backslash and forward slash both terminate a segment. */
        while (*p && *p != '/' && *p != '\\') p++;
        size_t slen = (size_t)(p - s);

        if (slen == 1 && s[0] == '.') {
            /* (6) drop "." segments. */
        } else if (slen == 2 && s[0] == '.' && s[1] == '.') {
            /* (6) ".." removes the preceding segment, if any. */
            if (seg_count > 0) {
                size_t k = --seg_count;
                out_len = (k > 0) ? seg[k] - 1 : 0;
            }
        } else if (slen > 0) {
            /* Append a separator before all but the first kept segment. */
            if (seg_count > 0) {
                if (out_len + 1 + 1 > out_cap) { if (seg != seg_stack) jce_free(seg); return 0; }
                out[out_len++] = '/';
            }
            seg[seg_count] = out_len; /* content start */
            if (out_len + slen + 1 > out_cap) { if (seg != seg_stack) jce_free(seg); return 0; }
            for (size_t i = 0; i < slen; i++) {
                char c = s[i];
                /* (5) ASCII uppercase -> lowercase. */
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                out[out_len++] = c;
            }
            seg_count++;
        }

        /* (4) collapse runs of consecutive separators. */
        while (*p == '/' || *p == '\\') p++;
    }

    /* (7) no trailing slash is possible here (we never trail one). */
    out[out_len] = '\0';
    if (seg != seg_stack) jce_free(seg);
    return out_len;
}

uint64_t jce_archive_hash_normalized(const char *norm, size_t len) {
    if (!norm) return 0;
    return (uint64_t)XXH3_64bits(norm, len);
}

uint64_t jce_archive_content_hash(const void *data, size_t size) {
    if (!data && size > 0) return 0;
    return (uint64_t)XXH3_64bits(data ? data : "", size);
}

uint64_t jce_archive_hash_path(const char *path) {
    if (!path) return 0;

    char stackbuf[512];
    size_t cap = sizeof(stackbuf);
    char *buf = stackbuf;

    /* Grow to a heap buffer if the path is unusually long. */
    size_t need = strlen(path) + 1;
    if (need > cap) {
        buf = (char *)jce_malloc(need);
        if (!buf) return 0;
        cap = need;
    }

    uint64_t hash = 0;
    size_t n = jce_archive_normalize_path(path, buf, cap);
    if (n > 0) hash = jce_archive_hash_normalized(buf, n);

    if (buf != stackbuf) jce_free(buf);
    return hash;
}
