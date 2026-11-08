/*
 * test_jce_project_script_modules.c — jce_project.json's "script_modules"
 * survives a load/save round trip.
 *
 * WHAT THIS IS ABOUT.  A C++ script is a class in a shared object the PROJECT
 * builds, and the only place a tool can learn which shared object that is, is
 * this manifest key.  The editor reads it at project-open and loads the module
 * (editor/src/core/jce_editor_script_backends.cpp); with no module loaded,
 * every .jcecpp scriptPath in the scene is refused.
 *
 * THE FAILURE THIS EXISTS FOR IS SILENT AND DESTRUCTIVE.  jce_project_save()
 * REBUILDS the manifest from the in-memory struct rather than patching the
 * file, so a key that is parsed but not written back disappears the first time
 * anything else is saved — and the editor saves this manifest for ordinary
 * reasons (jce_editor_project_update_field, the project dialog's fields).  The
 * symptom would arrive later and somewhere else: a project whose C++ scripts
 * stopped loading, with no edit to any script and nothing in the log about it.
 *
 * So the round trip is asserted in both directions, and the second case is the
 * one that matters: saving after touching an UNRELATED field must not lose it.
 */

#include "unity.h"

#include "jce_test_file_util.h"

#include <jce/application/jce_project.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>

#include <stdio.h>
#include <string.h>

#define TEST_DIR "test_project_script_modules"

static const char *k_manifest =
    "{\n"
    "  \"schema\": 3,\n"
    "  \"name\": \"scripted\",\n"
    "  \"target\": \"Scripted\",\n"
    "  \"source_assets\": \"assets\",\n"
    "  \"startup_scene\": \"scenes/main.scene.json\",\n"
    "  \"script_modules\": [\n"
    "    \"build/release/game_scripts.dll\",\n"
    "    \"D:/prebuilt/vendor_scripts.dll\"\n"
    "  ]\n"
    "}\n";

void setUp(void)
{
    jce_fs_host_create_directory(TEST_DIR);
    jce_test_write_file(TEST_DIR "/jce_project.json", k_manifest);
}

void tearDown(void)
{
    jce_fs_host_remove_recursive(TEST_DIR);
}

static void test_script_modules_are_parsed_in_order(void)
{
    JceProject *p = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL_MESSAGE(p, "the manifest did not load at all");

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, p->script_modules_count,
        "\"script_modules\" was not parsed -- the editor would load no C++ "
        "script module and every .jcecpp scriptPath would be refused in Play");
    /* ORDER IS PART OF THE CONTRACT: modules are loaded in the order given, and
     * a module registers its classes when it loads, so a project that relies on
     * one module's classes existing before another's needs this to be stable. */
    TEST_ASSERT_EQUAL_STRING("build/release/game_scripts.dll",
                             p->script_modules[0]);
    /* Relative and absolute entries both survive verbatim; resolving them is
     * the consumer's job, not the parser's. */
    TEST_ASSERT_EQUAL_STRING("D:/prebuilt/vendor_scripts.dll",
                             p->script_modules[1]);
    jce_project_free(p);
}

/* THE DATA-LOSS CASE.  Nothing in the editor edits this key; it edits OTHER
 * fields and saves the whole manifest.  If save() did not write script_modules
 * back, renaming the startup scene would silently delete the project's C++
 * scripting. */
static void test_saving_after_an_unrelated_edit_keeps_script_modules(void)
{
    JceProject *p = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT(2, p->script_modules_count);

    TEST_ASSERT_TRUE(jce_project_set_field(p, "startup_scene",
                                           "scenes/other.scene.json"));
    TEST_ASSERT_TRUE_MESSAGE(jce_project_save(p), "save failed");
    jce_project_free(p);

    JceProject *q = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL(q);
    TEST_ASSERT_EQUAL_STRING("scenes/other.scene.json", q->startup_scene);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, q->script_modules_count,
        "saving the manifest after editing an unrelated field DROPPED "
        "\"script_modules\" -- the project's C++ scripts stop loading and "
        "nothing says why");
    TEST_ASSERT_EQUAL_STRING("build/release/game_scripts.dll",
                             q->script_modules[0]);
    TEST_ASSERT_EQUAL_STRING("D:/prebuilt/vendor_scripts.dll",
                             q->script_modules[1]);
    jce_project_free(q);
}

/* A manifest with no such key is the NORMAL state — most projects have no C++
 * scripts — and must load as "nothing to do", not as an error and not as a
 * default path that would then be reported missing. */
static void test_a_manifest_without_the_key_declares_no_modules(void)
{
    jce_test_write_file(TEST_DIR "/jce_project.json",
                        "{\n  \"schema\": 2,\n  \"name\": \"plain\"\n}\n");
    JceProject *p = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL_MESSAGE(p, "a manifest without script_modules failed "
                                    "to load -- the key became mandatory");
    TEST_ASSERT_EQUAL_INT(0, p->script_modules_count);
    TEST_ASSERT_NULL(p->script_modules);

    /* And saving one must not INVENT the key either: a project that never had
     * C++ scripts should round-trip without gaining a declaration. */
    TEST_ASSERT_TRUE(jce_project_save(p));
    jce_project_free(p);

    JceProject *q = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL(q);
    TEST_ASSERT_EQUAL_INT(0, q->script_modules_count);
    jce_project_free(q);
}

/* The public saver keeps the single optional editor-private declaration as
 * its original JSON node; it neither interprets nor invents that policy. */
static void test_optional_confidential_node_survives_unrelated_save(void)
{
    const char *manifests[] = {
        "{\"schema\":2,\"name\":\"privacy\",\"confidential_asset_paths\":[\"materials/stock.mat.json\",\"scripts/main.lua\"]}",
        "{\"schema\":2,\"name\":\"privacy\",\"confidential_asset_paths\":[]}",
        "{\"schema\":2,\"name\":\"privacy\",\"confidential_asset_paths\":{\"invalid\":true}}",
        "{\"schema\":2,\"name\":\"privacy\",\"confidential_asset_paths\":[\"scripts/\\\\u0000.lua\"]}",
        "{\"schema\":2,\"name\":\"privacy\"}"
    };
    for (size_t i = 0; i < sizeof(manifests) / sizeof(manifests[0]); ++i) {
        jce_test_write_file(TEST_DIR "/jce_project.json", manifests[i]);
        JceJson *before = jce_json_parse_file(TEST_DIR "/jce_project.json");
        TEST_ASSERT_NOT_NULL(before);
        JceJson *original_node = jce_json_get(before, "confidential_asset_paths");
        char *original = original_node ? jce_json_print(original_node, false) : NULL;
        JceProject *p = jce_project_load(TEST_DIR);
        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_TRUE(jce_project_set_field(p, "name", "renamed"));
        TEST_ASSERT_TRUE(jce_project_save(p));
        jce_project_free(p);
        JceJson *after = jce_json_parse_file(TEST_DIR "/jce_project.json");
        TEST_ASSERT_NOT_NULL(after);
        TEST_ASSERT_EQUAL_STRING("renamed", jce_json_get_string(after, "name", ""));
        JceJson *saved_node = jce_json_get(after, "confidential_asset_paths");
        if (original_node) {
            TEST_ASSERT_NOT_NULL(saved_node);
            char *saved = jce_json_print(saved_node, false);
            TEST_ASSERT_EQUAL_STRING(original, saved);
            jce_json_free_string(saved);
        } else {
            TEST_ASSERT_NULL(saved_node);
        }
        jce_json_free_string(original);
        jce_json_free(before);
        jce_json_free(after);
    }
}

static void test_save_refuses_to_repair_duplicate_confidential_nodes(void)
{
    jce_test_write_file(TEST_DIR "/jce_project.json",
        "{\"schema\":2,\"name\":\"original\","
        "\"confidential_asset_paths\":[],\"confidential_asset_paths\":[\"scripts/main.lua\"]}");
    JceProject *p = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(jce_project_set_field(p, "name", "renamed"));
    TEST_ASSERT_FALSE(jce_project_save(p));
    jce_project_free(p);
    JceJson *unchanged = jce_json_parse_file(TEST_DIR "/jce_project.json");
    TEST_ASSERT_NOT_NULL(unchanged);
    TEST_ASSERT_EQUAL_STRING("original", jce_json_get_string(unchanged, "name", ""));
    int policy_count = 0;
    for (JceJson *node = jce_json_first_child(unchanged); node;
         node = jce_json_next_sibling(node)) {
        const char *key = jce_json_member_key(node);
        if (key && !strcmp(key, "confidential_asset_paths")) ++policy_count;
    }
    TEST_ASSERT_EQUAL_INT(2, policy_count);
    jce_json_free(unchanged);
}

static void assert_lossy_manifest_is_not_rewritten(const char *source, size_t size)
{
    TEST_ASSERT_TRUE(jce_fs_host_write_all(TEST_DIR "/jce_project.json", source, size));
    JceProject *p = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL(p); /* The existing public reader remains lenient. */
    TEST_ASSERT_TRUE(jce_project_set_field(p, "name", "renamed"));
    TEST_ASSERT_FALSE(jce_project_save(p));
    jce_project_free(p);
    uint64_t disk_size = 0;
    void *disk = jce_fs_host_read_all(TEST_DIR "/jce_project.json", &disk_size);
    TEST_ASSERT_NOT_NULL(disk);
    TEST_ASSERT_EQUAL_UINT64(size, disk_size);
    TEST_ASSERT_EQUAL_MEMORY(source, disk, size);
    jce_fs_buffer_free(disk);
}

static void test_save_refuses_lossy_nul_and_non_json_suffix_without_rewriting(void)
{
    static const char encoded[] = "{\"schema\":2,\"name\":\"original\",\"confidential_asset_paths\":[\"scripts/main.lua\\u0000suffix\"]}";
    static const char suffix[] = "{\"schema\":2,\"name\":\"original\",\"confidential_asset_paths\":[]} trailing";
    static const char raw_nul[] = "{\"schema\":2,\"name\":\"original\",\"confidential_asset_paths\":[]}\0suffix";
    assert_lossy_manifest_is_not_rewritten(encoded, sizeof(encoded) - 1);
    assert_lossy_manifest_is_not_rewritten(suffix, sizeof(suffix) - 1);
    assert_lossy_manifest_is_not_rewritten(raw_nul, sizeof(raw_nul) - 1);
}

static void test_sdk_save_preserves_all_unknown_nested_project_metadata(void)
{
    static const char manifest[] =
        "{\"schema\":3,\"name\":\"original\",\"sdk\":\"sdk/old\","
        "\"native_script_factories\":[{\"symbol\":\"factory\",\"options\":{\"flags\":[1,true,null]}}],"
        "\"native_script_refs\":{\"controller\":{\"source\":\"src/controller.c\",\"enabled\":true}},"
        "\"future_settings\":{\"nested\":[{\"path\":\"a\\\\b\",\"number\":1.25},null,false]},"
        "\"script_modules\":[\"native/scripts.dll\"],\"bundles\":[\"dist/main.jbundle\"]}";
    const char *keys[] = {"native_script_factories", "native_script_refs", "future_settings"};
    jce_test_write_file(TEST_DIR "/jce_project.json", manifest);
    JceJson *before = jce_json_parse_file(TEST_DIR "/jce_project.json");
    TEST_ASSERT_NOT_NULL(before);
    JceProject *p = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(jce_project_set_field(p, "sdk_path", "sdk/current"));
    TEST_ASSERT_TRUE(jce_project_save(p));
    JceJson *after = jce_json_parse_file(TEST_DIR "/jce_project.json");
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_STRING("sdk/current", jce_json_get_string(after, "sdk", ""));
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        JceJson *retained = jce_json_get(after, keys[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(retained, "unrelated SDK save dropped authoring metadata");
        char *expected = jce_json_print(jce_json_get(before, keys[i]), false);
        char *actual = jce_json_print(retained, false);
        TEST_ASSERT_NOT_NULL(expected);
        TEST_ASSERT_NOT_NULL(actual);
        TEST_ASSERT_EQUAL_STRING(expected, actual);
        jce_json_free_string(expected);
        jce_json_free_string(actual);
    }
    TEST_ASSERT_EQUAL_STRING("original", jce_json_get_string(after, "name", ""));
    TEST_ASSERT_EQUAL_INT(1, jce_json_array_size(jce_json_get(after, "script_modules")));
    TEST_ASSERT_EQUAL_INT(1, jce_json_array_size(jce_json_get(after, "bundles")));
    jce_json_free(before);
    jce_json_free(after);
    jce_project_free(p);
}

static void test_known_fields_update_and_optional_fields_clear_without_duplicates(void)
{
    JceProject *p = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(jce_project_set_field(p, "name", "renamed"));
    TEST_ASSERT_TRUE(jce_project_set_field(p, "version", "2.0"));
    TEST_ASSERT_TRUE(jce_project_set_field(p, "sdk_path", "sdk/new"));
    TEST_ASSERT_TRUE(jce_project_set_field(p, "source_assets", "content"));
    TEST_ASSERT_TRUE(jce_project_set_field(p, "cooked_assets", "cache/cooked"));
    TEST_ASSERT_TRUE(jce_project_set_field(p, "startup_scene", NULL));
    TEST_ASSERT_TRUE(jce_project_bundle_add(p, "dist/temporary.jbundle"));
    TEST_ASSERT_TRUE(jce_project_save(p));
    TEST_ASSERT_TRUE(jce_project_set_field(p, "sdk_path", NULL));
    TEST_ASSERT_TRUE(jce_project_bundle_set_all(p, NULL, 0));
    TEST_ASSERT_TRUE(jce_project_save(p));
    JceJson *saved = jce_json_parse_file(TEST_DIR "/jce_project.json");
    TEST_ASSERT_NOT_NULL(saved);
    TEST_ASSERT_EQUAL_STRING("renamed", jce_json_get_string(saved, "name", ""));
    TEST_ASSERT_EQUAL_STRING("2.0", jce_json_get_string(saved, "version", ""));
    TEST_ASSERT_EQUAL_STRING("content", jce_json_get_string(saved, "source_assets", ""));
    TEST_ASSERT_EQUAL_STRING("cache/cooked", jce_json_get_string(saved, "cooked_assets", ""));
    TEST_ASSERT_FALSE(jce_json_has(saved, "sdk"));
    TEST_ASSERT_FALSE(jce_json_has(saved, "startup_scene"));
    TEST_ASSERT_FALSE(jce_json_has(saved, "bundles"));
    int names = 0;
    for (JceJson *node = jce_json_first_child(saved); node; node = jce_json_next_sibling(node))
        if (!strcmp(jce_json_member_key(node), "name")) ++names;
    TEST_ASSERT_EQUAL_INT(1, names);
    TEST_ASSERT_EQUAL_INT(2, jce_json_array_size(jce_json_get(saved, "script_modules")));
    jce_json_free(saved);
    jce_project_free(p);
}

static void test_bad_existing_manifest_is_not_treated_as_new(void)
{
    const char *invalid[] = {"", "{\"name\":", "[]", "{\"sdk\":\"one\",\"sdk\":\"two\"}",
                             "{\"extension\":1,\"extension\":2}"};
    JceProject *p = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(jce_project_set_field(p, "sdk_path", "sdk/new"));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        const size_t expected_size = strlen(invalid[i]);
        TEST_ASSERT_TRUE(jce_fs_host_write_all(TEST_DIR "/jce_project.json", invalid[i], expected_size));
        TEST_ASSERT_FALSE_MESSAGE(jce_project_save(p), "bad or ambiguous metadata must not be repaired by Save SDK");
        uint64_t actual_size = 0;
        void *actual = jce_fs_host_read_all(TEST_DIR "/jce_project.json", &actual_size);
        TEST_ASSERT_NOT_NULL(actual);
        TEST_ASSERT_EQUAL_UINT64(expected_size, actual_size);
        if (expected_size) TEST_ASSERT_EQUAL_MEMORY(invalid[i], actual, expected_size);
        jce_fs_buffer_free(actual);
    }
    jce_project_free(p);
}

static void test_atomic_failure_preserves_original_project_file(void)
{
    JceProject *p = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(jce_project_set_field(p, "sdk_path", "sdk/rejected"));
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(TEST_DIR "/jce_project.json.tmp"));
    TEST_ASSERT_FALSE(jce_project_save(p));
    uint64_t size = 0;
    void *actual = jce_fs_host_read_all(TEST_DIR "/jce_project.json", &size);
    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT_EQUAL_UINT64(strlen(k_manifest), size);
    TEST_ASSERT_EQUAL_MEMORY(k_manifest, actual, (size_t)size);
    jce_fs_buffer_free(actual);
    jce_project_free(p);
}

static void test_new_manifest_is_created_without_inventing_unknown_metadata(void)
{
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(TEST_DIR "/jce_project.json"));
    JceProject *p = jce_project_new(TEST_DIR, "new-project");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(jce_project_save(p));
    JceProject *saved = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL(saved);
    TEST_ASSERT_EQUAL_STRING("new-project", saved->name);
    jce_project_free(saved);
    JceJson *root = jce_json_parse_file(TEST_DIR "/jce_project.json");
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_FALSE(jce_json_has(root, "native_script_factories"));
    TEST_ASSERT_FALSE(jce_json_has(root, "future_settings"));
    jce_json_free(root);
    jce_project_free(p);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_script_modules_are_parsed_in_order);
    RUN_TEST(test_saving_after_an_unrelated_edit_keeps_script_modules);
    RUN_TEST(test_a_manifest_without_the_key_declares_no_modules);
    RUN_TEST(test_optional_confidential_node_survives_unrelated_save);
    RUN_TEST(test_save_refuses_to_repair_duplicate_confidential_nodes);
    RUN_TEST(test_save_refuses_lossy_nul_and_non_json_suffix_without_rewriting);
    RUN_TEST(test_sdk_save_preserves_all_unknown_nested_project_metadata);
    RUN_TEST(test_known_fields_update_and_optional_fields_clear_without_duplicates);
    RUN_TEST(test_bad_existing_manifest_is_not_treated_as_new);
    RUN_TEST(test_atomic_failure_preserves_original_project_file);
    RUN_TEST(test_new_manifest_is_created_without_inventing_unknown_metadata);
    return UNITY_END();
}
