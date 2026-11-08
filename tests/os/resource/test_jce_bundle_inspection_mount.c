/* Actual public Bundle/Pak/VFS admission. No renderer/device or native bootstrap.
 * Mutants compile one complete owner with just its admission wire omitted. */
#include "unity.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/resource/jce_archive_writer.h>
#include <jce/resource/jce_bundle_loader.h>
#include <jce/resource/jce_bundle_pack.h>
#include <jce/resource/jce_pak_loader.h>
#include <stdio.h>
#include <string.h>

#ifndef JCE_BUNDLE_INSPECTION_FIXTURE_NAMESPACE
#error Every registered inspection fixture needs its distinct target namespace
#endif
static char root[1024], owned_prefix[1024];
static bool root_created;
static const JceFsPakProvider *previous_provider;
static bool root_is_owned(void)
{
    size_t prefix_size = strlen(owned_prefix);
    return prefix_size > 0 && strncmp(root, owned_prefix, prefix_size) == 0 &&
        root[prefix_size] != '\0' && strchr(root + prefix_size, '/') == NULL &&
        strchr(root + prefix_size, '\\') == NULL;
}
void setUp(void)
{
    static unsigned counter;
    char cwd[1024], base[1024], desired[1100];
    previous_provider = jce_fs_get_pak_provider();
    root_created = false; root[0] = owned_prefix[0] = '\0';
    TEST_ASSERT_TRUE(jce_fs_host_get_current_dir(cwd, sizeof(cwd)));
    TEST_ASSERT_TRUE(jce_path_normalize(base, sizeof(base), cwd));
    int length = snprintf(owned_prefix, sizeof(owned_prefix), "%s/_ut_inspection_%s_",
                           base, JCE_BUNDLE_INSPECTION_FIXTURE_NAMESPACE);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof(owned_prefix));
    snprintf(desired, sizeof(desired), "%s%u", owned_prefix, ++counter);
    /* Namespace isolation precedes candidate discovery; the public unique-path
     * helper is not an atomic reservation against uncoordinated callers. */
    TEST_ASSERT_TRUE(jce_fs_host_make_unique_path(desired, root, sizeof(root)));
    TEST_ASSERT_TRUE(root_is_owned());
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(root));
    TEST_ASSERT_FALSE(jce_fs_host_exists_dir(root));
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(root));
    root_created = true;
    jce_fs_set_pak_provider(NULL);
}
void tearDown(void)
{
    jce_fs_set_active(NULL);
    jce_fs_set_pak_provider(previous_provider);
    if (root_created) {
        bool owned = root_is_owned();
        TEST_ASSERT_TRUE(owned);
        if (owned) TEST_ASSERT_TRUE(jce_fs_host_remove_recursive(root));
    }
    root_created = false;
}
static void path_join(char *out, size_t cap, const char *leaf)
{ snprintf(out, cap, "%s/%s", root, leaf); }
static void quiet_log(JceBundlePackLogLevel level, const char *message, void *user)
{ (void)level; (void)message; (void)user; }
static void make_bundle(char path[1024], bool portable, bool sidecar)
{
    char scene[1024], sidecar_path[1050];
    const char *scenes[1];
    path_join(scene, sizeof(scene), "base.scene");
    const char text[] = "{\"entities\":[]}";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(scene, text, sizeof(text) - 1));
    scenes[0] = scene;
    JceBundlePackOptions options;
    memset(&options, 0, sizeof(options));
    options.scenes_dir = root; options.resource_root = root; options.out_dir = root;
    options.scene_files = scenes; options.scene_file_count = 1;
    options.single_file_mode = true; options.single_bundle_id = "admission";
    options.zstd_level = 1; options.cook_assets = true;
    options.target_platform = portable ? 6 : 5;
    TEST_ASSERT_EQUAL_INT(0, jce_bundle_pack_run(&options, quiet_log, NULL));
    path_join(path, 1024, "admission.jbundle");
    snprintf(sidecar_path, sizeof(sidecar_path), "%s.json", path);
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(sidecar_path));
    if (!sidecar) TEST_ASSERT_TRUE(jce_fs_host_remove_file(sidecar_path));
}
static JceBundleFile *inspect(const char *path, bool memory, void **bytes)
{
    *bytes = NULL;
    if (!memory) return jce_bundle_file_inspect_file(path, "web");
    uint64_t size = 0;
    *bytes = jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(*bytes);
    return jce_bundle_file_inspect_memory(*bytes, (size_t)size, "web");
}
static void check_inspection_mount_refused(bool memory, bool sidecar)
{
    char path[1024]; make_bundle(path, false, sidecar);
    void *bytes;
    JceBundleFile *bundle = inspect(path, memory, &bytes);
    TEST_ASSERT_NOT_NULL(bundle);
    TEST_ASSERT_EQUAL_STRING("admission", jce_bundle_file_id(bundle));
    JcePakArchive *pak = jce_bundle_file_pak(bundle);
    TEST_ASSERT_NOT_NULL(pak);
    TEST_ASSERT_NOT_NULL(jce_pak_find(pak, "base.scene")); /* borrowed reads stay useful */
    JceFileSystem *fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(fs);
    bool legacy_refused = !jce_fs_mount_pak(fs, pak);
    bool named_refused = !jce_fs_mount_pak_named(fs, "inspection", pak);
    uint32_t count = jce_fs_mounted_pak_count(fs);
    bool visible = jce_fs_exists(fs, "base.scene");
    jce_fs_destroy(fs); jce_bundle_file_close(bundle);
    if (bytes) jce_fs_buffer_free(bytes);
    TEST_ASSERT_TRUE(legacy_refused);
    TEST_ASSERT_TRUE(named_refused);
    TEST_ASSERT_EQUAL_UINT32(0, count);
    TEST_ASSERT_FALSE(visible);
}
static void test_memory_with_sidecar_borrowed_pak_cannot_mount(void)
{ check_inspection_mount_refused(true, true); }
static void test_memory_without_sidecar_borrowed_pak_cannot_mount(void)
{ check_inspection_mount_refused(true, false); }
static void test_file_with_verified_sidecar_borrowed_pak_cannot_mount(void)
{ check_inspection_mount_refused(false, true); }
static void test_file_without_sidecar_borrowed_pak_cannot_mount(void)
{ check_inspection_mount_refused(false, false); }

static const void *JCE_CALL opaque_find(const JcePakArchive *pak, const char *path)
{ (void)pak; (void)path; return NULL; }
static uint64_t JCE_CALL opaque_size(const void *asset)
{ (void)asset; return 0; }
static size_t JCE_CALL opaque_decode(const void *asset, void *out, size_t size)
{ (void)asset; (void)out; (void)size; return 0; }
static const JceFsPakProvider opaque_provider = {opaque_find, opaque_size, opaque_decode};
static void test_old_public_setter_preserves_canonical_tuple_guard(void)
{
    char path[1024]; make_bundle(path, false, true);
    JceBundleFile *bundle = jce_bundle_file_inspect_file(path, "web");
    TEST_ASSERT_NOT_NULL(bundle);
    const JceFsPakProvider *canonical = jce_fs_get_pak_provider();
    TEST_ASSERT_NOT_NULL(canonical);
    JceFsPakProvider tuple_copy = *canonical;
    jce_fs_set_pak_provider(&opaque_provider);
    jce_fs_set_pak_provider(canonical);
    JceFileSystem *fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(fs);
    JcePakArchive *pak = jce_bundle_file_pak(bundle);
    bool pointer_refused = !jce_fs_mount_pak(fs, pak);
    jce_fs_set_pak_provider(&tuple_copy);
    bool copy_refused = !jce_fs_mount_pak_named(fs, "tuple-copy", pak);
    uint32_t count = jce_fs_mounted_pak_count(fs);
    jce_fs_destroy(fs); jce_bundle_file_close(bundle);
    jce_fs_set_pak_provider(canonical);
    TEST_ASSERT_TRUE(pointer_refused); TEST_ASSERT_TRUE(copy_refused);
    TEST_ASSERT_EQUAL_UINT32(0, count);
}
static void test_unknown_opaque_provider_keeps_original_three_callback_abi(void)
{
    char path[1024]; make_bundle(path, false, true);
    JceBundleFile *bundle = jce_bundle_file_inspect_file(path, "web");
    TEST_ASSERT_NOT_NULL(bundle); /* establish the guarded canonical tuple first */
    const JceFsPakProvider *canonical = jce_fs_get_pak_provider();
    jce_fs_set_pak_provider(&opaque_provider);
    unsigned char opaque = 0; /* never a native-Pak layout */
    JceFileSystem *fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(fs);
    bool legacy_ok = jce_fs_mount_pak(fs, (JcePakArchive *)&opaque);
    bool named_ok = jce_fs_mount_pak_named(fs, "opaque", (JcePakArchive *)&opaque);
    bool absent = !jce_fs_exists(fs, "unused.txt");
    jce_fs_destroy(fs); jce_fs_set_pak_provider(canonical); jce_bundle_file_close(bundle);
    TEST_ASSERT_TRUE(legacy_ok); TEST_ASSERT_TRUE(named_ok); TEST_ASSERT_TRUE(absent);
}
static void test_null_provider_cannot_mount_inspection_before_canonical_restore(void)
{
    char path[1024]; make_bundle(path, false, true);
    JceBundleFile *bundle = jce_bundle_file_inspect_file(path, "web");
    TEST_ASSERT_NOT_NULL(bundle);
    const JceFsPakProvider *canonical = jce_fs_get_pak_provider();
    TEST_ASSERT_NOT_NULL(canonical);
    JceFileSystem *fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(fs);
    JcePakArchive *pak = jce_bundle_file_pak(bundle);
    jce_fs_set_pak_provider(NULL);
    bool legacy_refused = !jce_fs_mount_pak(fs, pak);
    bool named_refused = !jce_fs_mount_pak_named(fs, "provider-cleared", pak);
    jce_fs_set_pak_provider(canonical);
    uint32_t count = jce_fs_mounted_pak_count(fs);
    bool visible = jce_fs_exists(fs, "base.scene");
    jce_fs_destroy(fs); jce_bundle_file_close(bundle);
    TEST_ASSERT_TRUE(legacy_refused); TEST_ASSERT_TRUE(named_refused);
    TEST_ASSERT_EQUAL_UINT32(0, count); TEST_ASSERT_FALSE(visible);
}
static JcePakArchive *raw_pak(const char *address, void **bytes)
{
    JceArchiveWriter *writer = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(writer);
    TEST_ASSERT_TRUE(jce_archive_writer_add(writer, address, "raw", 3));
    size_t size = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(writer, bytes, &size));
    jce_archive_writer_destroy(writer);
    JcePakArchive *pak = jce_pak_open(*bytes, size); TEST_ASSERT_NOT_NULL(pak);
    return pak;
}
static void test_runtime_bundle_and_native_raw_pak_keep_both_mount_routes(void)
{
    char path[1024]; make_bundle(path, true, true);
    JceBundleFile *bundle = jce_bundle_file_open(NULL, path, NULL);
    TEST_ASSERT_NOT_NULL(bundle); /* modern portable content admitted by compiled host */
    JceFileSystem *fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(fs);
    JcePakArchive *runtime_pak = jce_bundle_file_pak(bundle);
    bool runtime_legacy = jce_fs_mount_pak(fs, runtime_pak);
    bool runtime_named = jce_fs_mount_pak_named(fs, "runtime", runtime_pak);
    bool runtime_visible = jce_fs_exists(fs, "base.scene");
    void *bytes; JcePakArchive *raw = raw_pak("data/raw.txt", &bytes);
    bool raw_legacy = jce_fs_mount_pak(fs, raw); /* replacement of unnamed slot */
    bool raw_named = jce_fs_mount_pak_named(fs, "raw", raw);
    bool raw_visible = jce_fs_exists(fs, "data/raw.txt");
    jce_fs_destroy(fs); jce_pak_close(raw); jce_free(bytes); jce_bundle_file_close(bundle);
    TEST_ASSERT_TRUE(runtime_legacy); TEST_ASSERT_TRUE(runtime_named); TEST_ASSERT_TRUE(runtime_visible);
    TEST_ASSERT_TRUE(raw_legacy); TEST_ASSERT_TRUE(raw_named); TEST_ASSERT_TRUE(raw_visible);
}
static void test_inspection_cannot_be_injected_into_already_mounted_fallback(void)
{
    char path[1024]; make_bundle(path, false, true);
    JceBundleFile *bundle = jce_bundle_file_inspect_file(path, "web");
    TEST_ASSERT_NOT_NULL(bundle);
    void *base_bytes, *layer_bytes;
    JcePakArchive *base = raw_pak("data/base.txt", &base_bytes);
    JcePakArchive *layer = raw_pak("data/layer.txt", &layer_bytes);
    JceFileSystem *fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_TRUE(jce_fs_mount_pak(fs, base));
    jce_pak_overlay_push(base, jce_bundle_file_pak(bundle));
    bool inspection_absent = !jce_fs_exists(fs, "base.scene");
    jce_pak_overlay_remove(base, jce_bundle_file_pak(bundle)); /* mutant cleanup */
    jce_pak_overlay_push(base, layer);
    bool ordinary_visible = jce_fs_exists(fs, "data/layer.txt");
    jce_pak_overlay_remove(base, layer);
    jce_fs_destroy(fs); jce_pak_close(base); jce_pak_close(layer);
    jce_free(base_bytes); jce_free(layer_bytes); jce_bundle_file_close(bundle);
    TEST_ASSERT_TRUE(inspection_absent); TEST_ASSERT_TRUE(ordinary_visible);
}
static void test_inspection_base_cannot_acquire_runtime_fallback(void)
{
    char path[1024]; make_bundle(path, false, false);
    void *bytes; JceBundleFile *bundle = inspect(path, true, &bytes);
    TEST_ASSERT_NOT_NULL(bundle);
    void *raw_bytes; JcePakArchive *raw = raw_pak("data/raw.txt", &raw_bytes);
    JcePakArchive *inspected = jce_bundle_file_pak(bundle);
    jce_pak_overlay_push(inspected, raw);
    bool absent = jce_pak_find(inspected, "data/raw.txt") == NULL;
    jce_pak_overlay_remove(inspected, raw);
    jce_pak_close(raw); jce_free(raw_bytes); jce_bundle_file_close(bundle); jce_fs_buffer_free(bytes);
    TEST_ASSERT_TRUE(absent);
}
static void test_shared_tail_fallback_push_refuses_cycle_and_keeps_native_mountable(void)
{
    char path[1024]; make_bundle(path, true, true);
    JceBundleFile *runtime = jce_bundle_file_open(NULL, path, NULL);
    TEST_ASSERT_NOT_NULL(runtime); /* installs canonical read/admission tuple */
    jce_bundle_file_close(runtime);
    void *a_bytes, *b_bytes, *common_bytes;
    JcePakArchive *a = raw_pak("data/a.txt", &a_bytes);
    JcePakArchive *b = raw_pak("data/b.txt", &b_bytes);
    JcePakArchive *common = raw_pak("data/common.txt", &common_bytes);
    jce_pak_overlay_push(a, common);
    jce_pak_overlay_push(b, common);
    jce_pak_overlay_push(a, b); /* A->common, B->common must not become a cycle */
    JceFileSystem *fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(fs);
    bool native_legacy_ok = jce_fs_mount_pak(fs, a);
    bool native_named_ok = jce_fs_mount_pak_named(fs, "acyclic", a);
    bool common_present = jce_pak_find(a, "data/common.txt") != NULL;
    /* This address occurs before a repeated tail even in the old-wire mutant:
     * its lookup terminates, and the new mount predicate detects that cycle. */
    bool b_absent = jce_pak_find(a, "data/b.txt") == NULL;
    /* Unlink the repeated common node first, then B, then any restored common;
     * this is finite for both the accepted chain and the exact old-wire cycle. */
    jce_pak_overlay_remove(a, common);
    jce_pak_overlay_remove(a, b);
    jce_pak_overlay_remove(a, common);
    jce_pak_overlay_remove(b, common);
    jce_fs_destroy(fs);
    jce_pak_close(a); jce_pak_close(b); jce_pak_close(common);
    jce_free(a_bytes); jce_free(b_bytes); jce_free(common_bytes);
    TEST_ASSERT_TRUE(native_legacy_ok); TEST_ASSERT_TRUE(native_named_ok);
    TEST_ASSERT_TRUE(common_present); TEST_ASSERT_TRUE(b_absent);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_memory_with_sidecar_borrowed_pak_cannot_mount);
    RUN_TEST(test_memory_without_sidecar_borrowed_pak_cannot_mount);
    RUN_TEST(test_file_with_verified_sidecar_borrowed_pak_cannot_mount);
    RUN_TEST(test_file_without_sidecar_borrowed_pak_cannot_mount);
    RUN_TEST(test_old_public_setter_preserves_canonical_tuple_guard);
    RUN_TEST(test_unknown_opaque_provider_keeps_original_three_callback_abi);
    RUN_TEST(test_null_provider_cannot_mount_inspection_before_canonical_restore);
    RUN_TEST(test_runtime_bundle_and_native_raw_pak_keep_both_mount_routes);
    RUN_TEST(test_inspection_cannot_be_injected_into_already_mounted_fallback);
    RUN_TEST(test_inspection_base_cannot_acquire_runtime_fallback);
    RUN_TEST(test_shared_tail_fallback_push_refuses_cycle_and_keeps_native_mountable);
    return UNITY_END();
}
