/* Internal deterministic .jceasset v1 writer shared by build-time encoders. */

#ifndef JCE_ASSET_WRITER_INTERNAL_H
#define JCE_ASSET_WRITER_INTERNAL_H

#include <jce/resource/jce_asset_format.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct JceAssetWriteChunk {
    uint16_t chunk_type;
    const void *raw_data;
    size_t raw_size;
} JceAssetWriteChunk;

typedef struct JceAssetWriteResult {
    void *data;
    size_t size;
    bool success;
    char error[256];
} JceAssetWriteResult;

JceAssetWriteResult jce_asset_writer_build(
    uint32_t asset_type,
    uint64_t source_hash,
    const JceAssetWriteChunk *chunks,
    uint32_t chunk_count,
    int compression_level);

#endif /* JCE_ASSET_WRITER_INTERNAL_H */
