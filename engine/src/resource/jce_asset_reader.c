/*
 * jce_asset_reader.c  Runtime .jceasset parser.
 *
 * Zero-copy reader: all pointers reference the caller's buffer.
 * Only dependency beyond standard C is ZSTD for chunk decompression.
 */

#include "jce_asset_reader.h"
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

	/* Bounds-check chunk table. */
	uint32_t n = hdr->chunk_count;
	size_t toc_end = JCEASSET_HEADER_SIZE + (size_t)n * JCEASSET_CHUNK_ENTRY_SIZE;
	if (toc_end > size) return false;

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

	/* Bounds-check the chunk data region. */
	if (chunk->data_offset + chunk->compressed_size > view->blob_size)
		return 0;

	const uint8_t *src = view->blob + chunk->data_offset;

	if (chunk->compression == JCEASSET_COMPRESS_ZSTD) {
		/* Decompress requires a buffer at least as large as original_size. */
		if (out_size < chunk->original_size) return 0;
		size_t result = ZSTD_decompress(out_buf, out_size,
		                                src, (size_t)chunk->compressed_size);
		if (ZSTD_isError(result)) return 0;
		return result;
	}

	/* Uncompressed — copy up to out_size bytes. */
	size_t to_copy = chunk->original_size;
	if (to_copy > out_size) to_copy = out_size;
	memcpy(out_buf, src, to_copy);
	return to_copy;
}
