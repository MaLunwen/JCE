/*
 * jce_asset_reader.c  Runtime .jceasset parser.
 *
 * Zero-copy reader: all pointers reference the caller's buffer.
 * Only dependency beyond standard C is ZSTD for chunk decompression.
 */

#include "jce_asset_reader.h"
#include "jce_read_bounds.h"

#include <string.h>
#include <zstd.h>

/* ================================================================== */
/* Magic check                                                         */
/* ================================================================== */

bool jce_asset_is_cooked(const void *data, size_t size)
{
    if (!data || size < 4) return false;
    const uint8_t *p = (const uint8_t *)data;
    return p[0] == JCEASSET_MAGIC_0
        && p[1] == JCEASSET_MAGIC_1
        && p[2] == JCEASSET_MAGIC_2
        && p[3] == JCEASSET_MAGIC_3;
}

/* ================================================================== */
/* Open / parse                                                        */
/* ================================================================== */

bool jce_asset_open(JceAssetView *view, const void *data, size_t size)
{
    if (!view || !data) return false;
    memset(view, 0, sizeof(*view));

    if (size < JCEASSET_HEADER_SIZE) return false;

    const uint8_t *blob = (const uint8_t *)data;

    /* Validate magic. */
    if (!jce_asset_is_cooked(data, size)) return false;

    const JceAssetFileHeader *hdr = (const JceAssetFileHeader *)blob;

    /* Version check. */
    if (hdr->version != JCEASSET_VERSION) return false;

    /* Bounds-check chunk table (overflow-safe: n * ENTRY can wrap size_t on
     * 32-bit targets — never multiply against a file-controlled count). */
    uint32_t n = hdr->chunk_count;
    if (!jce_count_fits(n, JCEASSET_CHUNK_ENTRY_SIZE,
                        (uint64_t)size - JCEASSET_HEADER_SIZE))
        return false;

    view->header    = hdr;
    view->chunks    = (const JceAssetChunkEntry *)(blob + JCEASSET_HEADER_SIZE);
    view->blob      = blob;
    view->blob_size = size;
    return true;
}

/* ================================================================== */
/* Find chunk                                                          */
/* ================================================================== */

const JceAssetChunkEntry *jce_asset_find_chunk(const JceAssetView *view,
                                                uint16_t chunk_type)
{
    if (!view || !view->header) return NULL;

    uint32_t n = view->header->chunk_count;
    for (uint32_t i = 0; i < n; i++) {
        if (view->chunks[i].chunk_type == chunk_type)
            return &view->chunks[i];
    }
    return NULL;
}

/* ================================================================== */
/* Read chunk data                                                     */
/* ================================================================== */

size_t jce_asset_chunk_data(const JceAssetView *view,
                            const JceAssetChunkEntry *chunk,
                            void *out_buf, size_t out_size)
{
    if (!view || !chunk || !out_buf || out_size == 0) return 0;

    /* Bounds-check the chunk data region (overflow-safe: data_offset is
     * file-controlled and data_offset + compressed_size can wrap uint64). */
    if (!jce_region_in_bounds(chunk->data_offset, chunk->compressed_size,
                              view->blob_size))
        return 0;

    const uint8_t *src = view->blob + chunk->data_offset;

    if (chunk->compression == JCEASSET_COMPRESS_ZSTD) {
        /* Decompress requires a buffer at least as large as original_size. */
        if (out_size < chunk->original_size) return 0;
        size_t result = ZSTD_decompress(out_buf, out_size,
                                        src, (size_t)chunk->compressed_size);
        if (ZSTD_isError(result) || result != chunk->original_size) return 0;
        return result;
    }

    /* Fixed-size prefix reads are part of the v1 contract: TEX_INFO may append
     * mip offsets after JceAssetTexInfo while older callers only request the
     * fixed header.  Preserve that behavior, but reject unknown compression
     * tags and never read beyond the stored region. */
    if (chunk->compression != JCEASSET_COMPRESS_NONE)
        return 0;
    size_t to_copy = (size_t)chunk->original_size;
    if (to_copy > out_size) to_copy = out_size;
    if (to_copy > chunk->compressed_size) to_copy = (size_t)chunk->compressed_size;
    memcpy(out_buf, src, to_copy);
    return to_copy;
}
