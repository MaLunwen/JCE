#include "unity.h"

#include <jce/api_core.h>
#include <jce/resource/jce_asset_format.h>
#include <jce/resource/jce_numeric_texture.h>

#include "resource/jce_asset_reader.h"

#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static JceNumericTextureBlob make_rg32f(void)
{
    static const float values[16] = {
        0.0f, 1.0f, 2.0f, 3.0f,
        4.0f, 5.0f, 6.0f, 7.0f,
        8.0f, 9.0f, 10.0f, 11.0f,
        12.0f, 13.0f, 14.0f, 15.0f
    };
    JceNumericTextureDesc desc = jce_numeric_texture_desc_default();
    JceNumericTextureBlob blob = {0};
    desc.width = 4;
    desc.height = 2;
    desc.format = JCEASSET_TEXFMT_RG32F;
    desc.row_pitch = 4u * 2u * sizeof(float);
    desc.compression_level = 0;
    TEST_ASSERT_EQUAL_INT(JCE_NUMERIC_TEXTURE_OK,
        jce_numeric_texture_encode(&desc, values, sizeof(values), &blob));
    return blob;
}

static void test_rg32f_round_trip_and_metadata(void)
{
    JceNumericTextureBlob blob = make_rg32f();
    JceNumericTextureDesc decoded;
    TEST_ASSERT_NOT_NULL(blob.data);
    TEST_ASSERT_EQUAL_INT(JCE_NUMERIC_TEXTURE_OK,
        jce_numeric_texture_validate(blob.data, blob.size, &decoded));
    TEST_ASSERT_EQUAL_UINT32(4u, decoded.width);
    TEST_ASSERT_EQUAL_UINT32(2u, decoded.height);
    TEST_ASSERT_EQUAL_UINT32(JCEASSET_TEXFMT_RG32F, decoded.format);
    TEST_ASSERT_EQUAL_UINT32(32u, decoded.row_pitch);

    JceAssetView view;
    TEST_ASSERT_TRUE(jce_asset_open(&view, blob.data, blob.size));
    const JceAssetChunkEntry *pixels =
        jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_PIXELS);
    TEST_ASSERT_NOT_NULL(pixels);
    TEST_ASSERT_EQUAL_UINT64(64u, pixels->original_size);
    uint8_t raw[64];
    TEST_ASSERT_EQUAL_UINT32(sizeof(raw),
        jce_asset_chunk_data(&view, pixels, raw, sizeof(raw)));
    TEST_ASSERT_EQUAL_HEX8(0x00, raw[4]);
    TEST_ASSERT_EQUAL_HEX8(0x00, raw[5]);
    TEST_ASSERT_EQUAL_HEX8(0x80, raw[6]);
    TEST_ASSERT_EQUAL_HEX8(0x3f, raw[7]);
    jce_numeric_texture_blob_free(&blob);
}

static void test_rejects_bad_size_row_pitch_format_and_nonfinite(void)
{
    float values[16] = {0};
    JceNumericTextureDesc desc = jce_numeric_texture_desc_default();
    JceNumericTextureBlob blob = {0};
    desc.width = 4;
    desc.height = 2;
    desc.format = JCEASSET_TEXFMT_RG32F;
    desc.struct_size = 0u;
    desc.row_pitch = 32;
    TEST_ASSERT_EQUAL_INT(JCE_NUMERIC_TEXTURE_INVALID_ARGUMENT,
        jce_numeric_texture_encode(&desc, values, sizeof(values), &blob));
    desc.struct_size = sizeof(desc);
    desc.row_pitch = 31;
    TEST_ASSERT_EQUAL_INT(JCE_NUMERIC_TEXTURE_ROW_PITCH_MISMATCH,
        jce_numeric_texture_encode(&desc, values, sizeof(values), &blob));
    desc.row_pitch = 32;
    TEST_ASSERT_EQUAL_INT(JCE_NUMERIC_TEXTURE_INVALID_ARGUMENT,
        jce_numeric_texture_encode(&desc, values, sizeof(values) - 1u, &blob));
    desc.format = 9999;
    TEST_ASSERT_EQUAL_INT(JCE_NUMERIC_TEXTURE_UNSUPPORTED_FORMAT,
        jce_numeric_texture_encode(&desc, values, sizeof(values), &blob));
    desc.format = JCEASSET_TEXFMT_RG32F;
    {
        const uint32_t quiet_nan = 0x7fc00000u;
        memcpy(&values[3], &quiet_nan, sizeof(quiet_nan));
    }
    TEST_ASSERT_EQUAL_INT(JCE_NUMERIC_TEXTURE_NONFINITE_VALUE,
        jce_numeric_texture_encode(&desc, values, sizeof(values), &blob));
}

static void test_validator_enforces_nonfinite_policy(void)
{
    float values[16] = {0};
    const uint32_t quiet_nan = 0x7fc00000u;
    JceNumericTextureDesc desc = jce_numeric_texture_desc_default();
    JceNumericTextureBlob blob = {0};
    desc.width = 4;
    desc.height = 2;
    desc.format = JCEASSET_TEXFMT_RG32F;
    desc.row_pitch = 32;
    desc.flags = JCEASSET_NUMERIC_FLAG_ALLOW_NONFINITE;
    memcpy(&values[3], &quiet_nan, sizeof(quiet_nan));
    TEST_ASSERT_EQUAL_INT(JCE_NUMERIC_TEXTURE_OK,
        jce_numeric_texture_encode(&desc, values, sizeof(values), &blob));
    TEST_ASSERT_EQUAL_INT(JCE_NUMERIC_TEXTURE_OK,
        jce_numeric_texture_validate(blob.data, blob.size, NULL));

    JceAssetView view;
    TEST_ASSERT_TRUE(jce_asset_open(&view, blob.data, blob.size));
    const JceAssetChunkEntry *metadata =
        jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_NUMERIC_INFO);
    TEST_ASSERT_NOT_NULL(metadata);
    JceAssetNumericInfo *info = (JceAssetNumericInfo *)
        ((uint8_t *)blob.data + metadata->data_offset);
    TEST_ASSERT_EQUAL_UINT16(JCEASSET_COMPRESS_NONE, metadata->compression);
    info->flags = 0u;
    TEST_ASSERT_EQUAL_INT(JCE_NUMERIC_TEXTURE_NONFINITE_VALUE,
        jce_numeric_texture_validate(blob.data, blob.size, NULL));
    jce_numeric_texture_blob_free(&blob);
}

static void test_rejects_truncation_and_hash_corruption(void)
{
    JceNumericTextureBlob blob = make_rg32f();
    TEST_ASSERT_EQUAL_INT(JCE_NUMERIC_TEXTURE_MALFORMED_ASSET,
        jce_numeric_texture_validate(blob.data, 40u, NULL));

    JceAssetView view;
    TEST_ASSERT_TRUE(jce_asset_open(&view, blob.data, blob.size));
    const JceAssetChunkEntry *pixels =
        jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_PIXELS);
    TEST_ASSERT_NOT_NULL(pixels);
    ((uint8_t *)blob.data)[pixels->data_offset] ^= 0x40u;
    TEST_ASSERT_EQUAL_INT(JCE_NUMERIC_TEXTURE_HASH_MISMATCH,
        jce_numeric_texture_validate(blob.data, blob.size, NULL));
    jce_numeric_texture_blob_free(&blob);
}

static void test_compressed_payload_validates(void)
{
    float values[8u * 8u * 2u] = {0};
    JceNumericTextureDesc desc = jce_numeric_texture_desc_default();
    JceNumericTextureBlob blob = {0};
    desc.width = 8;
    desc.height = 8;
    desc.format = JCEASSET_TEXFMT_RG32F;
    desc.row_pitch = 8u * 2u * sizeof(float);
    desc.compression_level = 3;
    TEST_ASSERT_EQUAL_INT(JCE_NUMERIC_TEXTURE_OK,
        jce_numeric_texture_encode(&desc, values, sizeof(values), &blob));
    JceAssetView view;
    TEST_ASSERT_TRUE(jce_asset_open(&view, blob.data, blob.size));
    const JceAssetChunkEntry *pixels =
        jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_PIXELS);
    TEST_ASSERT_NOT_NULL(pixels);
    TEST_ASSERT_EQUAL_UINT16(JCEASSET_COMPRESS_ZSTD, pixels->compression);
    TEST_ASSERT_EQUAL_INT(JCE_NUMERIC_TEXTURE_OK,
        jce_numeric_texture_validate(blob.data, blob.size, NULL));
    jce_numeric_texture_blob_free(&blob);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rg32f_round_trip_and_metadata);
    RUN_TEST(test_rejects_bad_size_row_pitch_format_and_nonfinite);
    RUN_TEST(test_validator_enforces_nonfinite_policy);
    RUN_TEST(test_rejects_truncation_and_hash_corruption);
    RUN_TEST(test_compressed_payload_validates);
    return UNITY_END();
}
