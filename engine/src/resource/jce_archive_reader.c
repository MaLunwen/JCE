/* jce_archive_reader.c
 *
 * Runtime reader for the JCE Archive format (JPAK, format_version 1).
 *
 * Open validates and decodes the 64-byte header and the (optionally
 * zstd-compressed) index region into a resident, hash-sorted array of
 * entries; the data region stays on the blob and is decompressed on demand
 * (spec §5.3).  Lookup is a binary search over path hashes (spec §5.2).
 */

#include <jce/resource/jce_archive.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/platform/jce_mmap.h>

#include "resource/jce_archive_format.h"
#include "resource/jce_archive_crypto.h"
#include "resource/jce_read_bounds.h"

#include <string.h>

#include <xxhash.h>
#include <zstd.h>

#define JARC_TAG "archive"

/* Resident copy of one dictionary-table record (spec §4.4). */
typedef struct {
    uint64_t offset; /* dictionary bytes' offset within the blob */
    uint32_t size;
    uint32_t tag;
} JarcDictInfo;

struct JceArchive {
    const uint8_t   *blob;
    size_t           blob_size; /* authenticated bytes, excludes final tag */
    int              owns_blob;
    JceMmap         *map;       /* set when opened via jce_archive_open_file */

    uint32_t         flags;
    uint16_t         dict_count;
    uint32_t         nonce_salt;   /* header nonce_salt32 (ex-RESERVED)   */
    uint64_t         index_offset;
    uint64_t         index_stored_size;
    uint64_t         data_content_hash;
    uint64_t         index_content_hash;

    uint32_t         entry_count;
    JceArchiveEntry *entries;   /* resident sorted array */
    char           **debug_paths; /* entry_count strings, or NULL */

    JarcDictInfo    *dicts;     /* dict_count records, or NULL */
    ZSTD_DDict     **ddicts;    /* dict_count lazily-built decode dicts */

    ZSTD_DCtx       *dctx;
    uint8_t          dec_key[JCE_ARCHIVE_KEY_BYTES];
    JceArchiveSecureKeys secure_keys;
    const uint8_t   *auth_tag;
    JceArchiveAuthStatus auth_status;
    int              has_key;    /* 1 once a decryption key has been set */
};

/* ── Process-wide decryption key ─────────────────────────────────────────
 * Installed once at boot (before any PAK/bundle opens — precedent: the
 * g_pak_path_override boot-time module global in jce_engine.c) and
 * auto-applied to every archive whose header carries JARC_FLAG_ENCRYPTED.
 * Per-archive jce_archive_set_decryption_key() still works and overrides.
 * Not synchronized: set it during single-threaded startup. */
static uint8_t g_process_key[JCE_ARCHIVE_KEY_BYTES];
static int     g_has_process_key = 0;

/* Opt-in integrity gate (default off): see jce_archive_set_verify_on_open. */
static int     g_verify_on_open = 0;

void jce_archive_set_process_key(const uint8_t key[32]) {
    if (!key) {
        jce_archive_crypto_zero(g_process_key, sizeof(g_process_key));
        g_has_process_key = 0;
        return;
    }
    memcpy(g_process_key, key, JCE_ARCHIVE_KEY_BYTES);
    g_has_process_key = 1;
}

void jce_archive_set_verify_on_open(int enable) {
    g_verify_on_open = enable ? 1 : 0;
}

static int archive_apply_key(JceArchive *ar, const uint8_t key[32]) {
    if (!ar || !key) return 0;
    ar->has_key = 1;
    if (ar->flags & JARC_FLAG_AUTHENTICATED) {
        uint8_t tag[JCE_ARCHIVE_AUTH_BYTES];
        jce_archive_secure_keys_derive(key, ar->nonce_salt,
                                       &ar->secure_keys);
        jce_archive_hmac_sha256(ar->secure_keys.auth,
                                sizeof(ar->secure_keys.auth),
                                ar->blob, ar->blob_size, tag);
        ar->auth_status = jce_archive_crypto_equal(
            tag, ar->auth_tag, JCE_ARCHIVE_AUTH_BYTES)
            ? JCE_ARCHIVE_AUTH_VALID : JCE_ARCHIVE_AUTH_INVALID;
        jce_archive_crypto_zero(tag, sizeof(tag));
        return ar->auth_status == JCE_ARCHIVE_AUTH_VALID;
    }
    memcpy(ar->dec_key, key, JCE_ARCHIVE_KEY_BYTES);
    ar->auth_status = JCE_ARCHIVE_AUTH_UNAVAILABLE;
    return 1;
}

/* Parse the dictionary table (immediately after the header) into a resident
 * array, validating that every dictionary's bytes lie within the blob.
 * Returns 1 on success, 0 on a malformed table. */
static int decode_dict_table(JceArchive *ar) {
    if (ar->dict_count == 0) return 1;

    ar->dicts  = (JarcDictInfo *)jce_malloc(sizeof(JarcDictInfo) * ar->dict_count);
    ar->ddicts = (ZSTD_DDict **)jce_malloc(sizeof(ZSTD_DDict *) * ar->dict_count);
    if (!ar->dicts || !ar->ddicts) return 0;
    for (uint16_t i = 0; i < ar->dict_count; i++) ar->ddicts[i] = NULL;

    for (uint16_t i = 0; i < ar->dict_count; i++) {
        const uint8_t *d = ar->blob + JARC_HEADER_SIZE + (size_t)i * JARC_DICT_ENTRY_SIZE;
        JarcDictInfo *o = &ar->dicts[i];
        o->offset = jarc_rd64(d + JARC_DOFF_DATA_OFFSET);
        o->size   = jarc_rd32(d + JARC_DOFF_SIZE);
        o->tag    = jarc_rd32(d + JARC_DOFF_TAG);
        if (!jce_region_in_bounds(o->offset, o->size, ar->blob_size)) {
            LOG_WARN(JARC_TAG, "reject: dictionary %u out of bounds", (unsigned)i);
            return 0;
        }
    }
    return 1;
}

/* Decode `count` 32-byte index records starting at `rec` into `out`.
 * Returns 1 on success (records monotonic by hash and in-bounds), 0 on a
 * malformed index. */
static int decode_index(JceArchive *ar, const uint8_t *rec, uint32_t count) {
    uint64_t prev_hash = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = rec + (size_t)i * JARC_INDEX_ENTRY_SIZE;
        JceArchiveEntry *o = &ar->entries[i];
        o->path_hash     = jarc_rd64(e + JARC_EOFF_PATH_HASH);
        o->data_offset   = jarc_rd64(e + JARC_EOFF_DATA_OFFSET);
        o->stored_size   = jarc_rd32(e + JARC_EOFF_STORED_SIZE);
        o->original_size = jarc_rd32(e + JARC_EOFF_ORIG_SIZE);
        o->compression   = e[JARC_EOFF_COMPRESSION];
        o->entry_flags   = e[JARC_EOFF_ENTRY_FLAGS];
        o->dict_id       = jarc_rd16(e + JARC_EOFF_DICT_ID);
        o->content_crc   = jarc_rd32(e + JARC_EOFF_CONTENT_CRC);

        if (i > 0 && o->path_hash < prev_hash) {
            LOG_WARN(JARC_TAG, "reject: index not sorted by hash at %u", (unsigned)i);
            return 0;
        }
        prev_hash = o->path_hash;

        if (!jce_region_in_bounds(o->data_offset, o->stored_size, ar->blob_size)) {
            LOG_WARN(JARC_TAG, "reject: entry %u out of bounds", (unsigned)i);
            return 0;
        }
    }
    return 1;
}

/* Parse the optional debug path table that follows the index region.
 * Best-effort: on any malformation the table is dropped (debug-only data
 * that the runtime never depends on for correctness). */
static void parse_debug_paths(JceArchive *ar) {
    size_t off = (size_t)(ar->index_offset + ar->index_stored_size);
    if (!jce_region_in_bounds(off, 4, ar->blob_size)) return;

    uint32_t count = jarc_rd32(ar->blob + off);
    off += 4;
    if (count != ar->entry_count) return;

    char **paths = (char **)jce_malloc(sizeof(char *) * (count ? count : 1));
    if (!paths) return;
    for (uint32_t i = 0; i < count; i++) paths[i] = NULL;

    for (uint32_t i = 0; i < count; i++) {
        if (!jce_region_in_bounds(off, 2, ar->blob_size)) goto fail;
        uint16_t len = jarc_rd16(ar->blob + off);
        off += 2;
        if (!jce_region_in_bounds(off, len, ar->blob_size)) goto fail;
        char *s = (char *)jce_malloc((size_t)len + 1);
        if (!s) goto fail;
        memcpy(s, ar->blob + off, len);
        s[len] = '\0';
        paths[i] = s;
        off += len;
    }
    ar->debug_paths = paths;
    return;

fail:
    for (uint32_t i = 0; i < count; i++) jce_free(paths[i]);
    jce_free(paths);
}

static JceArchive *open_internal(const uint8_t *blob, size_t size, int owns) {
    const uint8_t *index_raw = NULL;
    uint8_t       *index_tmp = NULL;
    if (!blob || size < JARC_HEADER_SIZE) return NULL;

    if (blob[JARC_OFF_MAGIC + 0] != JARC_MAGIC_0 ||
        blob[JARC_OFF_MAGIC + 1] != JARC_MAGIC_1 ||
        blob[JARC_OFF_MAGIC + 2] != JARC_MAGIC_2 ||
        blob[JARC_OFF_MAGIC + 3] != JARC_MAGIC_3) {
        return NULL;
    }
    if (jarc_rd32(blob + JARC_OFF_FORMAT_VERSION) != JARC_FORMAT_VERSION) {
        return NULL;
    }

    uint32_t entry_count        = jarc_rd32(blob + JARC_OFF_ENTRY_COUNT);
    uint64_t index_offset       = jarc_rd64(blob + JARC_OFF_INDEX_OFFSET);
    uint64_t index_stored_size  = jarc_rd64(blob + JARC_OFF_INDEX_STORED_SIZE);
    uint64_t index_orig_size    = jarc_rd64(blob + JARC_OFF_INDEX_ORIGINAL_SIZE);
    uint32_t flags              = jarc_rd32(blob + JARC_OFF_FLAGS);
    uint16_t dict_count         = jarc_rd16(blob + JARC_OFF_DICT_COUNT);
    size_t logical_size         = size;

    if (flags & JARC_FLAG_AUTHENTICATED) {
        const uint32_t required = JARC_FLAG_ENCRYPTED |
                                  JARC_FLAG_SECURE_INDEX;
        if ((flags & required) != required ||
            (flags & JARC_FLAG_HAS_DEBUG_PATHS) ||
            size < JARC_HEADER_SIZE + JARC_AUTH_TAG_SIZE) {
            LOG_WARN(JARC_TAG, "reject: invalid secure archive flags");
            return NULL;
        }
        logical_size -= JARC_AUTH_TAG_SIZE;
    } else if (flags & JARC_FLAG_SECURE_INDEX) {
        LOG_WARN(JARC_TAG, "reject: unauthenticated keyed index");
        return NULL;
    }

    /* Region bounds. */
    uint64_t dict_table_end = (uint64_t)JARC_HEADER_SIZE +
                              (uint64_t)dict_count * JARC_DICT_ENTRY_SIZE;
    if (dict_table_end > logical_size) return NULL;
    if (index_offset < dict_table_end || index_offset > logical_size) return NULL;
    if (!jce_region_in_bounds(index_offset, index_stored_size,
                              logical_size)) return NULL;
    if (index_orig_size != (uint64_t)entry_count * JARC_INDEX_ENTRY_SIZE) return NULL;

    JceArchive *ar = (JceArchive *)jce_malloc(sizeof(JceArchive));
    if (!ar) return NULL;
    memset(ar, 0, sizeof(*ar));
    ar->blob               = blob;
    ar->blob_size          = logical_size;
    ar->owns_blob          = owns;
    ar->flags              = flags;
    ar->dict_count         = dict_count;
    ar->index_offset       = index_offset;
    ar->index_stored_size  = index_stored_size;
    ar->data_content_hash  = jarc_rd64(blob + JARC_OFF_DATA_CONTENT_HASH);
    ar->index_content_hash = jarc_rd64(blob + JARC_OFF_INDEX_CONTENT_HASH);
    ar->entry_count        = entry_count;
    ar->auth_status        = JCE_ARCHIVE_AUTH_UNAVAILABLE;
    if (flags & JARC_FLAG_AUTHENTICATED)
        ar->auth_tag = blob + logical_size;
    /* nonce_salt32 (ex-RESERVED, spec §4.2): meaningful iff ENCRYPTED. */
    ar->nonce_salt         = (flags & JARC_FLAG_ENCRYPTED)
                                 ? jarc_rd32(blob + JARC_OFF_NONCE_SALT32) : 0;

    /* Auto-apply the process-wide key to encrypted archives so PAKs and
     * bundle mounts opened after boot decrypt without per-call wiring. */
    if ((flags & JARC_FLAG_ENCRYPTED) && g_has_process_key) {
        if (!archive_apply_key(ar, g_process_key)) {
            LOG_WARN(JARC_TAG, "reject: archive authentication failed");
            goto fail;
        }
    }

    /* Materialize the (possibly compressed) index region. */
    if (!decode_dict_table(ar)) goto fail;

    if (flags & JARC_FLAG_INDEX_COMPRESSED) {
        if (index_orig_size > 0) {
            index_tmp = (uint8_t *)jce_malloc((size_t)index_orig_size);
            if (!index_tmp) goto fail;
            size_t got = ZSTD_decompress(index_tmp, (size_t)index_orig_size,
                                         blob + index_offset, (size_t)index_stored_size);
            if (ZSTD_isError(got) || got != index_orig_size) {
                LOG_WARN(JARC_TAG, "reject: index decompression failed");
                goto fail;
            }
        }
        index_raw = index_tmp;
    } else {
        if (index_stored_size != index_orig_size) goto fail;
        index_raw = blob + index_offset;
    }

    if (entry_count > 0) {
        ar->entries = (JceArchiveEntry *)jce_malloc(sizeof(JceArchiveEntry) * entry_count);
        if (!ar->entries) goto fail;
        if (!decode_index(ar, index_raw, entry_count)) goto fail;
        if (flags & JARC_FLAG_AUTHENTICATED) {
            for (uint32_t i = 0; i < entry_count; ++i) {
                const uint8_t required = JARC_ENTRY_ENCRYPTED |
                                         JARC_ENTRY_AUTHENTICATED;
                if ((ar->entries[i].entry_flags & required) != required) {
                    LOG_WARN(JARC_TAG,
                             "reject: secure archive contains plain entry");
                    goto fail;
                }
            }
        }
    }

    if (index_tmp) { jce_free(index_tmp); index_tmp = NULL; }

    if (flags & JARC_FLAG_HAS_DEBUG_PATHS) parse_debug_paths(ar);

    ar->dctx = ZSTD_createDCtx();
    if (!ar->dctx) goto fail;

    /* Opt-in integrity gate (default off): reject tampered/corrupted blobs
     * before any consumer reads them. */
    if (g_verify_on_open && !jce_archive_verify_header(ar)) {
        LOG_WARN(JARC_TAG, "reject: content-hash verification failed");
        ZSTD_freeDCtx(ar->dctx);
        ar->dctx = NULL;
        goto fail;
    }

    return ar;

fail:
    if (index_tmp) jce_free(index_tmp);
    if (ar) {
        if (ar->ddicts) {
            for (uint16_t i = 0; i < ar->dict_count; i++)
                if (ar->ddicts[i]) ZSTD_freeDDict(ar->ddicts[i]);
            jce_free(ar->ddicts);
        }
        jce_free(ar->dicts);
        jce_free(ar->entries);
        jce_archive_crypto_zero(ar->dec_key, sizeof(ar->dec_key));
        jce_archive_crypto_zero(&ar->secure_keys, sizeof(ar->secure_keys));
        jce_free(ar);
    }
    return NULL;
}

JceArchive *jce_archive_open(const void *data, size_t size) {
    return open_internal((const uint8_t *)data, size, 0);
}

JceArchive *jce_archive_open_owned(void *data, size_t size) {
    JceArchive *ar = open_internal((const uint8_t *)data, size, 1);
    if (!ar && data) jce_free(data); /* honor ownership even on failure */
    return ar;
}

JceArchive *jce_archive_open_file(const char *path) {
    JceMmap *m = jce_mmap_open(path);
    if (!m) return NULL;
    JceArchive *ar = open_internal((const uint8_t *)jce_mmap_data(m),
                                   jce_mmap_size(m), 0);
    if (!ar) { jce_mmap_close(m); return NULL; }
    ar->map = m; /* released by jce_archive_close */
    return ar;
}

void jce_archive_close(JceArchive *ar) {
    if (!ar) return;
    if (ar->debug_paths) {
        for (uint32_t i = 0; i < ar->entry_count; i++) jce_free(ar->debug_paths[i]);
        jce_free(ar->debug_paths);
    }
    if (ar->ddicts) {
        for (uint16_t i = 0; i < ar->dict_count; i++)
            if (ar->ddicts[i]) ZSTD_freeDDict(ar->ddicts[i]);
        jce_free(ar->ddicts);
    }
    jce_free(ar->dicts);
    if (ar->dctx) ZSTD_freeDCtx(ar->dctx);
    jce_free(ar->entries);
    jce_archive_crypto_zero(ar->dec_key, sizeof(ar->dec_key));
    jce_archive_crypto_zero(&ar->secure_keys, sizeof(ar->secure_keys));
    if (ar->map) jce_mmap_close(ar->map);
    else if (ar->owns_blob) jce_free((void *)ar->blob);
    jce_free(ar);
}

uint32_t jce_archive_count(const JceArchive *ar) {
    return ar ? ar->entry_count : 0;
}

const JceArchiveEntry *jce_archive_get(const JceArchive *ar, uint32_t index) {
    if (!ar || index >= ar->entry_count) return NULL;
    return &ar->entries[index];
}

uint64_t jce_archive_path_id(const JceArchive *ar, const char *path) {
    if (!ar || !path) return 0;
    if (!(ar->flags & JARC_FLAG_SECURE_INDEX))
        return jce_archive_hash_path(path);
    if (!ar->has_key || ar->auth_status != JCE_ARCHIVE_AUTH_VALID)
        return 0;

    size_t cap = strlen(path) + 1u;
    char *normalized = (char *)jce_malloc(cap);
    uint64_t id = 0;
    if (!normalized) return 0;
    size_t len = jce_archive_normalize_path(path, normalized, cap);
    if (len > 0) {
        id = jce_archive_secure_path_hash(&ar->secure_keys, normalized, len);
    }
    jce_free(normalized);
    return id;
}

const JceArchiveEntry *jce_archive_find(const JceArchive *ar, const char *path) {
    if (!ar || !path || ar->entry_count == 0) return NULL;

    uint64_t key = jce_archive_path_id(ar, path);
    if (key == 0) return NULL;

    uint32_t lo = 0, hi = ar->entry_count; /* [lo, hi) */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint64_t h = ar->entries[mid].path_hash;
        if (h == key) return &ar->entries[mid];
        if (h < key) lo = mid + 1;
        else         hi = mid;
    }
    return NULL;
}

size_t jce_archive_read(const JceArchive *ar, const JceArchiveEntry *entry,
                        void *buf, size_t buf_size) {
    if (!ar || !entry || !buf) return 0;
    if (buf_size < entry->original_size) return 0;
    if (!jce_region_in_bounds(entry->data_offset, entry->stored_size, ar->blob_size)) return 0;

    const uint8_t *src = ar->blob + entry->data_offset;
    uint8_t *decrypted = NULL;
    size_t stored_payload_size = entry->stored_size;

    if ((ar->flags & JARC_FLAG_AUTHENTICATED) &&
        ar->auth_status != JCE_ARCHIVE_AUTH_VALID) {
        LOG_WARN(JARC_TAG, "read: archive authentication is not valid");
        return 0;
    }

    /* Decrypt first (spec §9.2): encrypted bytes were produced by compressing
     * then encrypting, so we reverse that order — decrypt, then decompress. */
    if (entry->entry_flags & JARC_ENTRY_ENCRYPTED) {
        if (!ar->has_key) {
            LOG_WARN(JARC_TAG, "read: encrypted entry requires a decryption key");
            return 0;
        }
        if ((entry->entry_flags & JARC_ENTRY_AUTHENTICATED) != 0) {
            if (entry->stored_size < JCE_ARCHIVE_NONCE_BYTES) return 0;
            const uint8_t *nonce = src;
            src += JCE_ARCHIVE_NONCE_BYTES;
            stored_payload_size -= JCE_ARCHIVE_NONCE_BYTES;
            if (stored_payload_size == 0) {
                src = NULL;
            } else {
                decrypted = (uint8_t *)jce_malloc(stored_payload_size);
                if (!decrypted) return 0;
                jce_archive_chacha20_xor(
                    ar->secure_keys.enc, nonce, 1, src, decrypted,
                    stored_payload_size);
                src = decrypted;
            }
        } else if (entry->stored_size > 0) {
            decrypted = (uint8_t *)jce_malloc(entry->stored_size);
            if (!decrypted) return 0;
            uint8_t nonce[JCE_ARCHIVE_NONCE_BYTES];
            jce_archive_derive_nonce(entry->path_hash, ar->nonce_salt, nonce);
            jce_archive_chacha20_xor(ar->dec_key, nonce, 1, src,
                                     decrypted, entry->stored_size);
            src = decrypted;
        }
    }

    size_t result = 0;
    switch (entry->compression) {
    case JARC_COMP_NONE:
        if (stored_payload_size != entry->original_size) break;
        if (entry->original_size > 0) memcpy(buf, src, entry->original_size);
        result = entry->original_size;
        break;

    case JARC_COMP_ZSTD: {
        size_t got = ZSTD_decompressDCtx(ar->dctx, buf, buf_size,
                                         src, stored_payload_size);
        if (!ZSTD_isError(got) && got == entry->original_size) result = got;
        break;
    }

    case JARC_COMP_ZSTD_DICT: {
        uint16_t id = entry->dict_id;
        if (id >= ar->dict_count) {
            LOG_WARN(JARC_TAG, "read: entry references missing dictionary %u",
                     (unsigned)id);
            break;
        }
        /* Build the decode dictionary once, then reuse it. */
        if (!ar->ddicts[id]) {
            ar->ddicts[id] = ZSTD_createDDict(ar->blob + ar->dicts[id].offset,
                                              ar->dicts[id].size);
            if (!ar->ddicts[id]) break;
        }
        size_t got = ZSTD_decompress_usingDDict(ar->dctx, buf, buf_size,
                                                src, stored_payload_size,
                                                ar->ddicts[id]);
        if (!ZSTD_isError(got) && got == entry->original_size) result = got;
        break;
    }

    case JARC_COMP_LZ4:
    default:
        LOG_WARN(JARC_TAG, "read: compression id %u unsupported in this build",
                 (unsigned)entry->compression);
        break;
    }

    if (decrypted) jce_free(decrypted);
    return result;
}

void jce_archive_set_decryption_key(JceArchive *ar, const uint8_t key[32]) {
    if (!ar || !key) return;
    archive_apply_key(ar, key);
}

int jce_archive_is_secure(const JceArchive *ar) {
    return ar && (ar->flags & JARC_FLAG_SECURE_INDEX) != 0;
}

int jce_archive_is_authenticated(const JceArchive *ar) {
    return ar && (ar->flags & JARC_FLAG_AUTHENTICATED) != 0;
}

JceArchiveAuthStatus jce_archive_auth_status(const JceArchive *ar) {
    return ar ? ar->auth_status : JCE_ARCHIVE_AUTH_INVALID;
}

int jce_archive_map_entry(const JceArchive *ar, const JceArchiveEntry *entry,
                          const void **ptr, size_t *size) {
    if (!ar || !entry || !ptr || !size) return 0;
    /* Only uncompressed, unencrypted bytes are directly usable in place. */
    if (entry->compression != JARC_COMP_NONE) return 0;
    if (entry->entry_flags & JARC_ENTRY_ENCRYPTED) return 0;
    if (!jce_region_in_bounds(entry->data_offset, entry->original_size, ar->blob_size)) return 0;
    *ptr  = ar->blob + entry->data_offset;
    *size = entry->original_size;
    return 1;
}

uint16_t jce_archive_dict_count(const JceArchive *ar) {
    return ar ? ar->dict_count : 0;
}

uint32_t jce_archive_dict_tag(const JceArchive *ar, uint16_t dict_id) {
    if (!ar || dict_id >= ar->dict_count) return 0;
    return ar->dicts[dict_id].tag;
}

int jce_archive_verify_header(const JceArchive *ar) {
    if (!ar) return 0;

    uint64_t data_start = (uint64_t)JARC_HEADER_SIZE +
                          (uint64_t)ar->dict_count * JARC_DICT_ENTRY_SIZE;
    if (data_start > ar->index_offset) return 0;
    size_t data_len = (size_t)(ar->index_offset - data_start);

    uint64_t dh = (uint64_t)XXH3_64bits(ar->blob + data_start, data_len);
    if (dh != ar->data_content_hash) return 0;

    uint64_t ih = (uint64_t)XXH3_64bits(ar->blob + ar->index_offset,
                                        (size_t)ar->index_stored_size);
    if (ih != ar->index_content_hash) return 0;

    return 1;
}

int jce_archive_verify_entry(const JceArchiveEntry *entry,
                             const void *buf, size_t size) {
    if (!entry || !buf) return 0;
    uint32_t crc = (uint32_t)XXH32(buf, size, 0);
    return crc == entry->content_crc;
}

const char *jce_archive_debug_path(const JceArchive *ar, uint32_t index) {
    if (!ar || !ar->debug_paths || index >= ar->entry_count) return NULL;
    return ar->debug_paths[index];
}
