/* Real public Bundle/Archive + exact host pack-tool controls; no engine/GPU.
 * Typed selected archives are produced by jce_bundle_pack_run. The unsafe
 * address probe alone uses the preserved v1.0 loader compatibility contract;
 * it proves CLI address admission, not a modern typed dependency graph. */
#include "unity.h"
#include "cli/jce_tool_main.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <jce/renderer/jce_texture.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_archive_writer.h>
#include <jce/resource/jce_bundle_format.h>
#include <jce/resource/jce_bundle_loader.h>
#include <jce/resource/jce_bundle_pack.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef JCE_PAK_SELECTED_FIXTURE_BASE
#error "Fixture must have a build-owned absolute output base"
#endif
#define CAP 1024
static char g_base[CAP], g_root[CAP], g_selected[CAP];
static char g_pak[CAP], g_bom_path[CAP], g_header[CAP], g_manifest[CAP];
static const char *g_program;
static JceArchive *g_ar;
static JceBundleFile *g_bundle;
static JceArchiveWriter *g_writer;
static JceJson *g_bom;
static void *g_blob, *g_work;
static uint8_t *g_pixels;
static uint64_t g_blob_size;
static uint32_t g_imported;
static int g_pack_errors, g_pack_warnings;
static const char k_same[] = "identical logical payload";
/* Existing original regression signal: 1x1 opaque red PNG, RGBA8.
 * PNG has a detectable signature for the public memory image dispatcher. */
static const uint8_t k_png[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,
    0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
    0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,
    0x89,0x00,0x00,0x00,0x0d,0x49,0x44,0x41,
    0x54,0x08,0xd7,0x63,0xf8,0xcf,0xc0,0xf0,
    0x1f,0x00,0x05,0x00,0x01,0xff,0x72,0x9c,
    0x52,0x67,0x00,0x00,0x00,0x00,0x49,0x45,
    0x4e,0x44,0xae,0x42,0x60,0x82
};
static const uint8_t k_key[32] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
};

static void path(char out[CAP], const char *leaf)
{
    TEST_ASSERT_TRUE(jce_path_join(out, CAP, g_root, leaf));
}
static void write_fixture(const char *leaf, const void *data, size_t size)
{
    char file[CAP]; path(file, leaf);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(file, data, size));
}
static void write_text(const char *leaf, const char *text)
{
    write_fixture(leaf, text, strlen(text));
}
void setUp(void)
{
    char desired[CAP];
    TEST_ASSERT_TRUE(jce_path_normalize(g_base, CAP, JCE_PAK_SELECTED_FIXTURE_BASE));
    TEST_ASSERT_TRUE(jce_path_is_absolute(g_base));
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(g_base));
    TEST_ASSERT_TRUE(jce_path_join(desired, CAP, g_base, "case"));
    TEST_ASSERT_TRUE(jce_fs_host_make_unique_path(desired, g_root, CAP));
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(g_root));
    path(g_pak, "final/project.pak"); path(g_bom_path, "final/bom.json");
    path(g_header, "final/embedded.h"); path(g_manifest, "final/assets.cmake");
    g_pack_errors = g_pack_warnings = 0; g_imported = 0; g_selected[0] = '\0';
    jce_archive_set_process_key(NULL);
}
void tearDown(void)
{
    if (g_bundle) jce_bundle_file_close(g_bundle);
    if (g_ar) jce_archive_close(g_ar);
    if (g_writer) jce_archive_writer_destroy(g_writer);
    jce_json_free(g_bom); jce_fs_buffer_free(g_blob);
    jce_free(g_work); jce_free(g_pixels);
    g_bundle = NULL; g_ar = NULL; g_writer = NULL; g_bom = NULL;
    g_blob = NULL; g_work = NULL; g_pixels = NULL; g_blob_size = 0;
    jce_archive_set_process_key(NULL);
    /* Refuse recursive deletion unless this is a direct case child of the
     * build-owned base. No pre-existing directory is removed by setUp. */
    if (g_root[0]) {
        char normalized[CAP];
        TEST_ASSERT_TRUE(jce_path_normalize(normalized, CAP, g_root));
        size_t n = strlen(g_base);
        TEST_ASSERT_TRUE(n > 0 && strncmp(normalized, g_base, n) == 0);
        TEST_ASSERT_EQUAL_CHAR('/', normalized[n]);
        TEST_ASSERT_TRUE(strncmp(normalized + n + 1, "case", 4) == 0);
        TEST_ASSERT_NULL(strchr(normalized + n + 1, '/'));
        TEST_ASSERT_TRUE(jce_fs_host_remove_recursive(normalized));
        g_root[0] = '\0';
    }
}
static void pack_log(JceBundlePackLogLevel level, const char *message, void *user)
{
    (void)message; (void)user;
    if (level == JCE_BUNDLE_PACK_LOG_ERROR) ++g_pack_errors;
    if (level == JCE_BUNDLE_PACK_LOG_WARNING) ++g_pack_warnings;
}
static bool find_bundle(const char *file, bool is_dir, void *user)
{
    size_t n = strlen(file); (void)user;
    if (!is_dir && n >= 8 && strcmp(file + n - 8, ".jbundle") == 0) {
        TEST_ASSERT_EQUAL_CHAR('\0', g_selected[0]);
        TEST_ASSERT_TRUE(jce_path_normalize(g_selected, CAP, file));
    }
    return true;
}
static void make_selected_target(int target, const char *inspection_target)
{
    char project[CAP], scene[CAP], out[CAP];
    write_text("project/selected.scene",
        "{\"entities\":[{\"scriptPath\":\"scripts/looks.lua\"}]}");
    write_text("project/scripts/looks.lua", "return {}\n");
    write_text("project/scripts/looks.lua.deps.json",
        "{\"contract\":{\"name\":\"jce.bundle.dependencies\",\"major\":1,\"minor\":0},"
        "\"assets\":[\"data/a.bin\",\"data/b.bin\",\"textures/a.png\"]}");
    write_fixture("project/data/a.bin", k_same, sizeof k_same - 1);
    write_fixture("project/data/b.bin", k_same, sizeof k_same - 1);
    write_fixture("project/textures/a.png", k_png, sizeof k_png);
    write_text("project/data/unused.bin", "not part of the selected closure");
    path(project, "project"); path(scene, "project/selected.scene");
    path(out, "selected");
    const char *scenes[] = {scene};
    JceBundlePackOptions opts = {0};
    opts.scenes_dir = project; opts.resource_root = project; opts.out_dir = out;
    opts.scene_files = scenes; opts.scene_file_count = 1;
    opts.single_file_mode = true; opts.single_bundle_id = "selected";
    opts.quiet = true; opts.zstd_level = 1;
    opts.cook_assets = true; opts.target_platform = target;
    TEST_ASSERT_EQUAL_INT(0, jce_bundle_pack_run(&opts, pack_log, NULL));
    TEST_ASSERT_EQUAL_INT(0, g_pack_errors);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_pack_warnings, "Cook warnings/raw fallback are not cooked success");
    TEST_ASSERT_TRUE(jce_fs_host_walk(out, find_bundle, NULL));
    TEST_ASSERT_NOT_EQUAL('\0', g_selected[0]);
    g_blob = jce_fs_host_read_all(g_selected, &g_blob_size);
    TEST_ASSERT_NOT_NULL(g_blob);
    /* Exact public consumer validates the actual modern typed manifest. */
    g_bundle = inspection_target
        ? jce_bundle_file_inspect_memory(g_blob, (size_t)g_blob_size, inspection_target)
        : jce_bundle_file_open_memory(NULL, g_blob, (size_t)g_blob_size, NULL);
    TEST_ASSERT_NOT_NULL(g_bundle);
    jce_bundle_file_close(g_bundle); g_bundle = NULL;
    g_ar = jce_archive_open(g_blob, (size_t)g_blob_size);
    TEST_ASSERT_NOT_NULL(g_ar);
    TEST_ASSERT_EQUAL_INT(1, jce_archive_verify_header(g_ar));
    g_imported = jce_archive_count(g_ar) - 1;
    TEST_ASSERT_NULL(jce_archive_find(g_ar, "data/unused.bin"));
    jce_archive_close(g_ar); g_ar = NULL;
    jce_fs_buffer_free(g_blob); g_blob = NULL;
}
static void make_selected(void)
{
    make_selected_target(6, NULL); /* portable RGBA8; retains runtime-load control */
}
static int run_pack_mode(const char *raw_a, const char *raw_b,
                         const char *selected, const char *key_file,
                         bool strip_debug_paths)
{
    char *args[24]; int n = 0;
    args[n++] = "jce_pak";
    if (raw_a) { args[n++] = "--resource-dir"; args[n++] = (char *)raw_a; }
    if (raw_b) { args[n++] = "--resource-dir"; args[n++] = (char *)raw_b; }
    if (selected) { args[n++] = "--selected-archive"; args[n++] = (char *)selected; }
    args[n++] = "--pak-file"; args[n++] = g_pak;
    args[n++] = "--header-file"; args[n++] = g_header;
    args[n++] = "--manifest-file"; args[n++] = g_manifest;
    args[n++] = "--json"; args[n++] = g_bom_path;
    args[n++] = "--quiet";
    if (key_file) { args[n++] = "--encrypt-key-file"; args[n++] = (char *)key_file; }
    if (strip_debug_paths) args[n++] = "--strip-debug-paths";
    args[n] = NULL;
    return jce_tool_main(n, args);
}
static int run_pack(const char *raw_a, const char *raw_b,
                    const char *selected, const char *key_file)
{
    return run_pack_mode(raw_a, raw_b, selected, key_file, false);
}
static int run_selected_pair(const char *second)
{
    char *args[] = {"jce_pak", "--selected-archive", g_selected,
        "--selected-archive", (char *)second, "--pak-file", g_pak,
        "--header-file", g_header, "--manifest-file", g_manifest,
        "--json", g_bom_path, "--quiet"};
    return jce_tool_main((int)(sizeof args / sizeof args[0]), args);
}
static int run_inspect(const char *selected)
{
    char *args[] = {"jce_pak", "--inspect", (char *)selected,
        "--verify", "--json", g_bom_path, "--quiet"};
    return jce_tool_main((int)(sizeof args / sizeof args[0]), args);
}
static int run_target(const char *platform, const char *target, bool inspect)
{
    char *args[24]; int n = 0;
    args[n++] = "jce_pak";
    args[n++] = inspect ? "--inspect" : "--selected-archive";
    args[n++] = g_selected;
    args[n++] = "--platform"; args[n++] = (char *)platform;
    if (target) { args[n++] = "--bundle-target"; args[n++] = (char *)target; }
    if (inspect) args[n++] = "--verify";
    else {
        args[n++] = "--pak-file"; args[n++] = g_pak;
        args[n++] = "--header-file"; args[n++] = g_header;
        args[n++] = "--manifest-file"; args[n++] = g_manifest;
    }
    args[n++] = "--json"; args[n++] = g_bom_path; args[n++] = "--quiet";
    args[n] = NULL;
    return jce_tool_main(n, args);
}
static void sidecar_path(char out[CAP], const char *selected)
{
    int n = snprintf(out, CAP, "%s.json", selected);
    TEST_ASSERT_TRUE(n > 0 && n < CAP);
}
static void make_second_selected(char second[CAP], bool different_payload)
{
    char first[CAP], project[CAP], scene[CAP], out[CAP];
    strcpy(first, g_selected); g_selected[0] = 0;
    write_text("project/other.scene", "{\"entities\":[{\"scriptPath\":\"scripts/looks.lua\"}]}");
    if (different_payload) write_text("project/data/a.bin", "changed selected content");
    path(project, "project"); path(scene, "project/other.scene"); path(out, "selected_second");
    const char *scenes[] = {scene};
    JceBundlePackOptions opts = {0};
    opts.scenes_dir = project; opts.resource_root = project; opts.out_dir = out;
    opts.scene_files = scenes; opts.scene_file_count = 1;
    opts.single_file_mode = true; opts.single_bundle_id = "second";
    opts.quiet = true; opts.zstd_level = 1;
    opts.cook_assets = true; opts.target_platform = 6;
    TEST_ASSERT_EQUAL_INT(0, jce_bundle_pack_run(&opts, pack_log, NULL));
    TEST_ASSERT_EQUAL_INT(0, g_pack_errors); TEST_ASSERT_EQUAL_INT(0, g_pack_warnings);
    TEST_ASSERT_TRUE(jce_fs_host_walk(out, find_bundle, NULL));
    TEST_ASSERT_NOT_EQUAL(0, g_selected[0]); strcpy(second, g_selected); strcpy(g_selected, first);
}
static void open_final(void)
{
    g_blob = jce_fs_host_read_all(g_pak, &g_blob_size);
    TEST_ASSERT_NOT_NULL(g_blob);
    g_ar = jce_archive_open(g_blob, (size_t)g_blob_size);
    TEST_ASSERT_NOT_NULL(g_ar);
    TEST_ASSERT_EQUAL_INT(1, jce_archive_verify_header(g_ar));
    g_bom = jce_json_parse_file(g_bom_path);
    TEST_ASSERT_NOT_NULL(g_bom);
    TEST_ASSERT_EQUAL_STRING("jce.pakbom.v1", jce_json_get_string(g_bom, "schema", ""));
    TEST_ASSERT_TRUE(jce_json_get_bool(g_bom, "sources_verified", false));
    JceJson *totals = jce_json_get(g_bom, "totals");
    TEST_ASSERT_TRUE(jce_json_get_bool(totals, "verify_ran", false));
    TEST_ASSERT_EQUAL_UINT32(jce_archive_count(g_ar), jce_json_get_int(totals, "verified_count", -1));
    TEST_ASSERT_EQUAL_INT(0, jce_json_get_int(totals, "corrupt_count", -1));
    TEST_ASSERT_EQUAL_INT(0, jce_json_get_int(totals, "skipped_count", -1));
    TEST_ASSERT_EQUAL_UINT32(jce_archive_count(g_ar),
        jce_json_array_size(jce_json_get(g_bom, "sources")));
}
static JceJson *source_row(const char *vpath)
{
    JceJson *rows = jce_json_get(g_bom, "sources");
    for (int i = 0; i < jce_json_array_size(rows); ++i) {
        JceJson *row = jce_json_array_at(rows, i);
        if (strcmp(vpath, jce_json_get_string(row, "path", "")) == 0) return row;
    }
    return NULL;
}
static void read_entry(const char *vpath)
{
    const JceArchiveEntry *e = jce_archive_find(g_ar, vpath);
    TEST_ASSERT_NOT_NULL(e);
    jce_free(g_work); g_work = jce_malloc(e->original_size ? e->original_size : 1);
    TEST_ASSERT_NOT_NULL(g_work);
    TEST_ASSERT_EQUAL_UINT32(e->original_size,
        jce_archive_read(g_ar, e, g_work, e->original_size));
    TEST_ASSERT_EQUAL_INT(1, jce_archive_verify_entry(e, g_work, e->original_size));
    JceJson *row = source_row(vpath); TEST_ASSERT_NOT_NULL(row);
    char id[17];
    (void)snprintf(id, sizeof id, "%016llx",
        (unsigned long long)jce_archive_content_hash(g_work, e->original_size));
    TEST_ASSERT_EQUAL_STRING(id, jce_json_get_string(row, "content_id", ""));
    TEST_ASSERT_EQUAL_UINT32(e->original_size, jce_json_get_int(row, "size", -1));
}
static void expect_payload(const char *vpath, const void *expected, size_t size)
{
    read_entry(vpath);
    TEST_ASSERT_EQUAL_UINT32(size, jce_archive_find(g_ar, vpath)->original_size);
    if (size) TEST_ASSERT_EQUAL_MEMORY(expected, g_work, size);
}
static void expect_selected_bom(void)
{
    JceJson *selected = jce_json_get(g_bom, "selected_source");
    TEST_ASSERT_EQUAL_STRING(g_selected, jce_json_get_string(selected, "path", ""));
    TEST_ASSERT_EQUAL_UINT32(g_imported, jce_json_get_int(selected, "imported_count", -1));
    uint64_t size = 0;
    jce_free(g_work); g_work = jce_fs_host_read_all(g_selected, &size);
    TEST_ASSERT_NOT_NULL(g_work);
    char id[17];
    (void)snprintf(id, sizeof id, "%016llx",
        (unsigned long long)jce_archive_content_hash(g_work, (size_t)size));
    TEST_ASSERT_EQUAL_STRING(id, jce_json_get_string(selected, "content_id", ""));
    TEST_ASSERT_EQUAL_UINT64(size, (uint64_t)jce_json_get_number(selected, "size", -1));
    jce_fs_buffer_free(g_work); g_work = NULL;
    TEST_ASSERT_NULL(jce_archive_find(g_ar, JCE_BUNDLE_MANIFEST_VPATH));
}
static void test_old_resource_multi_input_and_empty_payload(void)
{
    char a[CAP], b[CAP]; path(a, "raw_a"); path(b, "raw_b");
    write_text("raw_a/a.txt", "first"); write_text("raw_b/b.txt", "second");
    write_text("raw_a/empty.bin", "");
    TEST_ASSERT_EQUAL_INT(0, run_pack(a, b, NULL, NULL));
    open_final();
    TEST_ASSERT_EQUAL_UINT32(3, jce_archive_count(g_ar));
    expect_payload("a.txt", "first", 5); expect_payload("b.txt", "second", 6);
    expect_payload("empty.bin", "", 0);
    TEST_ASSERT_TRUE(jce_json_is_null(jce_json_get(g_bom, "selected_source")));
    TEST_ASSERT_FALSE(jce_json_get_bool(source_row("a.txt"), "selected", true));
}
static void test_selected_typed_cooked_dynamic_closure(void)
{
    make_selected();
    TEST_ASSERT_EQUAL_INT(0, run_pack(NULL, NULL, g_selected, NULL));
    open_final(); expect_selected_bom();
    TEST_ASSERT_EQUAL_UINT32(g_imported, jce_archive_count(g_ar));
    TEST_ASSERT_NOT_NULL(jce_archive_find(g_ar, "selected.scene"));
    expect_payload("scripts/looks.lua", "return {}\n", 10);
    expect_payload("data/a.bin", k_same, sizeof k_same - 1);
    expect_payload("data/b.bin", k_same, sizeof k_same - 1);
    TEST_ASSERT_NULL(jce_archive_find(g_ar, "data/unused.bin"));
    read_entry("textures/a.png");
    const JceArchiveEntry *texture = jce_archive_find(g_ar, "textures/a.png");
    uint32_t width = 0, height = 0;
    TEST_ASSERT_TRUE(jce_texture_decode_cooked_rgba8(g_work, texture->original_size,
        &g_pixels, &width, &height));
    TEST_ASSERT_EQUAL_UINT32(1, width); TEST_ASSERT_EQUAL_UINT32(1, height);
    const uint8_t red[] = {255,0,0,255}; TEST_ASSERT_EQUAL_MEMORY(red, g_pixels, 4);
}
static void test_equal_overlap_keeps_selected_provenance(void)
{
    char raw[CAP]; make_selected(); path(raw, "stock");
    write_fixture("stock/data/a.bin", k_same, sizeof k_same - 1);
    write_text("stock/stock.bin", "stock");
    TEST_ASSERT_EQUAL_INT(0, run_pack(raw, NULL, g_selected, NULL));
    open_final(); expect_selected_bom();
    TEST_ASSERT_EQUAL_UINT32(g_imported + 1, jce_archive_count(g_ar));
    expect_payload("data/a.bin", k_same, sizeof k_same - 1);
    TEST_ASSERT_TRUE(jce_json_get_bool(source_row("data/a.bin"), "selected", false));
    TEST_ASSERT_EQUAL_STRING(g_selected, jce_json_get_string(source_row("data/a.bin"), "source", ""));
    TEST_ASSERT_FALSE(jce_json_get_bool(source_row("stock.bin"), "selected", true));
}
static void expect_failed_without_replacing_prior(int status)
{
    TEST_ASSERT_NOT_EQUAL(0, status);
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(g_bom_path));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(g_header));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(g_manifest));
    uint64_t size = 0; g_work = jce_fs_host_read_all(g_pak, &size);
    TEST_ASSERT_NOT_NULL(g_work); TEST_ASSERT_EQUAL_UINT64(5, size);
    TEST_ASSERT_EQUAL_MEMORY("prior", g_work, 5);
    jce_fs_buffer_free(g_work); g_work = NULL;
}
static void test_different_selected_overlap_preserves_prior(void)
{
    char raw[CAP]; make_selected(); path(raw, "stock");
    write_text("stock/data/a.bin", "different"); write_fixture("final/project.pak", "prior", 5);
    expect_failed_without_replacing_prior(run_pack(raw, NULL, g_selected, NULL));
}
static void test_different_resource_inputs_preserve_prior(void)
{
    char a[CAP], b[CAP]; path(a, "raw_a"); path(b, "raw_b");
    write_text("raw_a/same.bin", "one"); write_text("raw_b/same.bin", "two");
    write_fixture("final/project.pak", "prior", 5);
    expect_failed_without_replacing_prior(run_pack(a, b, NULL, NULL));
}
static void writer_fixture(const char *leaf, bool manifest, bool secure, const char *vpath)
{
    JceArchiveWriterConfig cfg = {0}; cfg.emit_debug_paths = !secure;
    g_writer = jce_archive_writer_create(&cfg); TEST_ASSERT_NOT_NULL(g_writer);
    if (secure) jce_archive_writer_set_encryption_key(g_writer, k_key);
    const char text[] = "{\"contract\":{\"name\":\"jce.bundle\",\"major\":1,\"minor\":0},"
        "\"id\":\"admission\",\"kind\":\"shared\",\"assets\":[]}";
    if (manifest) {
        bool ok = secure ? jce_archive_writer_add_encrypted(g_writer,
            JCE_BUNDLE_MANIFEST_VPATH, text, sizeof text - 1, -1)
            : jce_archive_writer_add(g_writer, JCE_BUNDLE_MANIFEST_VPATH, text, sizeof text - 1);
        TEST_ASSERT_TRUE(ok);
    }
    TEST_ASSERT_TRUE(secure ? jce_archive_writer_add_encrypted(g_writer, vpath, "data", 4, -1)
        : jce_archive_writer_add(g_writer, vpath, "data", 4));
    size_t size = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(g_writer, &g_work, &size));
    jce_archive_writer_destroy(g_writer); g_writer = NULL;
    write_fixture(leaf, g_work, size); jce_free(g_work); g_work = NULL;
}
static void test_missing_corrupt_plain_and_secure_selected_rejected(void)
{
    char missing[CAP], corrupt[CAP], plain[CAP], secure[CAP];
    make_selected(); path(missing, "missing.jbundle"); path(corrupt, "corrupt.jbundle");
    path(plain, "plain.jbundle"); path(secure, "secure.jbundle");
    g_blob = jce_fs_host_read_all(g_selected, &g_blob_size); TEST_ASSERT_NOT_NULL(g_blob);
    /* Change a real indexed payload byte; header parsing remains intact. */
    g_ar = jce_archive_open(g_blob, (size_t)g_blob_size); TEST_ASSERT_NOT_NULL(g_ar);
    const JceArchiveEntry *entry = jce_archive_find(g_ar, "data/a.bin");
    TEST_ASSERT_NOT_NULL(entry); TEST_ASSERT_TRUE(entry->data_offset < g_blob_size);
    ((uint8_t *)g_blob)[entry->data_offset] ^= 1;
    write_fixture("corrupt.jbundle", g_blob, (size_t)g_blob_size);
    jce_archive_close(g_ar); g_ar = NULL;
    jce_fs_buffer_free(g_blob); g_blob = NULL;
    writer_fixture("plain.jbundle", false, false, "data.bin");
    writer_fixture("secure.jbundle", true, true, "data.bin");
    write_fixture("final/project.pak", "prior", 5);
    expect_failed_without_replacing_prior(run_pack(NULL, NULL, missing, NULL));
    expect_failed_without_replacing_prior(run_pack(NULL, NULL, corrupt, NULL));
    expect_failed_without_replacing_prior(run_pack(NULL, NULL, plain, NULL));
    expect_failed_without_replacing_prior(run_pack(NULL, NULL, secure, NULL));
}
static void test_secure_final_deep_bom_and_content_dedup(void)
{
    char key_file[CAP]; make_selected(); path(key_file, "key.hex");
    write_text("key.hex", "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    TEST_ASSERT_EQUAL_INT(0, run_pack(NULL, NULL, g_selected, key_file));
    jce_archive_set_process_key(k_key); open_final(); expect_selected_bom();
    TEST_ASSERT_TRUE(jce_archive_is_secure(g_ar));
    TEST_ASSERT_EQUAL_INT(JCE_ARCHIVE_AUTH_VALID, jce_archive_auth_status(g_ar));
    JceJson *header = jce_json_get(g_bom, "header");
    TEST_ASSERT_TRUE(jce_json_get_bool(header, "encrypted", false));
    TEST_ASSERT_TRUE(jce_json_get_bool(header, "secure_index", false));
    TEST_ASSERT_TRUE(jce_json_get_bool(header, "auth_verified", false));
    TEST_ASSERT_TRUE(jce_json_get_bool(header, "header_verified", false));
    TEST_ASSERT_FALSE(jce_json_get_bool(header, "has_debug_paths", true));
    expect_payload("data/a.bin", k_same, sizeof k_same - 1);
    expect_payload("data/b.bin", k_same, sizeof k_same - 1);
    TEST_ASSERT_EQUAL_UINT64(jce_archive_find(g_ar, "data/a.bin")->data_offset,
        jce_archive_find(g_ar, "data/b.bin")->data_offset);
    TEST_ASSERT_TRUE(jce_json_get_int(jce_json_get(g_bom, "totals"), "duplicate_groups", 0) > 0);
    TEST_ASSERT_TRUE(jce_json_get_int(jce_json_get(g_bom, "totals"), "duplicate_entries", 0) >= 2);
    TEST_ASSERT_TRUE(jce_json_get_bool(source_row("data/a.bin"), "selected", false));
    TEST_ASSERT_EQUAL_STRING(g_selected, jce_json_get_string(source_row("data/a.bin"), "source", ""));
}
static void make_stripped_stock(void)
{
    char raw[CAP]; path(raw, "stock");
    write_text("stock/shaders/vs_fixture_dx11.bin", "vertex shader bytes");
    write_text("stock/shaders/fs_fixture_dx11.bin", "fragment shader bytes");
    TEST_ASSERT_EQUAL_INT(0, run_pack_mode(raw, NULL, NULL, NULL, true));
    open_final();
}
static void test_plain_stripped_stock_bom_retains_verified_input_addresses(void)
{
    make_stripped_stock();
    TEST_ASSERT_EQUAL_UINT32(2, jce_archive_count(g_ar));
    TEST_ASSERT_FALSE(jce_archive_is_secure(g_ar));
    JceJson *header = jce_json_get(g_bom, "header");
    TEST_ASSERT_TRUE(jce_json_get_bool(header, "header_verified", false));
    TEST_ASSERT_FALSE(jce_json_get_bool(header, "has_debug_paths", true));
    TEST_ASSERT_FALSE(jce_json_get_bool(header, "encrypted", true));
    JceJson *rows = jce_json_get(g_bom, "entries");
    TEST_ASSERT_EQUAL_INT(2, jce_json_array_size(rows));
    for (uint32_t i = 0; i < jce_archive_count(g_ar); ++i) {
        const JceArchiveEntry *entry = jce_archive_get(g_ar, i);
        TEST_ASSERT_NULL(jce_archive_debug_path(g_ar, i));
        JceJson *row = jce_json_array_at(rows, (int)i);
        const char *vpath = jce_json_get_string(row, "path", "");
        TEST_ASSERT_NOT_EQUAL('\0', vpath[0]);
        TEST_ASSERT_EQUAL_PTR(entry, jce_archive_find(g_ar, vpath));
        TEST_ASSERT_NOT_NULL(source_row(vpath));
        TEST_ASSERT_EQUAL_UINT32(entry->original_size,
            jce_json_get_int(row, "original_size", -1));
        char checksum[11];
        (void)snprintf(checksum, sizeof checksum, "0x%08x", (unsigned)entry->content_crc);
        TEST_ASSERT_EQUAL_STRING(checksum, jce_json_get_string(row, "content_crc", ""));
        TEST_ASSERT_TRUE(jce_json_get_bool(row, "verified", false));
    }
    expect_payload("shaders/vs_fixture_dx11.bin", "vertex shader bytes", 19);
    expect_payload("shaders/fs_fixture_dx11.bin", "fragment shader bytes", 21);
}
static void test_inspect_stripped_stock_does_not_invent_source_addresses(void)
{
    make_stripped_stock();
    jce_json_free(g_bom); g_bom = NULL;
    char *args[] = {"jce_pak", "--inspect", g_pak, "--json", g_bom_path,
                    "--verify", "--quiet"};
    TEST_ASSERT_EQUAL_INT(0, jce_tool_main((int)(sizeof args / sizeof args[0]), args));
    g_bom = jce_json_parse_file(g_bom_path); TEST_ASSERT_NOT_NULL(g_bom);
    TEST_ASSERT_FALSE(jce_json_get_bool(g_bom, "sources_verified", true));
    TEST_ASSERT_EQUAL_INT(0, jce_json_array_size(jce_json_get(g_bom, "sources")));
    JceJson *header = jce_json_get(g_bom, "header");
    TEST_ASSERT_TRUE(jce_json_get_bool(header, "header_verified", false));
    TEST_ASSERT_FALSE(jce_json_get_bool(header, "has_debug_paths", true));
    JceJson *rows = jce_json_get(g_bom, "entries");
    TEST_ASSERT_EQUAL_INT(2, jce_json_array_size(rows));
    for (int i = 0; i < jce_json_array_size(rows); ++i) {
        JceJson *row = jce_json_array_at(rows, i);
        TEST_ASSERT_TRUE(jce_json_is_null(jce_json_get(row, "path")));
        TEST_ASSERT_TRUE(jce_json_get_bool(row, "verified", false));
        TEST_ASSERT_NULL(jce_archive_debug_path(g_ar, (uint32_t)i));
    }
    JceJson *totals = jce_json_get(g_bom, "totals");
    TEST_ASSERT_TRUE(jce_json_get_bool(totals, "verify_ran", false));
    TEST_ASSERT_EQUAL_INT(2, jce_json_get_int(totals, "verified_count", -1));
    TEST_ASSERT_EQUAL_INT(0, jce_json_get_int(totals, "corrupt_count", -1));
    TEST_ASSERT_EQUAL_INT(0, jce_json_get_int(totals, "skipped_count", -1));
}
static void test_legacy_bundle_unsafe_address_rejected_by_cli(void)
{
    char unsafe[CAP]; path(unsafe, "unsafe.jbundle");
    /* Writer normalization preserves an interior colon. A legacy Bundle
     * consumer admits this archive, so refusal exercises the CLI boundary. */
    writer_fixture("unsafe.jbundle", true, false, "bad:asset.bin");
    g_blob = jce_fs_host_read_all(unsafe, &g_blob_size); TEST_ASSERT_NOT_NULL(g_blob);
    g_bundle = jce_bundle_file_open_memory(NULL, g_blob, (size_t)g_blob_size, NULL);
    TEST_ASSERT_NOT_NULL(g_bundle);
    jce_bundle_file_close(g_bundle); g_bundle = NULL;
    jce_fs_buffer_free(g_blob); g_blob = NULL;
    write_fixture("final/project.pak", "prior", 5);
    expect_failed_without_replacing_prior(run_pack(NULL, NULL, unsafe, NULL));
}
static void expect_bundle_inspection(bool present, bool verified, int minor)
{
    g_bom = jce_json_parse_file(g_bom_path); TEST_ASSERT_NOT_NULL(g_bom);
    TEST_ASSERT_EQUAL_STRING("jce.pakbom.v1", jce_json_get_string(g_bom, "schema", ""));
    JceJson *bundle = jce_json_get(g_bom, "bundle");
    TEST_ASSERT_TRUE(jce_json_get_bool(bundle, "verified", false));
    JceJson *manifest = jce_json_get(bundle, "manifest");
    TEST_ASSERT_TRUE(jce_json_is_object(manifest));
    TEST_ASSERT_EQUAL_INT(minor, jce_json_get_int(jce_json_get(manifest, "contract"), "minor", -1));
    JceJson *sidecar = jce_json_get(bundle, "sidecar");
    TEST_ASSERT_EQUAL_INT(present, jce_json_get_bool(sidecar, "present", !present));
    TEST_ASSERT_EQUAL_INT(present, jce_json_get_bool(sidecar, "compatible", !present));
    TEST_ASSERT_EQUAL_INT(verified, jce_json_get_bool(sidecar, "verified", !verified));
    JceJson *totals = jce_json_get(g_bom, "totals");
    TEST_ASSERT_TRUE(jce_json_get_bool(totals, "verify_ran", false));
    TEST_ASSERT_EQUAL_INT(jce_json_get_int(totals, "entries", -1),
                          jce_json_get_int(totals, "verified_count", -2));
    TEST_ASSERT_EQUAL_INT(0, jce_json_get_int(totals, "corrupt_count", -1));
    TEST_ASSERT_EQUAL_INT(0, jce_json_get_int(totals, "skipped_count", -1));
}
static void test_inspect_actual_typed_manifest_and_sidecar_identity(void)
{
    make_selected(); TEST_ASSERT_EQUAL_INT(0, run_inspect(g_selected));
    expect_bundle_inspection(true, true, 1);
    JceJson *bundle = jce_json_get(g_bom, "bundle");
    JceJson *manifest = jce_json_get(bundle, "manifest");
    TEST_ASSERT_EQUAL_STRING("scene", jce_json_get_string(manifest, "kind", ""));
    g_blob = jce_fs_host_read_all(g_selected, &g_blob_size); TEST_ASSERT_NOT_NULL(g_blob);
    g_ar = jce_archive_open(g_blob, (size_t)g_blob_size); TEST_ASSERT_NOT_NULL(g_ar);
    char id[17]; snprintf(id, sizeof id, "%016llx",
        (unsigned long long)jce_archive_content_hash(g_blob, (size_t)g_blob_size));
    TEST_ASSERT_EQUAL_STRING(id, jce_json_get_string(bundle, "content_id", ""));
    const JceArchiveEntry *entry = jce_archive_find(g_ar, JCE_BUNDLE_MANIFEST_VPATH);
    TEST_ASSERT_NOT_NULL(entry);
    g_work = jce_malloc(entry->original_size); TEST_ASSERT_NOT_NULL(g_work);
    TEST_ASSERT_EQUAL_UINT32(entry->original_size, jce_archive_read(g_ar, entry, g_work, entry->original_size));
    JceJson *actual = jce_json_parse(g_work, entry->original_size); TEST_ASSERT_NOT_NULL(actual);
    char *expected = jce_json_print(actual, false), *reported = jce_json_print(manifest, false);
    TEST_ASSERT_NOT_NULL(expected); TEST_ASSERT_NOT_NULL(reported);
    TEST_ASSERT_EQUAL_STRING(expected, reported);
    jce_json_free_string(expected); jce_json_free_string(reported); jce_json_free(actual);
}
static void test_absent_sidecar_is_valid_single_file_delivery(void)
{
    char sidecar[CAP]; make_selected(); sidecar_path(sidecar, g_selected);
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(sidecar));
    TEST_ASSERT_EQUAL_INT(0, run_inspect(g_selected)); expect_bundle_inspection(false, false, 1);
    jce_json_free(g_bom); g_bom = NULL;
    TEST_ASSERT_EQUAL_INT(0, run_pack(NULL, NULL, g_selected, NULL));
    open_final(); expect_selected_bom();
}
static void test_present_bad_sidecar_rejects_inspect_and_pack(void)
{
    char sidecar[CAP]; make_selected(); sidecar_path(sidecar, g_selected);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(sidecar, "{bad", 4));
    write_fixture("final/bom.json", "prior", 5);
    TEST_ASSERT_NOT_EQUAL(0, run_inspect(g_selected));
    uint64_t size = 0; g_work = jce_fs_host_read_all(g_bom_path, &size);
    TEST_ASSERT_NOT_NULL(g_work); TEST_ASSERT_EQUAL_UINT64(5, size); TEST_ASSERT_EQUAL_MEMORY("prior", g_work, 5);
    jce_fs_buffer_free(g_work); g_work = NULL;
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(g_bom_path));
    write_fixture("final/project.pak", "prior", 5);
    expect_failed_without_replacing_prior(run_pack(NULL, NULL, g_selected, NULL));
    /* A syntactically valid sidecar belonging to another immutable Bundle is
     * still invalid for the first file; do not turn hash mismatch into absence. */
    char second[CAP], second_sidecar[CAP]; make_second_selected(second, false);
    sidecar_path(second_sidecar, second);
    g_work = jce_fs_host_read_all(second_sidecar, &size); TEST_ASSERT_NOT_NULL(g_work);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(sidecar, g_work, size));
    jce_fs_buffer_free(g_work); g_work = NULL;
    TEST_ASSERT_NOT_EQUAL(0, run_inspect(g_selected));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(g_bom_path));
    expect_failed_without_replacing_prior(run_pack(NULL, NULL, g_selected, NULL));
}
static void test_legacy_sidecar_is_compatible_but_not_verified(void)
{
    char selected[CAP], sidecar[CAP]; path(selected, "legacy.jbundle");
    writer_fixture("legacy.jbundle", true, false, "data.bin");
    sidecar_path(sidecar, selected);
    const char legacy[] = "{\"contract\":{\"name\":\"jce.bundle\",\"major\":1,\"minor\":0}}";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(sidecar, legacy, sizeof legacy - 1));
    TEST_ASSERT_EQUAL_INT(0, run_inspect(selected)); expect_bundle_inspection(true, false, 0);
    jce_json_free(g_bom); g_bom = NULL;
    TEST_ASSERT_EQUAL_INT(0, run_pack(NULL, NULL, selected, NULL)); open_final();
    expect_payload("data.bin", "data", 4);
}
static void test_multiple_selected_equal_overlap_has_union_and_provenance(void)
{
    char second[CAP]; make_selected(); make_second_selected(second, false);
    TEST_ASSERT_EQUAL_INT(0, run_selected_pair(second)); open_final();
    TEST_ASSERT_EQUAL_UINT32(g_imported + 1, jce_archive_count(g_ar));
    TEST_ASSERT_NOT_NULL(jce_archive_find(g_ar, "selected.scene"));
    TEST_ASSERT_NOT_NULL(jce_archive_find(g_ar, "other.scene"));
    TEST_ASSERT_NULL(jce_archive_find(g_ar, JCE_BUNDLE_MANIFEST_VPATH));
    expect_payload("data/a.bin", k_same, sizeof k_same - 1);
    TEST_ASSERT_EQUAL_STRING(g_selected, jce_json_get_string(source_row("data/a.bin"), "source", ""));
    TEST_ASSERT_EQUAL_STRING(second, jce_json_get_string(source_row("other.scene"), "source", ""));
    TEST_ASSERT_TRUE(jce_json_is_null(jce_json_get(g_bom, "selected_source")));
    JceJson *rows = jce_json_get(g_bom, "selected_sources"); TEST_ASSERT_EQUAL_INT(2, jce_json_array_size(rows));
    for (int i = 0; i < 2; ++i) {
        JceJson *row = jce_json_array_at(rows, i); const char *file = i ? second : g_selected;
        TEST_ASSERT_EQUAL_STRING(file, jce_json_get_string(row, "path", ""));
        TEST_ASSERT_EQUAL_UINT32(g_imported, jce_json_get_int(row, "imported_count", -1));
        uint64_t size = 0; jce_free(g_work); g_work = jce_fs_host_read_all(file, &size);
        TEST_ASSERT_NOT_NULL(g_work);
        char id[17]; snprintf(id, sizeof id, "%016llx",
            (unsigned long long)jce_archive_content_hash(g_work, (size_t)size));
        TEST_ASSERT_EQUAL_STRING(id, jce_json_get_string(row, "content_id", ""));
        TEST_ASSERT_EQUAL_UINT64(size, (uint64_t)jce_json_get_number(row, "size", -1));
        jce_fs_buffer_free(g_work); g_work = NULL;
    }
}
static void test_multiple_selected_different_overlap_preserves_prior(void)
{
    char second[CAP]; make_selected(); make_second_selected(second, true);
    write_fixture("final/project.pak", "prior", 5);
    expect_failed_without_replacing_prior(run_selected_pair(second));
}
static void test_web_selected_pack_and_inspect_bind_explicit_target(void)
{
    make_selected_target(5, "web");
    TEST_ASSERT_EQUAL_INT(0, run_target("web", NULL, true)); /* --platform web is exact */
    expect_bundle_inspection(true, true, 1);
    TEST_ASSERT_EQUAL_STRING("web", jce_json_get_string(
        jce_json_get(jce_json_get(g_bom, "bundle"), "manifest"), JCE_BUNDLE_KEY_TARGET_PROFILE, ""));
    jce_json_free(g_bom); g_bom = NULL;
    TEST_ASSERT_EQUAL_INT(0, run_target("web", "web", false));
    open_final(); expect_selected_bom();
    expect_payload("data/a.bin", k_same, sizeof k_same - 1);
}
static void test_wrong_exact_target_preserves_prior_and_absent_sidecar_remains_valid(void)
{
    char sidecar[CAP]; make_selected_target(5, "web");
    write_fixture("final/project.pak", "prior", 5);
    expect_failed_without_replacing_prior(run_target("desktop", "windows", false));
    TEST_ASSERT_NOT_EQUAL(0, run_target("desktop", "linux", true));
    TEST_ASSERT_NOT_EQUAL(0, run_target("web", "auto", true)); /* auto is not wildcard */
    sidecar_path(sidecar, g_selected); TEST_ASSERT_TRUE(jce_fs_host_remove_file(sidecar));
    TEST_ASSERT_EQUAL_INT(0, run_target("web", "web", true));
    expect_bundle_inspection(false, false, 1);
}
static void test_inspection_tokens_do_not_change_runtime_target_or_mount_filesystem(void)
{
    make_selected_target(5, "web");
    g_blob = jce_fs_host_read_all(g_selected, &g_blob_size); TEST_ASSERT_NOT_NULL(g_blob);
    TEST_ASSERT_NULL(jce_bundle_file_inspect_memory(g_blob, (size_t)g_blob_size, NULL));
    TEST_ASSERT_NULL(jce_bundle_file_inspect_memory(g_blob, (size_t)g_blob_size, "desktop"));
    TEST_ASSERT_NULL(jce_bundle_file_inspect_file(g_selected, "WEB"));
    TEST_ASSERT_NULL(jce_bundle_file_inspect_file(g_selected, "windows"));
    g_bundle = jce_bundle_file_inspect_memory(g_blob, (size_t)g_blob_size, "web");
    TEST_ASSERT_NOT_NULL(g_bundle);
    JceFileSystem *fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_FALSE(jce_fs_exists(fs, "selected.scene"));
    jce_bundle_file_close(g_bundle); g_bundle = NULL;
#if !JCE_PLATFORM_WEB
    TEST_ASSERT_NULL(jce_bundle_file_open_memory(fs, g_blob, (size_t)g_blob_size, "wrong-host"));
    TEST_ASSERT_NULL(jce_bundle_file_open(fs, g_selected, "wrong-host"));
    TEST_ASSERT_FALSE(jce_fs_exists(fs, "selected.scene"));
#endif
    jce_fs_destroy(fs);
}
static void test_web_inspection_keeps_sidecar_target_and_hash_validation(void)
{
    char sidecar[CAP]; make_selected_target(5, "web"); sidecar_path(sidecar, g_selected);
    g_bom = jce_json_parse_file(sidecar); TEST_ASSERT_NOT_NULL(g_bom);
    jce_json_remove(g_bom, JCE_BUNDLE_KEY_TARGET_PROFILE);
    jce_json_set_string(g_bom, JCE_BUNDLE_KEY_TARGET_PROFILE, "windows");
    char *text = jce_json_print(g_bom, false); TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(sidecar, text, strlen(text)));
    jce_json_free_string(text);
    TEST_ASSERT_NULL(jce_bundle_file_inspect_file(g_selected, "web"));
    TEST_ASSERT_NOT_EQUAL(0, run_target("web", "web", true));
    jce_json_remove(g_bom, JCE_BUNDLE_KEY_TARGET_PROFILE);
    jce_json_set_string(g_bom, JCE_BUNDLE_KEY_TARGET_PROFILE, "web");
    jce_json_remove(g_bom, JCE_BUNDLE_KEY_CONTENT_HASH);
    jce_json_set_string(g_bom, JCE_BUNDLE_KEY_CONTENT_HASH, "0000000000000000");
    text = jce_json_print(g_bom, false); TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(sidecar, text, strlen(text)));
    jce_json_free_string(text);
    TEST_ASSERT_NULL(jce_bundle_file_inspect_file(g_selected, "web"));
    TEST_ASSERT_NOT_EQUAL(0, run_target("web", "web", false));
}
static void test_cli_platform_and_exact_target_disagreement_is_ordinary_refusal(void)
{
    const char *categories[] = {"web", "mobile", "desktop"};
    const char *targets[] = {"windows", "windows", "web"};
    make_selected();
    for (size_t i = 0; i < 3; ++i) {
        char command[2304], evidence[4096] = {0}; size_t retained = 0;
        int written = snprintf(command, sizeof(command),
            "--child-cli --inspect \"%s\" --platform %s --bundle-target %s --quiet",
            g_selected, categories[i], targets[i]);
        TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof(command));
        JceProcessConfig cfg = {0}; cfg.executable_path = g_program;
        cfg.arguments = command; cfg.capture_stdout = cfg.capture_stderr = true;
        JceProcess *child = jce_process_spawn(&cfg); TEST_ASSERT_NOT_NULL(child);
        uint64_t start = jce_time_ticks_ms(); int exit_code = -1; bool exited = false;
        while (jce_time_ticks_ms() - start < 3000) {
            char bytes[256]; size_t got = jce_process_read_stderr(child, bytes, sizeof(bytes));
            size_t keep = got < sizeof(evidence) - 1 - retained ? got : sizeof(evidence) - 1 - retained;
            memcpy(evidence + retained, bytes, keep); retained += keep;
            (void)jce_process_read_stdout(child, bytes, sizeof(bytes));
            if (jce_process_poll_exit(child, &exit_code)) { exited = true; break; }
            jce_thread_sleep_ms(1);
        }
        if (exited) {
            char bytes[256]; size_t got;
            while ((got = jce_process_read_stderr(child, bytes, sizeof(bytes))) != 0) {
                size_t keep = got < sizeof(evidence) - 1 - retained ? got : sizeof(evidence) - 1 - retained;
                memcpy(evidence + retained, bytes, keep); retained += keep;
                if (jce_time_ticks_ms() - start >= 3000) break;
            }
        } else (void)jce_process_force_kill(child);
        jce_process_destroy(child);
        TEST_ASSERT_TRUE_MESSAGE(exited, "CLI target disagreement must exit normally, not timeout");
        TEST_ASSERT_EQUAL_INT(1, exit_code);
        TEST_ASSERT_NOT_NULL(strstr(evidence, "--platform and exact --bundle-target disagree"));
    }
    TEST_ASSERT_NOT_EQUAL(0, run_target("mobile", NULL, false));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(g_pak));
}

/* The self-child executes the complete real CLI even when argument admission
 * exits. Ordinary child refusal therefore remains a Unity assertion result. */
static int run_read_cli(const char *archive, const char *entry, const char *out,
                        const char *key, char evidence[4096])
{
    char command[4096]; size_t retained = 0;
    int n = snprintf(command, sizeof command,
        "--child-cli --inspect \"%s\" --read-entry \"%s\" --out \"%s\" --quiet%s%s%s",
        archive, entry, out, key ? " --key-file \"" : "", key ? key : "", key ? "\"" : "");
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof command);
    JceProcessConfig cfg = {0}; cfg.executable_path = g_program;
    cfg.arguments = command; cfg.capture_stdout = cfg.capture_stderr = true;
    JceProcess *child = jce_process_spawn(&cfg); TEST_ASSERT_NOT_NULL(child);
    uint64_t start = jce_time_ticks_ms(); int code = -1; bool exited = false;
    evidence[0] = '\0';
    do {
        char bytes[256];
        for (int stream = 0; stream < 2; ++stream) {
            size_t got = stream ? jce_process_read_stderr(child, bytes, sizeof bytes)
                                : jce_process_read_stdout(child, bytes, sizeof bytes);
            size_t keep = got < 4095 - retained ? got : 4095 - retained;
            memcpy(evidence + retained, bytes, keep); retained += keep;
        }
        if (jce_process_poll_exit(child, &code)) { exited = true; break; }
        jce_thread_sleep_ms(1);
    } while (jce_time_ticks_ms() - start < 5000);
    if (!exited) (void)jce_process_force_kill(child);
    jce_process_destroy(child); evidence[retained] = '\0';
    TEST_ASSERT_TRUE_MESSAGE(exited, "single-entry CLI must retire naturally");
    return code;
}
static void make_read_archive(const void *payload, size_t size, bool secure,
                              bool wrong_crc, bool debug_paths)
{
    JceArchiveWriterConfig cfg = {0}; cfg.emit_debug_paths = debug_paths;
    g_writer = jce_archive_writer_create(&cfg); TEST_ASSERT_NOT_NULL(g_writer);
    if (secure) {
        jce_archive_writer_set_encryption_key(g_writer, k_key);
        TEST_ASSERT_TRUE(jce_archive_writer_add_encrypted(g_writer, "data/payload.bin", payload, size, -1));
    } else if (wrong_crc) {
        JceArchiveBlob blob = {0}; blob.bytes = (uint8_t *)payload;
        blob.stored_size = blob.original_size = (uint32_t)size;
        blob.content_crc = 1; blob.dict_id = UINT16_MAX;
        TEST_ASSERT_TRUE(jce_archive_writer_add_precompressed(g_writer, "data/payload.bin", &blob));
    } else TEST_ASSERT_TRUE(jce_archive_writer_add(g_writer, "data/payload.bin", payload, size));
    size_t bytes = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(g_writer, &g_work, &bytes));
    jce_archive_writer_destroy(g_writer); g_writer = NULL;
    write_fixture("read.pak", g_work, bytes); jce_free(g_work); g_work = NULL;
}
static void expect_read_bytes(const char *file, const void *bytes, size_t count)
{
    uint64_t size = UINT64_MAX; g_work = jce_fs_host_read_all(file, &size);
    TEST_ASSERT_NOT_NULL(g_work); TEST_ASSERT_EQUAL_UINT64(count, size);
    if (count) TEST_ASSERT_EQUAL_MEMORY(bytes, g_work, count);
    jce_fs_buffer_free(g_work); g_work = NULL;
}
static void test_read_entry_compressed_stripped_exact_bytes(void)
{
    char archive[CAP], out[CAP], evidence[4096];
    const size_t size = 65536; g_pixels = jce_malloc(size); TEST_ASSERT_NOT_NULL(g_pixels);
    memset(g_pixels, 'q', size); memcpy(g_pixels, "single-entry-sensitive-payload", 30);
    make_read_archive(g_pixels, size, false, false, false);
    path(archive, "read.pak"); path(out, "read-output/payload.bin");
    g_ar = jce_archive_open_file(archive); TEST_ASSERT_NOT_NULL(g_ar);
    const JceArchiveEntry *entry = jce_archive_find(g_ar, "data/payload.bin");
    TEST_ASSERT_NOT_NULL(entry); TEST_ASSERT_TRUE(entry->compression != 0);
    jce_archive_close(g_ar); g_ar = NULL;
    TEST_ASSERT_EQUAL_INT(0, run_read_cli(archive, "data/payload.bin", out, NULL, evidence));
    TEST_ASSERT_NULL(strstr(evidence, "single-entry-sensitive-payload"));
    expect_read_bytes(out, g_pixels, size);
}
static void test_read_entry_present_empty_is_not_missing(void)
{
    char archive[CAP], out[CAP], evidence[4096];
    make_read_archive("", 0, false, false, true);
    path(archive, "read.pak"); path(out, "empty.bin");
    TEST_ASSERT_EQUAL_INT(0, run_read_cli(archive, "data/payload.bin", out, NULL, evidence));
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(out));
    expect_read_bytes(out, "", 0);
}
static void test_read_entry_typed_bundle_retains_public_admission(void)
{
    char out[CAP], evidence[4096]; make_selected(); path(out, "selected-scene.json");
    TEST_ASSERT_EQUAL_INT(0, run_read_cli(g_selected, "selected.scene", out, NULL, evidence));
    g_blob = jce_fs_host_read_all(g_selected, &g_blob_size); TEST_ASSERT_NOT_NULL(g_blob);
    g_ar = jce_archive_open(g_blob, (size_t)g_blob_size); TEST_ASSERT_NOT_NULL(g_ar);
    const JceArchiveEntry *entry = jce_archive_find(g_ar, "selected.scene"); TEST_ASSERT_NOT_NULL(entry);
    g_pixels = jce_malloc(entry->original_size); TEST_ASSERT_NOT_NULL(g_pixels);
    TEST_ASSERT_EQUAL_UINT32(entry->original_size,
        jce_archive_read(g_ar, entry, g_pixels, entry->original_size));
    expect_read_bytes(out, g_pixels, entry->original_size);
}
static void test_read_entry_secure_key_admission_and_exact_payload(void)
{
    char archive[CAP], out[CAP], key[CAP], wrong[CAP], evidence[4096];
    make_read_archive(k_same, sizeof k_same - 1, true, false, false);
    path(archive, "read.pak"); path(out, "secure.bin"); path(key, "key.hex"); path(wrong, "wrong.hex");
    write_text("key.hex", "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    write_text("wrong.hex", "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    TEST_ASSERT_NOT_EQUAL(0, run_read_cli(archive, "data/payload.bin", out, NULL, evidence));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(out));
    TEST_ASSERT_NOT_EQUAL(0, run_read_cli(archive, "data/payload.bin", out, wrong, evidence));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(out));
    TEST_ASSERT_EQUAL_INT(0, run_read_cli(archive, "data/payload.bin", out, key, evidence));
    TEST_ASSERT_NULL(strstr(evidence, "0001020304050607"));
    expect_read_bytes(out, k_same, sizeof k_same - 1);
}
static void test_read_entry_missing_and_unsafe_paths_do_not_publish(void)
{
    char archive[CAP], out[CAP], evidence[4096];
    make_read_archive(k_same, sizeof k_same - 1, false, false, true);
    path(archive, "read.pak"); path(out, "refused.bin");
    const char *paths[] = {"data/missing.bin", "../data/payload.bin", "/data/payload.bin",
        "data/./payload.bin", "data//payload.bin", "data\\payload.bin", "Data/payload.bin", "c:/data/payload.bin"};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
        TEST_ASSERT_NOT_EQUAL(0, run_read_cli(archive, paths[i], out, NULL, evidence));
        TEST_ASSERT_FALSE(jce_fs_host_exists_file(out));
    }
}
static void test_read_entry_crc_mismatch_with_valid_archive_header_refuses(void)
{
    char archive[CAP], out[CAP], evidence[4096];
    make_read_archive(k_same, sizeof k_same - 1, false, true, true);
    path(archive, "read.pak"); path(out, "corrupt.bin");
    g_ar = jce_archive_open_file(archive); TEST_ASSERT_NOT_NULL(g_ar);
    TEST_ASSERT_EQUAL_INT(1, jce_archive_verify_header(g_ar));
    jce_archive_close(g_ar); g_ar = NULL;
    TEST_ASSERT_NOT_EQUAL(0, run_read_cli(archive, "data/payload.bin", out, NULL, evidence));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(out));
}
static void test_read_entry_over_16_mib_refuses_before_output(void)
{
    char archive[CAP], out[CAP], evidence[4096]; size_t size = 16u * 1024u * 1024u + 1u;
    g_pixels = jce_malloc(size); TEST_ASSERT_NOT_NULL(g_pixels); memset(g_pixels, 'x', size);
    make_read_archive(g_pixels, size, false, false, false);
    path(archive, "read.pak"); path(out, "too-large.bin");
    TEST_ASSERT_NOT_EQUAL(0, run_read_cli(archive, "data/payload.bin", out, NULL, evidence));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(out));
}
static void test_read_entry_existing_file_and_directory_are_protected(void)
{
    char archive[CAP], out[CAP], directory[CAP], evidence[4096];
    make_read_archive(k_same, sizeof k_same - 1, false, false, true);
    path(archive, "read.pak"); path(out, "prior.bin"); path(directory, "prior-directory");
    write_text("prior.bin", "prior"); TEST_ASSERT_TRUE(jce_fs_host_create_directory(directory));
    TEST_ASSERT_NOT_EQUAL(0, run_read_cli(archive, "data/payload.bin", out, NULL, evidence));
    expect_read_bytes(out, "prior", 5);
    TEST_ASSERT_NOT_EQUAL(0, run_read_cli(archive, "data/payload.bin", directory, NULL, evidence));
    TEST_ASSERT_TRUE(jce_fs_host_exists_dir(directory));
}

static void test_read_entry_ambiguous_zero_codec_refuses(void)
{
    char archive[CAP], out[CAP], evidence[4096];
    g_writer = jce_archive_writer_create(NULL); TEST_ASSERT_NOT_NULL(g_writer);
    JceArchiveBlob entry = {0};
    TEST_ASSERT_TRUE(jce_archive_compress_resource(g_writer, "", 0, -1, &entry));
    jce_free(entry.bytes);
    uint8_t encoded = 0;
    entry.bytes = &encoded; entry.stored_size = 1; entry.original_size = 0;
    entry.compression = JCE_ARCHIVE_COMP_LZ4;
    TEST_ASSERT_TRUE(jce_archive_writer_add_precompressed(g_writer, "data/payload.bin", &entry));
    size_t bytes = 0; TEST_ASSERT_TRUE(jce_archive_writer_finish(g_writer, &g_work, &bytes));
    jce_archive_writer_destroy(g_writer); g_writer = NULL;
    write_fixture("read.pak", g_work, bytes); jce_free(g_work); g_work = NULL;
    path(archive, "read.pak"); path(out, "ambiguous-empty.bin");
    g_ar = jce_archive_open_file(archive); TEST_ASSERT_NOT_NULL(g_ar);
    TEST_ASSERT_EQUAL_INT(1, jce_archive_verify_header(g_ar));
    jce_archive_close(g_ar); g_ar = NULL;
    TEST_ASSERT_NOT_EQUAL(0, run_read_cli(archive, "data/payload.bin", out, NULL, evidence));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(out));
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--child-cli") == 0)
        return jce_tool_main(argc - 1, argv + 1);
    g_program = argv[0];
    UNITY_BEGIN();
    RUN_TEST(test_old_resource_multi_input_and_empty_payload);
    RUN_TEST(test_selected_typed_cooked_dynamic_closure);
    RUN_TEST(test_equal_overlap_keeps_selected_provenance);
    RUN_TEST(test_different_selected_overlap_preserves_prior);
    RUN_TEST(test_different_resource_inputs_preserve_prior);
    RUN_TEST(test_missing_corrupt_plain_and_secure_selected_rejected);
    RUN_TEST(test_secure_final_deep_bom_and_content_dedup);
    RUN_TEST(test_plain_stripped_stock_bom_retains_verified_input_addresses);
    RUN_TEST(test_inspect_stripped_stock_does_not_invent_source_addresses);
    RUN_TEST(test_legacy_bundle_unsafe_address_rejected_by_cli);
    RUN_TEST(test_inspect_actual_typed_manifest_and_sidecar_identity);
    RUN_TEST(test_absent_sidecar_is_valid_single_file_delivery);
    RUN_TEST(test_present_bad_sidecar_rejects_inspect_and_pack);
    RUN_TEST(test_legacy_sidecar_is_compatible_but_not_verified);
    RUN_TEST(test_multiple_selected_equal_overlap_has_union_and_provenance);
    RUN_TEST(test_multiple_selected_different_overlap_preserves_prior);
    RUN_TEST(test_web_selected_pack_and_inspect_bind_explicit_target);
    RUN_TEST(test_wrong_exact_target_preserves_prior_and_absent_sidecar_remains_valid);
    RUN_TEST(test_inspection_tokens_do_not_change_runtime_target_or_mount_filesystem);
    RUN_TEST(test_web_inspection_keeps_sidecar_target_and_hash_validation);
    RUN_TEST(test_cli_platform_and_exact_target_disagreement_is_ordinary_refusal);
    RUN_TEST(test_read_entry_ambiguous_zero_codec_refuses);
    RUN_TEST(test_read_entry_compressed_stripped_exact_bytes);
    RUN_TEST(test_read_entry_present_empty_is_not_missing);
    RUN_TEST(test_read_entry_typed_bundle_retains_public_admission);
    RUN_TEST(test_read_entry_secure_key_admission_and_exact_payload);
    RUN_TEST(test_read_entry_missing_and_unsafe_paths_do_not_publish);
    RUN_TEST(test_read_entry_crc_mismatch_with_valid_archive_header_refuses);
    RUN_TEST(test_read_entry_over_16_mib_refuses_before_output);
    RUN_TEST(test_read_entry_existing_file_and_directory_are_protected);
    return UNITY_END();
}
