/* Full FS and host-policy owners, real trace ring, typed metadata ports.
 * No file read or graphics; native metadata ports do not certify OS access. */
#include "unity.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_trace.h>
#include <jce/os/core/jce_thread.h>
#include "os/core/jce_filesystem_read_internal.h"
#include "os/core/jce_memory.h"
#include <physfs.h>
#include <string.h>
#if JCE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <errno.h>
#endif
static unsigned native_queries, conversions, allocations, virtual_queries, pak_queries, custom_reads;
static bool native_missing, native_denied, native_nonregular, reject_allocation, virtual_active, virtual_found, virtual_denied, pak_found;
static bool reject_conversion, virtual_sources, virtual_silent_failure;
static unsigned search_queries, virtual_error_reads;
static PHYSFS_ErrorCode virtual_error, virtual_query_error;
static int pak_token, asset_token;
static void *controlled_alloc(size_t size)
{ ++allocations; return reject_allocation ? NULL : jce_malloc(size); }
#if JCE_PLATFORM_WINDOWS
static int WINAPI controlled_utf8(UINT page, DWORD flags, LPCCH input, int bytes, LPWSTR out, int capacity)
{
    ++conversions;
    if (reject_conversion && out) return 0;
    return MultiByteToWideChar(page, flags, input, bytes, out, capacity);
}
static BOOL WINAPI controlled_attributes(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID output)
{
    TEST_ASSERT_NOT_NULL(path); TEST_ASSERT_EQUAL_INT(GetFileExInfoStandard, level); ++native_queries;
    if (native_missing || native_denied) return FALSE;
    WIN32_FILE_ATTRIBUTE_DATA *info = output; memset(info, 0, sizeof(*info));
    info->dwFileAttributes = native_nonregular ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    return TRUE;
}
static DWORD WINAPI controlled_error(void)
{ return native_denied ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND; }
#define MultiByteToWideChar controlled_utf8
#define GetFileAttributesExW controlled_attributes
#define GetLastError controlled_error
#else
static int controlled_stat(const char *path, struct stat *info)
{
    TEST_ASSERT_NOT_NULL(path); ++native_queries;
    if (native_missing || native_denied) { errno = native_denied ? EACCES : ENOENT; return -1; }
    memset(info, 0, sizeof(*info)); info->st_mode = native_nonregular ? S_IFDIR : S_IFREG; return 0;
}
#define stat(path, info) controlled_stat((path), (info))
#endif
#define jce_malloc controlled_alloc
static int controlled_is_init(void) { return virtual_active; }
static int controlled_vfs_stat(const char *path, PHYSFS_Stat *info)
{
    TEST_ASSERT_NOT_NULL(path); ++virtual_queries;
    if (!virtual_found) {
        if (virtual_sources && !virtual_silent_failure) virtual_error = virtual_denied ? PHYSFS_ERR_PERMISSION : PHYSFS_ERR_NOT_FOUND;
        else if (virtual_query_error != PHYSFS_ERR_OK) virtual_error = virtual_query_error;
        return 0;
    }
    memset(info, 0, sizeof(*info)); info->filetype = PHYSFS_FILETYPE_REGULAR; return 1;
}
static PHYSFS_ErrorCode controlled_vfs_error(void)
{
    ++virtual_error_reads; const PHYSFS_ErrorCode error = virtual_error;
    virtual_error = PHYSFS_ERR_OK; return error;
}
static void controlled_search_path(PHYSFS_StringCallback callback, void *data)
{ ++search_queries; if (virtual_sources) callback(data, "fixture-source"); }
#define PHYSFS_isInit controlled_is_init
#define PHYSFS_stat controlled_vfs_stat
#define PHYSFS_getSearchPathCallback controlled_search_path
#define PHYSFS_getLastErrorCode controlled_vfs_error
#ifndef JCE_TEST_FS_PRESENCE_VFS_OWNER
#define JCE_TEST_FS_PRESENCE_VFS_OWNER "os/core/jce_filesystem.c"
#endif
#ifndef JCE_TEST_FS_PRESENCE_HOST_OWNER
#define JCE_TEST_FS_PRESENCE_HOST_OWNER "os/core/jce_filesystem_host.c"
#endif
#include JCE_TEST_FS_PRESENCE_VFS_OWNER
#include JCE_TEST_FS_PRESENCE_HOST_OWNER
#undef jce_malloc
#undef PHYSFS_isInit
#undef PHYSFS_stat
#undef PHYSFS_getSearchPathCallback
#undef PHYSFS_getLastErrorCode
#if JCE_PLATFORM_WINDOWS
#undef MultiByteToWideChar
#undef GetFileAttributesExW
#undef GetLastError
#else
#undef stat
#endif
static JceFileSystem fs;
static JceMutex *fixture_lock;
static const void *JCE_CALL find_asset(const JcePakArchive *pak, const char *path)
{ TEST_ASSERT_EQUAL_PTR(&pak_token, pak); TEST_ASSERT_NOT_NULL(path); ++pak_queries; return pak_found ? &asset_token : NULL; }
static uint64_t JCE_CALL asset_size(const void *asset) { (void)asset; return 1; }
static size_t JCE_CALL decode(const void *asset, void *out, size_t cap)
{ (void)asset; (void)out; (void)cap; TEST_FAIL_MESSAGE("Presence must not decompress"); return 0; }
static const JceFsPakProvider provider = {find_asset, asset_size, decode};
static void *custom_read(const JceFileSystem *owner, const char *path, uint64_t *size)
{ (void)owner; (void)path; (void)size; ++custom_reads; TEST_FAIL_MESSAGE("Presence must not invoke custom reader"); return NULL; }
void setUp(void)
{
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0); jce_fs_set_active(NULL);
    memset(&fs, 0, sizeof(fs)); jce_fs_set_pak_provider(&provider);
    fixture_lock = jce_mutex_create(); TEST_ASSERT_NOT_NULL(fixture_lock); s_fs_mutex = fixture_lock;
    native_queries = conversions = allocations = virtual_queries = pak_queries = custom_reads = 0;
    native_missing = native_denied = native_nonregular = reject_allocation = reject_conversion = false;
    virtual_active = virtual_found = virtual_denied = pak_found = false;
    virtual_sources = true; virtual_silent_failure = false; search_queries = virtual_error_reads = 0;
    virtual_error = virtual_query_error = PHYSFS_ERR_OK;
    jce_trace_reset(); jce_trace_set_enabled(true);
}
void tearDown(void)
{ jce_trace_set_enabled(false); jce_trace_task_restore_id(0); jce_fs_set_active(NULL); jce_fs_set_pak_provider(NULL);
  s_fs_mutex = NULL; jce_mutex_destroy(fixture_lock); fixture_lock = NULL; }
static void native_regular_is_present_without_file_read(void)
{ TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_PRESENT, jce_fs_probe_read("asset.json")); TEST_ASSERT_EQUAL_UINT32(1, native_queries); TEST_ASSERT_EQUAL_UINT32(0, custom_reads); }
static void native_not_found_is_absent(void)
{ native_missing = true; TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_ABSENT, jce_fs_probe_read("asset.json")); }
static void native_access_failure_is_unknown(void)
{ native_denied = true; TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_UNKNOWN, jce_fs_probe_read("asset.json")); }
static void native_nonregular_is_unknown(void)
{ native_nonregular = true; TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_UNKNOWN, jce_fs_probe_read("asset.json")); }
static void null_and_empty_never_query_metadata(void)
{ TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_UNKNOWN, jce_fs_probe_read(NULL)); TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_UNKNOWN, jce_fs_probe_read("")); TEST_ASSERT_EQUAL_UINT32(0, native_queries); }
static void absolute_host_path_bypasses_isolated_custom_reader(void)
{
    jce_fs_set_active_policy(&fs, JCE_FS_ACTIVE_ISOLATED); jce_fs_set_active_reader(custom_read);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_PRESENT, jce_fs_probe_read("/asset.json"));
    TEST_ASSERT_EQUAL_UINT32(1, native_queries); TEST_ASSERT_EQUAL_UINT32(0, virtual_queries); TEST_ASSERT_EQUAL_UINT32(0, custom_reads);
}
static void builtin_virtual_regular_precedes_host(void)
{
    virtual_active = virtual_found = true; jce_fs_set_active(&fs);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_PRESENT, jce_fs_probe_read("asset.json"));
    TEST_ASSERT_EQUAL_UINT32(1, virtual_queries); TEST_ASSERT_EQUAL_UINT32(0, native_queries);
}
static void mounted_pak_presence_never_decompresses(void)
{
    fs.pak_count = 1; fs.paks[0].pak = (JcePakArchive *)&pak_token; pak_found = true;
    jce_fs_set_active(&fs);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_PRESENT, jce_fs_probe_read("asset.json"));
    TEST_ASSERT_EQUAL_UINT32(1, pak_queries); TEST_ASSERT_EQUAL_UINT32(0, native_queries); TEST_ASSERT_EQUAL_UINT32(0, allocations);
}
static void normalized_virtual_miss_follows_overlay_or_isolated_policy(void)
{
    virtual_active = true; jce_fs_set_active(&fs);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_PRESENT, jce_fs_probe_read("asset.json"));
    TEST_ASSERT_EQUAL_UINT32(1, native_queries);
    jce_fs_set_active_policy(&fs, JCE_FS_ACTIVE_ISOLATED);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_ABSENT, jce_fs_probe_read("asset.json"));
    TEST_ASSERT_EQUAL_UINT32(1, native_queries);
}
static void uncertain_virtual_source_cannot_be_masked_by_fallbacks(void)
{
    virtual_active = virtual_denied = true; pak_found = true;
    fs.pak_count = 1; fs.paks[0].pak = (JcePakArchive *)&pak_token; jce_fs_set_active(&fs);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_UNKNOWN, jce_fs_probe_read("asset.json"));
    TEST_ASSERT_EQUAL_UINT32(0, native_queries); TEST_ASSERT_EQUAL_UINT32(0, pak_queries);
}
static void missing_provider_is_unknown_for_mounted_content(void)
{
    fs.pak_count = 1; fs.paks[0].pak = (JcePakArchive *)&pak_token;
    jce_fs_set_pak_provider(NULL); jce_fs_set_active(&fs);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_UNKNOWN, jce_fs_probe_read("asset.json")); TEST_ASSERT_EQUAL_UINT32(0, native_queries);
}
static void custom_reader_without_probe_is_unknown_and_not_called(void)
{
    jce_fs_set_active(&fs); jce_fs_set_active_reader(custom_read); native_missing = true;
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_UNKNOWN, jce_fs_probe_read("asset.json"));
    TEST_ASSERT_EQUAL_UINT32(0, native_queries); TEST_ASSERT_EQUAL_UINT32(0, virtual_queries); TEST_ASSERT_EQUAL_UINT32(0, custom_reads);
}
static void any_reader_replacement_revokes_builtin_probe(void)
{
    jce_fs_set_active(&fs); jce_fs_set_active_reader(jce_fs_read_all);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_UNKNOWN, jce_fs_probe_read("asset.json")); TEST_ASSERT_EQUAL_UINT32(0, virtual_queries);
}
static void null_reader_retains_current_host_only_semantics(void)
{
    jce_fs_set_active_policy(&fs, JCE_FS_ACTIVE_ISOLATED); jce_fs_set_active_reader(NULL);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_PRESENT, jce_fs_probe_read("asset.json")); TEST_ASSERT_EQUAL_UINT32(1, native_queries);
}
static void metadata_oom_or_invalid_utf8_cannot_claim_absence(void)
{
#if JCE_PLATFORM_WINDOWS
    reject_allocation = true;
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_UNKNOWN, jce_fs_probe_read("asset.json"));
    TEST_ASSERT_EQUAL_UINT32(0, native_queries);
    reject_allocation = false;
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_UNKNOWN, jce_fs_probe_read("\xff"));
    TEST_ASSERT_EQUAL_UINT32(0, native_queries);
#else
    native_denied = true;
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_UNKNOWN, jce_fs_probe_read("asset.json"));
#endif
}
static void unavailable_builtin_lock_refuses_metadata_instead_of_absence(void)
{
    jce_fs_set_active(&fs); s_fs_mutex = NULL;
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_UNKNOWN, jce_fs_probe_read("asset.json"));
    TEST_ASSERT_EQUAL_UINT32(0, virtual_queries); TEST_ASSERT_EQUAL_UINT32(0, pak_queries);
    TEST_ASSERT_EQUAL_UINT32(0, native_queries);
}
static void empty_virtual_search_path_reaches_mounted_pak(void)
{
    virtual_active = pak_found = true; virtual_sources = false;
    fs.pak_count = 1; fs.paks[0].pak = (JcePakArchive *)&pak_token;
    jce_fs_set_active_policy(&fs, JCE_FS_ACTIVE_ISOLATED);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_PRESENT, jce_fs_probe_read("asset.json"));
    TEST_ASSERT_EQUAL_UINT32(1, search_queries); TEST_ASSERT_EQUAL_UINT32(1, virtual_queries);
    TEST_ASSERT_EQUAL_UINT32(2, virtual_error_reads); TEST_ASSERT_EQUAL_UINT32(1, pak_queries);
    TEST_ASSERT_EQUAL_UINT32(0, native_queries); TEST_ASSERT_EQUAL_UINT32(0, allocations);
}
static void stale_tls_error_does_not_poison_empty_search_path(void)
{
    virtual_active = pak_found = true; virtual_sources = false; virtual_error = PHYSFS_ERR_PERMISSION;
    fs.pak_count = 1; fs.paks[0].pak = (JcePakArchive *)&pak_token; jce_fs_set_active(&fs);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_PRESENT, jce_fs_probe_read("asset.json"));
    TEST_ASSERT_EQUAL_UINT32(2, virtual_error_reads); TEST_ASSERT_EQUAL_UINT32(1, pak_queries);
    TEST_ASSERT_EQUAL_UINT32(0, native_queries);
}
static void fresh_query_uncertainty_cannot_be_masked_by_pak(void)
{
    const PHYSFS_ErrorCode errors[] = {PHYSFS_ERR_BAD_FILENAME, PHYSFS_ERR_OUT_OF_MEMORY, PHYSFS_ERR_PERMISSION};
    virtual_active = pak_found = true; virtual_sources = false;
    fs.pak_count = 1; fs.paks[0].pak = (JcePakArchive *)&pak_token; jce_fs_set_active(&fs);
    for (unsigned n = 0; n < sizeof(errors) / sizeof(errors[0]); ++n) {
        virtual_query_error = errors[n];
        TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_UNKNOWN, jce_fs_probe_read("asset.json"));
    }
    virtual_sources = virtual_silent_failure = true; virtual_query_error = PHYSFS_ERR_OK;
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_UNKNOWN, jce_fs_probe_read("asset.json"));
    TEST_ASSERT_EQUAL_UINT32(4, search_queries); TEST_ASSERT_EQUAL_UINT32(4, virtual_queries);
    TEST_ASSERT_EQUAL_UINT32(8, virtual_error_reads); TEST_ASSERT_EQUAL_UINT32(0, pak_queries);
    TEST_ASSERT_EQUAL_UINT32(0, native_queries);
}
static void query_trace_records_presence_without_read_success(void)
{
    native_missing = true; TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_ABSENT, jce_fs_probe_read("asset.json"));
    JceTraceEvent events[64]; JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    const uint32_t count = jce_trace_read(&cursor, events, 64); unsigned outcomes = 0, values = 0;
    for (uint32_t n = 0; n < count; ++n) {
        if (events[n].type == JCE_TRACE_EVENT_TASK_END && strcmp(events[n].name, "fs.probe_read") == 0) {
            TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_SUCCEEDED, events[n].value_u32[0]); ++outcomes;
        }
        if (events[n].type == JCE_TRACE_EVENT_COUNTER && strcmp(events[n].name, "fs.probe_read.presence") == 0) {
            TEST_ASSERT_EQUAL_UINT64(JCE_FS_PRESENCE_ABSENT, events[n].value_u64); ++values;
        }
        TEST_ASSERT_FALSE_MESSAGE(strcmp(events[n].name, "fs.read_all") == 0, "Metadata must not read content");
    }
    TEST_ASSERT_EQUAL_UINT32(1, outcomes); TEST_ASSERT_EQUAL_UINT32(1, values); TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(native_regular_is_present_without_file_read); RUN_TEST(native_not_found_is_absent);
    RUN_TEST(native_access_failure_is_unknown); RUN_TEST(native_nonregular_is_unknown);
    RUN_TEST(null_and_empty_never_query_metadata); RUN_TEST(absolute_host_path_bypasses_isolated_custom_reader);
    RUN_TEST(builtin_virtual_regular_precedes_host); RUN_TEST(mounted_pak_presence_never_decompresses);
    RUN_TEST(normalized_virtual_miss_follows_overlay_or_isolated_policy); RUN_TEST(uncertain_virtual_source_cannot_be_masked_by_fallbacks);
    RUN_TEST(missing_provider_is_unknown_for_mounted_content); RUN_TEST(custom_reader_without_probe_is_unknown_and_not_called);
    RUN_TEST(any_reader_replacement_revokes_builtin_probe); RUN_TEST(null_reader_retains_current_host_only_semantics);
    RUN_TEST(metadata_oom_or_invalid_utf8_cannot_claim_absence); RUN_TEST(unavailable_builtin_lock_refuses_metadata_instead_of_absence);
    RUN_TEST(empty_virtual_search_path_reaches_mounted_pak);
    RUN_TEST(stale_tls_error_does_not_poison_empty_search_path);
    RUN_TEST(fresh_query_uncertainty_cannot_be_masked_by_pak);
    RUN_TEST(query_trace_records_presence_without_read_success);
    return UNITY_END();
}
