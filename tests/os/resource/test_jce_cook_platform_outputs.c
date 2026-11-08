/* Actual CLI, shared cooker and public Bundler outputs. No GPU/window/bootstrap.
 * Legacy producers are complete same-source owners with symbol names isolated
 * and only the former Web policy/cache revisions restored by CMake. */
#include "unity.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_json.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_bundle_pack.h>
#include <jce/resource/jce_pak_loader.h>
#include "resource/jce_asset_reader.h"
#include "jce_cook_catalog.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int jce_tool_main(int argc, char **argv);
int jce_legacy_cook_tool_main(int argc, char **argv);
int jce_legacy_bundle_pack_run(const JceBundlePackOptions *opts,
                              JceBundlePackLogFn log_fn, void *user);
#ifndef JCE_COOK_PLATFORM_FIXTURE_NAMESPACE
#error Every registered cook fixture needs its distinct target namespace
#endif
static char root[1024], owned_prefix[1024], resources[1100], textures[1150], scene[1200];
static bool root_created;
static bool root_is_owned(void)
{
    size_t prefix_size = strlen(owned_prefix);
    return prefix_size > 0 && strncmp(root, owned_prefix, prefix_size) == 0 &&
        root[prefix_size] != '\0' && strchr(root + prefix_size, '/') == NULL &&
        strchr(root + prefix_size, '\\') == NULL;
}
static char cli_text[16384];
static size_t cli_used;
static bool cli_truncated, bundle_reused;
static unsigned bundle_errors;
/* Counted stdout port: actual complete CLI formats the observed lines. No
 * cache decision, format choice or option dispatch is reimplemented here. */
int jce_test_cook_platform_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int count = vsnprintf(cli_text + cli_used, sizeof(cli_text) - cli_used, format, args);
    va_end(args);
    if (count < 0) return count;
    if ((size_t)count >= sizeof(cli_text) - cli_used) {
        cli_used = sizeof(cli_text) - 1; cli_truncated = true;
    } else cli_used += (size_t)count;
    return count;
}
static void write_text(const char *path, const char *text)
{ TEST_ASSERT_TRUE(jce_fs_host_write_all(path, text, strlen(text))); }
void setUp(void)
{
    static unsigned counter;
    char cwd[1024], base[1024], desired[1100];
    root_created = false; root[0] = owned_prefix[0] = '\0';
    TEST_ASSERT_TRUE(jce_fs_host_get_current_dir(cwd, sizeof(cwd)));
    TEST_ASSERT_TRUE(jce_path_normalize(base, sizeof(base), cwd));
    int length = snprintf(owned_prefix, sizeof(owned_prefix), "%s/_ut_cook_%s_",
                           base, JCE_COOK_PLATFORM_FIXTURE_NAMESPACE);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof(owned_prefix));
    snprintf(desired, sizeof(desired), "%s%u", owned_prefix, ++counter);
    /* The CTest target namespace prevents cooperating parallel executables
     * sharing a desired path. unique_path is discovery, not atomic ownership. */
    TEST_ASSERT_TRUE(jce_fs_host_make_unique_path(desired, root, sizeof(root)));
    TEST_ASSERT_TRUE(root_is_owned());
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(root));
    TEST_ASSERT_FALSE(jce_fs_host_exists_dir(root));
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(root));
    root_created = true;
    snprintf(resources, sizeof(resources), "%s/resources", root);
    snprintf(textures, sizeof(textures), "%s/textures", resources);
    char scenes[1150]; snprintf(scenes, sizeof(scenes), "%s/scenes", resources);
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(textures));
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(scenes));
    snprintf(scene, sizeof(scene), "%s/main.scene", scenes);
    write_text(scene, "{\"entities\":[{\"texturePath\":\"textures/colour.tga\"}]}");
    /* Real uncompressed top-origin BGRA TGA, 4x4, with a translucent pixel.
     * Neither the encoded header nor the cooked artifact is a policy table. */
    uint8_t tga[18 + 4 * 4 * 4] = {0};
    tga[2] = 2; tga[12] = 4; tga[14] = 4; tga[16] = 32; tga[17] = 0x28;
    for (unsigned i = 0; i < 16; ++i) {
        tga[18 + i * 4] = 9; tga[19 + i * 4] = 17;
        tga[20 + i * 4] = 255; tga[21 + i * 4] = i ? 255 : 128;
    }
    char source[1200]; snprintf(source, sizeof(source), "%s/colour.tga", textures);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(source, tga, sizeof(tga)));
    jce_archive_set_process_key(NULL);
}
void tearDown(void)
{
    jce_archive_set_process_key(NULL);
    jce_fs_set_active(NULL);
    if (root_created) {
        bool owned = root_is_owned();
        TEST_ASSERT_TRUE(owned);
        if (owned) TEST_ASSERT_TRUE(jce_fs_host_remove_recursive(root));
    }
    root_created = false;
}
static JceAssetTexInfo info_from_bytes(const void *bytes, size_t size)
{
    JceAssetView view;
    TEST_ASSERT_TRUE(jce_asset_open(&view, bytes, size));
    const JceAssetChunkEntry *info_chunk = jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_INFO);
    const JceAssetChunkEntry *pixel_chunk = jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_PIXELS);
    TEST_ASSERT_NOT_NULL(info_chunk); TEST_ASSERT_NOT_NULL(pixel_chunk);
    JceAssetTexInfo info;
    uint8_t info_bytes[256];
    TEST_ASSERT_TRUE(info_chunk->original_size <= sizeof(info_bytes));
    TEST_ASSERT_TRUE(jce_asset_chunk_data(&view, info_chunk, info_bytes,
                                          sizeof(info_bytes)) >= sizeof(info));
    memcpy(&info, info_bytes, sizeof(info));
    uint8_t pixels[128];
    size_t decoded = jce_asset_chunk_data(&view, pixel_chunk, pixels, sizeof(pixels));
    TEST_ASSERT_TRUE(decoded > 0 && decoded <= sizeof(pixels));
    TEST_ASSERT_EQUAL_UINT32(4, info.width); TEST_ASSERT_EQUAL_UINT32(4, info.height);
    if (info.format == JCEASSET_TEXFMT_RGBA8) {
        TEST_ASSERT_EQUAL_size_t(info.mip_count == 1 ? 64 : 84, decoded);
        TEST_ASSERT_EQUAL_UINT8(255, pixels[0]); TEST_ASSERT_EQUAL_UINT8(17, pixels[1]);
        TEST_ASSERT_EQUAL_UINT8(9, pixels[2]); TEST_ASSERT_EQUAL_UINT8(128, pixels[3]);
    } else if (info.format == JCEASSET_TEXFMT_ASTC_4x4) {
        TEST_ASSERT_EQUAL_size_t(info.mip_count == 1 ? 16 : 48, decoded);
    }
    return info;
}
static JceAssetTexInfo info_from_file(const char *path)
{
    uint64_t size = 0;
    void *bytes = jce_fs_host_read_all(path, &size); TEST_ASSERT_NOT_NULL(bytes);
    JceAssetTexInfo info = info_from_bytes(bytes, (size_t)size);
    jce_fs_buffer_free(bytes); return info;
}
static int run_cli(bool legacy, const char *output, const char *platform,
                    bool rgba, bool mips, bool explicit_astc)
{
    char *argv[20]; int argc = 0;
    argv[argc++] = "jce_cook"; argv[argc++] = "--batch"; argv[argc++] = textures;
    argv[argc++] = (char *)output; argv[argc++] = "--preserve-names";
    argv[argc++] = "--platform"; argv[argc++] = (char *)platform;
    argv[argc++] = "--level"; argv[argc++] = "0";
    argv[argc++] = "--quality"; argv[argc++] = "fast"; argv[argc++] = "--verbose";
    if (rgba) argv[argc++] = "--rgba8";
    if (mips) argv[argc++] = "--mipmaps";
    if (explicit_astc) { argv[argc++] = "--texfmt"; argv[argc++] = "astc"; }
    argv[argc] = NULL;
    cli_used = 0; cli_text[0] = '\0'; cli_truncated = false;
    int result = legacy ? jce_legacy_cook_tool_main(argc, argv) : jce_tool_main(argc, argv);
    TEST_ASSERT_FALSE(cli_truncated);
    return result;
}
static void check_cli(const char *platform, bool rgba, bool mips,
                       bool astc, uint32_t format)
{
    char output[1150], cooked[1200];
    snprintf(output, sizeof(output), "%s/cli", root);
    TEST_ASSERT_EQUAL_INT(0, run_cli(false, output, platform, rgba, mips, astc));
    snprintf(cooked, sizeof(cooked), "%s/colour.tga", output);
    JceAssetTexInfo info = info_from_file(cooked);
    TEST_ASSERT_EQUAL_UINT32(format, info.format);
    TEST_ASSERT_EQUAL_UINT32(mips ? 3 : 1, info.mip_count);
}
static void test_cli_web_default_actual_rgba8(void)
{ check_cli("web", false, false, false, JCEASSET_TEXFMT_RGBA8); }
static void test_cli_android_default_actual_astc(void)
{ check_cli("android", false, false, false, JCEASSET_TEXFMT_ASTC_4x4); }
static void test_cli_ios_default_actual_astc(void)
{ check_cli("ios", false, false, false, JCEASSET_TEXFMT_ASTC_4x4); }
static void test_cli_rgba_and_mipmap_overrides_survive_mobile_target(void)
{ check_cli("android", true, true, false, JCEASSET_TEXFMT_RGBA8); }
static void test_cli_explicit_astc_and_mips_survive_web_default(void)
{ check_cli("web", false, true, true, JCEASSET_TEXFMT_ASTC_4x4); }
static void pack_log(JceBundlePackLogLevel level, const char *message, void *user)
{
    (void)user;
    if (level == JCE_BUNDLE_PACK_LOG_ERROR) ++bundle_errors;
    if (message && strstr(message, "reused ")) bundle_reused = true;
}
static int run_bundle(bool legacy, const char *output, int platform,
                       bool single, const char *previous)
{
    const char *scenes[] = {scene};
    char scenes_dir[1150]; snprintf(scenes_dir, sizeof(scenes_dir), "%s/scenes", resources);
    JceBundlePackOptions options; memset(&options, 0, sizeof(options));
    options.scenes_dir = scenes_dir; options.resource_root = resources;
    options.out_dir = output; options.scene_files = scenes; options.scene_file_count = 1;
    options.cook_assets = true; options.target_platform = platform; options.zstd_level = 1;
    options.single_file_mode = single; options.single_bundle_id = "texture";
    options.prev_catalog = previous;
    bundle_reused = false; bundle_errors = 0;
    int rc = legacy ? jce_legacy_bundle_pack_run(&options, pack_log, NULL)
                    : jce_bundle_pack_run(&options, pack_log, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, bundle_errors); return rc;
}
static JceAssetTexInfo info_from_bundle(const char *output, bool single, char hash[17])
{
    char bundle_path[1400];
    if (single) snprintf(bundle_path, sizeof(bundle_path), "%s/texture.jbundle", output);
    else {
        char catalog_path[1400]; snprintf(catalog_path, sizeof(catalog_path), "%s/bundle_catalog.json", output);
        uint64_t size = 0; void *bytes = jce_fs_host_read_all(catalog_path, &size);
        TEST_ASSERT_NOT_NULL(bytes);
        JceJson *catalog = jce_json_parse_strict((const char *)bytes, (size_t)size);
        TEST_ASSERT_NOT_NULL(catalog);
        JceJson *entry = jce_json_first_child(jce_json_get(catalog, "bundles"));
        TEST_ASSERT_NOT_NULL(entry);
        const char *file = jce_json_get_string(entry, "file", "");
        TEST_ASSERT_TRUE(file[0] != '\0');
        snprintf(bundle_path, sizeof(bundle_path), "%s/%s", output, file);
        if (hash) snprintf(hash, 17, "%s", jce_json_get_string(entry, "build_hash", ""));
        jce_json_free(catalog); jce_fs_buffer_free(bytes);
    }
    JcePakArchive *pak = jce_pak_open_file(bundle_path); TEST_ASSERT_NOT_NULL(pak);
    const JcePakAsset *asset = jce_pak_find(pak, "textures/colour.tga"); TEST_ASSERT_NOT_NULL(asset);
    void *bytes = jce_malloc((size_t)asset->original_size); TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(asset->original_size,
        jce_pak_decompress(asset, bytes, (size_t)asset->original_size));
    JceAssetTexInfo info = info_from_bytes(bytes, (size_t)asset->original_size);
    jce_free(bytes); jce_pak_close(pak); return info;
}
static void check_bundle(int platform, const char *preset, uint32_t format, uint32_t mips)
{
    if (preset) {
        char path[1300]; snprintf(path, sizeof(path), "%s/colour.tga.import.json", textures);
        write_text(path, preset);
    }
    char output[1150]; snprintf(output, sizeof(output), "%s/bundle", root);
    TEST_ASSERT_EQUAL_INT(0, run_bundle(false, output, platform, true, NULL));
    JceAssetTexInfo info = info_from_bundle(output, true, NULL);
    TEST_ASSERT_EQUAL_UINT32(format, info.format); TEST_ASSERT_EQUAL_UINT32(mips, info.mip_count);
}
static void test_bundle_web_default_actual_rgba8_mips(void)
{ check_bundle(5, NULL, JCEASSET_TEXFMT_RGBA8, 3); }
static void test_bundle_android_default_actual_astc_mips(void)
{ check_bundle(3, NULL, JCEASSET_TEXFMT_ASTC_4x4, 3); }
static void test_bundle_ios_default_actual_astc_mips(void)
{ check_bundle(4, NULL, JCEASSET_TEXFMT_ASTC_4x4, 3); }
static void test_bundle_rgba_and_no_mips_import_override(void)
{ check_bundle(3, "{\"target_format\":\"rgba8\",\"gen_mips\":false}", JCEASSET_TEXFMT_RGBA8, 1); }
static void test_bundle_explicit_astc_import_override_on_web(void)
{ check_bundle(5, "{\"target_format\":\"astc\",\"gen_mips\":true}", JCEASSET_TEXFMT_ASTC_4x4, 3); }
static uint64_t cli_cache_hash(const char *output)
{
    char path[1300]; snprintf(path, sizeof(path), "%s/.jce_cook_catalog", output);
    JceCookCatalog catalog; TEST_ASSERT_TRUE(jce_cook_catalog_load(&catalog, path));
    const JceCookCatalogEntry *entry = jce_cook_catalog_find(&catalog, "colour.tga");
    TEST_ASSERT_NOT_NULL(entry); uint64_t hash = entry->hash;
    jce_cook_catalog_free(&catalog); return hash;
}
static void test_actual_old_cli_cache_rebuilds_then_current_warm_request_skips(void)
{
    char output[1150], cooked[1200]; snprintf(output, sizeof(output), "%s/cache_cli", root);
    snprintf(cooked, sizeof(cooked), "%s/colour.tga", output);
    TEST_ASSERT_EQUAL_INT(0, run_cli(true, output, "web", false, true, false));
    JceAssetTexInfo old = info_from_file(cooked);
    TEST_ASSERT_EQUAL_UINT32(JCEASSET_TEXFMT_ASTC_4x4, old.format);
    uint64_t old_hash = cli_cache_hash(output);
    TEST_ASSERT_EQUAL_INT(0, run_cli(false, output, "web", false, true, false));
    bool rebuilt = strstr(cli_text, "(unchanged)") == NULL;
    JceAssetTexInfo current = info_from_file(cooked); uint64_t current_hash = cli_cache_hash(output);
    TEST_ASSERT_EQUAL_INT(0, run_cli(false, output, "web", false, true, false));
    bool warm_skipped = strstr(cli_text, "(unchanged) colour.tga") != NULL;
    uint64_t warm_hash = cli_cache_hash(output);
    TEST_ASSERT_TRUE(rebuilt); TEST_ASSERT_TRUE(warm_skipped);
    TEST_ASSERT_NOT_EQUAL_UINT64(old_hash, current_hash); TEST_ASSERT_EQUAL_UINT64(current_hash, warm_hash);
    TEST_ASSERT_EQUAL_UINT32(JCEASSET_TEXFMT_RGBA8, current.format);
    TEST_ASSERT_EQUAL_UINT32(3, current.mip_count);
}
static void test_actual_old_bundle_cache_rebuilds_then_current_warm_request_reuses(void)
{
    char old_out[1150], current_out[1150], old_catalog[1300], current_catalog[1300];
    snprintf(old_out, sizeof(old_out), "%s/cache_old_bundle", root);
    snprintf(current_out, sizeof(current_out), "%s/cache_new_bundle", root);
    snprintf(old_catalog, sizeof(old_catalog), "%s/bundle_catalog.json", old_out);
    snprintf(current_catalog, sizeof(current_catalog), "%s/bundle_catalog.json", current_out);
    TEST_ASSERT_EQUAL_INT(0, run_bundle(true, old_out, 5, false, NULL));
    char old_hash[17], current_hash[17], warm_hash[17];
    JceAssetTexInfo old = info_from_bundle(old_out, false, old_hash);
    TEST_ASSERT_EQUAL_UINT32(JCEASSET_TEXFMT_ASTC_4x4, old.format);
    TEST_ASSERT_EQUAL_INT(0, run_bundle(false, current_out, 5, false, old_catalog));
    bool rebuilt = !bundle_reused;
    JceAssetTexInfo current = info_from_bundle(current_out, false, current_hash);
    TEST_ASSERT_EQUAL_INT(0, run_bundle(false, current_out, 5, false, current_catalog));
    bool warm_reused = bundle_reused;
    (void)info_from_bundle(current_out, false, warm_hash);
    TEST_ASSERT_TRUE(rebuilt); TEST_ASSERT_TRUE(warm_reused);
    TEST_ASSERT_TRUE(strcmp(old_hash, current_hash) != 0); TEST_ASSERT_EQUAL_STRING(current_hash, warm_hash);
    TEST_ASSERT_EQUAL_UINT32(JCEASSET_TEXFMT_RGBA8, current.format);
    TEST_ASSERT_EQUAL_UINT32(3, current.mip_count);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cli_web_default_actual_rgba8);
    RUN_TEST(test_cli_android_default_actual_astc);
    RUN_TEST(test_cli_ios_default_actual_astc);
    RUN_TEST(test_cli_rgba_and_mipmap_overrides_survive_mobile_target);
    RUN_TEST(test_cli_explicit_astc_and_mips_survive_web_default);
    RUN_TEST(test_bundle_web_default_actual_rgba8_mips);
    RUN_TEST(test_bundle_android_default_actual_astc_mips);
    RUN_TEST(test_bundle_ios_default_actual_astc_mips);
    RUN_TEST(test_bundle_rgba_and_no_mips_import_override);
    RUN_TEST(test_bundle_explicit_astc_import_override_on_web);
    RUN_TEST(test_actual_old_cli_cache_rebuilds_then_current_warm_request_skips);
    RUN_TEST(test_actual_old_bundle_cache_rebuilds_then_current_warm_request_reuses);
    return UNITY_END();
}
