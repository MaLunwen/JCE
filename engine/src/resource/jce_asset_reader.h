/*
 * jce_asset_reader.h  Runtime .jceasset parser (zero-copy).
 *
 * Reads cooked asset containers produced by jce_cook / jce_asset_cooker.
 * Designed for the hot path: no allocations, pointer arithmetic only.
 *
 * Usage:
 *   if (jce_asset_is_cooked(buf, len)) {
 *       JceAssetView v;
 *       if (jce_asset_open(&v, buf, len)) {
 *           const JceAssetChunkEntry *info = jce_asset_find_chunk(&v, JCEASSET_CHUNK_TEX_INFO);
 *           JceAssetTexInfo tex_info;
 *           jce_asset_chunk_data(&v, info, &tex_info, sizeof(tex_info));
 *       }
 *   }
 */

#ifndef JCE_ASSET_READER_H
#define JCE_ASSET_READER_H

#include <jce/resource/jce_asset_format.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Asset view (zero-copy, no allocation)                               */
/* ================================================================== */

/*
 * A parsed view into a .jceasset blob in memory.
 * All pointers reference the original buffer — caller must keep it alive.
 */
typedef struct JceAssetView {
    const JceAssetFileHeader *header;       /* points into blob */
    const JceAssetChunkEntry *chunks;       /* chunk table array */
    const uint8_t            *blob;         /* start of blob */
    size_t                    blob_size;    /* total blob size */
} JceAssetView;

/* ================================================================== */
/* API                                                                 */
/* ================================================================== */

/*
 * Check if a memory buffer begins with the "JCEA" magic.
 * Fast check — only reads 4 bytes.
 */
bool jce_asset_is_cooked(const void *data, size_t size);

/*
 * Parse a .jceasset blob into a zero-copy view.
 * Returns true on success. Validates magic, version, and bounds.
 */
bool jce_asset_open(JceAssetView *view, const void *data, size_t size);

/*
 * Find a chunk by type in the view's chunk table.
 * Returns NULL if not found.
 */
const JceAssetChunkEntry *jce_asset_find_chunk(const JceAssetView *view,
                                                uint16_t chunk_type);

/*
 * Read chunk data into a caller-provided buffer.
 * Handles ZSTD decompression transparently.
 *
 * out_buf:  destination buffer (must be >= chunk->original_size)
 * out_size: size of out_buf in bytes
 *
 * Returns the number of bytes written, or 0 on failure.
 */
size_t jce_asset_chunk_data(const JceAssetView *view,
                            const JceAssetChunkEntry *chunk,
                            void *out_buf, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* JCE_ASSET_READER_H */
