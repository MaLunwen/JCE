/* Real VFS implementation with only mutex-create failure injection. */
#include "unity.h"
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_trace.h>
static bool fail_mutex;
static JceMutex *controlled_mutex_create(void)
{
    return fail_mutex ? NULL : jce_mutex_create();
}
#define jce_mutex_create controlled_mutex_create
#define jce_fs_set_pak_provider controlled_jce_fs_set_pak_provider
#define jce_fs_register_pak_provider_internal controlled_jce_fs_register_pak_provider_internal
#define jce_fs_get_pak_provider controlled_jce_fs_get_pak_provider
#define jce_fs_mounted_pak_count controlled_jce_fs_mounted_pak_count
#define jce_fs_read controlled_jce_fs_read
#define jce_fs_size controlled_jce_fs_size
#define jce_fs_exists controlled_jce_fs_exists
#define jce_fs_write controlled_jce_fs_write
#define jce_fs_set_active controlled_jce_fs_set_active
#define jce_fs_set_active_policy controlled_jce_fs_set_active_policy
#define jce_fs_create controlled_jce_fs_create
#define jce_fs_destroy controlled_jce_fs_destroy
#define jce_fs_mount_pak controlled_jce_fs_mount_pak
#define jce_fs_mount_pak_named controlled_jce_fs_mount_pak_named
#define jce_fs_unmount_pak_named controlled_jce_fs_unmount_pak_named
#define jce_fs_mount_dir controlled_jce_fs_mount_dir
#define jce_fs_open controlled_jce_fs_open
#define jce_fs_close controlled_jce_fs_close
#define jce_fs_read_all controlled_jce_fs_read_all
#define jce_fs_read_pak_bounded controlled_jce_fs_read_pak_bounded
#define jce_fs_set_write_dir controlled_jce_fs_set_write_dir
#define jce_fs_open_write controlled_jce_fs_open_write
#define jce_fs_write_all controlled_jce_fs_write_all
#include "os/core/jce_filesystem.c"

void setUp(void) { jce_trace_set_enabled(false); }
void tearDown(void) { jce_trace_set_enabled(false); }
static void test_mutex_failure_rejects_vfs_before_physfs_access(void)
{
    TEST_ASSERT_NULL(s_fs_mutex);
    uint32_t users = s_fs_users;
    fail_mutex = true;
    jce_trace_set_enabled(true);
    jce_trace_reset();
    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_NULL(fs);
    TEST_ASSERT_NULL(s_fs_mutex);
    TEST_ASSERT_EQUAL_UINT32(users, s_fs_users);
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
    JceTraceCursor cursor;
    JceTraceEvent events[32];
    jce_trace_cursor_init(&cursor);
    uint32_t count = jce_trace_read(&cursor, events, 32), i;
    bool failed = false;
    for (i = 0; i < count; ++i)
        if (events[i].type == JCE_TRACE_EVENT_TASK_END)
            failed |= events[i].value_u32[0] == JCE_TRACE_TASK_FAILED;
    TEST_ASSERT_TRUE(failed);
}
static void test_normal_mutex_creation_still_supports_complete_vfs_lifetime(void)
{
    fail_mutex = false;
    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(s_fs_mutex);
    TEST_ASSERT_EQUAL_UINT32(1, s_fs_users);
    jce_fs_destroy(fs);
    TEST_ASSERT_EQUAL_UINT32(0, s_fs_users);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mutex_failure_rejects_vfs_before_physfs_access);
    RUN_TEST(test_normal_mutex_creation_still_supports_complete_vfs_lifetime);
    return UNITY_END();
}
