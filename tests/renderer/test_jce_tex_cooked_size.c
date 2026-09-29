/*
 * test_jce_tex_cooked_size.c  Cooked-texture pixel-chunk sizing (L3).
 *
 * Regression guard for the cooked-texture OOB read (audit 2026-06-13,
 * R2-texture-trust-wh): the loader trusted the file's width/height/format/
 * mip_count and uploaded/decoded that implied size out of a pixel chunk that
 * may be shorter, reading past the buffer (CPU decode) and past the bgfx
 * memory blob (GPU upload).  jce_tex_cooked_pixel_size reports the exact
 * bytes the chain implies so the loader can reject a short chunk.
 */

#include "unity.h"

#include "resource/jce_tex_compress.h"
#include <jce/resource/jce_asset_format.h>

#include <stdint.h>

void setUp(void)    {}
void tearDown(void) {}

static void test_rgba8_single_mip(void)
{
    /* 4x4 RGBA8 = 4*4*4 = 64 bytes. */
    TEST_ASSERT_EQUAL_UINT32(64u,
        jce_tex_cooked_pixel_size(4, 4, JCEASSET_TEXFMT_RGBA8, 1));
}

static void test_rgba8_full_chain(void)
{
    /* 4x4 -> 2x2 -> 1x1 = 64 + 16 + 4 = 84 bytes (3 mips). */
    TEST_ASSERT_EQUAL_UINT32(84u,
        jce_tex_cooked_pixel_size(4, 4, JCEASSET_TEXFMT_RGBA8, 3));
}

static void test_bc1_single_mip(void)
{
    /* 4x4 BC1 = one 4x4 block = 8 bytes. */
    TEST_ASSERT_EQUAL_UINT32(8u,
        jce_tex_cooked_pixel_size(4, 4, JCEASSET_TEXFMT_BC1, 1));
}

static void test_rejects_impossible_mip_count(void)
{
    /* A file claiming more mips than 4x4 can hold (3) is malformed -> 0. */
    TEST_ASSERT_EQUAL_UINT32(0u,
        jce_tex_cooked_pixel_size(4, 4, JCEASSET_TEXFMT_RGBA8, 99));
}

static void test_rejects_zero_dims_and_unknown_format(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u,
        jce_tex_cooked_pixel_size(0, 4, JCEASSET_TEXFMT_RGBA8, 1));
    TEST_ASSERT_EQUAL_UINT32(0u,
        jce_tex_cooked_pixel_size(4, 4, 9999 /*unknown fmt*/, 1));
}

static void test_numeric_format_sizes(void)
{
    TEST_ASSERT_EQUAL_UINT32(8u,
        jce_tex_cooked_pixel_size(2, 2, JCEASSET_TEXFMT_R16F, 1));
    TEST_ASSERT_EQUAL_UINT32(16u,
        jce_tex_cooked_pixel_size(2, 2, JCEASSET_TEXFMT_RG16F, 1));
    TEST_ASSERT_EQUAL_UINT32(32u,
        jce_tex_cooked_pixel_size(2, 2, JCEASSET_TEXFMT_RGBA16F, 1));
    TEST_ASSERT_EQUAL_UINT32(16u,
        jce_tex_cooked_pixel_size(2, 2, JCEASSET_TEXFMT_R32F, 1));
    TEST_ASSERT_EQUAL_UINT32(32u,
        jce_tex_cooked_pixel_size(2, 2, JCEASSET_TEXFMT_RG32F, 1));
    TEST_ASSERT_EQUAL_UINT32(64u,
        jce_tex_cooked_pixel_size(2, 2, JCEASSET_TEXFMT_RGBA32F, 1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rgba8_single_mip);
    RUN_TEST(test_rgba8_full_chain);
    RUN_TEST(test_bc1_single_mip);
    RUN_TEST(test_rejects_impossible_mip_count);
    RUN_TEST(test_rejects_zero_dims_and_unknown_format);
    RUN_TEST(test_numeric_format_sizes);
    return UNITY_END();
}
