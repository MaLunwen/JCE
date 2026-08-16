/* Deterministic .jceasset v1 writer shared by focused asset encoders. */

#include "jce_asset_writer_internal.h"

#include "os/core/jce_memory.h"

#include <string.h>
#include <zstd.h>

typedef struct JceAssetCompressedChunk {
    void *data;
    size_t size;
    bool compressed;
} JceAssetCompressedChunk;

static bool add_size(size_t a, size_t b, size_t *out)
{
    if (a > SIZE_MAX - b) return false;
    *out = a + b;
    return true;
}

static void set_error(JceAssetWriteResult *result, const char *message)
{
    size_t length = strlen(message);
    if (length >= sizeof(result->error))
        length = sizeof(result->error) - 1u;
    memcpy(result->error, message, length);
    result->error[length] = '\0';
}

JceAssetWriteResult jce_asset_writer_build(
    uint32_t asset_type,
    uint64_t source_hash,
    const JceAssetWriteChunk *chunks,
    uint32_t chunk_count,
    int compression_level)
{
    JceAssetWriteResult result = {0};
    JceAssetCompressedChunk *stored = NULL;
    size_t table_size;
    size_t total_size;
    size_t data_offset;

    if (!chunks || chunk_count == 0u ||
        (size_t)chunk_count > SIZE_MAX / JCEASSET_CHUNK_ENTRY_SIZE) {
        set_error(&result, "invalid chunk table");
        return result;
    }
    table_size = (size_t)chunk_count * JCEASSET_CHUNK_ENTRY_SIZE;
    if (!add_size(JCEASSET_HEADER_SIZE, table_size, &total_size)) {
        set_error(&result, "asset size overflow");
        return result;
    }

    stored = JCE_CALLOC(chunk_count, sizeof(*stored));
    if (!stored) {
        set_error(&result, "allocation failed");
        return result;
    }

    ZSTD_CCtx *cctx = compression_level > 0 ? ZSTD_createCCtx() : NULL;
    for (uint32_t i = 0; i < chunk_count; ++i) {
        size_t write_size = chunks[i].raw_size;
        if (!chunks[i].raw_data || write_size == 0u) {
            set_error(&result, "empty chunk");
            goto cleanup;
        }
        if (cctx && write_size > 64u) {
            size_t bound = ZSTD_compressBound(write_size);
            stored[i].data = JCE_MALLOC(bound);
            if (stored[i].data) {
                size_t compressed = ZSTD_compressCCtx(
                    cctx, stored[i].data, bound,
                    chunks[i].raw_data, write_size, compression_level);
                if (!ZSTD_isError(compressed) && compressed < write_size) {
                    stored[i].size = compressed;
                    stored[i].compressed = true;
                    write_size = compressed;
                } else {
                    JCE_FREE(stored[i].data);
                    stored[i].data = NULL;
                }
            }
        }
        if (!add_size(total_size, write_size, &total_size)) {
            set_error(&result, "asset size overflow");
            goto cleanup;
        }
    }
    ZSTD_freeCCtx(cctx);
    cctx = NULL;

    result.data = JCE_CALLOC(1u, total_size);
    if (!result.data) {
        set_error(&result, "allocation failed");
        goto cleanup;
    }
    result.size = total_size;

    JceAssetFileHeader header = {0};
    header.magic[0] = JCEASSET_MAGIC_0;
    header.magic[1] = JCEASSET_MAGIC_1;
    header.magic[2] = JCEASSET_MAGIC_2;
    header.magic[3] = JCEASSET_MAGIC_3;
    header.version = JCEASSET_VERSION;
    header.asset_type = asset_type;
    header.chunk_count = chunk_count;
    header.source_hash = source_hash;
    memcpy(result.data, &header, sizeof(header));

    data_offset = JCEASSET_HEADER_SIZE + table_size;
    for (uint32_t i = 0; i < chunk_count; ++i) {
        JceAssetChunkEntry entry = {0};
        const void *source = chunks[i].raw_data;
        size_t write_size = chunks[i].raw_size;
        if (stored[i].compressed) {
            source = stored[i].data;
            write_size = stored[i].size;
            entry.compression = JCEASSET_COMPRESS_ZSTD;
        }
        entry.chunk_type = chunks[i].chunk_type;
        entry.data_offset = (uint64_t)data_offset;
        entry.compressed_size = (uint64_t)write_size;
        entry.original_size = (uint64_t)chunks[i].raw_size;
        memcpy((uint8_t *)result.data + JCEASSET_HEADER_SIZE +
                   (size_t)i * JCEASSET_CHUNK_ENTRY_SIZE,
               &entry, sizeof(entry));
        memcpy((uint8_t *)result.data + data_offset, source, write_size);
        data_offset += write_size;
    }
    result.success = true;

cleanup:
    ZSTD_freeCCtx(cctx);
    if (stored) {
        for (uint32_t i = 0; i < chunk_count; ++i)
            JCE_FREE(stored[i].data);
        JCE_FREE(stored);
    }
    if (!result.success && result.data) {
        JCE_FREE(result.data);
        result.data = NULL;
        result.size = 0u;
    }
    return result;
}
