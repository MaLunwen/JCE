/* Transactional and integrity tests for the bundle catalog loader. */

#include "unity.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_archive_cook.h>
#include <jce/resource/jce_bundle_format.h>
#include <jce/resource/jce_bundle_loader.h>
#include <jce/resource/jce_bundle_pack.h>

#include <xxhash.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_root[1024];

#define CATALOG_V11_COMPAT_METADATA \
    " \"target_profile\": \"auto\"," \
    " \"content_abi\": \"jce-content-1\"," \
    " \"cook_version\": 0," \
    " \"minimum_engine_version\": \"0.0.0\","

void setUp(void)
{
    static int counter;
    char cwd[1024];
    (void)jce_fs_host_get_current_dir(cwd, sizeof(cwd));
    snprintf(g_root, sizeof(g_root), "%s/_ut_bundle_loader_%d",
             cwd, ++counter);
    (void)jce_fs_host_remove_recursive(g_root);
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(g_root));
}

void tearDown(void)
{
    jce_fs_set_active(NULL);
    (void)jce_fs_host_remove_recursive(g_root);
}

static void path_join(char *out, size_t cap, const char *leaf)
{
    snprintf(out, cap, "%s/%s", g_root, leaf);
}

static void quiet_log(JceBundlePackLogLevel level, const char *message,
                      void *user)
{
    (void)level;
    (void)message;
    (void)user;
}

static void make_bundle(const char *id)
{
    char scene[1024];
    const char *scenes[1];
    path_join(scene, sizeof(scene), "base.scene");
    TEST_ASSERT_TRUE(jce_fs_host_write_all(
        scene, "{ \"entities\": [] }", 18));
    scenes[0] = scene;

    JceBundlePackOptions options;
    memset(&options, 0, sizeof(options));
    options.scenes_dir = g_root;
    options.resource_root = g_root;
    options.out_dir = g_root;
    options.scene_files = scenes;
    options.scene_file_count = 1;
    options.single_file_mode = true;
    options.single_bundle_id = id;
    options.zstd_level = 1;
    TEST_ASSERT_EQUAL_INT(0,
        jce_bundle_pack_run(&options, quiet_log, NULL));
}

static void bundle_facts(const char *file, uint64_t *size, char hash[17])
{
    char path[1024];
    path_join(path, sizeof(path), file);
    void *data = jce_fs_host_read_all(path, size);
    TEST_ASSERT_NOT_NULL(data);
    uint64_t value = XXH3_64bits(data, (size_t)*size);
    snprintf(hash, 17, "%016llx", (unsigned long long)value);
    jce_fs_buffer_free(data);
}

static void bundle_build_hash(const char *file, char hash[17])
{
    char sidecar_name[256];
    char path[1024];
    snprintf(sidecar_name, sizeof(sidecar_name), "%s.json", file);
    path_join(path, sizeof(path), sidecar_name);
    uint64_t size = 0;
    void *data = jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(data);
    char *text = (char *)jce_malloc((size_t)size + 1);
    TEST_ASSERT_NOT_NULL(text);
    memcpy(text, data, (size_t)size);
    text[size] = '\0';
    const char *key = strstr(text, "\"build_hash\"");
    const char *colon = key ? strchr(key, ':') : NULL;
    const char *quote = colon ? strchr(colon, '"') : NULL;
    TEST_ASSERT_NOT_NULL(quote);
    TEST_ASSERT_EQUAL_CHAR('"', quote[17]);
    memcpy(hash, quote + 1, 16);
    hash[16] = '\0';
    jce_free(text);
    jce_fs_buffer_free(data);
}

static void write_catalog(const char *json)
{
    char path[1024];
    path_join(path, sizeof(path), "bundle_catalog.json");
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, json,
                                           (uint64_t)strlen(json)));
}

static JceBundleCatalog *open_catalog(JceFileSystem *fs)
{
    char path[1024];
    path_join(path, sizeof(path), "bundle_catalog.json");
    return jce_bundle_catalog_open(fs, path);
}

static void test_catalog_requires_contract(void)
{
    write_catalog("{ \"version\": 1, \"bundles\": {} }");
    JceFileSystem *fs = jce_fs_create();
    JceBundleCatalog *catalog = open_catalog(fs);
    bool rejected = catalog == NULL;
    if (catalog) jce_bundle_catalog_close(catalog);
    jce_fs_destroy(fs);
    TEST_ASSERT_TRUE(rejected);
}

static void test_catalog_rejects_incompatible_content_abi(void)
{
    write_catalog(
        "{ \"contract\": { \"name\": \"jce.bundle.catalog\","
        " \"major\": 1, \"minor\": 1 }, \"version\": 1,"
        " \"target_profile\": \"auto\","
        " \"content_abi\": \"jce-content-999\","
        " \"cook_version\": 1,"
        " \"minimum_engine_version\": \"0.0.0\","
        " \"bundles\": {} }");
    JceFileSystem *fs = jce_fs_create();
    JceBundleCatalog *catalog = open_catalog(fs);
    bool rejected = catalog == NULL;
    if (catalog) jce_bundle_catalog_close(catalog);
    jce_fs_destroy(fs);
    TEST_ASSERT_TRUE(rejected);
}

static void test_catalog_rejects_incompatible_target_profile(void)
{
    write_catalog(
        "{ \"contract\": { \"name\": \"jce.bundle.catalog\","
        " \"major\": 1, \"minor\": 1 }, \"version\": 1,"
        " \"target_profile\": \"unsupported-platform\","
        " \"content_abi\": \"jce-content-1\","
        " \"cook_version\": 1,"
        " \"minimum_engine_version\": \"0.0.0\","
        " \"bundles\": {} }");
    JceFileSystem *fs = jce_fs_create();
    JceBundleCatalog *catalog = open_catalog(fs);
    bool rejected = catalog == NULL;
    if (catalog) jce_bundle_catalog_close(catalog);
    jce_fs_destroy(fs);
    TEST_ASSERT_TRUE(rejected);
}

static void test_catalog_accepts_legacy_1_0_without_content_metadata(void)
{
    write_catalog(
        "{ \"contract\": { \"name\": \"jce.bundle.catalog\","
        " \"major\": 1, \"minor\": 0 }, \"version\": 1,"
        " \"bundles\": {} }");
    JceFileSystem *fs = jce_fs_create();
    JceBundleCatalog *catalog = open_catalog(fs);
    TEST_ASSERT_NOT_NULL(catalog);
    jce_bundle_catalog_close(catalog);
    jce_fs_destroy(fs);
}

static void test_catalog_rejects_unknown_dependency(void)
{
    make_bundle("base");
    uint64_t size = 0;
    char hash[17];
    bundle_facts("base.jbundle", &size, hash);

    char json[2048];
    snprintf(json, sizeof(json),
        "{ \"contract\": { \"name\": \"jce.bundle.catalog\","
        " \"major\": 1, \"minor\": 1 }, \"version\": 1,"
        CATALOG_V11_COMPAT_METADATA
        " \"bundles\": { \"base\": { \"file\": \"base.jbundle\","
        " \"kind\": \"scene\", \"scene_path\": \"base.scene\","
        " \"content_hash\": \"%s\","
        " \"build_hash\": \"0000000000000001\", \"size\": %llu,"
        " \"deps\": [\"missing\"] } } }",
        hash, (unsigned long long)size);
    write_catalog(json);

    JceFileSystem *fs = jce_fs_create();
    JceBundleCatalog *catalog = open_catalog(fs);
    bool rejected = catalog == NULL;
    if (catalog) jce_bundle_catalog_close(catalog);
    jce_fs_destroy(fs);
    TEST_ASSERT_TRUE(rejected);
}

static void test_catalog_rejects_dependency_cycle(void)
{
    write_catalog(
        "{ \"contract\": { \"name\": \"jce.bundle.catalog\","
        " \"major\": 1, \"minor\": 1 }, \"version\": 1,"
        CATALOG_V11_COMPAT_METADATA
        " \"bundles\": {"
        "  \"a\": { \"file\": \"a.jbundle\", \"kind\": \"scene\","
        "    \"scene_path\": \"a.scene\","
        "    \"content_hash\": \"0000000000000001\","
        "    \"build_hash\": \"0000000000000001\", \"size\": 1,"
        "    \"deps\": [\"b\"] },"
        "  \"b\": { \"file\": \"b.jbundle\", \"kind\": \"shared\","
        "    \"content_hash\": \"0000000000000002\","
        "    \"build_hash\": \"0000000000000002\", \"size\": 1,"
        "    \"deps\": [\"a\"] }"
        " } }");

    JceFileSystem *fs = jce_fs_create();
    JceBundleCatalog *catalog = open_catalog(fs);
    bool rejected = catalog == NULL;
    if (catalog) jce_bundle_catalog_close(catalog);
    jce_fs_destroy(fs);
    TEST_ASSERT_TRUE(rejected);
}

static void test_mount_failure_rolls_back_dependency(void)
{
    make_bundle("base");
    uint64_t size = 0;
    char hash[17];
    char build_hash[17];
    bundle_facts("base.jbundle", &size, hash);
    bundle_build_hash("base.jbundle", build_hash);

    char json[3072];
    snprintf(json, sizeof(json),
        "{ \"contract\": { \"name\": \"jce.bundle.catalog\","
        " \"major\": 1, \"minor\": 1 }, \"version\": 1,"
        CATALOG_V11_COMPAT_METADATA
        " \"bundles\": {"
        "  \"base\": { \"file\": \"base.jbundle\", \"kind\": \"scene\","
        "    \"scene_path\": \"base.scene\","
        "    \"content_hash\": \"%s\","
        "    \"build_hash\": \"%s\","
        "    \"size\": %llu, \"deps\": [] },"
        "  \"root\": { \"file\": \"not-present.jbundle\","
        "    \"kind\": \"scene\", \"scene_path\": \"root.scene\","
        "    \"content_hash\": \"%s\","
        "    \"build_hash\": \"0000000000000002\", \"size\": %llu,"
        "    \"deps\": [\"base\"] }"
        " } }",
        hash, build_hash, (unsigned long long)size,
        hash, (unsigned long long)size);
    write_catalog(json);

    JceFileSystem *fs = jce_fs_create();
    JceBundleCatalog *catalog = open_catalog(fs);
    TEST_ASSERT_NOT_NULL(catalog);
    TEST_ASSERT_FALSE(jce_bundle_mount(catalog, "root"));
    TEST_ASSERT_FALSE(jce_bundle_is_mounted(catalog, "base"));
    TEST_ASSERT_EQUAL_UINT32(0, jce_bundle_refcount(catalog, "base"));
    jce_bundle_catalog_close(catalog);
    jce_fs_destroy(fs);
}

static void test_mount_verifies_size_and_content_hash(void)
{
    make_bundle("base");
    uint64_t size = 0;
    char hash[17];
    char build_hash[17];
    bundle_facts("base.jbundle", &size, hash);
    bundle_build_hash("base.jbundle", build_hash);

    char json[2048];
    snprintf(json, sizeof(json),
        "{ \"contract\": { \"name\": \"jce.bundle.catalog\","
        " \"major\": 1, \"minor\": 1 }, \"version\": 1,"
        CATALOG_V11_COMPAT_METADATA
        " \"bundles\": { \"base\": { \"file\": \"base.jbundle\","
        " \"kind\": \"scene\", \"scene_path\": \"base.scene\","
        " \"content_hash\": \"0000000000000000\","
        " \"build_hash\": \"%s\","
        " \"size\": %llu, \"deps\": [] } } }",
        build_hash, (unsigned long long)size);
    write_catalog(json);

    JceFileSystem *fs = jce_fs_create();
    JceBundleCatalog *catalog = open_catalog(fs);
    TEST_ASSERT_NOT_NULL(catalog);
    TEST_ASSERT_FALSE(jce_bundle_mount(catalog, "base"));
    TEST_ASSERT_FALSE(jce_bundle_is_mounted(catalog, "base"));
    jce_bundle_catalog_close(catalog);

    snprintf(json, sizeof(json),
        "{ \"contract\": { \"name\": \"jce.bundle.catalog\","
        " \"major\": 1, \"minor\": 1 }, \"version\": 1,"
        CATALOG_V11_COMPAT_METADATA
        " \"bundles\": { \"base\": { \"file\": \"base.jbundle\","
        " \"kind\": \"scene\", \"scene_path\": \"base.scene\","
        " \"content_hash\": \"%s\","
        " \"build_hash\": \"%s\","
        " \"size\": %llu, \"deps\": [] } } }",
        hash, build_hash, (unsigned long long)size);
    write_catalog(json);
    catalog = open_catalog(fs);
    TEST_ASSERT_NOT_NULL(catalog);
    TEST_ASSERT_TRUE(jce_bundle_mount(catalog, "base"));
    TEST_ASSERT_EQUAL_UINT32(1, jce_bundle_refcount(catalog, "base"));
    TEST_ASSERT_TRUE(jce_bundle_unmount(catalog, "base"));
    TEST_ASSERT_FALSE(jce_bundle_is_mounted(catalog, "base"));
    jce_bundle_catalog_close(catalog);
    jce_fs_destroy(fs);
}

static void test_mount_rejects_catalog_manifest_identity_mismatch(void)
{
    make_bundle("base");
    uint64_t size = 0;
    char hash[17];
    char build_hash[17];
    bundle_facts("base.jbundle", &size, hash);
    bundle_build_hash("base.jbundle", build_hash);

    char json[2048];
    snprintf(json, sizeof(json),
        "{ \"contract\": { \"name\": \"jce.bundle.catalog\","
        " \"major\": 1, \"minor\": 1 }, \"version\": 1,"
        CATALOG_V11_COMPAT_METADATA
        " \"bundles\": { \"alias\": { \"file\": \"base.jbundle\","
        " \"kind\": \"scene\", \"scene_path\": \"base.scene\","
        " \"content_hash\": \"%s\","
        " \"build_hash\": \"%s\","
        " \"size\": %llu, \"deps\": [] } } }",
        hash, build_hash, (unsigned long long)size);
    write_catalog(json);

    JceFileSystem *fs = jce_fs_create();
    JceBundleCatalog *catalog = open_catalog(fs);
    TEST_ASSERT_NOT_NULL(catalog);
    TEST_ASSERT_FALSE(jce_bundle_mount(catalog, "alias"));
    TEST_ASSERT_FALSE(jce_bundle_is_mounted(catalog, "alias"));
    jce_bundle_catalog_close(catalog);
    jce_fs_destroy(fs);
}

static void test_standalone_sidecar_detects_archive_tampering(void)
{
    make_bundle("base");
    char path[1024];
    path_join(path, sizeof(path), "base.jbundle");

    uint64_t size = 0;
    uint8_t *data = (uint8_t *)jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_TRUE(size > 0);
    data[size - 1] ^= 0x5au;
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, data, size));
    jce_fs_buffer_free(data);

    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);
    JceBundleFile *bundle = jce_bundle_file_open(fs, path, NULL);
    bool rejected = bundle == NULL;
    if (bundle) jce_bundle_file_close(bundle);
    jce_fs_destroy(fs);
    TEST_ASSERT_TRUE(rejected);
}

static void test_standalone_requires_bundle_manifest(void)
{
    const char payload[] = "not a bundle";
    JceCookInput input = {
        "data/plain.txt", payload, sizeof(payload) - 1
    };
    JceCookConfig config;
    memset(&config, 0, sizeof(config));
    config.zstd_level = 1;
    config.emit_debug_paths = true;

    void *archive = NULL;
    size_t archive_size = 0;
    TEST_ASSERT_TRUE(jce_archive_cook(&input, 1, &config,
                                      &archive, &archive_size, NULL));
    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);
    JceBundleFile *bundle = jce_bundle_file_open_memory(
        fs, archive, archive_size, "plain");
    bool rejected = bundle == NULL;
    if (bundle) jce_bundle_file_close(bundle);
    jce_fs_destroy(fs);
    jce_free(archive);
    TEST_ASSERT_TRUE(rejected);
}

static void test_standalone_rejects_invalid_dependency_edge_id(void)
{
    const char payload[] = "payload";
    char asset_id[17];
    char content_id[17];
    char manifest[2048];
    snprintf(asset_id, sizeof(asset_id), "%016llx",
             (unsigned long long)jce_archive_hash_path("data/a.bin"));
    snprintf(content_id, sizeof(content_id), "%016llx",
             (unsigned long long)jce_archive_content_hash(
                 payload, sizeof(payload) - 1));
    snprintf(manifest, sizeof(manifest),
        "{ \"contract\": { \"name\": \"jce.bundle\","
        " \"major\": 1, \"minor\": 1 },"
        " \"id\": \"invalid-edge\", \"version\": 1,"
        " \"build_hash\": \"0000000000000001\","
        " \"target_profile\": \"auto\","
        " \"content_abi\": \"jce-content-1\", \"cook_version\": 0,"
        " \"minimum_engine_version\": \"0.0.0\","
        " \"kind\": \"shared\", \"encrypted\": false,"
        " \"depends_on\": [], \"assets\": [{"
        "  \"path\": \"data/a.bin\", \"size\": 7,"
        "  \"asset_id\": \"%s\", \"content_id\": \"%s\","
        "  \"hash\": \"%s\", \"type\": \"raw\","
        "  \"representation\": \"binary.raw\","
        "  \"dependencies\": [{ \"address\": \"data/b.bin\","
        "    \"asset_id\": \"0000000000000000\","
        "    \"origin\": \"test\" }] }] }",
        asset_id, content_id, content_id);

    JceCookInput inputs[2] = {
        { JCE_BUNDLE_MANIFEST_VPATH, manifest, strlen(manifest) },
        { "data/a.bin", payload, sizeof(payload) - 1 }
    };
    JceCookConfig config;
    memset(&config, 0, sizeof(config));
    config.zstd_level = 1;
    config.emit_debug_paths = true;

    void *archive = NULL;
    size_t archive_size = 0;
    TEST_ASSERT_TRUE(jce_archive_cook(inputs, 2, &config,
                                      &archive, &archive_size, NULL));
    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);
    JceBundleFile *bundle = jce_bundle_file_open_memory(
        fs, archive, archive_size, "invalid-edge");
    TEST_ASSERT_NULL(bundle);
    jce_fs_destroy(fs);
    jce_free(archive);
}

static void test_env_catalog_mounts_scene_closure(void)
{
    const char *path = getenv("JCE_BUNDLE_CATALOG");
    if (!path || !path[0]) {
        TEST_IGNORE_MESSAGE("JCE_BUNDLE_CATALOG unset");
        return;
    }

    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);
    JceBundleCatalog *catalog = jce_bundle_catalog_open(fs, path);
    TEST_ASSERT_NOT_NULL(catalog);

    const char *scene_id = NULL;
    for (uint32_t i = 0; i < jce_bundle_catalog_count(catalog); ++i) {
        const char *id = jce_bundle_catalog_id_at(catalog, i);
        const char *kind = jce_bundle_catalog_kind(catalog, id);
        if (kind && strcmp(kind, "scene") == 0) {
            scene_id = id;
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(scene_id);
    TEST_ASSERT_TRUE(jce_bundle_mount(catalog, scene_id));
    const char *scene_path = jce_bundle_catalog_scene_path(catalog, scene_id);
    TEST_ASSERT_NOT_NULL(scene_path);
    TEST_ASSERT_TRUE(jce_fs_exists(fs, scene_path));
    TEST_ASSERT_TRUE(jce_bundle_unmount(catalog, scene_id));
    jce_bundle_catalog_close(catalog);
    jce_fs_destroy(fs);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_catalog_requires_contract);
    RUN_TEST(test_catalog_rejects_incompatible_content_abi);
    RUN_TEST(test_catalog_rejects_incompatible_target_profile);
    RUN_TEST(test_catalog_accepts_legacy_1_0_without_content_metadata);
    RUN_TEST(test_catalog_rejects_unknown_dependency);
    RUN_TEST(test_catalog_rejects_dependency_cycle);
    RUN_TEST(test_mount_failure_rolls_back_dependency);
    RUN_TEST(test_mount_verifies_size_and_content_hash);
    RUN_TEST(test_mount_rejects_catalog_manifest_identity_mismatch);
    RUN_TEST(test_standalone_sidecar_detects_archive_tampering);
    RUN_TEST(test_standalone_requires_bundle_manifest);
    RUN_TEST(test_standalone_rejects_invalid_dependency_edge_id);
    RUN_TEST(test_env_catalog_mounts_scene_closure);
    return UNITY_END();
}
