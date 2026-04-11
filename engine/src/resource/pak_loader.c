/* pak_loader.c
 *
 * Runtime PAK archive loader.
 * Parses an in-memory JPAK blob, builds a lookup table sorted by
 * XXH3_64bits hash, and decompresses assets on demand via ZSTD.
 */

#include <jce/core/pak_loader.h>
#include "resource/pak_format.h"
#include <jce/core/jce_profiler.h>

#include <stdio.h>
#include "core/jce_memory.h"
#include <string.h>

#include <xxhash.h>
#include <zstd.h>

/* ================================================================== */
/* Internal types                                                      */
/* ================================================================== */

struct PakArchive {
    const uint8_t *blob;        /* start of the in-memory PAK         */
    size_t         blob_size;
    uint32_t       count;       /* number of entries                   */
    PakAsset      *assets;      /* heap-allocated array, sorted by hash*/
    uint64_t      *hashes;      /* parallel array of path hashes       */
    char         **paths;       /* heap-allocated NUL-terminated copies*/
    int            owns_blob;   /* 1 => blob was malloc'd; free on close*/
    ZSTD_DCtx     *dctx;        /* reusable decompression context      */
};

/* ================================================================== */
/* pak_open                                                            */
/* ================================================================== */

PakArchive *pak_open(const void *data, size_t size) {
    if (!data || size < JPAK_HEADER_SIZE) return NULL;

    const uint8_t *blob = (const uint8_t *)data;

    /* Validate magic (single bytes  endian-neutral). */
    if (blob[0] != JPAK_MAGIC_0 ||
        blob[1] != JPAK_MAGIC_1 ||
        blob[2] != JPAK_MAGIC_2 ||
        blob[3] != JPAK_MAGIC_3) {
        return NULL;
    }

    /* Read header fields via LE helpers  safe on any byte order. */
    uint32_t version  = jpak_read_le32(blob + 4);
    if (version != JPAK_VERSION) return NULL;

    uint32_t count    = jpak_read_le32(blob + 8);
    /* skip flags (blob+12) */
    uint64_t toc_off  = jpak_read_le64(blob + 16);
    uint64_t data_off = jpak_read_le64(blob + 24);

    /* Bounds check the TOC region. */
    uint64_t toc_end = toc_off + (uint64_t)count * JPAK_TOC_ENTRY_SIZE;
    if (toc_end > size || data_off > size) return NULL;

    PakArchive *pak = (PakArchive *)JCE_CALLOC(1, sizeof(PakArchive));
    if (!pak) return NULL;

    pak->blob      = blob;
    pak->blob_size = size;
    pak->count     = count;

    pak->dctx = ZSTD_createDCtx();

    if (count == 0) return pak;

    pak->assets = (PakAsset *)JCE_CALLOC(count, sizeof(PakAsset));
    pak->hashes = (uint64_t *)JCE_CALLOC(count, sizeof(uint64_t));
    pak->paths  = (char **)JCE_CALLOC(count, sizeof(char *));
    if (!pak->assets || !pak->hashes || !pak->paths) {
        pak_close(pak);
        return NULL;
    }

    for (uint32_t i = 0; i < count; ++i) {
        /* Pointer to the i-th TOC entry (each 40 bytes). */
        const uint8_t *e = blob + toc_off + (uint64_t)i * JPAK_TOC_ENTRY_SIZE;

        uint64_t path_hash       = jpak_read_le64(e + 0);
        uint32_t name_offset     = jpak_read_le32(e + 8);
        uint32_t name_length     = jpak_read_le32(e + 12);
        uint64_t entry_data_off  = jpak_read_le64(e + 16);
        uint64_t compressed_size = jpak_read_le64(e + 24);
        uint64_t original_size   = jpak_read_le64(e + 32);

        /* Bounds-check name region. */
        if ((uint64_t)name_offset + name_length > size) {
            pak_close(pak);
            return NULL;
        }

        /* Build a NUL-terminated path copy. */
        char *path_copy = (char *)JCE_MALLOC(name_length + 1);
        if (!path_copy) { pak_close(pak); return NULL; }
        memcpy(path_copy, blob + name_offset, name_length);
        path_copy[name_length] = '\0';

        pak->paths[i]  = path_copy;
        pak->hashes[i] = path_hash;

        pak->assets[i].path            = path_copy;
        pak->assets[i].compressed_data = blob + data_off + entry_data_off;
        pak->assets[i].compressed_size = compressed_size;
        pak->assets[i].original_size   = original_size;

        /* Bounds-check data region. */
        if (data_off + entry_data_off + compressed_size > size) {
            pak_close(pak);
            return NULL;
        }
    }

    return pak;
}

/* ================================================================== */
/* pak_close                                                           */
/* ================================================================== */

void pak_close(PakArchive *pak) {
    if (!pak) return;
    if (pak->paths) {
        for (uint32_t i = 0; i < pak->count; ++i)
            JCE_FREE(pak->paths[i]);
        JCE_FREE(pak->paths);
    }
    JCE_FREE(pak->hashes);
    JCE_FREE(pak->assets);
    ZSTD_freeDCtx(pak->dctx);
    if (pak->owns_blob) JCE_FREE((void *)pak->blob);
    JCE_FREE(pak);
}

/* ================================================================== */
/* pak_open_owned                                                      */
/* ================================================================== */

PakArchive *pak_open_owned(void *data, size_t size) {
    PakArchive *pak = pak_open(data, size);
    if (pak) pak->owns_blob = 1;
    return pak;
}

/* ================================================================== */
/* pak_open_file                                                       */
/* ================================================================== */

PakArchive *pak_open_file(const char *path) {
    if (!path) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long lsize = ftell(f);
    if (lsize <= 0) { fclose(f); return NULL; }
    rewind(f);

    uint8_t *buf = (uint8_t *)JCE_MALLOC((size_t)lsize);
    if (!buf) { fclose(f); return NULL; }

    if (fread(buf, 1, (size_t)lsize, f) != (size_t)lsize) {
        JCE_FREE(buf); fclose(f); return NULL;
    }
    fclose(f);

    PakArchive *pak = pak_open(buf, (size_t)lsize);
    if (pak) {
        pak->owns_blob = 1;
    } else {
        JCE_FREE(buf);
    }
    return pak;
}

/* ================================================================== */
/* pak_find    XXH3 hash + binary search                              */
/* ================================================================== */

const PakAsset *pak_find(const PakArchive *pak, const char *path) {
    if (!pak || !path || pak->count == 0) return NULL;

    uint64_t hash = XXH3_64bits(path, strlen(path));

    /* Binary search on the hash-sorted array. */
    uint32_t lo = 0, hi = pak->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (pak->hashes[mid] < hash)
            lo = mid + 1;
        else
            hi = mid;
    }

    /* There may be hash collisions  scan forward while hash matches. */
    for (uint32_t i = lo; i < pak->count && pak->hashes[i] == hash; ++i) {
        if (strcmp(pak->assets[i].path, path) == 0)
            return &pak->assets[i];
    }

    return NULL;
}

/* ================================================================== */
/* pak_decompress                                                      */
/* ================================================================== */

size_t pak_decompress(const PakAsset *asset, void *buf, size_t buf_size) {
    if (!asset || !buf || buf_size < asset->original_size)
        return 0;

    size_t result = ZSTD_decompress(
        buf, buf_size,
        asset->compressed_data, (size_t)asset->compressed_size);

    if (ZSTD_isError(result)) return 0;
    return result;
}

size_t pak_decompress_ex(const PakArchive *pak, const PakAsset *asset,
                         void *buf, size_t buf_size) {
    if (!asset || !buf || buf_size < asset->original_size)
        return 0;

    /* Use the archive's reusable DCtx when available. */
    size_t result;
    if (pak && pak->dctx) {
        result = ZSTD_decompressDCtx(
            pak->dctx, buf, buf_size,
            asset->compressed_data, (size_t)asset->compressed_size);
    } else {
        result = ZSTD_decompress(
            buf, buf_size,
            asset->compressed_data, (size_t)asset->compressed_size);
    }

    if (ZSTD_isError(result)) return 0;
    return result;
}

/* ================================================================== */
/* pak_count / pak_get                                                 */
/* ================================================================== */

uint32_t pak_count(const PakArchive *pak) {
    return pak ? pak->count : 0;
}

const PakAsset *pak_get(const PakArchive *pak, uint32_t index) {
    if (!pak || index >= pak->count) return NULL;
    return &pak->assets[index];
}
