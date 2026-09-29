/*
 * test_jce_streaming_async.c - Structured streaming execution contract.
 */

#include "unity.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_thread.h>
#include <jce/resource/jce_streaming.h>

#include <stdio.h>
#include <string.h>

static char g_root[1024];

typedef struct StreamingFixture {
    uint32_t loaded;
    uint32_t unloaded;
    size_t   bytes;
} StreamingFixture;

void setUp(void)
{
    char cwd[768];

    TEST_ASSERT_TRUE(
        jce_fs_host_get_current_dir(cwd, (uint32_t)sizeof(cwd)));
    TEST_ASSERT_TRUE(snprintf(
        g_root, sizeof(g_root), "%s/.jce_test_streaming_async", cwd) > 0);
    (void)jce_fs_host_remove_recursive(g_root);
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(g_root));
}

void tearDown(void)
{
    (void)jce_fs_host_remove_recursive(g_root);
}

static void write_fixture(const char *name, const char *contents)
{
    char path[1200];

    TEST_ASSERT_TRUE(snprintf(
        path, sizeof(path), "%s/%s", g_root, name) > 0);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(
        path, contents, (uint64_t)strlen(contents)));
}

static void on_loaded(uint32_t chunk_id, void *data, size_t size,
                      void *user_data)
{
    StreamingFixture *fixture = (StreamingFixture *)user_data;

    TEST_ASSERT_TRUE(jce_thread_is_main());
    TEST_ASSERT_TRUE(chunk_id == 1 || chunk_id == 2);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_TRUE(size > 0);
    fixture->loaded++;
    fixture->bytes += size;
}

static void on_unloaded(uint32_t chunk_id, void *user_data)
{
    StreamingFixture *fixture = (StreamingFixture *)user_data;

    TEST_ASSERT_TRUE(jce_thread_is_main());
    TEST_ASSERT_TRUE(chunk_id == 1 || chunk_id == 2);
    fixture->unloaded++;
}

static void register_chunk(JceStreamingSystem *streaming,
                           uint32_t id,
                           const char *path,
                           float x)
{
    JceStreamChunk chunk;

    memset(&chunk, 0, sizeof(chunk));
    chunk.chunk_id = id;
    chunk.center = jce_v3(x, 0.0f, 0.0f);
    chunk.radius = 1.0f;
    chunk.asset_path = path;
    jce_streaming_register_chunk(streaming, &chunk);
}

static void test_cooperative_streaming_is_deferred_and_bounded(void)
{
    JceStreamingConfig config;
    JceStreamingSystem *streaming;
    JceFileSystem *fs;
    StreamingFixture fixture = { 0, 0, 0 };
    uint32_t i;

    write_fixture("a.bin", "alpha");
    write_fixture("b.bin", "bravo!");

    memset(&config, 0, sizeof(config));
    config.mode = JCE_STREAM_RADIAL;
    config.load_radius = 100.0f;
    config.unload_radius = 150.0f;
    config.max_pending = 1;
    config.budget_mb = 1;
    config.single_thread = true;
    config.frame_budget_ms = 100.0f;

    fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_TRUE(jce_fs_mount_dir(fs, "", g_root));
    streaming = jce_streaming_create(&config);
    TEST_ASSERT_NOT_NULL(streaming);
    TEST_ASSERT_TRUE(jce_streaming_is_single_thread(streaming));
    jce_streaming_set_filesystem(streaming, fs);
    jce_streaming_set_callbacks(
        streaming, on_loaded, on_unloaded, &fixture);
    register_chunk(streaming, 1, "a.bin", 0.0f);
    register_chunk(streaming, 2, "b.bin", 2.0f);

    /*
     * Submission happens after the cooperative pump. The first update must
     * therefore queue exactly one load without executing user work inline.
     */
    jce_streaming_update(streaming, jce_v3(0.0f, 0.0f, 0.0f));
    TEST_ASSERT_EQUAL_UINT32(0, fixture.loaded);
    TEST_ASSERT_EQUAL_UINT32(0, jce_streaming_loaded_count(streaming));
    TEST_ASSERT_EQUAL_UINT32(1, jce_streaming_pending_count(streaming));

    for (i = 0; i < 8 && fixture.loaded < 2; ++i) {
        jce_streaming_update(streaming, jce_v3(0.0f, 0.0f, 0.0f));
        TEST_ASSERT_TRUE(jce_streaming_pending_count(streaming) <= 1);
    }
    TEST_ASSERT_EQUAL_UINT32(2, fixture.loaded);
    TEST_ASSERT_EQUAL_UINT32(2, jce_streaming_loaded_count(streaming));
    TEST_ASSERT_EQUAL_UINT32(11, fixture.bytes);

    jce_streaming_update(streaming, jce_v3(1000.0f, 0.0f, 0.0f));
    TEST_ASSERT_EQUAL_UINT32(0, jce_streaming_loaded_count(streaming));
    TEST_ASSERT_EQUAL_UINT32(2, fixture.unloaded);

    jce_streaming_destroy(streaming);
    jce_fs_destroy(fs);
}

int main(void)
{
    jce_thread_mark_main();
    UNITY_BEGIN();
    RUN_TEST(test_cooperative_streaming_is_deferred_and_bounded);
    return UNITY_END();
}
