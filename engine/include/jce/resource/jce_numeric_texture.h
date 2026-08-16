/*
 * jce_numeric_texture.h  Typed floating-point texture asset encoder.
 *
 * The API emits ordinary .jceasset v1 texture containers. Runtime loading
 * therefore follows the same PAK, integrity, decompression, and GPU upload
 * path as every other cooked texture.
 */

#ifndef JCE_NUMERIC_TEXTURE_H
#define JCE_NUMERIC_TEXTURE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum JceNumericTextureStatus {
    JCE_NUMERIC_TEXTURE_OK = 0,
    JCE_NUMERIC_TEXTURE_INVALID_ARGUMENT,
    JCE_NUMERIC_TEXTURE_UNSUPPORTED_FORMAT,
    JCE_NUMERIC_TEXTURE_SIZE_OVERFLOW,
    JCE_NUMERIC_TEXTURE_ROW_PITCH_MISMATCH,
    JCE_NUMERIC_TEXTURE_NONFINITE_VALUE,
    JCE_NUMERIC_TEXTURE_OUT_OF_MEMORY,
    JCE_NUMERIC_TEXTURE_MALFORMED_ASSET,
    JCE_NUMERIC_TEXTURE_HASH_MISMATCH
} JceNumericTextureStatus;

typedef struct JceNumericTextureDesc {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t row_pitch;
    uint32_t sampler_address;
    uint32_t sampler_filter;
    uint32_t color_space;
    uint32_t flags;
    int32_t compression_level;
} JceNumericTextureDesc;

typedef struct JceNumericTextureBlob {
    void *data;
    size_t size;
} JceNumericTextureBlob;

JCE_API JceNumericTextureDesc jce_numeric_texture_desc_default(void);

JCE_API JceNumericTextureStatus jce_numeric_texture_encode(
    const JceNumericTextureDesc *desc,
    const void *pixels,
    size_t pixel_bytes,
    JceNumericTextureBlob *out_blob);

JCE_API JceNumericTextureStatus jce_numeric_texture_validate(
    const void *asset,
    size_t asset_size,
    JceNumericTextureDesc *out_desc);

JCE_API void jce_numeric_texture_blob_free(JceNumericTextureBlob *blob);

JCE_EXTERN_C_END

#endif /* JCE_NUMERIC_TEXTURE_H */
