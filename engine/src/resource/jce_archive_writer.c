/* jce_archive_writer.c
 *
 * Single-pass deterministic writer for the JCE Archive format (JPAK,
 * format_version 1).  Resources are compressed independently with zstd
 * under the "keep only if it helps" guard (spec §6.3), the index is sorted
 * ascending by path hash, and the 64-byte header is back-patched last
 * (spec §3).  Identical inputs + config produce byte-identical output
 * (spec §10.5): hashes are unique so the sort order is total, the zstd
 * level is pinned, and compression is single-threaded.
 */

#include <jce/resource/jce_archive_writer.h>
#include <jce/resource/jce_archive.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>

#include "resource/jce_archive_format.h"
#include "resource/jce_archive_crypto.h"

#include <stdlib.h>
#include <string.h>

#include <xxhash.h>
#include <zstd.h>

#define JARC_TAG "archive"

/* Compress is kept only when stored < 95% of original (spec §6.3). */
#define JARC_KEEP_NUM 95
#define JARC_KEEP_DEN 100

typedef struct {
    uint64_t path_hash;
    char    *norm_path;
    size_t   norm_len;
    uint8_t *stored;        /* compressed-or-raw payload */
    uint32_t stored_size;
    uint32_t original_size;
    uint32_t content_crc;
    uint8_t  compression;   /* JARC_COMP_NONE / _ZSTD / _ZSTD_DICT */
    uint16_t dict_id;       /* dictionary index, or JARC_DICT_ID_NONE */
    uint8_t  encrypted;     /* 1 => `stored` bytes are ChaCha20-encrypted */
} WEntry;

/* A registered shared dictionary (spec §7).  The bytes are copied and a CDict
 * is built once at registration so every add_with_dict reuses it. */
typedef struct {
    uint32_t    tag;
    uint8_t    *bytes;
    uint32_t    size;
    ZSTD_CDict *cdict;
} WDict;

struct JceArchiveWriter {
    JceArchiveWriterConfig cfg;
    WEntry    *entries;
    size_t     count;
    size_t     cap;
    WDict     *dicts;
    size_t     dict_count;
    size_t     dict_cap;
    ZSTD_CCtx *cctx;
    uint8_t    enc_key[JCE_ARCHIVE_KEY_BYTES];
    int        has_key;     /* 1 once a key has been set */
    int        any_encrypted;
};

/* ── Dynamic byte buffer ─────────────────────────────────────────────── */

typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   cap;
    int      oom;
} ByteBuf;

static void bb_reserve(ByteBuf *b, size_t extra) {
    if (b->oom) return;
    if (b->size + extra <= b->cap) return;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < b->size + extra) ncap *= 2;
    uint8_t *nd = (uint8_t *)jce_realloc(b->data, ncap);
    if (!nd) { b->oom = 1; return; }
    b->data = nd;
    b->cap = ncap;
}

static void bb_append(ByteBuf *b, const void *p, size_t n) {
    bb_reserve(b, n);
    if (b->oom) return;
    if (n) memcpy(b->data + b->size, p, n);
    b->size += n;
}

static void bb_append_zeros(ByteBuf *b, size_t n) {
    bb_reserve(b, n);
    if (b->oom) return;
    if (n) memset(b->data + b->size, 0, n);
    b->size += n;
}

/* ── Writer lifecycle ────────────────────────────────────────────────── */

JceArchiveWriter *jce_archive_writer_create(const JceArchiveWriterConfig *cfg) {
    JceArchiveWriter *w = (JceArchiveWriter *)jce_malloc(sizeof(JceArchiveWriter));
    if (!w) return NULL;
    memset(w, 0, sizeof(*w));

    if (cfg) {
        w->cfg = *cfg;
    } else {
        w->cfg.zstd_level     = 9;
        w->cfg.alignment_log2 = 4; /* 16-byte */
    }
    if (w->cfg.zstd_level <= 0) w->cfg.zstd_level = 9;

    w->cctx = ZSTD_createCCtx();
    if (!w->cctx) { jce_free(w); return NULL; }
    return w;
}

void jce_archive_writer_destroy(JceArchiveWriter *w) {
    if (!w) return;
    for (size_t i = 0; i < w->count; i++) {
        jce_free(w->entries[i].norm_path);
        jce_free(w->entries[i].stored);
    }
    jce_free(w->entries);
    for (size_t i = 0; i < w->dict_count; i++) {
        if (w->dicts[i].cdict) ZSTD_freeCDict(w->dicts[i].cdict);
        jce_free(w->dicts[i].bytes);
    }
    jce_free(w->dicts);
    if (w->cctx) ZSTD_freeCCtx(w->cctx);
    jce_free(w);
}

/* ── Dictionaries ────────────────────────────────────────────────────── */

int jce_archive_writer_add_dictionary(JceArchiveWriter *w, uint32_t tag,
                                      const void *dict, size_t dict_size) {
    if (!w || !dict || dict_size == 0) return -1;
    if (dict_size > 0xFFFFFFFFu) {
        LOG_ERROR(JARC_TAG, "add_dictionary: dictionary exceeds 4 GB");
        return -1;
    }
    if (w->dict_count >= JARC_DICT_ID_NONE) { /* reserve 0xFFFF sentinel */
        LOG_ERROR(JARC_TAG, "add_dictionary: too many dictionaries");
        return -1;
    }

    if (w->dict_count == w->dict_cap) {
        size_t ncap = w->dict_cap ? w->dict_cap * 2 : 4;
        WDict *nd = (WDict *)jce_realloc(w->dicts, ncap * sizeof(WDict));
        if (!nd) return -1;
        w->dicts = nd;
        w->dict_cap = ncap;
    }

    uint8_t *copy = (uint8_t *)jce_malloc(dict_size);
    if (!copy) return -1;
    memcpy(copy, dict, dict_size);

    ZSTD_CDict *cd = ZSTD_createCDict(copy, dict_size, w->cfg.zstd_level);
    if (!cd) { jce_free(copy); return -1; }

    WDict *d = &w->dicts[w->dict_count];
    d->tag   = tag;
    d->bytes = copy;
    d->size  = (uint32_t)dict_size;
    d->cdict = cd;
    return (int)w->dict_count++;
}

/* ── Add ─────────────────────────────────────────────────────────────── */

/* Compress `data` with the keep-only-if-it-helps guard (spec §6.3 / §7).
 * With a dictionary the guard still governs: dictionary compression is kept
 * only if it shrinks the resource below the threshold, else the resource is
 * stored raw with no dictionary.  On success fills the out-params (caller owns
 * *out_stored, free with jce_free) and returns 1; returns 0 only on allocation
 * failure.  This is the single canonical compression stage, shared by the
 * normal add path and by jce_archive_compress_resource() (build cache, §11.1). */
static int compress_keep(JceArchiveWriter *w, const void *data, size_t size,
                         int dict_id, uint8_t **out_stored,
                         uint32_t *out_stored_size, uint8_t *out_comp,
                         uint16_t *out_dict, uint32_t *out_crc) {
    *out_stored      = NULL;
    *out_stored_size = (uint32_t)size;
    *out_comp        = JARC_COMP_NONE;
    *out_dict        = JARC_DICT_ID_NONE;
    *out_crc         = (uint32_t)XXH32(data, size, 0);

    if (size == 0) return 1;

    size_t bound = ZSTD_compressBound(size);
    uint8_t *tmp = (uint8_t *)jce_malloc(bound);
    if (!tmp) return 0;

    size_t  cs;
    uint8_t want_comp;
    if (dict_id >= 0) {
        cs = ZSTD_compress_usingCDict(w->cctx, tmp, bound, data, size,
                                      w->dicts[dict_id].cdict);
        want_comp = JARC_COMP_ZSTD_DICT;
    } else {
        cs = ZSTD_compressCCtx(w->cctx, tmp, bound, data, size, w->cfg.zstd_level);
        want_comp = JARC_COMP_ZSTD;
    }

    if (!ZSTD_isError(cs) &&
        (uint64_t)cs * JARC_KEEP_DEN < (uint64_t)size * JARC_KEEP_NUM) {
        *out_stored      = tmp;
        *out_stored_size = (uint32_t)cs;
        *out_comp        = want_comp;
        if (want_comp == JARC_COMP_ZSTD_DICT) *out_dict = (uint16_t)dict_id;
    } else {
        jce_free(tmp);
        uint8_t *raw = (uint8_t *)jce_malloc(size);
        if (!raw) return 0;
        memcpy(raw, data, size);
        *out_stored      = raw;
        *out_stored_size = (uint32_t)size;
        *out_comp        = JARC_COMP_NONE;
    }
    return 1;
}

static bool writer_add_impl(JceArchiveWriter *w, const char *path,
                            const void *data, size_t size, int dict_id,
                            int encrypt) {
    if (!w || !path || (size && !data)) return false;
    if (size > 0xFFFFFFFFu) {
        LOG_ERROR(JARC_TAG, "add: resource exceeds 4 GB cap");
        return false;
    }
    if (dict_id >= 0 && (size_t)dict_id >= w->dict_count) {
        LOG_ERROR(JARC_TAG, "add: invalid dict_id %d", dict_id);
        return false;
    }
    if (encrypt && !w->has_key) {
        LOG_ERROR(JARC_TAG, "add: encryption requested but no key set");
        return false;
    }

    /* Normalize the path (heap buffer sized to the input). */
    size_t cap = strlen(path) + 1;
    char *norm = (char *)jce_malloc(cap);
    if (!norm) return false;
    size_t nlen = jce_archive_normalize_path(path, norm, cap);
    if (nlen == 0) {
        LOG_ERROR(JARC_TAG, "add: path normalizes to empty: '%s'", path);
        jce_free(norm);
        return false;
    }
    uint64_t hash = jce_archive_hash_normalized(norm, nlen);

    /* Collision / duplicate detection (spec §12.3). */
    for (size_t i = 0; i < w->count; i++) {
        if (w->entries[i].path_hash == hash) {
            if (w->entries[i].norm_len == nlen &&
                memcmp(w->entries[i].norm_path, norm, nlen) == 0) {
                LOG_ERROR(JARC_TAG, "add: duplicate path '%s'", norm);
            } else {
                LOG_ERROR(JARC_TAG, "add: hash collision '%s' vs '%s'",
                          norm, w->entries[i].norm_path);
            }
            jce_free(norm);
            return false;
        }
    }

    /* Compress with the keep-only-if-it-helps guard (spec §6.3 / §7). */
    uint8_t  comp = JARC_COMP_NONE;
    uint8_t *stored = NULL;
    uint32_t stored_size = (uint32_t)size;
    uint16_t entry_dict = JARC_DICT_ID_NONE;
    uint32_t crc = 0;
    if (!compress_keep(w, data, size, dict_id,
                       &stored, &stored_size, &comp, &entry_dict, &crc)) {
        jce_free(norm);
        return false;
    }

    /* Encrypt last, after compression (spec §9.2 ordering): encrypted bytes
     * are incompressible, so compression must precede encryption.  The
     * keystream is keyed by the entry's unique path hash. */
    if (encrypt && stored_size > 0 && stored) {
        uint8_t nonce[JCE_ARCHIVE_NONCE_BYTES];
        jce_archive_derive_nonce(hash, w->cfg.encryption_salt, nonce);
        jce_archive_chacha20_xor(w->enc_key, nonce, 1, stored, stored, stored_size);
        w->any_encrypted = 1;
    }

    /* Grow the entry array. */
    if (w->count == w->cap) {
        size_t ncap = w->cap ? w->cap * 2 : 16;
        WEntry *ne = (WEntry *)jce_realloc(w->entries, ncap * sizeof(WEntry));
        if (!ne) { jce_free(norm); jce_free(stored); return false; }
        w->entries = ne;
        w->cap = ncap;
    }

    WEntry *e = &w->entries[w->count++];
    e->path_hash     = hash;
    e->norm_path     = norm;
    e->norm_len      = nlen;
    e->stored        = stored;
    e->stored_size   = stored_size;
    e->original_size = (uint32_t)size;
    e->content_crc   = crc;
    e->compression   = comp;
    e->dict_id       = entry_dict;
    e->encrypted     = (uint8_t)((encrypt && stored_size > 0) ? 1 : 0);
    return true;
}

void jce_archive_writer_set_encryption_key(JceArchiveWriter *w,
                                           const uint8_t key[32]) {
    if (!w || !key) return;
    memcpy(w->enc_key, key, JCE_ARCHIVE_KEY_BYTES);
    w->has_key = 1;
}

uint32_t jce_archive_salt_from_label(const char *label) {
    if (!label || !label[0]) return 0;
    uint64_t h = (uint64_t)XXH3_64bits(label, strlen(label));
    return (uint32_t)(h ^ (h >> 32));
}

bool jce_archive_writer_add(JceArchiveWriter *w, const char *path,
                            const void *data, size_t size) {
    return writer_add_impl(w, path, data, size, -1, 0);
}

bool jce_archive_writer_add_with_dict(JceArchiveWriter *w, const char *path,
                                      const void *data, size_t size, int dict_id) {
    return writer_add_impl(w, path, data, size, dict_id, 0);
}

bool jce_archive_writer_add_encrypted(JceArchiveWriter *w, const char *path,
                                      const void *data, size_t size, int dict_id) {
    return writer_add_impl(w, path, data, size, dict_id, 1);
}

int jce_archive_compress_resource(JceArchiveWriter *w, const void *data,
                                  size_t size, int dict_id,
                                  JceArchiveBlob *out) {
    if (!w || (size && !data) || !out) return 0;
    if (size > 0xFFFFFFFFu) return 0;
    if (dict_id >= 0 && (size_t)dict_id >= w->dict_count) return 0;

    uint8_t *stored = NULL;
    uint32_t stored_size = (uint32_t)size;
    uint8_t  comp = JARC_COMP_NONE;
    uint16_t dict = JARC_DICT_ID_NONE;
    uint32_t crc = 0;
    if (!compress_keep(w, data, size, dict_id,
                       &stored, &stored_size, &comp, &dict, &crc))
        return 0;

    out->bytes         = stored;
    out->stored_size   = stored_size;
    out->original_size = (uint32_t)size;
    out->content_crc   = crc;
    out->compression   = comp;
    out->dict_id       = dict;
    return 1;
}

bool jce_archive_writer_add_precompressed(JceArchiveWriter *w, const char *path,
                                          const JceArchiveBlob *blob) {
    if (!w || !path || !blob) return false;
    if (blob->stored_size && !blob->bytes) return false;
    if (blob->dict_id != JARC_DICT_ID_NONE && blob->dict_id >= w->dict_count) {
        LOG_ERROR(JARC_TAG, "add_precompressed: invalid dict_id %u",
                  (unsigned)blob->dict_id);
        return false;
    }

    size_t cap = strlen(path) + 1;
    char *norm = (char *)jce_malloc(cap);
    if (!norm) return false;
    size_t nlen = jce_archive_normalize_path(path, norm, cap);
    if (nlen == 0) {
        LOG_ERROR(JARC_TAG, "add_precompressed: path normalizes to empty: '%s'", path);
        jce_free(norm);
        return false;
    }
    uint64_t hash = jce_archive_hash_normalized(norm, nlen);

    for (size_t i = 0; i < w->count; i++) {
        if (w->entries[i].path_hash == hash) {
            if (w->entries[i].norm_len == nlen &&
                memcmp(w->entries[i].norm_path, norm, nlen) == 0)
                LOG_ERROR(JARC_TAG, "add_precompressed: duplicate path '%s'", norm);
            else
                LOG_ERROR(JARC_TAG, "add_precompressed: hash collision '%s' vs '%s'",
                          norm, w->entries[i].norm_path);
            jce_free(norm);
            return false;
        }
    }

    /* Copy the caller's blob so the writer owns a free-able buffer. */
    uint8_t *stored = NULL;
    if (blob->stored_size) {
        stored = (uint8_t *)jce_malloc(blob->stored_size);
        if (!stored) { jce_free(norm); return false; }
        memcpy(stored, blob->bytes, blob->stored_size);
    }

    if (w->count == w->cap) {
        size_t ncap = w->cap ? w->cap * 2 : 16;
        WEntry *ne = (WEntry *)jce_realloc(w->entries, ncap * sizeof(WEntry));
        if (!ne) { jce_free(norm); jce_free(stored); return false; }
        w->entries = ne;
        w->cap = ncap;
    }

    WEntry *e = &w->entries[w->count++];
    e->path_hash     = hash;
    e->norm_path     = norm;
    e->norm_len      = nlen;
    e->stored        = stored;
    e->stored_size   = blob->stored_size;
    e->original_size = blob->original_size;
    e->content_crc   = blob->content_crc;
    e->compression   = blob->compression;
    e->dict_id       = blob->dict_id;
    e->encrypted     = 0;
    return true;
}

static int cmp_entry(const void *a, const void *b) {
    uint64_t ha = ((const WEntry *)a)->path_hash;
    uint64_t hb = ((const WEntry *)b)->path_hash;
    if (ha < hb) return -1;
    if (ha > hb) return 1;
    return 0;
}

/* Content-dedup helper: sort candidate entries so byte-identical payloads end
 * up adjacent.  We group by (content_crc, stored_size) and break ties by emit
 * index so the canonical (smallest-index) copy is found first.  The crc is
 * XXH32 of the *original* bytes, which uniquely groups identical content; the
 * final byte-for-byte check (in the dedup pass) guards against crc collisions
 * and differing compression/dict parameters. */
typedef struct {
    uint32_t crc;
    uint32_t stored_size;
    size_t   idx;
} DupKey;

static int dup_key_cmp(const void *a, const void *b) {
    const DupKey *x = (const DupKey *)a;
    const DupKey *y = (const DupKey *)b;
    if (x->crc != y->crc) return x->crc < y->crc ? -1 : 1;
    if (x->stored_size != y->stored_size) return x->stored_size < y->stored_size ? -1 : 1;
    if (x->idx != y->idx) return x->idx < y->idx ? -1 : 1;
    return 0;
}

/* Compute dup_of[i] = canonical entry index whose stored bytes entry i shares,
 * or -1 when entry i is itself canonical (or dedup is disabled).  Canonical is
 * always the smallest emit index of a byte-identical set, so it is emitted
 * before any entry referencing it.  Encrypted entries never dedup because their
 * per-path nonce makes each ciphertext unique.  Returns false on OOM. */
static bool compute_dedup(const WEntry *entries, size_t count, int64_t *dup_of) {
    for (size_t i = 0; i < count; i++) dup_of[i] = -1;
    if (count < 2) return true;

    DupKey *keys = (DupKey *)jce_malloc(sizeof(DupKey) * count);
    if (!keys) return false;
    size_t m = 0;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].encrypted) continue; /* never coalesce ciphertext */
        keys[m].crc         = entries[i].content_crc;
        keys[m].stored_size = entries[i].stored_size;
        keys[m].idx         = i;
        m++;
    }
    qsort(keys, m, sizeof(DupKey), dup_key_cmp);

    for (size_t a = 0; a < m;) {
        size_t b = a + 1;
        while (b < m && keys[b].crc == keys[a].crc &&
               keys[b].stored_size == keys[a].stored_size)
            b++;
        /* [a,b) share (crc, size): confirm byte equality + matching codec. */
        for (size_t i = a; i < b; i++) {
            size_t ci = keys[i].idx;
            if (dup_of[ci] >= 0) continue; /* already a dup of an earlier copy */
            for (size_t j = i + 1; j < b; j++) {
                size_t cj = keys[j].idx;
                if (dup_of[cj] >= 0) continue;
                if (entries[ci].compression == entries[cj].compression &&
                    entries[ci].dict_id == entries[cj].dict_id &&
                    entries[ci].stored_size == entries[cj].stored_size &&
                    memcmp(entries[ci].stored, entries[cj].stored,
                           entries[ci].stored_size) == 0) {
                    dup_of[cj] = (int64_t)ci; /* ci < cj => emitted first */
                }
            }
        }
        a = b;
    }
    jce_free(keys);
    return true;
}

/* ── Finish ──────────────────────────────────────────────────────────── */

bool jce_archive_writer_finish(JceArchiveWriter *w, void **out_buf, size_t *out_size) {
    if (!w || !out_buf || !out_size) return false;

    /* Deterministic total order: hashes are unique (dups rejected on add). */
    if (w->count > 1) qsort(w->entries, w->count, sizeof(WEntry), cmp_entry);

    size_t align = (size_t)1u << w->cfg.alignment_log2;
    int global_page_aligned = (w->cfg.alignment_log2 >= 12);

    /* Per-entry flags, filled while laying out the data region below. */
    uint8_t *entry_flags = (uint8_t *)jce_malloc(w->count ? w->count : 1);
    if (!entry_flags) return false;

    ByteBuf buf = {0};

    /* Header placeholder + dictionary table placeholder (both back-patched).
     * The dictionary table sits immediately after the header (spec §3) so the
     * data region — and thus data_content_hash — begins right after it. */
    bb_append_zeros(&buf, JARC_HEADER_SIZE);
    bb_append_zeros(&buf, w->dict_count * JARC_DICT_ENTRY_SIZE);
    size_t data_start = buf.size;

    /* Dictionary bytes lead the data region (spec §3 / §4.4); record each
     * dictionary's absolute offset to back-patch the table below. */
    uint64_t *dict_offsets = (uint64_t *)jce_malloc(
        sizeof(uint64_t) * (w->dict_count ? w->dict_count : 1));
    if (!dict_offsets) { jce_free(entry_flags); jce_free(buf.data); return false; }
    for (size_t d = 0; d < w->dict_count; d++) {
        if (align > 1) {
            size_t pad = (align - (buf.size % align)) % align;
            bb_append_zeros(&buf, pad);
        }
        dict_offsets[d] = buf.size;
        bb_append(&buf, w->dicts[d].bytes, w->dicts[d].size);
    }
    if (buf.oom) { jce_free(entry_flags); jce_free(dict_offsets); jce_free(buf.data); return false; }

    /* Data region: write payloads in sorted order, recording absolute
     * offsets.  We stash each entry's data_offset in stored_size's sibling
     * by using a parallel array. */
    uint64_t *data_offsets = (uint64_t *)jce_malloc(sizeof(uint64_t) * (w->count ? w->count : 1));
    if (!data_offsets) { jce_free(entry_flags); jce_free(dict_offsets); jce_free(buf.data); return false; }

    /* Content dedup (spec-neutral; reader unaffected): byte-identical payloads
     * share one on-disk copy via a common data_offset. */
    int64_t *dup_of = (int64_t *)jce_malloc(sizeof(int64_t) * (w->count ? w->count : 1));
    if (!dup_of) { jce_free(entry_flags); jce_free(dict_offsets); jce_free(data_offsets); jce_free(buf.data); return false; }
    if (w->cfg.dedup_content) {
        if (!compute_dedup(w->entries, w->count, dup_of)) {
            jce_free(dup_of); jce_free(entry_flags); jce_free(dict_offsets);
            jce_free(data_offsets); jce_free(buf.data); return false;
        }
    } else {
        for (size_t i = 0; i < w->count; i++) dup_of[i] = -1;
    }

    for (size_t i = 0; i < w->count; i++) {
        if (dup_of[i] >= 0) {
            /* Reuse the canonical copy's offset; no bytes, no padding. The
             * canonical (smaller index) was laid out earlier this loop. */
            size_t c = (size_t)dup_of[i];
            data_offsets[i] = data_offsets[c];
            entry_flags[i]  = (uint8_t)(entry_flags[c] & JARC_ENTRY_PAGE_ALIGNED);
            continue;
        }
        /* mmap zero-copy (spec §8.2) requires page alignment, and only
         * uncompressed, unencrypted entries are usable in place. */
        int want_page = (w->cfg.mmap_friendly &&
                         w->entries[i].compression == JARC_COMP_NONE &&
                         !w->entries[i].encrypted);
        size_t ealign = want_page ? 4096 : align;
        if (ealign > 1) {
            size_t pad = (ealign - (buf.size % ealign)) % ealign;
            bb_append_zeros(&buf, pad);
        }
        data_offsets[i] = buf.size;
        uint8_t ef = (uint8_t)(((want_page || global_page_aligned) &&
                                (buf.size % 4096) == 0)
                                   ? JARC_ENTRY_PAGE_ALIGNED : 0);
        if (w->entries[i].encrypted) ef |= JARC_ENTRY_ENCRYPTED;
        entry_flags[i] = ef;
        bb_append(&buf, w->entries[i].stored, w->entries[i].stored_size);
    }
    jce_free(dup_of);
    if (buf.oom) { jce_free(entry_flags); jce_free(dict_offsets); jce_free(data_offsets); jce_free(buf.data); return false; }

    size_t data_end = buf.size;

    /* Build the raw index region. */
    size_t index_orig_size = w->count * JARC_INDEX_ENTRY_SIZE;
    uint8_t *index_raw = (uint8_t *)jce_malloc(index_orig_size ? index_orig_size : 1);
    if (!index_raw) { jce_free(entry_flags); jce_free(dict_offsets); jce_free(data_offsets); jce_free(buf.data); return false; }
    for (size_t i = 0; i < w->count; i++) {
        uint8_t *e = index_raw + i * JARC_INDEX_ENTRY_SIZE;
        const WEntry *we = &w->entries[i];
        jarc_wr64(e + JARC_EOFF_PATH_HASH,   we->path_hash);
        jarc_wr64(e + JARC_EOFF_DATA_OFFSET, data_offsets[i]);
        jarc_wr32(e + JARC_EOFF_STORED_SIZE, we->stored_size);
        jarc_wr32(e + JARC_EOFF_ORIG_SIZE,   we->original_size);
        e[JARC_EOFF_COMPRESSION] = we->compression;
        e[JARC_EOFF_ENTRY_FLAGS] = entry_flags[i];
        jarc_wr16(e + JARC_EOFF_DICT_ID, we->dict_id);
        jarc_wr32(e + JARC_EOFF_CONTENT_CRC, we->content_crc);
    }
    jce_free(data_offsets);
    jce_free(entry_flags);

    /* Optionally zstd-compress the index, keeping raw if it doesn't shrink. */
    uint32_t hdr_flags = w->cfg.mmap_friendly ? JARC_FLAG_MMAP_FRIENDLY : 0;
    if (w->any_encrypted) hdr_flags |= JARC_FLAG_ENCRYPTED;
    const uint8_t *index_stored = index_raw;
    size_t index_stored_size = index_orig_size;
    uint8_t *index_comp = NULL;
    if (w->cfg.compress_index && index_orig_size > 0) {
        size_t bound = ZSTD_compressBound(index_orig_size);
        index_comp = (uint8_t *)jce_malloc(bound);
        if (!index_comp) { jce_free(dict_offsets); jce_free(index_raw); jce_free(buf.data); return false; }
        size_t cs = ZSTD_compressCCtx(w->cctx, index_comp, bound,
                                      index_raw, index_orig_size, w->cfg.zstd_level);
        if (!ZSTD_isError(cs) && cs < index_orig_size) {
            index_stored = index_comp;
            index_stored_size = cs;
            hdr_flags |= JARC_FLAG_INDEX_COMPRESSED;
        } else {
            jce_free(index_comp);
            index_comp = NULL;
        }
    }

    size_t index_offset = buf.size;
    bb_append(&buf, index_stored, index_stored_size);

    /* Hashes (spec §9.1): over the data region and the index as stored. */
    uint64_t data_hash = 0;
    if (data_end > data_start)
        data_hash = (uint64_t)XXH3_64bits(buf.data + data_start, data_end - data_start);
    uint64_t index_hash = (uint64_t)XXH3_64bits(buf.data + index_offset, index_stored_size);

    /* Optional debug path table (spec §4.8), in sorted index order. */
    if (w->cfg.emit_debug_paths) {
        hdr_flags |= JARC_FLAG_HAS_DEBUG_PATHS;
        uint8_t cnt[4];
        jarc_wr32(cnt, (uint32_t)w->count);
        bb_append(&buf, cnt, 4);
        for (size_t i = 0; i < w->count; i++) {
            uint8_t len[2];
            jarc_wr16(len, (uint16_t)w->entries[i].norm_len);
            bb_append(&buf, len, 2);
            bb_append(&buf, w->entries[i].norm_path, w->entries[i].norm_len);
        }
    }

    jce_free(index_raw);
    if (index_comp) jce_free(index_comp);

    if (buf.oom) { jce_free(dict_offsets); jce_free(buf.data); return false; }

    /* Back-patch the dictionary table (immediately after the header). */
    for (size_t d = 0; d < w->dict_count; d++) {
        uint8_t *dt = buf.data + JARC_HEADER_SIZE + d * JARC_DICT_ENTRY_SIZE;
        jarc_wr64(dt + JARC_DOFF_DATA_OFFSET, dict_offsets[d]);
        jarc_wr32(dt + JARC_DOFF_SIZE,        w->dicts[d].size);
        jarc_wr32(dt + JARC_DOFF_TAG,         w->dicts[d].tag);
    }
    jce_free(dict_offsets);

    /* Back-patch the 64-byte header. */
    uint8_t *h = buf.data;
    h[JARC_OFF_MAGIC + 0] = JARC_MAGIC_0;
    h[JARC_OFF_MAGIC + 1] = JARC_MAGIC_1;
    h[JARC_OFF_MAGIC + 2] = JARC_MAGIC_2;
    h[JARC_OFF_MAGIC + 3] = JARC_MAGIC_3;
    jarc_wr32(h + JARC_OFF_FORMAT_VERSION,       JARC_FORMAT_VERSION);
    jarc_wr32(h + JARC_OFF_FLAGS,                hdr_flags);
    jarc_wr32(h + JARC_OFF_ENTRY_COUNT,          (uint32_t)w->count);
    jarc_wr64(h + JARC_OFF_INDEX_OFFSET,         index_offset);
    jarc_wr64(h + JARC_OFF_INDEX_STORED_SIZE,    index_stored_size);
    jarc_wr64(h + JARC_OFF_INDEX_ORIGINAL_SIZE,  index_orig_size);
    jarc_wr64(h + JARC_OFF_DATA_CONTENT_HASH,    data_hash);
    jarc_wr64(h + JARC_OFF_INDEX_CONTENT_HASH,   index_hash);
    jarc_wr16(h + JARC_OFF_DICT_COUNT,           (uint16_t)w->dict_count);
    h[JARC_OFF_DEFAULT_COMPRESSION] = JARC_COMP_ZSTD;
    h[JARC_OFF_ALIGNMENT_LOG2]      = w->cfg.alignment_log2;
    /* nonce_salt32 (ex-RESERVED): only meaningful when entries are
     * encrypted; kept 0 otherwise for byte-compat with pre-salt archives. */
    jarc_wr32(h + JARC_OFF_NONCE_SALT32,
              w->any_encrypted ? w->cfg.encryption_salt : 0);

    *out_buf = buf.data;
    *out_size = buf.size;
    return true;
}
