/* Exact host-FS owner, real SDL/files, only bounded failure/counting ports.
 * This checks native file preservation, not power-loss durability or GPU work. */
#include "unity.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_trace.h>
#include <jce/os/core/jce_path.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include "os/core/jce_memory.h"
#include "os/core/jce_trace_scope_internal.h"
#include "os/core/jce_trace_capture_internal.h"
#include "os/core/jce_log_internal.h"
#include <SDL3/SDL.h>
#include <limits.h>
#include "os/core/jce_filesystem_host_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef JCE_TEST_FS_HOST_FILE_PREFIX
#define JCE_TEST_FS_HOST_FILE_PREFIX "_ut_fs_atomic"
#endif
#ifndef JCE_TEST_FS_HOST_OWNER
#define JCE_TEST_FS_HOST_OWNER "../../../engine/src/os/core/jce_filesystem_host.c"
#endif

static char fixture_root[1024], destination[1100], staging[1105], owned_prefix[1024];
static bool root_created;
static unsigned rename_calls, old_remove_calls, temp_remove_calls, close_calls;
static bool armed, reject_rename, reject_first_rename, reject_close, reject_cleanup;
static bool reject_write;
static bool read_budget_armed;
static bool reject_read_alloc;
static Sint64 reported_read_size;
static unsigned size_calls, read_calls, read_alloc_calls;
static size_t requested_read_size, requested_alloc_size;

static bool copy_armed, copy_reject_open, copy_reject_size, copy_reject_alloc;
static bool copy_short_read, copy_short_write, copy_eof_error, copy_grew;
static bool copy_reject_source_close, copy_reject_destination_close, copy_race;
static SDL_IOStream *copy_source, *copy_destination;
static unsigned copy_source_closes, copy_destination_closes, copy_writes, copy_allocs;
static size_t copy_max_read, copy_max_write, copy_max_alloc;

/* Controlled provider failure and later TLS overwrite; actual owner below. */
static bool detail_fail_open, detail_error_from_path, detail_clobber_generation;
static bool detail_threaded, detail_logger_started, detail_sink_reenter;
static const char *detail_open_error;
static SDL_AtomicInt detail_error_calls, detail_record_count;
static JceFsHostOpenFailureContext detail_context, detail_foreign_context;
static unsigned detail_nested_reads;
static char detail_records[132][512];
static const char *counted_error(void);
static uint32_t counted_generation(void);
static unsigned request_hash_calls;
static uint64_t counted_request_hash(const char *path);
static bool counted_rename(const char *source, const char *target);
static bool counted_remove(const char *path);
static bool counted_close(SDL_IOStream *io);
static size_t counted_write(SDL_IOStream *io, const void *data, size_t size);
static Sint64 counted_size(SDL_IOStream *io);
static size_t counted_read(SDL_IOStream *io, void *data, size_t size);
static void *counted_read_alloc(size_t size);
static SDL_IOStream *counted_open(const char *path, const char *mode);
static SDL_IOStatus counted_status(SDL_IOStream *io);

/* All other calls, including IO and actual rename on successful controls,
 * remain real SDL. Include the exact owner once; do not link a second owner TU. */
#define jce_fnv1a64_str counted_request_hash
#define SDL_RenamePath counted_rename
#define SDL_RemovePath counted_remove
#define SDL_CloseIO counted_close
#define SDL_WriteIO counted_write
#define SDL_GetIOSize counted_size
#define SDL_ReadIO counted_read
#define SDL_IOFromFile counted_open
#define SDL_GetError counted_error
#define jce_trace_capture_generation counted_generation
#define SDL_GetIOStatus counted_status
#define jce__malloc_tracked counted_read_alloc
/* All dependencies above retain native declarations. Only the actual owner
 * below models 32-bit length arithmetic; this is not a 32-bit OS/ABI run. */
#ifdef JCE_TEST_FS_HOST_SIZE32
#undef SIZE_MAX
#define SIZE_MAX UINT32_MAX
#define size_t uint32_t
#endif
#include JCE_TEST_FS_HOST_OWNER
#ifdef JCE_TEST_FS_HOST_SIZE32
#undef size_t
#undef SIZE_MAX
#endif
#undef jce_fnv1a64_str
#undef SDL_RenamePath
#undef SDL_RemovePath
#undef SDL_CloseIO
#undef SDL_WriteIO
#undef SDL_GetIOSize
#undef SDL_ReadIO
#undef SDL_IOFromFile
#undef SDL_GetError
#undef jce_trace_capture_generation
#undef SDL_GetIOStatus
#undef jce__malloc_tracked

static uint64_t counted_request_hash(const char *path)
{
    if (!detail_threaded) ++request_hash_calls;
    return jce_fnv1a64_str(path);
}
static const char *counted_error(void)
{
    SDL_AddAtomicInt(&detail_error_calls, 1);
    return SDL_GetError();
}
static uint32_t counted_generation(void)
{
    uint32_t generation = jce_trace_capture_generation();
    if (detail_clobber_generation) SDL_SetError("later dependency replaced TLS error");
    return generation;
}
static bool counted_rename(const char *source, const char *target)
{
    if (armed && strcmp(source, staging) == 0 && strcmp(target, destination) == 0) {
        ++rename_calls;
        if (reject_rename || (reject_first_rename && rename_calls == 1)) return false;
    }
    return SDL_RenamePath(source, target);
}
static bool counted_remove(const char *path)
{
    if (armed && strcmp(path, destination) == 0) ++old_remove_calls;
    if (armed && strcmp(path, staging) == 0) {
        ++temp_remove_calls;
        if (reject_cleanup) return false;
    }
    return SDL_RemovePath(path);
}
static bool counted_close(SDL_IOStream *io)
{
    bool closed = SDL_CloseIO(io);
    if (copy_armed && io == copy_source) {
        ++copy_source_closes;
        if (copy_reject_source_close) return false;
    }
    if (copy_armed && io == copy_destination) {
        ++copy_destination_closes;
        if (copy_reject_destination_close) return false;
    }
    if (armed) {
        ++close_calls;
        if (reject_close) return false;
    }
    return closed;
}
static size_t counted_write(SDL_IOStream *io, const void *data, size_t size)
{
    if (copy_armed && io == copy_destination) {
        ++copy_writes;
        if (size > copy_max_write) copy_max_write = size;
        if (copy_short_write && size) --size;
    }
    /* Use an actual shorter SDL write, not a successful full write mislabeled
     * as failure. The owner must close exactly once and return false. */
    if (armed && reject_write && size) --size;
    return SDL_WriteIO(io, data, size);
}
static Sint64 counted_size(SDL_IOStream *io)
{
    if (copy_armed && io == copy_source) {
        if (copy_reject_size) return -1;
        if (copy_grew) return 6;
    }
    if (read_budget_armed) {
        ++size_calls;
        return reported_read_size;
    }
    return SDL_GetIOSize(io);
}
static size_t counted_read(SDL_IOStream *io, void *data, size_t size)
{
    if (copy_armed && io == copy_source) {
        if (size > copy_max_read) copy_max_read = size;
        if (copy_short_read && size) --size;
    }
    if (read_budget_armed) {
        ++read_calls;
        requested_read_size = size;
    }
    return SDL_ReadIO(io, data, size);
}
static void *counted_read_alloc(size_t size)
{
    if (copy_armed) {
        ++copy_allocs;
        if (size > copy_max_alloc) copy_max_alloc = size;
        /* Refuse whole-file/oversized mutants without allocating their request. */
        if (copy_reject_alloc || size > 65536u) return NULL;
    }
    if (read_budget_armed) {
        ++read_alloc_calls;
        requested_alloc_size = size;
        if (reject_read_alloc) return NULL;
        /* Never allocate huge buffers. Also reject zero after the old
         * 32-bit +1 wrap, so that mutant fails assertions without corrupt IO. */
        if (!size || size > 64) return NULL;
    }
    return jce__malloc_tracked(size);
}
static SDL_IOStream *counted_open(const char *path, const char *mode)
{
    if (detail_fail_open && strcmp(mode, "rb") == 0) {
        SDL_SetError("%s", detail_error_from_path ? path : detail_open_error);
        return NULL;
    }
    if (copy_armed && strcmp(path, destination) == 0 && strcmp(mode, "rb") == 0) {
        if (copy_reject_open) return NULL;
        copy_source = SDL_IOFromFile(path, mode);
        return copy_source;
    }
    if (copy_armed && strcmp(path, staging) == 0 && strcmp(mode, "wbx") == 0) {
        if (copy_race) {
            /* Real competing creation at the exclusive-open boundary. */
            SDL_IOStream *competitor = SDL_IOFromFile(path, "wb");
            TEST_ASSERT_NOT_NULL(competitor);
            TEST_ASSERT_EQUAL_UINT(8, SDL_WriteIO(competitor, "RACEDATA", 8));
            TEST_ASSERT_TRUE(SDL_CloseIO(competitor));
        }
        copy_destination = SDL_IOFromFile(path, mode);
        return copy_destination;
    }
    return SDL_IOFromFile(path, mode);
}
static SDL_IOStatus counted_status(SDL_IOStream *io)
{
    if (copy_armed && io == copy_source && copy_eof_error) return SDL_IO_STATUS_ERROR;
    return SDL_GetIOStatus(io);
}
static void begin_copy_ports(void)
{
    copy_armed = true;
    copy_reject_open = copy_reject_size = copy_reject_alloc = false;
    copy_short_read = copy_short_write = copy_eof_error = copy_grew = false;
    copy_reject_source_close = copy_reject_destination_close = copy_race = false;
    copy_source = copy_destination = NULL;
    copy_source_closes = copy_destination_closes = copy_writes = copy_allocs = 0;
    copy_max_read = copy_max_write = copy_max_alloc = 0;
}

static bool root_is_owned(void)
{
    size_t length = strlen(owned_prefix);
    return length > 0 && strncmp(fixture_root, owned_prefix, length) == 0 &&
        fixture_root[length] != '\0' && strchr(fixture_root + length, '/') == NULL &&
        strchr(fixture_root + length, '\\') == NULL;
}
void setUp(void)
{
    static unsigned sequence;
    char cwd[900], base[900], desired[1100];
    jce_trace_set_enabled(false);
    jce_trace_task_restore_id(0);
    jce_trace_reset();
    request_hash_calls = 0;
    detail_fail_open = detail_error_from_path = detail_clobber_generation = false;
    detail_threaded = detail_logger_started = detail_sink_reenter = false;
    detail_open_error = "provider refused OPEN";
    detail_nested_reads = 0;
    SDL_SetAtomicInt(&detail_error_calls, 0);
    SDL_SetAtomicInt(&detail_record_count, 0);
    memset(detail_records, 0, sizeof detail_records);
    jce__fs_host_open_failure_detail_begin(&detail_context, false);
    root_created = false; fixture_root[0] = owned_prefix[0] = '\0';
    TEST_ASSERT_TRUE(jce_fs_host_get_current_dir(cwd, sizeof cwd));
    TEST_ASSERT_TRUE(jce_path_normalize(base, sizeof base, cwd));
    int length = snprintf(owned_prefix, sizeof owned_prefix, "%s/%s_", base,
                           JCE_TEST_FS_HOST_FILE_PREFIX);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof owned_prefix);
    snprintf(desired, sizeof desired, "%s%u", owned_prefix, ++sequence);
    /* Each registered target has its own namespace; candidate discovery is
     * not an atomic reservation against noncooperating concurrent callers. */
    TEST_ASSERT_TRUE(jce_fs_host_make_unique_path(desired, fixture_root, sizeof fixture_root));
    TEST_ASSERT_TRUE(root_is_owned());
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(fixture_root));
    TEST_ASSERT_FALSE(jce_fs_host_exists_dir(fixture_root));
    armed = reject_rename = reject_first_rename = reject_close = reject_cleanup = false;
    copy_armed = false;
    reject_write = false;
    read_budget_armed = false;
    reject_read_alloc = false;
    reported_read_size = 0;
    size_calls = read_calls = read_alloc_calls = 0;
    requested_read_size = requested_alloc_size = 0;
    rename_calls = old_remove_calls = temp_remove_calls = close_calls = 0;
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(fixture_root));
    root_created = true;
    /* A real Unicode path also verifies the underlying SDL UTF-8 owner. */
    snprintf(destination, sizeof destination, "%s/scene_\xe6\xb5\x8b\xe8\xaf\x95.json", fixture_root);
    snprintf(staging, sizeof staging, "%s.tmp", destination);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(destination, "OLDDATA", 7));
}
void tearDown(void)
{
    if (detail_logger_started) {
        detail_sink_reenter = false;
        jce_log_quiesce_internal();
        jce__fs_host_open_failure_detail_end(&detail_context);
        jce__fs_host_open_failure_detail_end(&detail_foreign_context);
        jce_log_flush();
        jce_log_set_sink(NULL, NULL);
        jce_log_shutdown();
        detail_logger_started = false;
    }
    jce_trace_set_enabled(false);
    jce_trace_task_restore_id(0);
    jce_fs_set_active(NULL);
    armed = false;
    read_budget_armed = false;
    copy_armed = false;
    if (root_created) {
        bool owned = root_is_owned(); TEST_ASSERT_TRUE(owned);
        if (owned) TEST_ASSERT_TRUE(jce_fs_host_remove_recursive(fixture_root));
    }
    root_created = false;
}
static void expect_bytes(const char *bytes, size_t count)
{
    armed = false;
    uint64_t size = 0;
    void *actual = jce_fs_host_read_all(destination, &size);
    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT_EQUAL_UINT64(count, size);
    if (count) TEST_ASSERT_EQUAL_MEMORY(bytes, actual, count);
    jce_fs_buffer_free(actual);
}
static void rename_failure_preserves_previous_file_and_cleans_once(void)
{
    armed = reject_rename = true;
    TEST_ASSERT_FALSE(jce_fs_host_write_all_atomic(destination, "NEW", 3));
    TEST_ASSERT_EQUAL_UINT(1, rename_calls);
    TEST_ASSERT_EQUAL_UINT(0, old_remove_calls);
    TEST_ASSERT_EQUAL_UINT(1, temp_remove_calls);
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
    expect_bytes("OLDDATA", 7);
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(staging));
}
static void transient_rename_failure_never_unlinks_and_retries(void)
{
    armed = reject_first_rename = true;
    TEST_ASSERT_FALSE(jce_fs_host_write_all_atomic(destination, "NEW", 3));
    TEST_ASSERT_EQUAL_UINT(1, rename_calls);
    TEST_ASSERT_EQUAL_UINT(0, old_remove_calls);
    TEST_ASSERT_EQUAL_UINT(1, temp_remove_calls);
    expect_bytes("OLDDATA", 7);
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(staging));
}
static void close_failure_never_publishes_the_staging_file(void)
{
    armed = reject_close = true;
    TEST_ASSERT_FALSE(jce_fs_host_write_all_atomic(destination, "NEW", 3));
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
    TEST_ASSERT_EQUAL_UINT(0, rename_calls);
    TEST_ASSERT_EQUAL_UINT(0, old_remove_calls);
    TEST_ASSERT_EQUAL_UINT(1, temp_remove_calls);
    expect_bytes("OLDDATA", 7);
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(staging));
}
static void cleanup_failure_keeps_previous_file_and_does_not_retry(void)
{
    armed = reject_rename = reject_cleanup = true;
    TEST_ASSERT_FALSE(jce_fs_host_write_all_atomic(destination, "NEW", 3));
    TEST_ASSERT_EQUAL_UINT(1, rename_calls);
    TEST_ASSERT_EQUAL_UINT(0, old_remove_calls);
    TEST_ASSERT_EQUAL_UINT(1, temp_remove_calls);
    expect_bytes("OLDDATA", 7);
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(staging));
}
static void successful_real_replace_commits_once_without_unlink(void)
{
    armed = true;
    TEST_ASSERT_TRUE(jce_fs_host_write_all_atomic(destination, "NEW", 3));
    TEST_ASSERT_EQUAL_UINT(1, rename_calls);
    TEST_ASSERT_EQUAL_UINT(0, old_remove_calls);
    TEST_ASSERT_EQUAL_UINT(0, temp_remove_calls);
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
    expect_bytes("NEW", 3);
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(staging));
}
static void zero_length_real_replace_and_create_are_supported(void)
{
    armed = true;
    TEST_ASSERT_TRUE(jce_fs_host_write_all_atomic(destination, NULL, 0));
    expect_bytes("", 0);
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(staging));
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(destination));
    armed = true;
    TEST_ASSERT_TRUE(jce_fs_host_write_all_atomic(destination, "CREATED", 7));
    expect_bytes("CREATED", 7);
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(staging));
}
static void invalid_input_does_not_touch_previous_file(void)
{
    armed = true;
    TEST_ASSERT_FALSE(jce_fs_host_write_all_atomic(destination, NULL, 3));
    TEST_ASSERT_FALSE(jce_fs_host_write_all_atomic(NULL, "NEW", 3));
    TEST_ASSERT_FALSE(jce_fs_host_write_all_atomic("", "NEW", 3));
    TEST_ASSERT_EQUAL_UINT(0, rename_calls);
    TEST_ASSERT_EQUAL_UINT(0, old_remove_calls);
    TEST_ASSERT_EQUAL_UINT(0, temp_remove_calls);
    TEST_ASSERT_EQUAL_UINT(0, close_calls);
    expect_bytes("OLDDATA", 7);
}
static void append_close_failure_is_not_success_and_closes_once(void)
{
    armed = reject_close = true;
    TEST_ASSERT_FALSE(jce_fs_host_append(destination, "APPEND", 6));
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
    TEST_ASSERT_EQUAL_UINT(0, rename_calls);
    TEST_ASSERT_EQUAL_UINT(0, old_remove_calls);
    /* A close failure does not promise rollback of bytes already accepted. */
    expect_bytes("OLDDATAAPPEND", 13);
}
static void append_short_write_is_rejected_without_retry_and_closes_once(void)
{
    armed = reject_write = true;
    TEST_ASSERT_FALSE(jce_fs_host_append(destination, "APPEND", 6));
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
    TEST_ASSERT_EQUAL_UINT(0, rename_calls);
    expect_bytes("OLDDATAAPPEN", 12);
}
static void append_success_and_empty_append_use_one_real_close(void)
{
    armed = true;
    TEST_ASSERT_TRUE(jce_fs_host_append(destination, "APPEND", 6));
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
    expect_bytes("OLDDATAAPPEND", 13);
    armed = true;
    TEST_ASSERT_TRUE(jce_fs_host_append(destination, NULL, 0));
    TEST_ASSERT_EQUAL_UINT(2, close_calls);
    expect_bytes("OLDDATAAPPEND", 13);
}
static void exclusive_reservation_refuses_real_existing_file_and_directory(void)
{
    bool created = true;
    armed = true;
    TEST_ASSERT_FALSE(jce__fs_host_reserve_new(destination, "PENDING", 7, &created));
    TEST_ASSERT_FALSE(created);
    TEST_ASSERT_EQUAL_UINT(0, close_calls);
    expect_bytes("OLDDATA", 7);
    armed = true;
    TEST_ASSERT_FALSE(jce__fs_host_reserve_new(fixture_root, "PENDING", 7, &created));
    TEST_ASSERT_FALSE(created);
    TEST_ASSERT_EQUAL_UINT(0, close_calls);
    TEST_ASSERT_TRUE(jce_fs_host_exists_dir(fixture_root));
}
static void exclusive_reservation_short_write_and_close_failure_retain_created_file(void)
{
    bool created = false;
    armed = reject_write = true;
    TEST_ASSERT_FALSE(jce__fs_host_reserve_new(staging, "PENDING", 7, &created));
    TEST_ASSERT_TRUE(created);
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
    armed = false;
    uint64_t size = 0;
    void *bytes = jce_fs_host_read_all(staging, &size);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(6, size);
    TEST_ASSERT_EQUAL_MEMORY("PENDIN", bytes, 6);
    jce_fs_buffer_free(bytes);
    /* The fixture removes only its own failed file before another independent
     * reserve test. Production retains failed marker/partial evidence. */
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(staging));
    armed = reject_close = true;
    reject_write = false;
    TEST_ASSERT_FALSE(jce__fs_host_reserve_new(staging, "PENDING", 7, &created));
    TEST_ASSERT_TRUE(created);
    TEST_ASSERT_EQUAL_UINT(2, close_calls);
    armed = false;
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(staging));
    TEST_ASSERT_EQUAL_UINT(0, temp_remove_calls);
    expect_bytes("OLDDATA", 7);
}
static void exclusive_reservation_success_and_invalid_input_keep_ownership_explicit(void)
{
    bool created = true;
    armed = true;
    TEST_ASSERT_FALSE(jce__fs_host_reserve_new(staging, NULL, 1, &created));
    TEST_ASSERT_FALSE(created);
    TEST_ASSERT_FALSE(jce__fs_host_reserve_new(staging, "PENDING", 7, NULL));
    TEST_ASSERT_EQUAL_UINT(0, close_calls);
    TEST_ASSERT_TRUE(jce__fs_host_reserve_new(staging, "PENDING", 7, &created));
    TEST_ASSERT_TRUE(created);
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
    created = true;
    TEST_ASSERT_FALSE(jce__fs_host_reserve_new(staging, "OTHER", 5, &created));
    TEST_ASSERT_FALSE(created);
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
    TEST_ASSERT_EQUAL_UINT(0, temp_remove_calls);
    expect_bytes("OLDDATA", 7);
}
static void public_write_new_uses_exclusive_owner_and_preserves_failure_evidence(void)
{
    armed = true;
    TEST_ASSERT_FALSE(jce_fs_host_write_new(destination, "NEW", 3));
    TEST_ASSERT_FALSE(jce_fs_host_write_new(fixture_root, "NEW", 3));
    TEST_ASSERT_EQUAL_UINT(0, close_calls);
    expect_bytes("OLDDATA", 7);
    armed = true;
    TEST_ASSERT_TRUE(jce_fs_host_write_new(staging, "NEW", 3));
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
    TEST_ASSERT_FALSE(jce_fs_host_write_new(staging, "OTHER", 5));
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
    armed = false;
    uint64_t size = 0;
    void *bytes = jce_fs_host_read_all(staging, &size);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(3, size);
    TEST_ASSERT_EQUAL_MEMORY("NEW", bytes, 3);
    jce_fs_buffer_free(bytes);

    char failing[1100];
    snprintf(failing, sizeof(failing), "%s/public-short.log", fixture_root);
    armed = reject_write = true;
    TEST_ASSERT_FALSE(jce_fs_host_write_new(failing, "NEW", 3));
    TEST_ASSERT_EQUAL_UINT(2, close_calls);
    armed = false;
    bytes = jce_fs_host_read_all(failing, &size);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(2, size);
    TEST_ASSERT_EQUAL_MEMORY("NE", bytes, 2);
    jce_fs_buffer_free(bytes);
    snprintf(failing, sizeof(failing), "%s/public-close.log", fixture_root);
    reject_write = false;
    armed = reject_close = true;
    TEST_ASSERT_FALSE(jce_fs_host_write_new(failing, "NEW", 3));
    TEST_ASSERT_EQUAL_UINT(3, close_calls);
    armed = false;
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(failing));
    TEST_ASSERT_FALSE(jce_fs_host_write_new(NULL, "NEW", 3));
    TEST_ASSERT_FALSE(jce_fs_host_write_new(failing, NULL, 3));
    TEST_ASSERT_EQUAL_UINT(0, old_remove_calls);
    TEST_ASSERT_EQUAL_UINT(0, temp_remove_calls);
    expect_bytes("OLDDATA", 7);
}
static void persistent_stream_writes_multiple_batches_and_closes_once(void)
{
    bool created = false;
    armed = true;
    SDL_IOStream *io = jce__fs_host_stream_new(staging, &created);
    TEST_ASSERT_NOT_NULL(io);
    TEST_ASSERT_TRUE(created);
    TEST_ASSERT_TRUE(jce__fs_host_stream_write(io, "HEADER", 6));
    TEST_ASSERT_TRUE(jce__fs_host_stream_write(io, "PAYLOAD", 7));
    TEST_ASSERT_TRUE(jce__fs_host_stream_write(io, "FOOTER", 6));
    TEST_ASSERT_EQUAL_UINT(0, close_calls);
    TEST_ASSERT_TRUE(jce__fs_host_stream_close(&io));
    TEST_ASSERT_NULL(io);
    TEST_ASSERT_FALSE(jce__fs_host_stream_close(&io));
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
    armed = false;
    uint64_t size = 0;
    char *bytes = jce_fs_host_read_all(staging, &size);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(19, size);
    TEST_ASSERT_EQUAL_MEMORY("HEADERPAYLOADFOOTER", bytes, 19);
    jce_fs_buffer_free(bytes);
    expect_bytes("OLDDATA", 7);
}
static void persistent_stream_failure_clears_handle_without_retry_or_removal(void)
{
    bool created = false;
    armed = true;
    SDL_IOStream *io = jce__fs_host_stream_new(staging, &created);
    TEST_ASSERT_NOT_NULL(io);
    reject_write = true;
    TEST_ASSERT_FALSE(jce__fs_host_stream_write(io, "PAYLOAD", 7));
    reject_close = true;
    TEST_ASSERT_FALSE(jce__fs_host_stream_close(&io));
    TEST_ASSERT_NULL(io);
    TEST_ASSERT_FALSE(jce__fs_host_stream_close(&io));
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
    TEST_ASSERT_EQUAL_UINT(0, old_remove_calls);
    TEST_ASSERT_EQUAL_UINT(0, temp_remove_calls);
    TEST_ASSERT_FALSE(jce__fs_host_stream_write(io, "RETRY", 5));
    armed = false;
    uint64_t size = 0;
    char *bytes = jce_fs_host_read_all(staging, &size);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(6, size);
    TEST_ASSERT_EQUAL_MEMORY("PAYLOA", bytes, 6);
    jce_fs_buffer_free(bytes);
    expect_bytes("OLDDATA", 7);
}

static void read_all_real_small_and_empty_files_keep_text_sentinels(void)
{
    uint64_t size = 99;
    armed = true;
    char *bytes = jce_fs_host_read_all(destination, &size);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(7, size);
    TEST_ASSERT_EQUAL_MEMORY("OLDDATA", bytes, 7);
    TEST_ASSERT_EQUAL_CHAR('\0', bytes[7]);
    jce_fs_buffer_free(bytes);
    TEST_ASSERT_EQUAL_UINT(1, close_calls);

    armed = false;
    TEST_ASSERT_TRUE(jce_fs_host_write_all(destination, NULL, 0));
    armed = true;
    bytes = jce_fs_host_read_all(destination, &size);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(0, size);
    TEST_ASSERT_EQUAL_CHAR('\0', bytes[0]);
    jce_fs_buffer_free(bytes);
    TEST_ASSERT_EQUAL_UINT(2, close_calls);
}
static void read_all_allocation_refusal_closes_without_reading(void)
{
    uint64_t size = 99;
    armed = read_budget_armed = reject_read_alloc = true;
    reported_read_size = 7;
    void *bytes = jce_fs_host_read_all(destination, &size);
    jce_fs_buffer_free(bytes);
    TEST_ASSERT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(0, size);
    TEST_ASSERT_EQUAL_UINT(1, size_calls);
    TEST_ASSERT_EQUAL_UINT(1, read_alloc_calls);
    TEST_ASSERT_EQUAL_UINT64(8, requested_alloc_size);
    TEST_ASSERT_EQUAL_UINT(0, read_calls);
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
}
static void read_all_unknown_size_closes_without_allocation(void)
{
    uint64_t size = 99;
    armed = read_budget_armed = true;
    reported_read_size = -1;
    void *bytes = jce_fs_host_read_all(destination, &size);
    jce_fs_buffer_free(bytes);
    TEST_ASSERT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(0, size);
    TEST_ASSERT_EQUAL_UINT(1, size_calls);
    TEST_ASSERT_EQUAL_UINT(0, read_alloc_calls);
    TEST_ASSERT_EQUAL_UINT(0, read_calls);
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
}
#ifdef JCE_TEST_FS_HOST_SIZE32
static void read_all_size32_last_safe_length_reaches_allocator_once(void)
{
    uint64_t size = 99;
    armed = read_budget_armed = true;
    reported_read_size = (Sint64)UINT32_MAX - 1;
    void *bytes = jce_fs_host_read_all(destination, &size);
    jce_fs_buffer_free(bytes);
    TEST_ASSERT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(0, size);
    TEST_ASSERT_EQUAL_UINT(1, size_calls);
    TEST_ASSERT_EQUAL_UINT(1, read_alloc_calls);
    TEST_ASSERT_EQUAL_UINT64(UINT32_MAX, requested_alloc_size);
    TEST_ASSERT_EQUAL_UINT(0, read_calls);
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
}
static void read_all_size32_unrepresentable_lengths_never_allocate(void)
{
    const uint64_t lengths[] = {UINT32_MAX, UINT64_C(0x100000000),
                                UINT64_C(0x100000003)};
    for (unsigned i = 0; i < sizeof lengths / sizeof lengths[0]; ++i) {
        uint64_t size = 99;
        armed = read_budget_armed = true;
        reported_read_size = (Sint64)lengths[i];
        size_calls = read_calls = read_alloc_calls = close_calls = 0;
        void *bytes = jce_fs_host_read_all(destination, &size);
        const bool rejected = bytes == NULL;
        jce_fs_buffer_free(bytes);
        TEST_ASSERT_TRUE(rejected);
        TEST_ASSERT_EQUAL_UINT64(0, size);
        TEST_ASSERT_EQUAL_UINT(1, size_calls);
        TEST_ASSERT_EQUAL_UINT(0, read_alloc_calls);
        TEST_ASSERT_EQUAL_UINT(0, read_calls);
        TEST_ASSERT_EQUAL_UINT(1, close_calls);
    }
}
#endif

static void capped_read_real_file_preserves_cap_zero_and_total_size(void)
{
    uint64_t read = 99, total = 99;
    armed = true;
    char *bytes = jce_fs_host_read_capped(destination, 3, &read, &total);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(3, read);
    TEST_ASSERT_EQUAL_UINT64(7, total);
    TEST_ASSERT_EQUAL_MEMORY("OLD", bytes, 3);
    TEST_ASSERT_EQUAL_CHAR('\0', bytes[3]);
    jce_fs_buffer_free(bytes);
    TEST_ASSERT_EQUAL_UINT(1, close_calls);

    bytes = jce_fs_host_read_capped(destination, 0, &read, &total);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(0, read);
    TEST_ASSERT_EQUAL_UINT64(7, total);
    TEST_ASSERT_EQUAL_CHAR('\0', bytes[0]);
    jce_fs_buffer_free(bytes);
    TEST_ASSERT_EQUAL_UINT(2, close_calls);
}
static void capped_read_large_metadata_clamps_before_length_conversion(void)
{
    uint64_t read = 99, total = 99;
    char actual[8] = {0};
    armed = read_budget_armed = true;
    reported_read_size = INT64_C(0x100000003);
    char *bytes = jce_fs_host_read_capped(destination, 7, &read, &total);
    bool loaded = bytes != NULL;
    if (bytes && read <= 7) memcpy(actual, bytes, (size_t)read + 1u);
    jce_fs_buffer_free(bytes);
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_EQUAL_UINT64(7, read);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)reported_read_size, total);
    TEST_ASSERT_EQUAL_MEMORY("OLDDATA", actual, 7);
    TEST_ASSERT_EQUAL_CHAR('\0', actual[7]);
    TEST_ASSERT_EQUAL_UINT(1, size_calls);
    TEST_ASSERT_EQUAL_UINT(1, read_alloc_calls);
    TEST_ASSERT_EQUAL_UINT64(8, requested_alloc_size);
    TEST_ASSERT_EQUAL_UINT(1, read_calls);
    TEST_ASSERT_EQUAL_UINT64(7, requested_read_size);
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
}
static void capped_read_large_metadata_and_zero_cap_avoid_read(void)
{
    uint64_t read = 99, total = 99;
    armed = read_budget_armed = true;
    reported_read_size = INT64_C(0x100000003);
    char *bytes = jce_fs_host_read_capped(destination, 0, &read, &total);
    bool terminated = bytes && bytes[0] == '\0';
    jce_fs_buffer_free(bytes);
    TEST_ASSERT_TRUE(terminated);
    TEST_ASSERT_EQUAL_UINT64(0, read);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)reported_read_size, total);
    TEST_ASSERT_EQUAL_UINT(1, read_alloc_calls);
    TEST_ASSERT_EQUAL_UINT64(1, requested_alloc_size);
    TEST_ASSERT_EQUAL_UINT(0, read_calls);
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
}
static void capped_read_unknown_size_closes_without_allocation(void)
{
    uint64_t read = 99, total = 99;
    armed = read_budget_armed = true;
    reported_read_size = -1;
    void *bytes = jce_fs_host_read_capped(destination, 7, &read, &total);
    jce_fs_buffer_free(bytes);
    TEST_ASSERT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(0, read);
    TEST_ASSERT_EQUAL_UINT64(0, total);
    TEST_ASSERT_EQUAL_UINT(0, read_alloc_calls);
    TEST_ASSERT_EQUAL_UINT(0, read_calls);
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
}
#ifdef JCE_TEST_FS_HOST_SIZE32
static void capped_read_size32_last_safe_length_reaches_allocator_once(void)
{
    uint64_t read = 99, total = 99;
    armed = read_budget_armed = true;
    reported_read_size = (Sint64)UINT32_MAX - 1;
    void *bytes = jce_fs_host_read_capped(destination, UINT64_MAX, &read, &total);
    jce_fs_buffer_free(bytes);
    /* The huge-allocation port refuses; the owner must have admitted the
     * representable SIZE_MAX-byte allocation including its NUL sentinel. */
    TEST_ASSERT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(0, read);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)reported_read_size, total);
    TEST_ASSERT_EQUAL_UINT(1, read_alloc_calls);
    TEST_ASSERT_EQUAL_UINT64(UINT32_MAX, requested_alloc_size);
    TEST_ASSERT_EQUAL_UINT(0, read_calls);
    TEST_ASSERT_EQUAL_UINT(1, close_calls);
}
static void capped_read_size32_unrepresentable_lengths_never_allocate(void)
{
    const uint64_t lengths[] = {UINT32_MAX, UINT64_C(0x100000000),
                                UINT64_C(0x100000003)};
    for (unsigned i = 0; i < sizeof lengths / sizeof lengths[0]; ++i) {
        uint64_t read = 99, total = 99;
        armed = read_budget_armed = true;
        reported_read_size = (Sint64)lengths[i];
        size_calls = read_calls = read_alloc_calls = close_calls = 0;
        void *bytes = jce_fs_host_read_capped(destination, UINT64_MAX, &read, &total);
        bool rejected = bytes == NULL;
        jce_fs_buffer_free(bytes);
        TEST_ASSERT_TRUE(rejected);
        TEST_ASSERT_EQUAL_UINT64(0, read);
        TEST_ASSERT_EQUAL_UINT64(lengths[i], total);
        TEST_ASSERT_EQUAL_UINT(1, size_calls);
        TEST_ASSERT_EQUAL_UINT(0, read_alloc_calls);
        TEST_ASSERT_EQUAL_UINT(0, read_calls);
        TEST_ASSERT_EQUAL_UINT(1, close_calls);
    }
}
#endif

/* The complete owner emits into the real shared ring; counted ports only
 * bound the SDL size/allocation/close outcomes and observe hash calls. */
static JceTraceEvent read_trace_events[128];
static void begin_read_trace(void)
{
    jce_trace_set_enabled(true);
    jce_trace_reset();
    request_hash_calls = 0;
}
static const JceTraceEvent *read_counter(uint32_t count, uint64_t id,
                                        const char *name)
{
    const JceTraceEvent *match = NULL;
    for (uint32_t i = 0; i < count; ++i)
        if (read_trace_events[i].type == JCE_TRACE_EVENT_COUNTER &&
            read_trace_events[i].id == id && strcmp(read_trace_events[i].name, name) == 0) {
            TEST_ASSERT_NULL(match);
            match = &read_trace_events[i];
        }
    TEST_ASSERT_NOT_NULL(match);
    return match;
}
static void expect_read_counter(uint32_t count, uint64_t id, uint64_t parent,
                                 const char *name, uint32_t expected)
{
    const JceTraceEvent *event = read_counter(count, id, name);
    TEST_ASSERT_EQUAL_UINT64(parent, event->parent_id);
    TEST_ASSERT_EQUAL_INT(JCE_TRACE_CATEGORY_CORE, event->category);
    TEST_ASSERT_EQUAL_UINT64(expected, event->value_u64);
}
static void expect_read_pair(uint32_t count, uint64_t id, uint64_t parent,
                              const char *hi, const char *lo, uint64_t expected)
{
    expect_read_counter(count, id, parent, hi, (uint32_t)(expected >> 32));
    expect_read_counter(count, id, parent, lo, (uint32_t)expected);
}
static uint64_t expect_read_trace(const char *service, const char *request,
        uint32_t known, uint64_t total, uint64_t requested, uint64_t cap,
        uint64_t actual, uint32_t failure, uint32_t close, uint32_t short_read,
        JceTraceTaskState state, uint64_t parent)
{
    JceTraceCursor cursor;
    jce_trace_cursor_init(&cursor);
    uint32_t count = jce_trace_read(&cursor, read_trace_events, 128);
    const JceTraceEvent *submit = NULL, *end = NULL;
    uint32_t detail_count = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const JceTraceEvent *event = &read_trace_events[i];
        /* Only fixed names are allowed; no absolute request or SDL error text. */
        TEST_ASSERT_NULL(strstr(event->name, fixture_root));
        if (strcmp(event->name, service) == 0) {
            if (event->type == JCE_TRACE_EVENT_TASK_SUBMIT) submit = event;
            if (event->type == JCE_TRACE_EVENT_TASK_END) end = event;
        }
    }
    TEST_ASSERT_NOT_NULL(submit); TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_UINT64(parent, submit->parent_id);
    TEST_ASSERT_EQUAL_UINT64(submit->id, end->id);
    TEST_ASSERT_EQUAL_INT(state, end->value_u32[0]);
    for (uint32_t i = 0; i < count; ++i)
        if (read_trace_events[i].id == submit->id &&
            read_trace_events[i].type == JCE_TRACE_EVENT_COUNTER) ++detail_count;
    TEST_ASSERT_EQUAL_UINT32(15, detail_count);
    uint64_t hash = request ? jce_fnv1a64_str(request) : 0;
    expect_read_pair(count, submit->id, parent, "fs.read.path_hash_hi", "fs.read.path_hash_lo", hash);
    expect_read_pair(count, submit->id, parent, "fs.read.total_hi", "fs.read.total_lo", total);
    expect_read_pair(count, submit->id, parent, "fs.read.requested_hi", "fs.read.requested_lo", requested);
    expect_read_pair(count, submit->id, parent, "fs.read.cap_hi", "fs.read.cap_lo", cap);
    expect_read_pair(count, submit->id, parent, "fs.read.actual_hi", "fs.read.actual_lo", actual);
    expect_read_counter(count, submit->id, parent, "fs.read.dispatch", failure == JCE_FS_HOST_READ_INVALID
        ? JCE_FS_HOST_READ_NONE : JCE_FS_HOST_READ_HOST);
    expect_read_counter(count, submit->id, parent, "fs.read.known", known);
    expect_read_counter(count, submit->id, parent, "fs.read.failure_stage", failure);
    expect_read_counter(count, submit->id, parent, "fs.read.close", close);
    expect_read_counter(count, submit->id, parent, "fs.read.short", short_read);
    TEST_ASSERT_EQUAL_UINT64(parent, jce_trace_task_current_id());
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    return submit->id;
}
static void read_trace_real_success_close_refusal_preserves_success_and_parent(void)
{
    begin_read_trace();
    uint64_t parent = jce_trace_next_id(), size = 99;
    jce_trace_task_restore_id(parent);
    armed = reject_close = true;
    char *bytes = jce_fs_host_read_all(destination, &size);
    TEST_ASSERT_NOT_NULL(bytes); TEST_ASSERT_EQUAL_UINT64(7, size);
    TEST_ASSERT_EQUAL_MEMORY("OLDDATA", bytes, 7); TEST_ASSERT_EQUAL_CHAR('\0', bytes[7]);
    jce_fs_buffer_free(bytes);
    TEST_ASSERT_EQUAL_UINT(1, request_hash_calls); TEST_ASSERT_EQUAL_UINT(1, close_calls);
    expect_read_trace("fs.host.read_all", destination, 11, 7, 7, 0, 7,
        JCE_FS_HOST_READ_OK, JCE_FS_HOST_READ_CLOSE_FAILED, JCE_FS_HOST_READ_FULL,
        JCE_TRACE_TASK_SUCCEEDED, parent);
    jce_trace_task_restore_id(0);
}
static void read_trace_invalid_and_open_refusal_have_distinct_first_stages(void)
{
    begin_read_trace();
    uint64_t size = 99;
    TEST_ASSERT_NULL(jce_fs_host_read_all(NULL, &size));
    TEST_ASSERT_EQUAL_UINT64(0, size); TEST_ASSERT_EQUAL_UINT(0, request_hash_calls);
    expect_read_trace("fs.host.read_all", NULL, 0, 0, 0, 0, 0,
        JCE_FS_HOST_READ_INVALID, JCE_FS_HOST_READ_CLOSE_NONE, JCE_FS_HOST_READ_SHORT_NONE,
        JCE_TRACE_TASK_FAILED, 0);
    begin_read_trace();
    char absent[1200]; snprintf(absent, sizeof absent, "%s.absent", destination);
    TEST_ASSERT_NULL(jce_fs_host_read_all(absent, &size));
    TEST_ASSERT_EQUAL_UINT64(0, size); TEST_ASSERT_EQUAL_UINT(1, request_hash_calls);
    expect_read_trace("fs.host.read_all", absent, 8, 0, 0, 0, 0,
        JCE_FS_HOST_READ_OPEN, JCE_FS_HOST_READ_CLOSE_NONE, JCE_FS_HOST_READ_SHORT_NONE,
        JCE_TRACE_TASK_FAILED, 0);
    begin_read_trace();
    uint64_t read = 99, total = 99;
    TEST_ASSERT_NULL(jce_fs_host_read_capped(absent, UINT64_MAX, &read, &total));
    TEST_ASSERT_EQUAL_UINT64(0, read); TEST_ASSERT_EQUAL_UINT64(0, total);
    expect_read_trace("fs.host.read_capped", absent, 12, 0, 0, UINT64_MAX, 0,
        JCE_FS_HOST_READ_OPEN, JCE_FS_HOST_READ_CLOSE_NONE, JCE_FS_HOST_READ_SHORT_NONE,
        JCE_TRACE_TASK_FAILED, 0);
    begin_read_trace();
    TEST_ASSERT_NULL(jce_fs_host_read_capped("", UINT64_MAX, &read, &total));
    TEST_ASSERT_EQUAL_UINT64(0, read); TEST_ASSERT_EQUAL_UINT64(0, total);
    expect_read_trace("fs.host.read_capped", "", 4, 0, 0, UINT64_MAX, 0,
        JCE_FS_HOST_READ_INVALID, JCE_FS_HOST_READ_CLOSE_NONE, JCE_FS_HOST_READ_SHORT_NONE,
        JCE_TRACE_TASK_FAILED, 0);
}
static void read_trace_size_and_alloc_failure_keep_first_stage_after_failed_close(void)
{
    begin_read_trace();
    armed = read_budget_armed = reject_close = true;
    reported_read_size = -1;
    uint64_t size = 99;
    TEST_ASSERT_NULL(jce_fs_host_read_all(destination, &size));
    TEST_ASSERT_EQUAL_UINT64(0, size); TEST_ASSERT_EQUAL_UINT(0, read_alloc_calls);
    expect_read_trace("fs.host.read_all", destination, 8, 0, 0, 0, 0,
        JCE_FS_HOST_READ_SIZE, JCE_FS_HOST_READ_CLOSE_FAILED, JCE_FS_HOST_READ_SHORT_NONE,
        JCE_TRACE_TASK_FAILED, 0);
    begin_read_trace(); close_calls = 0;
    uint64_t read = 99, total = 99;
    TEST_ASSERT_NULL(jce_fs_host_read_capped(destination, 3, &read, &total));
    TEST_ASSERT_EQUAL_UINT64(0, read); TEST_ASSERT_EQUAL_UINT64(0, total);
    expect_read_trace("fs.host.read_capped", destination, 12, 0, 0, 3, 0,
        JCE_FS_HOST_READ_SIZE, JCE_FS_HOST_READ_CLOSE_FAILED, JCE_FS_HOST_READ_SHORT_NONE,
        JCE_TRACE_TASK_FAILED, 0);
    begin_read_trace(); close_calls = 0;
    reported_read_size = 7; reject_read_alloc = true;
    TEST_ASSERT_NULL(jce_fs_host_read_all(destination, &size));
    TEST_ASSERT_EQUAL_UINT64(0, size);
    expect_read_trace("fs.host.read_all", destination, 11, 7, 7, 0, 0,
        JCE_FS_HOST_READ_ALLOC, JCE_FS_HOST_READ_CLOSE_FAILED, JCE_FS_HOST_READ_SHORT_NONE,
        JCE_TRACE_TASK_FAILED, 0);
    begin_read_trace(); close_calls = 0;
    TEST_ASSERT_NULL(jce_fs_host_read_capped(destination, 3, &read, &total));
    TEST_ASSERT_EQUAL_UINT64(0, read); TEST_ASSERT_EQUAL_UINT64(7, total);
    TEST_ASSERT_EQUAL_UINT(1, close_calls); TEST_ASSERT_EQUAL_UINT(0, read_calls);
    expect_read_trace("fs.host.read_capped", destination, 15, 7, 3, 3, 0,
        JCE_FS_HOST_READ_ALLOC, JCE_FS_HOST_READ_CLOSE_FAILED, JCE_FS_HOST_READ_SHORT_NONE,
        JCE_TRACE_TASK_FAILED, 0);
}
static void read_trace_short_full_read_fails_but_capped_short_read_stays_success(void)
{
    begin_read_trace();
    armed = read_budget_armed = reject_close = true;
    reported_read_size = 9; uint64_t size = 99;
    TEST_ASSERT_NULL(jce_fs_host_read_all(destination, &size));
    TEST_ASSERT_EQUAL_UINT64(0, size); TEST_ASSERT_EQUAL_UINT(1, close_calls);
    expect_read_trace("fs.host.read_all", destination, 11, 9, 9, 0, 7,
        JCE_FS_HOST_READ_SHORT, JCE_FS_HOST_READ_CLOSE_FAILED, JCE_FS_HOST_READ_SHORT_OBSERVED,
        JCE_TRACE_TASK_FAILED, 0);
    begin_read_trace(); close_calls = 0;
    uint64_t read = 99, total = 99;
    char *bytes = jce_fs_host_read_capped(destination, 9, &read, &total);
    TEST_ASSERT_NOT_NULL(bytes); TEST_ASSERT_EQUAL_UINT64(7, read); TEST_ASSERT_EQUAL_UINT64(9, total);
    TEST_ASSERT_EQUAL_MEMORY("OLDDATA", bytes, 7); TEST_ASSERT_EQUAL_CHAR('\0', bytes[7]);
    jce_fs_buffer_free(bytes); TEST_ASSERT_EQUAL_UINT(1, close_calls);
    expect_read_trace("fs.host.read_capped", destination, 15, 9, 9, 9, 7,
        JCE_FS_HOST_READ_OK, JCE_FS_HOST_READ_CLOSE_FAILED, JCE_FS_HOST_READ_SHORT_OBSERVED,
        JCE_TRACE_TASK_SUCCEEDED, 0);
}
static void read_trace_large_total_and_full_width_cap_are_exact_and_zero_cap_is_success(void)
{
    begin_read_trace();
    armed = read_budget_armed = true; reported_read_size = INT64_C(0x100000003);
    uint64_t read = 99, total = 99;
    char *bytes = jce_fs_host_read_capped(destination, 7, &read, &total);
    TEST_ASSERT_NOT_NULL(bytes); jce_fs_buffer_free(bytes);
    expect_read_trace("fs.host.read_capped", destination, 15, (uint64_t)reported_read_size, 7, 7, 7,
        JCE_FS_HOST_READ_OK, JCE_FS_HOST_READ_CLOSE_OK, JCE_FS_HOST_READ_FULL,
        JCE_TRACE_TASK_SUCCEEDED, 0);
    begin_read_trace();
    bytes = jce_fs_host_read_capped(destination, 0, &read, &total);
    TEST_ASSERT_NOT_NULL(bytes); TEST_ASSERT_EQUAL_CHAR('\0', bytes[0]); jce_fs_buffer_free(bytes);
    expect_read_trace("fs.host.read_capped", destination, 15, (uint64_t)reported_read_size, 0, 0, 0,
        JCE_FS_HOST_READ_OK, JCE_FS_HOST_READ_CLOSE_OK, JCE_FS_HOST_READ_SHORT_NONE,
        JCE_TRACE_TASK_SUCCEEDED, 0);
    begin_read_trace(); reported_read_size = 7;
    bytes = jce_fs_host_read_capped(destination, UINT64_MAX, &read, &total);
    TEST_ASSERT_NOT_NULL(bytes); TEST_ASSERT_EQUAL_UINT64(7, read); jce_fs_buffer_free(bytes);
    expect_read_trace("fs.host.read_capped", destination, 15, 7, 7, UINT64_MAX, 7,
        JCE_FS_HOST_READ_OK, JCE_FS_HOST_READ_CLOSE_OK, JCE_FS_HOST_READ_FULL,
        JCE_TRACE_TASK_SUCCEEDED, 0);
}
static void read_trace_disabled_and_suppressed_skip_hash_and_all_detail_events(void)
{
    uint64_t read = 99, total = 99;
    char *bytes = jce_fs_host_read_all(destination, &read);
    TEST_ASSERT_NOT_NULL(bytes); jce_fs_buffer_free(bytes);
    bytes = jce_fs_host_read_capped(destination, UINT64_MAX, &read, &total);
    TEST_ASSERT_NOT_NULL(bytes); jce_fs_buffer_free(bytes);
    TEST_ASSERT_EQUAL_UINT(0, request_hash_calls);
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(0, jce_trace_read(&cursor, read_trace_events, 128));
    begin_read_trace();
    TEST_ASSERT_TRUE(jce_trace_suppress_begin());
    bytes = jce_fs_host_read_all(destination, &read);
    bool loaded_all = bytes != NULL; jce_fs_buffer_free(bytes);
    bytes = jce_fs_host_read_capped(destination, 2, &read, &total);
    bool loaded_cap = bytes != NULL; jce_fs_buffer_free(bytes);
    TEST_ASSERT_TRUE(jce_trace_suppress_end());
    TEST_ASSERT_TRUE(loaded_all); TEST_ASSERT_TRUE(loaded_cap);
    TEST_ASSERT_EQUAL_UINT(0, request_hash_calls);
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(0, jce_trace_read(&cursor, read_trace_events, 128));
}
#ifdef JCE_TEST_FS_HOST_SIZE32
static void read_trace_size32_admission_refusal_reports_limit_before_allocator(void)
{
    begin_read_trace();
    armed = read_budget_armed = true; reported_read_size = UINT32_MAX;
    uint64_t read = 99, total = 99;
    TEST_ASSERT_NULL(jce_fs_host_read_capped(destination, UINT64_MAX, &read, &total));
    TEST_ASSERT_EQUAL_UINT(0, read_alloc_calls); TEST_ASSERT_EQUAL_UINT(1, close_calls);
    expect_read_trace("fs.host.read_capped", destination, 15, UINT32_MAX, UINT32_MAX, UINT64_MAX, 0,
        JCE_FS_HOST_READ_LIMIT, JCE_FS_HOST_READ_CLOSE_OK, JCE_FS_HOST_READ_SHORT_NONE,
        JCE_TRACE_TASK_FAILED, 0);
}
#endif

static void expect_path_bytes(const char *path, const void *expected, size_t length)
{
    copy_armed = armed = false;
    uint64_t actual_size = 99;
    void *actual = jce_fs_host_read_all(path, &actual_size);
    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT_EQUAL_UINT64(length, actual_size);
    if (length) TEST_ASSERT_EQUAL_MEMORY(expected, actual, length);
    jce_fs_buffer_free(actual);
}
static void copy_new_invalid_existing_and_directory_preserve_ownership(void)
{
    bool created = true;
    TEST_ASSERT_FALSE(jce_fs_host_copy_file_new(NULL, staging, &created));
    TEST_ASSERT_FALSE(created);
    created = true;
    TEST_ASSERT_FALSE(jce_fs_host_copy_file_new(destination, NULL, &created));
    TEST_ASSERT_FALSE(created);
    TEST_ASSERT_FALSE(jce_fs_host_copy_file_new(destination, staging, NULL));
    char absent[1200]; snprintf(absent, sizeof absent, "%s.absent", destination);
    created = true;
    TEST_ASSERT_FALSE(jce_fs_host_copy_file_new(absent, staging, &created));
    TEST_ASSERT_FALSE(created);
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(staging));
    begin_copy_ports(); armed = true; created = true;
    TEST_ASSERT_FALSE(jce_fs_host_copy_file_new(destination, destination, &created));
    TEST_ASSERT_FALSE(created);
    TEST_ASSERT_EQUAL_UINT(1, copy_source_closes);
    TEST_ASSERT_EQUAL_UINT(0, copy_destination_closes);
    TEST_ASSERT_EQUAL_UINT(0, old_remove_calls); TEST_ASSERT_EQUAL_UINT(0, temp_remove_calls);
    expect_path_bytes(destination, "OLDDATA", 7);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(staging, "EXISTING", 8));
    begin_copy_ports(); created = true;
    TEST_ASSERT_FALSE(jce_fs_host_copy_file_new(destination, staging, &created));
    TEST_ASSERT_FALSE(created);
    expect_path_bytes(staging, "EXISTING", 8);
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(staging));
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(staging));
    begin_copy_ports(); created = true;
    TEST_ASSERT_FALSE(jce_fs_host_copy_file_new(destination, staging, &created));
    TEST_ASSERT_FALSE(created);
    copy_armed = false;
    TEST_ASSERT_TRUE(jce_fs_host_exists_dir(staging));
}
static void copy_new_real_multichunk_file_is_bounded_and_closes_both(void)
{
    const size_t length = 2u * 65536u + 37u;
    unsigned char *expected = JCE_MALLOC(length);
    TEST_ASSERT_NOT_NULL(expected);
    for (size_t i = 0; i < length; ++i) expected[i] = (unsigned char)((i * 31u + 7u) & 255u);
    bool source_written = jce_fs_host_write_all(destination, expected, length);
    begin_copy_ports(); armed = true;
    bool created = false;
    bool copied = jce_fs_host_copy_file_new(destination, staging, &created);
    copy_armed = false;
    /* Free the fixture's large comparison allocation before any assertion abort. */
    uint64_t actual_size = 0;
    void *actual = jce_fs_host_read_all(staging, &actual_size);
    bool bytes_match = actual && actual_size == length && memcmp(expected, actual, length) == 0;
    jce_fs_buffer_free(actual);
    actual = jce_fs_host_read_all(destination, &actual_size);
    bool source_preserved = actual && actual_size == length && memcmp(expected, actual, length) == 0;
    jce_fs_buffer_free(actual); JCE_FREE(expected);
    TEST_ASSERT_TRUE(source_written); TEST_ASSERT_TRUE(copied); TEST_ASSERT_TRUE(created);
    TEST_ASSERT_TRUE(bytes_match); TEST_ASSERT_TRUE(source_preserved);
    TEST_ASSERT_EQUAL_UINT(1, copy_source_closes); TEST_ASSERT_EQUAL_UINT(1, copy_destination_closes);
    TEST_ASSERT_EQUAL_UINT(3, copy_writes); TEST_ASSERT_EQUAL_UINT(1, copy_allocs);
    TEST_ASSERT_EQUAL_UINT(65536, copy_max_alloc);
    TEST_ASSERT_EQUAL_UINT(65536, copy_max_read); TEST_ASSERT_EQUAL_UINT(65536, copy_max_write);
    TEST_ASSERT_EQUAL_UINT(0, old_remove_calls); TEST_ASSERT_EQUAL_UINT(0, temp_remove_calls);
}
static void copy_new_empty_file_has_no_scratch_and_two_successful_closes(void)
{
    TEST_ASSERT_TRUE(jce_fs_host_write_all(destination, NULL, 0));
    begin_copy_ports(); bool created = false;
    TEST_ASSERT_TRUE(jce_fs_host_copy_file_new(destination, staging, &created));
    TEST_ASSERT_TRUE(created);
    TEST_ASSERT_EQUAL_UINT(0, copy_allocs); TEST_ASSERT_EQUAL_UINT(0, copy_writes);
    TEST_ASSERT_EQUAL_UINT(1, copy_source_closes); TEST_ASSERT_EQUAL_UINT(1, copy_destination_closes);
    expect_path_bytes(staging, "", 0);
}
static void copy_new_source_open_size_and_allocation_fail_before_creation(void)
{
    for (unsigned failure = 0; failure < 3; ++failure) {
        begin_copy_ports(); armed = true;
        copy_reject_open = failure == 0;
        copy_reject_size = failure == 1;
        copy_reject_alloc = failure == 2;
        bool created = true;
        TEST_ASSERT_FALSE(jce_fs_host_copy_file_new(destination, staging, &created));
        TEST_ASSERT_FALSE(created);
        TEST_ASSERT_EQUAL_UINT(failure == 0 ? 0 : 1, copy_source_closes);
        TEST_ASSERT_EQUAL_UINT(0, copy_destination_closes);
        TEST_ASSERT_EQUAL_UINT(0, copy_writes);
        TEST_ASSERT_EQUAL_UINT(0, old_remove_calls); TEST_ASSERT_EQUAL_UINT(0, temp_remove_calls);
        copy_armed = false;
        TEST_ASSERT_FALSE(jce_fs_host_exists_file(staging));
    }
    expect_path_bytes(destination, "OLDDATA", 7);
}
static void copy_new_short_io_and_terminal_eof_failure_retain_owned_candidate(void)
{
    for (unsigned failure = 0; failure < 4; ++failure) {
        begin_copy_ports(); armed = true;
        copy_short_read = failure == 0; copy_short_write = failure == 1;
        copy_eof_error = failure == 2; copy_grew = failure == 3;
        bool created = false;
        TEST_ASSERT_FALSE(jce_fs_host_copy_file_new(destination, staging, &created));
        TEST_ASSERT_TRUE(created);
        TEST_ASSERT_EQUAL_UINT(1, copy_source_closes); TEST_ASSERT_EQUAL_UINT(1, copy_destination_closes);
        TEST_ASSERT_EQUAL_UINT(0, old_remove_calls); TEST_ASSERT_EQUAL_UINT(0, temp_remove_calls);
        copy_armed = armed = false;
        TEST_ASSERT_TRUE(jce_fs_host_exists_file(staging));
        uint64_t length = 99; TEST_ASSERT_TRUE(jce_fs_host_get_size(staging, &length));
        TEST_ASSERT_EQUAL_UINT64(failure == 0 ? 0 : failure == 2 ? 7 : 6, length);
        /* This caller removes only the candidate it actually created. */
        if (created) TEST_ASSERT_TRUE(jce_fs_host_remove_file(staging));
    }
    expect_path_bytes(destination, "OLDDATA", 7);
}
static void copy_new_each_close_refusal_is_failure_and_still_closes_the_other(void)
{
    for (unsigned failure = 1; failure <= 3; ++failure) {
        begin_copy_ports(); armed = true;
        copy_reject_source_close = (failure & 1u) != 0;
        copy_reject_destination_close = (failure & 2u) != 0;
        bool created = false;
        TEST_ASSERT_FALSE(jce_fs_host_copy_file_new(destination, staging, &created));
        TEST_ASSERT_TRUE(created);
        TEST_ASSERT_EQUAL_UINT(1, copy_source_closes); TEST_ASSERT_EQUAL_UINT(1, copy_destination_closes);
        TEST_ASSERT_EQUAL_UINT(0, old_remove_calls); TEST_ASSERT_EQUAL_UINT(0, temp_remove_calls);
        expect_path_bytes(staging, "OLDDATA", 7);
        if (created) TEST_ASSERT_TRUE(jce_fs_host_remove_file(staging));
    }
}
static void copy_new_real_exclusive_open_race_keeps_competitor_bytes(void)
{
    begin_copy_ports(); armed = true; copy_race = true;
    bool created = true;
    TEST_ASSERT_FALSE(jce_fs_host_copy_file_new(destination, staging, &created));
    TEST_ASSERT_FALSE(created);
    TEST_ASSERT_EQUAL_UINT(1, copy_source_closes); TEST_ASSERT_EQUAL_UINT(0, copy_destination_closes);
    TEST_ASSERT_EQUAL_UINT(0, copy_writes);
    TEST_ASSERT_EQUAL_UINT(0, old_remove_calls); TEST_ASSERT_EQUAL_UINT(0, temp_remove_calls);
    expect_path_bytes(staging, "RACEDATA", 8);
    expect_path_bytes(destination, "OLDDATA", 7);
}
static void copy_new_trace_records_created_partial_bytes_and_failed_close(void)
{
    begin_read_trace(); begin_copy_ports();
    uint64_t parent = jce_trace_next_id(); jce_trace_task_restore_id(parent);
    copy_short_write = copy_reject_source_close = true;
    bool created = false;
    TEST_ASSERT_FALSE(jce_fs_host_copy_file_new(destination, staging, &created));
    TEST_ASSERT_TRUE(created); copy_armed = false;
    TEST_ASSERT_EQUAL_UINT(2, request_hash_calls);
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    uint32_t count = jce_trace_read(&cursor, read_trace_events, 128);
    const JceTraceEvent *submit = NULL, *end = NULL;
    unsigned detail_count = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const JceTraceEvent *event = &read_trace_events[i];
        TEST_ASSERT_NULL(strstr(event->name, fixture_root));
        if (strcmp(event->name, "fs.host.copy_file_new") != 0) continue;
        if (event->type == JCE_TRACE_EVENT_TASK_SUBMIT) submit = event;
        if (event->type == JCE_TRACE_EVENT_TASK_END) end = event;
    }
    TEST_ASSERT_NOT_NULL(submit); TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_UINT64(parent, submit->parent_id);
    TEST_ASSERT_EQUAL_UINT64(submit->id, end->id);
    TEST_ASSERT_EQUAL_INT(JCE_TRACE_TASK_FAILED, end->value_u32[0]);
    for (uint32_t i = 0; i < count; ++i)
        if (read_trace_events[i].id == submit->id &&
            read_trace_events[i].type == JCE_TRACE_EVENT_COUNTER) ++detail_count;
    TEST_ASSERT_EQUAL_UINT(15, detail_count);
    expect_read_pair(count, submit->id, parent, "fs.copy.source_hash_hi", "fs.copy.source_hash_lo",
                     jce_fnv1a64_str(destination));
    expect_read_pair(count, submit->id, parent, "fs.copy.destination_hash_hi", "fs.copy.destination_hash_lo",
                     jce_fnv1a64_str(staging));
    expect_read_pair(count, submit->id, parent, "fs.copy.total_hi", "fs.copy.total_lo", 7);
    expect_read_pair(count, submit->id, parent, "fs.copy.read_hi", "fs.copy.read_lo", 7);
    expect_read_pair(count, submit->id, parent, "fs.copy.written_hi", "fs.copy.written_lo", 6);
    expect_read_counter(count, submit->id, parent, "fs.copy.known", 7);
    expect_read_counter(count, submit->id, parent, "fs.copy.created", 1);
    expect_read_counter(count, submit->id, parent, "fs.copy.failure_stage", 7);
    expect_read_counter(count, submit->id, parent, "fs.copy.source_close", 2);
    expect_read_counter(count, submit->id, parent, "fs.copy.destination_close", 1);
    TEST_ASSERT_EQUAL_UINT64(parent, jce_trace_task_current_id());
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    jce_trace_task_restore_id(0);
}
static void copy_new_disabled_and_suppressed_skip_hash_and_detail_events(void)
{
    bool created = false;
    TEST_ASSERT_TRUE(jce_fs_host_copy_file_new(destination, staging, &created));
    TEST_ASSERT_TRUE(created); TEST_ASSERT_EQUAL_UINT(0, request_hash_calls);
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT(0, jce_trace_read(&cursor, read_trace_events, 128));
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(staging));
    begin_read_trace(); TEST_ASSERT_TRUE(jce_trace_suppress_begin());
    bool copied = jce_fs_host_copy_file_new(destination, staging, &created);
    TEST_ASSERT_TRUE(jce_trace_suppress_end());
    TEST_ASSERT_TRUE(copied); TEST_ASSERT_TRUE(created);
    TEST_ASSERT_EQUAL_UINT(0, request_hash_calls); jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT(0, jce_trace_read(&cursor, read_trace_events, 128));
}

/* Actual logger sink: bounded copied messages, never a delivery ACK. */
static void open_detail_sink(const JceLogRecord *record, void *user)
{
    (void)user;
    if (strncmp(record->message, "fs.open_failure.", 16) != 0) return;
    int index = SDL_AddAtomicInt(&detail_record_count, 1);
    if (index < 132)
        snprintf(detail_records[index], sizeof detail_records[index], "%s", record->message);
    if (detail_sink_reenter) {
        detail_sink_reenter = false;
        ++detail_nested_reads;
        uint64_t size;
        void *bytes = jce_fs_host_read_all("nested-sink-request", &size);
        jce_fs_buffer_free(bytes);
    }
}
static void begin_open_detail(bool enabled)
{
    jce_log_init(); detail_logger_started = true;
    jce_log_set_level(JCE_LOG_LEVEL_WARN);
    jce_log_set_colors(false);
    jce_log_set_sink(open_detail_sink, NULL);
    begin_read_trace();
    jce__fs_host_open_failure_detail_begin(&detail_context, enabled);
}
static void expect_no_open_detail(void)
{
    jce_log_flush();
    TEST_ASSERT_EQUAL_INT(0, SDL_GetAtomicInt(&detail_record_count));
    TEST_ASSERT_EQUAL_INT(0, SDL_GetAtomicInt(&detail_error_calls));
}
static void open_detail_off_unscoped_suppressed_success_and_isolated_stay_quiet(void)
{
    begin_open_detail(false); detail_fail_open = true;
    uint64_t size;
    TEST_ASSERT_NULL(jce_fs_host_read_all("default-off", &size));
    expect_no_open_detail();
    jce__fs_host_open_failure_detail_begin(&detail_context, true);
    jce_trace_set_enabled(false);
    TEST_ASSERT_NULL(jce_fs_host_read_all("no-trace-scope", &size));
    expect_no_open_detail();
    begin_read_trace();
    TEST_ASSERT_TRUE(jce_trace_suppress_begin());
    void *bytes = jce_fs_host_read_all("consumer-suppressed", &size);
    TEST_ASSERT_TRUE(jce_trace_suppress_end()); TEST_ASSERT_NULL(bytes);
    expect_no_open_detail();
    detail_fail_open = false;
    bytes = jce_fs_host_read_all(destination, &size);
    TEST_ASSERT_NOT_NULL(bytes); jce_fs_buffer_free(bytes);
    expect_no_open_detail();
    JceFileSystem *fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(fs);
    jce_fs_set_active_policy(fs, JCE_FS_ACTIVE_ISOLATED);
    bytes = jce_fs_host_read_all("unmounted-isolated-request", &size);
    jce_fs_set_active(NULL); jce_fs_destroy(fs);
    TEST_ASSERT_NULL(bytes); expect_no_open_detail();
}
#ifdef JCE_LOG_NONE
static void open_detail_compiled_out_never_snapshots_or_logs(void)
{
    begin_open_detail(true); detail_fail_open = detail_clobber_generation = true;
    uint64_t size, total;
    TEST_ASSERT_NULL(jce_fs_host_read_all("compiled-out", &size));
    TEST_ASSERT_NULL(jce_fs_host_read_capped("compiled-out", 8, &size, &total));
    jce_log_quiesce_internal(); jce__fs_host_open_failure_detail_end(&detail_context); expect_no_open_detail();
}
#else
static void expect_detail_text(const char *message, const char *field, const char *raw)
{
    char metadata[140], hex[257];
    size_t length = strlen(raw), captured = length < 128 ? length : 128;
    snprintf(metadata, sizeof metadata, "field=%s bytes=%zu captured=%u truncated=%u optional=unknown hex=",
        field, length, (unsigned)captured, (unsigned)(captured != length));
    const char *start = strstr(message, metadata); TEST_ASSERT_NOT_NULL(start);
    start += strlen(metadata);
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < captured; ++i) {
        unsigned char byte = (unsigned char)raw[i];
        hex[2*i] = digits[byte >> 4]; hex[2*i+1] = digits[byte & 15];
    }
    hex[2*captured] = '\0'; TEST_ASSERT_EQUAL_STRING(hex, start);
    TEST_ASSERT_TRUE(strlen(message) <= 511);
    TEST_ASSERT_NULL(strstr(message, "jce_log_truncated"));
}
static void expect_detail_correlation(unsigned first, uint64_t id, uint64_t parent,
                                      const char *request, const char *op)
{
    char prefix[220];
    int length = snprintf(prefix, sizeof prefix,
        "fs.open_failure.v1 task=%016llx parent=%016llx generation=%u path_hash=%016llx op=%s ",
        (unsigned long long)id, (unsigned long long)parent,
        (unsigned)jce_trace_capture_generation(),
        (unsigned long long)jce_fnv1a64_str(request), op);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof prefix);
    TEST_ASSERT_EQUAL_INT(0, strncmp(detail_records[first], prefix, (size_t)length));
    TEST_ASSERT_EQUAL_INT(0, strncmp(detail_records[first+1], prefix, (size_t)length));
}
static void open_detail_snapshot_precedes_later_error_overwrite_and_keeps_trace(void)
{
    begin_open_detail(true); detail_fail_open = detail_clobber_generation = true;
    detail_open_error = "original provider: % \"\n\xe6\xb5\x8b\xe8\xaf\x95";
    uint64_t size, total;
    jce_trace_task_restore_id(0xfedcba9876543210ULL);
    TEST_ASSERT_NULL(jce_fs_host_read_all("request-%-\xe6\xb5\x8b", &size));
    jce_log_flush(); TEST_ASSERT_EQUAL_INT(2, SDL_GetAtomicInt(&detail_record_count));
    expect_detail_text(detail_records[0], "path", "request-%-\xe6\xb5\x8b");
    expect_detail_text(detail_records[1], "error", detail_open_error);
    TEST_ASSERT_NOT_NULL(strstr(detail_records[0], "parent=fedcba9876543210"));
    TEST_ASSERT_NOT_NULL(strstr(detail_records[0], "op=read_all"));
    uint64_t id = expect_read_trace("fs.host.read_all", "request-%-\xe6\xb5\x8b", 8, 0, 0, 0, 0,
        JCE_FS_HOST_READ_OPEN, JCE_FS_HOST_READ_CLOSE_NONE, JCE_FS_HOST_READ_SHORT_NONE,
        JCE_TRACE_TASK_FAILED, 0xfedcba9876543210ULL);
    expect_detail_correlation(0, id, 0xfedcba9876543210ULL,
        "request-%-\xe6\xb5\x8b", "read_all");
    begin_read_trace(); jce_trace_task_restore_id(0);
    TEST_ASSERT_NULL(jce_fs_host_read_capped("capped-request", 19, &size, &total));
    jce_log_flush(); TEST_ASSERT_EQUAL_INT(4, SDL_GetAtomicInt(&detail_record_count));
    TEST_ASSERT_NOT_NULL(strstr(detail_records[2], "op=read_capped"));
    expect_detail_text(detail_records[3], "error", detail_open_error);
    id = expect_read_trace("fs.host.read_capped", "capped-request", 12, 0, 0, 19, 0,
        JCE_FS_HOST_READ_OPEN, JCE_FS_HOST_READ_CLOSE_NONE, JCE_FS_HOST_READ_SHORT_NONE,
        JCE_TRACE_TASK_FAILED, 0);
    expect_detail_correlation(2, id, 0, "capped-request", "read_capped");
}
static void open_detail_unicode_controls_truncation_and_empty_are_explicit(void)
{
    begin_open_detail(true); detail_fail_open = true;
    char path[191], error[196];
    memset(path, 'p', sizeof path); memset(error, 'e', sizeof error);
    memcpy(path+124, "\xe6\xb5\x8b\xe8\xaf\x95\n%\"", 9);
    memcpy(error+126, "\xe6\xb5\x8b\xe8\xaf\x95\r\n%\"", 10);
    path[sizeof path-1] = error[sizeof error-1] = '\0';
    detail_open_error = error;
    uint64_t size; TEST_ASSERT_NULL(jce_fs_host_read_all(path, &size));
    jce_log_flush(); TEST_ASSERT_EQUAL_INT(2, SDL_GetAtomicInt(&detail_record_count));
    expect_detail_text(detail_records[0], "path", path);
    expect_detail_text(detail_records[1], "error", error);
    detail_open_error = "";
    TEST_ASSERT_NULL(jce_fs_host_read_all("empty-provider-explanation", &size));
    jce_log_flush(); TEST_ASSERT_EQUAL_INT(4, SDL_GetAtomicInt(&detail_record_count));
    expect_detail_text(detail_records[3], "error", "");
}
static void open_detail_actual_formatter_fits_full_numeric_widths(void)
{
    begin_open_detail(true);
    JceTraceScope scope = {UINT64_MAX, UINT64_MAX, 0, "unused", 0};
    JceFsOpenFailureText text;
    memset(text.prefix, 0xff, sizeof text.prefix);
#ifdef JCE_TEST_FS_HOST_SIZE32
    text.length = UINT32_MAX;
#else
    text.length = (size_t)-1;
#endif
    text.captured = 128;
    s_open_detail_emit(&scope, UINT64_MAX, UINT32_MAX, "read_capped", "path", &text);
    s_open_detail_emit(&scope, UINT64_MAX, UINT32_MAX, "read_capped", "error", &text);
    jce_log_flush(); TEST_ASSERT_EQUAL_INT(2, SDL_GetAtomicInt(&detail_record_count));
    /* The size32 control models size_t only inside the included owner. */
#ifdef JCE_TEST_FS_HOST_SIZE32
    TEST_ASSERT_EQUAL_UINT(459, strlen(detail_records[0]));
    TEST_ASSERT_EQUAL_UINT(460, strlen(detail_records[1]));
#else
    TEST_ASSERT_EQUAL_UINT(sizeof(size_t) == 8 ? 469 : 459, strlen(detail_records[0]));
    TEST_ASSERT_EQUAL_UINT(sizeof(size_t) == 8 ? 470 : 460, strlen(detail_records[1]));
#endif
    TEST_ASSERT_NULL(strstr(detail_records[0], "jce_log_truncated"));
    TEST_ASSERT_NULL(strstr(detail_records[1], "jce_log_truncated"));
    TEST_ASSERT_NOT_NULL(strstr(detail_records[1], "task=ffffffffffffffff parent=ffffffffffffffff generation=4294967295"));
}
static void open_detail_filter_is_unavailable_and_sync_sink_is_suppressed(void)
{
    begin_open_detail(true); detail_fail_open = true;
    jce_log_set_level(JCE_LOG_LEVEL_OFF);
    uint64_t size; TEST_ASSERT_NULL(jce_fs_host_read_all("filtered-request", &size));
    jce_log_flush(); TEST_ASSERT_EQUAL_INT(0, SDL_GetAtomicInt(&detail_record_count));
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&detail_error_calls));
    jce_log_set_level(JCE_LOG_LEVEL_WARN); jce_log_quiesce_internal();
    jce__fs_host_open_failure_detail_end(&detail_context);
    jce_log_flush(); TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&detail_record_count));
    TEST_ASSERT_NOT_NULL(strstr(detail_records[0], "attempted=1 refused=0 refused_saturated=0 delivery=unavailable"));
    jce__fs_host_open_failure_detail_begin(&detail_context, true);
    SDL_SetAtomicInt(&detail_record_count, 0); SDL_SetAtomicInt(&detail_error_calls, 0);
    jce_log_quiesce_internal(); detail_sink_reenter = true;
    TEST_ASSERT_NULL(jce_fs_host_read_all("outer-request", &size));
    TEST_ASSERT_EQUAL_UINT(1, detail_nested_reads);
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&detail_error_calls));
    TEST_ASSERT_EQUAL_INT(2, SDL_GetAtomicInt(&detail_record_count));
}
typedef struct OpenDetailWorker { const char *request; int failures; } OpenDetailWorker;
static int open_detail_worker(void *opaque)
{
    OpenDetailWorker *worker = (OpenDetailWorker *)opaque;
    jce_trace_thread_register(worker->request);
    for (int i = 0; i < 35; ++i) {
        uint64_t size; void *bytes = jce_fs_host_read_all(worker->request, &size);
        if (bytes || size) ++worker->failures;
        jce_fs_buffer_free(bytes);
    }
    jce_trace_thread_unregister(); return 0;
}
static void open_detail_two_threads_keep_tls_and_whole_budget_groups(void)
{
    begin_open_detail(true); detail_fail_open = detail_error_from_path = detail_threaded = true;
    OpenDetailWorker a = {"worker-A", 0}, b = {"worker-B", 0};
    SDL_Thread *ta = SDL_CreateThread(open_detail_worker, "fs-A", &a);
    SDL_Thread *tb = SDL_CreateThread(open_detail_worker, "fs-B", &b);
    int ra = -1, rb = -1;
    if (ta) SDL_WaitThread(ta, &ra);
    if (tb) SDL_WaitThread(tb, &rb);
    detail_threaded = false;
    TEST_ASSERT_NOT_NULL(ta); TEST_ASSERT_NOT_NULL(tb);
    TEST_ASSERT_EQUAL_INT(0, ra); TEST_ASSERT_EQUAL_INT(0, rb);
    TEST_ASSERT_EQUAL_INT(0, a.failures); TEST_ASSERT_EQUAL_INT(0, b.failures);
    jce_log_quiesce_internal(); jce__fs_host_open_failure_detail_end(&detail_context); jce_log_flush();
    TEST_ASSERT_EQUAL_INT(129, SDL_GetAtomicInt(&detail_record_count));
    unsigned paths = 0, errors = 0;
    for (int i = 0; i < 128; ++i) {
        const char *message = detail_records[i]; unsigned long long id = 0;
        TEST_ASSERT_EQUAL_INT(1, sscanf(message, "fs.open_failure.v1 task=%llx", &id));
        TEST_ASSERT_TRUE(id != 0);
        bool path = strstr(message, "field=path") != NULL;
        if (path) ++paths; else ++errors;
        const char *which = strstr(message, "hex=776f726b65722d41") ? "worker-A" : "worker-B";
        expect_detail_text(message, path ? "path" : "error", which);
        unsigned matches = 0;
        for (int j = 0; j < 128; ++j) {
            unsigned long long other = 0;
            if (sscanf(detail_records[j], "fs.open_failure.v1 task=%llx", &other) == 1 && other == id) {
                ++matches; TEST_ASSERT_NOT_NULL(strstr(detail_records[j], strstr(message, " hex=")));
            }
        }
        TEST_ASSERT_EQUAL_UINT(2, matches);
    }
    TEST_ASSERT_EQUAL_UINT(64, paths); TEST_ASSERT_EQUAL_UINT(64, errors);
    TEST_ASSERT_NOT_NULL(strstr(detail_records[128], "attempted=70 refused=6 refused_saturated=0 delivery=unavailable"));
}
static void expect_bound_open_count(unsigned count)
{
    uint64_t size;
    TEST_ASSERT_NULL(jce_fs_host_read_all(destination, &size));
    TEST_ASSERT_EQUAL_INT(count, SDL_GetAtomicInt(&detail_context.admitted));
    TEST_ASSERT_EQUAL_INT(2 * count, SDL_GetAtomicInt(&detail_record_count));
}
static void open_detail_owner_borrow_survives_vfs_changes_and_rejects_foreign(void)
{
    begin_open_detail(true);
    jce_log_quiesce_internal();
    detail_fail_open = true;
    expect_bound_open_count(1);
    SDL_SetAtomicInt(&detail_foreign_context.admitted, 17);
    SDL_SetAtomicInt(&detail_foreign_context.refused, 23);
    TEST_ASSERT_FALSE(jce__fs_host_open_failure_detail_begin(&detail_context, true));
    TEST_ASSERT_FALSE(jce__fs_host_open_failure_detail_begin(&detail_context, false));
    TEST_ASSERT_FALSE(jce__fs_host_open_failure_detail_begin(&detail_foreign_context, true));
    TEST_ASSERT_FALSE(jce__fs_host_open_failure_detail_begin(&detail_foreign_context, false));
    TEST_ASSERT_FALSE(jce__fs_host_open_failure_detail_begin(NULL, true));
    TEST_ASSERT_FALSE(jce__fs_host_open_failure_detail_end(&detail_foreign_context));
    TEST_ASSERT_FALSE(jce__fs_host_open_failure_detail_end(NULL));
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&detail_context.admitted));
    TEST_ASSERT_EQUAL_INT(17, SDL_GetAtomicInt(&detail_foreign_context.admitted));
    TEST_ASSERT_EQUAL_INT(23, SDL_GetAtomicInt(&detail_foreign_context.refused));

    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);
    jce_fs_set_active_policy(fs, JCE_FS_ACTIVE_ISOLATED);
    TEST_ASSERT_EQUAL_PTR(fs, jce_fs_get_active());
    TEST_ASSERT_EQUAL_INT(JCE_FS_ACTIVE_ISOLATED, jce_fs_get_active_policy());
    expect_bound_open_count(2); /* Absolute host request, no relative VFS fallback. */
    jce_fs_set_active_reader(jce_fs_read_all); /* Even same reader invalidates probe. */
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_UNKNOWN, jce_fs_probe_read("unprobed-relative"));
    expect_bound_open_count(3);
    jce_fs_set_active_reader(NULL);
    TEST_ASSERT_NULL(jce__fs_active_reader());
    expect_bound_open_count(4);
    jce_fs_set_active_policy(fs, JCE_FS_ACTIVE_OVERLAY);
    TEST_ASSERT_TRUE(jce__fs_active_reader() == jce_fs_read_all);
    expect_bound_open_count(5);
    jce_fs_set_active(NULL);
    TEST_ASSERT_NULL(jce_fs_get_active());
    TEST_ASSERT_NULL(jce__fs_active_reader());
    TEST_ASSERT_EQUAL_INT(JCE_FS_ACTIVE_OVERLAY, jce_fs_get_active_policy());
    expect_bound_open_count(6);
    jce_fs_set_active(fs);
    expect_bound_open_count(7);
    jce_fs_set_active(NULL); /* Caller clears its borrowed VFS before destruction. */
    jce_fs_destroy(fs);
    expect_bound_open_count(8);

    TEST_ASSERT_TRUE(jce__fs_host_open_failure_detail_end(&detail_context));
    TEST_ASSERT_FALSE(jce__fs_host_open_failure_detail_end(&detail_context));
    TEST_ASSERT_EQUAL_INT(17, SDL_GetAtomicInt(&detail_record_count));
    TEST_ASSERT_NOT_NULL(strstr(detail_records[16], "attempted=8 refused=0"));
    uint64_t size;
    TEST_ASSERT_NULL(jce_fs_host_read_all(destination, &size));
    TEST_ASSERT_EQUAL_INT(8, SDL_GetAtomicInt(&detail_error_calls));
    TEST_ASSERT_TRUE(jce__fs_host_open_failure_detail_begin(&detail_foreign_context, true));
    TEST_ASSERT_EQUAL_INT(0, SDL_GetAtomicInt(&detail_foreign_context.admitted));
    TEST_ASSERT_EQUAL_INT(0, SDL_GetAtomicInt(&detail_foreign_context.refused));
    TEST_ASSERT_FALSE(jce__fs_host_open_failure_detail_end(&detail_context));
    TEST_ASSERT_NULL(jce_fs_host_read_all(destination, &size));
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&detail_foreign_context.admitted));
    TEST_ASSERT_TRUE(jce__fs_host_open_failure_detail_end(&detail_foreign_context));
    TEST_ASSERT_EQUAL_INT(20, SDL_GetAtomicInt(&detail_record_count));
    TEST_ASSERT_TRUE(jce__fs_host_open_failure_detail_begin(&detail_context, false));
    TEST_ASSERT_NULL(jce_fs_host_read_all(destination, &size));
    TEST_ASSERT_FALSE(jce__fs_host_open_failure_detail_end(&detail_context));
    TEST_ASSERT_EQUAL_INT(9, SDL_GetAtomicInt(&detail_error_calls));
    TEST_ASSERT_EQUAL_INT(20, SDL_GetAtomicInt(&detail_record_count));
}
static void open_detail_refusal_saturates_and_end_is_idempotent(void)
{
    begin_open_detail(true);
    SDL_SetAtomicInt(&detail_context.admitted, 64);
    SDL_SetAtomicInt(&detail_context.refused, INT_MAX - 1);
    detail_fail_open = true;
    uint64_t size;
    TEST_ASSERT_NULL(jce_fs_host_read_all("refused-1", &size));
    TEST_ASSERT_NULL(jce_fs_host_read_all("refused-2", &size));
    jce_log_quiesce_internal(); jce__fs_host_open_failure_detail_end(&detail_context); jce__fs_host_open_failure_detail_end(&detail_context);
    jce_log_flush(); TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&detail_record_count));
    TEST_ASSERT_NOT_NULL(strstr(detail_records[0], "attempted=2147483711 refused=2147483647 refused_saturated=1 delivery=unavailable"));
    TEST_ASSERT_NULL(jce_fs_host_read_all("after-end", &size));
    jce_log_flush(); TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&detail_record_count));
}
#endif

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(open_detail_off_unscoped_suppressed_success_and_isolated_stay_quiet);
#ifdef JCE_LOG_NONE
    RUN_TEST(open_detail_compiled_out_never_snapshots_or_logs);
#else
    RUN_TEST(open_detail_snapshot_precedes_later_error_overwrite_and_keeps_trace);
    RUN_TEST(open_detail_unicode_controls_truncation_and_empty_are_explicit);
    RUN_TEST(open_detail_actual_formatter_fits_full_numeric_widths);
    RUN_TEST(open_detail_filter_is_unavailable_and_sync_sink_is_suppressed);
    RUN_TEST(open_detail_two_threads_keep_tls_and_whole_budget_groups);
    RUN_TEST(open_detail_refusal_saturates_and_end_is_idempotent);
    RUN_TEST(open_detail_owner_borrow_survives_vfs_changes_and_rejects_foreign);
#endif
    RUN_TEST(copy_new_invalid_existing_and_directory_preserve_ownership);
    RUN_TEST(copy_new_real_multichunk_file_is_bounded_and_closes_both);
    RUN_TEST(copy_new_empty_file_has_no_scratch_and_two_successful_closes);
    RUN_TEST(copy_new_source_open_size_and_allocation_fail_before_creation);
    RUN_TEST(copy_new_short_io_and_terminal_eof_failure_retain_owned_candidate);
    RUN_TEST(copy_new_each_close_refusal_is_failure_and_still_closes_the_other);
    RUN_TEST(copy_new_real_exclusive_open_race_keeps_competitor_bytes);
    RUN_TEST(copy_new_trace_records_created_partial_bytes_and_failed_close);
    RUN_TEST(copy_new_disabled_and_suppressed_skip_hash_and_detail_events);
    RUN_TEST(read_trace_real_success_close_refusal_preserves_success_and_parent);
    RUN_TEST(read_trace_invalid_and_open_refusal_have_distinct_first_stages);
    RUN_TEST(read_trace_size_and_alloc_failure_keep_first_stage_after_failed_close);
    RUN_TEST(read_trace_short_full_read_fails_but_capped_short_read_stays_success);
    RUN_TEST(read_trace_large_total_and_full_width_cap_are_exact_and_zero_cap_is_success);
    RUN_TEST(read_trace_disabled_and_suppressed_skip_hash_and_all_detail_events);
#ifdef JCE_TEST_FS_HOST_SIZE32
    RUN_TEST(read_trace_size32_admission_refusal_reports_limit_before_allocator);
#endif
    RUN_TEST(read_all_real_small_and_empty_files_keep_text_sentinels);
    RUN_TEST(read_all_allocation_refusal_closes_without_reading);
    RUN_TEST(read_all_unknown_size_closes_without_allocation);
    RUN_TEST(capped_read_real_file_preserves_cap_zero_and_total_size);
    RUN_TEST(capped_read_large_metadata_clamps_before_length_conversion);
    RUN_TEST(capped_read_large_metadata_and_zero_cap_avoid_read);
    RUN_TEST(capped_read_unknown_size_closes_without_allocation);
#ifdef JCE_TEST_FS_HOST_SIZE32
    RUN_TEST(read_all_size32_last_safe_length_reaches_allocator_once);
    RUN_TEST(read_all_size32_unrepresentable_lengths_never_allocate);
    RUN_TEST(capped_read_size32_last_safe_length_reaches_allocator_once);
    RUN_TEST(capped_read_size32_unrepresentable_lengths_never_allocate);
#endif
    RUN_TEST(persistent_stream_writes_multiple_batches_and_closes_once);
    RUN_TEST(persistent_stream_failure_clears_handle_without_retry_or_removal);
    RUN_TEST(rename_failure_preserves_previous_file_and_cleans_once);
    RUN_TEST(transient_rename_failure_never_unlinks_and_retries);
    RUN_TEST(close_failure_never_publishes_the_staging_file);
    RUN_TEST(cleanup_failure_keeps_previous_file_and_does_not_retry);
    RUN_TEST(successful_real_replace_commits_once_without_unlink);
    RUN_TEST(zero_length_real_replace_and_create_are_supported);
    RUN_TEST(invalid_input_does_not_touch_previous_file);
    RUN_TEST(append_close_failure_is_not_success_and_closes_once);
    RUN_TEST(append_short_write_is_rejected_without_retry_and_closes_once);
    RUN_TEST(append_success_and_empty_append_use_one_real_close);
    RUN_TEST(exclusive_reservation_refuses_real_existing_file_and_directory);
    RUN_TEST(exclusive_reservation_short_write_and_close_failure_retain_created_file);
    RUN_TEST(exclusive_reservation_success_and_invalid_input_keep_ownership_explicit);
    RUN_TEST(public_write_new_uses_exclusive_owner_and_preserves_failure_evidence);
    return UNITY_END();
}
