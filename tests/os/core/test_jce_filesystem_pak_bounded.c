/* Actual VFS + archive writer/reader. Only decode is counted/failed through
 * the existing public provider inversion port; no fake archive or GPU. */
#include "unity.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/resource/jce_archive_writer.h>
#include <jce/resource/jce_pak_loader.h>
#include <string.h>
#include <stdio.h>

static unsigned decodes;
static bool fail_decode;
static const JceFsPakProvider *previous_provider;
static const void *JCE_CALL find_asset(const JcePakArchive *pak, const char *key)
{ return jce_pak_find(pak, key); }
static uint64_t JCE_CALL asset_size(const void *asset)
{ return ((const JcePakAsset *)asset)->original_size; }
static size_t JCE_CALL decode_asset(const void *asset, void *bytes, size_t capacity)
{
    ++decodes;
    return fail_decode ? 0 : jce_pak_decompress((const JcePakAsset *)asset, bytes, capacity);
}
static const JceFsPakProvider provider = {find_asset, asset_size, decode_asset};
typedef struct Pak { void *blob; JcePakArchive *archive; } Pak;
static Pak make_pak(const char *key, const void *bytes, size_t size)
{
    Pak out = {0}; size_t blob_size = 0;
    JceArchiveWriter *writer = jce_archive_writer_create(NULL);
    if (!writer) return out;
    bool ok = jce_archive_writer_add(writer, key, bytes, size) &&
              jce_archive_writer_finish(writer, &out.blob, &blob_size);
    jce_archive_writer_destroy(writer);
    if (ok) out.archive = jce_pak_open(out.blob, blob_size);
    return out;
}
static void close_pak(Pak pak) { jce_pak_close(pak.archive); jce_free(pak.blob); }
void setUp(void)
{
    previous_provider = jce_fs_get_pak_provider();
    jce_fs_set_pak_provider(&provider); decodes = 0; fail_decode = false;
}
void tearDown(void) { jce_fs_set_pak_provider(previous_provider); }

static void test_metadata_bound_precedes_real_decode(void)
{
    const size_t size = (1u << 20) + 1u;
    char *bytes = jce_malloc(size); TEST_ASSERT_NOT_NULL(bytes); memset(bytes, 'a', size);
    Pak pak = make_pak("large", bytes, size); jce_free(bytes); TEST_ASSERT_NOT_NULL(pak.archive);
    JceFileSystem *fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_TRUE(jce_fs_mount_pak(fs, pak.archive));
    void *data = NULL; uint64_t count = 9;
    JceFsPakReadResult read = jce_fs_read_pak_bounded(fs, "large", 1u << 20, &data, &count);
    /* Release unexpected old-behavior bytes before the ordinary assertion. */
    bool empty = data == NULL; jce_fs_buffer_free(data);
    jce_fs_destroy(fs); close_pak(pak);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PAK_READ_OVER_LIMIT, read);
    TEST_ASSERT_TRUE(empty); TEST_ASSERT_EQUAL_UINT64(0, count);
    TEST_ASSERT_EQUAL_UINT(0, decodes);
}
static void test_metadata_probe_and_binary_sentinel(void)
{
    const char bytes[] = {'a', 0, 'z'}; Pak pak = make_pak("data", bytes, sizeof bytes);
    TEST_ASSERT_NOT_NULL(pak.archive);
    JceFileSystem *fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_TRUE(jce_fs_mount_pak(fs, pak.archive)); uint64_t count = 0;
    TEST_ASSERT_EQUAL_INT(JCE_FS_PAK_READ_OK, jce_fs_read_pak_bounded(fs, "data", 3, NULL, &count));
    TEST_ASSERT_EQUAL_UINT64(3, count); TEST_ASSERT_EQUAL_UINT(0, decodes);
    void *data = NULL;
    TEST_ASSERT_EQUAL_INT(JCE_FS_PAK_READ_OK, jce_fs_read_pak_bounded(fs, "data", 3, &data, &count));
    TEST_ASSERT_NOT_NULL(data); TEST_ASSERT_EQUAL_MEMORY(bytes, data, 3);
    TEST_ASSERT_EQUAL_CHAR(0, ((char *)data)[3]); TEST_ASSERT_EQUAL_UINT(1, decodes);
    jce_fs_buffer_free(data); jce_fs_destroy(fs); close_pak(pak);
}
static void test_global_host_mount_cannot_contaminate_explicit_pak(void)
{
    char cwd[1024], dir[1200], path[1300];
    TEST_ASSERT_TRUE(jce_fs_host_get_current_dir(cwd, sizeof cwd));
    snprintf(dir, sizeof dir, "%s/pak-bounded-host", cwd);
    snprintf(path, sizeof path, "%s/data", dir);
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(dir));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, "host", 4));
    JceFileSystem *host = jce_fs_create(), *fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(host); TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_TRUE(jce_fs_mount_dir(host, NULL, dir));
    uint64_t count = 0; void *data = jce_fs_read_all(fs, "data", &count);
    TEST_ASSERT_NOT_NULL(data); TEST_ASSERT_EQUAL_MEMORY("host", data, 4); jce_fs_buffer_free(data);
    JceFsPakReadResult absent = jce_fs_read_pak_bounded(fs, "data", 8, &data, &count);
    bool missing = data == NULL; jce_fs_buffer_free(data);
    Pak pak = make_pak("data", "pak", 3); TEST_ASSERT_NOT_NULL(pak.archive);
    TEST_ASSERT_TRUE(jce_fs_mount_pak(fs, pak.archive));
    JceFsPakReadResult found = jce_fs_read_pak_bounded(fs, "data", 8, &data, &count);
    bool correct = data && count == 3 && memcmp(data, "pak", 3) == 0; jce_fs_buffer_free(data);
    jce_fs_destroy(fs); jce_fs_destroy(host); close_pak(pak);
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(path));
    TEST_ASSERT_TRUE(jce_fs_host_remove_recursive(dir));
    TEST_ASSERT_EQUAL_INT(JCE_FS_PAK_READ_ABSENT, absent); TEST_ASSERT_TRUE(missing);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PAK_READ_OK, found); TEST_ASSERT_TRUE(correct);
}
static void test_first_matching_mount_owns_failure_and_limit(void)
{
    Pak low = make_pak("data", "old", 3), high = make_pak("data", "newer", 5);
    TEST_ASSERT_NOT_NULL(low.archive); TEST_ASSERT_NOT_NULL(high.archive);
    JceFileSystem *fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_TRUE(jce_fs_mount_pak_named(fs, "low", low.archive));
    TEST_ASSERT_TRUE(jce_fs_mount_pak_named(fs, "high", high.archive));
    void *data = NULL; uint64_t count = 8;
    const JceFsPakReadResult limited = jce_fs_read_pak_bounded(fs, "data", 4, &data, &count);
    const bool limit_empty = data == NULL && count == 0;
    const unsigned limit_decodes = decodes; jce_fs_buffer_free(data); decodes = 0;
    fail_decode = true;
    const JceFsPakReadResult failed = jce_fs_read_pak_bounded(fs, "data", 5, &data, &count);
    const bool failure_empty = data == NULL && count == 0;
    const unsigned failure_decodes = decodes; jce_fs_buffer_free(data);
    fail_decode = false;
    TEST_ASSERT_EQUAL_INT(JCE_FS_PAK_READ_OK, jce_fs_read_pak_bounded(fs, "data", 5, &data, &count));
    TEST_ASSERT_EQUAL_MEMORY("newer", data, 5); jce_fs_buffer_free(data);
    jce_fs_destroy(fs); close_pak(high); close_pak(low);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PAK_READ_OVER_LIMIT, limited);
    TEST_ASSERT_TRUE(limit_empty); TEST_ASSERT_EQUAL_UINT(0, limit_decodes);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PAK_READ_FAILED, failed);
    TEST_ASSERT_TRUE(failure_empty); TEST_ASSERT_EQUAL_UINT(1, failure_decodes);
}
static void test_invalid_arguments_clear_outputs(void)
{
    void *data = (void *)1; uint64_t count = 9;
    TEST_ASSERT_EQUAL_INT(JCE_FS_PAK_READ_FAILED, jce_fs_read_pak_bounded(NULL, "data", 8, &data, &count));
    TEST_ASSERT_NULL(data); TEST_ASSERT_EQUAL_UINT64(0, count);
    JceFileSystem *fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PAK_READ_FAILED, jce_fs_read_pak_bounded(fs, NULL, 8, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(JCE_FS_PAK_READ_FAILED, jce_fs_read_pak_bounded(fs, "", 8, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(JCE_FS_PAK_READ_FAILED, jce_fs_read_pak_bounded(fs, "data", 0, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(JCE_FS_PAK_READ_ABSENT, jce_fs_read_pak_bounded(fs, "data", 8, NULL, NULL));
    jce_fs_destroy(fs);
}
int main(void)
{
    UNITY_BEGIN(); RUN_TEST(test_metadata_bound_precedes_real_decode);
    RUN_TEST(test_metadata_probe_and_binary_sentinel);
    RUN_TEST(test_global_host_mount_cannot_contaminate_explicit_pak);
    RUN_TEST(test_first_matching_mount_owns_failure_and_limit);
    RUN_TEST(test_invalid_arguments_clear_outputs); return UNITY_END();
}
