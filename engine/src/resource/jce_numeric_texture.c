/* Typed floating-point texture encoder and structural validator. */

#include <jce/resource/jce_numeric_texture.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/resource/jce_asset_format.h>

#include "jce_asset_reader.h"
#include "jce_asset_writer_internal.h"

#include <math.h>
#include <string.h>
#include <xxhash.h>

static bool numeric_layout(uint32_t format, uint32_t *channels,
                           uint32_t *bytes_per_channel)
{
    switch (format) {
    case JCEASSET_TEXFMT_R16F:     *channels = 1u; *bytes_per_channel = 2u; return true;
    case JCEASSET_TEXFMT_RG16F:    *channels = 2u; *bytes_per_channel = 2u; return true;
    case JCEASSET_TEXFMT_RGBA16F:  *channels = 4u; *bytes_per_channel = 2u; return true;
    case JCEASSET_TEXFMT_R32F:     *channels = 1u; *bytes_per_channel = 4u; return true;
    case JCEASSET_TEXFMT_RG32F:    *channels = 2u; *bytes_per_channel = 4u; return true;
    case JCEASSET_TEXFMT_RGBA32F:  *channels = 4u; *bytes_per_channel = 4u; return true;
    default: return false;
    }
}

static bool host_is_little_endian(void)
{
    const uint16_t one = 1u;
    return *(const uint8_t *)&one == 1u;
}

static bool finite_payload(const void *pixels, size_t bytes,
                           uint32_t bytes_per_channel)
{
    if (bytes_per_channel == 2u) {
        const uint8_t *p = pixels;
        for (size_t i = 0; i < bytes; i += 2u) {
            uint16_t bits = (uint16_t)p[i] | (uint16_t)((uint16_t)p[i + 1u] << 8u);
            if ((bits & 0x7c00u) == 0x7c00u) return false;
        }
        return true;
    }
    const uint8_t *p = pixels;
    for (size_t i = 0; i < bytes; i += 4u) {
        uint32_t bits = (uint32_t)p[i] |
            ((uint32_t)p[i + 1u] << 8u) |
            ((uint32_t)p[i + 2u] << 16u) |
            ((uint32_t)p[i + 3u] << 24u);
        float value;
        memcpy(&value, &bits, sizeof(value));
        if (!isfinite(value)) return false;
    }
    return true;
}

JceNumericTextureDesc jce_numeric_texture_desc_default(void)
{
    JceNumericTextureDesc desc = {0};
    desc.struct_size = (uint32_t)sizeof(desc);
    desc.format = JCEASSET_TEXFMT_R32F;
    desc.sampler_address = 0u;
    desc.sampler_filter = 1u;
    desc.color_space = 0u;
    desc.compression_level = 3;
    return desc;
}

JceNumericTextureStatus jce_numeric_texture_encode(
    const JceNumericTextureDesc *desc, const void *pixels,
    size_t pixel_bytes, JceNumericTextureBlob *out_blob)
{
    uint32_t channels;
    uint32_t channel_bytes;
    size_t tight_row;
    size_t required;
    uint8_t *canonical = NULL;

    if (out_blob) memset(out_blob, 0, sizeof(*out_blob));
    if (!desc || !pixels || !out_blob ||
        desc->struct_size != (uint32_t)sizeof(*desc) ||
        desc->width == 0u ||
        desc->height == 0u)
        return JCE_NUMERIC_TEXTURE_INVALID_ARGUMENT;
    if (!numeric_layout(desc->format, &channels, &channel_bytes))
        return JCE_NUMERIC_TEXTURE_UNSUPPORTED_FORMAT;
    if ((size_t)desc->width > SIZE_MAX / channels / channel_bytes)
        return JCE_NUMERIC_TEXTURE_SIZE_OVERFLOW;
    tight_row = (size_t)desc->width * channels * channel_bytes;
    if (tight_row > UINT32_MAX)
        return JCE_NUMERIC_TEXTURE_SIZE_OVERFLOW;
    if (desc->row_pitch != (uint32_t)tight_row)
        return JCE_NUMERIC_TEXTURE_ROW_PITCH_MISMATCH;
    if ((size_t)desc->height > SIZE_MAX / tight_row)
        return JCE_NUMERIC_TEXTURE_SIZE_OVERFLOW;
    required = (size_t)desc->height * tight_row;
    if (pixel_bytes != required)
        return JCE_NUMERIC_TEXTURE_INVALID_ARGUMENT;

    if (!host_is_little_endian()) {
        canonical = jce_malloc(required);
        if (!canonical) return JCE_NUMERIC_TEXTURE_OUT_OF_MEMORY;
        for (size_t i = 0; i < required; i += channel_bytes)
            for (uint32_t b = 0; b < channel_bytes; ++b)
                canonical[i + b] = ((const uint8_t *)pixels)[i + channel_bytes - 1u - b];
        pixels = canonical;
    }
    if (!(desc->flags & JCEASSET_NUMERIC_FLAG_ALLOW_NONFINITE) &&
        !finite_payload(pixels, required, channel_bytes)) {
        jce_free(canonical);
        return JCE_NUMERIC_TEXTURE_NONFINITE_VALUE;
    }

    JceAssetTexInfo texture_info = {0};
    texture_info.width = desc->width;
    texture_info.height = desc->height;
    texture_info.format = desc->format;
    texture_info.mip_count = 1u;

    JceAssetNumericInfo numeric_info = {0};
    numeric_info.struct_size = (uint32_t)sizeof(numeric_info);
    numeric_info.row_pitch = desc->row_pitch;
    numeric_info.channel_count = channels;
    numeric_info.sampler_address = desc->sampler_address;
    numeric_info.sampler_filter = desc->sampler_filter;
    numeric_info.color_space = desc->color_space;
    numeric_info.flags = desc->flags;
    numeric_info.decoded_hash = (uint64_t)XXH3_64bits(pixels, required);

    const JceAssetWriteChunk chunks[3] = {
        { JCEASSET_CHUNK_TEX_INFO, &texture_info, sizeof(texture_info) },
        { JCEASSET_CHUNK_TEX_NUMERIC_INFO, &numeric_info, sizeof(numeric_info) },
        { JCEASSET_CHUNK_TEX_PIXELS, pixels, required }
    };
    JceAssetWriteResult written = jce_asset_writer_build(
        JCEASSET_TYPE_TEXTURE, numeric_info.decoded_hash,
        chunks, 3u, desc->compression_level);
    jce_free(canonical);
    if (!written.success) return JCE_NUMERIC_TEXTURE_OUT_OF_MEMORY;
    out_blob->data = written.data;
    out_blob->size = written.size;
    return JCE_NUMERIC_TEXTURE_OK;
}

JceNumericTextureStatus jce_numeric_texture_validate(
    const void *asset, size_t asset_size, JceNumericTextureDesc *out_desc)
{
    JceAssetView view;
    if (!jce_asset_open(&view, asset, asset_size) ||
        view.header->asset_type != JCEASSET_TYPE_TEXTURE)
        return JCE_NUMERIC_TEXTURE_MALFORMED_ASSET;
    const JceAssetChunkEntry *tc = jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_INFO);
    const JceAssetChunkEntry *nc = jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_NUMERIC_INFO);
    const JceAssetChunkEntry *pc = jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_PIXELS);
    if (!tc || !nc || !pc || tc->original_size != sizeof(JceAssetTexInfo) ||
        nc->original_size != sizeof(JceAssetNumericInfo) ||
        pc->original_size > SIZE_MAX)
        return JCE_NUMERIC_TEXTURE_MALFORMED_ASSET;

    JceAssetTexInfo texture_info;
    JceAssetNumericInfo numeric_info;
    if (jce_asset_chunk_data(&view, tc, &texture_info, sizeof(texture_info)) != sizeof(texture_info) ||
        jce_asset_chunk_data(&view, nc, &numeric_info, sizeof(numeric_info)) != sizeof(numeric_info) ||
        numeric_info.struct_size != sizeof(numeric_info) || texture_info.mip_count != 1u)
        return JCE_NUMERIC_TEXTURE_MALFORMED_ASSET;

    uint32_t channels;
    uint32_t channel_bytes;
    if (!numeric_layout(texture_info.format, &channels, &channel_bytes))
        return JCE_NUMERIC_TEXTURE_UNSUPPORTED_FORMAT;
    if ((size_t)texture_info.width > SIZE_MAX / channels / channel_bytes)
        return JCE_NUMERIC_TEXTURE_SIZE_OVERFLOW;
    size_t tight_row = (size_t)texture_info.width * channels * channel_bytes;
    if (texture_info.width == 0u || texture_info.height == 0u ||
        tight_row > UINT32_MAX || numeric_info.row_pitch != tight_row ||
        numeric_info.channel_count != channels ||
        (size_t)texture_info.height > SIZE_MAX / tight_row ||
        pc->original_size != (uint64_t)((size_t)texture_info.height * tight_row))
        return JCE_NUMERIC_TEXTURE_MALFORMED_ASSET;

    size_t bytes = (size_t)pc->original_size;
    void *payload = jce_malloc(bytes);
    if (!payload) return JCE_NUMERIC_TEXTURE_OUT_OF_MEMORY;
    size_t copied = jce_asset_chunk_data(&view, pc, payload, bytes);
    if (copied != bytes) {
        jce_free(payload);
        return JCE_NUMERIC_TEXTURE_MALFORMED_ASSET;
    }
    uint64_t actual_hash = (uint64_t)XXH3_64bits(payload, bytes);
    if (actual_hash != numeric_info.decoded_hash) {
        jce_free(payload);
        return JCE_NUMERIC_TEXTURE_HASH_MISMATCH;
    }
    if (!(numeric_info.flags & JCEASSET_NUMERIC_FLAG_ALLOW_NONFINITE) &&
        !finite_payload(payload, bytes, channel_bytes)) {
        jce_free(payload);
        return JCE_NUMERIC_TEXTURE_NONFINITE_VALUE;
    }
    jce_free(payload);

    if (out_desc) {
        *out_desc = jce_numeric_texture_desc_default();
        out_desc->width = texture_info.width;
        out_desc->height = texture_info.height;
        out_desc->format = texture_info.format;
        out_desc->row_pitch = numeric_info.row_pitch;
        out_desc->sampler_address = numeric_info.sampler_address;
        out_desc->sampler_filter = numeric_info.sampler_filter;
        out_desc->color_space = numeric_info.color_space;
        out_desc->flags = numeric_info.flags;
    }
    return JCE_NUMERIC_TEXTURE_OK;
}

void jce_numeric_texture_blob_free(JceNumericTextureBlob *blob)
{
    if (!blob) return;
    jce_free(blob->data);
    blob->data = NULL;
    blob->size = 0u;
}
