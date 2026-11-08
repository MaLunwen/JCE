/* Actual SDL_image CPU pixels and Bundle cooking; no renderer/GPU startup. */
#include "unity.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/renderer/jce_image.h>
#include <jce/renderer/jce_texture.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_bundle_pack.h>
#include "resource/jce_asset_reader.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 2x2, bottom-origin, 24-bit TGA. Stored bottom blue/white, top red/green.
 * There is deliberately no TGA footer or signature and no PNG substitution. */
static const uint8_t k_tga24[] = {
    0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 24, 0,
    255, 0, 0, 255, 255, 255, 0, 0, 255, 0, 255, 0
};
static const uint8_t k_rgba24[] = {
    255, 0, 0, 255, 0, 255, 0, 255,
    0, 0, 255, 255, 255, 255, 255, 255
};
static const uint8_t k_tga32[] = {
    0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 32, 0x28,
    0, 0, 255, 40, 0, 255, 0, 80, 255, 0, 0, 120, 255, 255, 255, 255
};
static const uint8_t k_rgba32[] = {
    255, 0, 0, 40, 0, 255, 0, 80,
    0, 0, 255, 120, 255, 255, 255, 255
};
static const uint8_t k_tga_rle[] = {
    0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 24, 0x20,
    1, 0, 0, 255, 0, 255, 0, 0x81, 255, 0, 0
};
static const uint8_t k_rgba_rle[] = {
    255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255
};
static const uint8_t k_tga_gray[] = {
    0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 1, 0, 8, 0x20, 20, 220
};
static const uint8_t k_tga_indexed[] = {
    0, 1, 1, 0, 0, 4, 0, 24, 0, 0, 0, 0, 2, 0, 2, 0, 8, 0x20,
    0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 0, 1, 2, 3
};
/* A repeat packet claims five pixels for a four-pixel image. SDL's direct
 * decoder can discard the excess repeat count; admission must reject it. */
static const uint8_t k_tga_overrun[] = {
    0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 24, 0x20,
    0x84, 0, 0, 255
};
static const uint8_t k_red_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
    0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41,
    0x54, 0x08, 0xd7, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x72, 0x9c,
    0x52, 0x67, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
    0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
};

static char g_root[1024];
static uint8_t *g_pixels;
static void *g_encoded;
static JceArchive *g_archive;
static unsigned g_warnings;
static unsigned g_errors;

void setUp(void)
{
    g_pixels = NULL;
    g_encoded = NULL;
    g_archive = NULL;
    g_root[0] = '\0';
    g_warnings = g_errors = 0;
}

void tearDown(void)
{
    jce_image_free_rgba8(g_pixels);
    g_pixels = NULL;
    jce_free(g_encoded);
    g_encoded = NULL;
    if (g_archive)
        jce_archive_close(g_archive);
    g_archive = NULL;
    if (g_root[0])
        (void)jce_fs_host_remove_recursive(g_root);
}

static void expect_pixels(const uint8_t *encoded, size_t size, int expected_w,
                          int expected_h, const uint8_t *rgba)
{
    int w = -11, h = -22;
    g_pixels = jce_image_load_rgba8_from_memory(encoded, size, &w, &h);
    TEST_ASSERT_NOT_NULL(g_pixels);
    TEST_ASSERT_EQUAL_INT(expected_w, w);
    TEST_ASSERT_EQUAL_INT(expected_h, h);
    TEST_ASSERT_EQUAL_MEMORY(rgba, g_pixels, (size_t)w * h * 4u);
    jce_image_free_rgba8(g_pixels);
    g_pixels = NULL;
}

static void expect_rejected(const void *encoded, uint64_t size)
{
    int w = -11, h = -22;
    g_pixels = jce_image_load_rgba8_from_memory(encoded, size, &w, &h);
    TEST_ASSERT_NULL(g_pixels);
    TEST_ASSERT_EQUAL_INT(-11, w);
    TEST_ASSERT_EQUAL_INT(-22, h);
}

static void test_tga24_signatureless_bottom_origin_pixels(void)
{
    expect_pixels(k_tga24, sizeof(k_tga24), 2, 2, k_rgba24);
    uint8_t with_id[sizeof(k_tga24) + 2u];
    memcpy(with_id, k_tga24, 18u);
    with_id[0] = 2;
    with_id[18] = 'J';
    with_id[19] = 'C';
    memcpy(with_id + 20u, k_tga24 + 18u, sizeof(k_tga24) - 18u);
    expect_pixels(with_id, sizeof(with_id), 2, 2, k_rgba24);
}

static void test_tga32_top_origin_preserves_authored_alpha(void)
{
    expect_pixels(k_tga32, sizeof(k_tga32), 2, 2, k_rgba32);
}

static void test_tga_rle_raw_repeat_and_scanline_crossing(void)
{
    expect_pixels(k_tga_rle, sizeof(k_tga_rle), 2, 2, k_rgba_rle);
    uint8_t across[22] = {0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 24, 0x20,
                          0x83, 0, 0, 255};
    static const uint8_t red4[16] = {
        255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255
    };
    expect_pixels(across, sizeof(across), 2, 2, red4);
}

static void test_tga_gray_and_indexed_palette_pixels(void)
{
    static const uint8_t gray[8] = {20, 20, 20, 255, 220, 220, 220, 255};
    expect_pixels(k_tga_gray, sizeof(k_tga_gray), 2, 1, gray);
    expect_pixels(k_tga_indexed, sizeof(k_tga_indexed), 2, 2, k_rgba24);
    static const uint8_t opaque32[] = {
        0, 1, 1, 0, 0, 4, 0, 32, 0, 0, 0, 0, 2, 0, 2, 0, 8, 0x20,
        0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255,
        0, 1, 2, 3
    };
    expect_pixels(opaque32, sizeof(opaque32), 2, 2, k_rgba24);
}

static void test_signature_png_keeps_auto_decode_pixels(void)
{
    static const uint8_t red[4] = {255, 0, 0, 255};
    expect_pixels(k_red_png, sizeof(k_red_png), 1, 1, red);
}

static void test_truncated_header_id_palette_raw_and_rle_fail(void)
{
    for (size_t size = 0; size < sizeof(k_tga24); ++size)
        expect_rejected(k_tga24, size);
    for (size_t size = 18; size < sizeof(k_tga_rle); ++size)
        expect_rejected(k_tga_rle, size);
    expect_rejected(k_tga_indexed, sizeof(k_tga_indexed) - 1u);
    uint8_t missing_id[sizeof(k_tga24)];
    memcpy(missing_id, k_tga24, sizeof(missing_id));
    missing_id[0] = 255;
    expect_rejected(missing_id, sizeof(missing_id));
}

static void test_false_headers_dimensions_palette_and_rle_overrun_fail(void)
{
    const unsigned offsets[] = {1, 2, 3, 12, 16, 17, 17, 17};
    const uint8_t values[] = {2, 4, 1, 0, 0, 0x10, 0x40, 8};
    for (size_t i = 0; i < sizeof(values); ++i) {
        uint8_t bad[sizeof(k_tga24)];
        memcpy(bad, k_tga24, sizeof(bad));
        bad[offsets[i]] = values[i];
        expect_rejected(bad, sizeof(bad));
    }
    uint8_t huge[sizeof(k_tga24)];
    memcpy(huge, k_tga24, sizeof(huge));
    huge[12] = huge[13] = huge[14] = huge[15] = 255;
    expect_rejected(huge, sizeof(huge));
    uint8_t bad_palette[sizeof(k_tga_indexed)];
    memcpy(bad_palette, k_tga_indexed, sizeof(bad_palette));
    bad_palette[sizeof(bad_palette) - 1u] = 4;
    expect_rejected(bad_palette, sizeof(bad_palette));
    bad_palette[sizeof(bad_palette) - 1u] = 3;
    bad_palette[3] = 1;
    expect_rejected(bad_palette, sizeof(bad_palette));
    expect_rejected(k_tga_overrun, sizeof(k_tga_overrun));
    static const uint8_t false_header[32] = {0};
    expect_rejected(false_header, sizeof(false_header));
}

static void test_unsupported_attribute_and_palette_alpha_fail_closed(void)
{
    static const uint8_t alpha16[] = {
        0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 16, 0x21, 0, 0
    };
    static const uint8_t palette_alpha[] = {
        0, 1, 1, 0, 0, 1, 0, 32, 0, 0, 0, 0, 1, 0, 1, 0, 8, 0x20,
        0, 0, 255, 40, 0
    };
    expect_rejected(alpha16, sizeof(alpha16));
    expect_rejected(palette_alpha, sizeof(palette_alpha));
}

static void test_null_empty_and_oversized_inputs_leave_outputs_unchanged(void)
{
    expect_rejected(NULL, sizeof(k_tga24));
    expect_rejected(k_tga24, 0);
    expect_rejected(k_tga24, (uint64_t)INT_MAX + 1u);
}

static void pack_log(JceBundlePackLogLevel level, const char *message, void *user)
{
    (void)message;
    (void)user;
    if (level == JCE_BUNDLE_PACK_LOG_WARNING)
        ++g_warnings;
    if (level == JCE_BUNDLE_PACK_LOG_ERROR)
        ++g_errors;
}

static void pack_tga(const uint8_t *tga, size_t size, bool valid)
{
    static unsigned serial;
    char base[768], texture_dir[1024], texture[1024], scene[1024];
    char out[1024], bundle[1024];
    TEST_ASSERT_TRUE(jce_fs_host_get_current_dir(base, sizeof(base)));
    int count = snprintf(g_root, sizeof(g_root), "%s/_ut_image_tga_%u", base, ++serial);
    TEST_ASSERT_TRUE(count > 0 && (size_t)count < sizeof(g_root));
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(g_root));
    (void)snprintf(texture_dir, sizeof(texture_dir), "%s/textures", g_root);
    (void)snprintf(texture, sizeof(texture), "%s/textures/selected.tga", g_root);
    (void)snprintf(scene, sizeof(scene), "%s/selected_tga.scene", g_root);
    (void)snprintf(out, sizeof(out), "%s/out", g_root);
    (void)snprintf(bundle, sizeof(bundle), "%s/out/selected_tga.jbundle", g_root);
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(texture_dir));
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(out));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(texture, tga, size));
    const char *scene_json = "{\"entities\":[{\"texturePath\":\"textures/selected.tga\"}]}";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(scene, scene_json, strlen(scene_json)));
    const char *scenes[1] = {scene};
    JceBundlePackOptions opts = {0};
    opts.scenes_dir = g_root;
    opts.resource_root = g_root;
    opts.out_dir = out;
    opts.scene_files = scenes;
    opts.scene_file_count = 1;
    opts.single_file_mode = true;
    opts.cook_assets = true;
    opts.target_platform = 0; /* Windows; 2x2 BC is rejected to exact RGBA8 by existing policy. */
    opts.zstd_level = 1;
    TEST_ASSERT_EQUAL_INT(0, jce_bundle_pack_run(&opts, pack_log, NULL));
    TEST_ASSERT_EQUAL_UINT(0, g_errors);
    TEST_ASSERT_EQUAL_UINT(valid ? 0u : 1u, g_warnings);
    g_archive = jce_archive_open_file(bundle);
    TEST_ASSERT_NOT_NULL(g_archive);
    const JceArchiveEntry *entry = jce_archive_find(g_archive, "textures/selected.tga");
    TEST_ASSERT_NOT_NULL(entry);
    g_encoded = jce_malloc(entry->original_size);
    TEST_ASSERT_NOT_NULL(g_encoded);
    TEST_ASSERT_EQUAL_UINT(entry->original_size,
                          jce_archive_read(g_archive, entry, g_encoded, entry->original_size));
    JceAssetView view;
    if (!valid) {
        /* Existing explicit raw-on-cook-failure policy remains observable. */
        TEST_ASSERT_FALSE(jce_asset_is_cooked(g_encoded, entry->original_size));
        TEST_ASSERT_EQUAL_UINT(size, entry->original_size);
        TEST_ASSERT_EQUAL_MEMORY(tga, g_encoded, size);
        uint32_t w = 0, h = 0;
        TEST_ASSERT_FALSE(jce_texture_decode_cooked_rgba8(g_encoded, entry->original_size,
                                                        &g_pixels, &w, &h));
        return;
    }
    TEST_ASSERT_TRUE(jce_asset_open(&view, g_encoded, entry->original_size));
    TEST_ASSERT_EQUAL_UINT(JCEASSET_TYPE_TEXTURE, view.header->asset_type);
    const JceAssetChunkEntry *info_chunk = jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_INFO);
    TEST_ASSERT_NOT_NULL(info_chunk);
    /* Read the complete production INFO chunk, including appended mip
     * offsets, so compressed and uncompressed chunk forms are both exercised. */
    uint8_t info_bytes[sizeof(JceAssetTexInfo) + 32u * sizeof(uint32_t)];
    TEST_ASSERT_TRUE(info_chunk->original_size >= sizeof(JceAssetTexInfo) &&
                     info_chunk->original_size <= sizeof(info_bytes));
    TEST_ASSERT_EQUAL_UINT(info_chunk->original_size,
                          jce_asset_chunk_data(&view, info_chunk, info_bytes, sizeof(info_bytes)));
    JceAssetTexInfo info = {0};
    memcpy(&info, info_bytes, sizeof(info));
    TEST_ASSERT_EQUAL_UINT(2, info.width);
    TEST_ASSERT_EQUAL_UINT(2, info.height);
    TEST_ASSERT_EQUAL_UINT(JCEASSET_TEXFMT_RGBA8, info.format);
    uint32_t w = 0, h = 0;
    TEST_ASSERT_TRUE(jce_texture_decode_cooked_rgba8(g_encoded, entry->original_size, &g_pixels, &w, &h));
    TEST_ASSERT_EQUAL_UINT(2, w);
    TEST_ASSERT_EQUAL_UINT(2, h);
    TEST_ASSERT_EQUAL_MEMORY(k_rgba24, g_pixels, sizeof(k_rgba24));
}

static void test_real_tga_bundle_is_cooked_rgba8_without_raw_warning(void)
{
    pack_tga(k_tga24, sizeof(k_tga24), true);
}

static void test_malformed_tga_bundle_cook_failure_is_not_cooked_success(void)
{
    pack_tga(k_tga_overrun, sizeof(k_tga_overrun), false);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tga24_signatureless_bottom_origin_pixels);
    RUN_TEST(test_tga32_top_origin_preserves_authored_alpha);
    RUN_TEST(test_tga_rle_raw_repeat_and_scanline_crossing);
    RUN_TEST(test_tga_gray_and_indexed_palette_pixels);
    RUN_TEST(test_signature_png_keeps_auto_decode_pixels);
    RUN_TEST(test_truncated_header_id_palette_raw_and_rle_fail);
    RUN_TEST(test_false_headers_dimensions_palette_and_rle_overrun_fail);
    RUN_TEST(test_unsupported_attribute_and_palette_alpha_fail_closed);
    RUN_TEST(test_null_empty_and_oversized_inputs_leave_outputs_unchanged);
    RUN_TEST(test_real_tga_bundle_is_cooked_rgba8_without_raw_warning);
    RUN_TEST(test_malformed_tga_bundle_cook_failure_is_not_cooked_success);
    return UNITY_END();
}
