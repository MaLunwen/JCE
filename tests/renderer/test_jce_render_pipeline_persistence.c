/* Actual RP saver/loader and complete host-FS owner, with bounded native IO
 * failure ports. No second writer, renderer device or application is used. */
#include "unity.h"
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_trace.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include "os/core/jce_memory.h"
#include "os/core/jce_trace_scope_internal.h"
#include "os/core/jce_filesystem_host_internal.h"
#include <stdio.h>
#include <string.h>

typedef enum { FAULT_NONE, FAULT_WRITE, FAULT_CLOSE, FAULT_RENAME } SaveFault;
static char root[1024], prefix[1024], destination[1100], staging[1105];
static bool root_created, armed;
static SaveFault fault;
static unsigned writes, closes, renames, old_removes, staging_removes;
static void *baseline, *readback;
static uint64_t baseline_size;
static JceRenderPipelineDesc previous, replacement;

static bool counted_rename(const char *source, const char *target);
static bool counted_remove(const char *path);
static bool counted_close(SDL_IOStream *io);
static size_t counted_write(SDL_IOStream *io, const void *data, size_t size);

/* Native declarations above are unchanged. Include the one real host owner;
 * only actual SDL write/close/rename/remove boundaries are observed. */
#define SDL_RenamePath counted_rename
#define SDL_RemovePath counted_remove
#define SDL_CloseIO counted_close
#define SDL_WriteIO counted_write
#include "os/core/jce_filesystem_host.c"
#undef SDL_RenamePath
#undef SDL_RemovePath
#undef SDL_CloseIO
#undef SDL_WriteIO

static bool counted_rename(const char *source, const char *target)
{
    if (armed && strcmp(source, staging) == 0 && strcmp(target, destination) == 0) {
        ++renames;
        if (fault == FAULT_RENAME) return false;
    }
    return SDL_RenamePath(source, target);
}

static bool counted_remove(const char *path)
{
    if (armed && strcmp(path, destination) == 0) ++old_removes;
    if (armed && strcmp(path, staging) == 0) ++staging_removes;
    return SDL_RemovePath(path);
}

static bool counted_close(SDL_IOStream *io)
{
    const bool closed = SDL_CloseIO(io);
    if (armed) {
        ++closes;
        if (fault == FAULT_CLOSE) return false;
    }
    return closed;
}

static size_t counted_write(SDL_IOStream *io, const void *data, size_t size)
{
    if (armed) {
        ++writes;
        /* Perform an actual shorter native write, not a successful complete
         * write whose result is merely relabelled as failure. */
        if (fault == FAULT_WRITE && size) --size;
    }
    return SDL_WriteIO(io, data, size);
}

static bool owns_root(void)
{
    const size_t length = strlen(prefix);
    return length && strncmp(root, prefix, length) == 0 && root[length] &&
        !strchr(root + length, '/') && !strchr(root + length, '\\');
}

static void arm(SaveFault selected)
{
    fault = selected;
    writes = closes = renames = old_removes = staging_removes = 0;
    armed = true;
}

void setUp(void)
{
    static unsigned sequence;
    char cwd[900], normalized[900], desired[1100];
    armed = false; fault = FAULT_NONE;
    baseline = readback = NULL; root_created = false;
    root[0] = prefix[0] = '\0';
    jce_trace_set_enabled(false);
    TEST_ASSERT_TRUE(jce_fs_host_get_current_dir(cwd, sizeof(cwd)));
    TEST_ASSERT_TRUE(jce_path_normalize(normalized, sizeof(normalized), cwd));
    const int length = snprintf(prefix, sizeof(prefix), "%s/_ut_rp_save_", normalized);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof(prefix));
    snprintf(desired, sizeof(desired), "%s%u", prefix, ++sequence);
    TEST_ASSERT_TRUE(jce_fs_host_make_unique_path(desired, root, sizeof(root)));
    TEST_ASSERT_TRUE(owns_root());
    TEST_ASSERT_FALSE(jce_fs_host_exists_dir(root));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(root));
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(root));
    root_created = true;
    snprintf(destination, sizeof(destination), "%s/Settings/RenderPipeline.rp.json", root);
    snprintf(staging, sizeof(staging), "%s.tmp", destination);
    jce_render_pipeline_preset_low(&previous);
    jce_render_pipeline_preset_high(&replacement);
    TEST_ASSERT_TRUE(jce_render_pipeline_save(destination, &previous));
    baseline = jce_fs_host_read_all(destination, &baseline_size);
    TEST_ASSERT_NOT_NULL(baseline);
}

void tearDown(void)
{
    armed = false;
    if (readback) jce_fs_buffer_free(readback);
    if (baseline) jce_fs_buffer_free(baseline);
    readback = baseline = NULL;
    if (root_created) {
        const bool owned = owns_root();
        TEST_ASSERT_TRUE(owned);
        if (owned) TEST_ASSERT_TRUE(jce_fs_host_remove_recursive(root));
    }
    root_created = false;
}

static void expect_loaded(const JceRenderPipelineDesc *expected)
{
    JceRenderPipelineDesc loaded;
    memset(&loaded, 0xa5, sizeof(loaded));
    TEST_ASSERT_TRUE(jce_render_pipeline_load(destination, &loaded));
    TEST_ASSERT_EQUAL_UINT32(expected->shadow_resolution, loaded.shadow_resolution);
    TEST_ASSERT_EQUAL_INT(expected->post_quality, loaded.post_quality);
    TEST_ASSERT_EQUAL_INT(expected->enable_taa, loaded.enable_taa);
    TEST_ASSERT_EQUAL_FLOAT(expected->render_scale, loaded.render_scale);
}

static void expect_previous_bytes_and_reload(void)
{
    armed = false;
    uint64_t count = 0;
    readback = jce_fs_host_read_all(destination, &count);
    TEST_ASSERT_NOT_NULL(readback);
    TEST_ASSERT_EQUAL_UINT64(baseline_size, count);
    TEST_ASSERT_EQUAL_MEMORY(baseline, readback, (size_t)count);
    jce_fs_buffer_free(readback); readback = NULL;
    expect_loaded(&previous);
}

static void expect_successful_retry(void)
{
    arm(FAULT_NONE);
    TEST_ASSERT_TRUE(jce_render_pipeline_save(destination, &replacement));
    TEST_ASSERT_EQUAL_UINT(1, writes);
    TEST_ASSERT_EQUAL_UINT(1, closes);
    TEST_ASSERT_EQUAL_UINT(1, renames);
    TEST_ASSERT_EQUAL_UINT(0, old_removes);
    TEST_ASSERT_EQUAL_UINT(0, staging_removes);
    armed = false;
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(staging));
    expect_loaded(&replacement);
}

static void exercise_failed_save(SaveFault selected)
{
    arm(selected);
    TEST_ASSERT_FALSE(jce_render_pipeline_save(destination, &replacement));
    TEST_ASSERT_EQUAL_UINT(1, writes);
    TEST_ASSERT_EQUAL_UINT(1, closes);
    TEST_ASSERT_EQUAL_UINT(selected == FAULT_RENAME ? 1u : 0u, renames);
    TEST_ASSERT_EQUAL_UINT(0, old_removes);
    expect_previous_bytes_and_reload();
    TEST_ASSERT_EQUAL_UINT(1, staging_removes);
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(staging));
    expect_successful_retry();
}

static void replacement_publishes_a_reloadable_descriptor_once(void)
{
    expect_successful_retry();
}

static void partial_write_preserves_the_previous_asset_and_retries(void)
{
    exercise_failed_save(FAULT_WRITE);
}

static void failed_close_preserves_the_previous_asset_and_retries(void)
{
    exercise_failed_save(FAULT_CLOSE);
}

static void failed_rename_preserves_the_previous_asset_and_retries(void)
{
    exercise_failed_save(FAULT_RENAME);
}

static void invalid_requests_leave_previous_bytes_untouched(void)
{
    arm(FAULT_NONE);
    TEST_ASSERT_FALSE(jce_render_pipeline_save(NULL, &replacement));
    TEST_ASSERT_FALSE(jce_render_pipeline_save("", &replacement));
    TEST_ASSERT_FALSE(jce_render_pipeline_save(destination, NULL));
    TEST_ASSERT_EQUAL_UINT(0, writes);
    TEST_ASSERT_EQUAL_UINT(0, closes);
    TEST_ASSERT_EQUAL_UINT(0, renames);
    TEST_ASSERT_EQUAL_UINT(0, old_removes);
    TEST_ASSERT_EQUAL_UINT(0, staging_removes);
    expect_previous_bytes_and_reload();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(replacement_publishes_a_reloadable_descriptor_once);
    RUN_TEST(partial_write_preserves_the_previous_asset_and_retries);
    RUN_TEST(failed_close_preserves_the_previous_asset_and_retries);
    RUN_TEST(failed_rename_preserves_the_previous_asset_and_retries);
    RUN_TEST(invalid_requests_leave_previous_bytes_untouched);
    return UNITY_END();
}
