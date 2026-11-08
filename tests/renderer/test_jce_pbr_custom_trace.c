/* Exact production cache/path/read/link owner, real host/VFS + Archive/PAK
 * bytes and trace ring. Only the renderer program port and local fallback
 * allocation failure are controlled; no graphics device is created. */
#include "unity.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_archive_writer.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/renderer/jce_renderer.h>
#include "os/core/jce_memory.h"
#include "renderer/jce_pbr_custom_trace_internal.h"
#include "resource/jce_pak_mount_internal.h"
#include <stdio.h>
#include <string.h>

static const char host_vs[] = "_ut_pbr_custom_trace_vs.bin";
static const char host_fs[] = "_ut_pbr_custom_trace_fs.bin";
static const char suffixed_vs[] = "_ut_pbr_custom_trace_vs_diag.bin";
static const char suffixed_fs[] = "_ut_pbr_custom_trace_fs_diag.bin";
static const unsigned char stage_bytes[] = {1, 9, 7, 3};
static JcePakArchive *engine_pak;
static JceFileSystem *vfs;
static unsigned program_calls, alloc_calls, free_calls, owned_live;
static bool fail_alloc, fail_program;
static void *owned[4];
static const char *suffix;

static const JcePakArchive *fixture_engine_pak(void) { return engine_pak; }
static const char *fixture_suffix(void) { return suffix; }
static JceShaderHandle fixture_program(const void *vs, size_t vs_size,
                                      const void *fs, size_t fs_size)
{
    ++program_calls;
    TEST_ASSERT_EQUAL_UINT(sizeof(stage_bytes), vs_size);
    TEST_ASSERT_EQUAL_UINT(sizeof(stage_bytes), fs_size);
    TEST_ASSERT_EQUAL_MEMORY(stage_bytes, vs, sizeof(stage_bytes));
    TEST_ASSERT_EQUAL_MEMORY(stage_bytes, fs, sizeof(stage_bytes));
    return (JceShaderHandle){fail_program ? UINT16_MAX : 37};
}
static void *fixture_alloc(size_t size)
{
    ++alloc_calls;
    if (fail_alloc) return NULL;
    void *p = jce__malloc_tracked(size);
    if (p) { TEST_ASSERT_LESS_THAN_UINT(4, owned_live); owned[owned_live++] = p; }
    return p;
}
static void fixture_free(void *p)
{
    if (!p) return;
    ++free_calls;
    for (unsigned i = 0; i < owned_live; ++i) if (owned[i] == p) {
        owned[i] = owned[--owned_live]; break;
    }
    jce__free_tracked(p);
}
#define LOG_TAG "jce_pbr_material"
#define jce_shaders_embedded_engine_pak fixture_engine_pak
#define jce_shaders_backend_suffix fixture_suffix
#define jce_renderer_create_program_from_blobs fixture_program
#undef JCE_MALLOC
#undef JCE_FREE
#define JCE_MALLOC fixture_alloc
#define JCE_FREE fixture_free
#include JCE_TEST_PBR_CUSTOM_OWNER
#undef JCE_MALLOC
#undef JCE_FREE
#undef jce_shaders_embedded_engine_pak
#undef jce_shaders_backend_suffix
#undef jce_renderer_create_program_from_blobs

static JceTraceEvent events[1024];
static uint32_t event_count;
static void drain(void)
{
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    event_count = jce_trace_read(&cursor, events, 1024);
    TEST_ASSERT_TRUE(event_count < 1024); TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    JceTraceStats stats; jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
}
static uint64_t scope(const char *name)
{
    for (uint32_t i = 0; i < event_count; ++i)
        if (events[i].type == JCE_TRACE_EVENT_TASK_BEGIN && !strcmp(events[i].name, name)) return events[i].id;
    TEST_FAIL_MESSAGE(name); return 0;
}
static int64_t value(uint64_t id, const char *name)
{
    for (uint32_t i = 0; i < event_count; ++i)
        if (events[i].type == JCE_TRACE_EVENT_COUNTER && events[i].id == id && !strcmp(events[i].name, name))
            return (int64_t)events[i].value_u64;
    TEST_FAIL_MESSAGE(name); return INT64_MIN;
}
static void outcome(uint64_t id, JceTraceTaskState state)
{
    unsigned ends = 0;
    for (uint32_t i = 0; i < event_count; ++i)
        if (events[i].type == JCE_TRACE_EVENT_TASK_END && events[i].id == id) {
            ++ends; TEST_ASSERT_EQUAL_UINT32(state, events[i].value_u32[0]);
        }
    TEST_ASSERT_EQUAL_UINT(1, ends);
}
static uint64_t wide(uint64_t id, const char *lo, const char *hi)
{ return (uint64_t)(uint32_t)value(id, lo) | ((uint64_t)(uint32_t)value(id, hi) << 32u); }
static void write_host(void)
{
    TEST_ASSERT_TRUE(jce_fs_host_write_all(host_vs, stage_bytes, sizeof(stage_bytes)));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(host_fs, stage_bytes, sizeof(stage_bytes)));
}
static void make_pak(const char *vs, const char *fs, bool corrupt)
{
    JceArchiveWriter *writer = jce_archive_writer_create(NULL); TEST_ASSERT_NOT_NULL(writer);
    if (corrupt) {
        unsigned char invalid[] = {0xa1, 0x12, 0x33};
        JceArchiveBlob blob = {0}; blob.bytes = invalid; blob.stored_size = sizeof(invalid);
        blob.original_size = 64; blob.compression = JCE_ARCHIVE_COMP_ZSTD; blob.dict_id = UINT16_MAX;
        TEST_ASSERT_TRUE(jce_archive_writer_add_precompressed(writer, vs, &blob));
        TEST_ASSERT_TRUE(jce_archive_writer_add_precompressed(writer, fs, &blob));
    } else {
        TEST_ASSERT_TRUE(jce_archive_writer_add(writer, vs, stage_bytes, sizeof(stage_bytes)));
        TEST_ASSERT_TRUE(jce_archive_writer_add(writer, fs, stage_bytes, sizeof(stage_bytes)));
    }
    void *bytes = NULL; size_t size = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(writer, &bytes, &size));
    jce_archive_writer_destroy(writer);
    engine_pak = jce_pak_open_owned(bytes, size); TEST_ASSERT_NOT_NULL(engine_pak);
}
static uint16_t load(const JceFileSystem *fs, const char *vs, const char *frag)
{ return load_custom_program(fs, "material.mat.json", vs, frag); }
static void capture_begin(void) { jce_trace_reset(); jce_trace_set_enabled(true); }
static void program_result(bool attempted, bool valid, int reason)
{
    uint64_t parent = scope("material.custom_program.load");
    TEST_ASSERT_EQUAL_INT(attempted, value(parent, "custom_program.create_attempted"));
    TEST_ASSERT_EQUAL_INT(valid, value(parent, "custom_program.valid"));
    TEST_ASSERT_EQUAL_INT(1, value(parent, "custom_program.cache_admitted"));
    TEST_ASSERT_EQUAL_INT(reason, value(parent, "custom_program.reason"));
    outcome(parent, valid ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
}
void setUp(void)
{
    jce_trace_set_enabled(false); jce_trace_reset(); jce_trace_task_restore_id(0);
    jce_fs_set_active(NULL); jce_pak_register_filesystem_provider_internal();
    engine_pak = NULL; vfs = NULL; suffix = NULL;
    program_calls = alloc_calls = free_calls = owned_live = 0;
    fail_alloc = fail_program = false; memset(&s_prog_cache, 0, sizeof(s_prog_cache));
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(host_vs)); TEST_ASSERT_TRUE(jce_fs_host_remove_file(host_fs));
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(suffixed_vs)); TEST_ASSERT_TRUE(jce_fs_host_remove_file(suffixed_fs));
}
void tearDown(void)
{
    jce_trace_set_enabled(false); jce_fs_set_active(NULL);
    if (vfs) jce_fs_destroy(vfs);
    if (engine_pak) jce_pak_close(engine_pak);
    TEST_ASSERT_EQUAL_UINT(0, owned_live);
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(host_vs)); TEST_ASSERT_TRUE(jce_fs_host_remove_file(host_fs));
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(suffixed_vs)); TEST_ASSERT_TRUE(jce_fs_host_remove_file(suffixed_fs));
}
static void real_host_scopes_nest_and_cache_hit_does_no_work(void)
{
    write_host(); capture_begin(); jce_trace_task_restore_id(91);
    TEST_ASSERT_EQUAL_UINT16(37, load(NULL, host_vs, host_fs));
    TEST_ASSERT_EQUAL_UINT64(91, jce_trace_task_current_id());
    drain(); program_result(true, true, PBR_CUSTOM_PROGRAM_OK);
    const uint64_t parent = scope("material.custom_program.load");
    const uint64_t vs = scope("material.custom_shader.vs"), fs = scope("material.custom_shader.fs");
    unsigned children = 0;
    for (uint32_t i = 0; i < event_count; ++i) {
        if (events[i].type == JCE_TRACE_EVENT_TASK_SUBMIT && events[i].id == parent)
            TEST_ASSERT_EQUAL_UINT64(91, events[i].parent_id);
        if (events[i].type == JCE_TRACE_EVENT_TASK_SUBMIT && (events[i].id == vs || events[i].id == fs))
            TEST_ASSERT_EQUAL_UINT64(parent, events[i].parent_id);
        if (events[i].type == JCE_TRACE_EVENT_TASK_SUBMIT && !strcmp(events[i].name, "fs.host.read_all")) {
            TEST_ASSERT_TRUE(events[i].parent_id == vs || events[i].parent_id == fs); ++children;
        }
    }
    TEST_ASSERT_EQUAL_UINT(2, children);
    TEST_ASSERT_EQUAL_INT(PBR_CUSTOM_ROUTE_HOST_API, value(vs, "custom_shader.route"));
    TEST_ASSERT_EQUAL_INT(1, value(vs, "custom_shader.host_api_success"));
    TEST_ASSERT_EQUAL_INT(-1, value(vs, "custom_shader.vfs_api_success"));
    TEST_ASSERT_EQUAL_INT(0, value(vs, "custom_shader.fallback_attempted"));
    TEST_ASSERT_EQUAL_INT(-1, value(vs, "custom_shader.decompress_success"));
    TEST_ASSERT_EQUAL_UINT64(jce_fnv1a64_str(host_vs), wide(vs, "custom_shader.resolved_hash_lo", "custom_shader.resolved_hash_hi"));
    uint64_t key = jce_fnv1a64_append(JCE_FNV1A64_INIT, "material.mat.json", sizeof("material.mat.json"));
    key = jce_fnv1a64_append(key, host_vs, sizeof(host_vs)); key = jce_fnv1a64_append(key, host_fs, sizeof(host_fs));
    TEST_ASSERT_EQUAL_UINT64(key, wide(parent, "custom_program.request_hash_lo", "custom_program.request_hash_hi"));
    TEST_ASSERT_EQUAL_UINT(2, free_calls);
    const uint32_t before = event_count;
    TEST_ASSERT_EQUAL_UINT16(37, load(NULL, host_vs, host_fs)); drain();
    TEST_ASSERT_EQUAL_UINT32(before, event_count); TEST_ASSERT_EQUAL_UINT(1, program_calls);
}
static void real_engine_pak_follows_host_miss_and_records_bytes(void)
{
    make_pak("shaders/vs.bin", "shaders/fs.bin", false); capture_begin();
    TEST_ASSERT_EQUAL_UINT16(37, load(NULL, "shaders/vs.bin", "shaders/fs.bin"));
    drain(); program_result(true, true, PBR_CUSTOM_PROGRAM_OK);
    uint64_t vs = scope("material.custom_shader.vs");
    TEST_ASSERT_EQUAL_INT(0, value(vs, "custom_shader.reader_success"));
    TEST_ASSERT_EQUAL_INT(1, value(vs, "custom_shader.fallback_eligible"));
    TEST_ASSERT_EQUAL_INT(1, value(vs, "custom_shader.fallback_attempted"));
    TEST_ASSERT_EQUAL_INT(1, value(vs, "custom_shader.engine_pak_present"));
    TEST_ASSERT_EQUAL_INT(1, value(vs, "custom_shader.pak_entry_admitted"));
    TEST_ASSERT_EQUAL_INT(1, value(vs, "custom_shader.decompress_success"));
    TEST_ASSERT_EQUAL_INT(PBR_CUSTOM_ROUTE_ENGINE_PAK, value(vs, "custom_shader.route"));
    TEST_ASSERT_EQUAL_UINT64(sizeof(stage_bytes), wide(vs, "custom_shader.result_bytes_lo", "custom_shader.result_bytes_hi"));
    TEST_ASSERT_EQUAL_UINT(2, alloc_calls); TEST_ASSERT_EQUAL_UINT(2, free_calls);
}
static void explicit_vfs_success_does_not_attempt_engine_fallback(void)
{
    make_pak("shaders/vs.bin", "shaders/fs.bin", false);
    vfs = jce_fs_create(); TEST_ASSERT_NOT_NULL(vfs);
    TEST_ASSERT_TRUE(jce_fs_mount_pak(vfs, engine_pak)); capture_begin();
    TEST_ASSERT_EQUAL_UINT16(37, load(vfs, "shaders/vs.bin", "shaders/fs.bin")); drain();
    program_result(true, true, PBR_CUSTOM_PROGRAM_OK);
    uint64_t vs = scope("material.custom_shader.vs");
    TEST_ASSERT_EQUAL_INT(1, value(vs, "custom_shader.vfs_api_success"));
    TEST_ASSERT_EQUAL_INT(-1, value(vs, "custom_shader.host_api_success"));
    TEST_ASSERT_EQUAL_INT(PBR_CUSTOM_ROUTE_VFS_API, value(vs, "custom_shader.route"));
    TEST_ASSERT_EQUAL_INT(0, value(vs, "custom_shader.fallback_attempted"));
    TEST_ASSERT_EQUAL_UINT(0, alloc_calls);
}
static void actual_backend_resolution_preserves_distinct_request_and_resolved_keys(void)
{
    TEST_ASSERT_TRUE(jce_fs_host_write_all(suffixed_vs, stage_bytes, sizeof(stage_bytes)));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(suffixed_fs, stage_bytes, sizeof(stage_bytes)));
    suffix = "diag"; capture_begin();
    TEST_ASSERT_EQUAL_UINT16(37, load(NULL, host_vs, host_fs)); drain();
    program_result(true, true, PBR_CUSTOM_PROGRAM_OK);
    uint64_t vs = scope("material.custom_shader.vs");
    uint64_t requested = wide(vs, "custom_shader.request_hash_lo", "custom_shader.request_hash_hi");
    uint64_t resolved = wide(vs, "custom_shader.resolved_hash_lo", "custom_shader.resolved_hash_hi");
    TEST_ASSERT_EQUAL_UINT64(jce_fnv1a64_str(host_vs), requested);
    TEST_ASSERT_EQUAL_UINT64(jce_fnv1a64_str(suffixed_vs), resolved);
    TEST_ASSERT_NOT_EQUAL(requested, resolved);
}
static void missing_nonshader_path_cannot_fall_back_to_matching_engine_entry(void)
{
    make_pak("project/vs.bin", "project/fs.bin", false); capture_begin();
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, load(NULL, "project/vs.bin", "project/fs.bin")); drain();
    program_result(false, false, PBR_CUSTOM_PROGRAM_BLOB_FAILED);
    uint64_t vs = scope("material.custom_shader.vs");
    TEST_ASSERT_EQUAL_INT(0, value(vs, "custom_shader.fallback_eligible"));
    TEST_ASSERT_EQUAL_INT(0, value(vs, "custom_shader.fallback_attempted"));
    TEST_ASSERT_EQUAL_INT(-1, value(vs, "custom_shader.engine_pak_present"));
    TEST_ASSERT_EQUAL_INT(PBR_CUSTOM_BLOB_FALLBACK_FORBIDDEN, value(vs, "custom_shader.reason"));
    TEST_ASSERT_EQUAL_UINT(0, program_calls); TEST_ASSERT_EQUAL_UINT(0, alloc_calls);
}
static void absent_pak_and_entry_are_distinct_from_unobserved_decode(void)
{
    capture_begin(); TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, load(NULL, "shaders/no.bin", "shaders/none.bin")); drain();
    uint64_t vs = scope("material.custom_shader.vs");
    TEST_ASSERT_EQUAL_INT(0, value(vs, "custom_shader.engine_pak_present"));
    TEST_ASSERT_EQUAL_INT(-1, value(vs, "custom_shader.pak_entry_found"));
    TEST_ASSERT_EQUAL_INT(-1, value(vs, "custom_shader.decompress_success"));
    jce_trace_set_enabled(false); memset(&s_prog_cache, 0, sizeof(s_prog_cache));
    make_pak("shaders/vs.bin", "shaders/fs.bin", false); capture_begin();
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, load(NULL, "shaders/no.bin", "shaders/none.bin")); drain();
    vs = scope("material.custom_shader.vs");
    TEST_ASSERT_EQUAL_INT(0, value(vs, "custom_shader.pak_entry_found"));
    TEST_ASSERT_EQUAL_INT(PBR_CUSTOM_BLOB_NO_ENTRY, value(vs, "custom_shader.reason"));
    TEST_ASSERT_EQUAL_INT(0, value(vs, "custom_shader.decompress_attempted"));
}
static void allocation_failure_is_not_a_decompression_failure(void)
{
    make_pak("shaders/vs.bin", "shaders/fs.bin", false); fail_alloc = true; capture_begin();
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, load(NULL, "shaders/vs.bin", "shaders/fs.bin")); drain();
    program_result(false, false, PBR_CUSTOM_PROGRAM_BLOB_FAILED);
    uint64_t vs = scope("material.custom_shader.vs");
    TEST_ASSERT_EQUAL_INT(1, value(vs, "custom_shader.alloc_attempted"));
    TEST_ASSERT_EQUAL_INT(0, value(vs, "custom_shader.alloc_success"));
    TEST_ASSERT_EQUAL_INT(0, value(vs, "custom_shader.decompress_attempted"));
    TEST_ASSERT_EQUAL_INT(-1, value(vs, "custom_shader.decompress_success"));
    TEST_ASSERT_EQUAL_INT(PBR_CUSTOM_BLOB_ALLOC_FAILED, value(vs, "custom_shader.reason"));
    TEST_ASSERT_EQUAL_UINT(0, free_calls);
}
static void actual_bad_compressed_bytes_free_each_fallback_allocation(void)
{
    make_pak("shaders/vs.bin", "shaders/fs.bin", true); capture_begin();
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, load(NULL, "shaders/vs.bin", "shaders/fs.bin")); drain();
    program_result(false, false, PBR_CUSTOM_PROGRAM_BLOB_FAILED);
    uint64_t vs = scope("material.custom_shader.vs");
    TEST_ASSERT_EQUAL_INT(1, value(vs, "custom_shader.alloc_success"));
    TEST_ASSERT_EQUAL_INT(1, value(vs, "custom_shader.decompress_attempted"));
    TEST_ASSERT_EQUAL_INT(0, value(vs, "custom_shader.decompress_success"));
    TEST_ASSERT_EQUAL_INT(PBR_CUSTOM_BLOB_DECOMPRESS_FAILED, value(vs, "custom_shader.reason"));
    TEST_ASSERT_EQUAL_UINT64(0, wide(vs, "custom_shader.result_bytes_lo", "custom_shader.result_bytes_hi"));
    TEST_ASSERT_EQUAL_UINT(2, free_calls);
}
static void program_failure_is_opaque_and_cached_with_both_blobs_freed(void)
{
    write_host(); fail_program = true; capture_begin();
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, load(NULL, host_vs, host_fs)); drain();
    program_result(true, false, PBR_CUSTOM_PROGRAM_CREATE_FAILED_UNKNOWN);
    TEST_ASSERT_EQUAL_UINT(2, free_calls); TEST_ASSERT_EQUAL_UINT(0, owned_live);
    uint32_t before = event_count;
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, load(NULL, host_vs, host_fs)); drain();
    TEST_ASSERT_EQUAL_UINT32(before, event_count); TEST_ASSERT_EQUAL_UINT(1, program_calls);
}
static void cache_budget_and_disabled_capture_do_not_invent_child_stages(void)
{
    s_prog_cache.count = PBR_PROG_CACHE_MAX; capture_begin();
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, load(NULL, host_vs, host_fs)); drain();
    uint64_t parent = scope("material.custom_program.load");
    TEST_ASSERT_EQUAL_INT(0, value(parent, "custom_program.cache_admitted"));
    TEST_ASSERT_EQUAL_INT(PBR_CUSTOM_PROGRAM_CACHE_BUDGET, value(parent, "custom_program.reason"));
    TEST_ASSERT_EQUAL_UINT(0, program_calls); TEST_ASSERT_EQUAL_UINT(0, alloc_calls);
    jce_trace_set_enabled(false); memset(&s_prog_cache, 0, sizeof(s_prog_cache)); write_host();
    jce_trace_reset(); TEST_ASSERT_EQUAL_UINT16(37, load(NULL, host_vs, host_fs)); drain();
    TEST_ASSERT_EQUAL_UINT32(0, event_count);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(real_host_scopes_nest_and_cache_hit_does_no_work);
    RUN_TEST(real_engine_pak_follows_host_miss_and_records_bytes);
    RUN_TEST(actual_backend_resolution_preserves_distinct_request_and_resolved_keys);
    RUN_TEST(explicit_vfs_success_does_not_attempt_engine_fallback);
    RUN_TEST(missing_nonshader_path_cannot_fall_back_to_matching_engine_entry);
    RUN_TEST(absent_pak_and_entry_are_distinct_from_unobserved_decode);
    RUN_TEST(allocation_failure_is_not_a_decompression_failure);
    RUN_TEST(actual_bad_compressed_bytes_free_each_fallback_allocation);
    RUN_TEST(program_failure_is_opaque_and_cached_with_both_blobs_freed);
    RUN_TEST(cache_budget_and_disabled_capture_do_not_invent_child_stages);
    return UNITY_END();
}
