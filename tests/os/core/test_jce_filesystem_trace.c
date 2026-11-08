/* Actual filesystem IO and state observation; no mocked implementation. */
#include "unity.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_trace.h>
#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_path.h>
#include "os/core/jce_filesystem_host_internal.h"
#include "os/core/jce_filesystem_read_internal.h"
#include <stdio.h>
#include <string.h>

static char root[1024], path[1200], owned_prefix[1024];
static bool root_created;
static JceTraceEvent events[256];
static bool root_is_owned(void)
{
    size_t length = strlen(owned_prefix);
    return length > 0 && strncmp(root, owned_prefix, length) == 0 &&
        root[length] != '\0' && strchr(root + length, '/') == NULL &&
        strchr(root + length, '\\') == NULL;
}
void setUp(void)
{
    static unsigned sequence;
    char cwd[1024], base[1024], desired[1100];
    jce_trace_set_enabled(false);
    jce_trace_task_restore_id(0);
    root_created = false; root[0] = owned_prefix[0] = '\0';
    TEST_ASSERT_TRUE(jce_fs_host_get_current_dir(cwd, sizeof(cwd)));
    TEST_ASSERT_TRUE(jce_path_normalize(base, sizeof(base), cwd));
    int length = snprintf(owned_prefix, sizeof(owned_prefix), "%s/_ut_fs_trace_", base);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof(owned_prefix));
    snprintf(desired, sizeof(desired), "%s%u", owned_prefix, ++sequence);
    TEST_ASSERT_TRUE(jce_fs_host_make_unique_path(desired, root, sizeof(root)));
    TEST_ASSERT_TRUE(root_is_owned());
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(root)); TEST_ASSERT_FALSE(jce_fs_host_exists_dir(root));
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(root)); root_created = true;
    snprintf(path, sizeof(path), "%s/asset.txt", root);
    jce_trace_set_enabled(true);
    jce_trace_reset();
}
void tearDown(void)
{
    jce_trace_set_enabled(false);
    jce_trace_task_restore_id(0);
    jce_fs_set_active(NULL);
    if (root_created) {
        bool owned = root_is_owned(); TEST_ASSERT_TRUE(owned);
        if (owned) TEST_ASSERT_TRUE(jce_fs_host_remove_recursive(root));
    }
    root_created = false;
}
static uint32_t read_events(void)
{
    JceTraceCursor cursor;
    jce_trace_cursor_init(&cursor);
    return jce_trace_read(&cursor, events, 256);
}
static const JceTraceEvent *find(uint32_t count, JceTraceEventType type,
                                const char *name)
{
    uint32_t i;
    for (i = 0; i < count; ++i)
        if (events[i].type == type && strcmp(events[i].name, name) == 0)
            return &events[i];
    return NULL;
}
static void test_real_atomic_host_io_has_nested_attempts_and_failure_state(void)
{
    uint64_t size = 0;
    TEST_ASSERT_TRUE(jce_fs_host_write_all_atomic(path, "abc", 3));
    void *data = jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_UINT64(3, size);
    TEST_ASSERT_EQUAL_MEMORY("abc", data, 3);
    jce_fs_buffer_free(data);
    TEST_ASSERT_FALSE(jce_fs_host_write_all_atomic(NULL, "abc", 3));
    uint32_t count = read_events();
    const JceTraceEvent *outer = find(count, JCE_TRACE_EVENT_TASK_SUBMIT,
                                    "fs.host.write_all_atomic");
    const JceTraceEvent *inner = find(count, JCE_TRACE_EVENT_TASK_SUBMIT,
                                    "fs.host.write_all");
    TEST_ASSERT_NOT_NULL(outer);
    TEST_ASSERT_NOT_NULL(inner);
    TEST_ASSERT_EQUAL_UINT64(outer->id, inner->parent_id);
    bool succeeded = false, failed = false;
    uint32_t i;
    for (i = 0; i < count; ++i)
        if (events[i].type == JCE_TRACE_EVENT_TASK_END &&
            strcmp(events[i].name, "fs.host.write_all_atomic") == 0) {
            succeeded |= events[i].value_u32[0] == JCE_TRACE_TASK_SUCCEEDED;
            failed |= events[i].value_u32[0] == JCE_TRACE_TASK_FAILED;
        }
    TEST_ASSERT_TRUE(succeeded);
    TEST_ASSERT_TRUE(failed);
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
}
static void test_vfs_success_missing_path_and_null_cleanup_are_distinct(void)
{
    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_TRUE(jce_fs_set_write_dir(fs, root));
    TEST_ASSERT_TRUE(jce_fs_mount_dir(fs, "", root));
    TEST_ASSERT_TRUE(jce_fs_write_all(fs, "asset.txt", "abc", 3));
    uint64_t size = 0;
    void *data = jce_fs_read_all(fs, "asset.txt", &size);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_UINT64(3, size);
    jce_fs_buffer_free(data);
    TEST_ASSERT_NULL(jce_fs_open(fs, "absent.txt"));
    jce_fs_close(NULL);
    jce_fs_destroy(fs);
    uint32_t count = read_events(), i;
    TEST_ASSERT_NOT_NULL(find(count, JCE_TRACE_EVENT_TASK_END, "fs.create"));
    TEST_ASSERT_NOT_NULL(find(count, JCE_TRACE_EVENT_TASK_END, "fs.destroy"));
    bool failed = false, cancelled = false;
    for (i = 0; i < count; ++i) {
        if (events[i].type != JCE_TRACE_EVENT_TASK_END) continue;
        if (strcmp(events[i].name, "fs.open") == 0)
            failed |= events[i].value_u32[0] == JCE_TRACE_TASK_FAILED;
        if (strcmp(events[i].name, "fs.close") == 0)
            cancelled |= events[i].value_u32[0] == JCE_TRACE_TASK_CANCELLED;
    }
    TEST_ASSERT_TRUE(failed);
    TEST_ASSERT_TRUE(cancelled);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT64(stats.work_items_started, stats.work_items_completed);
}
static void test_disabled_observation_preserves_actual_io_without_events(void)
{
    jce_trace_set_enabled(false);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, "abc", 3));
    uint64_t size = 0;
    void *data = jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_UINT64(3, size);
    jce_fs_buffer_free(data);
    TEST_ASSERT_EQUAL_UINT32(0, read_events());
}
static void test_bounded_real_vfs_checks_size_and_trace_outcome(void)
{
    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_TRUE(jce_fs_mount_dir(fs, "", root));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, "abc", 3));
    uint64_t size = 0;
    TEST_ASSERT_NULL(jce_fs_read_all_bounded_internal(fs, "asset.txt", 2, &size));
    TEST_ASSERT_EQUAL_UINT64(3, size);
    void *data = jce_fs_read_all_bounded_internal(fs, "asset.txt", 3, &size);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_MEMORY("abc", data, 3);
    TEST_ASSERT_EQUAL_UINT64(3, size);
    jce_fs_buffer_free(data);
    TEST_ASSERT_NULL(jce_fs_read_all_bounded_internal(fs, "missing.txt", 3, &size));
    TEST_ASSERT_EQUAL_UINT64(0, size);
    TEST_ASSERT_NULL(jce_fs_read_all_bounded_internal(fs, "asset.txt", 0, &size));
    jce_fs_destroy(fs);
    uint32_t count = read_events();
    bool succeeded = false, failed = false;
    for (uint32_t i = 0; i < count; ++i)
        if (events[i].type == JCE_TRACE_EVENT_TASK_END &&
            strcmp(events[i].name, "fs.read_all_bounded") == 0) {
            succeeded |= events[i].value_u32[0] == JCE_TRACE_TASK_SUCCEEDED;
            failed |= events[i].value_u32[0] == JCE_TRACE_TASK_FAILED;
        }
    TEST_ASSERT_TRUE(succeeded);
    TEST_ASSERT_TRUE(failed);
}
static void expect_detail_counter(uint32_t count, uint64_t id, uint64_t parent,
                                   const char *name, uint32_t expected)
{
    const JceTraceEvent *counter = NULL;
    for (uint32_t i = 0; i < count; ++i)
        if (events[i].type == JCE_TRACE_EVENT_COUNTER && events[i].id == id &&
            strcmp(events[i].name, name) == 0) {
            TEST_ASSERT_NULL(counter); counter = &events[i];
        }
    TEST_ASSERT_NOT_NULL(counter);
    TEST_ASSERT_EQUAL_UINT64(parent, counter->parent_id);
    TEST_ASSERT_EQUAL_UINT64(expected, counter->value_u64);
}
static void expect_real_dispatch(const char *request, uint32_t dispatch,
                                 uint32_t stage, uint32_t known, uint32_t close,
                                 uint64_t bytes, JceTraceTaskState state)
{
    uint32_t count = read_events();
    const JceTraceEvent *submit = find(count, JCE_TRACE_EVENT_TASK_SUBMIT, "fs.host.read_all");
    const JceTraceEvent *end = find(count, JCE_TRACE_EVENT_TASK_END, "fs.host.read_all");
    TEST_ASSERT_NOT_NULL(submit); TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_UINT64(submit->id, end->id);
    TEST_ASSERT_EQUAL_INT(state, end->value_u32[0]);
    uint64_t hash = jce_fnv1a64_str(request);
    expect_detail_counter(count, submit->id, 0, "fs.read.path_hash_hi", (uint32_t)(hash >> 32));
    expect_detail_counter(count, submit->id, 0, "fs.read.path_hash_lo", (uint32_t)hash);
    expect_detail_counter(count, submit->id, 0, "fs.read.dispatch", dispatch);
    expect_detail_counter(count, submit->id, 0, "fs.read.failure_stage", stage);
    expect_detail_counter(count, submit->id, 0, "fs.read.known", known);
    expect_detail_counter(count, submit->id, 0, "fs.read.close", close);
    expect_detail_counter(count, submit->id, 0, "fs.read.actual_hi", (uint32_t)(bytes >> 32));
    expect_detail_counter(count, submit->id, 0, "fs.read.actual_lo", (uint32_t)bytes);
    for (uint32_t i = 0; i < count; ++i) TEST_ASSERT_NULL(strstr(events[i].name, root));
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
}
static void test_real_active_vfs_and_host_dispatch_details_preserve_isolation(void)
{
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, "abc", 3));
    JceFileSystem *fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_TRUE(jce_fs_mount_dir(fs, "", root));
    jce_fs_set_active_policy(fs, JCE_FS_ACTIVE_ISOLATED);
    uint64_t size = 99;
    jce_trace_reset();
    void *data = jce_fs_host_read_all("asset.txt", &size);
    TEST_ASSERT_NOT_NULL(data); TEST_ASSERT_EQUAL_UINT64(3, size); jce_fs_buffer_free(data);
    expect_real_dispatch("asset.txt", JCE_FS_HOST_READ_VFS, JCE_FS_HOST_READ_OK, 11,
        JCE_FS_HOST_READ_CLOSE_NONE, 3, JCE_TRACE_TASK_SUCCEEDED);
    jce_trace_reset();
    TEST_ASSERT_NULL(jce_fs_host_read_all("absent.txt", &size)); TEST_ASSERT_EQUAL_UINT64(0, size);
    expect_real_dispatch("absent.txt", JCE_FS_HOST_READ_VFS, JCE_FS_HOST_READ_ISOLATED_VFS, 0,
        JCE_FS_HOST_READ_CLOSE_NONE, 0, JCE_TRACE_TASK_FAILED);
    /* Even isolated VFS keeps explicit fully-qualified host addresses host-owned. */
    jce_trace_reset(); data = jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(data); TEST_ASSERT_EQUAL_UINT64(3, size); jce_fs_buffer_free(data);
    expect_real_dispatch(path, JCE_FS_HOST_READ_HOST, JCE_FS_HOST_READ_OK, 11,
        JCE_FS_HOST_READ_CLOSE_OK, 3, JCE_TRACE_TASK_SUCCEEDED);
    char relative[1200], leaf[1024];
    TEST_ASSERT_TRUE(jce_path_basename(leaf, sizeof(leaf), root));
    snprintf(relative, sizeof relative, "%s/asset.txt", leaf);
    /* The same real loose host file is reachable only through OVERLAY fallback. */
    jce_trace_reset(); TEST_ASSERT_NULL(jce_fs_host_read_all(relative, &size));
    expect_real_dispatch(relative, JCE_FS_HOST_READ_VFS, JCE_FS_HOST_READ_ISOLATED_VFS, 0,
        JCE_FS_HOST_READ_CLOSE_NONE, 0, JCE_TRACE_TASK_FAILED);
    jce_fs_set_active_policy(fs, JCE_FS_ACTIVE_OVERLAY);
    jce_trace_reset(); data = jce_fs_host_read_all(relative, &size);
    TEST_ASSERT_NOT_NULL(data); TEST_ASSERT_EQUAL_UINT64(3, size);
    TEST_ASSERT_EQUAL_MEMORY("abc", data, 3); jce_fs_buffer_free(data);
    expect_real_dispatch(relative, JCE_FS_HOST_READ_VFS_THEN_HOST, JCE_FS_HOST_READ_OK, 11,
        JCE_FS_HOST_READ_CLOSE_OK, 3, JCE_TRACE_TASK_SUCCEEDED);
    jce_fs_set_active(NULL); jce_fs_destroy(fs);
}
static void test_real_host_presence_distinguishes_file_missing_directory(void)
{
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, "abc", 3));
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_PRESENT, jce_fs_probe_read(path));
    char absent[1300]; snprintf(absent, sizeof absent, "%s/not-present.json", root);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_ABSENT, jce_fs_probe_read(absent));
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_UNKNOWN, jce_fs_probe_read(root));
    const uint32_t count = read_events(); unsigned queries = 0;
    for (uint32_t n = 0; n < count; ++n) {
        if (events[n].type == JCE_TRACE_EVENT_TASK_END && strcmp(events[n].name, "fs.probe_read") == 0) {
            TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_SUCCEEDED, events[n].value_u32[0]); ++queries;
        }
        TEST_ASSERT_FALSE(strcmp(events[n].name, "fs.host.read_all") == 0);
    }
    TEST_ASSERT_EQUAL_UINT32(3, queries);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_real_atomic_host_io_has_nested_attempts_and_failure_state);
    RUN_TEST(test_real_active_vfs_and_host_dispatch_details_preserve_isolation);
    RUN_TEST(test_vfs_success_missing_path_and_null_cleanup_are_distinct);
    RUN_TEST(test_disabled_observation_preserves_actual_io_without_events);
    RUN_TEST(test_bounded_real_vfs_checks_size_and_trace_outcome);
    RUN_TEST(test_real_host_presence_distinguishes_file_missing_directory);
    return UNITY_END();
}
